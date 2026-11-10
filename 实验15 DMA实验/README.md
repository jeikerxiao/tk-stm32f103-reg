# 实验15 DMA实验

> 一句话概括：给 DMA 写清楚"从哪搬、搬到哪、搬多少个"，然后让硬件自己把 6800 字节数组通过串口 1 发出去，
> CPU 在这段时间里还能刷 LCD、点 LED —— 这就是"搬运交给硬件，CPU 去干别的活"。

## 一、实验简介

- **实验目的（官方）**：学习 STM32 DMA 功能的使用。
- **通俗展开**：假设你要把 6800 个字节从内存发到串口。用普通写法是一个 `for` 循环：
  等串口"发送缓冲区空"→ 塞一个字节 → 再等 → 再塞……CPU 全程被绑死在这件事上。
  DMA（Direct Memory Access，直接存储器访问）就是芯片里雇的一个**搬运工**：
  你只告诉他"源地址、目标地址、搬运次数"，他就自己一个一个搬，搬完敲你一下（标志位或中断）。
  本实验用 **存储器 → USART1 发送数据寄存器** 这个方向演示 DMA 的完整套路：配通道、给地址、给长度、开 DMA 请求、开搬运。
- **学习路线中的位置**：
  - 前置：实验2 按键输入实验（KEY0 触发）、实验4 串口实验（发送对象就是串口 1）、实验11 TFTLCD（MCU屏）实验（显示进度）。
  - 后续：**实验16 ADC 实验**（那里是 DMA 最典型的"外设 → 存储器"用法，而且改成用传输完成中断）、实验17 内部温度传感器、实验28 SD 卡（DMA 加速读写）。
- **实验平台**：正点原子 MiniSTM32 V4 开发板，主芯片 STM32F103RC，Keil MDK 工程，寄存器版（无 HAL/标准库）。

## 二、实验原理

### 2.1 DMA 的四个"格子"

F103 有 2 个 DMA 控制器（DMA1 有 7 个通道，DMA2 有 5 个通道），**每个通道就是一组 4 个 32 位寄存器**
（`DMA_Channel_TypeDef`，见 [stm32f103xe.h](Drivers/CMSIS/Device/ST/STM32F1xx/Include/stm32f103xe.h)）：

| 寄存器 | 全称 | 干什么 | 本实验的值 |
| --- | --- | --- | --- |
| `CCR` | 通道 x 配置寄存器（Control register） | 通道总开关 + 方向/增量/宽度/优先级等所有模式位 | 见 2.3 |
| `CNDTR` | 通道 x 数据个数寄存器（Number of data register） | **还剩多少个数据要搬**，每搬一个就 -1，减到 0 结束 | 启动时写 `SEND_BUF_SIZE` = 6800 |
| `CPAR` | 通道 x 外设地址寄存器（Peripheral address register） | 外设端地址 | `&USART1->DR` = 0x40013804 |
| `CMAR` | 通道 x 存储器地址寄存器（Memory address register） | 存储器端地址 | `g_sendbuf` 数组首地址 |

另外 DMA1 整体还有两个状态寄存器：`DMA1->ISR`（Interrupt Status Register，各通道的标志）和
`DMA1->IFCR`（Interrupt Flag Clear register，写 1 清对应标志）。

一句话记住：**CPAR + CMAR + CNDTR = 从哪、到哪、多少个；CCR = 怎么搬；ISR/IFCR = 搬完了没。**

### 2.2 外设→存储器还是存储器→外设：DIR 位

`CCR` 的第 4 位 **DIR**：

- `DIR = 0`：外设 → 存储器（读外设）。典型场景：ADC 转换结果自动进数组。
- `DIR = 1`：存储器 → 外设（写外设）。**本实验就是它**：数组 → `USART1->DR` → PA9 → 电脑。

DMA 的请求源（哪个外设接哪个通道）是**硬件连线，改不了**。本实验的 `dma.c` 注释写得很明确：

```c
dmax_chy = DMA1_Channel4;   /* USART1_TX使用的DMA通道为: DMA1_Channel4 */
```

要查任意外设对应哪个通道，看代码注释里指的那张表——《STM32 中文参考手册》10.3.7 节。
顺带记两个后面实验会用到的：**ADC1 只能用 DMA1_Channel1**，**USART1_RX 是 DMA1_Channel5**。

### 2.3 CCR 每一位在本实验里的选择

`dma_usart1_tx_config()` 先把 `CCR` 清 0，再逐位填（寄存器版的好处是能把每一位的选择摊开看）：

| 位 | 名称 | 本实验值 | 含义与为什么这样填 |
| --- | --- | --- | --- |
| 0 | EN | 0（配置时先不开） | 通道使能。写 `CNDTR` 必须在 EN=0 时做，所以真正的开启放在 `dma_enable()` |
| 1 | TCIE | 0 | 传输完成中断使能。本实验**不开**，改用轮询 `ISR` 标志（对比：实验16 就是开这个位走中断） |
| 4 | DIR | 1 | 存储器 → 外设（发送） |
| 5 | CIRC | 0 | 非循环模式。搬完 6800 个字节就停，不再自动重来 |
| 6 | PINC | 0 | **外设地址不递增**。USART1 只有 `DR` 这一个数据口，6800 次搬运都写同一个地址，若递增就写到别的寄存器上了 |
| 7 | MINC | 1 | **存储器地址递增**。`g_sendbuf` 是连续数组，搬一个往后走一个，这正是 DMA 存在的意义 |
| 9:8 | PSIZE | 00 | 外设数据宽度 8 位（串口数据寄存器一次吃 1 个字节） |
| 11:10 | MSIZE | 00 | 存储器数据宽度 8 位（数组元素是 `uint8_t`）。两边宽度必须一致，否则 DMA 会做合并/拆分 |
| 13:12 | PL | 01 | 通道优先级"中等"。多个通道同时抢总线时，软件优先级高的先走；同级再比硬件编号（通道 1 最高） |
| 14 | MEM2MEM | 0 | 非"内存到内存"模式。置 1 时不需要外设请求，DMA 自己闷头搬（只能在 DMA1 上用） |

### 2.4 串口这边的配合开关：DMAT

DMA 不会主动往串口塞数据，它等串口发"信号"：**发送数据寄存器空（TXE）**时，USART1 会给 DMA1_Channel4 发一个请求，
DMA 就把下一个字节写进 `USART1->DR`。这个"允许串口去敲 DMA 的门"的开关是 `USART1->CR3` 的第 7 位 **DMAT**：

```c
USART1->CR3 = 1 << 7;   /* 使能串口1的DMA发送 */
```

不开这一位，DMA 配得再漂亮也不会动（因为没人向它发请求）。

### 2.5 一次完整搬运的流程

1. `dma_basic_config()`：开 DMA1 时钟（`RCC->AHBENR` 第 0 位）→ 写 `CPAR`（`&USART1->DR`）→ 写 `CMAR`（`g_sendbuf`）→ `CNDTR = 0`。
2. `dma_usart1_tx_config()`：填 `CCR` 的方向/增量/宽度/优先级（见 2.3），此时 EN 仍是 0。
3. 按 KEY0 → `USART1->CR3 = 1 << 7;` 打开串口的 DMA 发送请求。
4. `dma_enable(DMA1_Channel4, SEND_BUF_SIZE)`：先 `CCR.EN = 0` → 死等 EN 真的变 0 → 写 `CNDTR = 6800` → `CCR.EN = 1`。
5. DMA 开始工作：每收到一个 TXE 请求，就把 `*CMAR` 写进 `*CPAR`，然后 `CMAR += 1`（MINC）、`CNDTR -= 1`。
6. `CNDTR` 减到 0 → 硬件把 `DMA1->ISR` 第 **13** 位 **TCIF4**（通道 4 传输完成标志）置 1；因为没开 TCIE，不产生中断。
7. CPU 轮询到该位为 1 → 写 `DMA1->IFCR` 第 13 位清标志 → 一次搬运结束，可以发起下一次。

### 2.6 参数计算（代入本工程真实数值）

- **发送缓冲区大小**：
  `TEXT_TO_SEND[] = {"正点原子 STM32F103 DMA 串口实验"}`，源码是 GBK 编码，
  4 个汉字 × 2 + 空格 + 9 字符 + 空格 + 3 字符 + 空格 + 4 个汉字 × 2 = **31 字节**，加结尾 `\0` 共 **`sizeof(TEXT_TO_SEND)` = 32**。
  ```c
  #define SEND_BUF_SIZE  (sizeof(TEXT_TO_SEND) + 2) * 200   /* = (32 + 2) × 200 = 6800 */
  ```
  即每段 = 32 字节内容（含那个 `\0`）+ `0x0D` + `0x0A` = 34 字节，共 200 段，总 **6800 字节**。
- **传输耗时**：115200bps、8N1（1 起始位 + 8 数据位 + 1 停止位 = 10 位/字节）
  → `6800 × 10 / 115200 ≈ 0.59 秒`。这段时间 CPU 完全不参与搬运。
- **波特率寄存器**（由 `usart_init(72, 115200)` 算出，[usart.c](Drivers/SYSTEM/usart/usart.c)）：
  `BRR = (72 × 1000000 + 115200/2) / 115200 = 625`，
  校验：波特率 = 72MHz / 625 = 115200bps，与上位机设置一致。
- **CNDTR 是 16 位的**，最大 65535，6800 远够用；若数据更多就要分批搬（`dma_enable()` 形参也是 `uint16_t`）。
- **进度百分比公式**（main.c 里）：`pro = 1 - CNDTR / SEND_BUF_SIZE`，再 `× 100` 取整显示。

## 三、硬件资源与接线

| 资源 | 引脚/地址 | 代码中的宏/位置 | 说明 |
| --- | --- | --- | --- |
| USART1_TX | **PA9** | `USART_TX_GPIO_PORT/PIN`，[usart.h](Drivers/SYSTEM/usart/usart.h) | 复用推挽输出，数据从这儿出去 |
| USART1_RX | **PA10** | `USART_RX_GPIO_PIN` | 接收（本实验只发不接收数据） |
| DMA 外设端地址 | `&USART1->DR` = 0x40013804 | `dma_basic_config(dmax_chy, (uint32_t)&USART1->DR, mar)`，[dma.c](Drivers/BSP/DMA/dma.c) | CPAR 的内容 |
| DMA 通道 | **DMA1_Channel4**（CCR 0x40020044 / CNDTR 0x40020048 / CPAR 0x4002004C / CMAR 0x40020050） | `DMA1_Channel4`，[stm32f103xe.h](Drivers/CMSIS/Device/ST/STM32F1xx/Include/stm32f103xe.h) | USART1_TX 固定占用，不能改成别的通道 |
| DMA 存储器端地址 | `g_sendbuf[6800]` | [User/main.c](User/main.c) | CMAR 的内容，全局数组（占 6800 字节 SRAM） |
| KEY0 | PC5 | `KEY0_GPIO_PIN`，[key.h](Drivers/BSP/KEY/key.h) | 上拉输入，按下为低电平，触发一次 DMA 发送 |
| KEY1 / WK_UP | PA15 / PA0 | [key.h](Drivers/BSP/KEY/key.h) | 本实验未使用 |
| LED0（红灯） | PA8 | `LED0_GPIO_PORT/PIN`，[led.h](Drivers/BSP/LED/led.h) | 每 200ms 翻转，证明 CPU 还在干别的事 |
| LED1（绿灯） | PD2 | [led.h](Drivers/BSP/LED/led.h) | 本实验未使用 |
| TFTLCD（MCU 屏 8080 并口） | D0~D15=PB0~PB15，WR=PC7，RD=PC6，CS=PC9，RS=PC8，BL=PC10 | [lcd.h](Drivers/BSP/LCD/lcd.h) | 显示传送进度 |

**接线与跳线**：

1. USB 线插开发板 **USB_UART** 口，找到 CH340 对应的 COM 口；
   USART1 排针上的 **PA9 接 RXD、PA10 接 TXD**（跳线帽/杜邦线，交叉连接）。
2. LCD 插板载 LCD 座（仅支持 MCU 屏，不支持 RGB 屏），注意 VCC 一角对齐，插反会烧模块。
3. 本实验是"发送"方向，电脑只要开着串口助手收即可，**不需要**往串口发东西。

**关于 72MHz**：`sys_stm32_clock_init(9)` = 8MHz HSE × 9 = 72MHz，DMA 挂在 AHB 上跟着这个主频跑。
串口波特率由 `BRR` 从 PCLK2 分频得到，与 DMA 搬运速度无关——**发送总耗时由 115200bps 决定**，
就算 CPU 快 100 倍也还是 0.59 秒，这是外设瓶颈，DMA 只负责解放 CPU。

## 四、代码结构与调用流程

```
实验15 DMA实验/
├── User/main.c                准备 6800 字节缓冲区、按 KEY0 启动 DMA、轮询标志并显示进度
├── Drivers/BSP/DMA/dma.c|.h   DMA 驱动：dma_basic_config / dma_usart1_tx_config / dma_enable
├── Drivers/BSP/KEY/key.c|.h   KEY0 按键（触发发送）
├── Drivers/BSP/LED/led.c|.h   LED0 闪烁
├── Drivers/BSP/LCD/lcd.c|.h   LCD 显示进度
├── Drivers/SYSTEM/{sys,delay,usart}   时钟/GPIO/NVIC、SysTick 延时、串口与 printf
└── Middlewares/USMART/…       本工程 main 未调用 usmart_dev.init（与 RTC 实验不同）
```

`main()` 流程：

1. `sys_stm32_clock_init(9)` → `delay_init(72)` → `usart_init(72, 115200)` → `led_init()` → `lcd_init()` → `key_init()`
2. `dma_usart1_tx_config((uint32_t)g_sendbuf)` —— **只配一次**，之后每次发送只要调 `dma_enable()` 改长度并开启
3. LCD 打 4 行标题，最后一行提示 `KEY0:Start`
4. `for` 循环填充 `g_sendbuf`：循环复制字符串，复制完一遍补一个 `0x0D`、再补一个 `0x0A`
5. `while(1)`：
   1. `key = key_scan(0)`；
   2. 若 `key == KEY0_PRES`：`printf("\r\nDMA DATA:\r\n")` → LCD 打提示 → `USART1->CR3 = 1 << 7`（开 DMAT）→
      `dma_enable(DMA1_Channel4, SEND_BUF_SIZE)`（开始搬）→
      进入 `while(1)` 等待：查 `DMA1->ISR` bit13，没完成就刷新百分比；完成了清标志并跳出 → LCD 显示 `100` 和 `Transimit Finished!`
   3. `i++`、`delay_ms(10)`；每 20 次（约 200ms）`LED0_TOGGLE()`

## 五、核心代码解析

### 5.1 基础配置：地址与长度归零（[dma.c](Drivers/BSP/DMA/dma.c)）

```c
void dma_basic_config(DMA_Channel_TypeDef *dmax_chy,  uint32_t par, uint32_t mar)
{
    if (dmax_chy > DMA1_Channel7)   /* 大于DMA1_Channel7, 则为DMA2的通道了 */
    {
        RCC->AHBENR |= 1 << 1;      /* 开启DMA2时钟 */
    }
    else
    {
        RCC->AHBENR |= 1 << 0;      /* 开启DMA1时钟 */
    }

    delay_ms(5);                    /* 等待DMA时钟稳定 */

    dmax_chy->CPAR = par;           /* DMA 外设地址 */
    dmax_chy->CMAR = mar;           /* DMA 存储器地址 */
    dmax_chy->CNDTR = 0;            /* DMA 传输长度清零, 后续在dma_enable函数设置 */
}
```

- 靠**指针大小比较**判断是 DMA1 还是 DMA2 的通道：`DMA_Channel_TypeDef` 结构是顺序排布的，
  比 `DMA1_Channel7` 地址大的就是 DMA2 的通道。DMA1 挂 AHB，使能位是 `RCC->AHBENR` bit0，DMA2 是 bit1。
- `par`/`mar` 传的是**地址数值**（`(uint32_t)&USART1->DR`、`(uint32_t)g_sendbuf`），不是数据本身——
  这是寄存器版最容易绕晕的一点：DMA 要的是"门牌号"。
- 长度故意留 0，因为长度是每次发送都要变的，交给 `dma_enable()`。

### 5.2 模式配置：CCR 逐位填

```c
void dma_usart1_tx_config(uint32_t mar)
{
    DMA_Channel_TypeDef *dmax_chy;

    dmax_chy = DMA1_Channel4;   /* USART1_TX使用的DMA通道为: DMA1_Channel4 */

    /* 使能DMA时钟, 并设置CPAR和CMAR寄存器, 这里外设地址为: &USART1->DR */
    dma_basic_config(dmax_chy, (uint32_t)&USART1->DR, mar);

    dmax_chy->CCR = 0X00000000; /* 复位CCR寄存器 */
    dmax_chy->CCR |= 1 << 4;    /* DIR = 1 , 存储器到外设模式 */
    ...
    dmax_chy->CCR |= 1 << 7;    /* MINC = 1, 存储器增量模式 */
    ...
    dmax_chy->CCR |= 1 << 12;   /* PL[1:0] =  1, 中等优先级 */
    dmax_chy->CCR |= 0 << 14;   /* MEM2MEM = 0 , 非存储器到存储器模式 */
}
```

先把 `CCR` 整体清 0 再按位 `|=`，是为了避免上电/复位后的随机值残留；
`|= 0 << n` 这种"或 0"其实什么都不改，留着纯粹是**给读者看的注释**，表示这一位我们选择 0。

### 5.3 启动一次搬运：为什么要先关再等

```c
void dma_enable(DMA_Channel_TypeDef *dmax_chy, uint16_t cndtr)
{
    dmax_chy->CCR &= ~(1 << 0);         /* 关闭DMA传输 */

    while (dmax_chy->CCR & (1 << 0));   /* 确保DMA可以被设置 */

    dmax_chy->CNDTR = cndtr;            /* DMA传输数据量 */
    dmax_chy->CCR |= 1 << 0;            /* 开启DMA传输 */
}
```

`CNDTR` **只有在 EN=0 时才允许写**；而且清 EN 是异步生效的（DMA 内部还有正在进行的总线操作），
所以那句 `while (...)` 不是多余的死等，是"确认通道真的关掉了"。这是重复启动 DMA 的标准写法：
关 → 等关到位 → 改长度 → 开。

### 5.4 主程序里的发送与进度轮询（[main.c](User/main.c)）

```c
if (key == KEY0_PRES)   /* KEY0按下 */
{
    printf("\r\nDMA DATA:\r\n");
    lcd_show_string(30, 130, 200, 16, 16, "Start Transimit....", BLUE);
    lcd_show_string(30, 150, 200, 16, 16, "   %", BLUE);    /* 显示百分号 */
    USART1->CR3 = 1 << 7;   /* 使能串口1的DMA发送 */

    dma_enable(DMA1_Channel4, SEND_BUF_SIZE);   /* 开始一次DMA传输！ */

    /* 等待DMA传输完成，此时我们来做另外一些事情，比如点灯
     * 实际应用中，传输数据期间，可以执行另外的任务
     */
    while (1)
    {
        if (DMA1->ISR & (1 << 13))  /* 等待 DMA1_Channel4 传输完成 */
        {
            DMA1->IFCR |= 1 << 13;  /* 清除 DMA1_Channel4 传输完成标志 */
            break;
        }

        pro = DMA1_Channel4->CNDTR;                 /* 得到当前还剩余多少个数据 */
        len = SEND_BUF_SIZE;                        /* 总长度 */
        pro = 1 - (pro / len);                      /* 得到百分比 */
        pro *= 100;                                 /* 扩大100倍 */
        lcd_show_num(30, 150, pro, 3, 16, BLUE);    /* 显示传输进度 */
    }
    lcd_show_num(30, 150, 100, 3, 16, BLUE);        /* 显示100% */
    lcd_show_string(30, 130, 200, 16, 16, "Transimit Finished!", BLUE); /* 提示传送完成 */
}
```

- **为什么是 bit13？** DMA1 每个通道在 `ISR`/`IFCR` 里占 4 个位：通道 n 从 `(n-1)*4` 开始，
  依次是 GIF、**TCIF（传输完成）**、HTIF（半传输）、TEIF（错误）。通道 4 → 基址 12，TCIF 就是 **12+1 = 13**。
  （对照实验16：ADC 用 DMA1_Channel1，所以判断的是 `DMA1->ISR & (1 << 1)`。）
- 清标志的写法是"往 `IFCR` 对应位**写 1** 清除"，这是 EXTI/DMA 这一类硬件标志的统一规矩，千万别写成 `&= ~(...)`。
- 轮询循环里没有 `delay_ms`，所以刷 LCD 刷得很频繁；这段正好演示 DMA 的价值——CPU 一边刷屏，硬件一边发数据。
- `pro` 是 `float`，`lcd_show_num()` 的形参是 `uint32_t`，小数部分被截掉，屏上只会看到整数百分比。
- 传输完成后**没有**把 `USART1->CR3` 的 DMAT 关回去；下次按 KEY0 会重新走一遍相同流程，功能上没问题，
  只是发送间隙里串口一直处于"可以向 DMA 要数据"的状态（因为 `CNDTR` 已是 0，DMA 不会再搬）。

### 5.5 缓冲区填充（把 34 字节的"一行"重复 200 遍）

```c
const uint8_t TEXT_TO_SEND[] = {"正点原子 STM32F103 DMA 串口实验"};     /* 要循环发送的字符串 */
#define SEND_BUF_SIZE       (sizeof(TEXT_TO_SEND) + 2) * 200            /* 发送数据长度 */
uint8_t g_sendbuf[SEND_BUF_SIZE];   /* 发送数据缓冲区 */
...
for (i = 0; i < SEND_BUF_SIZE; i++) /* 填充ASCII字符集数据 */
{
    if (k >= len)   /* 入换行符 */
    {
        if (mask)
        {
            g_sendbuf[i] = 0x0a;
            k = 0;
        }
        else
        {
            g_sendbuf[i] = 0x0d;
            mask++;
        }
    }
    else     /* 复制TEXT_TO_SEND语句 */
    {
        mask = 0;
        g_sendbuf[i] = TEXT_TO_SEND[k];
        k++;
    }
}
```

`len = sizeof(TEXT_TO_SEND)` = 32（含结尾的 `\0`），复制完 32 个字节后 `k >= len`，
先写一个 `0x0D`（回车）、`mask` 变 1，下一个位置写 `0x0A`（换行）并把 `k` 归 0 —— 于是每 34 字节形成一行。
注意：因为把 `\0` 也当数据发出去了，串口助手里每行末尾会有一个看不见的空字符，属正常现象。

## 六、编译、下载与运行

1. Keil MDK 打开 [Projects/MDK-ARM/atk_f103.uvprojx](Projects/MDK-ARM/atk_f103.uvprojx)。
2. 器件 **STM32F103RC**（IROM 0x08000000 起 256KB，IRAM 0x20000000 起 48KB），
   C 宏定义 `STM32F103xE`，启动文件 `startup_stm32f103xe.s`，
   包含路径 `..\..\Drivers\CMSIS\Device\ST\STM32F1xx\Include;..\..\Drivers\CMSIS\Include;..\..\Drivers;..\..\User;..\..\Middlewares`。
3. 编译产物在 `Output/`（`atk_f103.axf`、`atk_f103.hex`），ST-Link / J-Link 下载；
   也可用串口 ISP（BOOT0=1 上电，FlyMcu 下载 hex）后恢复 BOOT0=0。
4. 上位机：XCOM 或 SSCOM 串口助手，**115200、8N1**，勾上"接收数据以 ASCII 显示"；
   务必用 XCOM/SSCOM 这类不主动拉 DTR/RTS 的助手，否则可能把 MCU 复位。
5. 源码是 GBK 编码，若中文注释在你编辑器里显示成乱码，请用 GBK/ANSI 打开（README 本身是 UTF-8）。

## 七、实验现象

1. 上电后 LED0 每 200ms 闪一次，LCD 显示 `STM32` / `DMA TEST` / `ATOM@ALIENTEK` / `KEY0:Start`。
2. 打开串口助手（115200），按一下 **KEY0**：
   - 串口先收到一行 `\r\nDMA DATA:\r\n`（这句是 `printf` 走普通发送发的，不是 DMA）；
   - 紧接着是 200 行 `正点原子 STM32F103 DMA 串口实验`（每行末尾带 `\r\n`），总计 6800 字节，约 0.59 秒发完；
     源码是 GBK 编码，字符串里的中文就是以 **GBK 双字节**发出去的，所以串口助手要选 GBK/ANSI 才显示正常，
     选 UTF-8 会看到乱码（数据本身没错，只是解码方式不对）；
   - LCD 第 130 行显示 `Start Transimit....`，第 150 行的数字从 0 一路涨到 100，最后显示 `Transimit Finished!`。
3. 在传输的这 0.6 秒里，DMA 在独立搬运数据；本例程此时刷 LCD，LED0 因为卡在等待循环里不会闪，
   把这段等待改成中断驱动（见第九章延伸），就能看到"边发数据边点灯"的效果。
4. 再按 KEY0 可以重复发送，进度从 0 重新走到 100。

## 八、注意事项

- **DMA 不动的排查顺序**：① `RCC->AHBENR` bit0 有没有开 DMA1 时钟；② `USART1->CR3` bit7（DMAT）有没有置 1；
  ③ 通道选对没（USART1_TX 必须是 **DMA1_Channel4**）；④ `CPAR` 是不是 `&USART1->DR`；
  ⑤ `CCR` 的 DIR 是不是 1（想发送却填了 0，就变成往外设的"读"，什么都发不出）；⑥ `CNDTR` 是不是 0。
- **`CNDTR` 只能在 EN=0 时写**：直接改正在运行的通道值不会生效，务必按 `dma_enable()` 的"关 → 等 → 写 → 开"来。
- **长度上限 65535**：`CNDTR` 是 16 位，`dma_enable()` 的形参也是 `uint16_t`；要发更多数据必须分多批。
- **`printf` 与 DMA 共用 `USART1->DR` 会打架**：`printf` 是 CPU 轮询 TXE 后写 `DR`（[usart.c](Drivers/SYSTEM/usart/usart.c)）。
  DMA 发送期间再调 `printf`，两股数据会混在一起。所以本工程只在启动 DMA **之前** printf 一次，
  等待 DMA 期间绝对不要再 printf。
- **`USART1->CR3 = 1 << 7;` 是整体赋值**：它把 CR3 其他位一起清了。本工程的 `usart_init()` 没用到 CR3 的其他功能，
  所以无碍；若你自己的工程用了 HDMAEN、CTS 等，要改成 `|= 1 << 7`。
- **标志清除是"写 1"**：`DMA1->IFCR |= 1 << 13;`。位号记不住就按公式 `(n-1)*4 + 1`（n 是通道号）。
- **半传输/错误标志**：如果想知道"搬了一半"，判 `ISR` bit14（HTIF4）；出错（外设 FIFO/总线问题）判 bit15（TEIF4），
  本工程未处理错误中断。
- **缓冲区生命周期**：`g_sendbuf` 是全局数组（在 SRAM 里，DMA 搬完全程它都得活着）。
  把它写成函数内的局部数组、函数返回后再启动 DMA，是经典 bug（栈可能被覆盖）。
- **官方 readme.txt 与代码一致**："DMA1 映射到串口1发送"与代码里的 `DMA1_Channel4` 完全对应，
  KEY0 触发、115200bps、仅支持 MCU 屏等说明也与代码相符。唯一容易被误读的是"实验现象 1：按下 KEY0，串口 1 以 DMA 方式发送数据"
  之外还有一句 `printf("\r\nDMA DATA:\r\n")` 是普通发送，不是 DMA 发的。
- **本实验用轮询而非中断**：`CCR` 的 TCIE（bit1）没有置 1，也没配 `DMA1_Channel4_IRQn` 的 NVIC，
  所以"传输完成"是靠软件反复读 `ISR` bit13 得到的。这在数据量小、CPU 不忙的场合够用。

## 九、总结与延伸

**本实验掌握的要点**：

1. DMA 搬运三要素 = `CPAR`（外设地址）+ `CMAR`（存储器地址）+ `CNDTR`（次数），模式全在 `CCR`。
2. 方向由 `DIR` 决定（1 = 内存→外设，0 = 外设→内存）；`MINC` 让缓冲区地址递增、`PINC` 一般保持 0（外设只有一个数据口）。
3. 外设的 DMA 请求是硬连线的：USART1_TX 只能走 DMA1_Channel4，ADC1 只能走 DMA1_Channel1。
4. 外设侧还要开自己的 DMA 请求使能位——串口这边就是 `USART1->CR3` 的 **DMAT**。
5. 启动 DMA 的正确节奏：关 EN → 等 EN=0 → 写 CNDTR → 置 EN=1；完成后判 `ISR` 的 TCIF 位、写 `IFCR` 清位。
6. DMA 的意义不在"更快"（发送速度由 115200bps 决定），而在于搬运期间**把 CPU 解放出来干别的活**。

**延伸小练习**：

1. **改成中断驱动**：在 `dma_usart1_tx_config()` 里加 `dmax_chy->CCR |= 1 << 1;`（TCIE），
   用 `sys_nvic_init(3, 3, DMA1_Channel4_IRQn, 2)` 开 NVIC，再写 `DMA1_Channel4_IRQHandler()`：在里面清 TCIF 标志、
   打印 "DMA 完成"。参考隔壁实验16-2 的 `ADC_ADCX_DMACx_IRQHandler()` 写法（判 TC → 置软件标志 → 清 TC）。
   改完就能在主循环里让 LED0 继续闪，直观看到 CPU 与 DMA 并行。
2. **改数据量**：把 `SEND_BUF_SIZE` 的倍数从 200 改成 20，看传输时间按比例缩短（约 0.06 秒），验证 10 位/字节的计算。
3. **加半传输提示**：轮询 `DMA1->ISR & (1 << 14)`，在正好搬完一半时让 LED1 亮一下，理解 HTIF 与 TCIF 的区别；
   再进一步可把 `DIR` 改成 0、置 `USART1->CR3 |= 1 << 6;`（DMAR），做一版"DMA 收数据"（对照实验16 的 ADC DMA 结构）。

**衔接下一课**：实验16 ADC 实验会用同一个 `dma.c` 把这套机制反过来用（ADC1 → DMA1_Channel1 → 数组，DIR=0、宽度 16 位），
并且改用传输完成中断通知 CPU。理解了本实验的"三要素 + CCR 模式位 + ISR/IFCR 标志"，
ADC+DMA、串口收发、SPI 大数据量传输都只是换外设地址和通道号而已。
