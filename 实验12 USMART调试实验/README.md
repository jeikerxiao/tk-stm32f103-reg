# 实验12 USMART调试实验

> 一句话概括：把"函数表 + 名字串"注册进固件，然后在串口助手里敲 `lcd_draw_line 0 0 239 319 0XF800` 这样的一行命令，单片机就真的去调用那个函数——用串口当一个"在线命令行调试器"，改参数不用重新编译。

## 一、实验简介

- **实验目的**（官方 `readme.txt`）：学习辅助调试工具 USMART 的使用。展开成三层：
  1. 明白"**字符串怎么变成一次函数调用**"——解析函数名、匹配注册表、按参数个数强转函数指针并调用；
  2. 学会**注册你自己的函数**（改 `usmart_config.c` 的注册表），不重编主程序就能在线试参数、试功能；
  3. 体会"**用串口代替断点**"的调试思路：读任意内存地址、写寄存器、给某段代码计时（`runtime`）。
- **在学习路线中的位置**：前置是实验 4（串口收发 + 中断接收，USMART 完全建立在它之上）和实验 11（TFTLCD，本实验的被调函数几乎全是 LCD 画图函数）。
  后续实验 21/30/31/33/34 等大工程里，USMART 是最省事的验证手段；实验 41 综合测试也带着它。
- **开发板与芯片**：正点原子 MiniSTM32 V4，`STM32F103RC`（72MHz）。
  USMART 组件本身不挑板子；要看到 LCD 相关命令的效果，需自备 ALIENTEK MCU 屏（同实验 11）。

## 二、实验原理

### 2.1 USMART 到底解决什么问题

裸机调试最痛的三件事：**加 printf 要重新编译、打断点要停 CPU、改个测试参数又要重新编译下载**。
USMART 的做法是：程序一开机就把"可以调用的函数清单"和"每个函数的名字串"准备好，
然后每隔 100ms 看一眼串口有没有新收到的一行命令；收到了就解析出函数名和参数，**在定时器中断里直接把那个函数调用掉**，
最后把返回值/耗时从串口打印回来。于是：

```text
你在串口助手敲：  lcd_clear 0XF800  +  回车换行
                       │
                       ▼
单片机收到一行 → 查注册表找到 lcd_clear 的地址 → 把 0XF800 变成参数 → 调用 lcd_clear(0xF800)
                       │
                       ▼
串口回显：       (lcd_clear(0XF800);)     ← 屏立刻变成红色
```

它**不需要**任何额外硬件，占用的资源写在 [usmart.h](Middlewares/USMART/usmart.h) 的文件头注释里：

```
 *              FLASH:4K~K字节(通过USMART_USE_HELP和USMART_USE_WRFUNS设置)
 *              SRAM:72字节(最少的情况下)
 *              SRAM计算公式:   SRAM=PARM_LEN+72-4  其中PARM_LEN必须大于等于4.
```

### 2.2 核心数据结构：一张"函数名 ↔ 函数地址"的表

USMART 靠两个结构体工作（[usmart.h](Middlewares/USMART/usmart.h)）：

```c
/* 函数名列表 */
struct _m_usmart_nametab
{
    void *func;             /* 函数指针 */
    const char *name;       /* 函数名(查找串) */
};
```

`func` 是函数的入口地址（编译期就知道，`&lcd_clear` 就是它在 FLASH 里的地址），
`name` 是**这个函数完整原型的字符串**——USMART 就是靠这个字符串同时拿到三样信息：函数名、参数个数、有没有返回值。

控制管理器把表、四个动作函数和运行时缓存捆在一起：

```c
/* usmart控制管理器 */
struct _m_usmart_dev
{
    struct _m_usmart_nametab *funs;     /* 函数名指针 */

    void (*init)(uint16_t tclk);        /* 初始化 */
    uint8_t (*cmd_rec)(char *str);   /* 识别函数名及参数 */
    void (*exe)(void);                  /* 执行  */
    void (*scan)(void);                 /* 扫描 */
    uint8_t fnum;                       /* 函数数量 */
    uint8_t pnum;                       /* 参数数量 */
    uint8_t id;                         /* 函数id */
    uint8_t sptype;                     /* 参数显示类型(非字符串参数):0,10进制;1,16进制; */
    uint16_t parmtype;                  /* 参数的类型 */
    uint8_t  plentbl[MAX_PARM];         /* 每个参数的长度暂存表 */
    uint8_t  parm[PARM_LEN];            /* 函数的参数 */
    ...
};
```

`usmart_config.c` 里把这张表和结构体填好，`usmart_dev` 就是 USMART 的"对象"：

```c
struct _m_usmart_nametab usmart_nametab[] =
{
    ...
    (void *)delay_ms, "void delay_ms(uint16_t nms)",
    (void *)lcd_clear, "void lcd_clear(uint16_t color)",
    ...
};

struct _m_usmart_dev usmart_dev =
{
    usmart_nametab,
    usmart_init,
    usmart_cmd_rec,
    usmart_exe,
    usmart_scan,
    sizeof(usmart_nametab) / sizeof(struct _m_usmart_nametab), /* 函数数量 */
    ...
};
```

`sizeof(表)/sizeof(表项)` 是 C 里算数组元素个数的标准写法，所以**你往表里多加一行，`fnum` 自动跟着变**，不用手改数量。
本工程当前注册了 **17 个函数**（见 2.3 节的表）。

### 2.3 本工程注册表里到底有哪些命令

直接从 [usmart_config.c](Middlewares/USMART/usmart_config.c) 抄出来的可用命令（`read_addr/write_addr`
由 `USMART_USE_WRFUNS == 1` 条件编译进来）：

| 命令（函数名） | 注册的参数原型 | 干什么 |
| --- | --- | --- |
| `read_addr` | `uint32_t read_addr(uint32_t addr)` | 读任意地址的 32 位值（看寄存器/看内存） |
| `write_addr` | `void write_addr(uint32_t addr,uint32_t val)` | 往任意地址写 32 位值（**危险，见注意事项**） |
| `delay_ms` | `void delay_ms(uint16_t nms)` | 毫秒延时，可用来测 `runtime` |
| `delay_us` | `void delay_us(uint32_t nus)` | 微秒延时 |
| `lcd_clear` | `void lcd_clear(uint16_t color)` | 整屏刷成指定颜色 |
| `lcd_fill` | `void lcd_fill(uint16_t sx, uint16_t sy, uint16_t ex, uint16_t ey, uint16_t color)` | 矩形填充 |
| `lcd_draw_line` | `void lcd_draw_line(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color)` | 画直线 |
| `lcd_draw_circle` | `lcd_draw_circle(uint16_t x0, uint16_t y0, uint8_t r, uint16_t color)` | 画圆 |
| `lcd_draw_rectangle` | `void lcd_draw_rectangle(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color)` | 画矩形框 |
| `lcd_show_num` | `void lcd_show_num(...x,y,num,len,size,color)` | 显示数字（高位 0 变空格） |
| `lcd_show_xnum` | `void lcd_show_xnum(...,mode,color)` | 扩展数字显示（可填 0 / 叠加） |
| `lcd_show_string` | `void lcd_show_string(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint8_t size, char *p, uint16_t color)` | 显示字符串（字符串参数要加引号） |
| `lcd_draw_point` | `void lcd_draw_point(uint16_t x, uint16_t y, uint16_t color)` | 画一个点 |
| `lcd_read_point` | `uint16_t lcd_read_point(uint16_t x, uint16_t y)` | 读某点颜色（验证画点是否正确） |
| `lcd_display_dir` | `void lcd_display_dir(uint8_t dir)` | 0 竖屏 / 1 横屏 |
| `led_set` | `void led_set(uint8_t sta)` | 控制 LED1（PD2 绿灯），`sta` 直接写引脚电平 |
| `test_fun` | `void test_fun(void(*ledset)(uint8_t), uint8_t sta)` | 演示"把一个函数的地址当参数传给另一个函数" |

### 2.4 一次命令的完整旅程（编号步骤）

```text
① 串口中断收字节      Drivers/SYSTEM/usart/usart.c 的 USART1_IRQHandler
                      每收到一个字符存进 g_usart_rx_buf[]，g_usart_rx_sta 计数；
                      先收到 0x0d('\r') 置 bit14，再收到 0x0a('\n') 置 bit15 = 一帧接收完成
② TIM4 每 100ms 中断  usmart_port.c 的 USMART_TIMX_IRQHandler → 调 usmart_dev.scan()
③ scan 取数据流       usmart.c/usmart_port.c: usmart_get_input_string()
                      检查 g_usart_rx_sta & 0x8000，取长度、在末尾补 '\0'、清零重新接收
④ cmd_rec 匹配函数    usmart_cmd_rec(): 用 usmart_get_fname() 从你输入的串里抠出函数名，
                      再对注册表每一项同样抠出本地函数名，逐个 usmart_strcmp 比较；
                      命中则记下 usmart_dev.id；一个都不中 → 交给 usmart_sys_cmd_exe() 按系统命令处理
⑤ 解析参数            usmart_get_fparam() 把每个参数解析成 32 位值或字符串指针，
                      依次存进 usmart_dev.parm[]，并用 parmtype 的 bit 标记"这个参数是字符串"
⑥ exe 真调用          usmart_exe() 按参数个数 pnum 走 switch(0~10)，把地址强转成
                      "返回 uint32_t、收 N 个 uint32_t 参数"的函数指针再调用
⑦ 回显结果            打印 "函数名(参数1,参数2)=返回值;"，若 runtime 开启再打印耗时
```

第 ⑥ 步是整个组件最"魔法"也最好懂的一段（真实代码）：

```c
    switch (usmart_dev.pnum)
    {
        case 0: /* 无参数(void类型) */
            res = (*(uint32_t(*)())usmart_dev.funs[id].func)();
            break;

        case 1: /* 有1个参数 */
            res = (*(uint32_t(*)())usmart_dev.funs[id].func)(temp[0]);
            break;
        /* ... case 2 ~ case 10 依次传 temp[0]...temp[9] ... */
    }
```

`usmart_dev.funs[id].func` 是个 `void*`，`(uint32_t(*)())` 是"返回 `uint32_t`、无参数"的函数指针类型，
`*(...)` 解引用即调用。**为什么敢把 `void lcd_clear(uint16_t)` 当成返回 `uint32_t` 来调？**
因为 ARM 的调用约定里参数走 R0~R3、返回值走 R0：`lcd_clear` 执行完 R0 里恰好是它最后一次运算的残留值，
USMART 把它当返回值读出来，对 `void` 函数来说是个"无意义数字"——`usmart_exe()` 的注释就是这么说的：
"当所执行的函数没有返回值的时候,所打印的返回值是一个无意义的数据"。
是否打印这个返回值，由注册串开头是不是 `void` 决定：

```c
        if (usmart_strcmp(fpname, "void") == 0)
        {
            *rval = 0;  /* 不需要返回值 */
        }
        else
        {
            *rval = 1;  /* 需要返回值 */
        }
```

`usmart_get_fname()` 取注册串**开头最多 5 个非空格字符**当作返回值类型来比 `void`。
所以本工程里 `lcd_draw_circle` 那条注册串漏写了 `void`（写成 `"lcd_draw_circle(uint16_t x0, ...)"`），
执行完就会多打一个无意义的 `=0Xxxxxxxxx`——不影响功能，但你知道来历就不奇怪了。

参数是字符串时（如 `lcd_show_string`），存进 `temp[i]` 的不是数字而是**指向 `parm[]` 缓冲区的指针**：

```c
        if (usmart_dev.parmtype & (1 << i)) /* 参数是字符串 */
        {
            ...
            temp[i] = (uint32_t) & (usmart_dev.parm[usmart_get_parmpos(i)]);
        }
```

这就是"能被传字符串"的实现：字符串本身躺在 `usmart_dev.parm[]` 里，函数收到的是它的首地址。

### 2.5 定时器扫描与时间参数计算（本工程实际数值）

`usmart_port.h` 决定用哪个定时器做"心跳"：

```c
#define USMART_ENTIMX_SCAN      1       /* 使用TIM的定时中断来扫描SCAN函数,... */
...
#define USMART_TIMX                     TIM4
#define USMART_TIMX_IRQn                TIM4_IRQn
#define USMART_TIMX_IRQHandler          TIM4_IRQHandler
#define USMART_TIMX_CLK_ENABLE()        do{ RCC->APB1ENR |= 1 << 2; }while(0)   /* TIMX 时钟使能 */
```

`usmart_dev.init(72)` → `usmart_init(72)` → `usmart_timx_init(72)`，逐项算给你看：

| 寄存器 | 代码写法 | 数值 | 含义 |
| --- | --- | --- | --- |
| `RCC->APB1ENR` | `\|= 1 << 2` | bit2 = TIM4EN | TIM4 在 APB1 上，bit2 是它的时钟使能位 |
| `TIM4->PSC` | `(tclk * 100) - 1`，tclk=72 | 7199 | 计数时钟 = 72MHz ÷ 7200 = **10kHz**，即 1 计数 = 0.1ms |
| `TIM4->ARR` | `1000` | 1000 | 计满 1000 次溢出 → **100ms 一次中断** → `scan()` 每 100ms 跑一次 |
| `TIM4->DIER` | `\|= 1 << 0` | UIE=1 | 允许更新（溢出）中断 |
| `TIM4->CR1` | `\|= 0x01` | CEN=1 | 启动定时器 |
| `sys_nvic_init(3,3,TIM4_IRQn,2)` | 组 2、抢占 3、子 3 | — | 最低优先级，中断里执行 `usmart_dev.scan()` |

> **为什么 tclk 传 72 而不是 36？** TIM4 挂在 APB1（`PCLK1 = HCLK/2 = 36MHz`），但 STM32 有个规则：
> APB1 分频不为 1 时，挂在该总线上的定时器时钟再乘 2，于是 TIM2~TIM4 的输入时钟是 **72MHz**。
> 代码注释也写了"计数频率必须为 10Khz,以和 runtime 单位(0.1ms)同步"。

`runtime` 计时的实现很朴素（`usmart_port.c`）：调用函数前 `usmart_timx_reset_time()` 把 `ARR` 临时改成 `0xFFFF`、
`CNT` 清零；函数执行期间中断被"吞掉"（同一个中断里在跑你的函数），函数返回后直接读 `CNT`（若期间溢出则再加 `0xFFFF`），
所以计时单位是 0.1ms，最大可测 `2 × 0xFFFF × 0.1ms ≈ 13.1s`（代码注释原话）。

`delay_ms(1000)` 之类是否"卡住"程序？不会卡主循环，但**会卡住同优先级/低优先级中断**：USMART 的函数是在
TIM4 中断里执行的，这期间 TIM4 自己的中断出不来（这就是 runtime 用 `CNT` 而不是靠中断累加计时的原因）。

### 2.6 七个系统命令（不带函数名的"内建指令"）

`usmart.c` 里的系统指令表：

```c
char *sys_cmd_tab[] =
{
    "?",
    "help",
    "list",
    "id",
    "hex",
    "dec",
    "runtime",
};
```

帮助信息里对每条的描述（`USMART_PRINTF` 输出的原文）：

| 命令 | 作用 |
| --- | --- |
| `?` / `help` | 打印 USMART 帮助（`USMART_USE_HELP=0` 时可省近 700 字节） |
| `list` | 逐行打印注册表里每个函数的**完整原型**，是"这台板子支持哪些命令"的自述 |
| `id` | 打印每个函数的**入口地址**（`test_fun` 要用它拿 `led_set` 的地址） |
| `hex` | 参数按 16 进制显示；`hex 100` 则做进制转换，回显 `0X64` |
| `dec` | 参数按 10 进制显示；`dec 0X64` 回显 `100` |
| `runtime 1` / `runtime 0` | 开/关函数执行耗时统计（必须 `USMART_ENTIMX_SCAN=1`） |

注意帮助里那句话：**"USMART有7个系统命令(必须小写)"**，函数名和命令都区分大小写。

## 三、硬件资源与接线

| 资源 | 引脚 | 代码中的宏 / 位置 | 说明 |
| --- | --- | --- | --- |
| 串口1 TX | PA9 | `USART_TX_GPIO_PORT/PIN`，[usart.h](Drivers/SYSTEM/usart/usart.h) | 接 CH340 的 RXD（跳线帽） |
| 串口1 RX | PA10 | `USART_RX_GPIO_PORT/PIN` | 接 CH340 的 TXD（跳线帽） |
| 串口1 | `USART1`，`RCC->APB2ENR \|= 1<<14` | `USART_UX` / `USART_UX_CLK_ENABLE()` | 115200-8-N-1，接收中断 |
| TIM4（USMART 心跳） | 片上外设 | `USMART_TIMX`，[usmart_port.h](Middlewares/USMART/usmart_port.h) | 100ms 中断调 `scan()`，占用 APB1 bit2 时钟 |
| NVIC | `NVIC->ISER[0]`、`NVIC->IP[]` | `sys_nvic_init(3,3,ch,2)` | 组 2；USART1 与 TIM4 都是最低优先级 |
| LED0（红灯） | **PA8** | `LED0_GPIO_PORT/PIN`，[led.h](Drivers/BSP/LED/led.h) | 主循环里 `LED0_TOGGLE()` 心跳 |
| LED1（绿灯） | **PD2** | `LED1_GPIO_PORT/PIN` | 被 `led_set(sta)` 直接控制，**低电平点亮** |
| LCD（被调函数） | PC6~PC10 + PB0~PB15 | [lcd.h](Drivers/BSP/LCD/lcd.h) | 同实验 11，仅支持 MCU 屏 |

**接线/上位机设置**：

1. USB 线插板上 `USB_UART` 口，让电脑识别出 CH340 串口（驱动要先装）。
2. 板上 **USART1 排针**：用跳线帽把 `PA9 → RXD`、`PA10 → TXD` 分别连上（不连就没法收你敲的命令）。
3. 串口助手选 **XCOM 或 SSCOM**，波特率 **115200**，勾选"加回车换行"发送（USMART 只认 `\r\n` 结尾的一行）。
   官方 `readme.txt` 特别警告：别的串口助手可能主动控制 DTR/RTS，导致 MCU 复位或程序不跑。
4. TFTLCD 模块插在 LCD 排座上（不插屏也能进 USMART，只是 LCD 类命令没有视觉效果，且 `lcd_init()` 会打印 `LCD ID:0` 或 `FFFF`）。

## 四、代码结构与调用流程

```
实验12 USMART调试实验/
├── User/main.c                       定义 led_set / test_fun + 初始化 + LED0 心跳
├── Middlewares/USMART/
│   ├── usmart.h / usmart.c           结构体定义、cmd_rec 匹配、exe 调用、系统命令处理
│   ├── usmart_config.c               ★用户配置区：注册表 usmart_nametab + 对象 usmart_dev
│   ├── usmart_port.h / usmart_port.c 移植层：MAX_PARM/PARM_LEN 等参数、TIM4 定时扫描、取数据流
│   ├── usmart_str.h / usmart_str.c   字符串工具：strcmp / str2num / 取参数 / 次方
│   └── readme.txt                    USMART 组件版本说明（GBK）
├── Drivers/BSP/LCD/                  lcd.c + lcd_ex.c + lcdfont.h（被调函数来自这里）
├── Drivers/BSP/LED/                  led.c / led.h
├── Drivers/SYSTEM/sys|delay|usart    时钟、延时（被注册的可调函数）、串口（收发 + printf 重定向）
├── Projects/MDK-ARM/atk_f103.uvprojx Keil 工程（Target 名：USMART）
└── readme.txt                        官方实验说明（GBK）
```

`main()` 流程：

```text
1 sys_stm32_clock_init(9)   72MHz
2 delay_init(72)
3 usart_init(72, 115200)    ← 必须先于 USMART：USMART 用 printf 回显
4 usmart_dev.init(72)       ← 初始化 TIM4 定时扫描 + sptype=1（默认 16 进制显示参数）
5 led_init()
6 lcd_init()                读 ID + 初始化屏 + 清屏（printf 打印 LCD ID）
7 lcd_show_string(...) ×3   屏上固定显示 STM32 / USMART TEST / ATOM@ALIENTEK
while(1) { LED0_TOGGLE(); delay_ms(500); }   ← 主循环什么都不做，功能全靠中断里的 USMART 驱动
```

**注意 `main()` 里没有 `usmart_dev.scan()`**：扫描由 TIM4 中断自动做（`USMART_ENTIMX_SCAN=1`）。
若把它改成 0，就得自己隔一段时间调一次 `usmart_dev.scan()`。

## 五、核心代码解析

### 5.1 `User/main.c`（全文）

```c
#include "./SYSTEM/sys/sys.h"
#include "./SYSTEM/usart/usart.h"
#include "./SYSTEM/delay/delay.h"
#include "./BSP/LED/led.h"
#include "./BSP/LCD/lcd.h"
#include "./USMART/usmart.h"

/* LED状态设置函数 */
void led_set(uint8_t sta)
{
    LED1(sta);
}

/* 函数参数调用测试函数 */
void test_fun(void(*ledset)(uint8_t), uint8_t sta)
{
    ledset(sta);
}

int main(void)
{
    sys_stm32_clock_init(9);    /* 设置时钟, 72Mhz */
    delay_init(72);             /* 延时初始化 */
    usart_init(72, 115200);     /* 串口初始化为115200 */
    usmart_dev.init(72);        /* 初始化USMART */
    led_init();                 /* 初始化LED */
    lcd_init();                 /* 初始化LCD */
    lcd_show_string(30,50,200,16,16,"STM32", RED);
    lcd_show_string(30,70,200,16,16,"USMART TEST", RED);
    lcd_show_string(30,90,200,16,16,"ATOM@ALIENTEK", RED);

    while(1)
    {
        LED0_TOGGLE();  /* LED0(RED) 闪烁 */
        delay_ms(500);
    }
}
```

- `led_set()` 为什么要自己包一层？因为 `LED1(x)` 是**宏**（宏没有地址，不能被注册），
  必须是真正的函数才能取地址放进注册表。这是使用 USMART 的第一原则：**只能注册函数，不能注册宏/变量**。
- `test_fun()` 演示"函数指针当参数"，配合 `id` 命令就能在线组合调用（见第七节）。
- 主循环只闪灯，是**故意的**：这样你能清楚看到"USMART 的函数调用并不发生在主循环里，而是在 TIM4 中断里"。

### 5.2 注册一个函数的正确写法（`usmart_config.c`）

```c
#include "./USMART/usmart.h"
#include "./USMART/usmart_str.h"

/* 用户配置区
 * 这下面要包含所用到的函数所申明的头文件(用户自己添加)
 */

#include "./SYSTEM/sys/sys.h"
#include "./SYSTEM/delay/delay.h"
#include "./BSP/LCD/lcd.h"

extern void led_set(uint8_t sta);
extern void test_fun(void(*ledset)(uint8_t), uint8_t sta);
```

要点逐条：

1. `led_set` / `test_fun` 定义在 `main.c` 里，`usmart_config.c` 看不到它们的头文件，所以用 `extern` 手工声明。
   （如果你注册的是某个 BSP 里的函数，直接 `#include` 它的 `.h` 就行。）
2. 注册项格式是 `(void *)函数名, "完整原型字符串",`，两行一组放同一对花括号里，**逗号结尾**。
3. 原型字符串要让解析器读得懂：**开头是返回值类型（无返回值写 `void`）**，然后是 `函数名(参数列表)`。
4. 函数名长度不能超过 `MAX_FNAME_LEN`（本工程 30）；参数最多 `MAX_PARM`（10）个；
   所有参数总长度不超过 `PARM_LEN`（200 字节）。
5. **`usmart_ex_init` 的说明**：本工程的注册表里**没有** `usmart_ex_init` 这个函数（全仓库都搜不到这个符号），
   它是正点原子其他版本例程里常见的"集中做额外初始化"的写法。想要同样的效果，在这里的做法就是
   "自己写一个函数 + 注册它"：

   ```c
   /* 比如：把一串初始化动作打包成一个可以在线调用的命令 */
   void my_board_init(void)      /* 放在 main.c 或任意参与编译的 .c 里 */
   {
       lcd_clear(BLACK);
       lcd_show_string(10, 10, 200, 16, 16, "READY", RED);
   }
   ```

   然后在 `usmart_config.c` 里加 `extern void my_board_init(void);` 和一行
   `(void *)my_board_init, "void my_board_init(void)",`，重新编译后串口敲 `my_board_init` 即可调用。

### 5.3 匹配函数：`usmart_cmd_rec()` 干了什么

```c
    sta = usmart_get_fname(str, rfname, &rpnum, &rval); /* 得到接收到的数据的函数名及参数个数 */

    if (sta)return sta; /* 错误 */

    for (i = 0; i < usmart_dev.fnum; i++)
    {
        sta = usmart_get_fname((char *)usmart_dev.funs[i].name, sfname, &spnum, &rval); /* 得到本地函数名及参数个数 */

        if (sta)return sta; /* 本地解析有误 */

        if (usmart_strcmp(sfname, rfname) == 0) /* 相等 */
        {
            if (spnum > rpnum)return USMART_PARMERR;/* 参数错误(输入参数比源函数参数少) */

            usmart_dev.id = i;  /* 记录函数ID. */
            break;  /* 跳出. */
        }
    }

    if (i == usmart_dev.fnum)return USMART_NOFUNCFIND;  /* 未找到匹配的函数 */
```

- 它对**你输入的串**和**注册表里每条原型串**都做同样的"抠函数名"处理，然后 `strcmp` 比较；
  命中就把下标记到 `usmart_dev.id`，`usmart_exe()` 之后全靠这个 id 找地址。
- 注意它的判定是 `spnum > rpnum` 才报参数错——也就是**少给参数会报错，多给的会被忽略**。
- 五种错误码在 `usmart.h` 里：`USMART_OK/USMART_FUNCERR/USMART_PARMERR/USMART_PARMOVER/USMART_NOFUNCFIND`，
  串口上通常表现为没反应或提示找不到函数，`list` 一下就能确认函数名拼写。

### 5.4 数据怎么进来的：`usmart_get_input_string()` 与串口帧格式

```c
char *usmart_get_input_string(void)
{
    uint8_t len;
    char *pbuf = 0;

    if (g_usart_rx_sta & 0x8000)        /* 串口接收完成？ */
    {
        len = g_usart_rx_sta & 0x3fff;  /* 得到此次接收到的数据长度 */
        g_usart_rx_buf[len] = '\0';     /* 在末尾加入结束符. */
        pbuf = (char*)g_usart_rx_buf;
        g_usart_rx_sta = 0;             /* 开启下一次接收 */
    }

    return pbuf;
}
```

这条串直接连到实验 4 学的串口接收状态机：`g_usart_rx_sta` 的 bit15 = 收完、bit14 = 收到 `\r`、低 14 位 = 长度。
所以命令**必须以回车换行（0x0D 0x0A）结尾**（官方 `readme.txt` 第 3 条），单敲回车（很多助手默认只发 `\r`）不会触发执行。
缓冲上限 `USART_REC_LEN = 200`，与 `PARM_LEN = 200` 正好对齐——**一行命令最长约 199 个字符**，超了就整行丢弃重收。

## 六、编译、下载与运行

1. **打开工程**：`实验12 USMART调试实验/Projects/MDK-ARM/atk_f103.uvprojx`（Target 名 `USMART`）。
2. **工程设置**（已配好）：
   - Device `STM32F103RC`、Define `STM32F103xE`、启动文件 `startup_stm32f103xe.s`
   - Include Paths **比实验 10/11 多一条 `..\..\Middlewares`**：

     ```text
     ..\..\Drivers\CMSIS\Device\ST\STM32F1xx\Include;..\..\Drivers\CMSIS\Include;..\..\Drivers;..\..\User;..\..\Middlewares
     ```

     这就是源码里能写 `#include "./USMART/usmart.h"` 的原因（相对 `Middlewares` 目录）。新加 USMART 到别的实验时，
     **这条包含路径必须补上**，否则一堆 `cannot open source input file` 错误。
   - 分组 `Middlewares/USMART`：`usmart.c`、`usmart_config.c`、`usmart_port.c`、`usmart_str.c` 四个都要进工程。
3. **printf 的半主机问题**：`Drivers/SYSTEM/usart/usart.c` 里已经做了
   `#pragma import(__use_no_semihosting)`（AC6 用 `__asm(".global __use_no_semihosting")`）与 `fputc()` 重定向，
   所以**不用勾选 MicroLIB** 也能 `printf`；如果你新建工程把 usart.c 的这段漏掉了，一执行 `printf` 就断在
   `__stdout`/半主机死循环里。
4. **编译下载**：`F7` → `F8`（ST-Link/J-Link SWD），或 BOOT0=1 串口 ISP 烧 `Output/atk_f103.hex`。
5. **上位机**：XCOM/SSCOM，115200-8-N-1，发送框勾选"发送新行/加回车换行"，先发一个 `help` 或 `list` 打通链路。

## 七、实验现象

1. 上电后 LCD 显示 3 行红字（`STM32` / `USMART TEST` / `ATOM@ALIENTEK`），串口打印 `LCD ID:xxxx`，LED0 每 0.5s 闪一次。
2. 敲 `help` + 回车换行 → 串口返回 USMART 版本框和 7 条命令说明。
3. 敲 `list` → 逐行打印 17 条函数原型（可用来核对拼写）。
4. 敲 `led_set 0` → 绿灯 LED1（PD2）**点亮**（低电平点亮，`sta` 就是引脚电平）；`led_set 1` → 熄灭。
5. 敲 `lcd_clear 0XF81F` → 整屏变品红；`dec 0XF81F` → 回显 `63519`（顺便验证颜色值）。
6. 敲 `lcd_draw_line 0 0 239 319 0XF800` → 屏上画出一条红色对角线；
   `lcd_fill 20 20 100 100 0X07E0` → 填一个绿色方块。
7. 敲 `lcd_show_string 10 200 200 16 16 "USMART ONLINE" 0X001F` → 屏上出现蓝色字符串（注意字符串要加双引号）。
8. 敲 `runtime 1` 后再敲 `delay_ms 1000` → 回显形如
   `delay_ms(0X3E8);Function Run Time:1000.0ms`（1000ms = 0x3E8，耗时正好被 TIM4 的 0.1ms 计数抓到）。
9. 先 `id` 记下 `led_set` 的入口地址（形如 `0X0800xxxx`），再敲 `test_fun 0X0800xxxx 0`
   → 等价于 `test_fun(led_set, 0)`，绿灯亮。这就是"把函数当参数传"的在线演示。
10. 敲不存在的函数（如 `lcd_test`）→ 提示未找到匹配函数（`USMART_NOFUNCFIND` 分支），不会死机。

## 八、注意事项

1. **命令区分大小写，且必须小写**（帮助信息原话）；输入结束必须是 **回车 + 换行**。
2. **波特率 115200**；用 XCOM/SSCOM，其他串口助手可能拉 DTR/RTS 把 MCU 复位（官方 `readme.txt` 第 1、2 条）。
   复位后 `lcd_init()` 会重新打印一次 ID，据此可判断是不是被复位了。
3. **必须先 `usart_init()` 再 `usmart_dev.init()`**，且 USMART 依赖 `printf`：
   串口没初始化就执行命令，会卡在 `fputc`/半主机里，表现为"敲了没反应、灯也不闪了"。
4. **`read_addr` / `write_addr` 是双刃剑**：它们就是两行裸指针操作（`usmart.c`）：

   ```c
   uint32_t read_addr(uint32_t addr)
   {
       return *(uint32_t *)addr;
   }
   ...
   void write_addr(uint32_t addr, uint32_t val)
   {
       *(uint32_t *)addr = val;
   }
   ```

   读是安全的观察手段，例如 `read_addr 0X4001080C` 读 `GPIOA->ODR`（GPIOA 基址 `0x40010800` + ODR 偏移 `0x0C`）、
   `read_addr 0X40010004` 读 `AFIO->MAPR`（看 JTAG 是否已关闭）。
   **写则可能立刻死机**：`write_addr` 会把 APB2/AHB 的时钟使能位、GPIO 的 `CRL/CRH` 配置位改乱，
   比如把正在用的 GPIOA 时钟关掉、把 LED0(PA8) 配成输入后再执行 `LED0_TOGGLE()`，
   现象是"串口彻底没输出、灯停闪"，只能按复位或重新上电恢复（复位后 `sys_stm32_clock_init()` 重新配置，一般能救回来）。
   **不要拿它写内部 FLASH（`0x08000000` 起）**：内部 FLASH 编程要走解锁/置位 PG 的时序，普通写入会触发硬件错误；
   也不要写 `FLASH->KR`（`0x40022004`）这类密钥寄存器。想改 FLASH 请用实验 26。
   读一个未映射/未使能时钟的地址同样可能挂总线（例如读 `0x40000000` 之前的保留区），拿不准就先查基址。
5. **被调函数在 TIM4 中断里执行**：
   - 函数里若再用 `delay_ms()`（SysTick 时钟摘取法）是可以的，但执行时间必须远小于你预期的响应周期，
     否则会拖住同优先级/低优先级中断（USART1 与 TIM4 在组 2 下都是 `pprio=3/sprio=3`，同级不能互相打断）。
   - 不要在注册的函数里去改 `usmart_dev` 自身或再调 `usmart_dev.scan()`。
   - 带返回值才打印的前提是注册串开头不是 `void`。
6. **只能注册"函数"**：宏、全局变量、`static` 函数（跨文件看不到地址）都不行；
   函数必须参与编译（对应 `.c` 要在 Keil 工程里），否则链接报 undefined symbol。
7. **参数是 32 位通道**：所有参数按 `uint32_t` 传递，`uint8_t/uint16_t` 形参取低位没问题；
   **不支持浮点参数**，也不支持结构体/数组参数。数字可用 `100` 或 `0X64`，支持负号（V3.2 起）。
8. **长度上限**：函数名 `MAX_FNAME_LEN=30`、参数个数 `MAX_PARM=10`、参数总长 `PARM_LEN=200`，
   改这些宏要和 `usmart_exe()` 的 `switch` 分支、`USART_REC_LEN` 一起改（`usmart_port.h` 注释已提醒）。
9. **字符串参数要加双引号**，例如 `"USMART TEST"`；字符串里的空格不会被当成参数分隔符（引号内是一整串）。
10. **`runtime` 必须 `USMART_ENTIMX_SCAN=1` 才有用**（本工程为 1）；最大计时约 13.1s，超了会算错。
11. **代码与本仓库其他实验/官方说明的一致性**：
    - 本实验 `readme.txt` 写的 LED0=PA8、LED1=PD2 与 [led.h](Drivers/BSP/LED/led.h) **一致**（实验 10 的 `readme.txt` 才是不一致的，它写了 PB5/PE5）。
    - `readme.txt` 说"通过 usmart 调用单片机里面的函数，实现对 LCD 显示和 LED 以及延时的控制"，
      与 `usmart_config.c` 的注册表相符；额外还注册了 `read_addr/write_addr`（受 `USMART_USE_WRFUNS` 控制），
      官方现象段没提这两条，注意它们的危险性（见第 4 条）。
    - `usmart_config.c` 里 `lcd_draw_circle` 的注册串**缺 `void`**、`lcd_read_point` 的注册串写 `uint16_t`
      而实际函数在 `lcd.h` 里声明为 `uint32_t lcd_read_point(...)`：注册串只用于取函数名/参数个数/是否打印返回值，
      不会造成调用错误，但别把它当权威原型来抄。
12. 源码 GBK 编码；USMART 组件是 `Middlewares/`，别误删 `usmart_config.c`——它是唯一需要你改的文件，
    `usmart.c`/`usmart_str.c` 一般不动。

## 九、总结与延伸

**带走的 5 个要点**

1. "命令 → 函数调用"的三步：**解析函数名 → 在注册表里 strcmp 匹配得 id → 按参数个数把 `void*` 强转成函数指针调用**；
   参数统一走 32 位槽 `temp[]`，字符串参数的"值"就是它在 `parm[]` 里的地址。
2. 注册表 `usmart_nametab` 的每一项都是 `{函数地址, "原型字符串"}`；`fnum` 由 `sizeof` 自动算，加一行就多一条命令。
3. USMART 靠 **TIM4 每 100ms 中断扫一次**（PSC=7199→10kHz、ARR=1000），`runtime` 靠同一定时器临时把 ARR 改成 0xFFFF 计 0.1ms。
4. 串口接收状态机（`g_usart_rx_sta` bit15/bit14 + 长度）是 USMART 的地基：命令必须 `\r\n` 结尾、一行 ≤ 约 199 字符。
5. 7 条系统命令（`?/help/list/id/hex/dec/runtime`）+ `read_addr/write_addr` 构成"在线调试工作台"：
   查函数表、查入口地址、读内存、看寄存器、给代码计时。

**延伸小练习**

1. 把实验 10 的 OLED 也接进来：在 OLED 实验工程里加 USMART 四个 `.c/.h` + 补 `..\..\Middlewares` 包含路径，
   注册 `oled_show_string`（`void oled_show_string(uint8_t x, uint8_t y, char *p, uint8_t size)`）和 `oled_clear`，
   在线敲命令刷屏——注意注册串里参数名要能被解析出正确个数。
2. 用 `runtime 1` 测 `lcd_clear 0X0000` 与 `lcd_fill 0 0 239 319 0X0000` 的耗时差异，
   再对照实验 11 里"51 帧 / 78 帧"的优化注释，验证"少动 CS/RS 就更快"。
3. 写一个 `void lcd_show_ver(uint8_t x, uint8_t y, uint16_t color)` 把 "STM32F103RC" 显示出来并注册，
   然后在串口里连续调不同 `color` 快速试配色——体会"不用重编主程序就能试参数"的价值。
4. 用 `read_addr` 观察 `GPIOA->ODR`（`0X4001080C`）在 `led_set 0/1` 前后的变化，把"寄存器就是内存地址"这件事坐实。

**衔接下一课**：实验 13（RTC）开始进入"片上外设 + 备份域"，你可以照本实验的做法把
`rtc_set_time/rtc_get_time` 注册进 USMART，用串口直接调时间；实验 21（触摸屏）、30（汉字显示）、31（图片显示）
里 USMART 依然是最快的验证工具。
