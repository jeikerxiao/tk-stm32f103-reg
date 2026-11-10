# 实验18 DAC实验

> 一句话概括：用 STM32F103 片内的 12 位 DAC 把数字"写成电压"，从 PA4 输出直流、三角波、正弦波；三个子实验分别演示**软件直接写值**、**软件循环合成三角波**和**TIM7 触发 + DMA 循环搬运正弦表**。

## 一、实验简介

- **实验目的**（取自各子实验官方 `readme.txt`）：
  - 18-1：学习 STM32 内部 DAC 的使用；
  - 18-2：学习 STM32 DAC 输出三角波的使用；
  - 18-3：学习 STM32 DAC DMA 输出正弦波的使用。
  通俗地说：ADC（实验16）是把外面的电压读成数字，DAC（数模转换器）则相反——把数字"倒"成一个电压。你给它 0 它输出约 0V，给它 2048 它输出约 1.65V，给它 4095 它输出接近 3.3V。如果按一定节奏不断给它不同的数字，引脚上就"画"出了三角波、正弦波、任意波形。
- **在学习路线中的位置**：
  - 前置：实验16 ADC 实验（DAC 实验要用 ADC 回读自己的输出做验证）、实验15 DMA 实验、实验7 定时器实验（18-3 用 TIM7 做节拍）、实验2 按键输入实验、实验11 TFTLCD 实验、实验12 USMART 调试实验。
  - 后续：实验41 综合测试实验（用 DAC+ADC+定时器组合输出波形）。
- **开发板与芯片**：正点原子 MiniSTM32 V4，主芯片 STM32F103RC，72MHz；片内 1 个 DAC 模块、2 个输出通道。

## 二、实验原理

### 2.1 DAC 是什么，数据怎么走到引脚

DAC 内部是一串电阻（或电容阵列）+ 一个开关网络：12 位数字控制 12 路开关，把参考电压 `VREF+`（本板 = VDDA = 3.3V）按二进制权重分压，得到对应大小的模拟电压。转换关系：

```text
Vout = DHR × VREF+ / 4096 = DHR × 3.3V / 4096 ≈ DHR × 0.806mV
DHR = 0    → 约 0V      DHR = 2048 → 约 1.65V      DHR = 4095 → 约 3.30V
```

数据在芯片里的"旅程"是三级格子（都通过 `DAC_TypeDef` 结构体访问）：

| 名称 | 代码里的写法 | 作用（大白话） |
| --- | --- | --- |
| DHR 数据保持寄存器 | `DAC1->DHR12R1`（通道 1，12 位右对齐）；还有 `DHR12L1`、`DHR8R1`、`DHR12R2`、`DHR12RD` 等 | "待输出的草稿"。CPU/DMA 往里写数字，写进去不会立刻反映到引脚 |
| DOR 数据输出寄存器 | `DAC1->DOR1`（只读） | "正式输出"。DHR 的内容在被触发时拷贝进 DOR，DOR 决定此刻引脚上的电压 |
| 输出引脚 | DAC1_OUT1 = **PA4**，DAC1_OUT2 = **PA5** | 芯片固定，不可改到别的脚 |

关于对齐方式：本工程的 CMSIS 头 [stm32f103xe.h](实验18-1%20DAC输出实验/Drivers/CMSIS/Device/ST/STM32F1xx/Include/stm32f103xe.h) 把 `DAC_DHR12R1_DACC1DHR_Pos` 定义为 0、掩码 `0xFFF`，即 12 位右对齐数据写在寄存器 bit11:0，所以代码里直接 `DAC1->DHR12R1 = 2048;` 就够了；而 `DAC_DHR12L1_DACC1DHR_Pos` 是 4（12 位左对齐要用 bit15:4）。三种 DHR 只是给"12 位右对齐 / 12 位左对齐 / 8 位"三种写法准备的入口，最终存进的内部 DHRx 都是同一个 12 位寄存器。

### 2.2 关键开关：DAC->CR 的每一位

`DAC_CR` 里通道 1 用 bit0～bit15、通道 2 用 bit16～bit31（完全对称，所以代码用 `offset = (outx - 1) * 16` 一个变量搞定两个通道）。本工程涉及的位：

| bit（通道 1） | 名称 | 本实验填的值与作用 |
| --- | --- | --- |
| 0 | EN1 | 1 = 使能 DAC 通道 1（只使能模拟部分，数字部分一直工作）；写完 DHR 才有输出 |
| 1 | BOFF1 | **1 = 关闭输出缓冲**（注意是反逻辑）。缓冲关闭时输出阻抗高，只能接高阻负载（示波器 1MΩ、运放/比较器输入），换来的是更快的建立时间 |
| 2 | TEN1 | 触发使能。0 = 不靠外部触发（写 DHR 即传送到 DOR）；1 = 必须由选定的触发事件搬运 |
| 5:3 | TSEL1[2:0] | 触发源选择。18-1/18-2 填 0（配合 TEN=0，等效软件立即更新）；18-3 填 **2 = TIM7_TRGO** |
| 7:6 | WAVE1[1:0] | 内置波形发生器：00 关闭、01 噪声、10 三角波。**本工程三个实验都保持 00，见 2.4 说明** |
| 11:8 | MAMP1[3:0] | 内置三角波/噪声的幅度选择（幅度 = 2^MAMP − 1，最大 4095）。本工程未使用，始终为 0 |
| 12 | DMAEN1 | 1 = DAC 通道 1 的 DMA 请求使能（每次触发把 DHR 清空时向 DMA 要一个新数据） |
| 14 | ——（代码注释写作 CEN） | F103 上此位为保留位，代码写入的是 0，是无副作用的占位写法 |

18-1 的 `dac_init(1)` 逐行就是这张表的落地（[dac.c](实验18-1%20DAC输出实验/Drivers/BSP/DAC/dac.c)）：

```c
    DAC1->CR = 0;                       /* DR寄存器清零 */
    DAC1->CR |= 1 << (1 + offset);      /* BOFF1/2=1,关闭DAC通道1/2输出缓冲 */
    DAC1->CR |= 0 << (2 + offset);      /* TEN1/2=0,不使用触发功能 */
    DAC1->CR |= 0 << (3 + offset);      /* TSEL1/2[3:0]=0,软件触发 */
    DAC1->CR |= 0 << (6 + offset);      /* WAVE1/2[1:0]=0,不使用波形发生 */
    DAC1->CR |= 0 << (12 + offset);     /* DMAEN1/2=0,DAC1/2 DMA不使能 */
    DAC1->CR |= 0 << (14 + offset);     /* CEN1/2=0,DAC1/2工作在普通模式 */

    DAC1->CR |= 1 << (0 + offset);      /* 使能DAC1_OUT1/2 */
    DAC1->DHR12R1 = 0;                  /* 通道1输出 0 */
    DAC1->DHR12R2 = 0;                  /* 通道2输出 0 */
```

时钟与引脚：

```c
    RCC->APB2ENR |= 1 << 2;;            /* 使能DAC OUT1/2的IO口时钟(都在PA口,PA4/PA5) */
    RCC->APB1ENR |= 1 << 29;            /* 使能DAC1的时钟 */
```

即 DAC 外设挂在 **APB1（36MHz）**，而它的输出引脚在 GPIOA（APB2）。

### 2.3 输出能有多快（本工程的官方计算）

`dac_init()` 与 `dac_dma_wave_init()` 的函数头注释给出了两组上限估算，都是围绕"输出建立时间 tSETTLING"算的：

```text
dac_init:            DAC 输入时钟 APB1 = 36MHz（周期 27.8ns）；缓冲关闭时 tSETTLING = 3us
                     → 最高约 333KHz；一个周期 10 个点，则最高约 33KHz 波形
dac_dma_wave_init:   tSETTLING ≤ 4us
                     → 最高约 250KHz；一个周期 10 个点，则最高约 25KHz 波形
```

也就是说：**每个采样点之间至少要留 3～4µs**。18-3 的 300kHz 触发（3.3µs/点）已经贴着上限，实测高频段幅度会被"压平"一点，属正常。

### 2.4 三角波的两种做法：硬件波形发生器 vs 软件合成

- **硬件做法**（本工程没用它）：`WAVE1 = 10` 选内置三角波发生器，`MAMP1` 定幅度，`TEN1 = 1` + `TSEL1` 选一个触发源。之后每来一个触发，内部计数器自动 +1 或 −1，直接在 0 与幅度之间来回，**CPU 一行代码都不用跑**。
- **软件做法**（18-2 实际做法）：靠 `delay_us(dt)` 控制节拍，一个 `for` 循环反复 `DAC1->DHR12R1 += incval` 走到顶，再来一个 `for` 循环 `-= incval` 走到底：

```c
    for(j = 0; j < n; j++)
    { 
        DAC1->DHR12R1 = 0;                  /* 先输出0 */
        for(i = 0; i < (samples / 2); i++)  /* 输出上升沿 */
        { 
             DAC1->DHR12R1 +=  incval;
             delay_us(dt);
        }
        
        for(i = 0; i < (samples / 2); i++)  /* 输出下降沿 */
        {
             DAC1->DHR12R1 -=  incval;
             delay_us(dt);
        }
    }
```

由此得到频率公式（与函数头注释一致）：

```text
f ≈ 1 / (dt × samples)      →   f(KHz) ≈ 1000 / (dt(us) × samples)
18-2 Wave1: dt=5us, samples=2000 → 1000/(5×2000) = 0.1KHz = 100Hz（2000 点，波形很平滑）
18-2 Wave2: dt=500us, samples=20 → 1000/(500×20) = 0.1KHz = 100Hz（20 点，明显台阶）
```

递增量 `incval = (maxval + 1) / (samples / 2)` 是**整数除法**：`maxval=4095, samples=2000` 时得到 4（不是 4.096），所以波峰是 `1000 × 4 = 4000` ≈ 3.22V；`samples=20` 时得到 409，波峰 `10 × 409 = 4090` ≈ 3.29V。软件合成占用 CPU，调用一次输出 `n=100` 个波形约耗时 1 秒。

### 2.5 正弦波的做法：定时器节拍 + DMA 循环查表（18-3）

正弦波不能靠 DAC 内置发生器（它只会三角波/噪声），通用做法是 **DDS/查表**：事先把一周期正弦值算成一个数组（正弦表），然后：

```text
TIM7 每来一次更新事件(TRGO) → DAC 通道1 发出一次 DMA 请求 → DMA2_Channel3 把正弦表下一个数搬进 DAC1->DHR12R1
   ↑ DMA 配置为循环模式(CIRC=1)，搬到表尾自动回到表头 → 波形无限循环，CPU 完全不参与
```

频率计算（`dac_dma_wave_enable()` 注释给了公式）：

```text
TIM7 输入时钟 f = APB1(36MHz) × 2 = 72MHz     （APB1 有 2 分频时，其定时器时钟翻倍）
触发频率  ftrgo = f / ((psc + 1) × (arr + 1))
波形频率  fwave = ftrgo / 每周期点数(cndtr)

默认输出: psc=72-1, arr=10-1 → 72e6/(72×10) = 100kHz；100 点/周期 → 1kHz 正弦
KEY0:     psc=24-1, arr=10-1 → 72e6/(24×10) = 300kHz；100 点/周期 → 3kHz 正弦
KEY1:     psc=24-1, arr=10-1 → 300kHz；10 点/周期 → 30kHz 正弦
```

对应地，DMA 通道也必须"知道自己是第几个数"：`CIRC = 1`（循环）+ `MINC = 1`（存储器地址递增）+ `PINC = 0`（外设地址固定在 `&DAC1->DHR12R1`）+ `DIR = 1`（存储器→外设）+ 两侧数据宽度都 16 位。F103 的固定映射：**DAC 通道 1 → DMA2_Channel3，DAC 通道 2 → DMA2_Channel4**（代码里就按 outx 二选一）。

### 2.6 用 ADC 验证 DAC（18-1 / 18-3）

DAC 输出脚和 ADC 输入脚都用跳线帽连起来，程序一边写 DHR、一边用 ADC 读回真实电压，两者对比即可确认 DAC 是否真的在工作。本工程读回用的是 **ADC2 的通道 14（PC4）**（`adc2_get_result_average(ADC2_CHY, ...)`，见 [adc2.h](实验18-1%20DAC输出实验/Drivers/BSP/ADC/adc2.h)）。

> 补充：STM32F103 的 ADC1/ADC2 还有一个**内部通道 19**，芯片内部就连到 DAC_OUT1，理论上不用外接跳线帽也能读；本工程没有走这条路，而是外部回环（好处是顺带验证了引脚输出是否正常）。

## 三、硬件资源与接线

### 3.1 公共部分

| 资源 | 引脚 | 代码中的宏/位置 | 说明 |
| --- | --- | --- | --- |
| DAC1 通道 1 输出 | **PA4** | `dac_init(1)`，寄存器 `DAC1->DHR12R1` | 芯片固定脚；本工程关闭输出缓冲（BOFF1=1） |
| DAC1 通道 2 输出 | PA5 | `dac_init(2)` / `DAC1->DHR12R2` | 本实验未使用 |
| LED0（红灯） | PA8 | `LED0_GPIO_PORT/PIN`，[led.h](实验18-1%20DAC输出实验/Drivers/BSP/LED/led.h) | `LED0_TOGGLE()` 表示程序在跑 |
| KEY0 | PC5 | `KEY0_GPIO_PIN = SYS_GPIO_PIN5`，`KEY0_PRES = 1` | 上拉输入，按下为 0 |
| KEY1 | PA15 | `KEY1_GPIO_PIN = SYS_GPIO_PIN15`，`KEY1_PRES = 2` | 上拉输入；此脚与 JTAG 的 JTDI 复用，不要改动 SWJ 映射 |
| KEY_UP (WK_UP) | PA0 | `WKUP_GPIO_PIN = SYS_GPIO_PIN0`，`WKUP_PRES = 3` | 下拉输入，按下为 1 |
| 串口1 + USMART | PA9/PA10 | `usart_init(72, 115200)`、`usmart_dev.init(...)` | 115200 8N1；USART1 排针需把 PA9/PA10 跳到 RXD/TXD |
| TFTLCD 模块 | 数据 PB0～PB15，RD=PC6、WR=PC7、RS=PC8、CS=PC9、BL=PC10 | [lcd.h](实验18-1%20DAC输出实验/Drivers/BSP/LCD/lcd.h) | 8080 并口 MCU 屏，不支持 RGB 屏 |
| 时钟 | — | `sys_stm32_clock_init(9)` | 72MHz：APB2=72MHz、APB1=36MHz（定时器时钟 72MHz） |

### 3.2 实验18-1 DAC输出实验

| 资源 | 引脚 | 代码位置 | 说明 |
| --- | --- | --- | --- |
| DAC1_OUT1 | PA4 | `dac_init(1)` | 需要**用跳线帽把板上 DAC 插针与 ADC 插针短接**，ADC 才读得到 |
| ADC2 通道 14 | PC4 | `ADC2_CHY = 14`、`ADC2_CHY_GPIO_PORT = GPIOC` | 回读 DAC 电压 |

### 3.3 实验18-2 DAC输出三角波实验

只用 DAC1_OUT1（PA4）。**必须用示波器**：探头接 PA4（板上 DAC 插针），接地夹接 GND，才能看到波形；工程里虽然 `main.c` 包含了 `adc2.h`，但 Keil 工程未编译 `adc2.c`，本实验不回读电压。

### 3.4 实验18-3 DAC输出正弦波实验

| 资源 | 引脚 | 代码位置 | 说明 |
| --- | --- | --- | --- |
| DAC1_OUT1 | PA4 | `dac_dma_wave_init(1, (uint32_t)&DAC1->DHR12R1, (uint32_t)g_dac_sin_buf)` | 建议同样接示波器观察；DAC 与 ADC 插针用跳线帽短接 |
| ADC2 通道 14 | PC4 | `adc2_init()` + `adc2_get_result_average(ADC2_CHY, 10)` | 读回电压（读到的是快速变化正弦的某个平均值，不必与 DAC 显示值一致） |
| DMA2 通道 3 | 固定映射 | `dma_basic_config()` + `dma_enable(DMA2_Channel3, cndtr)` | 循环搬运正弦表 |
| TIM7 | 片内基本定时器 | `RCC->APB1ENR \|= 1 << 5`、`TIM7->CR2 \|= 2 << 4` | 只做 DAC 触发节拍，无输出引脚 |

## 四、代码结构与调用流程

```text
实验18 DAC实验/实验18-3 DAC输出正弦波实验/
├── Drivers/BSP/
│   ├── DAC/dac.c|dac.h      # dac_init / dac_set_voltage / dac_triangular_wave / dac_dma_wave_init / dac_dma_wave_enable
│   ├── ADC/adc2.c|adc2.h    # ADC2（PC4，通道14）查询式回读，验证 DAC 输出
│   ├── DMA/dma.c|dma.h      # dma_basic_config(开时钟+CPAR/CMAR) / dma_enable(装 CNDTR 并启动)
│   ├── KEY/key.c|key.h      # KEY0/KEY1/WK_UP 扫描（mode=0 不支持连按）
│   ├── LED/led.c|led.h      # LED0 心跳
│   └── LCD/lcd.c|lcd.h      # 8080 屏显示
├── Middlewares/USMART/      # 串口命令行调用函数（dac_set_voltage / dac_dma_sin_set）
├── User/main.c
└── Projects/MDK-ARM/atk_f103.uvprojx
```

各子实验 `main()` 流程：

- **18-1**：`sys_stm32_clock_init(9)` → `delay_init(72)` → `usart_init(72,115200)` → `usmart_dev.init(240)` → `led_init` → `lcd_init` → `key_init` → `adc2_init` → `dac_init(1)` → 显示提示 → 循环：`key_scan(0)`；WK_UP 按下则 `dacval += 200` 写 `DAC1->DHR12R1`，KEY1 按下则 `dacval -= 200`；每 10 次（或有按键）显示 DAC 寄存器值、DAC 电压、ADC 回读电压；`LED0_TOGGLE()`。
- **18-2**：初始化同上（多一步 `usmart_dev.init(72)`），没有 `adc2_init()` → 循环：`KEY0` → `dac_triangular_wave(4095, 5, 2000, 100)`；`KEY1` → `dac_triangular_wave(4095, 500, 20, 100)`；调用期间阻塞约 1s，屏幕上在调用前后切换 `DAC Wave1/Wave2/None` 提示。
- **18-3**：`... → usmart_dev.init(72) → led/lcd/key → adc2_init() → adc2_channel_set(ADC2_CHY, 0) → dac_dma_wave_init(1, &DAC1->DHR12R1, g_dac_sin_buf) → dac_creat_sin_buf(2048, 100) → dac_dma_wave_enable(1, 100, 10-1, 72-1)`（上电即输出 1kHz 正弦）→ 循环：KEY0 换 3kHz（100 点）、KEY1 换 30kHz（10 点），每次都重新生成表并重装 DMA/TIM7；随后读 `DAC1->DHR12R1` 与 ADC2 平均值显示。

## 五、核心代码解析

### 5.1 18-1：最简 DAC 输出（写 DHR 即改变电压）

`main.c` 里按键调压的核心只有两行：

```c
        if (key == WKUP_PRES)
        {
            if (dacval < 4000)dacval += 200;

            DAC1->DHR12R1 = dacval; /* 输出增大200 */
        }
```

因为 `TEN1 = 0`（不使能触发），写入 `DHR12R1` 后数据会**自动传送到 DOR1**（经过一个 APB1 时钟），所以电压立即跟随；`dacval` 每次 ±200，即 ±200×0.806mV ≈ ±161mV，上限 4000（≈3.22V）。

回读显示部分：

```c
            adcx = DAC1->DHR12R1;   /* 获取DAC1_OUT1的输出状态 */
            lcd_show_xnum(94, 130, adcx, 4, 16, 0, BLUE);       /* 显示DAC寄存器值 */
            
            temp = (float)adcx * (3.3 / 4096);                  /* 得到DAC电压值 */
```

`DAC1->DHR12R1` 是可读回的（读回来的就是写入的 12 位值），所以这里直接读它当"设定值"；`3.3 / 4096` 就是 2.1 的公式。

`dac_set_voltage()` 则提供"按毫伏设置"的便捷入口，也挂在 USMART 上可用串口调用：

```c
void dac_set_voltage(uint8_t outx, uint16_t vol)
{
    double temp = vol;
    temp /= 1000;
    temp = temp * 4096 / 3.3;

    if (temp >= 4096) temp = 4095;  /* 如果值大于等于4096, 则取4095 */
```

`vol` 单位是 mV（0～3300）：先化成 V，再乘 `4096 / 3.3` 换算成 DHR 码值，最后防溢出钳到 4095。串口里输入 `dac_set_voltage(1,3000)` 就让 PA4 输出 3.0V。

### 5.2 18-2：软件合成三角波的三个参数

```c
void dac_triangular_wave(uint16_t maxval, uint16_t dt, uint16_t samples, uint16_t n)
{
    uint16_t i, j;
    float incval;                           /* 递增量 */
    
    if((maxval + 1) <= samples)return ;     /* 数据不合法 */
        
    incval = (maxval + 1) / (samples / 2);  /* 计算递增量 */
```

- `maxval`：期望波峰码值；`samples`：一个周期的点数（上升 `samples/2` + 下降 `samples/2`）；`dt`：每点延时 µs，决定频率；`n`：输出几个完整波形。
- `if ((maxval + 1) <= samples) return;` 这条保护是因为"每步至少走 1 个码值"：如果点数比幅度还多，整数除法会让 `incval = 0`，波形就再也不动了。
- 上升/下降都靠 `DAC1->DHR12R1 += incval` 的**读改写**实现，配合 `delay_us(dt)` 控制节拍；由于 `dac_init()` 里 `TEN1=0`，每次写 DHR 都立即更新输出。
- 主程序调用与注释（`main.c`）：

```c
        if (key == KEY0_PRES)       /* 高采样率 , 约1Khz波形 */
        {
            lcd_show_string(30, 130, 200, 16, 16, "DAC Wave1 ", BLUE); 
            dac_triangular_wave(4095, 5, 2000, 100);    /* 幅值4095, 采样点间隔5us, 2000个采样点, 100个波形 */
```

按 2.4 的公式，这里的实际频率约 **100Hz**（不是注释写的 1kHz），"高采样率"指的是每周期点数多、波形平滑。

### 5.3 18-3：DMA + TIM7 触发配置

```c
    RCC->APB1ENR |= 1 << 5;             /* TIM7时钟使能 */ 
    
    dac_init(outx);                     /* 初始化DAC 对应通道 */
    /* 有差异部分 单独设置 */
    DAC1->CR |= 1 << (12 + offset);     /* DMAEN1/2 = 1,通道1/2 DMA使能 */
    DAC1->CR |= 2 << (3 + offset);      /* TSEL1/2[2:0] = 2, TIM7_TRGO触发 */
    DAC1->CR |= 1 << (2 + offset);      /* TEN1/2 = 1,触发使能 */
```

这三位的顺序无所谓（此时 `EN1` 已在 `dac_init()` 里置起，但还没有触发到来），含义是：*"通道 1 的更新只能由 TIM7 的 TRGO 驱动，并且每次更新后向 DMA 要下一个数"*。

DMA 侧与 ADC 实验的区别就在于方向与循环：

```c
    dmax_chy->CCR |= 1 << 4;            /* DIR = 1 , 存储器到外设模式 */
    dmax_chy->CCR |= 1 << 5;            /* CIRC = 1, 循环模式 */
    dmax_chy->CCR |= 0 << 6;            /* PINC = 0, 外设地址非增量模式 */
    dmax_chy->CCR |= 1 << 7;            /* MINC = 1, 存储器增量模式 */
```

节拍器与启动：

```c
void dac_dma_wave_enable(uint8_t outx, uint16_t cndtr, uint16_t arr, uint16_t psc)
{
    TIM7->CR1 &= ~(1 << 0);                 /* 关闭定时器TIMX,停止触发 */
    if (outx == 1)
    {
        dma_enable(DMA2_Channel3, cndtr);   /* 重新启动DMA2_Channel3传输 */
    }
    else
    {
        dma_enable(DMA2_Channel4, cndtr);   /* 重新启动DMA2_Channel4传输 */
    }

    TIM7->ARR = arr;                        /* 设定计数器自动重装值 */
    TIM7->PSC = psc;                        /* 设置预分频器  */
    TIM7->CR2 |= 2 << 4;                    /* 定时器7的更新事件触发DAC转换 */ 
    TIM7->CR1 |= 1 << 0;                    /* 使能定时器TIMX */ 
}
```

`TIM7->CR2` 的 `MMS[6:4] = 010` 表示"把更新事件(UEV)作为 TRGO 输出"，TIM7 是基本定时器，没有 PWM 脚，正是专门用来给 DAC 当节拍的。切换频率的调用即：

```c
            dac_creat_sin_buf(2048, 100);
            dac_dma_wave_enable(1, 100, 10 - 1, 24 - 1);    /* 300Khz触发频率, 100个点, 得到最高3KHz的正弦波. */
```

### 5.4 18-3：正弦表的生成（查表法）

```c
void dac_creat_sin_buf(uint16_t maxval, uint16_t samples)
{
    uint8_t i;
    float inc = (2 * 3.1415962) / samples;      /* 计算增量（一个周期DAC_SIN_BUF个点）*/
    float outdata = 0;

    for (i = 0; i < samples; i++)
    {
        outdata = maxval * (1 + sin(inc * i));  /* 计算以dots个点为周期的每个点的值，放大maxval倍，并偏移到正数区域 */
        if (outdata > 4095) outdata = 4095;     /* 上限限定 */ 
        g_dac_sin_buf[i] = outdata;
    }
}
```

- `sin()` 输出 −1～+1，`maxval * (1 + sin)` 就把它搬到了 0～2×maxval 的正数区（DAC 不能输出负电压）。取 `maxval = 2048` 时正好铺满 0～4096，再被钳到 4095，即满幅 0～3.3V 正弦。
- 表放在 `uint16_t g_dac_sin_buf[4096]`（占 8KB RAM，实际只填前 `samples` 个；STM32F103RC 有 48KB SRAM，够用），DMA 的 `CNDTR = samples`，循环模式下一遍一遍搬同一段表。
- 两点使用限制（代码本身的特性，改进时要小心）：循环变量是 `uint8_t i`，所以 `samples` 最大只能 255；`dac_dma_sin_set(arr, psc)` 把 `cndtr` 写死为 100，因此通过 USMART 改频率时点数必须还是 100。

## 六、编译、下载与运行

1. 打开对应子实验工程：
   - `实验18-1 DAC输出实验/Projects/MDK-ARM/atk_f103.uvprojx`
   - `实验18-2 DAC输出三角波实验/Projects/MDK-ARM/atk_f103.uvprojx`
   - `实验18-3 DAC输出正弦波实验/Projects/MDK-ARM/atk_f103.uvprojx`
2. 器件 **STM32F103RC**，启动文件 `startup_stm32f103xe.s`，包含路径 `Drivers\CMSIS\...`、`Drivers\CMSIS\Include`、`Drivers`、`User`、`Middlewares`；工程未勾选 MicroLIB（`Options → Target` 里用的是 ARM 标准 C 库，18-3 的 `math.h`/`sin()` 与 USMART 的 `printf` 都依赖它）。
3. 编译产物在子实验 `Output/` 下（`atk_f103.axf/bin`）；下载用 ST-Link/J-Link（SWD）或串口 ISP。
4. 需要用 USMART 时：USB 线接 USB_UART，串口助手（XCOM/SSCOM）波特率 **115200**，命令以回车换行结束；输入 `help` 可看可调用函数列表。18-1 注册了 `dac_set_voltage(outx,vol)`，18-2 注册了 `dac_triangular_wave(maxval,dt,samples,n)`，18-3 注册了 `dac_set_voltage` 与 `dac_dma_sin_set(arr,psc)`。

## 七、实验现象

- **18-1**：LCD 显示 `DAC TEST`、`WK_UP:+  KEY1:-` 以及 `DAC VAL / DAC VOL / ADC VOL` 三行。
  1. 先用跳线帽把板上 **DAC 插针与 ADC 插针短接**；
  2. 每按一次 WK_UP，`DAC VAL` 增 200、`DAC VOL` 增约 0.16V，`ADC VOL` 跟着上升并基本一致（差几十毫伏属正常误差）；
  3. 每按一次 KEY1 反之下降，最多到 4000（约 3.22V）、最小到 0；
  4. 串口里执行 `dac_set_voltage(1,3000)`，`ADC VOL` 应显示约 3.000V；
  5. LED0 每次刷新时翻转。
- **18-2**：LCD 显示 `DAC Triangular WAVE TEST` 与 `KEY0:Wave1 KEY1:Wave2`。示波器探头接 PA4（DAC 插针）、地夹接 GND：
  1. 按 KEY0，约 1 秒的阻塞输出期间屏幕显示 `DAC Wave1`，示波器看到 **约 100Hz、边很斜直、台阶细密** 的三角波（2000 点，幅度 0～约 3.22V）；
  2. 按 KEY1，看到 **同频率但明显"阶梯状"** 的三角波（20 点，幅度 0～约 3.29V）；
  3. 输出结束后屏幕恢复 `DAC None`；也可用串口 `dac_triangular_wave(4095,5,200,20)` 自定义。
- **18-3**：LCD 显示 `DAC DMA Sine WAVE TEST`、`KEY0:3Khz KEY1:30Khz` 与 `DAC VAL / DAC VOL / ADC VOL`。上电即从 PA4 输出 **1kHz 正弦**，示波器可见完整正弦（100 点，0～3.3V 满幅）；
  1. 按 KEY0 → **3kHz** 正弦（还是 100 点）；
  2. 按 KEY1 → **30kHz** 正弦（只有 10 点，波形看起来更接近"阶梯正弦"，且幅度略被 DAC 建立时间限制）；
  3. `DAC VAL/DAC VOL` 显示的是此刻 DMA 刚写进 DHR 的那一个点，正弦跳动很快，数值乱跳是正常现象；`ADC VOL` 是 ADC2 对变化中的电压做的 10 次平均，仅作参考；
  4. 串口执行 `dac_dma_sin_set(9,23)` 可把频率调回 3kHz，`dac_dma_sin_set(9,71)` 调回 1kHz。
- 三个子实验的 LED0 都会持续闪烁，表示主循环未被卡死（18-2 在输出波形的 1 秒内不闪）。

## 八、注意事项

1. **DAC 输出脚只有 PA4/PA5**，且必须 `ENx = 1` 才有输出；只配 GPIO、忘写 `DAC1->CR |= 1 << 0` 会量到 0V。
2. **本工程关闭了输出缓冲（BOFF1=1）**：输出阻抗高，只能接高阻负载。接低阻负载（如直接接喇叭、LED、几百Ω电阻）会明显拉偏电压甚至带不动。要用它驱动负载请改成 `DAC1->CR &= ~(1 << (1 + offset))`（开启缓冲）或加一级运放跟随器。
3. **代码与官方说明不一致的地方（以代码为准）**：
   - `dac_init()` 里配置 DAC 引脚用的是 `sys_gpio_set(GPIOA, 1 << (4 + outx), ...)`，`outx = 1` 时算出来是 bit5，也就是**配置的是 PA5**；而同一行的注释与官方说明都是"PA4 = DAC1_OUT1"。要严格把输出脚设成模拟输入应写成 `1 << (3 + outx)`。功能上仍可输出：DAC 通道使能后 PA4 由内部模拟开关接管，而 PA4 复位默认是浮空输入，也能工作。
   - 18-1 的 `readme.txt` 写 "ADC2：通道14 - **PC14**"，代码是 `GPIOC / SYS_GPIO_PIN4` = **PC4**（18-3 的 readme 写的 PC4 才与代码一致）；同一份 `readme.txt` 的实验现象里还写"通过 **ADC1 的通道 19** 采集 DAC 的输出电压"，实际代码用的是 **ADC2 通道 14 外部回环**，并没有用片内 DAC_OUT 通道。
   - 18-1 `main.c` 注释"得到ADC2通道1的转换结果""adc是16bit的"均与代码不符：通道是 **14**，ADC 是 **12 位**（18-3 同位置注释写的 12bit 是对的）。
   - 18-2 `main.c` 两处"约1Khz波形"的注释，按 `dt × samples` 算实际约 **100Hz**；`dac_triangular_wave()` 函数头给出的 `f ≈ 1000/(dt×samples) KHz` 才是准的。18-3 `main.c` 中 KEY0 分支注释"约1Khz"同样应为 3kHz。
   - 18-1 的 `usmart_dev.init(240)` 与 18-2/18-3 的 `init(72)` 不一致：该参数含义是"USMART 所用定时器（TIM4）的工作频率，单位 MHz"，本工程实际是 72。填 240 只是让扫描周期变成约 33ms，功能仍可用。
   - `adc2_channel_set(ADC2_CHY, 0)` 的 `stime` 参数在驱动里被忽略（内部固定写 7 = 239.5 周期），和实验16 是同一个坑。
   - 18-1/18-3 的 `dac_triangular_wave`/`dac_dma_wave_*` 函数在 `dac.c` 里都有实现（V1.2），但 18-1 的 `dac.h` 只声明了 `dac_init`/`dac_set_voltage`，属于头文件未同步；不影响编译。
   - 18-2 的 `main.c` 包含 `./BSP/ADC/adc2.h` 但工程没有把 `adc2.c` 加入编译，属遗留 include。
4. **18-2 的波形输出是阻塞的**：`dac_triangular_wave(..., n)` 期间 CPU 一直在循环里，按键、LCD 都不响应；想"输出波形的同时干别的"必须改用 18-3 的 DMA 方案。
5. **频率上限受 DAC 建立时间约束**：`dac_init` 按 tSETTLING=3µs 估算约 333kHz/点，`dac_dma_wave_init` 按 ≤4µs 估算约 250kHz/点；18-3 的 300kHz 触发已在此边界，30kHz 正弦（10 点）会看到顶部不够圆、幅度略低。想更准就增加每周期点数并降低触发频率。
6. **示波器探头的地夹必须接板子 GND**，否则波形会漂、甚至引入短路；量程建议 1V/div、时基 2ms/div（100Hz 三角波）或 100µs/div（1kHz 正弦）。
7. 若 LCD 全黑/乱码：确认是 MCU 8080 屏、插反与否、背光 `LCD_BL`(PC10)；若串口无 USMART 响应：检查 PA9/PA10 跳线帽与波特率 115200。

## 九、总结与延伸

**本实验掌握的要点**：

1. DAC 数据链：`DHR12R1`（12 位右对齐，本工程位位置为 bit11:0）→ 触发搬运 → `DOR1` → 输出缓冲 → PA4；电压 = 码值 × 3.3 / 4096。
2. `DAC->CR` 的关键位：`EN`(0)、`BOFF`(1，写 1 是关缓冲)、`TEN`(2)、`TSEL`(5:3)、`WAVE`(7:6)、`MAMP`(11:8)、`DMAEN`(12)，通道 2 全部 +16。
3. 软件合成波形靠"写 DHR + delay_us"，频率 = 1/(dt×samples)，会占住 CPU；硬件三角波发生器（`WAVE=10` + `MAMP`）能做到零 CPU，本实验没用。
4. 任意波形的标准做法：正弦表 + **TIM 的 TRGO 做节拍**（TIM7、`CR2.MMS=010`）+ **DMA2_Channel3/4 循环模式**搬运；`fwave = ftrgo / 每周期点数`。
5. DAC 输出速度受 tSETTLING（3～4µs）限制；关闭缓冲换速度但只能带高阻负载；用 ADC 回环是验证 DAC 的实用手段。

**延伸小练习**：

1. 把 `dac_init()` 的 `DAC1->CR |= 1 << (1 + offset)` 改成清 0（开启输出缓冲），再对比 18-3 的波形幅度与建立情况，并接一个低阻负载体会差异。
2. 用硬件波形发生器做一个"真·三角波"：在 `dac_init()` 里把 `WAVE1` 写成 `2`（bit7:6=10）、`MAMP1` 写成 `0xF`、`TSEL1 = 2`（TIM7_TRGO）、`TEN1 = 1`，再用 `dac_dma_wave_enable()` 那套 TIM7 代码给节拍，即可不用 DMA 表输出三角波。
3. 修改 `dac_creat_sin_buf()`：把 `uint8_t i` 改成 `uint16_t i`，生成 512 点表，配合 `psc/arr` 输出更低失真的正弦；再试试叠加直流偏置（如 `maxval*(1+sin)*0.5 + 1024`）输出 1V～2V 的"小正弦"。
4. 把 ADC2 换成 DAC 内部回读路线（ADC1 通道 19 = DAC_OUT），验证不需要跳线帽也能读回电压。

**衔接**：DAC+ADC 打通后，实验20 SPI、实验19 IIC 会教你用总线去驱动外部 DAC/ADC/EEPROM；实验41 综合测试实验把本实验的波形输出与 LCD、按键、串口命令整合成一个"迷你信号发生器"。
