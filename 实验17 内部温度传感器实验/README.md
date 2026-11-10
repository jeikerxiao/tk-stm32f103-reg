# 实验17 内部温度传感器实验

> 一句话概括：STM32 芯片内部自带一个温度传感器，它接到 ADC1 的第 16 通道；本实验用 ADC 读它的电压、按公式换算成温度，再把结果显示在 TFTLCD 屏上。

## 一、实验简介

- **实验目的**（官方 `readme.txt`）：学习使用 STM32 的 ADC 功能采样 STM32 自身温度。
  通俗讲：ADC 就是"把连续变化的电压翻译成 0~4095 的数字"的翻译官。芯片里藏了一个会随温度改变输出电压的传感器，
  把这个电压翻译出来、再套一个公式，就读出了片内温度——不用任何外接传感器。
- **在学习路线中的位置**：前置实验是「实验16 ADC 实验」（ADC 单通道/多通道/DMA 采集），本实验是其"内部通道 + 换算公式"的应用；
  它还会复用「实验2 LCD」把结果显示出来。
- **开发板与芯片**：正点原子 MiniSTM32 V4，主芯片 STM32F103RC。**寄存器版**，直接配 `ADC1->CR1/CR2/SMPR/SQR/DR/SR`。

> 小概念：`ADC 通道`就是"从哪一路取电压来测"。通道 0~17 里大部分接外部引脚，而通道 16（温度）和通道 17（内部基准 Vrefint）是**片内**的，不占引脚。

## 二、实验原理

### 2.1 内部温度传感器与 ADC 通道 16 的关系

- STM32F103 在芯片内部集成了一个温度传感器，它把"当前结温"变成一个模拟电压，**只连接到 ADC1 的通道 16**。
- 也就是说：**只有 ADC1 能测内部温度**，ADC2/ADC3 没有这条路；代码注释也明确写"内部温度传感器只连接在 ADC1 的通道 16 上"。
- 要启用这条内部通路，需要把 `ADC1->CR2` 的 **TSVREFE 位（bit23）** 置 1——它同时打开温度传感器和内部基准 Vrefint 到 ADC 的开关。

> 注意：官方 `readme.txt` 的"实验现象"里写"通过 ADC1 的**通道 1**读取温度传感器"，这与代码不符，**以代码为准**：
> [adc.h](Drivers/BSP/ADC/adc.h) 里 `ADC_TEMPSENSOR_CHX = 16`，用的是**通道 16**（内部温度传感器物理上就挂在 16，不可能是 1）。

### 2.2 电压与温度的换算关系

STM32F103 温度传感器近似线性，数据手册给出两个典型系数（本工程代码里就用它们）：

- 在 25℃ 时，传感器电压约 **1.43V**（代码里的 `1.43`）。
- 温度每升高 1℃，电压下降约 **4.3mV**（代码里的 `0.0043`，即"负斜率"）。

反推温度公式：

```text
温度(℃) = (1.43 - Vs) / 0.0043 + 25
```

其中 `Vs` 是传感器电压。而 ADC 读到的数字量 `adcx`（0~4095，12 位）换成电压：

```text
Vs = adcx * 3.3 / 4096       （参考电压取 3.3V，满量程 4096）
```

### 2.3 代入本工程系数算一遍（工作示例）

假设某次 ADC 平均读数 `adcx = 1800`：

1. `Vs = 1800 * 3.3 / 4096 = 1.4502 V`
2. `温度 = (1.43 - 1.4502) / 0.0043 + 25 = (-0.0202)/0.0043 + 25 = -4.70 + 25 ≈ 20.30℃`
3. 代码把温度 **扩大 100 倍** 存成整数：`result = 20.30 * 100 = 2030`
4. 显示时：整数部分 `2030/100 = 20`，小数部分 `2030%100 = 30` → 屏幕上显示 **`20.30C`**。

反过来验证 25℃：`Vs=1.43V → adcx≈1.43*4096/3.3≈1776`，代回公式得约 25℃，自洽。

### 2.4 ADC 时钟与采样/转换时间（顺带理解）

- ADC 时钟来自 APB2 的 PCLK2=72MHz，F103 的 ADC 最高时钟 14MHz，代码取 **6 分频**：`RCC->CFGR` 的 `ADCPRE[1:0]`(bit[15:14]) 写 2 → 72/6 = **12MHz**。
- 采样时间被设为最长档 7 → **239.5 个 ADC 周期**；转换固定还要 12.5 个周期。
- 单次转换时间 = (239.5 + 12.5) / 12MHz = 252 / 12 = **21us**（见 [adc.c](Drivers/BSP/ADC/adc.c) 注释）。

## 三、硬件资源与接线

| 资源 | 引脚/位置 | 代码中的宏 | 说明 |
| --- | --- | --- | --- |
| 内部温度传感器 | ADC1 通道 16（片内） | `ADC_TEMPSENSOR_CHX`，[adc.h](Drivers/BSP/ADC/adc.h) | 无外部引脚，靠 `CR2.TSVREFE` 使能 |
| ADC1 | — | `ADC_ADCX = ADC1`，[adc.h](Drivers/BSP/ADC/adc.h) | 时钟使能 `RCC->APB2ENR` bit9 |
| TFTLCD（显示屏） | MCU 并口(16 位 8080) | [lcd.c](Drivers/BSP/LCD/lcd.c)、[lcd.h](Drivers/BSP/LCD/lcd.h) | 正点原子 2.8/3.5/4.3/7/10 寸 MCU 屏 |
| LED0（红灯） | PA8 | `LED0_GPIO_PIN`，[led.h](Drivers/BSP/LED/led.h) | 主循环 `LED0_TOGGLE()` 每 250ms 闪 |
| LED1（绿灯） | PD2 | `LED1_GPIO_PIN`，[led.h](Drivers/BSP/LED/led.h) | 本实验未使用 |
| 串口1（备用） | PA9/PA10 → 板载 CH340 | [usart.c](Drivers/SYSTEM/usart/usart.c) | 初始化了但温度输出走 LCD |

- **外接模块**：需插一块正点原子 **TFTLCD（MCU 屏，16 位 8080 并口）** 到屏幕排针接口，注意插对方向/引脚。
- **USB 转串口**：官方注意事项提到用 USB 线连 `USB_UART`、找到 CH340 口；但本 `main.c` 的温度读数显示在 **LCD**，串口仅被初始化、并未用于输出温度（USMART 头虽被 `#include`，但主程序未启动 usmart 命令解析）。
- **时钟树含义**：`sys_stm32_clock_init(9)` → 系统 72MHz；ADC 用其中 12MHz，`delay`/`usart` 亦基于该主频。

## 四、代码结构与调用流程

```text
实验17 内部温度传感器实验/
├── User/main.c                 主程序：初始化 + 循环读温度并显示到 LCD
├── Drivers/BSP/ADC/adc.c/.h    ADC 驱动：adc_init / adc_get_result / adc_temperature_init / adc_get_temperature
├── Drivers/BSP/DMA/dma.c/.h    DMA 底层（供 ADC 的 DMA 采集函数用；温度路径未用到）
├── Drivers/BSP/LCD/lcd.c/.h    LCD 驱动：lcd_show_string / lcd_show_xnum 显示
├── Drivers/BSP/LED/led.c/.h    LED 驱动
├── Middlewares/USMART/         串口命令组件（工程保留，本 demo 未启用）
└── Drivers/SYSTEM/{sys,delay,usart}  时钟/延时/串口公共底层
```

各文件职责：`adc.c` 是核心，负责 ADC 配置、单通道查询式采集和温度换算；`lcd.c` 负责把数字显示出来；`dma.c`、`USMART` 是从 ADC/串口大工程继承来的，温度实验的实际数据流并不经过它们。

**`main()` 流程**（见 [User/main.c](User/main.c)）：

1. `sys_stm32_clock_init(9)` / `delay_init(72)` / `usart_init(72,115200)` / `led_init()`：常规初始化。
2. `lcd_init()`：初始化液晶屏。
3. `adc_temperature_init()`：初始化 ADC 并使能内部温度传感器。
4. 在 LCD 上打印标题与一行 `TEMPERATE: 00.00C` 模板。
5. `while(1)`：
   - `temp = adc_get_temperature();`（扩大 100 倍的带符号温度）
   - 若 `temp<0`：显示负号并取绝对值；否则显示空格。
   - `lcd_show_xnum(..., temp/100, 2, ...)` 显示整数位；`lcd_show_xnum(..., temp%100, 2, ...)` 显示小数位。
   - `LED0_TOGGLE(); delay_ms(250);` 每 250ms 刷新一次。

流程小图：`ADC 读通道16 → 平均20次 → 转电压 → 套公式算温度 → ×100 取整 → LCD 分整数/小数显示`。

## 五、核心代码解析

### 5.1 启用内部温度传感器 —— `adc_temperature_init`

摘自 [Drivers/BSP/ADC/adc.c](Drivers/BSP/ADC/adc.c)：

```c
void adc_temperature_init(void)
{
    adc_init();                 /* 先初始化ADC */
    ADC_ADCX->CR2 |= 1 << 23;   /* TSVREFE = 1, 启用内部温度传感器和Vrefint */
}
```

- 先 `adc_init()` 把 ADC1 的时钟、模式、采样、校准配好（见 5.3）。
- 再写 `ADC1->CR2` bit23 = **TSVREFE=1**：这是关键一步，不置 1，通道 16 上根本没有传感器信号。

### 5.2 读原始值并换算成温度 —— `adc_get_temperature`

```c
short adc_get_temperature(void)
{
    uint32_t adcx;
    short result;
    double temperature;

    adcx = adc_get_result_average(ADC_TEMPSENSOR_CHX, 20);  /* 读取内部温度传感器通道,10次取平均 */
    temperature = (float)adcx * (3.3 / 4096);               /* 转化为电压值 */
    temperature = (1.43 - temperature) / 0.0043 + 25;       /* 计算温度 */
    result = temperature *= 100;                            /* 扩大100倍. */
    return result;

}
```

逐步解释：

1. `adc_get_result_average(ADC_TEMPSENSOR_CHX, 20)`：对**通道 16** 连续采 20 次求平均，抑制抖动。（注：源码注释写"10 次取平均"，但实参是 **20**，以代码为准；见注意事项。）
2. `adcx * (3.3/4096)`：把 12 位数字量换算成电压（参考电压 3.3V）。
3. `(1.43 - temperature)/0.0043 + 25`：套用 2.2 的公式得摄氏度。
4. `temperature *= 100` 再赋给 `short result`：扩大 100 倍，便于用整数拆分显示两位小数（分辨率 0.01℃）。

### 5.3 单次采集是怎么做的 —— `adc_get_result` / `adc_init` 关键位

`adc_get_result` 每次转换的核心几行（[adc.c](Drivers/BSP/ADC/adc.c)）：

```c
    ADC_ADCX->SQR3 &= ~(0X1F << 5 * 0); /* 规则序列1通道清零 */
    ADC_ADCX->SQR3 |= ch << (5 * 0);    /* 规则序列1 通道 = ch */
    ADC_ADCX->CR2 |= 1 << 22;           /* 启动规则转换通道 */
    while (!(ADC_ADCX->SR & 1 << 1));   /* 等待转换结束 */
    return ADC_ADCX->DR;                /* 返回adc值 */
```

- 把要测的通道号 `ch`（这里是 16）填进规则序列 1（`SQR3` 的 `SQ1[4:0]`）。
- `CR2 |= 1<<22`（SWSTART）软件触发一次转换。
- `while (!(SR & 1<<1))` 轮询等待 **EOC（转换结束标志，SR bit1）**。
- 从数据寄存器 `DR` 读回 12 位结果。

`adc_init()` 里与"能正常测内部温度"最相关的配置节选：

```c
    RCC->CFGR &= ~(3 << 14);        /* ADCPRE[1:0] ADC时钟预分频清零 */
    RCC->CFGR |= 2 << 14;           /* 设置ADC时钟预分频系数为 6, 即 PCLK2 / 6 = 12Mhz */
    ...
    ADC_ADCX->CR2 |= 7 << 17;       /* 软件控制转换 */
    ADC_ADCX->CR2 |= 1 << 20;       /* 使用用外部触发(SWSTART)!!! 必须使用一个事件来触发 */
    ...
    ADC_ADCX->CR2 |= 1 << 0;        /* 开启AD转换器 */
```

即：ADC 时钟 12MHz、单次转换、由 SWSTART 软件触发、开 ADON 并做上电校准。

> 说明：`adc_init()` 里还会顺手把外部通道引脚 PC4（宏 `ADC_ADCX_CHY=14` 对应脚）配成模拟输入，
> 那是通用 ADC 驱动的一部分；本实验从**片内通道 16**取信号，并不用到 PC4。

### 5.4 主循环显示整数/小数

[User/main.c](User/main.c) 关键片段：

```c
        temp = adc_get_temperature();   /* 得到温度值 */

        if (temp < 0)
        {
            temp = -temp;
            lcd_show_string(30 + 10 * 8, 120, 16, 16, 16, "-", BLUE);   /* 显示负号 */
        }
        ...
        lcd_show_xnum(30 + 11 * 8, 120, temp / 100, 2, 16, 0, BLUE);    /* 显示整数部分 */
        lcd_show_xnum(30 + 14 * 8, 120, temp % 100, 2, 16, 0X80, BLUE); /* 显示小数部分 */
```

因为 `temp` 已被扩大 100 倍，`temp/100` 就是摄氏整数部分，`temp%100` 就是两位小数。
`lcd_show_xnum` 的 `mode` 位 `0X80` 表示"高位用 0 填充"：小数部分传 `0X80` 才能保证显示成两位（如 `5` → `05`），
整数部分传 `0`（不补前导 0，避免前面一串 0）。真正的**小数点 `.`** 不在 `lcd_show_xnum` 里画，
而是开机时那句模板 `TEMPERATE: 00.00C` 预先写死在屏上，之后每 250ms 只是把数字盖到对应位置。

## 六、编译、下载与运行

1. Keil MDK 打开 `Projects/MDK-ARM/atk_f103.uvprojx`，器件 `STM32F103RC`；工程含 ADC/LCD/DMA/USMART 等源，路径已配好。
2. Build 生成 `.axf`/`.hex`，用 ST-LINK/J-LINK 下载。
3. **先插好 TFTLCD（MCU 屏）** 再上电，否则屏无显示（甚至初始化异常）。
4. 若要用官方提到的串口注意事项（波特率 115200、XCOM/SSCOM），本 demo 的温度读数并不从串口输出，串口仅初始化备用。

## 七、实验现象

对照官方"实验现象"，实际会看到：

1. LCD 屏上方显示 `STM32` / `Temperature TEST` / `ATOM@ALIENTEK` 标题。
2. 屏上 `TEMPERATE:` 后随时间刷新出当前片内温度，如 `26.13C`（用手捂住芯片或用手指加热，数值会慢慢上升；对着吹气会下降）。
3. LED0（红灯）每约 250ms 翻转一次，作为"程序在跑"的心跳指示。
4. 温度显示跳变属正常（传感器精度有限、且换算系数是典型值），主要看趋势而不是绝对精确。

## 八、注意事项

- **通道号以代码为准**：官方 `readme.txt` 写"ADC1 的通道 1"，实际是**通道 16**（`ADC_TEMPSENSOR_CHX=16`）；温度传感器物理上只接 ADC1 通道 16，别的 ADC 测不到。
- **取平均次数注释与代码不符**：`adc_get_temperature` 的注释说"10 次取平均"，实参却是 `20`（`adc_get_result_average(..., 20)`）。功能不受影响，读代码时按 20 理解。
- **TSVREFE 必须置 1**：忘记 `ADC1->CR2 |= 1<<23`，通道 16 采到的将是不确定的 0/满值。
- **ADC 时钟别超 14MHz**：本工程 12MHz 是安全值；改主频/分频后重算。
- **换算系数是典型值**：1.43V@25℃、4.3mV/℃ 是手册典型参数，个体差异会让读数偏几℃，属正常；要更准可用芯片出厂校准值做两点校正。
- **LCD 必须插对**：MCU 屏插反/没插会黑屏或损坏；先确认接好再上电。
- **DMA/USMART 未被本实验数据流使用**：`adc.c` 里 `adc_dma_init/adc_nch_dma_init`、`Middlewares/USMART` 是继承代码，温度读数走的是查询式 `adc_get_result`，别误以为本实验在用 DMA。
- **源码为 GBK 编码**：正确选编码打开中文注释；本 README 为 UTF-8。

## 九、总结与延伸

**本实验要点：**

1. STM32 内部温度传感器接 **ADC1 通道 16**，需 `ADC1->CR2.TSVREFE`(bit23) 使能才能采。
2. 换算两步：`Vs=adcx*3.3/4096`，`温度=(1.43-Vs)/0.0043+25`，再 ×100 存整数便于显示。
3. ADC 采集三件套：配通道(`SQR3`)→软件触发(`CR2.SWSTART`)→等 `SR.EOC` 读 `DR`。
4. 采样取平均（本工程 20 次）能显著压住读数抖动。

**延伸小练习：**

1. 把 `adc_get_result_average(..., 20)` 改成 5 次，观察读数波动变大，理解"平均次数↔稳定度↔耗时"的折中。
2. 用两点校正提高精度：读取芯片 `ADC 校准地址` 里 30℃/110℃ 的出厂 VSENSE，替换掉固定系数 1.43/0.0043，重新推公式。
3. 把温度同时经串口(115200)每 500ms 打印一行，或改成超过某个阈值点亮 LED1 做"高温报警"。

**衔接**：本实验把「实验16 ADC」的采集能力用到内部通道上。往后可以：读通道 17 的内部基准 Vrefint 反推真实 VDD、
或用定时器触发 ADC 做定时采样，进一步体会"ADC + 定时器 + 中断"的组合。
