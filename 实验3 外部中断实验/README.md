# 实验3 外部中断实验

> 一句话概括：还是实验 2 那三个按键、那三个引脚，但把"程序主动去轮询"换成"按键一按，硬件自动打断主程序去执行中断服务函数"——学会 STM32F1 外部中断（EXTI）三件套：`AFIO->EXTICR` 选线、`EXTI->IMR/RTSR/FTSR` 开中断并定触发边沿、`NVIC` 管优先级，以及最重要的**在中断里清 `EXTI->PR` 挂起标志**。

## 一、实验简介

- **实验目的**（官方 `readme.txt` 写的是"学习 GPIO 作为输入的使用"，这是从实验 2 复制过来的表述；按代码实际内容，本实验目的是**学习 GPIO 输入 + 外部中断（EXTI）+ NVIC 中断管理**）。
  通俗展开：轮询就像你每隔 10ms 探头看一眼门有没有人敲；中断是装在门上的铃铛——有人敲它自己响，你不用一直看。省 CPU、响应快，而且不会漏掉"敲一下就松手"的短动作。
- **在学习路线中的位置**：
  - 前置：[实验1 跑马灯实验](../实验1%20跑马灯实验)（输出 GPIO）、[实验2 按键输入实验](../实验2%20按键输入实验)（输入 GPIO，本实验直接复用它的 `key_init()`/`key_scan()` 中的 `key_init()`）。
  - 后续：几乎所有带"自动"味道的实验都是中断用户——实验 7~9 定时器中断、实验 4/15/35 串口与 DMA 接收中断、实验 13 RTC 闹钟中断。本实验是**第一次写中断服务函数（ISR）**，是后面所有中断实验的模板。
- **硬件平台**：正点原子 MiniSTM32 V4 开发板，主芯片 **STM32F103RC**（Cortex-M3，72MHz）。

## 二、实验原理

### 2.1 STM32F1 外部中断的完整通路（一条中断要过的四道门）

```text
引脚(PA0/PC5/PA15)
   │ ①AFIO->EXTICR[x]：决定"这条 EXTI 线由哪个 GPIO 端口驱动"
   ▼
EXTI 线（line0~line15，线号 = 引脚号）
   │ ②EXTI->RTSR / EXTI->FTSR：这条线是上升沿触发还是下降沿触发
   │ ③EXTI->IMR：这条线的中断屏蔽开关（0 = 屏蔽，1 = 放行）
   ▼
边沿检测 → EXTI->PR 挂起标志置 1 → 产生中断请求
   │ ④NVIC：总中断开着吗？这个中断通道使能了吗？优先级多少？
   ▼
CPU 暂停主程序 → 跳到中断向量表里的 EXTIx_IRQHandler → 执行完返回
```

四个环节任何一个没通，中断就不会来；反过来，任何一个配错也会有"莫名其妙"的现象（比如忘清 PR 会一直重复进中断）。

关于 EXTI 线有两点必须记住：

1. **线号和引脚尾号一一对应**：PA0/PB0/PC0/…/PE0 都只能接到 line0；PA5~PE5 都只能接 line5。所以 **同一个线号同一时刻只能属于一个端口**。
2. `AFIO->EXTICR` 就是那个"多选一开关"：本实验 PC5 用 line5，就必须把 line5 映射到 GPIOC；如果另一个脚 PA5 也想中断，就会冲突（后配置的会覆盖前者）。

STM32F103 的 EXTI 有 20 条线：line0~15 来自 GPIO（本实验只用 0、5、15 三条），line16~19 是片内事件源（PVD、ADC 模拟看门狗、USB 唤醒、RTC 闹钟），本实验不涉及。

### 2.2 `AFIO->EXTICR` 线路选择：4 位一个线号，4 个寄存器

`AFIO_TypeDef`（见 [stm32f103xe.h](Drivers/CMSIS/Device/ST/STM32F1xx/Include/stm32f103xe.h)）里是 `__IO uint32_t EXTICR[4];`，即 `EXTICR1~EXTICR4` 四个寄存器，每个管 4 条线、每条线占 4 位：

| 寄存器 | 覆盖的 EXTI 线 | 每条线的 4 位位置 |
| --- | --- | --- |
| `AFIO->EXTICR[0]`（EXTICR1） | line0 ~ line3 | bit[3:0] / [7:4] / [11:8] / [15:12] |
| `AFIO->EXTICR[1]`（EXTICR2） | line4 ~ line7 | 同上 |
| `AFIO->EXTICR[2]`（EXTICR3） | line8 ~ line11 | 同上 |
| `AFIO->EXTICR[3]`（EXTICR4） | line12 ~ line15 | 同上 |

4 位里的端口编码：`0000`=GPIOA、`0001`=GPIOB、`0010`=GPIOC、`0011`=GPIOD、`0100`=GPIOE。

工程里 [sys.c](Drivers/SYSTEM/sys/sys.c) 的 `sys_nvic_ex_config()` 算这两个下标的方式非常值得看懂：

```c
    gpio_num = ((uint32_t)p_gpiox - (uint32_t)GPIOA) / 0X400 ;/* 得到gpio编号 */
    RCC->APB2ENR |= 1 << 0;     /* AFIO = 1,使能AFIO时钟 */
    ...
            offset = (pinpos % 4) * 4;
            AFIO->EXTICR[pinpos / 4] &= ~(0x000F << offset);    /* 清除原来设置！！！ */
            AFIO->EXTICR[pinpos / 4] |= gpio_num << offset;     /* EXTI.BITx映射到gpiox.bitx */
```

- `gpio_num`：用"端口地址差 ÷ 0x400"得到端口序号。头文件里 `GPIOA_BASE = 0x40010800`、`GPIOB_BASE = 0x40010C00`、`GPIOC_BASE = 0x40011000`、`GPIOD_BASE = 0x40011400`，相邻端口正好间隔 `0x400`，所以除完得到 GPIOA→0、GPIOB→1、GPIOC→2、GPIOD→3，**刚好等于硬件要求的编码**。
- `pinpos / 4` 选第几个 EXTICR 寄存器，`(pinpos % 4) * 4` 选寄存器内的 4 位偏移。

**代入本实验的三个按键**：

| 按键 | 引脚 | 线号 pinpos | 落到的寄存器与位 | 端口编码 gpio_num | 结果 |
| --- | --- | --- | --- | --- | --- |
| WK_UP | PA0 | 0 | `EXTICR[0]` bit[3:0] | 0（GPIOA） | `EXTICR1[3:0] = 0x0` |
| KEY0 | PC5 | 5 | `EXTICR[1]` bit[7:4] | 2（GPIOC） | `EXTICR2[7:4] = 0x2` |
| KEY1 | PA15 | 15 | `EXTICR[3]` bit[15:12] | 0（GPIOA） | `EXTICR4[15:12] = 0x0` |

> 注意：**AFIO 自己也挂时钟**，`RCC->APB2ENR` 的 bit0（AFIOEN）。`sys_nvic_ex_config()` 第一句就把它打开了；如果你手写 EXTI 却忘了开 AFIO 时钟，`EXTICR` 写了跟没写一样。

### 2.3 `EXTI->IMR` / `RTSR` / `FTSR` / `PR`

`EXTI_TypeDef` 六个寄存器：`IMR`、`EMR`、`RTSR`、`FTSR`、`SWIER`、`PR`，每个都是"bit n 对应 line n"。本实验用到四个：

| 寄存器 | 全称 | 作用 | 本实验怎么写 |
| --- | --- | --- | --- |
| `EXTI->IMR` | Interrupt Mask Register | 中断屏蔽；bit=1 放行，bit=0 屏蔽 | line0/line5/line15 分别置 1（`sys_nvic_ex_config` 内自动完成） |
| `EXTI->RTSR` | Rising Trigger Selection | bit=1 允许**上升沿**触发 | PA0 置 1（`SYS_GPIO_RTIR`） |
| `EXTI->FTSR` | Falling Trigger Selection | bit=1 允许**下降沿**触发 | PC5、PA15 置 1（`SYS_GPIO_FTIR`） |
| `EXTI->PR` | Pending Register | 挂起标志；边沿到来由硬件置 1；**软件必须写 1 才能清 0** | 每个 ISR 里 `EXTI->PR = 对应引脚位;` |

触发模式由 [sys.h](Drivers/SYSTEM/sys/sys.h) 三个宏决定（`SYS_GPIO_FTIR=1`、`SYS_GPIO_RTIR=2`、`SYS_GPIO_BTIR=3`）：

```c
            EXTI->IMR |= 1 << pinpos;   /* 开启line BITx上的中断(如果要禁止中断，则反操作即可) */

            if (tmode & 0x01) EXTI->FTSR |= 1 << pinpos;        /* line bitx上事件下降沿触发 */

            if (tmode & 0x02) EXTI->RTSR |= 1 << pinpos;        /* line bitx上事件上升降沿触发 */
```

- `tmode=1`（FTIR）→ 只有 bit0 命中 → 只置 FTSR。
- `tmode=2`（RTIR）→ 只有 bit1 命中 → 只置 RTSR。
- `tmode=3`（BTIR）→ 两个都命中 → 上升、下降沿都触发（即"任意边沿"）。

**为什么 KEY0/KEY1 选下降沿、WK_UP 选上升沿？** 因为这两个键是"按下接 GND"（配上拉输入，空闲时是高电平）→ 按下的瞬间产生**下降沿**；KEY_UP 是"按下接 3.3V"（配下拉输入，空闲时是低电平）→ 按下的瞬间产生**上升沿**。中断边沿要跟着电路极性选，选反了就变成"松手时才触发"。

**PR 是中断实验的头号大坑**：硬件在进入 ISR 前把 PR 置 1，它**不会自动清零**。如果 ISR 结束前不清 PR，退出中断后挂起标志仍在，CPU 会立刻再次进入同一个中断，程序看起来"卡在 ISR 里出不来"（现象：LED 常亮/常灭、主循环的 printf 不再打印）。清的方式是**往这一位写 1**（这是硬件特殊设计，写 0 无效）。

### 2.4 NVIC：中断使能与分组优先级

Cortex-M3 的中断控制器叫 NVIC（Nested Vectored Interrupt Controller，嵌套向量中断控制器）。两件事：

1. **使能这个中断通道**：`NVIC->ISER[set] |= 1 << bit`，`ch/32` 选哪个 32 位寄存器、`ch%32` 选位。
2. **设优先级**：一个优先级字节分成"抢占优先级"和"子优先级（响应优先级）"两半，怎么分由 **AIRCR 里的分组（PRIGROUP）** 决定。[sys.c](Drivers/SYSTEM/sys/sys.c) 的注释给出了 5 个分组：

| `group` 参数 | 分组名 | 抢占优先级位 | 子优先级位 | 抢占可填 | 子可填 |
| --- | --- | --- | --- | --- | --- |
| 0 | Group 0 | 0 位 | 4 位 | 只有 0 | 0~15 |
| 1 | Group 1 | 1 位 | 3 位 | 0~1 | 0~7 |
| **2** | **Group 2** | **2 位** | **2 位** | **0~3** | **0~3** |
| 3 | Group 3 | 3 位 | 1 位 | 0~7 | 0~1 |
| 4 | Group 4 | 4 位 | 0 位 | 0~15 | 只有 0 |

规则：**抢占优先级不同 → 高的可以打断低的（中断嵌套）；抢占相同、子优先级不同 → 不能互相打断，只是谁先响应**。数字越小优先级越高。本工程只用组 2（`sys_nvic_init(..., 2)`），所以抢占 0~3、子优先级 0~3。

```c
void sys_nvic_init(uint8_t pprio, uint8_t sprio, uint8_t ch, uint8_t group)
{
    uint32_t temp;
    sys_nvic_priority_group_config(group);  /* 设置分组 */
    temp = pprio << (4 - group);
    temp |= sprio & (0x0f >> group);
    temp &= 0xf;                            /* 取低四位 */
    NVIC->ISER[ch / 32] |= 1 << (ch % 32);  /* 使能中断位(要清除的话,设置ICER对应位为1即可) */
    NVIC->IP[ch] |= temp << 4;              /* 设置响应优先级和抢断优先级 */
}
```

组 2 时优先级字节的高 4 位里：bit3~bit2 = 抢占，bit1~bit0 = 子优先级；`temp << 4` 把它放到 STM32F1 实际使用的 4 位优先级高位（`NVIC->IP[ch]` 是 8 位，但 F1 只实现高 4 位，低 4 位读 0）。

本实验的三个中断编号（来自 [stm32f103xe.h](Drivers/CMSIS/Device/ST/STM32F1xx/Include/stm32f103xe.h)）：

| 中断源 | `IRQn`（`ch`） | 使能位置 | `sys_nvic_init(pprio, sprio, ch, 2)` | 优先级字节 `temp` | 抢占/子 |
| --- | --- | --- | --- | --- | --- |
| KEY0 → line5 | `EXTI9_5_IRQn = 23` | `ISER[0]` bit23 | `(0, 2, 23, 2)` | `temp = 0x2` → `IP[23] |= 0x20` | 0 / 2 |
| KEY1 → line15 | `EXTI15_10_IRQn = 40` | `ISER[1]` bit8 | `(1, 2, 40, 2)` | `temp = 0x6` → `IP[40] |= 0x60` | 1 / 2 |
| WK_UP → line0 | `EXTI0_IRQn = 6` | `ISER[0]` bit6 | `(2, 2, 6, 2)` | `temp = 0xA` → `IP[6] |= 0xA0` | 2 / 2 |

即 **KEY0 优先级最高，KEY1 次之，WK_UP 最低**，三者抢占级互不相同，所以可以在 ISR 里互相嵌套打断。

> 中断线还有"合并"关系：EXTI 线 5~9 共用一个向量 `EXTI9_5_IRQn`（服务函数只能写一个 `EXTI9_5_IRQHandler`），线 10~15 共用 `EXTI15_10_IRQn`。这就是 [exti.h](Drivers/BSP/EXTI/exti.h) 里 KEY0 对应 `EXTI9_5_IRQHandler`、KEY1 对应 `EXTI15_10_IRQHandler` 的原因。若同一条 IRQ 线上接了两个键，服务函数里必须自己读 `EXTI->PR` 判断到底是哪条线触发的。

### 2.5 中断服务函数（ISR）的名字必须和启动文件一致

[startup_stm32f103xe.s](Drivers/CMSIS/Device/ST/STM32F1xx/Source/Templates/arm/startup_stm32f103xe.s) 的中断向量表里已经排好了 `EXTI0_IRQHandler`、`EXTI9_5_IRQHandler`、`EXTI15_10_IRQHandler`（都标了 `[WEAK]`）。你写的函数**名字必须一模一样**，链接器才会把你的函数替换掉那个弱定义的空函数；名字写错（比如多一个下划线）编译不报错，但中断进来什么也不干——这是最经典的"中断没反应"原因之一。

本工程用宏来保证名字不错：

```c
#define KEY0_INT_IRQn                   EXTI9_5_IRQn
#define KEY0_INT_IRQHandler             EXTI9_5_IRQHandler
```

## 三、硬件资源与接线

| 资源 | 引脚 | 代码中的宏 / 位置 | 说明 |
| --- | --- | --- | --- |
| WK_UP 中断 | **PA0** → EXTI line0 | `WKUP_INT_GPIO_PORT/PIN`、`WKUP_INT_IRQn = EXTI0_IRQn`，[exti.h](Drivers/BSP/EXTI/exti.h) | 下拉输入 + **上升沿**触发；按下读 1 |
| KEY0 中断 | **PC5** → EXTI line5 | `KEY0_INT_GPIO_PORT/PIN`、`KEY0_INT_IRQn = EXTI9_5_IRQn` | 上拉输入 + **下降沿**触发；按下读 0 |
| KEY1 中断 | **PA15** → EXTI line15 | `KEY1_INT_GPIO_PORT/PIN`、`KEY1_INT_IRQn = EXTI15_10_IRQn` | 上拉输入 + **下降沿**触发；按下读 0 |
| LED0（红灯） | PA8 | `LED0_GPIO_PORT/PIN`，[led.h](Drivers/BSP/LED/led.h) | 推挽输出，低电平点亮；由 KEY0/WK_UP 翻转 |
| LED1（绿灯） | PD2 | `LED1_GPIO_PORT/PIN`，[led.h](Drivers/BSP/LED/led.h) | 推挽输出，低电平点亮；由 KEY0/KEY1 翻转 |
| 串口1（打印 `OK`） | PA9/PA10 → CH340 | `USART_UX = USART1`，[usart.h](Drivers/SYSTEM/usart/usart.h) | `usart_init(72, 115200)`，本实验 `main.c` 每秒 `printf("OK\r\n")` |
| GPIOA/GPIOC 时钟 | — | `RCC->APB2ENR` bit2 / bit4 | 按键与 LED0 所在端口 |
| GPIOD 时钟 | — | `RCC->APB2ENR` bit5 | LED1 |
| **AFIO 时钟** | — | `RCC->APB2ENR` **bit0** | EXTI 线路选择必需，由 `sys_nvic_ex_config()` 自动打开 |
| USART1 时钟 | — | `RCC->APB2ENR` bit14 | `printf` 要用 |

**接线/跳线**：
- 只玩按键和 LED：什么都不用接，USB 供电即可。
- **想看 `printf` 的 "OK"**：必须按实验 4 的串口接法——USB 线插到板子上丝印为 USB_UART 的 USB 口，并把丝印 `USART1` 排针上的 **PA9 与 RXD、PA10 与 TXD 各用一个跳线帽连起来**（交叉：MCU 的发送脚接 CH340 的接收脚）；不接跳线不会影响 LED 中断功能，只是看不到 "OK"。
- **时钟树**：`sys_stm32_clock_init(9)` = 8MHz HSE ×9 = 72MHz，`PCLK2 = 72MHz`（GPIO/AFIO/USART1 在这条线上）、`PCLK1 = 36MHz`。EXTI/AFIO 工作在 APB2 = 72MHz 下，边沿响应是硬件级的，微秒级；`delay_ms(20)` 消抖用的还是 `delay_init(72)` 建立的 SysTick 基准。

## 四、代码结构与调用流程

```text
实验3 外部中断实验/
├── Drivers/
│   ├── BSP/
│   │   ├── EXTI/exti.c / exti.h   # 新增：中断引脚/向量映射宏 + 3 个 ISR + extix_init()
│   │   ├── KEY/key.c / key.h      # 复用实验2：key_init() 配输入模式（本实验只用它的 init）
│   │   └── LED/led.c / led.h      # 复用实验1/2
│   ├── CMSIS/                     # 芯片头文件 + 启动文件（中断向量表在这里）
│   └── SYSTEM/sys | delay | usart # sys_nvic_ex_config()/sys_nvic_init()、延时、printf 重定向
├── Projects/MDK-ARM/atk_f103.uvprojx   # Keil 工程，Target 名：EXTI
├── User/main.c
└── readme.txt
```

- `Drivers/BSP/EXTI/exti.h`：把"哪个按键 → 哪个 GPIO → 哪条 EXTI 线 → 哪个中断向量/服务函数"四件事绑成一宏组。
- `Drivers/BSP/EXTI/exti.c`：三个 `xxx_IRQHandler()`（中断里干活的地方）+ `extix_init()`（总装：先配 GPIO，再配 EXTI 线，最后配 NVIC）。
- `Drivers/SYSTEM/sys/sys.c`：`sys_nvic_ex_config()`（AFIO/EXTICR + IMR + RTSR/FTSR）、`sys_nvic_init()`（NVIC 使能与优先级）。
- `User/main.c`：初始化后进入"每秒打印一次 OK"的死循环——**它已经不负责读按键了**，按键动作全在中断里发生。

`main()` 流程：

1. `sys_stm32_clock_init(9)` → 72MHz。
2. `delay_init(72)` → SysTick（消抖延时要用）。
3. `usart_init(72, 115200)` → 让 `printf` 有输出通道。
4. `led_init()` → PA8/PD2 推挽输出。
5. `extix_init()` → 内部依次：`key_init()`（配 PC5/PA15 上拉输入、PA0 下拉输入）→ `sys_nvic_ex_config()` 三次（配 EXTICR 线路、IMR、边沿）→ `sys_nvic_init()` 三次（开 NVIC 通道并设优先级）。
6. `LED0(0)` → 先点亮红灯作为观察起点。
7. `while(1)` → `printf("OK\r\n"); delay_ms(1000);` 循环，等中断来"抢"CPU。

## 五、核心代码解析

### 5.1 完整 `User/main.c`（去掉文件头注释）

```c
#include "./SYSTEM/sys/sys.h"
#include "./SYSTEM/usart/usart.h"
#include "./SYSTEM/delay/delay.h"
#include "./BSP/LED/led.h"
#include "./BSP/EXTI/exti.h"


int main(void)
{
    sys_stm32_clock_init(9);    /* 设置时钟, 72Mhz */
    delay_init(72);             /* 延时初始化 */
    usart_init(72, 115200);     /* 串口初始化为115200 */
    led_init();                 /* 初始化LED */
    extix_init();               /* 初始化外部中断输入 */
    LED0(0);                    /* 先点亮红灯 */

    while (1)
    {
        printf("OK\r\n");
        delay_ms(1000);
    }
}
```

要点：
- **注意顺序**：`extix_init()` 里第一件事就是 `key_init()`，所以 GPIO 输入配置发生在中断配置之前——如果引脚还没配成输入就使能了 EXTI，边沿可能来自未确定的初始状态。
- 主循环里再没有 `key_scan()`，这就是中断方式的特征：主程序只管干自己的事（这里每秒打印一个 OK）。
- 通过观察"OK 有没有按时打印"可以反推中断有没有把 CPU 卡住。

### 5.2 `extix_init()`：中断的三步总装

```c
void extix_init(void)
{
    key_init();
    sys_nvic_ex_config(KEY0_INT_GPIO_PORT, KEY0_INT_GPIO_PIN, SYS_GPIO_FTIR);   /* KEY0配置为下降沿触发中断 */
    sys_nvic_ex_config(KEY1_INT_GPIO_PORT, KEY1_INT_GPIO_PIN, SYS_GPIO_FTIR);   /* KEY1配置为下降沿触发中断 */
    sys_nvic_ex_config(WKUP_INT_GPIO_PORT, WKUP_INT_GPIO_PIN, SYS_GPIO_RTIR);   /* WKUP配置为上升沿触发中断 */

    sys_nvic_init( 0, 2, KEY0_INT_IRQn, 2); /* 抢占0，子优先级2，组2 */
    sys_nvic_init( 1, 2, KEY1_INT_IRQn, 2); /* 抢占1，子优先级2，组2 */
    sys_nvic_init( 2, 2, WKUP_INT_IRQn, 2); /* 抢占3，子优先级2，组2 */
}
```

| 步骤 | 干了什么 | 动了哪些寄存器 |
| --- | --- | --- |
| ① `key_init()` | 把三个按键脚配成输入（KEY0/KEY1 上拉、WK_UP 下拉） | `RCC->APB2ENR`(bit2/bit4)、`GPIOC->CRL[23:20]=0x8`、`GPIOA->CRH[31:28]=0x8`、`GPIOA->CRL[3:0]=0x8`、`GPIOA->ODR` |
| ② `sys_nvic_ex_config()` ×3 | 选线路 + 开屏蔽位 + 定边沿 | `RCC->APB2ENR` bit0（AFIO）、`AFIO->EXTICR[]`、`EXTI->IMR`、`EXTI->FTSR`/`EXTI->RTSR` |
| ③ `sys_nvic_init()` ×3 | 使能 NVIC 通道 + 设分组和优先级 | `SCB->AIRCR`（写 0x05FA 钥匙 + PRIGROUP）、`NVIC->ISER[]`、`NVIC->IP[]` |

**这里有一处代码注释与实际值不一致**：最后一行注释写"抢占3，子优先级2，组2"，但传入的第一个实参是 `2`，所以实际抢占优先级是 **2**（`NVIC->IP[6] |= 0xA0`，抢占=bit3:2=10b=2）。以代码为准。

`SCB->AIRCR` 为什么每次写都要带 `0X05FA0000`？因为 AIRCR 是**受写保护**的寄存器，必须把"钥匙字段 0x5FA"和你想写的位一起写进去才生效：

```c
    temp = SCB->AIRCR;      /* 读取先前的设置 */
    temp &= 0X0000F8FF;     /* 清空先前分组 */
    temp |= 0X05FA0000;     /* 写入钥匙 */
    temp |= temp1;
```

### 5.3 中断服务函数：`EXTI->PR` 一定要清

```c
void KEY0_INT_IRQHandler(void)
{
    delay_ms(20);  /* 消抖 */
    EXTI->PR = KEY0_INT_GPIO_PIN;   /* 清除KEY0所在中断线 的中断标志位 */

    if (KEY0 == 0)
    {
        LED0_TOGGLE();  /* LED0 状态取反 */ 
        LED1_TOGGLE();  /* LED1 状态取反 */ 
    }
}
```

- `delay_ms(20)`：中断同样会撞抖动问题，进 ISR 先等 20ms 再判电平，这比轮询版的 10ms 更保守。
- `EXTI->PR = KEY0_INT_GPIO_PIN;`（即 `EXTI->PR = 1<<5`）：**只把 line5 那一位写 1**，其它位写 0 对 PR 没有影响（硬件规定 PR 只响应写 1 清除）。这行必须执行，否则会重复进中断。
- `if (KEY0 == 0)`：清完标志再确认引脚确实还是低（真的按着），才执行翻转。这一层"再确认"过滤掉了毛刺边沿。
- `LED0_TOGGLE()` 展开是 `GPIOA->ODR ^= (1<<8)`，在中断里直接改 ODR 即可，不需要额外临界保护（本实验只有这里动 LED）。
- 另外两个 ISR 结构相同，差别只在清哪一位和按下的有效电平：
  - `EXTI15_10_IRQHandler`：`EXTI->PR = 1<<15;`，判 `KEY1 == 0` → `LED1_TOGGLE()`。
  - `EXTI0_IRQHandler`：`EXTI->PR = 1<<0;`，判 `WK_UP == 1`（高有效）→ `LED0_TOGGLE()`。

**为什么 KEY0/KEY1 用 `== 0`、WK_UP 用 `== 1`？** 和它们的供电方向有关（KEY0/KEY1 接 GND，WK_UP 接 3.3V），中断边沿和电平判断必须统一，否则会出现"进了中断但不翻转"。

## 六、编译、下载与运行

1. Keil MDK 打开 **[Projects/MDK-ARM/atk_f103.uvprojx](Projects/MDK-ARM/atk_f103.uvprojx)**（Target 名 `EXTI`）。
2. 工程设置（与前两个实验一致）：器件 `STM32F103RC`，Pack `Keil.STM32F1xx_DFP.2.3.0`，C 宏 `STM32F103xE`，包含路径 `..\..\Drivers\CMSIS\Device\ST\STM32F1xx\Include;..\..\Drivers\CMSIS\Include;..\..\Drivers;..\..\User`；`Drivers/BSP` 组下包含 `led.c`、`key.c`、`exti.c` 三个文件（**新加 .c 文件一定要拖进 Keil 分组，否则编译时不参与链接，会出现"函数未定义"或中断函数不生效**）。
3. 编译输出到 `..\..\Output\`，文件 `atk_f103`（hex 在 `Output/atk_f103.hex`）。
4. 下载用 ST-Link / J-Link（SWD 排针）。要看 `OK` 的话，再插 USB 到 USB_UART 口，并在 USART1 排针上把 PA9↔RXD、PA10↔TXD 各戴一个跳线帽。
5. 上位机：XCOM / SSCOM 串口助手，波特率 **115200**，数据位 8、停止位 1、无校验，编码选 GBK/ANSI。

## 七、实验现象

1. 复位后：LED0（红灯）亮，LED1（绿灯）灭。
2. 按 **KEY0（PC5，下降沿）**：红灯和绿灯同时翻转（中断里执行两个 `TOGGLE`）。
3. 按 **KEY1（PA15，下降沿）**：只有绿灯翻转。
4. 按 **WK_UP（PA0，上升沿）**：只有红灯翻转。
5. 若接了串口：串口助手每约 1 秒收到一行 `OK`，说明主循环一直在跑，按键时也不会因为按键而中断打印（打印节奏最多被 20ms 消抖轻微影响）。
6. **验证"松手不触发"**：KEY0/KEY1 只在按下的那一下翻转（空闲高 → 按下低 = 下降沿），松手产生的上升沿不会触发，因为 `RTSR` 对应位没置 1；WK_UP 相反，是松手→按下时的高电平边沿触发。
7. **验证优先级**：三个键的抢占优先级 0/1/2 互不相同。理论上按住高优先级键的 ISR 执行期间再触发低优先级键可以嵌套；由于每个 ISR 只 20ms 且人眼按键较慢，正常操作中不太容易看到嵌套现象，这个差异主要靠代码与调试器观察。

## 八、注意事项

1. **忘清 `EXTI->PR`** → 程序"卡"在中断里，主循环的 `OK` 不再打印，LED 一直亮或一直灭、按一次键像被按了无数次。这是 EXTI 实验第一大坑。
2. **边沿选反** → 变成"松手才响应"或完全不响应。原则：按下接 GND 的键选下降沿（`SYS_GPIO_FTIR`），按下接 3V3 的键选上升沿（`SYS_GPIO_RTIR`）。
3. **忘开 AFIO 时钟** → `AFIO->EXTICR` 写不进去，线号映射停在默认 GPIOA。若你的线号本来就是 PAx（本工程 WK_UP/KEY1），现象仍可能对，但 PC5 就会失效——`EXTICR2[7:4]` 仍是 0（GPIOA），而 PA5 上并没有按键，表现就是"KEY0 完全没反应"。
4. **ISR 名字写错** → 编译链接都不报错，但走的是启动文件里那个 `[WEAK]` 空函数。本工程用 `xxx_INT_IRQHandler` 宏绑死名字来规避。
5. **同一条 IRQ 向量被多线共用**：`EXTI9_5_IRQHandler` 覆盖 line5~line9，`EXTI15_10_IRQHandler` 覆盖 line10~line15。如果以后在 line6 也加按键，就必须在这个服务函数里逐个查 `EXTI->PR` 才能区分是谁触发的，否则会误翻转。
6. **同一个线号不能接两个端口**：`EXTICR` 后配置的会覆盖前面的；已用 `EXTI->PR = KEY0_INT_GPIO_PIN` 这种"清对应位"写法时也要注意别把另一条线的标志误处理。
7. **中断里不要做长活**：本实验为了教学直观，在 ISR 里调了 `delay_ms(20)`（忙等，占用 CPU 20ms）。实际项目中 ISR 应尽量短，把延时/复杂处理放到主循环或标志位+软件定时器里。
8. **`sys_nvic_init()` 用的是 `|=` 而不是 `=`**（`NVIC->IP[ch] |= temp << 4;`）：如果同一个通道先按组 4 再按组 2 配置，旧的高位残留会让优先级"叠"起来。同一工程里请**保持所有中断用同一个分组**（本工程统一为组 2）。
9. **`PA15` 与 JTAG**：PA15 复位后默认承担 JTDI 功能，本工程直接把它作 KEY1/EXTI line15 使用；如果你的调试环境要用完整 JTAG，或者在别的实验里动过 `AFIO->MAPR` 的 `SWJ_CFG`，请改用 SWD（`sys_gpio_remap_set(24, 3, 2)` 表示关 JTAG、保留 SWD），以免 PA15 被调试器占用。
10. **代码与官方说明的差异**（以代码为准）：
    - `readme.txt` 的"实验目的"写的是"学习 GPIO 作为输入的使用"（与实验 2 相同），但本实验重点是 EXTI/NVIC 中断。
    - `readme.txt` 的"实验现象"写"通过开发板上的三个独立按键控制LED灯**或者蜂鸣器**"，而 [exti.c](Drivers/BSP/EXTI/exti.c) 的三个 ISR 只操作 LED0/LED1，**没有任何蜂鸣器代码**。
    - `readme.txt` 的现象列表里没提串口，但 [User/main.c](User/main.c) 每秒 `printf("OK\r\n")`，实际还依赖 `usart_init(72, 115200)` 和 USART1 跳线帽才能看到该输出。
    - `exti.c` 里 `sys_nvic_init( 2, 2, WKUP_INT_IRQn, 2)` 的行尾注释误写成"抢占3"，实参是 2。
11. **源码 GBK 编码**：Keil 编辑器编码需设为 GBK，否则中文注释乱码；不要另存为 UTF-8。

## 九、总结与延伸

**本实验掌握 5 个要点：**

1. 一条外部中断要过的四道门：**AFIO/EXTICR 选端口 → EXTI IMR 放行 → RTSR/FTSR 定边沿 → NVIC 使能+优先级**，缺一不响。
2. `AFIO->EXTICR[pin/4]` 的位偏移是 `(pin%4)*4`，写入的 4 位就是端口序号（A=0、B=1、C=2、D=3）；**线号 = 引脚尾号**，同线号跨端口互斥。
3. `EXTI->PR` 是"写 1 清除"的挂起标志，**ISR 里必须清**，否则反复进中断。
4. NVIC 分组决定抢占/子优先级怎么分（本工程组 2 = 2 位抢占 + 2 位子优先级）；抢占不同才能嵌套，抢占相同只看子优先级排队。
5. EXTI 线 5~9、10~15 共用中断向量，服务函数名必须与启动文件里的 `xxx_IRQHandler` 完全一致。

**延伸小练习：**

1. 把 WK_UP 改成 `SYS_GPIO_BTIR`（任意边沿触发），观察"按下和松手各翻转一次"，理解 FTSR 与 RTSR 同时置 1 的效果。
2. 在 `EXTI0_IRQHandler` 里也加上 `LED1_TOGGLE();`，并把 `sys_nvic_init(2, 2, WKUP_INT_IRQn, 2)` 的抢占优先级改成 0（与 KEY0 相同）、子优先级改成 3，观察"同抢占级不嵌套、只按子优先级排序"的行为差别。
3. 把 `main.c` 的 `printf("OK\r\n")` 延时从 1000ms 改成 100ms，同时把 ISR 里的 `delay_ms(20)` 改成 `delay_ms(200)`，用串口打印的间隔变化亲眼看到"中断里长延时把主循环拖慢了"——从而理解为什么 ISR 要短。
4. 自己加一路按键中断：选一个空闲引脚（如 PB 口的某个键），在 `exti.h` 仿照现有宏加一组 `xxx_INT_GPIO_*` / `xxx_INT_IRQn` / `xxx_INT_IRQHandler`，在 `exti.c` 加对应 ISR 和两次配置调用——注意 line 号落在哪个 `EXTICR` 寄存器、哪个共用向量。

**衔接下一步**：中断机制在 [实验4 串口实验](../实验4%20串口实验) 里立刻复用一次——串口用的是"接收缓冲区非空中断"（`USART->CR1` 的 RXNEIE + `NVIC` 使能），流程和 EXTI 几乎一样，只是把"AFIO/EXTICR/边沿"换成"USART 的 SR/CR1 标志与使能位"；同时实验 4 会教你算 `BRR` 波特率分频值。
