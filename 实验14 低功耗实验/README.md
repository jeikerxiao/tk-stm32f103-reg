# 实验14 低功耗实验

> 一句话概括：让 STM32 在"没事干"的时候把 CPU、时钟甚至稳压器逐级关掉，用按键（EXTI/WKUP 引脚）把它叫醒；
> 顺带学会用 PVD（可编程电压监测器）在电池快没电时抢先报警——这是所有电池供电产品的基本功。

## 一、实验简介

- **实验目的（官方）**：分别学习 STM32 的 PVD 电压监控、睡眠模式、停止模式、待机模式的使用。
- **通俗展开**：单片机最费电的地方是"CPU 一直在空转"。低功耗的思路很简单——
  ① 人在打盹（Sleep）：身体停下手脚还热着，一有动静立刻醒；
  ② 深睡（Stop）：连内部稳压器的功耗都降下来，只有少数几个"门铃"（EXTI 中断、WKUP 引脚）还醒着；
  ③ 关机（Standby）：除了记忆区（备份域/RTC），全屋断电，只能靠"重新开机"（复位）来唤醒。
  再加上 PVD 这个"电压哨兵"，随时盯着 VDD，一旦低于你设定的门限就立刻通知你抢救数据。
- **学习路线中的位置**：
  - 前置：实验2 按键输入实验、实验3 外部中断实验（唤醒全靠 EXTI）、实验11 TFTLCD（MCU屏）实验、实验13 RTC 实验（掉电保时间）。
  - 后续：实验26 FLASH 模拟 EEPROM、实验28 SD 卡实验等需要掉电保护的场景，以及一切电池供电项目。
- **实验平台**：正点原子 MiniSTM32 V4 开发板，主芯片 STM32F103RC（Cortex-M3），Keil MDK 工程。
- **代码风格**：寄存器版，直接写 `PWR->CR`、`SCB->SCR`、`EXTI->PR`，并靠内联汇编 `wfi` 指令进入低功耗。

### 1.1 子实验一览

| 子实验 | 目录 | 主角 | 一句话结论 |
| --- | --- | --- | --- |
| 14-1 PVD电压监控 | `实验14-1 PVD电压监控/` | `PWR->CR` 的 PVDE/PLS + EXTI 线 16 | VDD 低于 2.9V 就中断报警（不是低功耗模式，属 PWR 模块） |
| 14-2 睡眠模式 | `实验14-2 睡眠模式/` | 只执行 `wfi` | 省电最少、唤醒最快、无需重配时钟 |
| 14-3 停止模式 | `实验14-3 停止模式/` | `LPDS=1` + `PDDS=0` + SLEEPDEEP + `wfi` | 中等省电，SRAM/寄存器保住，**唤醒后必须重新配置时钟** |
| 14-4 待机模式 | `实验14-4 待机模式/` | `PDDS=1` + SLEEPDEEP + `wfi` + WKUPEN | 最省电，SRAM 全丢，**只能靠复位（重新运行程序）退出** |

## 二、实验原理

### 2.1 先搞懂三个"开关"

进入哪种低功耗模式，其实是两个寄存器位 + 一条指令组合出来的：

| 名字 | 位置 | 作用 |
| --- | --- | --- |
| SLEEPDEEP | `SCB->SCR` 第 2 位（Cortex-M3 内核的寄存器，不是 STM32 外设） | 0 = 执行 `wfi` 后进**睡眠**；1 = 进**深度睡眠**，再由 PDDS 决定是停止还是待机 |
| LPDS | `PWR->CR` 第 0 位 | 深度睡眠时内部稳压器的状态：0 = 正常运行模式，1 = 低功耗模式（只在停止模式下有感） |
| PDDS | `PWR->CR` 第 1 位 | SLEEPDEEP=1 时的分岔口：0 = 停止模式，1 = 待机模式 |
| WFI 指令 | `sys_wfi_set()`，[sys.c](<实验14-2 睡眠模式/Drivers/SYSTEM/sys/sys.c>) | "Wait For Interrupt"，CPU 停下，等任意一个已使能的中断把它叫醒 |

组合真值表（本实验四份代码就是这么拼的）：

| SLEEPDEEP | PDDS | LPDS | 结果 |
| --- | --- | --- | --- |
| 0 | 无关 | 无关 | **Sleep 睡眠模式** |
| 1 | 0 | 0 | Stop，稳压器满速（更费电，唤醒更快） |
| 1 | 0 | 1 | **Stop 停止模式**（本工程用的组合） |
| 1 | 1 | 无关 | **Standby 待机模式**（稳压器关掉） |

`sys.c` 里那句汇编就是全部秘密：

```c
void sys_wfi_set(void)
{
    __ASM volatile("wfi");
}
```

### 2.2 三种模式的横向对比

| 对比项 | 睡眠 Sleep | 停止 Stop | 待机 Standby |
| --- | --- | --- | --- |
| CPU 时钟 HCLK | 停 | 停 | 停 |
| 外设时钟（APB/AHB） | **继续跑**（外设照常工作，也能唤醒 CPU） | 停 | 停 |
| 内部稳压器 | 正常 | LPDS=1 时低功耗 | **关闭** |
| PLL/HSE/HSI | 保持运行 | 全部关闭 | 全部关闭 |
| SRAM / 寄存器内容 | 全部保持 | 全部保持 | **全部丢失**（仅备份域和 RTC 保留） |
| 唤醒源 | 任意已使能的中断/事件 | 任意 EXTI 线中断（16~19 内部事件线）、PWR 的 WKUP 引脚 | **只有** WKUP 引脚上升沿、外部复位、上电/掉电复位、独立看门狗复位 |
| 唤醒后做什么 | 从 `wfi` 下一条继续跑，**不用重配时钟** | 从 `wfi` 下一条继续跑，但**必须重新配置系统时钟** | 相当于复位，程序**从头开始执行** |
| 进入代码 | `pwr_enter_sleep()` | `pwr_enter_stop()` | `pwr_enter_standby()` |
| 省电程度 | 小（mA 级） | 中（µA 级） | 大（三者中最省，µA 级以下） |
| 唤醒速度 | 最快（几个时钟周期） | 慢（晶振/PLL 要重新起来，本工程靠软件重建时钟，有肉眼可见的短延迟） | 最慢（等于重新上电，要跑一遍完整的初始化流程） |

一句话记忆：**省电越多，醒来越慢、丢的东西越多。**

> 表中"省电程度"只给数量级参考（睡眠 mA 级、停止几十 µA 级、待机 µA 级）。
> 具体数值取决于 VDD、温度、还开着哪些外设，本工程代码不做任何电流测量，
> 精确数字请查 STM32F103 数据手册的 Standby/Stop/Sleep current 表，或自己串万用表实测。

### 2.3 WK_UP 按键为什么是唤醒首选

WK_UP 是开发板上的 **PA0** 按键（代码里的宏名 `WK_UP`）。选它有三个原因：

1. PA0 同时是 STM32 的 **PWR_WKUP 引脚**，硬件上连到内部，待机模式下即使全屋断电，它依然能被监测（`PWR->CSR` 第 8 位 WKUPEN 使能）。
2. PA0 也是普通 EXTI 线 0，睡眠/停止模式下靠 `EXTI0_IRQHandler` 唤醒即可。
3. 按键按下时 PA0 从低变高（代码把 PA0 配成了内部下拉输入 `SYS_GPIO_PUPD_PD`），天然产生**上升沿**，正好满足 WKUP 的唤醒条件。

`pwr_wkup_key_init()`（[pwr.c](<实验14-2 睡眠模式/Drivers/BSP/PWR/pwr.c>)）做的就是把 PA0 配成下拉输入 + 上升沿触发的外部中断：

```c
sys_gpio_set(PWR_WKUP_GPIO_PORT, PWR_WKUP_GPIO_PIN,
             SYS_GPIO_MODE_IN, SYS_GPIO_OTYPE_PP, SYS_GPIO_SPEED_MID, SYS_GPIO_PUPD_PD);    /* WKUP引脚模式设置,下拉输入 */

sys_nvic_ex_config(PWR_WKUP_GPIO_PORT, PWR_WKUP_GPIO_PIN, SYS_GPIO_RTIR);   /* WKUP配置为上升沿触发中断 */
sys_nvic_init( 2, 2, PWR_WKUP_INT_IRQn, 2);                                 /* 抢占2，子优先级2，组2 */
```

**进入低功耗前必须先清 EXTI 挂起标志**（`EXTI->PR = PWR_WKUP_GPIO_PIN;`）。
因为如果线 0 上还挂着上次没处理完的标志，`wfi` 一执行就"立刻被中断叫醒"，看起来就像根本睡不下去。
这是本实验最容易踩的坑。

### 2.4 停止/待机的"收尾工作"

- 停止模式：`wfi` 返回后，`pwr_enter_stop()` 把 SLEEPDEEP 清回 0（`SCB->SCR &= ~(1 << 2);`），
  但进入停止时 **HSE、HSI、PLL 都被硬件关掉了**，醒来时系统时钟只能退到内部 HSI（8MHz）这条兜底路上，
  主频从 72MHz 掉回 8MHz。所以 `main.c` 必须手动重新初始化时钟和外设（详见 5.3）。
- 待机模式：稳压器整个关掉，CPU 的"记忆"（SRAM、寄存器）全丢，硬件走的就是复位流程，
  所以代码里 `pwr_enter_standby()` 后面那句注释说得很明白：**不会执行到这里**。
  想区分"是复位开机还是待机唤醒"，可以读 `PWR->CSR` 第 1 位 SBDS（待机标志）。

### 2.5 PVD 电压监控（14-1）原理

PVD = Programmable Voltage Detector，可编程电压监测器。它就是一个内部比较器：
把 **VDD 实际电压**和**你选定的门限**比一比，`PWR->CSR` 第 2 位 PVDSO 输出比较结果（1 表示 VDD 已低于门限）。

门限由 `PWR->CR` 的 **PLS[2:0]（第 7:5 位）** 决定，共 8 档（代码注释即手册值）：

| pls 值 | 000 | 001 | 010 | 011 | 100 | 101 | 110 | 111 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 门限电压 | 2.2V | 2.3V | 2.4V | 2.5V | 2.6V | 2.7V | 2.8V | **2.9V** |

比较器翻转不直接是一个中断号，而是接到 **EXTI 的线 16**（内部事件线，16 = PVD/AVD，17~19 = RTC 闹钟/USB 唤醒等），
所以要三步一起配：`EXTI->IMR |= 1 << 16`（不屏蔽）、`EXTI->FTSR |= 1 << 16`（下降沿）、`EXTI->RTSR |= 1 << 16`（上升沿），
再使能 `PVD_IRQn`。这样"电压跌破门限"和"电压恢复"两个方向都能进中断。

用途：电池设备在 VDD 跌到 MCU 开始乱跑之前，还有一段"还能工作但已不可靠"的窗口，
PVD 就是给你抢这几十毫秒去把关键数据写进 FLASH/备份区的机会。

## 三、硬件资源与接线

### 3.0 各子实验共用的资源

| 资源 | 引脚 | 代码中的宏/位置 | 说明 |
| --- | --- | --- | --- |
| LED0（红灯） | PA8 | `LED0_GPIO_PORT/PIN`，[led.h](<实验14-2 睡眠模式/Drivers/BSP/LED/led.h>) | 推挽输出、低电平点亮；闪烁 = 程序在跑 |
| LED1（绿灯） | PD2 | `LED1_GPIO_PORT/PIN`，[led.h](<实验14-2 睡眠模式/Drivers/BSP/LED/led.h>) | 睡眠/停止/低电压指示 |
| KEY0 | PC5 | `KEY0_GPIO_PORT/PIN`，[key.h](<实验14-2 睡眠模式/Drivers/BSP/KEY/key.h>) | 上拉输入，按下为**低**电平（`key_scan()` 判 `KEY0 == 0`），用于"请求进入低功耗" |
| KEY1 | PA15 | `KEY1_GPIO_PORT/PIN`，[key.h](<实验14-2 睡眠模式/Drivers/BSP/KEY/key.h>) | 本实验未使用 |
| WK_UP | PA0 | `PWR_WKUP_GPIO_PORT/PIN`（= `WKUP_GPIO_*`） | 下拉输入，按下为高，**唤醒源** |
| 串口1 | TX=PA9，RX=PA10 | [usart.h](<实验14-2 睡眠模式/Drivers/SYSTEM/usart/usart.h>) | 115200bps，接板载 CH340 |
| TFTLCD（MCU 屏 8080 并口） | D0~D15=PB0~PB15，WR=PC7，RD=PC6，CS=PC9，RS=PC8，BL=PC10 | [lcd.h](<实验14-2 睡眠模式/Drivers/BSP/LCD/lcd.h>) | 仅支持 MCU 屏，不支持 RGB 屏 |

- LCD 插在开发板 LCD 插座上，注意对齐 VCC 一角，插反会烧模块。
- 想用 USB 转串口，把 USART1 排针的 PA9→RXD、PA10→TXD 用跳线帽连好（本工程四个子实验都没调 USMART，串口只是初始化好备用）。
- `sys_stm32_clock_init(9)`：8MHz HSE × 9 = **72MHz** 主频。三种模式对它的影响不同：
  睡眠时 72MHz 还在（只是 CPU 不取指令）；停止时 HSE/HSI/PLL 全被关掉，醒来后**必须软件重建**；
  待机时等于重新上电，程序从头跑一遍再配一次。见各子实验小节。

### 3.1 实验14-1 PVD电压监控

只用片上资源：`PWR` + `EXTI 线16` + `PVD_IRQn`，加 LED0/LED1 与 LCD 做显示。
**要人为制造"低电压"才看得到效果**，可选做法：

1. 用可调稳压电源从 3.3V 输入端缓慢下调，跨过 2.9V；
2. 换一根又长又细的 USB 线 + 大负载（点亮背光、插 U 盘），让 VDD 有轻微压降；
3. 轻轻把 USB 线拔出/插回，捕捉上电/掉电瞬间的电压斜坡。

### 3.2 实验14-2 睡眠模式

用 **KEY0(PC5)** 请求进入睡眠、**WK_UP(PA0)** 唤醒，LED1 作进入/退出指示，无需外接任何东西。

### 3.3 实验14-3 停止模式

资源与 14-2 完全相同，只是进入方式更深；板上没有跳线要求。
（想实测电流，需把开发板的供电跳线/电流表串进去测，本实验只验证功能不验证电流。）

### 3.4 实验14-4 待机模式

资源同上，唯一区别是唤醒只认 **PA0 的 WKUP 上升沿**（或按板上 RST 复位键）。

## 四、代码结构与调用流程

### 4.1 目录树（四个子实验结构一样，只列相关的）

```
实验14 低功耗实验/
├── 实验14-1 PVD电压监控/
│   ├── User/main.c                    PVD 初始化 + LED0 闪烁 + LCD 显示
│   └── Drivers/BSP/PWR/pwr.c|.h       pwr_pvd_init() + PVD_IRQHandler()
├── 实验14-2 睡眠模式/
│   ├── User/main.c                    KEY0 进睡眠，WK_UP 唤醒
│   └── Drivers/BSP/PWR/pwr.c|.h       + pwr_wkup_key_init() / pwr_enter_sleep()
├── 实验14-3 停止模式/                 pwr.c|.h 再加 pwr_enter_stop()
└── 实验14-4 待机模式/                 pwr.c|.h 再加 pwr_enter_standby()
```

每个工程还包含 `Drivers/SYSTEM/{sys,delay,usart}`（时钟/GPIO/NVIC、SysTick 延时、串口 printf）、
`Drivers/BSP/{LED,KEY,LCD}`（灯、按键、屏）与 `Middlewares/USMART`（本工程未被 `main` 调用）。

`pwr.h` 的版本演进正好对应四个子实验：V1.0 只有 PVD；V1.1 加睡眠；V1.2 加停止；V1.3 加待机。

### 4.2 main() 流程

- **14-1**：时钟 → 延时 → 串口 → LED → LCD → `pwr_pvd_init(7)`（2.9V 档）→ 屏显 "PVD Voltage OK!" → 循环里每 200ms 翻转 LED0。低电压时 `PVD_IRQHandler` 自己会把屏幕和 LED1 改掉。
- **14-2**：…→ `key_init()` → `pwr_wkup_key_init()` → 屏显操作提示 → 循环：
  `key_scan(0)`；按到 KEY0 就 `LED1(0)` → `pwr_enter_sleep()` →（睡）→（被 PA0 上升沿唤醒）→ `LED1(1)` → 继续闪 LED0。
- **14-3**：初始化同 14-2；按到 KEY0 就 `LED1(0)` → `pwr_enter_stop()` →（睡）→（被唤醒）→
  **`sys_stm32_clock_init(9)` → `led_init()` → `key_init()`** → `LED1(1)` → 继续闪 LED0。
- **14-4**：初始化同 14-2；按到 KEY0 就 `pwr_enter_standby()` →（睡）→（WK_UP 上升沿 = 硬件复位）→ **程序从头重跑**。

## 五、核心代码解析

### 5.1 实验14-1 PVD：初始化与中断（[main.c](<实验14-1 PVD电压监控/User/main.c>)、[pwr.c](<实验14-1 PVD电压监控/Drivers/BSP/PWR/pwr.c>)）

```c
void pwr_pvd_init(uint8_t pls)
{
    RCC->APB1ENR |= 1 << 28;    /* 使能电源时钟 */
    PWR->CR &= ~(3 << 5);       /* PLS[2:0]清零 */
    PWR->CR |=  pls << 5;       /* PLS[2:0] = pls,注意不要超范围! */
    PWR->CR |= 1 << 4;          /* PVDE = 1,使能PVD检测 */

    EXTI->IMR |= 1 << 16;       /* 开启line16 上的中断(PVD & AVD) */
    EXTI->FTSR |= 1 << 16;      /* line16 上事件下降沿触发 */
    EXTI->RTSR |= 1 << 16;      /* line16 上事件上升降沿触发 */

    sys_nvic_init(3, 3, PVD_IRQn, 2);   /* 组2，最低优先级 */
}
```

逐位说明：

1. `RCC->APB1ENR` 第 28 位是 **PWREN**：PWR 挂在 APB1 上，不开总线时钟，后面写 `PWR->CR` 全无效。
2. `PWR->CR` 第 7:5 位 **PLS[2:0]**：门限选择，`main` 传 `pls = 7`（111）→ **2.9V**；先清零再 `pls << 5` 是标准的"移位填字段"写法。
3. `PWR->CR` 第 4 位 **PVDE**：PVD 比较器总开关。
4. `EXTI->IMR` 第 16 位 = 1：不屏蔽线 16；`FTSR`/`RTSR` 第 16 位同时置 1 = 两个边沿都触发中断。
5. `sys_nvic_init(3, 3, PVD_IRQn, 2)`：NVIC 分组 2，抢占优先级 3、子优先级 3（该组下的最低优先级），
   即"电压报警"不该打断别的中断处理。

中断服务函数只做三件事——读状态、报结果、清标志：

```c
void PVD_IRQHandler(void)
{
    if (PWR->CSR & (1 << 2))    /* 电压比PLS所选电压还低 */
    {
        lcd_show_string(30, 130, 200, 16, 16, "PVD Low Voltage!", RED);
        LED1(0);                                                        /* 点亮绿灯, 表明电压低了 */
    }
    else
    {
        lcd_show_string(30, 130, 200, 16, 16, "PVD Voltage OK! ", BLUE);
        LED1(1);                                                        /* 灭掉绿灯 */
    }

    EXTI->PR |= 1 << 16;        /* 清除line16的中断标志 */
}
```

`PWR->CSR` 第 2 位 **PVDSO** 是比较器实时输出：1 = VDD 已低于门限。写 `EXTI->PR` 的第 16 位（写 1 清除）
是 EXTI 的规矩，不清就会无限重进中断。

### 5.2 实验14-2 睡眠模式（[main.c](<实验14-2 睡眠模式/User/main.c>)、[pwr.c](<实验14-2 睡眠模式/Drivers/BSP/PWR/pwr.c>)）

进入睡眠的函数短到只有两行——这是它与手册里复杂描述的最大反差，值得记住：

```c
void pwr_enter_sleep(void)
{
    EXTI->PR = PWR_WKUP_GPIO_PIN;   /* 清除WKUP上的中断标志位 */
    sys_wfi_set();                  /* 执行WFI指令, 进入睡眠模式 */
}
```

- **为什么没动 `PWR->CR` 和 `SCB->SCR`？** 因为睡眠 = SLEEPDEEP 为 0 时的 WFI，
  而 `SCB->SCR` 复位值就是 0，本工程也没人置过它，所以什么都不用配。
  `PWR->CR` 的 LPDS 位在睡眠模式下是**无效位**（手册：LPDS 只影响深度睡眠时的稳压器），配了也没意义。
- 唤醒后的动作在 `main` 里：

```c
if (key == KEY0_PRES)
{
    LED1(0);            /* 点亮绿灯,提示进入睡眠模式 */

    pwr_enter_sleep();  /* 进入睡眠模式 */

    LED1(1);            /* 关闭绿灯,提示退出睡眠模式 */
}
```

`wfi` 之后的那条语句会照常执行，因为睡眠不丢任何东西。
- 唤醒用的中断服务函数是 `pwr.h` 里宏名换出来的：

```c
void PWR_WKUP_INT_IRQHandler(void)      /* 展开后就是 EXTI0_IRQHandler */
{
    EXTI->PR = PWR_WKUP_GPIO_PIN;       /* 清除WKUP所在中断线 的中断标志位 */
}
```

  `#define PWR_WKUP_INT_IRQn EXTI0_IRQn` / `#define PWR_WKUP_INT_IRQHandler EXTI0_IRQHandler`
  这种"用宏起别名"的写法，好处是以后换唤醒脚只改宏，不用动函数体。
  中断里**什么都不做、只清标志**，因为唤醒本身目的就是让 `wfi` 返回。

### 5.3 实验14-3 停止模式（[main.c](<实验14-3 停止模式/User/main.c>)、[pwr.c](<实验14-3 停止模式/Drivers/BSP/PWR/pwr.c>)）

```c
void pwr_enter_stop(void)
{
    RCC->APB1ENR |= 1 << 28;        /* 使能电源时钟 */
    EXTI->PR = PWR_WKUP_GPIO_PIN;   /* 清除WKUP上的中断标志位 */

    PWR->CR |= 1 << 0;              /* LPDS=1, 停止模式下电压调节器处于低功耗模式 */
    PWR->CR &= ~(1 << 1);           /* PDDS=0, CPU深度睡眠时进入停止模式 */
    SCB->SCR |= 1 << 2;             /* 使能SLEEPDEEP位 */

    sys_wfi_set();                  /* 执行WFI指令, 进入停止模式 */

    SCB->SCR &= ~(1 << 2);          /* 关闭SLEEPDEEP位 */
}
```

四个位各司其职：LPDS=1 让稳压器降到低功耗档（更省电）；PDDS=0 把"深度睡眠"引到停止而不是待机；
SLEEPDEEP=1 让 WFI 升级为深度睡眠；回来后把 SLEEPDEEP 清 0，以免影响下次进睡眠模式。

**退出后 `main.c` 的补课操作是本实验的重点**：

```c
if (key == KEY0_PRES)
{
    LED1(0);            /* 点亮绿灯,提示进入停止模式 */

    pwr_enter_stop();   /* 进入停止模式 */

    /* 从停止模式唤醒, 需要重新设置系统时钟, 72Mhz */
    sys_stm32_clock_init(9);    /* 设置时钟, 72Mhz */
    /* 由于sys_stm32_clock_init中会关闭外设时钟关闭，因此唤醒后用到的外设该时钟都得重新使能 */
    led_init();
    key_init();
    LED1(1);            /* 关闭绿灯,提示退出停止模式 */
}
```

原因看 `sys_stm32_clock_init()` 的头几行（[sys.c](<实验14-3 停止模式/Drivers/SYSTEM/sys/sys.c>)）：

```c
RCC->AHBENR = 0x00000014;       /* 睡眠模式闪存和SRAM时钟使能.其他关闭 */
RCC->APB2ENR = 0x00000000;      /* 外设时钟关闭 */
RCC->APB1ENR = 0x00000000;
```

它把所有外设时钟清零再重建，所以唤醒后必须把还想用的外设重新 `xxx_init()`。
本工程只补了 `led_init()` 和 `key_init()`；**没有**重开 USART1、LCD 和延时时钟——
所以从停止模式醒来后，屏幕保持原画面（LCD 内容在显存/GRAM 里），但 `printf`、刷屏、精确延时都不能再用。

### 5.4 实验14-4 待机模式（[main.c](<实验14-4 待机模式/User/main.c>)、[pwr.c](<实验14-4 待机模式/Drivers/BSP/PWR/pwr.c>)、[sys.c](<实验14-4 待机模式/Drivers/SYSTEM/sys/sys.c>)）

```c
void pwr_enter_standby(void)
{
    EXTI->PR = PWR_WKUP_GPIO_PIN;   /* 清除WKUP上的中断标志位 */

    sys_standby();                  /* 进入待机模式 */
}
```

真正干活的是 `sys_standby()`：

```c
void sys_standby(void)
{
    RCC->APB1ENR |= 1 << 28;    /* 使能电源时钟 */
    PWR->CSR |= 1 << 8;         /* 设置WKUP用于唤醒 */
    PWR->CR |= 1 << 2;          /* 清除WKUP 标志 */
    PWR->CR |= 1 << 1;          /* PDDS = 1, 允许进入深度睡眠模式(PDDS) */
    SCB->SCR |= 1 << 2;         /* 使能SLEEPDEEP位 (SYS->CTRL) */
    sys_wfi_set();              /* 执行WFI指令, 进入待机模式 */
}
```

- `PWR->CSR` 第 8 位 **WKUPEN**：让 PA0 这个"硬件唤醒脚"生效（待机时 EXTI 已经断电，只能靠它）。
- `PWR->CR` 第 2 位 **CWUF**：写 1 清除 `PWR->CSR` 第 0 位的 WUF（WKUP 标志）。
  这一步不能省——只要 WUF 还是 1，WKUP 引脚就被内部拉住，模式根本进不去（或刚进就被踢出来）。
- `PWR->CR` 第 1 位 **PDDS = 1** + SLEEPDEEP = 1：深度睡眠时选待机而非停止。
  注意待机模式下 LPDS 无意义（稳压器整个关掉）。

`main.c` 里那句注释是理解待机模式的关键：

```c
if (key == KEY0_PRES)
{
    pwr_enter_standby();    /* 进入待机模式 */
    /* 从待机模式唤醒相当于系统重启(复位), 因此不会执行到这里 */
}
```

也就是说：**唤醒后你看到的"重新亮屏、LED0 又开始闪"，是程序从 `main` 第一行重跑的结果，不是"睡醒了继续"**。
所有全局变量归零、外设重新初始化，只有 RTC（接 VBAT 时）和备份区里的东西还在。

## 六、编译、下载与运行

1. 每个子实验都是独立工程，分别打开：
   - `实验14-1 PVD电压监控/Projects/MDK-ARM/atk_f103.uvprojx`
   - `实验14-2 睡眠模式/Projects/MDK-ARM/atk_f103.uvprojx`
   - `实验14-3 停止模式/Projects/MDK-ARM/atk_f103.uvprojx`
   - `实验14-4 待机模式/Projects/MDK-ARM/atk_f103.uvprojx`
2. 器件 **STM32F103RC**，C 宏 `STM32F103xE`，启动文件 `startup_stm32f103xe.s`；
   包含路径 `..\..\Drivers\CMSIS\Device\ST\STM32F1xx\Include;..\..\Drivers\CMSIS\Include;..\..\Drivers;..\..\User;..\..\Middlewares`。
3. 编译输出在各自 `Output/` 目录（`atk_f103.axf`、`atk_f103.hex`），用 ST-Link/J-Link 下载。
4. 14-1 建议同时开串口助手（XCOM/SSCOM，**115200**）方便看调试信息；14-2/3/4 靠按键和灯就能验证，可以不开串口。
5. 想实测电流，需要断开板上供电跳线串万用表/电流表（本工程代码只保证功能正确，不保证电流数值）。

## 七、实验现象

### 7.1 实验14-1 PVD电压监控

1. 正常 3.3V 供电时，LCD 显示标题 `PVD TEST` 与蓝色的 `PVD Voltage OK!`，LED1 灭，LED0 每 200ms 闪一次。
2. 把供电电压缓慢降到 **2.9V 以下**（跨过 `pls = 7` 的门限）：进 `PVD_IRQHandler`，
   LCD 第 130 行变成红色的 `PVD Low Voltage!`，同时 LED1（绿灯）点亮。
3. 电压重新升回 2.9V 以上：EXTI 线 16 的**上升沿**再次进中断，显示回到 `PVD Voltage OK!`，LED1 灭。
4. 电压很低时屏可能先花/先灭，这是正常的——2.9V 已经接近 MCU 的可靠工作下限（2.0V 是绝对下限）。

### 7.2 实验14-2 睡眠模式

1. 上电后 LED0 每 200ms 闪烁，LCD 提示 `KEY0:Enter SLEEP MODE` / `KEY_UP:Exit SLEEP MODE`。
2. 按一下 **KEY0**：LED1（绿灯）点亮，LED0 **立刻停止闪烁**（CPU 已停在 `wfi`，主循环不再跑）。
3. 按一下 **WK_UP(PA0)**：LED0 恢复闪烁，同时 `LED1(1)` 把绿灯关掉 —— 说明从睡眠中醒来并接着执行 `wfi` 之后的语句。

### 7.3 实验14-3 停止模式

现象与 14-2 几乎一样（LED1 亮 = 进停止、LED0 停；按 WK_UP = 醒来、LED1 灭、LED0 继续闪），
区别在**醒来的代价**：因为要重跑 `sys_stm32_clock_init(9)`（等 HSE 起振 + PLL 锁定），
WK_UP 按下到 LED0 恢复闪烁之间有一段能感觉到的短延迟，而睡眠模式几乎是瞬间恢复。
另外醒来后屏幕上不会再更新，`printf` 也不通——见 8 的注意事项。

### 7.4 实验14-4 待机模式

1. 上电正常显示 `STANDBY TEST`，LED0 闪烁。
2. 按 **KEY0**：LED0 停，LCD 画面熄灭/变白（CPU 与外设整体断电，背光脚状态丢失）。
3. 按 **WK_UP(PA0)**：MCU 复位重跑，LCD 重新初始化并重新显示标题，LED0 又开始闪——
   这不是"继续执行"，是"重新开机"。按板上 **RST** 键同样能恢复。

## 八、注意事项与常见坑

1. **进不去低功耗 / 一进就醒**：99% 是 EXTI 挂起标志没清。`EXTI->PR` 写 1 清除（本工程用
   `EXTI->PR = PWR_WKUP_GPIO_PIN;`）必须在 `wfi` 之前做，`pwr_enter_sleep/stop/standby` 都写了这一句，别删。
2. **唤醒不生效**：确认三件事——`sys_nvic_ex_config()` 里对应线的 `EXTI->IMR` 已置 1、边沿方向对（WK_UP 是**上升沿**）、
   `sys_nvic_init()` 使能了这个 IRQn 且 NVIC 分组参数正确（本工程用组 2）。
3. **睡眠模式没省电**：睡眠时**外设还在跑**（ADC、定时器、USART、LCD 都会吃电流）。想更省电，进睡眠前先关外设时钟或停外设。
4. **停止模式唤醒后程序跑飞/时间错乱**：忘了重配时钟。必须像 14-3 那样调 `sys_stm32_clock_init(9)`，
   然后**重新初始化所有还要用的外设**（`sys_stm32_clock_init` 会把 `RCC->APB2ENR/APB1ENR` 清零）。
   唤醒后如果要 printf，记得补 `usart_init(72, 115200)`；要刷屏，补 `lcd_init()`。
5. **待机模式想"接着跑"是不可能的**：SRAM 内容全丢，栈也没了，只能重新复位。
   需要跨待机保存的数据，请放备份区 SRAM/BKP 或 FLASH（参考实验13 的 `rtc_write_bkr`、实验26 的模拟 EEPROM）。
6. **待机 + WKUP 的时序**：`sys_standby()` 里那句 `PWR->CR |= 1 << 2`（CWUF）不能省，WUF 标志不清零就进不去/立刻退出。
7. **PVD 门限别乱填**：`pls` 只接受 0~7，传 8 以上会把 `PWR->CR` 更高位（DBP 等）污染掉。
   本工程固定 7（2.9V），离 3.3V 只有 0.4V 余量，正常供电时**不会**误触发，但电源波动大时可能反复跳。
8. **代码与手册/说明的几处不一致（以手册与代码实际行为为准）**：
   - `pwr_pvd_init()` 里清零用的是 `PWR->CR &= ~(3 << 5);`，只清了第 6:5 位；
     而 PLS 有 3 位（第 7:5 位）。也就是说如果曾设过 pls ≥ 4（bit7 = 1）再改小，bit7 不会被清掉。
     本工程只在启动时调用一次并传 7，不受影响，但你自己改代码时建议写成 `&= ~(7 << 5)`。
   - 实验14-3 的 `readme.txt` "实验现象"整段是从 14-2 复制来的，里面写的是"进入/退出**睡眠模式**"，
     实际本实验是**停止模式**（代码里也是 `pwr_enter_stop()`、LCD 显示 `STOP TEST`）。
   - 实验14-4 的 `readme.txt` "硬件资源"写的是"电池管理(低功耗模式 - 停止模式)"，实际是**待机模式**（`STANDBY TEST`）。
   - 实验14-2/3/4 的 `readme.txt` "注意事项"是"无"，实际上停止/待机模式的两条硬规矩（清 EXTI 标志、唤醒后重配时钟）
     恰恰是最需要注意的，本文档第八章已补齐。
   - `sys_standby()` 中 `SCB->SCR |= 1 << 2` 那句的注释写成 "(SYS->CTRL)"，那是这份代码沿用的旧称呼；
     本工程的头文件里**没有** `SYS` 这个外设，也不要去找 `SYS->CTRL`，它就是 CMSIS 的 `SCB->SCR` 第 2 位 SLEEPDEEP。
9. **安全提示**：14-1 反复把电压拉到 2.9V 以下、14-3/14-4 频繁进入深度睡眠，对板子无害但会让调试器（ST-Link）
   在待机后短暂找不到芯片——这是正常的，按 RST 或重新上电即可，不要以为芯片烧了。

## 九、总结与延伸

**本实验掌握的要点**：

1. 低功耗模式 = `SLEEPDEEP`（`SCB->SCR` bit2）× `PDDS`（`PWR->CR` bit1）× `LPDS`（`PWR->CR` bit0）三个位组合，最后统一由一条 `wfi` 指令触发。
2. 睡眠：外设照常工作，任意中断可唤醒，醒来即用，代价是省电最少。
3. 停止：时钟全停但 SRAM/寄存器保住，唤醒后**软件必须重建 72MHz 时钟并重开外设时钟**。
4. 待机：稳压器关闭、SRAM 全丢，只有 WKUP 上升沿/复位能"唤醒"，实质是**重新复位运行**。
5. PVD：`PWR->CR` 的 PVDE + PLS[2:0] 选门限，结果经 EXTI 线 16 双沿触发中断，`PWR->CSR` bit2（PVDSO）读实时状态。
6. 进低功耗前清 `EXTI->PR`、进待机前清 WUF（`PWR->CR` bit2）并置 WKUPEN（`PWR->CSR` bit8）——这两句是最容易被删掉导致"睡不下去"的代码。

**延伸小练习**：

1. 在 14-2 的 `PWR_WKUP_INT_IRQHandler()` 里加一句 `printf("waked up!\r\n");`，比较中断内打印和 `wfi` 返回后打印的先后顺序。
2. 改 14-1：把 `pwr_pvd_init(7)` 依次改成 0~7，用可调电源找出每档实际的翻转点，验证 2.2~2.9V 门限表；
   顺手把清零语句改成 `PWR->CR &= ~(7 << 5);`，观察反复换档时门限是否还能改小。
3. 改 14-3：唤醒后补上 `usart_init(72, 115200);` 和 `lcd_init();`，再 `printf` 一句"已从停止模式恢复"，
   把"睡眠后外设不断"和"停止后外设需重建"的差异亲眼跑出来。
   综合版（推荐）：把 14-4 的待机 + 实验13 的 RTC 合起来——进待机前用 `rtc_write_bkr()` 记一次数，
   WK_UP 唤醒复位后读出并显示"已经睡了 N 次"，这就是一个真正的电池设备原型。

**衔接下一课**：低功耗章节讲的是"少耗电"，实验15 DMA 实验讲的是"少费 CPU"。
两者目标一致——把活交给硬件自己去干，让 CPU 尽可能睡得更久、醒得更短。
