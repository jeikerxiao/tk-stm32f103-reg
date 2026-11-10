# 实验7 基本定时器实验

> 一句话概括：用片内的基本定时器 TIM6 每隔 500ms 自动产生一次中断，在中断里翻转绿灯——学会"PSC/ARR 定时 + 更新中断"这套 STM32 最常用、也最基础的定时器骨架。

本实验只有一个子实验：**实验7-1 基本定时器中断实验**。该子实验目录自带完整的 `User`、`Drivers`、`Projects`，父目录只放本说明。
所有代码都在 `实验7-1 基本定时器中断实验/` 里，下文链接与路径均相对该子目录。

## 一、实验简介

- **实验目的**（官方 `readme.txt`）：学习基本定时器中断的使用。
  通俗讲：定时器就是芯片里的"自动秒表 + 闹钟"。你设好"数满多少下、每下间隔多久"，它自己从 0 往上数，
  数满（溢出）就"按闹钟"——触发一次中断。你只管在中断里写"要做的事"，不用在主循环里傻傻轮询计时。
- **在学习路线中的位置**：前置实验是「实验1 LED」「按键/外部中断实验」（已会 GPIO 与 NVIC 中断）；
  后续「输入捕获/输出比较/PWM」等通用定时器实验都建立在这套 PSC/ARR + 更新中断的模型上。
- **开发板与芯片**：正点原子 MiniSTM32 V4，主芯片 STM32F103RC。**寄存器版**，直接写 `TIM6->ARR/PSC/DIER/CR1/SR`。

> 小概念：**基本定时器**（TIM6、TIM7）是 STM32 里最简的定时器，只能"定时/计数 + 溢出更新"，
> 没有对外输出 PWM、也没有输入捕获——所以特别适合用来练"定时打断程序"这件事。

## 二、实验原理

### 2.1 定时器怎么"数数"（PSC 预分频 + ARR 自动重装）

把定时器想成两级水管：

1. **预分频器 PSC**：先把 72MHz 主时钟"每 (PSC+1) 个才放行 1 个"，得到一个慢一点的"计数脉冲"。
2. **计数器 CNT**：从 0 开始，每来一个计数脉冲加 1。
3. **自动重装值 ARR**：CNT 数到等于 ARR 时，产生一次"更新事件"，CNT 清零重来，同时可触发中断。

所以"数满一圈"要的时间 = (ARR+1) 个计数脉冲，而每个计数脉冲又要 (PSC+1) 个主时钟。

### 2.2 溢出时间公式并代入本工程数值

```text
Tout = ((ARR + 1) * (PSC + 1)) / Ft      单位: us   （Ft 为定时器时钟，单位 MHz）
```

TIM6 挂在 APB1 上。本工程 APB1(PCLK1)=36MHz，但因为 APB1 分频系数 ≥2，
**定时器时钟会自动 ×2**（见 [btim.c](<实验7-1 基本定时器中断实验/Drivers/BSP/TIMER/btim.c>) 注释），所以 **Ft = 72MHz**。

调用是 `btim_timx_int_init(5000 - 1, 7200 - 1)`，即 `ARR = 4999`、`PSC = 7199`：

- 计数脉冲频率 = Ft / (PSC+1) = 72MHz / 7200 = **10 kHz**（即每 0.1ms 数一下，对应注释"10Khz 的计数频率"）。
- 数满一圈 = (ARR+1) 个脉冲 = 5000 个数 → 5000 / 10000 = **0.5s**。
- 套公式：Tout = (5000 * 7200) / 72 us = 36,000,000 / 72 us = 500,000 us = **500 ms**。

所以 TIM6 每 500ms 触发一次更新中断。

### 2.3 "更新事件"与软件更新 UG

只要 CNT 数到 ARR、或计数器被重装，就叫发生了一次 **更新事件（Update Event）**，硬件会把状态寄存器 `TIM6->SR` 的 **UIF（bit0，更新中断标志）** 置 1。

除了自然溢出，也可以**手动制造一次更新事件**：把事件生成寄存器 `TIM6->EGR` 的 **UG 位（bit0）** 写 1，
硬件立刻产生一次更新，从而把预分频缓冲值、重装值"加载生效"（并置位 UIF）。
> 提醒：`UG`（软件更新）位在 **`TIMx->EGR`（事件生成寄存器）**，不是 `TIMx->CR2`；
> `CR2` 在这里主要用于主模式触发选择（MMS 位）。本工程用不到 UG——
> 因为初始化时是先写好 `ARR/PSC` 再使能定时器（`CR1.CEN=1`），首次更新自然会把缓冲值装载，无需手动 UG。
> 相关位名 `TIM_EGR_UG`(bit0) 在工程 CMSIS 头 [stm32f103xe.h](<实验7-1 基本定时器中断实验/Drivers/CMSIS/Device/ST/STM32F1xx/Include/stm32f103xe.h>) 中确实存在。

### 2.4 DIER / SR 的中断流程（本实验核心链路）

一次 500ms 中断从产生到被处理，走这条链：

```text
CNT 数到 ARR  →  更新事件  →  SR.UIF(bit0)=1
        →  因 DIER.UIE(bit0)=1 允许  →  向 NVIC 申请 TIM6_DAC 中断
        →  CPU 跳到 TIM6_DAC_IRQHandler  →  判断 SR.UIF、做事(翻 LED1)  →  清 SR.UIF
```

涉及的寄存器位一览：

| 寄存器 | 位 | 宏名（CMSIS） | 本实验 |
| --- | --- | --- | --- |
| `TIM6->PSC`  | — | — | 写 7199，得 10kHz 计数频率 |
| `TIM6->ARR`  | — | — | 写 4999，数满 5000 下 = 500ms |
| `TIM6->DIER` | bit0 | `TIM_DIER_UIE` | 置 1：允许更新中断 |
| `TIM6->CR1`  | bit0 | `TIM_CR1_CEN`  | 置 1：启动计数器 |
| `TIM6->SR`   | bit0 | `TIM_SR_UIF`   | 溢出置 1；中断里写 0 清除 |

中断号用 `TIM6_DAC_IRQn`（=54），由 `sys_nvic_init(1, 3, TIM6_DAC_IRQn, 2)` 使能（抢占 1、子优先级 3、分组 2）。

## 三、硬件资源与接线

### 3.1 实验7-1 基本定时器中断实验

| 资源 | 引脚 | 代码中的宏/位置 | 说明 |
| --- | --- | --- | --- |
| LED0（红灯） | PA8 | `LED0_GPIO_PORT/PIN`，[led.h](<实验7-1 基本定时器中断实验/Drivers/BSP/LED/led.h>) | 主循环每 200ms 翻转一次 |
| LED1（绿灯） | PD2 | `LED1_GPIO_PORT/PIN`，[led.h](<实验7-1 基本定时器中断实验/Drivers/BSP/LED/led.h>) | TIM6 中断里翻转，指示定时中断 |
| TIM6 | 无引脚 | [btim.h](<实验7-1 基本定时器中断实验/Drivers/BSP/TIMER/btim.h>) / [btim.c](<实验7-1 基本定时器中断实验/Drivers/BSP/TIMER/btim.c>) | 片内基本定时器，纯定时，不接外部 |

- **接线/跳线**：全部板载，**不接任何外部模块**；TIM6 是片内定时器，没有对外引脚。
- **时钟树含义**：`sys_stm32_clock_init(9)` = HSE 8MHz×9 → 系统 72MHz；APB1=36MHz，定时器时钟再 ×2 回到 72MHz。
  正是这个 72MHz 决定了 PSC/ARR 算出的 500ms。

## 四、代码结构与调用流程

```text
实验7 基本定时器实验/
└── 实验7-1 基本定时器中断实验/
    ├── User/main.c                     主程序：初始化 + 主循环翻转 LED0(200ms)
    ├── Drivers/BSP/TIMER/btim.c/.h     TIM6 定时中断初始化 + 中断服务函数
    ├── Drivers/BSP/LED/led.c/.h        LED 驱动：led_init、LED0/LED1_TOGGLE 宏
    └── Drivers/SYSTEM/{sys,delay,usart} 时钟/延时/串口公共底层
```

各文件职责：`btim.h` 用一组宏把"用哪个定时器、哪个中断号、开哪个时钟位"集中定义；`btim.c` 完成 TIM6 配置并写中断服务函数；
`led.c` 提供 LED 宏；`SYSTEM` 提供时钟、SysTick 延时、串口。

**`main()` 流程**（见 [User/main.c](<实验7-1 基本定时器中断实验/User/main.c>)）：

1. `sys_stm32_clock_init(9)`：72MHz。
2. `delay_init(72)` / `usart_init(72,115200)` / `led_init()`：初始化。
3. `btim_timx_int_init(5000-1, 7200-1)`：配置 TIM6，500ms 一次更新中断。
4. `while(1)`：`LED0_TOGGLE(); delay_ms(200);` 主循环每 200ms 翻转红灯；绿灯由中断每 500ms 翻转。

两条线并行：`主循环(200ms 翻 LED0)` 与 `TIM6 中断(500ms 翻 LED1)`，靠不同周期体现"定时中断在独立工作"。

## 五、核心代码解析

### 5.1 实验7-1：把 TIM6/中断号/时钟集中成宏

摘自 [btim.h](<实验7-1 基本定时器中断实验/Drivers/BSP/TIMER/btim.h>)：

```c
#define BTIM_TIMX_INT                       TIM6
#define BTIM_TIMX_INT_IRQn                  TIM6_DAC_IRQn
#define BTIM_TIMX_INT_IRQHandler            TIM6_DAC_IRQHandler
#define BTIM_TIMX_INT_CLK_ENABLE()          do{ RCC->APB1ENR |= 1 << 4; }while(0)   /* TIM6 时钟使能 */
```

好处：驱动代码里全用 `BTIM_TIMX_INT` 这些别名，将来想换 TIM7 只需改这 4 行。bit4 就是 `RCC->APB1ENR` 里的 TIM6EN。

### 5.2 实验7-1：初始化 TIM6 定时中断 —— `btim_timx_int_init`

摘自 [btim.c](<实验7-1 基本定时器中断实验/Drivers/BSP/TIMER/btim.c>)：

```c
void btim_timx_int_init(uint16_t arr, uint16_t psc)
{
    BTIM_TIMX_INT_CLK_ENABLE();
    BTIM_TIMX_INT->ARR = arr;           /* 设定计数器自动重装值 */
    BTIM_TIMX_INT->PSC = psc;           /* 设置预分频器  */
    BTIM_TIMX_INT->DIER |= 1 << 0;      /* 允许更新中断 */
    BTIM_TIMX_INT->CR1 |= 1 << 0;       /* 使能定时器TIMX */
    sys_nvic_init(1, 3, BTIM_TIMX_INT_IRQn, 2); /* 抢占1，子优先级3，组2 */
}
```

按配置步骤拆：

1. `BTIM_TIMX_INT_CLK_ENABLE()`：开 TIM6 时钟（`RCC->APB1ENR` bit4）。不开时钟后面写寄存器全无效。
2. `ARR = arr`（4999）：设溢出重装值。
3. `PSC = psc`（7199）：设预分频，得到 10kHz 计数频率。
4. `DIER |= 1<<0`：置 `UIE`，允许"更新中断"上报 NVIC。
5. `CR1 |= 1<<0`：置 `CEN`，**计数器开始跑**。
6. `sys_nvic_init(1,3,TIM6_DAC_IRQn,2)`：NVIC 分组 2、抢占 1、子优先级 3，并打开 `TIM6_DAC_IRQn` 这路中断。

> 为什么"先设 ARR/PSC、再开 CEN"就够、不用 EGR.UG？因为 CEN 打开后第一次更新会把预分频/重装缓冲值装载生效，500ms 节拍正确。

### 5.3 实验7-1：中断服务函数 —— 翻绿灯并清标志

摘自 [btim.c](<实验7-1 基本定时器中断实验/Drivers/BSP/TIMER/btim.c>)：

```c
void BTIM_TIMX_INT_IRQHandler(void)
{ 
    if (BTIM_TIMX_INT->SR & 0X0001)   /* 溢出中断 */
    {
        LED1_TOGGLE();
    }

    BTIM_TIMX_INT->SR &= ~(1 << 0); /* 清除中断标志位 */
} 
```

- 进入本函数说明发生了 TIM6 更新事件（每 500ms 一次）。
- `if (SR & 0x0001)`：确认是 `UIF`（更新中断标志）引起的，翻转 LED1。
- `SR &= ~(1<<0)`：**必须手动清 UIF**，否则退出后立刻又满足中断条件，会不停重入——这是定时器中断最常见的坑。

### 5.4 主循环

[User/main.c](<实验7-1 基本定时器中断实验/User/main.c>)：

```c
    while (1)
    {
        LED0_TOGGLE();
        delay_ms(200);
    }
```

主循环只管以 200ms 节奏翻红灯 LED0，跟定时器互不干扰；两条不同频率的闪烁正好让你直观区分"软件延时"与"硬件定时中断"。

## 六、编译、下载与运行

1. Keil MDK 打开 `实验7-1 基本定时器中断实验/Projects/MDK-ARM/atk_f103.uvprojx`，器件为 `STM32F103RC`。
2. 工程已配好启动文件与包含路径（`Drivers/...`、`User/`），直接 Build。
3. 产物 `.axf`/`.hex` 在工程的输出目录；用 ST-LINK/J-LINK 或串口 ISP 下载到开发板。
4. 现象靠 LED 观察，不强制开串口；若调试打印，波特率 115200。

## 七、实验现象

### 7.1 实验7-1 基本定时器中断实验

对照官方"实验现象"，实际应看到：

1. **LED0（红灯）快闪**：主循环每 200ms 翻转，即约 0.4s 一个明暗周期。
2. **LED1（绿灯）慢闪**：TIM6 每 500ms 进中断翻转，即约 1.0s 一个明暗周期。
3. 二者频率明显不同（红灯比绿灯快），说明"定时中断独立于主循环在稳定工作"。
4. 若绿灯停闪，多半是定时器中断没进（检查 `DIER.UIE`、NVIC 使能、是否忘记清 `SR.UIF`）。

## 八、注意事项

- **清标志别忘了**：中断里一定要 `SR &= ~(1<<0)` 清 UIF，否则中断反复进入，LED1 会以极高频率闪（看起来像常亮）。
- **时钟顺序**：先 `RCC->APB1ENR` 开 TIM6 时钟，再写它的寄存器，最后 `CR1.CEN` 启动，顺序别乱。
- **APB1 与定时器 ×2**：TIM6 计数频率基于 72MHz（APB1 36MHz×2），换主频或改分频会直接改变 500ms 结果，重算公式。
- **UG 位所在寄存器**：软件更新 UG 在 `TIM6->EGR`(bit0) 而**非** `CR2`；本例不需要 UG，特此澄清常见误解。
- **中断优先级/分组**：`sys_nvic_init(...,2)` 用的是 NVIC 分组 2，若你工程改分组会改变抢占/子优先级的位分配。
- **代码与官方说明一致性**：官方 `readme.txt`（"500ms 中断一次、LED1 翻转；LED0 200ms 一次"）与代码完全一致，无差异。
- **源码为 GBK 编码**：正确选编码打开中文注释；本 README 为 UTF-8。

## 九、总结与延伸

**本实验要点：**

1. 基本定时器 = PSC 预分频 + CNT 计数 + ARR 重装；`Tout=((ARR+1)*(PSC+1))/Ft`，本工程 500ms。
2. 更新事件置位 `SR.UIF`，靠 `DIER.UIE` 放行到 NVIC，进 `TIM6_DAC_IRQHandler`。
3. 中断服务函数三步：判标志 → 做事 → 清标志（漏清标志是最常见故障）。
4. 通过宏把"用哪个 TIM/中断号/时钟位"封装起来，换定时器只改几行。

**延伸小练习：**

1. 把 `btim_timx_int_init(5000-1, 7200-1)` 改成 `(2000-1, 7200-1)`，绿灯周期应变为 200ms，数一下频率变化。
2. 在初始化里加一句 `BTIM_TIMX_INT->EGR |= 1 << 0;`（置 UG），观察它立即产生一次更新（体验软件更新事件），再理解本例为何其实不需要。
3. 把中断里的 `LED1_TOGGLE()` 换成经串口打印一行计数，配合全局变量做一个"秒表/心跳"。

**衔接**：掌握了"定时 + 更新中断"，后面「通用定时器」会加上计数外部脉冲、输出 PWM、输入捕获；
「ADC 采样触发」也常用定时器来产生规则序列触发——都是这套模型的延伸。
