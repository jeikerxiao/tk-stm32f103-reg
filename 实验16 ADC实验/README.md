# 实验16 ADC实验

> 一句话概括：用 STM32F103 片内的 12 位 ADC 把引脚上的模拟电压"读成数字"，在 LCD 上显示 ADC 原始值与换算后的电压；四个子实验分别演示**查询式采集**、**DMA 单通道采集**、**DMA 多通道扫描**和**过采样把分辨率提高到 16 位**。

## 一、实验简介

- **实验目的**（取自各子实验官方 `readme.txt`）：
  - 16-1：学习 STM32 内部 ADC 的使用；
  - 16-2：学习 STM32 单通道 ADC 采集（DMA 读取）；
  - 16-3：学习 STM32 多通道 ADC 采集（DMA 读取）；
  - 16-4：学习 STM32 单通道 ADC 过采样的使用。
  通俗地说：CPU 只懂 0 和 1，而现实世界（电位器电压、光敏电阻、温度）是连续变化的模拟量。ADC（模数转换器）就是一把"电压尺子"，把 0～3.3V 的电压量成 0～4095 的数字。本实验教你怎么用寄存器把这把尺子开起来、读出来，并且用 DMA 让它"自己干活"、用多次采样让它"量得更细"。
- **在学习路线中的位置**：
  - 前置：实验1 跑马灯（GPIO 输出）、实验11 TFTLCD（MCU 屏）实验（显示结果）、实验15 DMA 实验（16-2/16-3/16-4 需要理解 DMA 搬运）。
  - 后续：实验17 内部温度传感器实验（就是读 ADC 的内部通道）、实验18 DAC 实验（逆过程：数字 → 电压）、实验41 综合测试实验。
- **开发板与芯片**：正点原子 MiniSTM32 V4 开发板，主芯片 STM32F103RC（高容量型，片内带 ADC1/ADC2/ADC3），主频 72MHz。

## 二、实验原理

### 2.1 先搞懂两个词：寄存器、ADC

- **寄存器**就是 CPU 内部（或外设里）一个可以读写的 32 位小格子，每个格子有若干根"线"（bit），配置外设 = 往这些格子里按位填 0/1。本工程不使用 HAL 库或标准库，所有配置都写成 `ADC1->CR2 |= 1 << 0;` 这样的直接位操作。
- **ADC 的工作过程**（12 位逐次逼近型）：先"采样"——用一个电容接上输入引脚，把外部电压"抓拍"下来；再"保持+转换"——内部用 12 位二进制逐步逼近这个电压，得到 0～4095 的数字，存进数据寄存器 `DR`。
- **一个通道 = 一个引脚**。STM32F103 的 ADC1/ADC2 有 18 个通道：通道 0～15 对应 PA0～PF15 等 16 个引脚，通道 16/17 是内部通道（温度传感器、内部参考电压 VREFINT）。本实验用到的是：
  - 通道 14 → **PC4**（单通道实验的输入脚）；
  - 通道 0～5 → **PA0～PA5**（多通道实验的 6 个输入脚）。

### 2.2 ADC 的"心跳"：时钟与采样时间

ADC 自己也要吃时钟。它挂在 **APB2（PCLK2 = 72MHz）** 上，还要再分频后才能用，手册规定 ADC 时钟最高 14MHz。本工程用 6 分频：

```text
ADCCLK = PCLK2 / ADCPRE = 72MHz / 6 = 12MHz      （一个 ADC 时钟周期 = 1/12MHz ≈ 83.3ns）
```

代码里通过 `RCC->CFGR` 的 **ADCPRE[15:14] = 2（即 10b）** 实现 6 分频（00=2 分频、01=4 分频、10=6 分频、11=8 分频）：

```c
RCC->CFGR &= ~(3 << 14);        /* ADCPRE[1:0] ADC时钟预分频清零 */
RCC->CFGR |= 2 << 14;           /* 设置ADC时钟预分频系数为 6, 即 PCLK2 / 6 = 12Mhz */
```

**采样时间**由 `ADC1->SMPR2`（通道 0～9）和 `ADC1->SMPR1`（通道 10～17）设置：每个通道占 3 个 bit（值 0～7），选得越大，电容充电越充分、精度越稳，但速度越慢。驱动函数 `adc_channel_set()` 的注释给出了对照表：

| SMPR 值 | 采样时间（ADC 时钟周期） | 本工程 |
| --- | --- | --- |
| 000 | 1.5 | 16-4 想用的（见 8.2 说明） |
| 001 | 7.5 |  |
| 010 | 13.5 |  |
| 011 | 28.5 |  |
| 100 | 41.5 |  |
| 101 | 55.5 |  |
| 110 | 71.5 |  |
| 111 | 239.5 | 16-1/16-2/16-3 实际使用的值 |

**一次转换的总时间 = 采样时间 + 12.5 个 ADC 周期**（12.5 是 12 位逐次逼近的固定开销）。代入本工程数值：

```text
采样 239.5 + 12.5 = 252 个 ADC 周期
252 / 12MHz = 21µs      —— 这与 adc.c 里函数头注释"转换时间 = 252 个ADC周期 = 21us"完全一致
```

由此可推出各子实验一轮采集耗时（见 2.5/2.6 节）：单通道 100 点 ≈ 2.1ms；6 通道 300 点 ≈ 6.3ms；过采样 2560 点 ≈ 53.8ms。

### 2.3 一次采集的完整流程（编号步骤）

以 [adc.c（16-1）](实验16-1%20单通道ADC采集/Drivers/BSP/ADC/adc.c) 的 `adc_init()` + `adc_get_result()` 为准：

1. **开时钟**：`RCC->APB2ENR |= 1 << 4`（GPIOC）与 `|= 1 << 9`（ADC1）。任何外设用之前必须先给它和它的 IO 口"通电"。
2. **引脚设为模拟输入**：`sys_gpio_set(GPIOC, SYS_GPIO_PIN4, SYS_GPIO_MODE_AIN, ...)`，即把 `GPIOC->CRL` 对应 4 位写成 `MODE=11/CNF=00`，数字输入缓冲关闭，纯 analog 通道。
3. **复位 ADC**：`RCC->APB2RSTR |= 3 << 9; RCC->APB2RSTR &= ~(3 << 9);`（bit9=ADC1、bit10=ADC2 复位，脉冲式复位后自动释放）。
4. **分频**：见 2.2 的 ADCPRE。
5. **配 `CR1`**：`DUALMODE[19:16]=0` 独立模式；`SCAN(bit8)=0` 非扫描（单通道够用）。
6. **配 `CR2`**：`CONT(bit1)=0` 单次转换；`EXTSEL[19:17]=111` + `EXTTRIG(bit20)=1`，即"必须用 SWSTART 软件启动"；`ALIGN(bit11)=0` 数据右对齐。
7. **定规则序列长度**：`SQR1` 的 `L[23:20]=0` → 序列里只有 1 个转换（序列 1）。
8. **上电 + 校准**：`ADON(bit0)=1` 开转换器 → `RSTCAL(bit3)=1` 复位校准寄存器并等它被硬件清零 → `CAL(bit2)=1` 启动自校准并等它清零。校准是为了消除器件自身的偏移误差，**必须做**，否则读数可能偏几十个字。
9. **选通道 + 采样时间**：`SQR3` 的 `SQ1[4:0] = ch`（规则序列第 1 个位置放哪个通道），`SMPR1/SMPR2` 设该通道采样时间。
10. **启动并取结果**：`CR2 |= 1 << 22`（SWSTART）→ 轮询 `SR` 的 `EOC(bit1)` 等待转换完成 → 读 `DR`（读 DR 也会自动清 EOC）。

### 2.4 多通道扫描：序列寄存器 SQR1/SQR3 与采样时间寄存器 SMPR1/SMPR2

要一次采 6 个通道，就把"转换排队表"填好，让 ADC 自动按顺序一个个转换：

- `CR1.SCAN = 1`：打开扫描模式；
- `SQR1.L = 5`：规则序列里有 `L+1 = 6` 个转换；
- `SQR3`：每 5 个 bit 表示一个序列位置（SQ1 在 bit4:0、SQ2 在 bit9:5、SQ3 在 bit14:10、SQ4 在 bit19:15、SQ5 在 bit24:20、SQ6 在 bit29:25），本工程依次填 0、1、2、3、4、5，也就是通道 0～5（PA0～PA5）；
- `SMPR2`：通道 0～9 的采样时间全放这里（每通道 3 bit），通道 10～17 在 `SMPR1`。所以代码里对 ch0～ch5 逐个调用 `adc_channel_set(ADC_ADCX, n, 7)`。

ADC 按 SQ1→SQ6 转换 6 次，算一轮；配合 `CONT=1`（连续转换）就会一轮接一轮转下去，每转完一次都由 DMA 搬走一个数，于是缓冲区里呈现 **ch0,ch1,…,ch5,ch0,ch1,…** 的交错排列——16-3 的显示代码正是按 `g_adc_dma_buf[(6 * i) + j]` 这个规律取数的。

### 2.5 用 DMA 搬 ADC 结果（16-2 / 16-3 / 16-4）

**DMA（直接存储器访问）＝ 专门的"搬运工"**：CPU 配好"从哪个地址搬、搬到哪、搬多少个"，之后每次 ADC 转换完成，硬件自动把 `ADC1->DR` 的一个 16 位值放进数组，CPU 完全不用管，省掉轮询和逐次读取。

F103 的外设-通道映射是固定的（不能随便换）：

- **ADC1 只能用 DMA1_Channel1**，**ADC2 不支持 DMA**，ADC3 用 DMA2_Channel5 —— 这三点直接写在 [adc.h（16-2）](实验16-2%20单通道ADC采集（DMA读取）/Drivers/BSP/ADC/adc.h) 的注释里，本工程宏 `ADC_ADCX_DMACx` 就是 `DMA1_Channel1`。
- ADC 侧要设 `CR2.DMA(bit8) = 1`，每转换完一个数据就发出一次 DMA 请求。
- DMA 侧：`CPAR = &ADC1->DR`（外设地址固定不动，PINC=0）、`CMAR = 缓冲区首地址`（存储器递增，MINC=1）、`DIR=0`（外设→存储器）、数据宽度 `PSIZE/MSIZE=1`（16 位，正好是 ADC 转换结果宽度）、`PL=01` 中等优先级、开传输完成中断 `TCIE=1`。

关于"双缓冲 / 循环模式"，这里要说清楚：**STM32F103 的 DMA 通道没有双缓冲（DBM）功能**（那是 F4/G4 系列的能力），它的 `CCR` 只有一个模式选择位 `CIRC(bit5)`：

| DMA 模式 | 本工程用法 | 特点 |
| --- | --- | --- |
| 普通模式 `CIRC=0` | ADC 三个 DMA 实验都用这个（代码里 `CCR \|= 0 << 5`） | 搬完 `CNDTR` 个数据自动停止，需要软件重新装填数量并重新使能 |
| 循环模式 `CIRC=1` | ADC 实验没用；实验18-3 的 DAC 正弦波用 | 搬完自动回到缓冲区头继续搬，适合持续输出波形 |

所以本实验的"连续采集"是这样实现的：一轮 `ADC_DMA_BUF_SIZE` 个数据搬完 → `DMA1_Channel1` 中断置标志 `g_adc_dma_sta = 1` → 主循环算平均值 → 再调 `adc_dma_enable(ADC_DMA_BUF_SIZE)` 重新装填启动下一轮。注意 `adc_dma_enable()` 里刻意先 `CR2 &= ~(1 << 0)`（关 ADC）再动 DMA、最后重新开 ADC 并 SWSTART，目的是保证 DMA 与 ADC 的请求计数对齐、不从半包数据开始搬。

### 2.6 过采样：怎么把 12 位"变成"16 位（16-4）

单次 12 位转换的最低位（LSB）只有 `3.3V / 4096 ≈ 0.8mV`，而且带噪声，读数会跳。**过采样（oversampling）**的思路很朴素：同一个电压多采很多次再平均。若各次噪声近似独立、且幅度足够让 LSB 抖动（本工程用最短采样时间/高输入阻抗来"制造"抖动），则：

```text
每多采 4 次 → 噪声功率降 4 倍 → 有效位数 +1 bit
+4 bit（12 位 → 16 位）需要 4^4 = 256 次转换
```

代码里就是这个数值（[main.c（16-4）](实验16-4%20单通道ADC过采样（16位分辨率）/User/main.c)）：

```c
#define ADC_OVERSAMPLE_TIMES    256         /* ADC过采样次数, 这里提高4bit分辨率, 需要256倍采样 */

#define ADC_DMA_BUF_SIZE        ADC_OVERSAMPLE_TIMES * 10   /* ADC DMA采集 BUF大小, 应等于过采样次数的整数倍 */
```

即缓冲区 `g_adc_dma_buf[2560]` 放 **2560 次**转换结果 = 10 组"256 次一组的累加和"。主循环的处理是：

```c
adcx = sum / (ADC_DMA_BUF_SIZE / ADC_OVERSAMPLE_TIMES); /* 取平均值 */
adcx >>= 4;     /* 除以2^4倍, 得到12+4位 ADC精度值, 注意: 提高 N bit精度, 需要 >> N */
```

`sum / 10` 把 10 组还原成"一组 256 次的累加值"（范围 0～4095×256），再 `>> 4` 等价于除以 16，得到 0～65535 的 **16 位结果**，换算电压时就用 `3.3 / 65536`。

代价：速度。按 21µs/次计算，2560 次约 **53.8ms** 才刷新一次结果（约 18.6 次/秒），比 16-2（2.1ms 一轮）慢 25 倍——分辨率和速度是一对必须权衡的矛盾。

## 三、硬件资源与接线

### 3.1 三个子实验共用的资源

| 资源 | 引脚 | 代码中的宏/位置 | 说明 |
| --- | --- | --- | --- |
| LED0（红灯） | PA8 | `LED0_GPIO_PORT/PIN`，[led.h](实验16-1%20单通道ADC采集/Drivers/BSP/LED/led.h) | 低电平点亮，主循环 `LED0_TOGGLE()` 闪烁表示程序在跑 |
| LED1（绿灯） | PD2 | `LED1_GPIO_PORT/PIN` | 本实验未使用 |
| 串口1 | PA9(TX)/PA10(RX) | `USART_TX/RX_GPIO_PIN`，[usart.h](实验16-1%20单通道ADC采集/Drivers/SYSTEM/usart/usart.h) | 115200bps；接板载 CH340，需在 USART1 排针上把 PA9/PA10 跳线到 RXD/TXD |
| TFTLCD 模块 | 数据 PB0～PB15，RD=PC6、WR=PC7、RS=PC8、CS=PC9、BL=PC10 | `LCD_*_GPIO_PIN`，[lcd.h](实验16-1%20单通道ADC采集/Drivers/BSP/LCD/lcd.h) | 16 位 8080 并口 MCU 屏，插在开发板 LCD 插座上；**不支持 RGB 屏** |
| 时钟 | — | `sys_stm32_clock_init(9)` | PLL 倍频 9：8MHz 外部晶振 ×9 = 72MHz；APB2=72M、APB1=36M，ADC 再从 APB2 6 分频得 12MHz |

> ADC 输入引脚是芯片引脚直接引出，**板上没有隔离/保护电路**，被测电压必须在 0～3.3V 之间（`VDDA` 范围），否则可能烧芯片。

### 3.2 实验16-1 单通道ADC采集

| 资源 | 引脚 | 代码中的宏/位置 | 说明 |
| --- | --- | --- | --- |
| ADC1 规则序列 1 = 通道 14 | **PC4** | `ADC_ADCX = ADC1`、`ADC_ADCX_CHY = 14`、`ADC_ADCX_CHY_GPIO_PORT = GPIOC`、`ADC_ADCX_CHY_GPIO_PIN = SYS_GPIO_PIN4` | 电压输入脚 |
| 可调电压源 | 板上变阻器 RV1 | 官方 `readme.txt`：跳线帽连接 ADC 与 RV1（P1 插针） | 旋动 RV1 → PC4 电压在 0～3.3V 变化 |

### 3.3 实验16-2 单通道ADC采集（DMA读取）

同 3.2，另外增加一路 DMA：

| 资源 | 通道 | 代码中的宏 | 说明 |
| --- | --- | --- | --- |
| DMA1 通道 1 | ADC1 专用 | `ADC_ADCX_DMACx = DMA1_Channel1`，`ADC_ADCX_DMACx_IRQn` | 中断优先级由 `sys_nvic_init(3, 3, ..., 2)` 设置（组2 最低优先级） |

### 3.4 实验16-3 多通道ADC采集（DMA读取）

| 资源 | 引脚 | 代码中的宏/位置 | 说明 |
| --- | --- | --- | --- |
| ADC1 序列 1～6 = 通道 0～5 | PA0、PA1、PA2、PA3、PA4、PA5 | `adc_nch_dma_init()` 内 6 次 `sys_gpio_set(GPIOA, SYS_GPIO_PINn, SYS_GPIO_MODE_AIN, ...)` | 用杜邦线分别接 0～3.3V 待测电压 |
| DMA1 通道 1 | 同上 | `ADC_ADCX_DMACx` | 缓冲区 300 个 16 位数据 |

### 3.5 实验16-4 单通道ADC过采样（16位分辨率）

同 3.2 的 ADC1 通道 14（PC4），官方 `readme.txt` 建议"用杜邦线接 ADC 引脚到 VCC、GND、TPAD 等测试转换效果"；缓冲区 2560 个 16 位数据。

## 四、代码结构与调用流程

以 16-2 为例的目录树（16-3/16-4 结构相同，16-1 没有 `BSP/DMA`）：

```text
实验16 ADC实验/实验16-2 单通道ADC采集（DMA读取）/
├── Drivers/
│   ├── BSP/
│   │   ├── ADC/adc.c|adc.h     # ADC 与 ADC+DMA 全部驱动（adc_init/adc_dma_init/adc_dma_enable/中断服务）
│   │   ├── DMA/dma.c|dma.h     # DMA 通用配置：dma_basic_config(设 CPAR/CMAR)、dma_enable(装 CNDTR 并启动)
│   │   ├── LED/led.c|led.h     # LED0/LED1
│   │   └── LCD/lcd.c|lcd.h     # 8080 并口屏驱动，提供 lcd_show_string / lcd_show_xnum
│   └── SYSTEM/
│       ├── sys/sys.c|sys.h     # 时钟初始化、sys_gpio_set(写 CRL/CRH)、sys_nvic_init(中断优先级)
│       ├── delay/delay.c|delay.h  # SysTick 微秒/毫秒延时
│       └── usart/usart.c|usart.h  # 串口1，115200
├── User/main.c                 # 应用主程序
└── Projects/MDK-ARM/atk_f103.uvprojx
```

各子实验 `main()` 流程（编号步骤）：

- **16-1**：`sys_stm32_clock_init(9)` → `delay_init(72)` → `usart_init(72,115200)` → `led_init()` → `lcd_init()` → `adc_init()` → 画标题 → 死循环：`adc_get_result_average(ADC_ADCX_CHY, 10)`（10 次查询、每次之间 `delay_ms(5)`）→ 显示原始值/电压 → `LED0_TOGGLE()` → `delay_ms(100)`。
- **16-2**：前 5 步同上（但没有 `adc_init()`，它被 `adc_dma_init()` 内部调用）→ `adc_dma_init((uint32_t)&g_adc_dma_buf)` → `adc_dma_enable(100)` → 死循环：等 `g_adc_dma_sta == 1` → 累加 100 点取平均 → 显示 → 清标志 → 再 `adc_dma_enable(100)` → `LED0_TOGGLE()` + `delay_ms(100)`。
- **16-3**：`adc_nch_dma_init(&g_adc_dma_buf)` → `adc_dma_enable(300)` → 等标志 → 双层循环 `for(j=0;j<6;j++) for(i=0;i<50;i++) sum += g_adc_dma_buf[(6*i)+j]` → 分别显示 6 个通道 → 重新启动下一轮。
- **16-4**：`adc_dma_init(&g_adc_dma_buf)` → `adc_channel_set(ADC_ADCX, ADC_ADCX_CHY, 0)` → `adc_dma_enable(2560)` → 等标志 → `sum/10` 再 `>>4` → 按 `3.3/65536` 显示 16 位结果 → 下一轮。

## 五、核心代码解析

### 5.1 `adc_init()`：单次、软件触发、带校准（16-1，其余子实验复用）

```c
    ADC_ADCX->CR1 &= ~(0XF << 16);  /* 工作模式清零 */
    ADC_ADCX->CR1 |= 0 << 16;       /* 独立工作模式 */
    ADC_ADCX->CR1 &= ~(1 << 8);     /* 非扫描模式 */

    ADC_ADCX->CR2 &= ~(1 << 1);     /* 单次转换模式 */
    ADC_ADCX->CR2 &= ~(7 << 17);
    ADC_ADCX->CR2 |= 7 << 17;       /* 软件控制转换 */
    ADC_ADCX->CR2 |= 1 << 20;       /* 使用用外部触发(SWSTART)!!! 必须使用一个事件来触发 */
    ADC_ADCX->CR2 &= ~(1 << 11);    /* 右对齐 */
```

- `CR1[19:16]` 是双 ADC 模式选择，写 0 = ADC1 独立工作，最简单。
- `CR1.bit8` SCAN=0：只转换 1 个规则通道时不需要扫描（16-3 会把它置 1）。
- `CR2.bit1` CONT=0：SWSTART 一次只转换一次；置 1 就是"转完自动接着转"，DMA 实验里要置 1。
- `EXTSEL[19:17]=111` 配合 `EXTTRIG(bit20)=1` 是把"外部触发事件"选成 **SWSTART**。F103 规定：规则通道转换必须由某个触发事件启动，纯软件方式就是"选 SWSTART 事件 + 软件置位 SWSTART"，这两行缺一个 ADC 就不会动。
- `ALIGN(bit11)=0`：结果右对齐放在 `DR[11:0]`，读出来直接是 0～4095。

校准三步（顺序不能颠倒）：

```c
    ADC_ADCX->CR2 |= 1 << 0;        /* 开启AD转换器 */

    ADC_ADCX->CR2 |= 1 << 3;        /* 使能复位校准 */

    while (ADC_ADCX->CR2 & 1 << 3); /* 等待校准结束 */

    /* 该位由软件设置并由硬件清除  在校准寄存器被初始化后该位将被清除 */
    ADC_ADCX->CR2 |= 1 << 2;        /* 开启AD校准 */

    while (ADC_ADCX->CR2 & 1 << 2); /* 等待校准结束 */
```

`ADON(bit0)=1` 后 ADC 需要一小段稳定时间；`RSTCAL(bit3)` 是"复位内部校准寄存器"，`CAL(bit2)` 是"启动校准"，这两位都由硬件在完成后自动清零，所以用 `while` 死等即可。

### 5.2 `adc_channel_set()` 与 `adc_get_result()`：通道选择 + 采样时间 + 启动读取

```c
void adc_channel_set(ADC_TypeDef *adcx, uint8_t ch, uint8_t stime)
{
    if (ch < 10)    /* 通道0~9,使用SMPR2配置 */
    {
        adcx->SMPR2 &= ~(7 << (3 * ch));        /* 通道ch 采样时间清空 */
        adcx->SMPR2 |= 7 << (3 * ch);           /* 通道ch 采样周期设置,周期越高精度越高 */
    }
    else            /* 通道10~17,使用SMPR1配置 */
    {
        adcx->SMPR1 &= ~(7 << (3 * (ch - 10))); /* 通道ch 采样时间清空 */
        adcx->SMPR1 |= 7 << (3 * (ch - 10));    /* 通道ch 采样周期设置,周期越高精度越高 */
    }
}
```

- `ch < 10` 判断的依据就是 2.2 说的寄存器分工：`SMPR2` 管通道 0～9、`SMPR1` 管通道 10～17，每通道 3 bit，所以偏移量是 `3 * ch`。
- 注意形参 `stime` 在函数体里**一次都没用到**，写入的永远是常量 `7`（239.5 周期）。这是本驱动的一个"坑"，详见第八章 8.2。

```c
uint32_t adc_get_result(uint8_t ch)
{
    adc_channel_set(ADC_ADCX, ch, 7);   /* 设置ADCX对应通道采样时间为239.5个时钟周期 */

    ADC_ADCX->SQR3 &= ~(0X1F << 5 * 0); /* 规则序列1通道清零 */
    ADC_ADCX->SQR3 |= ch << (5 * 0);    /* 规则序列1 通道 = ch */
    ADC_ADCX->CR2 |= 1 << 22;           /* 启动规则转换通道 */

    while (!(ADC_ADCX->SR & 1 << 1));   /* 等待转换结束 */

    return ADC_ADCX->DR;                /* 返回adc值 */
}
```

`SQR3` 的低 5 bit 是 `SQ1`（序列第 1 位放哪个通道）；`1 << 22` 是 SWSTART；`SR.bit1` 是 EOC（End Of Conversion）。这里用轮询等待，CPU 在此期间什么都干不了——这正是引出 16-2 DMA 版本的理由。

### 5.3 16-1：查询式 + 软件平均

`adc_get_result_average(ch, times)` 就是"采 times 次、求和、除以 times"，每次之间 `delay_ms(5)` 让输入电压有机会重新稳定，避免连读同一个引脚的采样电容互相影响。主循环里：

```c
        adcx = adc_get_result_average(ADC_ADCX_CHY, 10);    /* 获取通道5的转换值，10次取平均 */
        lcd_show_xnum(142, 110, adcx, 5, 16, 0, BLUE);      /* 显示ADCC采样后的原始值 */
 
        temp = (float)adcx * (3.3 / 4096);                  /* 获取计算后的带小数的实际电压值，比如3.1111 */
```

电压换算公式：`V = 数字值 × 3.3 / 4096`（12 位，参考电压 3.3V）。后面几行是把小数拆成整数部分 + 三位小数分别显示的技巧：先显示整数，再 `temp -= adcx; temp *= 1000;` 显示小数，配合 `lcd_show_xnum(..., 0X80, ...)`（`0X80` 表示小数点显示）。注意第一行的注释写"通道5"，实际宏是通道 14（见第八章）。

### 5.4 16-2：DMA 单通道采集

```c
void adc_dma_init(uint32_t mar)
{
    adc_init(); /* 先初始化ADC */

    /* 配置ADC连续转换, DMA传输ADC数据 */
    ADC_ADCX->CR2 |= 1 << 8;        /* DMA = 1, DMA传输使能 */
    ADC_ADCX->CR2 |= 1 << 1;        /* CONT = 1, 连续转换模式 */

    ADC_ADCX->SQR3 &= ~(0X1F << 5 * 0);         /* 规则序列1通道清零 */
    ADC_ADCX->SQR3 |= ADC_ADCX_CHY << (5 * 0);  /* 规则序列1 通道 = ADC_ADCX_CHY */

    adc_channel_set(ADC_ADCX, ADC_ADCX_CHY, 7); /* 设置ADCX对应通道采样时间为239.5个时钟周期 */

    /* 使能DMA时钟, 并设置CPAR和CMAR寄存器, 这里外设地址为: &ADC1->DR */
    dma_basic_config(ADC_ADCX_DMACx, (uint32_t)&ADC1->DR, mar);
```

- 在 `adc_init()` 已完成的配置之上，只补三件事：开 DMA 请求位、开连续转换、把序列 1 固定为通道 14。因为通道固定，`CONT=1` 时 ADC 就变成一个"每 21µs 产生一个数"的流水线。
- `dma_basic_config()`（[dma.c](实验16-2%20单通道ADC采集（DMA读取）/Drivers/BSP/DMA/dma.c)）负责使能 `RCC->AHBENR` 的 DMA1 时钟（`if (dmax_chy > DMA1_Channel7)` 判断该开 DMA1 还是 DMA2），并写 `CPAR`/`CMAR`、把 `CNDTR` 清零。
- 其余 `CCR` 位在 `adc_dma_init()` 里逐位设置，含义见 2.5 的表。其中 `|= 0 << 14` 这行注释写作 MEM2MEM。

一次传输完成的处理：

```c
void ADC_ADCX_DMACx_IRQHandler(void)
{
    if (ADC_ADCX_DMACx_IS_TC())     /* DMA传输完成? */
    {
        g_adc_dma_sta = 1;          /* 标记DMA传输完成 */
        ADC_ADCX_DMACx_CLR_TC();    /* 清除DMA对应通道 传输完成中断标志 */
    }
}
```

宏展开就是判断/清除 `DMA1->ISR` 的 bit1（TCIF1，通道 1 传输完成标志）。中断里只置一个标志位，计算和显示都留在主循环——这是"上半部极简、重活放主循环"的好写法，主循环靠 `if (g_adc_dma_sta == 1)` 轮询该标志。

重新启动一轮采集：

```c
void adc_dma_enable(uint16_t cndtr)
{
    ADC_ADCX->CR2 &= ~(1 << 0);         /* 先关闭ADC */
    
    dma_enable(ADC_ADCX_DMACx, cndtr);  /* 重新使能DMA传输 */
    
    ADC_ADCX->CR2 |= 1 << 0;            /* 重新启动ADC */
    ADC_ADCX->CR2 |= 1 << 22;           /* 启动规则转换通道 */
}
```

`dma_enable()` 内部是先 `CCR &= ~(1<<0)`（关 DMA）并等 EN 位真正清 0，再写 `CNDTR = cndtr`，最后置 EN。顺序很重要：**CNDTR 只能在通道停止时修改**。

### 5.5 16-3：多通道扫描 + DMA

```c
    ADC_ADCX->CR1 |= 1 << 8;        /* SCAN = 1, 扫描模式 */
    ADC_ADCX->CR2 |= 1 << 8;        /* DMA = 1, DMA传输使能 */
    ADC_ADCX->CR2 |= 1 << 1;        /* CONT = 1, 连续转换模式 */
    
    ADC_ADCX->SQR1 &= ~(0XF << 20); /* L[3:0]清零 */
    ADC_ADCX->SQR1 |= 5 << 20;      /* 6个转换在规则序列中 也就是转换规则序列1~6 */
    
    ADC_ADCX->SQR3  = 0 << 0;       /* SQ1[4:0] = 0, 规则序列1 = 通道0(PA0) */
    ADC_ADCX->SQR3 |= 1 << 5;       /* SQ2[4:0] = 1, 规则序列2 = 通道1(PA1) */
    ADC_ADCX->SQR3 |= 2 << 10;      /* SQ3[4:0] = 2, 规则序列3 = 通道2(PA2) */
    ADC_ADCX->SQR3 |= 3 << 15;      /* SQ4[4:0] = 3, 规则序列4 = 通道3(PA3) */
    ADC_ADCX->SQR3 |= 4 << 20;      /* SQ5[4:0] = 4, 规则序列5 = 通道4(PA4) */
    ADC_ADCX->SQR3 |= 5 << 25;      /* SQ6[4:0] = 5, 规则序列5 = 通道5(PA5) */
```

`L = 5` 表示序列长度 `5+1 = 6`；`SQR3` 每 5 bit 一位、依次填通道号 0～5。之后为 PA0～PA5 六个引脚逐个配置模拟输入，并给通道 0～5 都设采样时间。DMA 侧与 16-2 完全相同，只是 `adc_dma_enable(ADC_DMA_BUF_SIZE)` 传的长度是 `50 * 6 = 300`：一轮扫描 6 个通道，共 50 轮。取数时用 `(6 * i) + j` 就能把同一通道的 50 个点归到一起平均。

### 5.6 16-4：过采样的数值处理

```c
            adcx = sum / (ADC_DMA_BUF_SIZE / ADC_OVERSAMPLE_TIMES); /* 取平均值 */
            adcx >>= 4;     /* 除以2^4倍, 得到12+4位 ADC精度值, 注意: 提高 N bit精度, 需要 >> N */

            /* 显示结果 */
            lcd_show_xnum(142, 110, adcx, 5, 16, 0, BLUE);      /* 显示ADC采样后的原始值 */

            temp = (float)adcx * (3.3 / 65536);                 /* 获取计算后的带小数的实际电压值，比如3.1111 */
```

- 这里 `adcx`、`sum` 都改成了 `uint32_t`，因为 2560 次累加的最大值 `2560 × 4095 ≈ 1.05e7` 远超 16 位，12 位结果本身也在 32 位里才安全。
- `sum / 10` 后是"256 个 12 位数的和"（≤ 1,048,320），`>> 4` 后落在 0～65535，正好是 16 位；因此分母从 `4096` 变成 `65536`，显示位数也从 4 位变成 5 位。
- 通用规则：**想多提高 N bit，就要采 `4^N` 次，然后右移 N 位**。12→14 位是 16 次、`>>2`；12→16 位是 256 次、`>>4`。

## 六、编译、下载与运行

1. 用 Keil MDK 打开对应子实验的工程（每个子实验是独立工程，互不影响）：
   - `实验16-1 单通道ADC采集/Projects/MDK-ARM/atk_f103.uvprojx`
   - `实验16-2 单通道ADC采集（DMA读取）/Projects/MDK-ARM/atk_f103.uvprojx`
   - `实验16-3 多通道ADC采集（DMA读取）/Projects/MDK-ARM/atk_f103.uvprojx`
   - `实验16-4 单通道ADC过采样（16位分辨率）/Projects/MDK-ARM/atk_f103.uvprojx`
2. 器件为 **STM32F103RC**，启动文件 `startup_stm32f103xe.s`（ARM 汇编，在 `Drivers/CMSIS/.../Source/Templates/arm/` 下），包含路径为 `..\..\Drivers\CMSIS\Device\ST\STM32F1xx\Include;..\..\Drivers\CMSIS\Include;..\..\Drivers;..\..\User;..\..\Middlewares`。
3. 编译（F7）生成 `atk_f103.axf/bin`，产物在子实验的 `Output/` 目录。
4. 下载：用 ST-Link（SWD 接口，SWDIO/SWCLK/GND/3.3V）或 J-Link 直接烧写；也可用串口 ISP（BOOT0=1 复位进入 bootloader）。
5. 上位机：需要看串口时，USB 线接 **USB_UART** 口，串口助手选 XCOM/SSCOM，**波特率 115200、8N1**，并在 USART1 排针把 PA9→TXD、PA10→RXD 用跳线帽连上。

## 七、实验现象

- **16-1**：LCD 显示 `ADC TEST` 与 `ADC1_CH14_VAL:` / `ADC1_CH14_VOL:`。跳线帽把 ADC 插针与 RV1（P1 插针）连通后，缓慢旋动变阻器，原始值在 0～4095 之间变化，电压值同步在 0.000～3.300V 变化；LED0 约每 100ms+ 闪烁一次（一轮平均约 50ms）。
- **16-2**：标题为 `ADC DMA TEST`，现象与 16-1 相同，但 CPU 不轮询 ADC；原始值显示 4 位。改接电压（或旋 RV1）时读数同样跟随。
- **16-3**：标题为 `ADC 6CH DMA TEST`，屏幕分 6 行显示 `ADC1_CH0_VAL/VOL` 一直到 `ADC1_CH5`。用杜邦线把 PA0～PA5 分别接到不同电压源（0～3.3V），每行显示自己通道的平均值；某脚悬空时读数会漂移（正常现象，模拟输入阻抗高时易受干扰）。
- **16-4**：标题为 `ADC OverSample TEST`，原始值显示 5 位（0～65535）。把 ADC 引脚分别接 GND（读数应为 0 附近且非常稳定）、VCC/3.3V（接近 65535）、或 TPAD 触摸点（手摸时读数明显变化），可看到小数位比 12 位模式稳得多；刷新周期约 50～100ms 一次（受 2560 次转换 + 100ms 延时影响）。
- 所有子实验中 LED0 持续闪烁，代表主循环在跑。

## 八、注意事项

1. **ADC 通道与引脚必须对应**：通道号不是随便填的。16-1/16-2/16-4 用通道 14 → 必须接 **PC4**；16-3 用通道 0～5 → 必须接 **PA0～PA5**。接错引脚表现为"读数不变化"。
2. **官方 `readme.txt` 与代码不完全一致，以代码为准**：
   - 16-1 的 `readme.txt` 写"ADC1 : 通道14 - PC4"，与代码 `ADC_ADCX_CHY = 14`、`GPIOC/SYS_GPIO_PIN4` 一致；但 `main.c` 里 `adc_get_result_average(ADC_ADCX_CHY, 10)` 的行尾注释写的是"获取通道5的转换值"，属于历史遗留笔误，**实际是通道 14**。
   - `main.c` 的行尾注释另有两处历史遗留笔误：16-3 里 `for (i = 0; i < ADC_DMA_BUF_SIZE / 6; i++)` 注释写"每个通道采集了10次数据"，实际 `ADC_DMA_BUF_SIZE / 6 = 50`，即每通道累加 **50 次**；16-4 里 `adc_channel_set(..., 0)` 的注释问题见下条。
   - `adc_channel_set()` 与 `adc2_channel_set()` 的形参 `stime` 在函数体里没有被使用，写 SMPR 时**固定写入 7（239.5 周期）**。因此 16-4 中 `adc_channel_set(ADC_ADCX, ADC_ADCX_CHY, 0)` 这行注释所说的"设为 1.5 个时钟周期以达到最高采集速度"在当前驱动下**并不生效**，实际每通道仍是 21µs 转换；想要真正的 1.5 周期，需要把函数体里的 `7` 改成 `stime`。
   - `dma.c`/`adc.c` 里 `CCR |= 0 << 14` 注释为 MEM2MEM；按 RM0008，DMA_CCR 的 bit14 是 PINCOS、MEM2MEM 在 bit16（本工程 CMSIS 头 `DMA_CCR_MEM2MEM_Pos` 也写作 14）。因为这行写的是 0，功能上无影响。
   - 16-x 的 `readme.txt` 强调"串口输入字符串以回车换行结束"（那是给 USMART 用的），但四个 ADC 工程的 `main.c` **都没有调用 `usmart_dev.init()`**；`Middlewares/USMART/` 源码虽然随工程带着，`main.c` 也留了一行 `#include "./USMART/usmart.h"`，可 Keil 工程（`atk_f103.uvprojx` 的分组只有 Startup/User/Drivers/SYSTEM/Drivers/BSP/Readme）**并没有把 usmart 的 .c 加入编译**。因此 ADC 实验**无法通过串口调用函数**，串口只用于打印（本实验甚至没有打印）。
3. **DMA 通道不能乱改**：`ADC_ADCX_DMACx` 必须是 `DMA1_Channel1`（ADC1 的固定请求通道）；ADC2 没有 DMA 请求线。换成别的通道会"永远收不到数据"。
4. **采样时间与信号源阻抗要匹配**：驱动内阻很高（例如直接用电位计末端、光敏电阻）时，239.5 周期的长采样才准确；短采样会导致充电不足、读数偏低。
5. **模拟输入禁接超过 3.3V 的电压**：这些引脚无保护电路，超压可能损坏芯片；测量高于 3.3V 的电压请先用电阻分压。
6. **16-3 的缓冲区长度必须是通道数的整数倍**：`ADC_DMA_BUF_SIZE = 50 * 6`，且主循环按 `6*i + j` 取数；若改了序列长度（`SQR1.L`）必须同步改这个除法。
7. **16-4 想要真正的 16 位精度，前提是噪声足够抖动 LSB**：如果被测电压极其干净、又用最短采样时间，256 次累加的低位可能一直是 0，"看起来是 16 位"但有效位仍不到 16 位。这是过采样的物理限制，不是代码问题。
8. 若 LCD 无任何显示：先确认是 **MCU 8080 屏**（不支持 RGB 屏）、屏插反否、`LCD_BL`(PC10) 背光亮起否；若下载失败请检查 ST-Link 接线与是否按住复位后释放。

## 九、总结与延伸

**本实验掌握的要点**：

1. ADC = 采样时间（`SMPR1/SMPR2`，每通道 3 bit）+ 12.5 周期转换时间；ADC 时钟 12MHz 时长采样一次 21µs。
2. 规则序列由 `SQR1.L`（长度）、`SQR3`（顺序）、`CR1.SCAN`（扫描）、`CR2.CONT`（连续）、`CR2.SWSTART + EXTTRIG/EXTSEL=111`（软件触发）共同决定；上电后必须 `ADON → RSTCAL → CAL` 校准。
3. 查询式采集简单但阻塞；`CR2.DMA=1` + `DMA1_Channel1` 让硬件自动把 `DR` 搬到数组，中断只置标志。
4. F103 的 DMA 只有普通/循环两种模式，没有双缓冲；普通模式需要软件重装 `CNDTR`（本工程通过"关 ADC → `dma_enable()` → 开 ADC → SWSTART"重新武装一轮）。
5. 过采样每 `4^N` 次换 `+N` bit：12→16 位用 256 次、`sum/10` 再 `>>4`，代价是速度降约 25 倍。

**延伸小练习**：

1. 把 `adc_channel_set()` 里的常量 `7` 改成 `stime`，再跑 16-4，对比两种采样时间下的读数稳定性与刷新速度。
2. 改 `ADC_ADCX_CHY` 为 15（PA7）或 16（内部温度传感器），观察 16-1 的现象变化（改通道 16 时可对照实验17）。
3. 16-3 基础上把序列改成 3 个通道（`SQR1.L = 2`、`SQR3` 填 0/1/2）、缓冲区改成 `100 * 3`，并把显示改成 3 行。
4. 16-4 里把 `ADC_OVERSAMPLE_TIMES` 改成 16、右移改成 `>>2`，得到 14 位结果，验证"4 倍采样 = 1 bit"。

**衔接**：读电压会了之后，实验17 用同一套 ADC 框架读片内温度传感器；实验18 则反过来，用 DAC 把数字写成电压，再把 DAC 输出接回 ADC 输入，形成闭环验证。
