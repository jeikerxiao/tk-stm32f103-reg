# 实验8 通用定时器实验

> 一句话概括：把 STM32F103 里最强的四类"计数器"玩法一次练齐——用通用定时器 TIM3 做 500ms 定时中断、用 TIM2 输出 PWM、用 TIM5 捕获外部脉冲宽度、把 TIM2 当外部脉冲计数器，掌握 PSC/ARR/CCR/EGR/SMCR 这几个寄存器位到底怎么填。

本实验包含 4 个子实验，每个子实验都是一份**完整独立的 Keil 工程**（各自带 `User`、`Drivers`、`Projects`）：

| 子实验 | 目录 | 用的定时器 | 干什么 |
| --- | --- | --- | --- |
| 8-1 | `实验8-1 通用定时器中断实验` | TIM3 | 500ms 定时中断翻绿灯 |
| 8-2 | `实验8-2 通用定时器PWM输出实验` | TIM2 CH1（PA0） | PWM 呼吸灯 |
| 8-3 | `实验8-3 通用定时器输入捕获实验` | TIM5 CH1（PA0） | 捕获高电平脉宽并串口打印 |
| 8-4 | `实验8-4 通用定时器脉冲计数实验` | TIM2 CH1（PA0） | 外部脉冲个数统计 |

下文所有代码链接都相对本 README 所在目录（形如 `[gtim.c](<实验8-1 通用定时器中断实验/Drivers/BSP/TIMER/gtim.c>)`）。

## 一、实验简介

- **实验目的**（取自各子实验官方 `readme.txt`）：
  - 8-1：学习通用定时器中断的使用。
  - 8-2：学习通用定时器的 PWM 功能。
  - 8-3：学习通用定时器输入捕获功能。
  - 8-4：学习通用定时器输入脉冲计数。
- **通俗展开**：寄存器就是一个 CPU 内部可读写的 32 位"控制格子"，配置外设就是按位往里填数。
  通用定时器（General Purpose Timer，TIM2/TIM3/TIM4/TIM5）本质上是一个**会自动数的计数器**：
  给它一个"多久数一下"（PSC 预分频）、"数到几算一圈"（ARR 自动重装值），它就能自己跑，
  数满一圈（溢出/更新）时按你的要求通知 CPU（中断）、翻转引脚（PWM/输出比较）、或者反过来去数别人（输入捕获/脉冲计数）。
- **在学习路线中的位置**：
  - 前置：[实验7 基本定时器实验](../实验7%20基本定时器实验)（PSC/ARR + 更新中断这条主干，本实验 8-1 完全复用它的写法）；
    [实验2 按键输入实验](../实验2%20按键输入实验)、[实验3 外部中断实验](../实验3%20外部中断实验)（GPIO 输入、NVIC 中断）。
  - 后续：[实验9 高级定时器实验](../实验9%20高级定时器实验)（在通用定时器之上多了刹车、互补输出、死区、重复计数器）；
    实验22 红外遥控（`REMOTE_IN_TIMX = TIM5` 做捕获解码头码）、实验41 综合测试（同时用到 `gtim.c`/`atim.c`）都是本实验思路的工程化应用。
- **开发板与芯片**：正点原子 MiniSTM32 V4，主芯片 **STM32F103RC**（Cortex-M3，72MHz，512KB Flash / 48KB SRAM），Keil MDK 寄存器版工程。

## 二、实验原理

### 2.1 通用定时器的计数链路：CK_PSC → PSC → CK_CNT → CNT → ARR

```text
72MHz 定时器时钟 TIMxCLK
      ↓  ÷(PSC+1)
CK_CNT（计数脉冲）
      ↓  每来一个脉冲
CNT 计数器 0 → 1 → 2 … → ARR
      ↓  CNT == ARR 时
更新事件（Update Event）：SR.UIF 置 1，CNT 重新从 0 开始，ARR 缓冲值重新装载
      ↓  若 DIER.UIE = 1
向 NVIC 申请 TIMx 中断 → 执行 TIMx_IRQHandler
```

- **PSC（Prescaler，预分频器）**：`TIMx->PSC`，16 位。硬件每收到 `(PSC+1)` 个 `TIMxCLK` 才给计数器 1 个脉冲，
  所以写 71 就是 72 分频。**+1 的原因**：写 0 表示"1 分频"（不分频）。
- **ARR（Auto-Reload Register，自动重装值）**：`TIMx->ARR`，TIM2~TIM5 都是 16 位（最大 65535）。
  CNT 数到 ARR 就溢出，所以一圈要数 `(ARR+1)` 个脉冲。
- **CCR（Capture/Compare Register，输出比较/捕获寄存器）**：`TIMx->CCR1~CCR4`，与 CNT 比较，
  输出模式时决定翻转时刻（→PWM），输入模式时用来"存下 CNT 的快照"（→捕获）。

**溢出时间公式（本工程 `gtim.c` 注释里给出的版本）**：

```text
Tout = ((ARR + 1) * (PSC + 1)) / Ft      单位: us    （Ft = 定时器时钟频率, 单位 MHz）
```

### 2.2 通用定时器的时钟为什么是 72MHz（初学者最容易算错的一步）

TIM2~TIM5 挂在 **APB1** 总线上。[sys.c](<实验8-1 通用定时器中断实验/Drivers/SYSTEM/sys/sys.c>) 的 `sys_clock_set()` 里：

```c
        RCC->CFGR = 0X00000400;     /* PCLK1 = HCLK / 2; PCLK2 = HCLK; HCLK = SYSCLK; */
```

`0x400` 即 `PPRE1[2:0] = 100`（APB1 二分频）。`main()` 里的 `sys_stm32_clock_init(9)` → 8MHz HSE × 9 = 72MHz，于是：

| 时钟 | 数值 | 说明 |
| --- | --- | --- |
| HCLK / SYSCLK | 72MHz | 内核与 AHB |
| PCLK2（APB2） | 72MHz | 不分频，TIM1/TIM8/USART1/GPIO 用它 |
| PCLK1（APB1） | 36MHz | 二分频（上限 36MHz），USART2~5、I2C、SPI2/3 用它 |
| **TIM2~TIM5 时钟** | **72MHz** | **RM 规定：当 APB1 分频系数 ≥ 2 时，定时器时钟 = PCLK1 × 2** |

> 这条"×2"规则在 `gtim.c` 的函数注释里也写明了。本实验所有计算都用 **Ft = 72MHz**，不要误用 36MHz，否则时间会差一倍。

### 2.3 PWM 是怎么"数出来"的（占空比公式）

`TIMx->CCMR1` 的 `OCxM[2:0] = 110` 就是 **PWM 模式 1**：

```text
CNT <  CCRx  →  通道"有效"
CNT >= CCRx  →  通道"无效"
```

"有效"到底是高电平还是低电平，由 `TIMx->CCER` 的 `CCxP` 位决定：`CCxP=0` 高电平有效，`CCxP=1` 低电平有效。

**占空比（高电平时间占比）计算**：

```text
PWM 频率   = Ft / ((ARR + 1) * (PSC + 1))
高电平占比 = CCRx / (ARR + 1)        （CCxP = 0，高电平有效时）
           = 1 - CCRx / (ARR + 1)    （CCxP = 1，低电平有效时）
```

工程上常见简写 `占空比 = CCR/ARR`：当 ARR 很大时 `+1` 可以忽略，但严格值应带 `+1`，本实验 ARR=499 时两者相差 0.2%，够用但不严谨。

### 2.4 输入捕获：两次捕获 + 溢出累加（8-3 的核心）

一次"高电平有多宽"的测量，靠**同一个通道的两次捕获事件**完成（`TIMx->CCER.CCxP` 决定捕捉哪种边沿）：

1. 初始 `CCxP = 0` → 捕捉**上升沿**；上升沿到来时硬件把 `CNT` 快照进 `CCRx`，并置 `SR.CCxIF`。
2. 软件在上升沿中断里：把 `CNT` 清 0（从零开始计时）、把 `CCxP` 改成 1 → 切换成捕捉**下降沿**。
3. 下降沿到来时硬件再次把 `CNT` 快照进 `CCRx` —— 这次快照就是**高电平持续了几个计数脉冲**。
4. 如果高电平比 ARR 一圈还长，中间会不断发生溢出（`SR.UIF`），中断里给溢出次数 +1，
   最终时间 = `溢出次数 × (ARR+1) + CCRx`（本工程按 16 位统一处理，`ARR+1 = 65536`）。

**"定时器重装载"在这里的意义**：因为 CNT 只有 16 位（最大 65535），一次数不满长脉冲，所以靠
"每次上升沿清零 CNT + 溢出次数累加"来扩展量程，代价是最多只能累加 63 次（用状态字节低 6 位存）。

顺便区分两个易混概念：

- **测周期/测频率** = 同一个边沿连续捕获**两次**，两次的 `CCRx` 差值就是 1 个周期的计数值，`f = Ft/(PSC+1)/Δ`。
- **测占空比** = 上升沿捕获一次 + 下降沿捕获一次（即 8-3 的两次异边沿捕获），`D = 高电平计数值 / 周期计数值`。
  8-3 只算了高电平时间，周期没测；想直接得占空比，可参考实验 9-4 的 PWM 输入模式（硬件帮你同时量好两者）。

### 2.5 EGR 的 UG 位、CR1 的 CEN 位与"从模式"（8-4 的核心）

- **`TIMx->CR1` bit0 = CEN（Counter Enable，计数器使能）**，就是工程注释里说的"CE"。
  注意它**不在 EGR 里**：`CR1.CEN = 1` 才开始计数，`= 0` 立刻停。
- **`TIMx->EGR`（Event Generation Register，事件生成寄存器）bit0 = UG（Update Generation）**：
  软件写 1，硬件**立刻**制造一次更新事件——最重要的副作用是"让刚写进 `PSC` 的值马上生效"。
  预分频器有影子寄存器，正常情况要等到下一次溢出才把 `PSC` 的新值装进去；写 UG 就不用等。
  本工程 8-3/8-4 初始化里都有：

  ```c
      GTIM_TIMX_CAP->EGR  |= 1 << 0;  /* 软件控制产生更新事件,使写入PSC的值立即生效,否则将会要等到定时器溢出才会生效 */
  ```

  其余位 `CC1G~CC4G`（bit1~bit4）是软件强制触发一次比较/捕获，本实验没用到。
- **从模式（Slave Mode）**：`TIMx->SMCR` 里的 `SMS[2:0]`（bit2:0）选择"别人怎么控制我这个定时器"，
  `TS[2:0]`（bit6:4）选择"用哪一路外部信号作为触发源"。本工程用到两种：

  | SMS | 名称 | 作用 | 本实验哪里用 |
  | --- | --- | --- | --- |
  | `111` | **外部时钟模式 1** | 触发源的每个有效边沿 = CNT 加 1（计数脉冲来自引脚，不是内部时钟） | 8-4 脉冲计数 |
  | `100` | **复位模式** | 触发源一有效就把 CNT 清 0 | 9-4 PWM 输入模式（高级定时器篇） |

  `TS` 常用取值（本工程注释的写法）：`5 = TI1FP1`（通道 1 滤波后的信号）、`6 = TI2FP2`（通道 2 滤波后的信号）。
  所以 8-4 里 `SMCR |= (4 + chy) << 4` 且 `chy = 1` → TS=5 → 用 CH1 的边沿驱动计数。
  因为计数脉冲来自引脚，**PSC 依然会先对输入脉冲做 `(PSC+1)` 分频**，所以 8-4 把 `PSC` 设 0（每个脉冲都数）。

## 三、硬件资源与接线

### 3.1 实验8-1 通用定时器中断实验

| 资源 | 引脚 | 代码中的宏 / 位置 | 说明 |
| --- | --- | --- | --- |
| LED0（红灯） | PA8 | `LED0_GPIO_PORT/PIN`，[led.h](<实验8-1 通用定时器中断实验/Drivers/BSP/LED/led.h>) | 主循环 `delay_ms(200)` 翻转，指示程序在跑 |
| LED1（绿灯） | PD2 | `LED1_GPIO_PORT/PIN`，同上 | TIM3 中断里 `LED1_TOGGLE()`，500ms 一次 |
| TIM3 | 无对外引脚 | `GTIM_TIMX_INT = TIM3`，[gtim.h](<实验8-1 通用定时器中断实验/Drivers/BSP/TIMER/gtim.h>) | 纯片内定时，只申请 `TIM3_IRQn`（=29） |
| TIM3 时钟 | — | `GTIM_TIMX_INT_CLK_ENABLE()`：`RCC->APB1ENR |= 1 << 1`（TIM3EN，bit1） | 忘开则寄存器写不进 |
| GPIOA / GPIOD 时钟 | — | `RCC->APB2ENR` bit2 / bit5 | 由 `led_init()` 打开 |

- **接线**：全板载，不需要杜邦线、不需要跳线帽。
- **官方 `readme.txt` 与代码一致**：TIM3、500ms 中断、LED1 翻转，都能对上。

### 3.2 实验8-2 通用定时器PWM输出实验

| 资源 | 引脚 | 代码中的宏 / 位置 | 说明 |
| --- | --- | --- | --- |
| PWM 输出（TIM2_CH1） | **PA0** | `GTIM_TIMX_PWM=TIM2`、`GTIM_TIMX_PWM_CHY=1`、`GTIM_TIMX_PWM_CHY_GPIO_PIN=SYS_GPIO_PIN0`，[gtim.h](<实验8-2 通用定时器PWM输出实验/Drivers/BSP/TIMER/gtim.h>) | 复用推挽输出，1MHz 计数、2kHz PWM |
| LED0（红灯） | PA8 | [led.h](<实验8-2 通用定时器PWM输出实验/Drivers/BSP/LED/led.h>) | 低电平点亮，被 PWM 控制亮度 |
| **杜邦线** | **PA0 → PA8** | [User/main.c](<实验8-2 通用定时器PWM输出实验/User/main.c>) 注释 | 必须短接，否则看不到亮度变化 |
| TIM2 时钟 | — | `GTIM_TIMX_PWM_CHY_CLK_ENABLE()`：`RCC->APB1ENR |= 1 << 0`（TIM2EN） | |
| GPIOA 时钟 | — | `GTIM_TIMX_PWM_CHY_GPIO_CLK_ENABLE()`：`RCC->APB2ENR |= 1 << 2`（IOPAEN） | |

- **为什么要飞线**：板上 LED0 固定在 PA8，而本工程用的是 TIM2_CH1，它的默认引脚是 PA0，
  所以用一根杜邦线把 PA0 的 PWM 接到 PA8 上驱动 LED0。
- **接线前注意**：`main()` 先把 PA8 的 `CRH` 低 4 位清零，让 LED0 的 GPIO 让位给外部输入，避免和 PA0 打架。

### 3.3 实验8-3 通用定时器输入捕获实验

| 资源 | 引脚 | 代码中的宏 / 位置 | 说明 |
| --- | --- | --- | --- |
| 捕获输入（TIM5_CH1） | **PA0** | `GTIM_TIMX_CAP=TIM5`、`GTIM_TIMX_CAP_CHY=1`，[gtim.h](<实验8-3 通用定时器输入捕获实验/Drivers/BSP/TIMER/gtim.h>) | TIM5_CH1 默认脚就是 PA0，无需重映射 |
| 激励源 | **WK_UP 按键（PA0）** | `WKUP_GPIO_PORT/PIN`，[key.h](<实验8-3 通用定时器输入捕获实验/Drivers/BSP/KEY/key.h>) | 按下时 PA0 被接到 3.3V → 产生一个高电平脉冲 |
| LED0（红灯） | PA8 | [led.h](<实验8-3 通用定时器输入捕获实验/Drivers/BSP/LED/led.h>) | 200ms 翻转，指示程序运行 |
| 串口1 | TX=PA9、RX=PA10 | [usart.h](<实验8-3 通用定时器输入捕获实验/Drivers/SYSTEM/usart/usart.h>)，115200 | 打印捕获到的脉宽 |
| TIM5 时钟 | — | `GTIM_TIMX_CAP_CHY_CLK_ENABLE()`：`RCC->APB1ENR |= 1 << 3`（TIM5EN） | |

- **接线/跳线（板载 USB 转串口）**：USB 线插板上 **USB_UART** 座；丝印 `USART1` 的 2×3 排针上，
  用跳线帽把 **PA9 接 RXD、PA10 接 TXD**（互连，即 MCU 的发送接转换芯片的接收）。
- **激励怎么给**：直接用手按住 WK_UP 键（PA0）0.1~3 秒再松开，就能在串口助手看到 `HIGH:xxxx us`。
  想测外部信号，就把信号接到 PA0（注意电压 3.3V 电平、别超压）。

### 3.4 实验8-4 通用定时器脉冲计数实验

| 资源 | 引脚 | 代码中的宏 / 位置 | 说明 |
| --- | --- | --- | --- |
| 计数输入（TIM2_CH1） | **PA0** | `GTIM_TIMX_CNT=TIM2`、`GTIM_TIMX_CNT_CHY=1`，[gtim.h](<实验8-4 通用定时器脉冲计数实验/Drivers/BSP/TIMER/gtim.h>) | 外部时钟模式 1，上升沿 +1 |
| KEY0 | PC5 | `KEY0_GPIO_PORT/PIN`，[key.h](<实验8-4 通用定时器脉冲计数实验/Drivers/BSP/KEY/key.h>) | 按下 → `gtim_timx_cnt_chy_restart()` 清零重新计数 |
| WK_UP | PA0 | 同上 | 每按一次产生一个上升沿，等于"送一个脉冲" |
| LED0（红灯） | PA8 | [led.h](<实验8-4 通用定时器脉冲计数实验/Drivers/BSP/LED/led.h>) | 200ms 翻转 |
| 串口1 | PA9/PA10，115200 | [usart.h](<实验8-4 通用定时器脉冲计数实验/Drivers/SYSTEM/usart/usart.h>) | 打印 `CNT:n` |
| TIM2 时钟 | — | `GTIM_TIMX_CNT_CHY_CLK_ENABLE()`：`RCC->APB1ENR |= 1 << 0` | |

- **只有 CH1/CH2 能这样数外部脉冲**：`gtim.h` 注释明确"只有CH1和CH2通道可以用做输入计数, CH3/CH4不支持"
  （因为触发选择 `TS` 只支持 TI1FP1/TI2FP2）。
- 串口跳线要求与 8-3 完全相同（PA9→RXD、PA10→TXD，USB 插 USB_UART）。

## 四、代码结构与调用流程

```text
实验8 通用定时器实验/
├── 实验8-1 通用定时器中断实验/
│   ├── User/main.c                        # 初始化 + 主循环 200ms 翻 LED0
│   ├── Drivers/BSP/TIMER/gtim.c/.h        # 通用定时器：TIM3 中断初始化 + 中断服务函数
│   ├── Drivers/BSP/TIMER/btim.c/.h        # 基本定时器(TIM6) 驱动，来自实验7，本工程未加入编译
│   ├── Drivers/BSP/LED/led.c/.h           # LED0(PA8)/LED1(PD2)
│   ├── Drivers/SYSTEM/{sys,delay,usart}   # 时钟/NVIC/GPIO 公共层、SysTick 延时、串口
│   └── Projects/MDK-ARM/atk_f103.uvprojx  # Target 名 GTIM
├── 实验8-2 通用定时器PWM输出实验/          # gtim.c/.h 增加 gtim_timx_pwm_chy_init()
├── 实验8-3 通用定时器输入捕获实验/          # 再加 gtim_timx_cap_chy_init()、BSP/KEY
└── 实验8-4 通用定时器脉冲计数实验/          # 再加 cnt_chy_init / get_count / restart
```

- 4 个子工程的 `gtim.c` 是**同一份文件逐步加长**的版本（V1.0→V1.3），
  8-4 里既保留了 8-2 的 PWM 函数也保留了 8-3 的捕获函数，只是 `main()` 没调用；工程文件只把 `gtim.c`、`led.c`（+`key.c`）加进编译，`btim.c` 没有加入。
- `main()` 通用流程：

```text
① sys_stm32_clock_init(9)     72MHz（顺带把所有外设时钟先关掉、复位 RCC）
② delay_init(72)              SysTick 延时基准
③ usart_init(72, 115200)      串口1，printf 走它（8-1/8-2 实际不打印，只是模板）
④ led_init() / key_init()     PA8、PD2 输出；PC5/PA15/PA0 输入
⑤ gtim_timx_*_init(...)       配置本次实验要用的那台通用定时器
⑥ while(1)                    主循环做事；定时/捕获/计数全在中断里完成
```

## 五、核心代码解析

### 5.1 实验8-1：宏把"用哪台定时器"集中起来

[gtim.h](<实验8-1 通用定时器中断实验/Drivers/BSP/TIMER/gtim.h>)：

```c
#define GTIM_TIMX_INT                       TIM3
#define GTIM_TIMX_INT_IRQn                  TIM3_IRQn
#define GTIM_TIMX_INT_IRQHandler            TIM3_IRQHandler
#define GTIM_TIMX_INT_CLK_ENABLE()          do{ RCC->APB1ENR |= 1 << 1; }while(0)   /* TIM3 时钟使能 */
```

4 个宏分别绑定"寄存器基址 / NVIC 中断号 / C 中断函数名 / 时钟使能位"，想换成 TIM4 只改这 4 行。
`do{...}while(0)` 是宏的惯用写法，保证它在 `if (x) MACRO(); else ...` 里也只执行一次。

[gtim.c](<实验8-1 通用定时器中断实验/Drivers/BSP/TIMER/gtim.c>) 的初始化按步骤拆：

```c
void gtim_timx_int_init(uint16_t arr, uint16_t psc)
{
    GTIM_TIMX_INT_CLK_ENABLE();
    GTIM_TIMX_INT->ARR = arr;           /* 设定计数器自动重装值 */
    GTIM_TIMX_INT->PSC = psc;           /* 设置预分频器  */
    GTIM_TIMX_INT->DIER |= 1 << 0;      /* 允许更新中断 */
    GTIM_TIMX_INT->CR1 |= 1 << 0;       /* 使能定时器TIMX */
    sys_nvic_init(1, 3, GTIM_TIMX_INT_IRQn, 2); /* 抢占1，子优先级3，组2 */
}
```

1. 开时钟（APB1 bit1）——顺序必须在最前面，否则后面全是白写。
2. `ARR = 4999`、`PSC = 7199`：`Tout = (5000 × 7200)/72 = 500000us`，即 **500ms**；计数频率 = 72M/7200 = 10kHz。
3. `DIER |= 1<<0`：置 **UIE**（更新中断允许），否则 `SR.UIF` 置位也没人通知 NVIC。
4. `CR1 |= 1<<0`：置 **CEN**，计数器开始跑。
5. `sys_nvic_init(1, 3, TIM3_IRQn, 2)`：NVIC 分组 2、抢占 1、子 3，并把 `NVIC->ISER[0]` 的 bit29 置 1。

中断服务函数（同文件）：

```c
void GTIM_TIMX_INT_IRQHandler(void)
{ 
    if (GTIM_TIMX_INT->SR & 0X0001)   /* 溢出中断 */
    {
        LED1_TOGGLE();
    }

    GTIM_TIMX_INT->SR &= ~(1 << 0); /* 清除中断标志位 */
} 
```

**必须清 `SR.UIF`**（bit0），不然退出中断后又立刻满足条件，绿灯会变成高频"常亮"。

[User/main.c](<实验8-1 通用定时器中断实验/User/main.c>) 全文逻辑（去掉文件头注释）：

```c
int main(void)
{
    sys_stm32_clock_init(9);    /* 设置时钟, 72Mhz */
    delay_init(72);             /* 延时初始化 */
    usart_init(72, 115200);     /* 串口初始化为115200 */
    led_init();                 /* 初始化LED */
    gtim_timx_int_init(5000 - 1, 7200 - 1); /* 10Khz的计数频率，计数5K次为500ms */

    while (1)
    {
        LED0_TOGGLE();
        delay_ms(200);
    }
}
```

两条时间线并行：主循环 200ms 翻红灯，TIM3 中断 500ms 翻绿灯。

### 5.2 实验8-2：PWM 输出初始化，逐位拆解

[gtim.c](<实验8-2 通用定时器PWM输出实验/Drivers/BSP/TIMER/gtim.c>)：

```c
void gtim_timx_pwm_chy_init(uint16_t arr, uint16_t psc)
{
    uint8_t chy = GTIM_TIMX_PWM_CHY;
    GTIM_TIMX_PWM_CHY_GPIO_CLK_ENABLE();    /* TIMX 通道 IO口时钟使能 */
    GTIM_TIMX_PWM_CHY_CLK_ENABLE();         /* TIMX 时钟使能 */

    sys_gpio_set(GTIM_TIMX_PWM_CHY_GPIO_PORT, GTIM_TIMX_PWM_CHY_GPIO_PIN,
                 SYS_GPIO_MODE_AF, SYS_GPIO_OTYPE_PP, SYS_GPIO_SPEED_HIGH, SYS_GPIO_PUPD_PU);    /* TIMX PWM CHY 引脚模式设置 */

    GTIM_TIMX_PWM->ARR = arr;       /* 设定计数器自动重装值 */
    GTIM_TIMX_PWM->PSC = psc;       /* 设置预分频器  */
    GTIM_TIMX_PWM->BDTR |= 1 << 15; /* 使能MOE位(仅TIM1/8 有此寄存器,必须设置MOE才能输出PWM), 其他通用定时器, 这个
                                     * 寄存器是无效的, 所以设置/不设置并不影响结果, 为了兼容这里统一改成设置MOE位
                                     */

    if (chy <= 2)
    {
        GTIM_TIMX_PWM->CCMR1 |= 6 << (4 + 8 * (chy - 1));   /* CH1/2 PWM模式1 */
        GTIM_TIMX_PWM->CCMR1 |= 1 << (3 + 8 * (chy - 1));   /* CH1/2 预装载使能 */
    }
    ...
    GTIM_TIMX_PWM->CCER |= 1 << (4 * (chy - 1));        /* OCy 输出使能 */
    GTIM_TIMX_PWM->CCER |= 1 << (1 + 4 * (chy - 1));    /* OCy 低电平有效 */
    GTIM_TIMX_PWM->CR1 |= 1 << 7;   /* ARPE使能 */
    GTIM_TIMX_PWM->CR1 |= 1 << 0;   /* 使能定时器TIMX */
}
```

`chy = 1` 时那两行 `CCMR1` 展开为：

| 写法 | 位 | 名字 | 作用 |
| --- | --- | --- | --- |
| `6 << (4 + 8*0)` = `6<<4` | bit6:4 | `OC1M = 110` | **PWM 模式 1**（CNT<CCR 有效） |
| `1 << (3 + 8*0)` = `1<<3` | bit3 | `OC1PE = 1` | CCR1 预装载：新值等更新事件才生效，避免中途改占空比出毛刺 |
| `1 << (4*0)` = `1<<0` | bit0 | `CC1E = 1` | 打开通道 1 输出 |
| `1 << (1+4*0)` = `1<<1` | bit1 | `CC1P = 1` | **低电平有效**（本实验刻意反过来，见下） |
| `CR1 |= 1<<7` | bit7 | `ARPE = 1` | ARR 也走预装载 |
| `CR1 |= 1<<0` | bit0 | `CEN = 1` | 启动计数器 |

`BDTR bit15 = MOE` 是高级定时器（TIM1/TIM8）的主输出使能位，TIM2 写它没作用，代码留着只为"一份驱动通吃"。

**代入数值**：`gtim_timx_pwm_chy_init(500 - 1, 72 - 1)` → `ARR=499`、`PSC=71`：

- 计数频率 = 72MHz / (71+1) = **1MHz**（1 个数 = 1us）
- PWM 周期 = (499+1) × 1us = 500us → 频率 = 1/500us = **2kHz**
- `CC1P = 1`（低电平有效）→ 引脚**低电平**时间占比 = `CCR1/(ARR+1) = CCR1/500`
- LED0 是低电平点亮 → **导通（亮）比例 = CCR1/500**，`CCR1 = 0` 全灭，`CCR1 = 301` 时约 60.2% 亮度

[User/main.c](<实验8-2 通用定时器PWM输出实验/User/main.c>) 的呼吸逻辑：

```c
    /* 清除 CNF8[1:0] = 0, MODE8[1:0] = 0, 将PA8设置成浮空输入模式, 避免和PA0冲突 */
    GPIOA->CRH &= ~(0X000F);
    
    /* 需要用杜邦线 短接 PA0 和 PA8, 才能看到PWM输出!!! */
    gtim_timx_pwm_chy_init(500 - 1, 72 - 1);    /* 1Mhz的计数频率,2Khz的PWM. */

    while (1)
    {
        delay_ms(10);

        if (dir)ledrpwmval++;
        else ledrpwmval--;

        if (ledrpwmval > 300)dir = 0;
        if (ledrpwmval == 0)dir = 1;

        GTIM_TIMX_PWM_CHY_CCRX = ledrpwmval;
    }
```

`ledrpwmval` 在 0↔301 之间往返，每 10ms 走一步 → 一个"暗→亮→暗"循环约 6 秒。
`GTIM_TIMX_PWM_CHY_CCRX` 就是 `TIM2->CCR1` 的别名宏，直接改占空比，无需重配定时器。

### 5.3 实验8-3：输入捕获的两次捕获与溢出累加

状态字节设计（[gtim.c](<实验8-3 通用定时器输入捕获实验/Drivers/BSP/TIMER/gtim.c>) 注释原文）：

```c
/* 输入捕获状态(g_timxchy_cap_sta)
 * [7]  :0,没有成功的捕获;1,成功捕获到一次.
 * [6]  :0,还没捕获到高电平;1,已经捕获到高电平了.
 * [5:0]:捕获高电平后溢出的次数,最多溢出63次,所以最长捕获值 = 63*65536 + 65535 = 4194303
 *       注意:为了通用,我们默认ARR和CCRy都是16位寄存器,对于32位的定时器(如:TIM5),也只按16位使用
 *       按1us的计数频率,最长溢出时间为:4194303 us, 约4.19秒
 */
uint8_t g_timxchy_cap_sta = 0;  /* 输入捕获状态 */
uint32_t g_timxchy_cap_val =0 ; /* 输入捕获值 */
```

初始化关键位（同文件 `gtim_timx_cap_chy_init(0XFFFF, 72 - 1)`，`chy=1`）：

```c
    GTIM_TIMX_CAP->CCMR1 |= 1 << 8 * (chy - 1);        /* CCyS[1:0]   = 01 选择输入端 IC1/2映射到TI1/2上 */
    GTIM_TIMX_CAP->CCMR1 |= 0 << (2 + 8 * (chy - 1));  /* ICyPSC[1:0] = 00 输入捕获不分频,全捕获 */
    GTIM_TIMX_CAP->CCMR1 |= 0 << (4 + 8 * (chy - 1));  /* ICyF[3:0]   = 00 输入端滤波 不滤波 */

    GTIM_TIMX_CAP->CCER |= 1 << (4 * (chy - 1));       /* CCyE = 1 输入捕获使能 */
    GTIM_TIMX_CAP->CCER |= 0 << (1 + 4 * (chy - 1));   /* CCyP = 0 捕获上升沿 ,注意:CCyNP使用默认值0 */

    GTIM_TIMX_CAP->EGR  |= 1 << 0;  /* 软件控制产生更新事件,使写入PSC的值立即生效,否则将会要等到定时器溢出才会生效 */
    GTIM_TIMX_CAP->DIER |= 1 << 1;  /* 允许捕获中断 */
    GTIM_TIMX_CAP->DIER |= 1 << 0;  /* 允许更新中断 */
```

| 位 | 值 | 含义 |
| --- | --- | --- |
| `CCMR1.CC1S = 01` | 输入方向 | IC1 接到 TI1（=PA0），通道 1 变成捕获 |
| `CCMR1.IC1PSC = 00` | 不分频 | 每个边沿都捕获（2/4 分频可抗抖动） |
| `CCMR1.IC1F = 0000` | 不滤波 | 想抗噪可设 0100/1000/1010（N 个采样确认） |
| `CCER.CC1E = 1` | 使能 | 允许捕获 |
| `CCER.CC1P = 0` | 上升沿 | 先捕上升沿；中断里再改成 1 捕下降沿 |
| `DIER.CC1IE = 1`、`UIE = 1` | 两类中断 | 捕获中断 + 溢出中断都要开 |
| `EGR.UG = 1` | 软件更新 | 让 `PSC=71` 立即生效（不等溢出） |

中断服务函数（逐段）：

```c
        if (tsr & (1 << 0)) /* 溢出中断 */
        {
            if (g_timxchy_cap_sta & 0X40)   /* 已经捕获到高电平了 */
            {
                if ((g_timxchy_cap_sta & 0X3F) == 0X3F) /* 高电平太长了 */
                {
                    g_timxchy_cap_sta |= 0X80;  /* 标记成功捕获了一次 */
                    g_timxchy_cap_val = 0XFFFF;
                }
                else    /* 还可以累加高电平长度 */
                {
                    g_timxchy_cap_sta++;   
                }
            }
        }

        if (tsr & (1 << chy))   /* 通道y 发生了捕获事件 */
        {
            if (g_timxchy_cap_sta & 0X40)   /* 捕获到一个下降沿 */
            {
                g_timxchy_cap_sta |= 0X80;  /* 标记成功捕获到一次高电平脉宽 */
                g_timxchy_cap_val = GTIM_TIMX_CAP_CHY_CCRX; /* 获取当前的捕获值. */
                
                GTIM_TIMX_CAP->CCER &= ~(1 << (1 + 4 * (chy - 1)));/* CCyP = 0 设置为上升沿捕获 */ 
            }
            else    /* 还未开始,第一次捕获上升沿 */
            {
                g_timxchy_cap_val = 0;
                g_timxchy_cap_sta = 0X40;   /* 标记捕获到了上升沿 */
                GTIM_TIMX_CAP->CNT = 0;     /* 计数器清空 */ 
                GTIM_TIMX_CAP->CCER |= 1 << (1 + 4 * (chy - 1));    /* CCyP = 1 设置为下降沿捕获 */ 
            }
        }
```

流程：上升沿 → 清 `CNT`、置 bit6、把极性改成下降沿；期间每次溢出 bit5:0 +1；下降沿 → 读 `CCR1` 存进 `g_timxchy_cap_val`、置 bit7、极性改回上升沿；函数末尾 `SR = 0` 一次清掉全部标志（UIF/CC1IF）。

主循环把"溢出次数 × 65536 + CCR1"还原成总时长（[User/main.c](<实验8-3 通用定时器输入捕获实验/User/main.c>)）：

```c
        if (g_timxchy_cap_sta & 0X80)       /* 成功捕获到了一次高电平 */
        {
            temp = g_timxchy_cap_sta & 0X3F;
            temp *= 65536;                  /* 溢出时间总和 */
            temp += g_timxchy_cap_val;      /* 得到总的高电平时间 */
            printf("HIGH:%d us\r\n", temp); /* 打印总的高点平时间 */
            g_timxchy_cap_sta = 0;          /* 开启下一次捕获*/
        }
```

计数频率 1MHz ⇒ **1 个计数 = 1us**，所以打印值直接就是微秒。`sta = 0` 清 bit7 后，下一次上升沿才能重新开始捕获。

### 5.4 实验8-4：从模式外部时钟 1 + 溢出次数换算总脉冲数

[gtim.c](<实验8-4 通用定时器脉冲计数实验/Drivers/BSP/TIMER/gtim.c>) 初始化：

```c
    GTIM_TIMX_CNT->ARR = 65535; /* 设定计数器自动重装值为最大 */
    GTIM_TIMX_CNT->PSC = psc;   /* 设置预分频器  */

    GTIM_TIMX_CNT->CCMR1 |= 1 << 8 * (chy - 1);        /* CCyS[1:0]   = 01 选择输入端 IC1/2映射到TI1/2上 */
    GTIM_TIMX_CNT->CCMR1 |= 0 << (2 + 8 * (chy - 1));  /* ICyPSC[1:0] = 00 输入捕获不分频,全捕获 */
    GTIM_TIMX_CNT->CCMR1 |= 0 << (4 + 8 * (chy - 1));  /* ICyF[3:0]   = 00 输入端滤波 不滤波 */
    
    GTIM_TIMX_CNT->CCER |= 1 << (4 * (chy - 1));       /* CCyE = 1 输入捕获使能 */
    GTIM_TIMX_CNT->CCER |= 0 << (1 + 4 * (chy - 1));   /* CCyP = 0 捕获上升沿,即上升沿计数 ,注意:CCyNP使用默认值0 */

    GTIM_TIMX_CNT->SMCR |= (4 + chy) << 4;  /* TS[4:0] = 5/6  触发选择: 5,TI1FP1(TIMX_CH1);  6,TI2FP2(TIMX_CH2); */
    GTIM_TIMX_CNT->SMCR |= 7 << 0;          /* SMS[2:0] = 7   从模式选择: 外部时钟模式1 */
```

- 通道仍按"输入捕获"配（CC1S=01、CC1E=1、CC1P=0 上升沿），目的是**生成 TI1FP1 这条滤波后的信号**。
- `SMCR.TS = 5` 选 TI1FP1 作触发源、`SMCR.SMS = 111` 外部时钟模式 1 ⇒ CNT 的计数脉冲**不再来自 72MHz 内部时钟，而是来自 PA0 上的上升沿**。
  此时 `PSC` 变成"每 (PSC+1) 个外部脉冲才数 1 下"，`gtim.c` 注释因此建议设 0（每个脉冲都数，精度最高），主程序也正是传 `0`。
  `ARR` 仍是 65535，所以每数满 65536 个脉冲溢出一次。溢出中断里只做一件事：

```c
void GTIM_TIMX_CNT_IRQHandler(void)
{ 
    if (GTIM_TIMX_CNT->SR & (1 << 0))   /* 溢出中断 */
    {
        g_timxchy_cnt_ofcnt++;          /* 累计溢出次数 */
    } 
    GTIM_TIMX_CNT->SR = 0;              /* 清除所有中断标志位 */
}
```

- 总脉冲数 = 溢出次数 × 65536 + 当前 CNT（`>>16` 相当于把 CNT 拼在高位）：

```c
uint32_t gtim_timx_cnt_chy_get_count(void)
{
    uint32_t count = 0;
    count = g_timxchy_cnt_ofcnt * 65536;/* 计算溢出次数对应的计数值 */
    count += GTIM_TIMX_CNT->CNT;        /* 加上当前CNT的值 */
    return count;
}

void gtim_timx_cnt_chy_restart(void)
{
    GTIM_TIMX_CNT->CR1  &= (1 << 0);    /* 关闭定时器TIMX */
    g_timxchy_cnt_ofcnt = 0;            /* 累加器清零 */
    GTIM_TIMX_CNT->CNT = 0;             /* 计数器清零 */
    GTIM_TIMX_CNT->CR1  |= 1 << 0;      /* 使能定时器TIMX */
}
```

> 这里有个可以拿来考自己的细节：`GTIM_TIMX_CNT->CR1 &= (1 << 0);` 的注释写着"关闭定时器TIMX"，
> 但按位与一个只有 bit0 为 1 的掩码，效果是**保留 CEN 原值（1）、把 CR1 其它位全部清零**——定时器其实并没有被关掉，
> 只是顺手清掉了 CKD/ARPE 等配置（本例 cnt 初始化只置了 CEN，所以看不出来副作用）。
> 想"只关计数器"应写 `CR1 &= ~(1 << 0);`。读官方例程时要分清"注释的意图"和"代码的行为"。

主循环（[User/main.c](<实验8-4 通用定时器脉冲计数实验/User/main.c>)）：KEY0 按下 → `restart()` 清零重数；每 10ms 读一次计数值，变化才 `printf("CNT:%d\r\n", oldcnt)`；`t > 20`（约 200ms）翻一次 LED0。

## 六、编译、下载与运行

1. 用 Keil MDK 打开**对应子实验**的工程，例如
   `实验8-3 通用定时器输入捕获实验/Projects/MDK-ARM/atk_f103.uvprojx`（Target 名都是 `GTIM`）。
2. 工程已配置：器件 `STM32F103RC`、Pack `Keil.STM32F1xx_DFP.2.3.0`、C 宏 `STM32F103xE`、
   启动文件 `startup_stm32f103xe.s`、包含路径 `..\..\Drivers\CMSIS\Device\ST\STM32F1xx\Include;..\..\Drivers\CMSIS\Include;..\..\Drivers;..\..\User`。
   （注意：包含路径里有 `..\..\Drivers`，所以源码能写 `#include "./SYSTEM/sys/sys.h"` 这种从 `Drivers` 起头的路径。）
3. Build 后产物在 `Output/`（`atk_f103.hex` 等）；下载算法 `STM32F10x_512.FLM`（512KB Flash）。
4. 下载：ST-Link/J-Link 接板上 SWD 排针；也可用串口 ISP（BOOT0 置 1 复位）。
5. 8-3 / 8-4 需要上位机：USB 线插 **USB_UART**，排针 PA9-RXD / PA10-TXD 跳好，串口助手选 **XCOM 或 SSCOM**，
   波特率 **115200**，8N1，勾选接收区显示为 ASCII。

## 七、实验现象

### 7.1 实验8-1
1. 上电后 **LED0（红灯）快闪**：每 200ms 翻转，明暗周期约 0.4s。
2. **LED1（绿灯）慢闪**：每 500ms 进一次 TIM3 中断翻转，周期约 1s。
3. 绿灯比红灯"慢一半"，说明节拍由硬件定时器维持，与主循环的 `delay_ms` 无关。
4. 绿灯不闪 → 检查 `DIER.UIE`、NVIC 是否使能、中断里是否清了 `SR.UIF`。

### 7.2 实验8-2
1. **必须先用杜邦线短接 PA0 与 PA8**，否则 LED0 一直是 `led_init()` 设置的灭态。
2. 接好后 LED0 **由暗到亮、再由亮到暗**循环（一个来回约 6s），最亮时约 60% 导通时间（不是 100%）。
3. 用示波器看 PA0：2kHz 方波，低电平宽度随 `ledrpwmval` 0→301 变化（0~602us）。
4. 现象完全没有 → 先量 PA0 有无波形（有波形就是飞线/LED 极性问题），再查 TIM2 时钟与 `CCER.CC1E`。

### 7.3 实验8-3
1. 串口打印 200ms 一次的隐式心跳是 LED0 闪烁，屏幕上则是按下 WK_UP 松开后一行 `HIGH:xxxx us`。
2. 手按 WK_UP（PA0）1 秒 → 打印约 `HIGH:1000000 us` 量级；按住超过 4.19 秒 → 数值封顶在 `4194303`。
3. 只捕到一次就"没反应"？正常：需要**松开按键**（下降沿）才算一次完整捕获；主循环打印后会把状态清 0，可继续下一次。
4. 乱码 → 波特率不是 115200，或串口助手占用了 DTR/RTS 导致复位（换 XCOM/SSCOM）。

### 7.4 实验8-4
1. 每按一次 **WK_UP（PA0）**，串口就多打一行 `CNT:n`，n 递增（一个按键上升沿 = 至少一个脉冲）。
2. 因为 `IC1F = 0000`（不滤波）、`IC1PSC = 00`（不分频），机械按键的抖动会让一次按下**多跳几个数**，这是正常现象；
   想验证"干净的脉冲"，可以把 `GTIM_TIMX_CNT->CCMR1` 的 `IC1F` 改成 `0100`/`1000` 加输入滤波。
3. 按 **KEY0（PC5）** → `gtim_timx_cnt_chy_restart()`，下一次读到的计数从 0 重新累加。
4. LED0 每 200ms 翻转，指示主循环在跑。

## 八、注意事项

1. **清中断标志**：所有 `TIMx` 中断服务函数末尾都要清 `SR`（8-1 清 bit0，8-3/8-4 直接 `SR = 0` 清全部）。忘了清就是"反复进中断"。
2. **先开时钟再写寄存器**：`RCC->APB1ENR` 对应位（TIM2=bit0、TIM3=bit1、TIM4=bit2、TIM5=bit3）；GPIO 时钟在 `RCC->APB2ENR`（PA=bit2、PB=bit3、PC=bit4、PD=bit5）。
3. **Ft 用 72MHz 不是 36MHz**：APB1 已二分频 ⇒ 定时器时钟 ×2。改主频或改 `PPRE1` 后，所有 ARR/PSC 数值都要重算。
4. **16 位还是 32 位**：TIM2/TIM5 的 CNT/ARR 实际是 32 位，但 `gtim.c` 的形参都是 `uint16_t` 且注释说明"为了通用只按 16 位使用"，所以 8-3 才有 4194303us 的量程上限。
5. **捕获引脚模式**：`gtim_timx_cap_chy_init()`/`gtim_timx_cnt_chy_init()` 用 `SYS_GPIO_MODE_AF` 配 PA0，
   而 [sys.c](<实验8-3 通用定时器输入捕获实验/Drivers/SYSTEM/sys/sys.c>) 的 `sys_gpio_set()` 只在**输入模式**分支里写 ODR 选上下拉，
   所以传进去的 `SYS_GPIO_PUPD_PD` 在复用模式下并不会真的产生下拉（注释"复用功能 下拉"与实际行为不一致）。
   参考手册里捕获输入更常用浮空/上下拉输入（`CNF=01/10, MODE=00`）；若你改用到别的引脚后捕获异常，可以往这里查。
6. **8-3 与 8-4 都占用了 PA0**：PA0 既是 WK_UP 按键又是捕获/计数脚，`key_init()` 先把它配成下拉输入，随后 `gtim_*` 初始化把它改成复用功能。
   因此这两个实验里**不能再指望 KEY_UP 的键值**（8-4 只用 KEY0），也别同时接外部 5V 信号进来。
7. **8-2 的飞线是必须的**，且 PA8 已被程序设为"不驱动"状态（`GPIOA->CRH &= ~0x0F`）：
   注释说"浮空输入"，实际写入的 4 位是 `0000`，按 F103 GPIO 表那是**模拟输入**；两者都不会驱动引脚，本例无碍，但严格来说浮空输入应是 `CNF=01/MODE=00`（4 位 `0x4`）。
8. **官方 `readme.txt` 与代码不一致之处（以代码为准）**：
   - 8-3 的 `gtim.h` 注释写"这里的输入捕获使用定时器TIM2_CH1"，实际宏是 `GTIM_TIMX_CAP = TIM5`（`readme.txt` 写的 TIM5 才对）。
   - 8-3/8-4 的 `GTIM_TIMX_CAP_CHY_GPIO_CLK_ENABLE()` / `GTIM_TIMX_CNT_CHY_GPIO_CLK_ENABLE()` 注释标"PA口时钟使能"，
     代码却写 `RCC->APB1ENR |= 1 << 2` / `|= 1 << 0`（这两个位分别是 **TIM4EN / TIM2EN**，GPIOA 时钟在 APB2ENR bit2）。
     程序仍能跑，是因为 `led_init()`/`key_init()`/`usart_init()` 早已打开了 GPIOA 时钟。
   - 8-3 的 `main.c` 里 `extern uint16_t g_timxchy_cap_val;`，而 `gtim.c` 中定义是 `uint32_t g_timxchy_cap_val`，
     两边类型不一致（靠"捕获值 ≤0xFFFF"侥幸不出错，移植时要留意）。
   - 8-4 的 `readme.txt` 标题写成"通用定时器输入捕获 实验"（复制 8-3 忘改），实际是脉冲计数。
9. **中文乱码**：源码是 GBK 编码，Keil 需 `Edit → Configuration → Editor → Encoding` 选 GB2312/Chinese GBK，
   或在命令行用 `iconv -f GBK -t UTF-8 文件名` 查看；不要把源文件另存为 UTF-8。
10. **串口助手选型**：官方提醒用 XCOM/SSCOM，其它助手可能拉 DTR/RTS 使 MCU 复位或程序不跑。

## 九、总结与延伸

**本实验要点（5 条）：**

1. `Tout = ((ARR+1)*(PSC+1))/Ft`，Ft 对本工程 TIM2~TIM5 是 **72MHz**；PWM 频率同理，占空比 = `CCR/(ARR+1)`（极性看 `CCER.CCxP`）。
2. 一条定时器初始化套路：**开时钟 → 写 PSC/ARR → 配 CCMR（模式）→ 配 CCER（使能/极性）→ 开 DIER 中断 → 置 CR1.CEN → NVIC 使能**。
3. PWM 输出用 `OCxM=110`（模式1）+ `OC1PE`/`ARPE` 预装载，改占空比只需改 `CCR1`。
4. 输入捕获 = "上升沿捕获一次（清 CNT、换极性）+ 下降沿捕获一次（读 CCR）"，长脉冲靠 `EGR.UG`/溢出次数累加，最大约 4.19s。
5. 脉冲计数靠 **从模式**：`SMCR.SMS=111`（外部时钟模式1）+ `SMCR.TS=101`（TI1FP1），计数值 = 溢出次数×65536 + CNT。

**延伸小练习：**

1. 把 8-1 的 `gtim_timx_int_init(5000 - 1, 7200 - 1)` 改成 `(2000 - 1, 7200 - 1)`，绿灯应变成 200ms 一次（和红灯同频）；
   再把 `GTIM_TIMX_INT` 改成 `TIM4`、时钟位改成 `RCC->APB1ENR |= 1 << 2`、中断号/函数名改成 `TIM4_IRQn/TIM4_IRQHandler`，体会宏封装的价值。
2. 8-2 里把 `CCER |= 1 << (1 + 4*(chy-1))`（低电平有效）改成清 0（高电平有效），
   同时把 `ledrpwmval` 的上限改成 500，观察 LED0 从"最亮 60%"变成"最暗 0%/最亮 100%"的方向变化，验证 `1 - CCR/(ARR+1)`。
3. 8-3 改造成**测频率**：把状态机改成"连续两次上升沿捕获"，用 `Δ = 第二次CCR - 第一次CCR + 溢出次数*65536` 算周期，
   打印 `f = 1000000/Δ us`；再用高电平时间 / 周期时间得到占空比（做完你就理解了实验 9-4 的 PWM 输入模式为什么要用两个通道）。
4. 8-4 里把 `GTIM_TIMX_CNT_CHY` 改成 2（`GTIM_TIMX_CNT->SMCR |= 6 << 4` 用 TI2FP2），信号换到 PA1（TIM2_CH2），验证 CH1/CH2 都能计数。

**衔接下一步**：[实验9 高级定时器实验](../实验9%20高级定时器实验) 用 TIM1/TIM8 做"只发 5 个 PWM""4 路不同相位的方波""互补输出 + 死区 + 刹车""PWM 输入模式一次量出周期与占空比"——
全是本实验寄存器的超集，重点是新增的 `BDTR`（MOE/BK/BKP/DTG/LOCK）、`RCR` 重复计数器和 `SMCR` 复位模式。
