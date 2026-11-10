# 实验39 UCOSII实验2-信号量和邮箱

> 一句话概括：在实验 38 的多任务框架上，加入**邮箱**传递按键值、**二值/计数信号量**触发 LED1 任务，让六个任务围绕 LCD 触摸屏协作，从而理解任务间通信（谁 post、谁 pend）与临界区互斥。

## 一、实验简介

- **实验目的**（官方 `readme.txt`）：学习 UCOSII 的信号量和邮箱等资源的使用。
  通俗地讲：实验 38 里各任务各干各的、互不相干；本实验让它们**互相通知**——按键任务
  发现按键就把"键值"投进邮箱，主任务从邮箱取出键值再决定做什么；主任务每收到一次
  KEY1 就给信号量 +1，LED1 任务每取走一个信号量就闪一次灯。
- **在学习路线中的位置**：前置实验为实验 5（KEY 按键）、实验 9/10（LCD）、
  实验 17/18（触摸屏），以及实验 38（UCOSII 任务调度）。后续实验 40 引入消息队列、
  事件标志组（信号量集）和软件定时器，实验 41 综合测试实验把本实验的架构放大成真实 App。
- **实验平台**：正点原子 MiniSTM32 V4 开发板 + 正点原子 2.8/3.5/4.3/7/10 寸 TFTLCD
  MCU 屏（16 位 8080 并口）；串口 1（PA9/PA10，板载 CH340）已初始化但本实验不打印。
- **RTOS 版本**：uC/OS-II V2.93.01，寄存器版工程（无 HAL/标准库）。

## 二、实验原理

### 2.1 为什么需要"任务间通信"

裸机里所有逻辑都在一个循环里，一个变量就能当"消息"。RTOS 里任务各有栈、彼此看不见
局部变量，必须靠内核提供的**事件（OS_EVENT）**来同步/通信。uC/OS-II 的事件对象都带
一个等待列表 `OSEventGrp/OSEventTbl`，谁在等它、等了多久都记在内核里，被唤醒时由
内核决定让哪个任务先跑。

### 2.2 信号量（Semaphore）：一张"排队号码牌"

把信号量想成一个**计数器 + 一张等候名单**：

| 操作 | 函数 | 行为 |
| --- | --- | --- |
| 创建 | `OSSemCreate(cnt)` | 计数值初始为 `cnt`；本实验 `OSSemCreate(0)`（一开始"没有货"） |
| 发（Post/V） | `OSSemPost(psem)` | 计数 +1；若有人正等，则直接唤醒最高优先级的那个等待者（计数不 +1） |
| 取（Pend/P） | `OSSemPend(psem, timeout, &err)` | 计数 >0 则 -1 并立即返回；为 0 则把自己挂到等待表里阻塞，直到有人 Post 或超时 |

- 计数为 0/1 的就是"二值信号量"，常用来表示"一件事发生了/资源可用"；
  本实验的 `sem_led1` 其实是**计数信号量**：连按 KEY1 多次，计数会累加，
  于是 LED1 会以"每秒一次"的节奏把欠的次数补完——这正是官方现象里
  "连续按下多次后，每隔 1 秒叫一次"的机制（本代码里换成了 LED1 闪一下）。
- 直接读 `sem_led1->OSEventCnt` 就能看到当前计数值，本实验把它显示在 LCD 上。

### 2.3 邮箱（Mailbox / Message Box）：一次只能装一封"信"

| 对比项 | 信号量 `OSSemXxx` | 邮箱 `OSMboxXxx` |
| --- | --- | --- |
| 传的是什么 | 只有"次数"，没有内容（一个计数） | 一个 `void *` 指针（可以指向任意数据） |
| 有没有容量 | 计数可累加，Post 多少次都行 | 只有 1 格：`OSMboxPost` 时若已有人等待则直接送达，否则写入 `OSMboxPtr`；**若这一格已有内容则返回 `OS_ERR_MBOX_FULL`** |
| 典型用途 | 资源计数、事件通知、释放互斥 | 传一个键值/指针/状态码 |
| 本实验 | `sem_led1`（0 → +1，`led1_task` 取用） | `msg_key`（存按键值 1/2/3，`main_task` 取用） |

```c
OS_EVENT *msg_key;                       /* 按键邮箱事件块指针 */
OS_EVENT *sem_led1;                      /* LED1信号量指针 */
msg_key  = OSMboxCreate((void *)0);      /* 创建消息邮箱，初始为空 */
sem_led1 = OSSemCreate(0);               /* 创建信号量，初始计数0 */
```

邮箱传的是 `(void *)key`，而 `key` 的取值只有 `KEY0_PRES(1)`/`KEY1_PRES(2)`/
`WKUP_PRES(3)`（见 [key.h](Drivers/BSP/KEY/key.h)），这种"小整数直接当指针"的写法是
uC/OS-II 的常见技巧（不需要额外内存，也不用担心对齐）。

### 2.4 临界区与互斥：`OS_ENTER_CRITICAL` 在本实验保护什么

本实验有两类"不能被打断"的场景：

1. **创建任务/事件时**：`start_task` 用 `OS_ENTER_CRITICAL()/OS_EXIT_CRITICAL()` 包住
   五次 `OSTaskCreateExt`，理由与实验 38 相同（防止 SysTick 中断插在调度数据结构中间）。
2. **操作 LCD / 触摸屏时**：`touch_task` 把 `tp_dev.scan(0)` 和 `lcd_draw_bline()` 都
   包进临界区：

```c
OS_ENTER_CRITICAL();            /* 进入临界区(关闭中断) */
tp_dev.scan(0);
OS_EXIT_CRITICAL();             /* 退出临界区(开中断) */
...
OS_ENTER_CRITICAL();            /* 进入临界段,防止其他任务,打断LCD操作,导致液晶乱序 */
lcd_draw_bline(lastpos[0], lastpos[1], tp_dev.x[0], tp_dev.y[0], 2, RED);
OS_EXIT_CRITICAL();
```

为什么必要？8080 并口 LCD 的"写命令 → 写数据"是一串时序：`LCD_RS(0)`→`LCD_WR(0)`→
`LCD_DATA_OUT`→`LCD_WR(1)`（宏展开为 `GPIOx->BSRR = pin << 16*(!x)`，见
[lcd.h](Drivers/BSP/LCD/lcd.h)）。如果两次 `WR` 之间被另一个同样操作 LCD 的任务
（这里是 `main_task` 的 `lcd_fill`/`lcd_show_xnum`）抢占，读写状态机就乱了，屏幕出现
花点/白线/颜色错位。临界区（`OS_CRITICAL_METHOD = 3u`，`OS_CPU_SR_Save()` 里
`CPSID I` + `MSR BASEPRI, 0x10`）把这类抢占彻底挡住。

> 提醒：临界区只解决"不被打断"，代价是把 1 ms 节拍也挡住了，所以里面只做必须的事；
> 画图这种长操作其实是"能用互斥量更好"的场景，本实验为了演示选择了最直观的临界区。

### 2.5 任务与优先级规划（真实值取自代码）

| 任务 | 优先级 | 栈大小 | 职责（代码事实） |
| --- | --- | --- | --- |
| `key_task` | 1（最高） | 128 | `key_scan(0)` 扫描，有键则 `OSMboxPost(msg_key,(void*)key)`，每 10 ms 一轮 |
| `main_task` | 2 | **512** | `OSMboxPend(msg_key,10,&err)` 取键值并分发；显示信号量计数与 CPU 占用率 |
| `led1_task` | 3 | 128 | `OSSemPend(sem_led1,0,&err)` 阻塞等信号量，取到则 LED1 亮 60 ms、灭 940 ms |
| `led_task` | 4 | 128 | 心跳灯：`delay_ms(10)` 计数，t==8 灭、t==100 亮（约 1 s 一次的一闪） |
| `touch_task` | 5 | 128 | 触摸扫描 + 在 y>120 区域画红色粗线 |
| `start_task` | 10 | 128 | 创建邮箱/信号量、`OSStatInit()`、初始化 SysTick、创建上 5 个任务后自我挂起 |
| `OS_TASK_TMR_PRIO` | 61 | 128 | 定时器任务（`OS_TMR_EN=1u`，本实验未建定时器） |
| `OS_TASK_STAT_PRIO` | 62 | 128 | 统计任务，产出 `OSCPUUsage` |
| `OS_TASK_IDLE_PRIO` | 63 | 128 | 空闲任务 |

`main_task` 栈给到 512（其它 128），因为它要调用 `tp_adjust()`（触摸屏校准，
内部有较大局部数组与浮点运算）和多个 `lcd_show_string`。

### 2.6 节拍与超时换算

`OS_TICKS_PER_SEC = 1000u` → 1 节拍 = 1 ms。因此：

- `OSMboxPend(msg_key, 10, &err)`：最多等 **10 ms**，取不到就返回 `OS_ERR_TIMEOUT`，
  这样主任务每 10 ms 还能刷一次状态显示，不会因为没人发消息而永久卡死。
- `OSSemPend(sem_led1, 0, &err)`：参数 0 表示**永不超时**，一直等到有人 Post。
- SysTick 重装值 `(72*1000000)/1000 = 72000`，与实验 38 完全一致。

## 三、硬件资源与接线

| 资源 | 引脚 | 代码中的宏/位置 | 说明 |
| --- | --- | --- | --- |
| LED0（DS0 红灯，心跳） | PA8 | `LED0_GPIO_PORT/PIN`，[led.h](Drivers/BSP/LED/led.h) | 低电平点亮，`APB2ENR \|= 1<<2` |
| LED1（DS1 绿灯） | PD2 | `LED1_GPIO_PORT/PIN`，[led.h](Drivers/BSP/LED/led.h) | 低电平点亮，`APB2ENR \|= 1<<5` |
| KEY0 | PC5 | `KEY0_GPIO_PORT/PIN`，`KEY0_PRES = 1` | 按下有效（低电平），`APB2ENR \|= 1<<4` |
| KEY1 | PA15 | `KEY1_GPIO_PORT/PIN`，`KEY1_PRES = 2` | 按下有效（低电平），注意 PA15 是 JTDI，需释放调试口 |
| WK_UP | PA0 | `WKUP_GPIO_PORT/PIN`，`WKUP_PRES = 3` | 按下有效（高电平） |
| LCD 背光 | PC10 | `LCD_BL_GPIO_PIN` | 推挽输出，置 1 亮 |
| LCD WR / RD | PC7 / PC6 | `LCD_WR`、`LCD_RD`（BSRR 位带） | 8080 并口写/读选通 |
| LCD CS / RS | PC9 / PC8 | `LCD_CS`、`LCD_RS` | 片选 / 命令-数据选择 |
| LCD 数据 D0~D15 | PB0~PB15 | `LCD_DATA_GPIO_PORT GPIOB`，`PIN 0XFFFF` | 16 位并口，`APB2ENR \|= 1<<3` |
| LCD RST | — | 源码中已注释掉 | 与系统复位共用，不复位软控 |
| 电阻屏 T_PEN/T_CS | PC1 / PC13 | [touch.h](Drivers/BSP/TOUCH/touch.h) | X 中断 / 片选 |
| 电阻屏 T_MISO/T_MOSI/T_CLK | PC2 / PC3 / PC0 | 同上 | 软件 SPI（XPT2046） |
| 电容屏 IIC（FT5206/GT9xxx） | SCL PC0、SDA PC3 | `ctiic.c` | 与电阻屏软件 SPI 复用引脚 |
| 24C02（校准参数存储） | SCL PC12、SDA PC11 | `myiic.c`/`24cxx.c` | 电阻屏校准数据保存于此 |
| 串口 1 | PA9/PA10（CH340） | `usart_init(72,115200)` | 已初始化，本实验不输出 |

接线要点：

1. LCD 屏插到开发板的 **MCU 屏插座（LCD 接口）**，只支持 16 位 8080 并口屏；
   注意 RST 与板上复位共用，插反方向会烧屏，先断电再插。
2. 屏自带触摸屏：电阻屏走 XPT2046 软件 SPI（PC0~PC3、PC13），电容屏走 IIC（PC0/PC3）。
   代码用 `tp_dev.touchtype` 的 bit7 区分（`& 0X80` 为 1 表示电容屏）。
3. 时钟仍由 `sys_stm32_clock_init(9)` 配成 72 MHz；`delay_init(72)` 之后 SysTick 以
   HCLK 计数，但节拍逻辑等 OS 跑起来（`OSRunning == OS_TRUE`）才生效。

## 四、代码结构与调用流程

```
实验39 UCOSII实验2-信号量和邮箱/
├── User/
│   ├── main.c              外设初始化后进入 uc_os2_demo()
│   ├── uc-os2_demo.c       6 个任务 + msg_key/sem_led1 + 主界面绘制 + lcd_draw_bline()
│   └── uc-os2_demo.h
├── Drivers/BSP/
│   ├── LED/  KEY/  LCD/  TOUCH/         寄存器级外设驱动
│   ├── IIC/  24CXX/                     电容屏 IIC 与校准参数存放（24C02）
├── Drivers/SYSTEM/sys|delay|usart/
└── Middlewares/uC-OS2/                   内核(Source)、移植(Ports/ARM-Cortex-M/ARMv7-M/ARM)、配置(Cfg/Template)
```

`main()` 流程：`sys_stm32_clock_init(9)` → `delay_init(72)` → `usart_init(72,115200)`
→ `led_init()` → `lcd_init()` → `key_init()` → `tp_dev.init()` → `uc_os2_demo()`。

`uc_os2_demo()` 流程（注意它在 **OS 启动前**就画了界面）：

1. `ucos_load_main_ui()`：清屏、写标题、画两条横线（y=70、y=120）与"Touch Area"、
   `CPU: %`、`SEM:000` 等提示文字。
2. `OSInit()` → `OSTaskCreateExt(start_task, 优先级10…)` → `OSStart()`（不返回）。
3. `start_task`：`OSMboxCreate` + `OSSemCreate` → `OSStatInit()` → `OS_CPU_SysTickInit(72000)`
   → 临界区内依次创建 touch/led/led1/main/key 五个任务 → 出临界区 → `OSTaskSuspend(10)`。
4. 运行期数据流：

```
key_task(1) --OSMboxPost(msg_key,键值)--> main_task(2)
main_task(2) --OSSemPost(sem_led1)--> led1_task(3) --LED1闪烁-->
main_task(2) --OSTaskSuspend/Resume(5)--> touch_task(5)   （校准期间暂停画线）
led_task(4)  --每约1秒闪一次LED0（心跳）
touch_task(5)--临界区内 lcd_draw_bline()--> LCD
```

## 五、核心代码解析

### 5.1 [User/main.c](User/main.c)（全文）

```c
int main(void)
{
    sys_stm32_clock_init(9);    /* 设置时钟, 72Mhz */
    delay_init(72);             /* 延时初始化 */
    usart_init(72, 115200);     /* 串口初始化为115200 */
    led_init();                 /* 初始化LED */
    lcd_init();                 /* 初始化LCD */
    key_init();                 /* 初始化按键 */
    tp_dev.init();              /* 触摸屏初始化 */
    uc_os2_demo();              /* 运行uC/OS-II例程 */
}
```

所有外设初始化都在 OS 之前完成，好处是驱动层不必考虑"初始化没做完就被抢占"。

### 5.2 生产-消费模型：谁 Post、谁 Pend

| 内核对象 | 创建处 | 生产者（Post） | 消费者（Pend） |
| --- | --- | --- | --- |
| `msg_key`（邮箱） | `start_task` | `key_task`：`OSMboxPost(msg_key, (void *)key)` | `main_task`：`OSMboxPend(msg_key, 10, &err)` |
| `sem_led1`（信号量） | `start_task` | `main_task`：`OSSemPost(sem_led1)`（KEY1 按下时） | `led1_task`：`OSSemPend(sem_led1, 0, &err)` |

```c
void key_task(void *pdata)                 /* 优先级1：扫描按键 */
{
    uint32_t key;
    while (1)
    {
        key = key_scan(0);
        if (key)
        {
            OSMboxPost(msg_key, (void *)key);   /* 发送消息 */
        }
        delay_ms(10);
    }
}

void led1_task(void *pdata)                /* 优先级3：等待信号量 */
{
    uint8_t err;
    while (1)
    {
        OSSemPend(sem_led1, 0, &err);   /* 请求信号量 */
        LED1(0);                        /* 打开LED1 */
        delay_ms(60);
        LED1(1);                        /* 关闭LED1 */
        delay_ms(940);
    }
}
```

要点：

- `key_task` 优先级最高，保证按键响应最快；它 post 完就走，不关心谁处理——**解耦**。
- `led1_task` 平时**阻塞**在 `OSSemPend` 上，不消耗 CPU；因此 60+940 = 1000 ms 的
  "闪一次"期间，CPU 完全可以给 touch/main 用。
- `delay_ms(60)` 在 OS 下等价于 `OSTimeDly(60)`，会主动让出 CPU。
- KEY0 走的是另一条路：不经过信号量，`main_task` 自己直接
  `LED1_TOGGLE()`（`LED1_GPIO_PORT->ODR ^= LED1_GPIO_PIN`）并清触摸区
  `lcd_fill(0, 121, lcddev.width - 1, lcddev.height - 1, WHITE)`——
  这是"KEY0 控制 DS1 亮灭 + 清屏"现象的实现处。

### 5.3 `main_task`：带超时的邮箱轮询 + 直接读事件计数

```c
key = (uint32_t)OSMboxPend(msg_key, 10, &err);
switch (key)
{
    case KEY0_PRES:  LED1_TOGGLE(); lcd_fill(0,121,lcddev.width-1,lcddev.height-1,WHITE); break;
    case KEY1_PRES:  semmask = 1;  OSSemPost(sem_led1);                     break;
    case WKUP_PRES:
        OSTaskSuspend(TOUCH_TASK_PRIO);                     /* 挂起触摸屏任务 */
        if ((tp_dev.touchtype & 0X80) == 0) { tp_adjust(); } /* 仅电阻屏需要校准 */
        OSTaskResume(TOUCH_TASK_PRIO);                      /* 解挂 */
        ucos_load_main_ui();                                /* 重新加载主界面 */
        break;
}

if (semmask || sem_led1->OSEventCnt)                    /* 需要显示sem */
{
    lcd_show_xnum(192, 50, sem_led1->OSEventCnt, 3, 16, 0X80, BLUE);   /* 显示信号量的值 */
    if (sem_led1->OSEventCnt == 0) { semmask = 0; }                    /* 停止更新 */
}

if (tcnt == 10)                                         /* 0.6秒更新一次CPU使用率 */
{
    tcnt = 0;
    lcd_show_xnum(192, 30, OSCPUUsage, 3, 16, 0, BLUE); /* 显示CPU使用率 */
}
tcnt++;
delay_ms(10);
```

逐点解释：

1. 超时 10 tick = 10 ms：没有按键时 `OSMboxPend` 每 10 ms 返回一次
   `OS_ERR_TIMEOUT`（`key` 为 0，落到 `switch` 之外继续刷新显示）。
2. `WKUP_PRES` 分支演示**挂起/恢复任务**：校准期间必须独占触摸屏，否则 `touch_task`
   会同时去读 XPT2046，两者抢同一组 SPI 线，校准数据就废了。
   `OSTaskSuspend(TOUCH_TASK_PRIO)` 把 touch 摘出就绪表，`tp_adjust()` 结束后再
   `OSTaskResume()`；`if ((tp_dev.touchtype & 0X80) == 0)` 判断为电容屏时跳过校准，
   只做界面重绘，这就是官方所说"电容屏按 KEY_UP 相当于清屏一次"。
3. `sem_led1->OSEventCnt` 是信号量的当前计数（`OS_EVENT` 结构体的公共字段），
   `lcd_show_xnum(..., 3, 16, 0X80, BLUE)` 显示 3 位、16 号字体，`0X80` 表示
   高位用 `0` 填充（mode & 0x80 语义见 [lcd.c](Drivers/BSP/LCD/lcd.c)）。
   `semmask` 用来保证"Post 过一次至少刷新一次"，计数归零后停止刷新以省 CPU。
4. `tcnt == 10` × `delay_ms(10)` ≈ 100 ms 一次（源码注释写"0.6 秒更新一次"与实际不符，
   见"八、注意事项"）。
5. `OSCPUUsage` 由统计任务（优先级 62）算出，范围 0~99；本任务把它显示在 `CPU:` 后面。

### 5.4 `led_task`：用 `delay_ms` 计数做心跳灯

```c
uint8_t t;
while (1)
{
    t++;
    delay_ms(10);
    if (t == 8)   { LED0(1);    /* LED0灭 */ }
    if (t == 100) { t = 0; LED0(0); /* LED0亮 */ }
}
```

一个循环周期 100 × 10 ms = 1 s：第 80 ms 灭灯、第 1000 ms 亮灯，即"每秒钟亮 80 ms 后
熄灭约 920 ms"的短促闪烁——与实验 38 的 DS0 效果近似，但这里用 `delay_ms()` 实现，
实验 38 用 `OSTimeDly()` 实现，两者在此等价。它存在的意义就是**心跳指示**：只要 DS0
规律地闪，说明 `led_task` 在跑、内核调度正常、SysTick 节拍正常。

## 六、编译、下载与运行

1. Keil MDK 打开 [Projects/MDK-ARM/atk_f103.uvprojx](Projects/MDK-ARM/atk_f103.uvprojx)；
   器件 STM32F103ZE、宏 `STM32F103xE`、启动文件 `startup_stm32f103xe.s`。
2. 工程分组：`Startup / User / Drivers/BSP / Drivers/SYSTEM / Middlewares/uC-OS2 的
   BSP、CPU、LIB、OS2 / Readme`。内核源码逐文件加入（`os_core.c`、`os_mbox.c`、
   `os_sem.c`、`os_flag.c`、`os_q.c`、`os_tmr.c` 等），**未**加入 `ucos_ii.c`。
   `Middlewares/MALLOC` 目录存在但没进工程，本实验用不到动态内存。
3. 编译产物在 `Output/atk_f103.axf`、`.hex`；用 ST-Link/J-Link 下载。
4. 串口 1 已初始化（115200 8N1），本实验不打印，可作调试用（自行加 `printf`）。
5. 烧录后按 RESET；LCD 显示主界面即说明 `ucos_load_main_ui()` 与任务创建都成功。

## 七、实验现象

1. 上电后 LCD 显示标题区（STM32 / UCOSII TEST2 / ATOM@ALIENTEK / 按键说明），
   中间有一条红色横线，下方写着 "Touch Area"；`CPU:` 与 `SEM:000` 区域持续刷新。
2. **DS0 每约 1 秒短亮一次**（心跳），说明系统正常运行。
3. 手指按在触摸区（横线以下）拖动 → 屏幕上画出**红色粗线**（线宽 2，`lcd_draw_bline`），
   同时 `CPU:` 数字明显上升（画图很吃 CPU）。
4. 按 **KEY0**：DS1 亮/灭状态翻转（`LED1_TOGGLE()`），同时触摸区（y≥121）被清成白色。
5. 按 **KEY1**：`SEM:` 数字 +1，`led1_task` 被唤醒把 DS1 点亮 60 ms、灭 940 ms；
   连按 3 次则 `SEM:` 变成 003（或 002…），随后 DS1 **每隔约 1 秒闪一次**，
   直到欠的次数全部执行完、`SEM:000` 为止。
6. 按 **WK_UP**：电阻屏进入 5 点校准（依次点屏幕中央十字，出现
   `Touch Screen Adjust OK!` 后自动重新加载主界面，校准参数存入 24C02）；
   电容屏则只重新加载主界面（相当于清屏一次）。

## 八、注意事项

1. **官方 `readme.txt` 与代码不一致（以代码为准）**：
   - readme 说创建了"蜂鸣器任务……每得到一个信号量蜂鸣器就叫一次""按 KEY1 控制蜂鸣器
     发声"。本工程 `Drivers/BSP` 下**没有蜂鸣器/BEEP 驱动**，代码里对应的是
     `led1_task` + `sem_led1`，得到信号量后是**点亮 LED1(PD2) 60 ms**，不是发声。
     机制（Post/Pend 计数、每次间隔 1 s）完全一致，只是执行器从蜂鸣器换成了 LED1。
   - readme 说"通过按 KEY0，可以控制 KEY1 的亮灭"，这里第二个"KEY1"应指 **DS1(LED1)**；
     代码确实是 `LED1_TOGGLE()`，与按键 KEY1 无关。
   - readme 说"主任务…执行 LED1 控制"，代码中 KEY0 分支是 `LED1_TOGGLE()` 直接改灯，
     而 `led1_task` 只在信号量到来时闪灯，两处都会动 LED1（同时操作时现象会叠加）。
2. **临界区不能太长。** `tp_dev.scan(0)` 整段被包在 `OS_ENTER_CRITICAL()` 里，电阻屏
   采样期间 1 ms 节拍会丢失，`OSTime` 计数偏慢属于正常现象；不要把这段改成更长操作。
3. **`delay_ms()` 不能在中断里替代 `OSTimeDly()`**：`delay_ms()` 判断
   `delay_osrunning && delay_osintnesting == 0`，中断中会退回忙等，任务中则让出 CPU。
4. **信号量没有"所有权"。** 任何任务都能 `OSSemPost`，因此它只能做"通知/计数"，
   不能保护共享资源（保护共享资源要用 `OSMutexPend/Post`，`OS_MUTEX_EN = 1u` 已开启）。
5. **邮箱只有一格。** 若 `key_task` 在 `main_task` 取走前又 Post 一次，第二次会返回
   `OS_ERR_MBOX_FULL`（本实验人手按键，几乎不会遇到；若改成定时器高频 Post 就会丢消息，
   这时应改用消息队列，见实验 40）。
6. **`OSMboxPend` 返回值要判空**：`msg_key` 初值为 `(void*)0`，且超时也会返回 0，
   所以 `switch (key)` 的 default 分支（本代码没有写 default）自然什么都不做。
7. **PA15/PC13 引脚复用**：KEY1 在 PA15（JTDI）、T_CS 在 PC13。若同时使用 JTAG 调试，
   需要工程里释放 JTAG（本例程未启用 `sys_jtag_set` 相关代码，实测可用）；
   PC13 驱动能力弱，仅用于触摸片选。
8. **插 LCD 屏前先断电**，方向插反会烧背光/排线；24C02 校准写入过程中不要掉电。
9. **工程器件与板子不符**：Keil 选 STM32F103ZE（512 KB Flash），实板为 STM32F103RC
   （256 KB），代码量远小于 256 KB，无实际影响，但别据此放大容量。
10. **SysTick 中断只在 `delay.c` 里实现**（官方注意事项 2），不要再写一份
    `SysTick_Handler()`，否则任务不会调度。
11. 源码为 **GBK 编码**，请用 GBK 打开，否则中文注释乱码。

## 九、总结与延伸

**要点**

1. 信号量 = 计数 + 等待名单：`OSSemCreate(0)` / `OSSemPost` / `OSSemPend` 三者配合，
   能把"发生了 N 次事件"交给另一个任务慢慢执行，且等待期间不耗 CPU。
2. 邮箱 = 一格信箱：传一个 `void *`，Post 多了会满，Pend 可带超时；比信号量多了"内容"。
3. 二值信号量可当简单的"事件通知"，`OS_MUTEX` 才带优先级继承，适合保护共享资源。
4. 临界区（`OS_ENTER_CRITICAL`，方法 3）用于保护不可打断的外设时序（8080 LCD、
   电阻屏 SPI），但会连节拍一起关掉，范围要尽量小。
5. 挂起/恢复任务（`OSTaskSuspend`/`OSTaskResume`）是"让某个任务临时让路"的正确手段，
   本实验用在校准前后。
6. 心跳灯（`led_task`）+ `OSCPUUsage` 显示，是多任务系统最基本的可观测性设计。

**延伸练习**

1. 把 `OSSemCreate(0)` 改成 `OSSemCreate(1)`，观察 `SEM:` 初值与上电后 LED1 是否
   立刻闪一次，理解"初值代表已有资源数"。
2. 在 `key_task` 里把同一个键值连续 Post 两次（不加 `delay_ms`），验证邮箱满时
   `OSMboxPost` 返回 `OS_ERR_MBOX_FULL`，并思考为什么实验 40 要用消息队列。
3. 把 `main_task` 的 `OSMboxPend` 超时改成 0（永久等待），现象会变成 CPU 占用率下降、
   但 `SEM:`/`CPU:` 不再刷新——请解释原因（提示：没人 Post 时任务不回来执行后面的显示）。
4. 给 `touch_task` 的画图段改用 `OSMutexPend/OSMutexPost`（`OS_MUTEX_EN` 已开），
   比较与临界区的差别。

**衔接**：实验 40 把"一次只装一封"的邮箱升级为**消息队列**（本实验的痛点），
并引入**事件标志组（正点原子称信号量集）**与**软件定时器**，实现多事件等待与周期回调。
