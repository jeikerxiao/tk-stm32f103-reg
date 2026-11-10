# 实验9 高级定时器实验

> 一句话概括：用 STM32F103 的高级定时器 TIM1/TIM8 完成四件事——输出指定个数的 PWM、输出比较模式多路错相 PWM、互补输出带死区与刹车、PWM 输入模式自动测频测占空比，理解高级定时器相对通用定时器多出来的那套"电机控制专用"硬件。

本实验目录下有 4 个相互独立的子实验，每个子实验都是一个完整的 Keil 工程，可单独编译下载。

## 一、实验简介

- 实验目的（来自各子实验官方 readme.txt，通俗展开）：
  1. **实验9-1 输出指定个数 PWM**：让 TIM1 精确地只输出 N 个 PWM 脉冲就自动停下，学会用高级定时器独有的重复计数器 RCR。
  2. **实验9-2 输出比较模式**：用 TIM8 的 4 个通道输出占空比都是 50%、但相位彼此错开 25% 的 PWM，理解"比较匹配就翻转电平"这种最简单的输出方式。
  3. **实验9-3 互补输出带死区控制**：TIM1_CH1（PA8）与互补通道 TIM1_CH1N（PB13）输出互补 PWM，中间插入死区时间，并用 BKIN（PB12）实现刹车，这是 H 桥/三相桥等功率驱动的核心技术。
  4. **实验9-4 PWM 输入模式**：让 TIM8 自动测量一路外部 PWM 的周期和高电平脉宽（即频率与占空比），并通过串口打印。
- 学习路线位置：前置实验是通用定时器实验（PWM 输出、输入捕获）；本实验是定时器系列的最后一站，之后可衔接电机控制、数字电源类综合实验。
- 平台：正点原子 MiniSTM32 V4 开发板，主芯片 STM32F103RC（72MHz，寄存器版，不使用标准库/HAL）。

## 二、实验原理

### 2.1 什么是"寄存器版"操作

寄存器就是 CPU 内部/外设里一个个可读写的 32 位控制格子，配置外设 = 往对应格子里按位填数。本文中所有写法都直接取自工程源码，例如 `TIM1->ARR = arr;` 就是把自动重装值写进 TIM1 的 ARR 寄存器。

### 2.2 高级定时器（TIM1/TIM8）与通用定时器（TIM2~TIM5）的差异

STM32F103 里 TIM1、TIM8 被称为高级（控制）定时器，硬件上是为驱动电机/功率桥设计的。相对通用定时器，多了以下东西，全部有对应代码可查：

| 特性 | 寄存器/位 | 在哪几个子实验用到 |
| --- | --- | --- |
| 主输出使能 MOE | `BDTR` 的 bit15 | 9-1/9-2/9-3/9-4（TIM8 输入侧不需要，但输出侧全部要置 1） |
| 重复计数器 RCR | `TIMx->RCR`，0~255 | 9-1（一次输出 N 个 PWM 的关键） |
| 互补输出通道 OCxN | `CCER` 的 CCxNE/CCxNP 位 | 9-3 |
| 死区时间 DTG[7:0] | `BDTR` 的 bit0~7 | 9-3 |
| 刹车输入 BKIN | `BDTR` 的 BKE(bit12)/BKP(bit13) | 9-3 |
| 自动输出使能 AOE | `BDTR` 的 bit14 | 9-3 |
| 从模式控制 SMCR | `SMCR` 的 SMS/TS 位（复位从模式） | 9-4 |
| TI1 双映射（TI1×2） | `CCMR1` 的 CC1S/CC2S + `SMCR` 的 TS | 9-4 |

几点白话解释：

- **MOE（Main Output Enable）**：高级定时器即使一切配置好，只要 MOE=0，引脚上就一个波形都没有——相当于功率输出的"总闸"。通用定时器没有这个闸，这也是很多人把通用定时器的 PWM 代码搬到 TIM1 上"没波形"的头号原因。代码里处处可见 `ATIM_TIMX_NPWM->BDTR |= 1 << 15; /* 使能MOE位, 高级定时器必须设置MOE才能输出PWM */`。
- **RCR（重复计数器）**：通用定时器每计数溢出一次就产生一次更新事件；高级定时器可以设置"溢出 RCR+1 次才产生一次更新事件"。PWM 波形本身每个周期照常输出，只是更新事件被"攒"起来了。9-1 正是用 `RCR = npwm - 1` 精确数出 npwm 个 PWM。
- **互补通道与死区**：CHx 和 CHxN 输出电平永远相反，驱动上管/下管两个开关管。但两个管子切换时若同时导通一瞬间就是直通短路，所以硬件自动在两者之间插入一段"都不导通"的时间，即死区时间，由 `BDTR` 低 8 位 DTG 编码决定。
- **刹车 BKIN**：外部给一个电平（本实验是高电平）就立即封杀所有输出，硬件级保护，不需要 CPU 参与。
- **CR2 的 MMS（主模式选择）**：TIM1/TIM8 还可以通过 `CR2` 的 MMS 位把自身的 TRGO 信号输出给其它定时器做同步触发（主从联动）。本次 4 个子实验没有用到该功能，代码中未写 MMS，仅作为知识点提出。
- **时钟域**：TIM1/TIM8 挂在 APB2，时钟 72MHz（`RCC->APB2ENR` bit11 使能 TIM1、bit13 使能 TIM8）；TIM2~TIM5 挂在 APB1（PCLK1=36MHz 但定时器倍频×2，仍是 72MHz），TIM2 由 `RCC->APB1ENR` bit0 使能。

### 2.3 参数计算（以工程实际值代入）

定时器溢出时间公式（代码注释原话）：`Tout = ((arr + 1) * (psc + 1)) / Ft`，单位 us，Ft 为定时器工作频率（MHz，本工程均为 72）。

- 9-1：`atim_timx_npwm_chy_init(5000 - 1, 7200 - 1)`，计数频率 = 72MHz / 7200 = 10kHz，PWM 周期 = 5000 / 10kHz = 0.5s，即 2Hz 的 PWM；占空比由 CCR1=2500 设定为 50%。
- 9-2：`atim_timx_comp_pwm_init(1000 - 1, 72 - 1)`，计数频率 = 1MHz；比较匹配翻转模式下每 2 个计数周期完成一个完整 PWM（代码注释：输出频率减半），故 PWM 频率 = 1MHz / (1000×2) = 500Hz，占空比恒 50%。
- 9-3：`atim_timx_cplm_pwm_init(1000 - 1, 72 - 1)`，PWM 频率 = 1MHz / 1000 = 1kHz；`CKD[1:0]=10` 使 tDTS = 4 × tCK_INT = 4/72MHz ≈ 55.56ns；`atim_timx_cplm_pwm_set(300, 100)` 中 dtg=100（二进制 0110 0100，bit7=0），查代码注释的第一种编码：死区时间 = dtg[7:0] × tDTS = 100 × 55.56ns ≈ 5.56us。
- 9-4：TIM8 输入侧 `PSC=0、ARR=65535`，计数时钟 72MHz，每计一个数约 13.9ns；TIM2 输出侧 `gtim_timx_pwm_chy_init(10 - 1, 72 - 1)`，1MHz 计数、周期 10us（100kHz PWM），CCR1=2 且低电平有效，所以 PA0 上约为 20% 低 + 80% 高的 PWM。

## 三、硬件资源与接线

### 3.1 实验9-1 高级定时器输出指定个数PWM实验

| 资源 | 引脚 | 代码中的宏/位置 | 说明 |
| --- | --- | --- | --- |
| TIM1_CH1（PWM 输出） | PA8 | `ATIM_TIMX_NPWM_CHY_GPIO_PORT/PIN`，[atim.h](<实验9-1 高级定时器输出指定个数PWM实验/Drivers/BSP/TIMER/atim.h>) | 复用推挽输出，同时接 LED0 |
| LED0（红灯） | PA8 | `LED0_GPIO_PORT/PIN`，[led.h](<实验9-1 高级定时器输出指定个数PWM实验/Drivers/BSP/LED/led.h>) | 低电平点亮，随 PWM 亮灭 |
| LED1（绿灯） | PD2 | `LED1_GPIO_PORT/PIN`，led.h | 指示程序运行 |
| KEY0 | PC5 | `KEY0_GPIO_PORT/PIN`，[key.h](<实验9-1 高级定时器输出指定个数PWM实验/Drivers/BSP/KEY/key.h>) | 按下触发输出 5 个 PWM |
| KEY1 / WK_UP | PA15 / PA0 | `KEY1_GPIO_*` / `WKUP_GPIO_*`，key.h | 本工程只用 KEY0 |
| TIM1 全局中断 | — | `ATIM_TIMX_NPWM_IRQn = TIM1_UP_IRQn` | 更新中断里改写 RCR |

无需外接杜邦线，上电即用。

### 3.2 实验9-2 高级定时器输出比较模式实验

| 资源 | 引脚 | 代码中的宏/位置 | 说明 |
| --- | --- | --- | --- |
| TIM8_CH1 | PC6 | `ATIM_TIMX_COMP_CH1_GPIO_PORT/PIN`，[atim.h](<实验9-2 高级定时器输出比较模式实验/Drivers/BSP/TIMER/atim.h>) | 相位 25% |
| TIM8_CH2 | PC7 | `ATIM_TIMX_COMP_CH2_*` | 相位 50% |
| TIM8_CH3 | PC8 | `ATIM_TIMX_COMP_CH3_*` | 相位 75% |
| TIM8_CH4 | PC9 | `ATIM_TIMX_COMP_CH4_*` | 相位 100% |
| LED0 | PA8 | led.h | 闪烁指示程序运行 |

观察波形需示波器，探头接 PC6~PC9（可经板载排针引出）。注意官方 readme 提醒：这几个引脚与 LCD 部分控制引脚共用，本实验不要插着 LCD 跑。

### 3.3 实验9-3 高级定时器互补输出带死区控制实验

| 资源 | 引脚 | 代码中的宏/位置 | 说明 |
| --- | --- | --- | --- |
| TIM1_CH1（正常输出） | PA8 | `ATIM_TIMX_CPLM_CHY_GPIO_PORT/PIN`，[atim.h](<实验9-3 高级定时器互补输出带死区控制实验/Drivers/BSP/TIMER/atim.h>) | 输出 70% 占空比 PWM |
| TIM1_CH1N（互补输出） | PB13 | `ATIM_TIMX_CPLM_CHYN_GPIO_PORT/PIN` | 输出 30% 占空比 PWM |
| TIM1_BKIN（刹车输入） | PB12 | `ATIM_TIMX_CPLM_BKIN_GPIO_PORT/PIN` | 高电平刹车 |
| LED1 | PD2 | led.h | 闪烁指示程序运行 |

测试刹车：用杜邦线把 PB12 接到 3.3V（或接按键另一端拉高），PA8/PB13 波形立即消失；撤掉后自动恢复。

### 3.4 实验9-4 高级定时器PWM输入模式实验

| 资源 | 引脚 | 代码中的宏/位置 | 说明 |
| --- | --- | --- | --- |
| TIM2_CH1（PWM 输出源） | PA0 | `GTIM_TIMX_PWM = TIM2`，[gtim.h](<实验9-4 高级定时器PWM输入模式实验/Drivers/BSP/TIMER/gtim.h>) | 输出约 100kHz、高 80% |
| TIM8_CH1（PWM 输入测量） | PC6 | `ATIM_TIMX_PWMIN_CHY_GPIO_PORT/PIN`，[atim.h](<实验9-4 高级定时器PWM输入模式实验/Drivers/BSP/TIMER/atim.h>) | 复用功能下拉输入 |
| 串口1 | PA9(TX)/PA10(RX) | [usart.h](<实验9-4 高级定时器PWM输入模式实验/Drivers/SYSTEM/usart/usart.h>) | 115200 打印测量结果 |
| LED0 | PA8 | led.h | 每 200ms 闪烁一次 |

**必须用一根杜邦线把 PA0 连到 PC6**，否则 TIM8 采不到信号。串口需 USB 线接板载 USB_UART，并把 USART1 排针上 PA9/PA10 用跳线帽分别接 RXD/TXD。

### 3.5 时钟说明

4 个子实验的 main() 第一句都是 `sys_stm32_clock_init(9)`：8MHz 外部晶振经 PLL 倍频 9 倍得 72MHz 系统时钟；APB2=72MHz 不分频，所以 TIM1/TIM8 时钟就是 72MHz。这也是所有 ARR/PSC 计算的基础。

## 四、代码结构与调用流程

### 4.1 目录树（4 个子实验结构相同，以 9-1 为例）

```text
实验9-1 高级定时器输出指定个数PWM实验/
├── Drivers/
│   ├── BSP/
│   │   ├── LED/led.c|led.h          LED0/LED1 初始化与翻转宏
│   │   ├── KEY/key.c|key.h          （仅 9-1）按键初始化与扫描
│   │   └── TIMER/
│   │       ├── atim.c|atim.h        高级定时器驱动（本实验主角）
│   │       ├── gtim.c|gtim.h        通用定时器驱动（9-4 用它产生 PWM 源）
│   │       └── btim.c|btim.h        基本定时器驱动（本实验未用到）
│   ├── SYSTEM/sys|usart|delay/      时钟、串口、延时
│   └── CMSIS/                       芯片寄存器定义（TIM1/TIM8 结构体等）
├── User/main.c                      主程序
├── Projects/MDK-ARM/atk_f103.uvprojx  Keil 工程
└── readme.txt                       官方说明（GBK 编码）
```

各子实验 atim.c 是"增量式"的：9-1 只含 NPWM 函数；9-2 新增 `atim_timx_comp_pwm_init`；9-3 再新增 `atim_timx_cplm_pwm_init/_set`；9-4 再新增 `atim_timx_pwmin_chy_init/_process/_restart`。每个工程只需关注自己那份。

### 4.2 main() 流程（编号步骤）

- 9-1（[main.c](<实验9-1 高级定时器输出指定个数PWM实验/User/main.c>)）：时钟 72M → delay/串口/LED/KEY 初始化 → `atim_timx_npwm_chy_init(5000 - 1, 7200 - 1)`（PWM 2Hz）→ 写 `ATIM_TIMX_NPWM_CHY_CCRX = 2500`（50% 占空比）→ `atim_timx_npwm_chy_set(5)` 先输出 5 个 PWM → 循环扫描按键，KEY0 按下再输出 5 个；约每 500ms 翻转 LED1。
- 9-2（[main.c](<实验9-2 高级定时器输出比较模式实验/User/main.c>)）：时钟 → delay/串口/LED → `atim_timx_comp_pwm_init(1000 - 1, 72 - 1)` → 依次写 4 个 CCRx（250-1/500-1/750-1/1000-1）设定相位 → while 循环里每 200ms 翻转 LED0，此后 PWM 完全由硬件自主输出，CPU 什么都不干。
- 9-3（[main.c](<实验9-3 高级定时器互补输出带死区控制实验/User/main.c>)）：时钟 → delay/串口/LED → `atim_timx_cplm_pwm_init(1000 - 1, 72 - 1)` → `atim_timx_cplm_pwm_set(300, 100)` 设 CCR 与死区 → 循环每 200ms 翻转 LED1。
- 9-4（[main.c](<实验9-4 高级定时器PWM输入模式实验/User/main.c>)）：时钟 → delay/串口/LED → `gtim_timx_pwm_chy_init(10 - 1, 72 - 1)`（TIM2 输出 100kHz 源）→ `atim_timx_pwmin_chy_init()`（TIM8 捕获）→ `GTIM_TIMX_PWM_CHY_CCRX = 2`（高 80%）→ 每 200ms 检查一次捕获标志 `g_timxchy_pwmin_sta`，有数据就 printf 打印并 `atim_timx_pwmin_chy_restart()` 重新检测，同时翻转 LED0。

## 五、核心代码解析

### 5.1 实验9-1：用 RCR 精确输出 N 个 PWM

思路：PWM 每个周期照常跑，但把更新事件"攒"起来——设 RCR = N-1，则恰好 N 个 PWM 周期后才触发一次更新中断，在中断里关掉定时器即可。一次最多 256 个（RCR 是 8 位），超过就分批，`g_npwm_remain` 记录剩余脉冲数。中断服务函数核心（摘自 [atim.c](<实验9-1 高级定时器输出指定个数PWM实验/Drivers/BSP/TIMER/atim.c>)，注释为原文）：

```c
if (g_npwm_remain > 256)        /* 还有大于256个脉冲需要发送 */
{
    g_npwm_remain = g_npwm_remain - 256;
    npwm = 256;
}
else if (g_npwm_remain % 256)   /* 还有位数（不到256）个脉冲要发送 */
{
    npwm = g_npwm_remain % 256;
    g_npwm_remain = 0;          /* 没有脉冲了 */
}

if (npwm)   /* 有脉冲要发送 */
{
    ATIM_TIMX_NPWM->RCR = npwm - 1; /* 设置重复计数寄存器值为npwm-1, 即npwm个脉冲 */
    ATIM_TIMX_NPWM->EGR |= 1 << 0;  /* 产生一次更新事件,以更新RCR寄存器 */
    ATIM_TIMX_NPWM->CR1 |= 1 << 0;  /* 使能定时器TIMX */
}
else
{
    ATIM_TIMX_NPWM->CR1 &= ~(1 << 0);   /* 关闭定时器TIMX */
}
```

逐段解释：

- `EGR |= 1 << 0`：软件置位 UG 位强制产生一次更新事件。RCR 带影子寄存器，写进去要等更新事件才生效，这里手动"推"它生效。
- `RCR = npwm - 1`：溢出 npwm 次才中断一次，即精确放行 npwm 个 PWM。
- `npwm == 0` 分支：脉冲发完，CEN 清零停表，PA8 从此静止。
- 初始化里必须有的两句：`ATIM_TIMX_NPWM->BDTR |= 1 << 15;`（MOE 总闸）和 `ATIM_TIMX_NPWM->DIER |= 1 << 0;`（更新中断使能），并由 `sys_nvic_init(1, 3, ATIM_TIMX_NPWM_IRQn, 2)` 打开 TIM1 更新中断。
- PWM 模式 1 的寄存器写法：`CCMR1 |= 6 << (4 + 8 * (chy - 1));` 即 OC1M=110；`CCER |= 1 << (4 * (chy - 1));` 打开 CC1E 输出使能。chy 来自宏 `ATIM_TIMX_NPWM_CHY`（=1），改宏即可换通道。

### 5.2 实验9-2：输出比较"匹配翻转"模式

PWM 模式靠"计数值 < CCR 输出有效"产生占空比；输出比较模式（OCxM=011）更简单：计数器数到与 CCR 相等那一瞬间，输出电平翻转一次，别的什么都不做。所以占空比恒 50%，CCR 只决定"第一次翻转发生在何时"，也就是相位。摘自 [atim.c](<实验9-2 高级定时器输出比较模式实验/Drivers/BSP/TIMER/atim.c>)：

```c
ATIM_TIMX_COMP->CCMR1 |= 3 << 4;    /* 通道1 输出比较模式 匹配时翻转 */
ATIM_TIMX_COMP->CCMR1 |= 1 << 3;    /* 通道1 预装载使能 */
...
ATIM_TIMX_COMP->CCER |= 1 << 0;     /* OC1 输出使能 */ 
ATIM_TIMX_COMP->CCER |= 1 << 4;     /* OC2 输出使能 */ 
ATIM_TIMX_COMP->CCER |= 1 << 8;     /* OC3 输出使能 */ 
ATIM_TIMX_COMP->CCER |= 1 << 12;    /* OC4 输出使能 */ 
ATIM_TIMX_COMP->BDTR |= 1 << 15;    /* 使能MOE位, 高级定时器必须设置MOE才能输出PWM */
```

- `3 << 4`：CCMR1 的 bit6:4（OC1M）=011，即翻转模式；通道 2 同理是 `3 << 12`，通道 3/4 在 CCMR2。
- main.c 里 `ATIM_TIMX_COMP_CH1_CCRX = 250 - 1`（整周期 1000 计数），表示通道 1 在周期约 25% 处才第一次翻转；4 路写不同 CCRx，得到相位 25%、50%、75%、100% 的四路 50% 占空比方波，示波器上看像"接力"一样错开。
- 代码注释点出关键坑："本例程输出比较模式，每 2 个计数周期才能完成一个 PWM 输出，因此输出频率减半"——一次翻转只产生半个周期，所以 1MHz 计数、ARR=999 得到的是 500Hz 而不是 1kHz。

### 5.3 实验9-3：互补输出 + 死区 + 刹车

摘自 [atim.c](<实验9-3 高级定时器互补输出带死区控制实验/Drivers/BSP/TIMER/atim.c>) 的 `atim_timx_cplm_pwm_init`：

```c
ATIM_TIMX_CPLM->CCER |= 1 << (4 * (chy - 1));       /* OCy 输出使能 */
ATIM_TIMX_CPLM->CCER |= 1 << (1 + 4 * (chy - 1));   /* OCy 低电平有效 */
ATIM_TIMX_CPLM->CCER |= 1 << (2 + 4 * (chy - 1));   /* OCyN 输出使能 */
ATIM_TIMX_CPLM->CCER |= 1 << (3 + 4 * (chy - 1));   /* OCyN 低电平有效 */

ATIM_TIMX_CPLM->BDTR |= 1 << 13;    /* BKP = 1, BKIN高电平有效 */
__nop();                            /* 设置完BKP位, 必须等待1个APB周期, 否则紧接着再执行BDTR某些位操作, 会导致写入失效 */
__nop();
__nop();
ATIM_TIMX_CPLM->BDTR |= 1 << 12;    /* BKE = 1, 使能BKIN检测 */
ATIM_TIMX_CPLM->CR1  |= 2 << 8;     /* CKD[1:0] = 10, tDTS = 4 * tCK_INT = Ft / 4 = 18Mhz*/
ATIM_TIMX_CPLM->BDTR |= 1 << 14;    /* AOE=1,设置MOE能被更新事件置位... */
```

- chy=1 时四句 CCER 等效把 bit3:0 全置 1：CC1E（输出使能）、CC1P=1（CH1 低电平有效）、CC1NE（互补输出使能）、CC1NP=1（CH1N 低电平有效）。内部参考信号 OC1REF 在 PWM 模式 1、CCR=300 时是 30% 高电平；经两次反相后 PA8=70% 高、PB13=30% 高，恰好互补——这就是 readme 说的 7:3。
- `__nop();` 三连是寄存器版的精髓坑：写完 BDTR 的 BKP 位必须等至少 1 个 APB 周期再写 BDTR 其它位，否则后一次写入可能失效，代码用空指令补齐等待。
- 刹车链路：BKE=1 使能 BKIN 检测，BKP=1 表示高电平有效。PB12 一给高，OC 输出立即被硬件封杀；撤销后因为 AOE=1（更新事件自动重新置位 MOE），波形自动恢复，无需重启。
- `atim_timx_cplm_pwm_set(300, 100)` 内部先清 `BDTR` 低 8 位再写 `dtg << 0`（DTG），最后 `BDTR |= 1 << 15` 打开 MOE——顺序别调换，死区设好再开输出才安全。

### 5.4 实验9-4：PWM 输入模式（硬件自动测频测占空比）

PWM 输入模式是"双通道输入捕获 + 从模式复位"的组合拳：把同一个 TI1 信号同时接到 IC1（捕上升沿）和 IC2（捕下降沿），并把上升沿配置为"计数器复位"触发源。于是每个上升沿计数器清零重跑：CCR1 数到的是整周期，CCR2 数到的是高电平宽度，一次捕获同时得到频率与占空比。摘自 [atim.c](<实验9-4 高级定时器PWM输入模式实验/Drivers/BSP/TIMER/atim.c>)：

```c
ATIM_TIMX_PWMIN->CCMR1 |= 1 << 0;   /* CC1S[1:0] = 01  选择输入端 IC1映射到TI1上 */
ATIM_TIMX_PWMIN->CCMR1 |= 2 << 8;   /* CC2S[1:0] = 10  选择输入端 IC2映射到TI1上 */
ATIM_TIMX_PWMIN->CCER |= 0 << 1;    /* CC1P      = 0   CC1NP默认是0, CCR1捕获上升沿 */
ATIM_TIMX_PWMIN->CCER |= 1 << 5;    /* CC2P      = 1   CC2NP默认是0, CCR2捕获下降沿 */
ATIM_TIMX_PWMIN->SMCR |= 5 << 4;    /* TS[4:0]   = 101 触发选择TI1FP1 */
...
ATIM_TIMX_PWMIN->SMCR |= 4 << 0;    /* SMS[2:0]  = 100 复位模式 */
```

- "CC2S=10 把 IC2 也映射到 TI1"就是所谓 TI1×2 内部连线：一个输入引脚两路捕获通道共用（寄存器名是 SMCR，即 Slave Mode Control Register，从模式控制寄存器）。
- TS=101 选 TI1FP1（上升沿那路）作触发输入，SMS=100 从模式=复位：上升沿一到 CNT 清零。捕获处理后 `g_timxchy_pwmin_hval = CCR2 + 1;`（高电平脉宽）、`g_timxchy_pwmin_cval = CCR1 + 1;`（周期）。
- 自适应分频：ARR 固定 65535，被测信号周期若超过 65536 个计数就只溢出、不捕获。`atim_timx_pwmin_chy_process` 检测到溢出但无周期捕获标志时，把 `g_timxchy_pwmin_psc` 倍增写回 `PSC`，自动降速直到捕到为止；main.c 打印时用 `tpsc = ((double)g_timxchy_pwmin_psc + 1) / 72;` 把计数值换算成微秒。
- TIM1/TIM8 的捕获中断（`TIM8_CC_IRQHandler`）与更新中断（`TIM8_UP_IRQHandler`）是分开的两个服务函数，代码把两者都转发给同一个 `atim_timx_pwmin_chy_process`——这是高级定时器与通用定时器（单一 TIMx_IRQn 入口）在驱动写法上的明显差异。

## 六、编译、下载与运行

1. 用 Keil MDK 打开对应子实验的工程（4 个子实验各自独立）：
   - `<实验9-1 高级定时器输出指定个数PWM实验/Projects/MDK-ARM/atk_f103.uvprojx>`
   - `<实验9-2 高级定时器输出比较模式实验/Projects/MDK-ARM/atk_f103.uvprojx>`
   - `<实验9-3 高级定时器互补输出带死区控制实验/Projects/MDK-ARM/atk_f103.uvprojx>`
   - `<实验9-4 高级定时器PWM输入模式实验/Projects/MDK-ARM/atk_f103.uvprojx>`
2. 工程器件为 STM32F103RC，包含启动文件与 CMSIS 头文件；Include Path 已配好（指向 Drivers、User 等），直接 F7 编译即可，生成物在各自的 Output 目录下。
3. 下载：ST-Link/J-Link 在线下载，或串口 ISP 下载均可；一次只打开一个子实验工程，避免把 9-1 的固件烧进去又跑 9-3 的现象。
4. 上位机：只有 9-4 需要串口助手，波特率 115200；官方 readme 建议用 XCOM/SSCOM，其它串口助手可能因 DTR/RTS 拉线导致 MCU 复位。9-2/9-3 建议准备示波器。

## 七、实验现象

- **9-1**：上电后 LED0（PA8 上的红灯）先亮灭 5 次（每个 PWM 周期 0.5s，亮灭一次即一个 PWM 波）然后停住；按一下 KEY0，再亮灭 5 次。LED1 约 0.5s 一次慢闪，表示程序活着。
- **9-2**：示波器四路探头分别接 PC6~PC9，看到 4 路 500Hz、50% 占空比的方波，彼此相位依次错开约 25%（即 500us）；LED0 以 500ms 周期闪烁。
- **9-3**：PA8 输出 1kHz、70% 占空比 PWM，PB13 输出互补的 30% 占空比 PWM，放大波形可看到两者切换间隙约 5.56us 的死区；把 PB12 接到高电平，两路波形同时消失，断开后自动恢复；LED1 闪烁。
- **9-4**：PA0-PC6 杜邦线接好后，串口每 200ms 打印一组：`PWM PSC`（分频系数，正常为 0）、`PWM Hight`（高电平计数值）、`PWM Cycle`（周期计数值），以及换算出的 `Hight time≈8us、Cycle time≈10us、Frequency≈100kHz`；LED0 每 200ms 闪一次。拔掉杜邦线则打印的 `PWM PSC` 会一路翻倍到 65535（代码里的"无输入"自适应）。

## 八、注意事项

1. **MOE 是高级定时器的第一坑**：忘了置 `BDTR` bit15，引脚上绝无波形。本实验所有输出初始化都写了这一位，自己改代码时别删。
2. **BDTR 写后等待**：9-3 中写完 BKP 位后的三个 `__nop()` 不是摆设（STM32F1 硬件勘误），改代码时若连续写 BDTR 多个位，务必保留等待。
3. **9-1 脉冲个数别取 256 的整数倍**：中断里用 `g_npwm_remain % 256` 判断收尾批次，当剩余恰为 256/512 等整数倍时该表达式为 0，会直接走"关闭定时器"分支，导致这批脉冲发不完。用 1~255 或非 256 整数倍的个数最稳。
4. **9-2 的相位与频率**：翻转模式下一个匹配只出半个周期，实测频率是"1MHz/1000"的一半（500Hz），不是 bug；官方 readme 与代码在这点上一致，atim.h 宏注释"默认是针对TIM1"则与代码不符——`ATIM_TIMX_COMP` 实际定义为 TIM8（readme.txt 列的也是 TIM8 引脚），以代码为准。
5. **9-2 引脚冲突**：PC6~PC9 与 LCD 部分控制引脚共用，跑本实验别插 LCD 模块。
6. **9-3 刹车测试**：给 PB12 的高电平不要顺手接到 5V 以外的功率回路；验证完记得撤线，否则"怎么没波形"多半是刹车还踩着。
7. **9-4 注释与代码不一致**：atim.c 函数注释写"采样时钟频率为240Mhz，精度4.167ns"，但 F103 上 TIM8 计数时钟为 72MHz，且 main.c 的换算 `tpsc = (psc + 1) / 72` 也按 72MHz 计算，请以代码为准（约 13.9ns 精度）。atim.h 中 PWM 输入宏的注释"捕获WK_UP按键的输入"同样是复制残留，实际捕获的是 PC6（TIM8_CH1）。
8. **9-4 接线与串口**：PA0→PC6 杜邦线必接；USB 线要插 USB_UART 口，USART1 排针上 PA9→RXD、PA10→TXD 跳线帽要套好，波特率 115200。
9. **编码**：所有源文件与 readme.txt 均为 GBK 编码，用 UTF-8 打开中文注释会乱码，编辑器里请选择 GBK(GB2312) 重新打开，不要直接另存为 UTF-8。

## 九、总结与延伸

要点：

1. 高级定时器 = 通用定时器 + MOE 总闸 + RCR 重复计数 + 互补输出/死区/刹车 + 主从同步（CR2.MMS）；前四样在本实验全部有真实寄存器操作。
2. "输出 N 个 PWM"不靠软件计数，靠 RCR 把更新事件延后 N 个周期，硬件级精确。
3. 输出比较翻转模式天然 50% 占空比，CCR 调相位不调占空比；要多相位同步输出（如三相逆变）就用它。
4. 死区时间由 DTG[7:0] 编码、以 tDTS 为单位（本实验 CKD=10，tDTS≈55.56ns），互补输出保护功率管的机理要记牢。
5. PWM 输入模式 = TI1×2 双映射 + 复位从模式，一次捕获同时得周期与高电平宽度；工程上还做了溢出自动扩分频，能测很低的频率。

延伸练习（都在现有代码上改几行就能做）：

- 9-1：把 `atim_timx_npwm_chy_init(5000 - 1, 7200 - 1)` 的 arr 改小，观察 LED0 亮灭节奏变快，算一下新的 PWM 频率。
- 9-2：交换 `ATIM_TIMX_COMP_CH2_CCRX` 与 `CH3_CCRX` 的值，示波器上看两路相位是否跟着互换。
- 9-3：把 `atim_timx_cplm_pwm_set(300, 100)` 的 dtg 改成 0，对比死区消失后两路波形是否有重叠导通区间；再把 `ATIM_TIMX_CPLM_CHY_CCRY` 改为 500 看 50/50 互补。
- 9-4：把 9-1 工程的 PA8（2Hz PWM）用杜邦线接到 PC6，验证低频下 `PWM PSC` 自动倍增的现象。

衔接：至此定时器系列（基本/通用/高级）完成。后续做电机驱动、数字电源类项目时，9-3 的互补+死区+刹车就是标准模板；做信号测量类项目时，9-4 的 PWM 输入比逐次中断算脉宽的写法效率高得多。


