# 实验40 UCOSII实验3-消息队列、信号量集和软件定时器

> 一句话概括：把实验 39 的"一格信箱"换成可以排队的**消息队列**，用**事件标志组**（正点原子称之为"信号量集"）让一个任务同时等待多个按键事件，再用三个**软件定时器**周期性地刷新状态、变换颜色和自动发消息，从而掌握 uC/OS-II 剩下的三件主力武器。

## 一、实验简介

- **实验目的**（官方 `readme.txt`）：学习 UCOSII 的消息队列、信号量集和软件定时器等
  资源的使用。
- **为什么需要它们**（接着实验 39 的痛点讲）：
  1. 邮箱只有 1 格，消息来得快就丢——**消息队列**有很多格，能排队，适合"生产快、消费慢"。
  2. 信号量只能表示"次数"，不能表示"A 且 B 都发生了""A 或 B 任一发生"——
     **事件标志组（16 位位图）**可以按位组合等待。
  3. 任务里用 `OSTimeDly()` 循环做周期事很浪费栈、也不准——**软件定时器**由内核的
     定时器任务在到期时调用回调，不需要为它专门开一个任务。
- **在学习路线中的位置**：uC/OS-II 三部曲（实验 38 任务调度 → 39 信号量与邮箱 →
  40 队列/标志组/定时器）的收官，紧接着实验 41 综合测试实验就是这套机制的实战。
- **实验平台**：正点原子 MiniSTM32 V4（STM32F103RC）+ 2.8/3.5/4.3/7/10 寸 TFTLCD
  MCU 屏（16 位 8080 并口）+ 触摸屏（电阻/电容）+ 串口 1（PA9/PA10，CH340）。
- **RTOS 版本**：uC/OS-II V2.93.01，寄存器版工程。

## 二、实验原理

### 2.1 消息队列（Message Queue）：一列可以挂很多消息的火车

```c
void *MsgGrp[256];                  /* 消息队列存储地址,最大支持256个消息 */
q_msg = OSQCreate(&MsgGrp[0], 256); /* 创建消息队列，深度 256 */
err   = OSQPost(q_msg, p);          /* 发送：把指针 p 放进队尾 */
p     = OSQPend(q_msg, 0, &err);    /* 接收：从队头取一个指针，0 = 永久等待 */
```

| 项目 | 本实验的真实值 | 说明 |
| --- | --- | --- |
| 队列深度 | 256 | `OSQCreate` 第二参数；由 `MsgGrp[256]` 这块静态数组提供存储 |
| 队列存储开销 | 256 × 4 = 1024 字节（静态数组） | 队列存的是**指针**，不拷贝数据本体 |
| 每条消息数据大小 | 13 字节（`mymalloc(SRAMIN, 13)`），内容为 `"ALIENTEK %03d"`（12 字符 + `'\0'`） | 数据在内存池里，队列只传地址，所以**深度与数据大小互不牵连** |
| 内存池实际占用 | 32 字节/条（`MEM1_BLOCK_SIZE = 32`） | 13 字节向上取整到 1 个块 |
| 队列控制块上限 | `OS_MAX_QS = 4u` | 本工程最多 4 个队列 |
| 事件对象上限 | `OS_MAX_EVENTS = 10u` | 邮箱 + 队列 + 信号量 + 标志组 合计算 |
| 取空/满时行为 | 空则 Pend 阻塞；满则 `OSQPost` 返回 `OS_ERR_Q_FULL` | 本实验正是利用"满"让 tmr3 自动停止 |
| 当前队列长度 | `((OS_Q *)(q_msg->OSEventPtr))->OSQEntries` | LCD 上 `Q :` 显示的就是它 |

要点：队列传 `void *`，因此**必须由某方负责释放**。本例生产者 `tmr3_callback` 用
`mymalloc` 申请，消费者 `qmsgshow_task` 显示完用 `myfree` 释放；若 Post 失败（队列满），
生产者自己释放——这是"谁分配谁负责、传递后移交所有权"的典型写法。

### 2.2 事件标志组（信号量集，Event Flag Group）

一个 `OS_FLAG_GRP` 内部是 1 个 `OS_FLAGS` 位图（本工程 `OS_FLAGS_NBITS = 16u`，即
16 位，可代表 16 个独立事件），可以同时表达多个"发生/未发生"：

```c
flags_key = OSFlagCreate(0, &err);                          /* 创建，初值 0（全 0） */
OSFlagPost(flags_key, 1 << (key - 1), OS_FLAG_SET, &err);   /* 主任务：把对应位置 1 */
flags = OSFlagPend(flags_key, 0X0007, OS_FLAG_WAIT_SET_ANY, 0, &err);  /* 标志组任务：等低3位任意一个 */
OSFlagPost(flags_key, 0X0007, OS_FLAG_CLR, &err);           /* 处理完：3 位全清 0 */
```

| 等待方式宏 | 含义 |
| --- | --- |
| `OS_FLAG_WAIT_SET_ALL` | 掩码里所有位都为 1 才唤醒（"且"） |
| `OS_FLAG_WAIT_SET_ANY` | 掩码里任意一位为 1 就唤醒（"或"）——本实验用的就是它 |
| `OS_FLAG_WAIT_CLR_ALL` / `OS_FLAG_WAIT_CLR_ANY` | 等待位被清 0（需 `OS_FLAG_WAIT_CLR_EN`） |
| `+ OS_FLAG_CONSUME` | 唤醒后自动清掉读到的位（本例没加，改用手写 `OS_FLAG_CLR` 清零） |

`OSFlagPend` 返回**使它就绪的那几位**（不是整个位图），因此 `flags_task` 里可以
`if (flags & 0X0001)` / `0X0002` / `0X0004` 分别判断是 KEY0/KEY1/WK_UP。
对应关系：`KEY0_PRES = 1 → bit0`、`KEY1_PRES = 2 → bit1`、`WKUP_PRES = 3 → bit2`
（代码写作 `1 << (key - 1)`）。

> 关于"OSSemPendMulti"：本工程内核里**没有**这个名字的函数。uC/OS-II V2.93.01 提供的
> "一次等待多个事件"的 API 叫 **`OSEventPendMulti()`**（声明在
> [ucos_ii.h](Middlewares/uC-OS2/uC-OS2/Source/ucos_ii.h)，实现于 `os_core.c`，
> 由 `OS_EVENT_MULTI_EN = 1u` 开关控制，语义是"等多个信号量/队列/标志组，可选
> `OS_OPT_PEND_MULTI_ALL`（全部满足）/`OS_OPT_PEND_MULTI_ANY`（任一满足）"）。
> 本实验并没有使用它，而是用**事件标志组**这一个对象来表达多事件——这才是"信号量集"
> 在本例程里的真实用法。

### 2.3 软件定时器（Software Timer）

- 定时器由内核的**定时器管理任务**统一管理，优先级 `OS_TASK_TMR_PRIO`
  （`app_cfg.h` 中 `= OS_LOWEST_PRIO - 2u` = **61**，高于统计 62 与空闲 63）。
- 定时器自己的"节拍"比系统节拍慢：`OS_TMR_CFG_TICKS_PER_SEC = 100u`，
  即每 10 ms 才拨一次定时器轮盘。拨盘动作来自
  [os_cpu_c.c](Middlewares/uC-OS2/uC-OS2/Ports/ARM-Cortex-M/ARMv7-M/os_cpu_c.c) 的 tick 钩子：

```c
void  OSTimeTickHook (void)
{
#if OS_TMR_EN > 0u
    OSTmrCtr++;
    if (OSTmrCtr >= (OS_TICKS_PER_SEC / OS_TMR_CFG_TICKS_PER_SEC)) {   /* 1000/100 = 10 */
        OSTmrCtr = 0u;
        OSTmrSignal();                                                 /* 通知定时器任务(61) 走一格 */
    }
#endif
}
```

- 所以 `OSTmrCreate(dly, period, …)` 里的 `dly`/`period` **单位是 10 ms（一个定时器
  节拍）**，不是 1 ms 系统节拍：

| 定时器 | 代码 | 换算 | 回调做什么 |
| --- | --- | --- | --- |
| `tmr1` | `OSTmrCreate(10, 10, OS_TMR_OPT_PERIODIC, tmr1_callback, 0, "tmr1", &err)` | 10 × 10 ms = **100 ms** | 显示 CPU 使用率（多次平均）、MEM 使用率、队列长度 Q |
| `tmr2` | `OSTmrCreate(10, 20, OS_TMR_OPT_PERIODIC, tmr2_callback, 0, "tmr2", &err)` | 20 × 10 ms = **200 ms** | 在右下区域(131,221~width-1,height-1)轮流填充 7 种颜色 |
| `tmr3` | `OSTmrCreate(10, 10, OS_TMR_OPT_PERIODIC, tmr3_callback, 0, "tmr3", &err)` | 10 × 10 ms = **100 ms** | `mymalloc` 13 字节 → `sprintf("ALIENTEK %03d")` → `OSQPost` |

  第一个参数 `dly` 是"进入周期模式前先等多久"（ONE_SHOT 模式下就是超时值），
  这里三个都填 10，即先等 100 ms 再开始周期。
- 数量上限 `OS_TMR_CFG_MAX = 16u`，轮盘大小 `OS_TMR_CFG_WHEEL_SIZE = 7u`（7 个槽）。
- **回调运行在定时器任务上下文（优先级 61），不是创建者的任务里**——这是本实验现象
  （KEY0 停止 tmr2 后颜色立刻不再变化）与 CPU 占用率变化的根源。

### 2.4 与内存池的配合

消息体从静态内存池分配（[Middlewares/MALLOC](Middlewares/MALLOC/malloc.c)）：

```c
#define SRAMIN             0            /* 内部SRAM 池编号 */
#define SRAMBANK           1            /* 只管理 1 块内存池 */
#define MEM1_BLOCK_SIZE    32           /* 内存块大小 32 字节 */
#define MEM1_MAX_SIZE      40 * 1024    /* 最大管理内存 40K，F103RC 内部 SRAM 总共 48KB */
```

`my_mem_perused(SRAMIN)` 返回**千分比**（`used*1000/总块数`），故 `tmr1_callback`
里写成 `my_mem_perused(SRAMIN) / 10` 才是百分比。这也是为什么队列堆积时 `MEM:`
会跟着涨（每挂起一条消息 = 32 字节 ≈ 池子的 0.08%）。

### 2.5 任务与优先级（取自代码）

| 任务 | 优先级 | 栈 | 职责 |
| --- | --- | --- | --- |
| `key_task` | 2 | 128 | `key_scan(0)` → `OSMboxPost(msg_key, key)`，10 ms 一轮 |
| `main_task` | 4 | **512** | `OSMboxPend(msg_key,10,…)`；创建并启停 3 个定时器；`OSFlagPost`；校准 |
| `flags_task` | 3 | 128 | `OSFlagPend(…,0X0007,OS_FLAG_WAIT_SET_ANY,0,…)` → 显示按键名 + LED1 短闪 + 清标志 |
| `qmsgshow_task` | 5 | 128 | `OSQPend(q_msg,0,…)` → 显示消息 → `myfree` → `OSTimeDly(500)` |
| `led_task` | 6 | 128 | 心跳灯 LED0（`OSTimeDly(10)` 计数到 8 灭、100 亮） |
| `touch_task` | 7 | 128 | 触摸画线（限 x<129、y>221 左下区域） |
| `start_task` | 10 | 128 | SysTick、创建 3 个事件对象、`OSStatInit()`、创建 6 个任务、自我挂起 |
| 定时器任务 | 61 | 128 | `OS_TMR_EN=1u`，执行三个 `tmrX_callback` |
| 统计任务 | 62 | 128 | 产出 `OSCPUUsage` |
| 空闲任务 | 63 | 128 | 兜底 |

## 三、硬件资源与接线

| 资源 | 引脚 | 代码中的宏/位置 | 说明 |
| --- | --- | --- | --- |
| LED0（DS0 红灯，心跳） | PA8 | `LED0_GPIO_PORT/PIN`，[led.h](Drivers/BSP/LED/led.h) | 低电平点亮 |
| LED1（DS1 绿灯） | PD2 | `LED1_GPIO_PORT/PIN`，[led.h](Drivers/BSP/LED/led.h) | 由 `flags_task` 点亮 50 ms |
| KEY0 / KEY1 / WK_UP | PC5 / PA15 / PA0 | `KEY0_PRES 1`、`KEY1_PRES 2`、`WKUP_PRES 3`，[key.h](Drivers/BSP/KEY/key.h) | 前两者低有效，WK_UP 高有效 |
| LCD 背光/WR/RD/CS/RS | PC10/PC7/PC6/PC9/PC8 | [lcd.h](Drivers/BSP/LCD/lcd.h) | 8080 并口控制线（BSRR 位带写） |
| LCD 数据 D0~D15 | PB0~PB15 | `LCD_DATA_GPIO_PORT GPIOB`，`0XFFFF` | 16 位并口 |
| 电阻屏 T_PEN/T_CS/T_MISO/T_MOSI/T_CLK | PC1/PC13/PC2/PC3/PC0 | [touch.h](Drivers/BSP/TOUCH/touch.h) | XPT2046 软件 SPI |
| 电容屏 IIC SCL/SDA | PC0 / PC3 | `ctiic.c` | 与电阻屏共用引脚 |
| 24C02（校准参数） | SCL PC12 / SDA PC11 | `myiic.c`、`24cxx.c` | 电阻屏校准数据存这里 |
| 串口 1 TX/RX | PA9 / PA10（CH340） | `usart_init(72, 115200)` | `qmsgshow_task` 里有一句 `printf`，可观察 |
| 内部 SRAM 内存池 | 片内 | `mem1base[40*1024]`，[malloc.h](Middlewares/MALLOC/malloc.h) | 消息体的来源 |

- 外接件与实验 39 相同：LCD 屏插 MCU 屏座（断电操作），电阻屏校准参数写 24C02。
- 时钟：`sys_stm32_clock_init(9)` → 72 MHz；SysTick 重装 `(72*1000000)/1000 = 72000`
  → 1 ms 系统节拍；定时器轮盘每 10 个系统节拍走一格 → 10 ms。

## 四、代码结构与调用流程

```
实验40 UCOSII实验3-消息队列、信号量集和软件定时器/
├── User/uc-os2_demo.c     7 个任务 + 4 个事件对象 + 3 个定时器 + 主界面 + lcd_draw_bline
├── Drivers/BSP/           LED KEY LCD TOUCH IIC 24CXX
├── Drivers/SYSTEM/        sys delay usart
└── Middlewares/
    ├── MALLOC/            mymalloc/myfree/my_mem_perused（消息体内存池）
    └── uC-OS2/            Source(os_q.c os_flag.c os_tmr.c ...)、Ports(ARMv7-M/ARM)、Cfg/Template
```

启动顺序（`uc_os2_demo()`）：

1. `my_mem_init(SRAMIN)` → `ucos_load_main_ui()`（画界面，含 3 条分隔线 y=70/120/220
   与竖线 x=130/150）→ 再一次 `my_mem_init(SRAMIN)`（源码重复了一次，见注意事项）。
2. `OSInit()` → 创建 `start_task`（优先级 10）→ `OSStart()`。
3. `start_task`：`OS_CPU_SysTickInit(72000)` → `OSMboxCreate` / `OSQCreate(MsgGrp,256)` /
   `OSFlagCreate(0)` → `OSStatInit()` → 临界区内创建 led/touch/qmsgshow/main/flags/key
   六个任务 → `OSTaskSuspend(10)`。
4. `main_task` 一进来就创建 tmr1/tmr2/tmr3，并 `OSTmrStart(tmr1)`、`OSTmrStart(tmr2)`
   （tmr3 默认不启动，等 KEY1）。

运行期数据流：

```
key_task(2) --OSMboxPost(msg_key)--> main_task(4)
main_task(4) --OSFlagPost(bit)--> flags_task(3) --显示 KEYx DOWN + LED1 闪-->
main_task(4) --OSFlagPost/OSFlagPend 由 flags_task 处理，main_task 只管置位与延时清零-->
tmr3(定时器任务61,100ms) --mymalloc + OSQPost(q_msg)--> qmsgshow_task(5) --OSQPend→显示→myfree-->
tmr1(61,100ms)  --> 刷新 CPU / MEM / Q 三个数字
tmr2(61,200ms)  --> 右下区域换颜色
touch_task(7)   --> 左下区域画红线
led_task(6)     --> DS0 心跳
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

### 5.2 三个事件对象的创建：顺序有讲究

```c
msg_key   = OSMboxCreate((void *)0);  /* 创建消息邮箱 */
q_msg     = OSQCreate(&MsgGrp[0], 256); /* 创建消息队列 */
flags_key = OSFlagCreate(0, &err);  /* 创建信号量集 */
OSStatInit();                       /* 开启统计任务 */
OS_ENTER_CRITICAL();                /* 进入临界区(关闭中断) */
/* ...6 个 OSTaskCreateExt... */
OS_EXIT_CRITICAL();
OSTaskSuspend(START_TASK_PRIO);
```

- 与实验 39 不同，本实验**先初始化 SysTick 再创建事件对象**（`OS_CPU_SysTickInit` 在
  函数最前面），且事件对象创建全部在临界区**之外**。因为事件对象创建只改自己的结构体、
  不涉及就绪表，不需要关中断；而任务创建会改就绪表，必须保护。
- 创建事件必须在**使用它的任务跑起来之前**，所以放在 `start_task` 开头。
- `OSQCreate(&MsgGrp[0], 256)`：缓冲区必须是调用者自备的 `void *` 数组，内核只登记
  首地址和长度（`OSQMaxSize = 256`、`OSQEntries = 0`）。

### 5.3 生产者：`tmr3_callback`（每 100 ms 往队列塞一条消息）

```c
void tmr3_callback(OS_TMR *ptmr, void *p_arg)
{
    uint8_t *p;
    uint8_t err;
    static uint8_t msg_cnt = 0;                         /* msg编号 */
    p = mymalloc(SRAMIN, 13);                           /* 申请13个字节的内存 */
    if (p)
    {
        sprintf((char *)p, "ALIENTEK %03d", msg_cnt);
        msg_cnt++;
        err = OSQPost(q_msg, p);                        /* 发送队列 */
        if (err != OS_ERR_NONE)                         /* 发送失败 */
        {
            myfree(SRAMIN, p);                          /* 释放内存 */
            OSTmrStop(tmr3, OS_TMR_OPT_NONE, 0, &err);  /* 关闭软件定时器3 */
        }
    }
}
```

- 13 字节正好装下 `"ALIENTEK 000"`（12 字符）+ 结束符，编号 0~255 循环
  （`msg_cnt` 是 `uint8_t`）。
- 队列满时 `OSQPost` 返回 `OS_ERR_Q_FULL`，此处**先释放内存再停掉自己**，既不漏内存
  也不无限堆积。消费者每 500 ms 取 1 条、生产者每 100 ms 发 1 条，净增 4 条/500 ms，
  所以队列约 30 多秒后写满，此时 Q 停在 256、`TMR3` 自动停——现象可核对。
- 回调是在定时器任务（优先级 61）里执行的，因此能安全调用 `OSQPost`/`mymalloc`；
  **但绝不能在里面写长循环或 `OSTimeDly()`**（会把所有定时器都拖慢）。

### 5.4 消费者：`qmsgshow_task`（优先级 5，永久等待）

```c
void qmsgshow_task(void *pdata)
{
    char *p;
    uint8_t err;
    while (1)
    {
        printf("qmsgshow_task\r\n");
        p = OSQPend(q_msg, 0, &err); /* 请求消息队列 */
        lcd_show_string(5, 170, 240, 16, 16, p, RED); /* 显示消息 */
        myfree(SRAMIN, p);
        OSTimeDly(500);
    }
}
```

- `OSQPend(q_msg, 0, &err)` 超时参数 0 = 一直等；队列空时该任务不占 CPU。
- 取到的是生产者放进来的指针，因此显示完必须 `myfree(SRAMIN, p)`，否则内存池被耗尽，
  后续 `mymalloc` 返回 0（代码里 `if (p)` 就是在防这个）。
- 显示位置固定在 `(5,170)` 的 "QUEUE MSG" 区第二行，新消息覆盖旧消息。
- `OSTimeDly(500)` 故意"消费得比生产慢"，用来演示**队列积压**的效果（Q、MEM 上升）。
- `printf("qmsgshow_task\r\n")` 是调试残留，115200 串口接上就能看到它每轮打印。

### 5.5 信号量集：`flags_task` 与 `main_task` 的配合

```c
/* main_task 中： */
if (key)
{
    flagsclrt = 51;                                             /* 500ms后清除 */
    OSFlagPost(flags_key, 1 << (key - 1), OS_FLAG_SET, &err);   /* 设置对应的信号量为1 */
}
if (flagsclrt)                                                  /* 倒计时 */
{
    flagsclrt--;
    if (flagsclrt == 1)lcd_fill(140, 162, 239, 162 + 16, WHITE);   /* 清除显示 */
}

/* flags_task 中： */
flags = OSFlagPend(flags_key, 0X0007, OS_FLAG_WAIT_SET_ANY, 0, &err);   /* 等待信号量 */
if (flags & 0X0001) { lcd_show_string(140, 162, 240, 16, 16, "KEY0 DOWN  ", RED); }
if (flags & 0X0002) { lcd_show_string(140, 162, 240, 16, 16, "KEY1 DOWN  ", RED); }
if (flags & 0X0004) { lcd_show_string(140, 162, 240, 16, 16, "KEY_UP DOWN  ", RED); }
LED1(0);
OSTimeDly(50);
LED1(1);
OSFlagPost(flags_key, 0X0007, OS_FLAG_CLR, &err);               /* 全部信号量清零 */
```

逐点解释：

1. `main_task` 只负责"置位"，不做显示；`flags_task` 优先级 3 高于 main_task 的 4？
   不是——数字越小优先级越高，`flags_task(3)` **高于** `main_task(4)`，所以 `OSFlagPost`
   一执行，`flags_task` 立刻抢占运行，这是"事件驱动"的直观体现。
2. `OS_FLAG_WAIT_SET_ANY` + 掩码 `0X0007`：低 3 位任意一个变 1 就唤醒；
   由于没有加 `OS_FLAG_CONSUME`，唤醒后位图不会自动清零，所以任务末尾显式
   `OSFlagPost(..., 0X0007, OS_FLAG_CLR, ...)` 把 3 位全清，否则下一次 Pend 会立即返回。
3. `LED1(0); OSTimeDly(50); LED1(1);` 就是官方"蜂鸣器滴一声"的位置（本板用 LED1 指示，
   见注意事项）。
4. `flagsclrt = 51` 配合 `main_task` 末尾 `OSTimeDly(10)`，约 51 × 10 ms ≈ 500 ms 后
   把 "KEYx DOWN" 文字区域擦掉。

### 5.6 定时器开关与校准：`main_task` 的按键分发

```c
case KEY0_PRES:                                  /* tmr2 开关 + 清触摸区 */
    tmr2sta = !tmr2sta;
    if (tmr2sta) { OSTmrStart(tmr2, &err); }
    else { OSTmrStop(tmr2, OS_TMR_OPT_NONE, 0, &err);
           lcd_show_string(148, 262, 240, 16, 16, "TMR2 STOP", RED); }
    lcd_fill(0, 221, 129, lcddev.height - 1, WHITE);
    break;
case KEY1_PRES:                                  /* tmr3 开关（队列生产者） */
    tmr3sta = !tmr3sta;
    if (tmr3sta) { OSTmrStart(tmr3, &err); } else { OSTmrStop(tmr3, OS_TMR_OPT_NONE, 0, &err); }
    break;
case WKUP_PRES:                                  /* 校准 */
    OSTaskSuspend(TOUCH_TASK_PRIO);                              /* 挂起触摸屏任务 */
    OSTaskSuspend(QMSGSHOW_TASK_PRIO);                           /* 挂起队列信息显示任务 */
    OSTmrStop(tmr1, OS_TMR_OPT_NONE, 0, &err);                   /* 关闭软件定时器1 */
    if (tmr2sta) { OSTmrStop(tmr2, OS_TMR_OPT_NONE, 0, &err); }
    if ((tp_dev.touchtype & 0X80) == 0) { tp_adjust(); }
    OSTmrStart(tmr1, &err);
    if (tmr2sta) { OSTmrStart(tmr2, &err); }
    OSTaskResume(TOUCH_TASK_PRIO);
    OSTaskResume(QMSGSHOW_TASK_PRIO);
    ucos_load_main_ui();
    break;
```

- `OSTmrStop(tmr, OS_TMR_OPT_NONE, 0, &err)`：`OS_TMR_OPT_NONE` 表示"仅停止，不调用回调、
  不唤醒等待者"；另外两种是 `OS_TMR_OPT_CALLBACK`/`OS_TMR_OPT_CALLBACK_ARG`。
- 校准时把**所有会操作 LCD/触摸屏的任务和定时器全停**（touch、qmsgshow、tmr1、tmr2），
  否则 `tp_adjust()` 画图与 tmr2 的 `lcd_fill` 会互相覆盖，校准点也被触摸读干扰。
  这是实验 39 临界区思路的"升级版"：用挂起/停止代替关中断，节拍不会丢。
- `tmr2` 的开关状态用 `tmr2sta` 记住，校准结束才能"原本开着就重新开"。
- `touch_task` 的画图条件被改成
  `tp_dev.x[0] < (130 - 1) && tp_dev.y[0] < lcddev.height && tp_dev.y[0] > (220 + 1)`：
  只在左下方 "TOUCH" 区域（x<129、y>221）画线，右下 x>131 区域留给 tmr2 变色。

## 六、编译、下载与运行

1. Keil MDK 打开 [Projects/MDK-ARM/atk_f103.uvprojx](Projects/MDK-ARM/atk_f103.uvprojx)，
   器件 STM32F103ZE，宏 `STM32F103xE`，启动文件 `startup_stm32f103xe.s`
   （向量表里 `PendSV_Handler` → `OS_CPU_PendSVHandler`）。
2. 分组 `Startup / User / Drivers/BSP / Drivers/SYSTEM / Middlewares/uC-OS2 的
   BSP、CPU、LIB、OS2 / MALLOC / Readme`；`Middlewares/MALLOC/malloc.c` 已加入工程，
   必须参与编译（否则链接报 `mymalloc` 未定义）。
3. 内核源码逐文件编译：`os_core.c os_flag.c os_mbox.c os_mem.c os_mutex.c os_q.c
   os_sem.c os_task.c os_time.c os_tmr.c`；`ucos_ii.c` **不**在工程里，别重复添加。
4. Include Path 要含 `Middlewares/MALLOC` 与 uC-OS2 的 Source/Ports/Cfg 三层。
5. 编译输出在 `Output/`，ST-Link/J-Link 下载；串口助手 115200-8-N-1 可选接（观察
   `qmsgshow_task` 打印）。
6. 复位后按 LCD 提示操作。

## 七、实验现象

1. LCD 主界面分四个区：上方 STM32/UCOSII TEST3/ATOM@ALIENTEK 与按键说明，
   右侧 `CPU:`、`MEM:`、`Q :` 三个数字；中部左侧 "QUEUE MSG / Message:"，
   右侧 "FLAGS"；下方左侧 "TOUCH"、右侧 "TMR2"。
2. **DS0 每约 1 秒短亮一次**（心跳）；右下 TMR2 区**每 200 ms 换一种颜色**
   （RED→GREEN→BLUE→MAGENTA→CYAN→YELLOW→BRRED 循环）。
3. 右上 `CPU:` 数字约每 600 ms 更新一次（tmr1 每 100 ms 累加 5 次后显示平均值），
   `MEM:` 每 100 ms 刷新。
4. 按 **KEY0**：TMR2 区停止变色并显示 `TMR2 STOP`（再按一次恢复），同时左下 TOUCH 区
   被清成白色；FLAGS 区出现 `KEY0 DOWN`，约 500 ms 后消失，期间 **DS1 亮 50 ms**。
5. 按 **KEY1**：启动 tmr3 自动发消息，QUEUE MSG 区开始滚动显示
   `ALIENTEK 000`、`ALIENTEK 001`……（每 500 ms 显示 1 条），同时 `Q :`（高位补 0，
   如 004）与 `MEM:` 逐步上升；再按一次 KEY1 停止 tmr3，可以看到 `Q :` 与 `MEM:`
   逐渐下降；`Q :` 回到 000 时 QUEUE MSG 区不再更新。
6. 如果长时间不关 tmr3，`Q :` 会停在 **256**（队列写满），此时 tmr3 因
   `OSQPost` 失败自动 `OSTmrStop` 停止（`MEM:` 停在约 20%）。
7. 按 **WK_UP**：电阻屏进入 5 点校准（tmr1/tmr2、touch、qmsgshow 全部暂停），完成后
   提示 `Touch Screen Adjust OK!` 并自动重绘主界面；电容屏跳过校准，仅重绘界面。
8. 手指在左下 TOUCH 区拖动可画红色粗线（线宽 2），画线期间 `CPU:` 上升。

## 八、注意事项

1. **代码与官方 `readme.txt` 不一致（以代码为准）**：
   - readme 说"信号量集任务……任何按键按下，该任务都会控制**蜂鸣器**发出'滴'的一声"。
     本工程没有蜂鸣器驱动（`Drivers/BSP` 下无 BEEP），`flags_task` 实际是
     **点亮 LED1(PD2) 50 ms** + 在 FLAGS 区显示按键文字。
   - readme 描述的现象编号 2/3 与代码一致（KEY0 管 tmr2 + 清触摸区、KEY1 管 tmr3），
     但"KEY_UP 仅电阻屏有效"这条要在代码里靠 `(tp_dev.touchtype & 0X80) == 0` 判断。
2. **任务名与标题里"QSeren"之类的拼写**：真实 API 是 `OSQCreate/OSQPost/OSQPend`，
   队列长度字段是 `OS_Q.OSQEntries`（最大深度 `OSQMaxSize`）。
3. **"OSSemPendMulti" 并不存在**：本内核的多事件等待 API 叫 `OSEventPendMulti`
   （`OS_EVENT_MULTI_EN = 1u` 已开启但本实验未用）；本实验用事件标志组实现"等多事件之一"。
4. **`OSTmrCreate` 的时间单位是 10 ms，不是 1 ms**：因为
   `OS_TMR_CFG_TICKS_PER_SEC = 100u`。源码里 `tmr3_callback` 的注释写着
   "每 300ms 执行一次"是**过时注释**，实际是 100 ms（官方 `readme.txt` 写的 100 ms
   才是对的）。
5. **`uc_os2_demo()` 里 `my_mem_init(SRAMIN)` 调用了两次**（画界面前后各一次）。
   第二次会把内存池状态表清零——由于此时还没有任何分配，结果无害，但这是多余代码，
   若你改成"先分配再第二次 init"就会把已用内存清零造成泄漏假象。
6. **`qmsgshow_task` 里的 `printf("qmsgshow_task\r\n")`** 是调试残留：会每轮占用一点
   串口带宽，正式产品应删除（本实验串口已初始化，接到 CH340 上能看到输出）。
7. **消息内存必须配对释放**：生产者 `mymalloc`、消费者 `myfree`，Post 失败时生产者
   自己释放。若你把 `myfree` 删掉，跑几十秒就会 `mymalloc` 返回 0、tmr3 静默停止。
8. **回调里不要做重活**：三个回调都在同一个定时器任务（优先级 61）里串行执行，
   `tmr2_callback` 的 `lcd_fill` 一次要填 30 KB 级像素，会显著抬高 CPU 占用率；
   若再加一个 10 ms 的密集定时器，其它定时器就会被"挤后"。
9. **挂起 `qmsgshow_task` 期间队列不会丢**：`OSTaskSuspend` 只是让它不去 Pend，
   消息继续在队列里排着，恢复后一次性显示最后一条——这正是队列相对邮箱的优势。
10. **工程器件与板子不符**：Keil 工程选 STM32F103ZE（512 KB Flash），实板 STM32F103RC
    （256 KB / 48 KB SRAM）。内存池只取了 40 KB 是给静态任务栈留余量的，
    不要随意把 `MEM1_MAX_SIZE` 改到 48 KB，否则链接报 RAM 溢出。
11. **SysTick 中断只在 [delay.c](Drivers/SYSTEM/delay/delay.c) 实现**，不要重复定义；
    重复定义表现为"所有任务不运行/定时器不动"。
12. 源文件均为 **GBK 编码**，请用 GBK 打开以免中文注释乱码。

## 九、总结与延伸

**要点**

1. 消息队列 = 多格信箱：深度由调用者给的 `void *` 数组决定（本例 256），传的是指针，
   数据本体（13 字节 / 实占 32 字节块）在内存池里，用完必须显式 `myfree`。
2. 事件标志组 = 16 位事件位图：`OS_FLAG_WAIT_SET_ANY/ALL (+OS_FLAG_CONSUME)` 表达
   "或/且"等待；`OSFlagPend` 返回使它就绪的位；不加 CONSUME 时要自己 `OS_FLAG_CLR`。
3. 软件定时器：`OSTmrCreate(dly, period, OS_TMR_OPT_PERIODIC, cb, arg, name, &err)`，
   单位是 `OS_TICKS_PER_SEC/OS_TMR_CFG_TICKS_PER_SEC = 10` 个系统节拍（10 ms），
   回调运行在优先级 61 的定时器任务里，最多 `OS_TMR_CFG_MAX = 16` 个。
4. 队列 + 定时器 + 内存池三者可以组合出"周期性采集→缓冲→慢速显示"这类真实结构。
5. 校准这类"要独占外设"的场合，优先用 `OSTaskSuspend` + `OSTmrStop` 让路，
   比实验 39 的关临界区更温和（不丢节拍）。

**延伸练习**

1. 把 `OSQCreate(&MsgGrp[0], 256)` 改成 `OSQCreate(&MsgGrp[0], 8)`，观察 `Q :` 很快
   到达 008、tmr3 自动停止的时间大幅缩短，体会"深度小 → 满得快"。
2. 把 `qmsgshow_task` 的 `OSTimeDly(500)` 去掉，改成 while 里连续 `OSQPend` 直到队列空
   再 `OSTimeDly(500)`，对比 `Q :`/`MEM:` 曲线——消费变快后队列不再堆积。
3. 给 `flags_task` 换成 `OS_FLAG_WAIT_SET_ANY + OS_FLAG_CONSUME` 并删掉末尾的
   `OSFlagPost(..., OS_FLAG_CLR, ...)`，验证"自动消费"效果完全一样，理解 CONSUME。
4. 新增 `tmr4 = OSTmrCreate(0, 50, OS_TMR_OPT_PERIODIC, …, &err)`（500 ms），在回调里
   `OSFlagPost(flags_key, 0X0008, OS_FLAG_SET, &err)`，并把 `flags_task` 的掩码改成
   `0X000F`、增加一路 `"TMR4 FIRE"` 显示——练习事件标志组扩展与新定时器（注意
   `OS_TMR_CFG_MAX = 16` 够用）。
5. 试着用 `OSEventPendMulti()` 同时等待 `q_msg` 与 `msg_key`（`OS_EVENT_MULTI_EN` 已
   开启），比较它与事件标志组写法在"能不能带数据"上的差别。

**衔接**：实验 41 综合测试实验把本实验的所有机制用到了实际产品级的代码里——
主界面 SPB 桌面、相册/电子书/画板/时钟/网络等多个 App 任务，加上 FATFS、RTC、
CH395Q 网络芯片与 USB，值得把本实验的定时器与队列结构当作模板反复研读。
