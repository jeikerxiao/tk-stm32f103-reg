# 实验25 无线通信实验

> 一句话概括：用 SPI 总线驱动 NRF24L01 无线模块，一块板当发送端、一块板当接收端，KEY0/KEY1 现场选角色，把 32 字节数据包在 2.4GHz 空中来回扔——顺便把 SPI 主机寄存器配置彻底拆明白。

## 一、实验简介

- **实验目的**（官方 `readme.txt`）：学习 NRF24L01 2.4G 无线模块和 SPI 的使用。
  通俗讲：NRF24L01 是一颗"串口进、天线出"的无线收发芯片，单片机只要用 SPI 把数据交给它，
  它就自己完成打包、发射、自动应答、重发。你要搞懂两件事：
  ① SPI 主机怎么配（时钟/相位/分频/片选）；② 无线通信为什么必须"两边约定一致"（地址、通道、数据宽度、速率）。
- **在学习路线中的位置**：
  - 前置：**实验2 按键输入**（用 KEY0/KEY1 选模式）、**实验11 TFTLCD**（显示收发内容）、
    **实验20 SPI 实验**（本工程里 `spi.c` 就是它的同一份驱动，强烈建议先看）。
  - 后续：实验28 SD 卡、实验29 FATFS 也用 SPI，但换成"多设备共享总线"的场景，可与本实验对照。
- **开发板与芯片**：正点原子 MiniSTM32 V4（STM32F103RC，Cortex-M3 @72MHz，256KB FLASH / 48KB RAM）。
  **本实验需要两块开发板 + 两个 NRF24L01 模块**（官方 `readme.txt` 第 6、8 条明确要求），一块烧 TX 程序、一块烧 RX 程序。

> 小概念：**SPI** 是"4 根线的同步移位寄存器"——SCK 出时钟，MOSI 主机发、MISO 从机发，
> 片选 CS 拉低就开始移位；发一个字节的同时必然收到一个字节，所以叫"读写一体"。

## 二、实验原理

### 2.1 NRF24L01 是什么，解决什么问题

NRF24L01 是 2.4GHz 免 license 频段的无线收发芯片，特点：

| 项目 | 本实验的取值 | 出处 |
| --- | --- | --- |
| 工作频道 | `RF_CH = 40` → 中心频率 2400 + 40 = **2440MHz** | `nrf24l01_rx_mode()` / `nrf24l01_tx_mode()` |
| 空中速率 | `RF_SETUP = 0x0f` → **2Mbps**（代码注释"0db增益,2Mbps"） | 同上 |
| 地址宽度 | `TX_ADR_WIDTH/RX_ADR_WIDTH = 5` 字节 | [nrf24l01.h](Drivers/BSP/NRF24L01/nrf24l01.h) |
| payload 宽度 | `TX_PLOAD_WIDTH/RX_PLOAD_WIDTH = 32` 字节 | 同上 |
| 自动应答/重发 | `EN_AA = 0x01`（通道0）、`SETUP_RETR = 0x1a` | `nrf24l01_tx_mode()` |
| CRC | `CONFIG` 里使能，16 位 CRC | `nrf24l01_rx_mode()/tx_mode()` |

它内部有 6 个数据通道（pipe0~pipe5），本实验只用 **通道 0**。芯片自己带 FIFO（收发各 3 层深），
所以主机不用"实时喂数据"，写好一包、拉一下 CE 就能走开——这就是所谓的 **ShockBurst 模式**。

### 2.2 硬件接口：6 根线

模块与单片机的全部连接（SPI 3 根 + 控制 3 根）：

| 模块引脚 | 作用 | 单片机引脚（代码为准） | 代码宏 |
| --- | --- | --- | --- |
| SCK | SPI 时钟 | **PA5** | `SPI1_SCK_GPIO_PIN = SYS_GPIO_PIN5`，[spi.h](Drivers/BSP/SPI/spi.h) |
| MISO | 从机→主机数据 | **PA6** | `SPI1_MISO_GPIO_PIN = SYS_GPIO_PIN6`（代码按复用推挽配的，见第八章） |
| MOSI | 主机→从机数据 | **PA7** | `SPI1_MOSI_GPIO_PIN = SYS_GPIO_PIN7` |
| CE | 收发使能/触发发送 | **PA4** | `NRF24L01_CE_GPIO_PIN = SYS_GPIO_PIN4` |
| CSN | SPI 片选（低有效） | **PC4** | `NRF24L01_CSN_GPIO_PIN = SYS_GPIO_PIN4` |
| IRQ | 中断/状态输出（低有效） | **PA1** | `NRF24L01_IRQ_GPIO_PIN = SYS_GPIO_PIN1` |
| VCC/GND | 供电 | 3.3V | 模块插在开发板 **P2** 座子（官方说明"朝外插"） |

**⚠️ 代码与官方说明不一致**：官方 `readme.txt` 写的是"IRQ(PA4)/CS(PC4)/CE(PA1)"，
而代码里是 **CE = PA4、IRQ = PA1**（正好对调）。本 README 一律按代码：
`NRF24L01_CE(x)` 操作 `GPIOA` 的 bit4，`NRF24L01_IRQ` 读 `GPIOA` 的 bit1。
模块插板上的 P2 座时由硬件走线决定，不必手动接线；只有你自己飞线时才要按代码的引脚接。

另外 `nrf24l01_init()` 里多做了一件事：把 **PB12 配成推挽输出并置 1**。
PB12 是板上 SPI FLASH（25QXX）的片选，拉高是为了让 25QXX 释放 SPI 总线，防止它和 NRF24L01 同时响应：

```c
/* 针对PB12(F_CS) 进行设置, 避免SPI FLASH 影响NRF24L01操作 */
sys_gpio_set(GPIOB, SYS_GPIO_PIN12, SYS_GPIO_MODE_OUT, SYS_GPIO_OTYPE_PP, SYS_GPIO_SPEED_HIGH, SYS_GPIO_PUPD_PU);
sys_gpio_pin_set(GPIOB, SYS_GPIO_PIN12, 1); /* PB12 输出1 放置25QXX干扰NRF24L01使用 */
```

### 2.3 SPI 主机怎么配（寄存器版）

STM32F1 的 SPI 靠一个 `SPI1->CR1` 就说完全部配置：

| CR1 位 | 名称 | 本工程写法 | 含义 |
| --- | --- | --- | --- |
| bit0 | CPHA | `spi1_init()` 置 1，`nrf24l01_init()` 清 0 | 数据在第几个时钟边沿采样 |
| bit1 | CPOL | `spi1_init()` 置 1，`nrf24l01_init()` 清 0 | 空闲时 SCK 的电平 |
| bit2 | MSTR | 1 | 主机模式（时钟由我们产生） |
| bit5:3 | BR[2:0] | 初值 7（÷256），检测时改成 4（÷32） | SCK = PCLK2 ÷ 2^(BR+1) |
| bit6 | SPE | 1 | SPI 使能（改 BR/CPOL/CPHA 前要先清 0） |
| bit7 | LSBFIRST | 0 | 高位先发 |
| bit8 | SSI | 1 | 内部主机片选拉高，避免模式错误 |
| bit9 | SSM | 1 | 片选交给软件（我们用 PC4 手动控制 CSN） |
| bit10 | RXONLY | 0 | 全双工（收 + 发都用） |
| bit11 | DFF | 0 | 8 位数据帧 |

- **先 mode 3 再改 mode 0**：`spi1_init()` 把 CPOL/CPHA 都置 1（通用 SPI FLASH 用 mode 3），
  紧接着 `nrf24l01_init()` 针对 NRF24L01 把它们清 0：

```c
SPI1->CR1 &= ~(1 << 6);     /* SPE=0,SPI设备失能 */
SPI1->CR1 &= ~(1 << 1);     /* CPOL=0,空闲状态下,SCK为低电平 */
SPI1->CR1 &= ~(1 << 0);     /* CPHA=0,数据采样从第1个时间边沿开始 */
SPI1->CR1 |= 1 << 6;        /* SPE=1,SPI设备使能 */
```

  记住顺序：**改 CR1 的使能/时钟相关位之前，必须先 SPE=0，改完再 SPE=1**，否则不生效。
- **SCK 速度**：`spi1_set_speed(SPI_SPEED_32)`（`SPI_SPEED_32 = 4`）→ BR=4 → 分频 2^(4+1)=32。
  SPI1 挂在 **APB2（PCLK2 = 72MHz）**上，所以 SCK ≈ **72/32 = 2.25MHz**（NRF24L01 的 SPI 上限是 10MHz，安全）。
- **读写一个字节**（[spi.c](Drivers/BSP/SPI/spi.c)）：

```c
uint8_t spi1_read_write_byte(uint8_t txdata)
{
    while ((SPI1_SPI->SR & 1 << 1) == 0);   /* 等待发送区空 */
    SPI1_SPI->DR = txdata;                  /* 发送一个byte */
    while ((SPI1_SPI->SR & 1 << 0) == 0);   /* 等待接收完一个byte */
    return SPI1_SPI->DR;                    /* 返回收到的数据 */
}
```

  即"写 DR → 等 SR 的 TXE(bit1) → 等 RXNE(bit0) → 读 DR"。SPI 没有"只发不收"，
  所以下层命令读寄存器时要发一个 dummy 字节（`0XFF`）来"换"回数据。

### 2.4 NRF24L01 的命令与寄存器

SPI 之上的"协议"就是首字节命令码（[nrf24l01.h](Drivers/BSP/NRF24L01/nrf24l01.h) 里的宏）：

| 命令宏 | 值 | 用途 |
| --- | --- | --- |
| `NRF_READ_REG` | 0x00 | 读配置寄存器，低 5 位是寄存器地址 |
| `NRF_WRITE_REG` | 0x20 | 写配置寄存器，低 5 位是寄存器地址 |
| `RD_RX_PLOAD` | 0x61 | 从 RX FIFO 读出收到的包 |
| `WR_TX_PLOAD` | 0xA0 | 把要发的包写进 TX FIFO |
| `FLUSH_TX` / `FLUSH_RX` | 0xE1 / 0xE2 | 清空 TX / RX FIFO |
| `REUSE_TX_PL` | 0xE3 | 反复重发上一包 |
| `NOP` | 0xFF | 空操作，常用来只取状态字节 |

关键寄存器（本实验真的写了这几个）：

| 寄存器宏 | 地址 | 本实验写入值 | 作用 |
| --- | --- | --- | --- |
| `CONFIG` | 0x00 | RX 模式 `0x0f`、TX 模式 `0x0e` | 收发模式、上电、CRC 使能与宽度 |
| `EN_AA` | 0x01 | 0x01 | 通道0 自动应答（ACK）使能 |
| `EN_RXADDR` | 0x02 | 0x01 | 允许通道0 接收 |
| `SETUP_RETR` | 0x04 | 0x1a（仅 TX） | 自动重发次数与间隔 |
| `RF_CH` | 0x05 | 40 | 工作频道（2440MHz） |
| `RF_SETUP` | 0x06 | 0x0f | 空中速率 2Mbps、发射功率、LNA 增益 |
| `STATUS` | 0x07 | 读回后再原样写回 | 中断标志 + 收到的管道号 |
| `RX_ADDR_P0` | 0x0A | 5 字节地址 | 通道0 的接收地址 |
| `TX_ADDR` | 0x10 | 5 字节地址 | 发送目标地址 |
| `RX_PW_P0` | 0x11 | 32 | 通道0 一包多少字节 |

**地址为什么两边一样？** 代码里：

```c
const uint8_t TX_ADDRESS[TX_ADR_WIDTH] = {0x34, 0x43, 0x10, 0x10, 0x01};    /* 发送地址 */
const uint8_t RX_ADDRESS[RX_ADR_WIDTH] = {0x34, 0x43, 0x10, 0x10, 0x01};    /* 发送地址 */
```

两个数组内容完全相同。发送端把它写进 `TX_ADDR`（"我发给谁"），接收端写进 `RX_ADDR_P0`（"我在听谁"），
两边必须一致才能对上；同时发送端还要把同一个地址写进自己的 `RX_ADDR_P0`（代码注释："设置RX节点地址,主要为了使能ACK"），
因为自动应答是"接收端把 ACK 发回发送端的通道0"，地址不同就收不到 ACK，会被判为发送失败。

### 2.5 发送 / 接收模式切换与 CE、FIFO 的配合

`mode` 由按键决定，之后调用不同函数：

**接收（`nrf24l01_rx_mode()`）**：

1. `NRF24L01_CE(0)`：先停下发/收，才能改配置。
2. 写 `RX_ADDR_P0` = 5 字节地址；`EN_AA=0x01`、`EN_RXADDR=0x01`、`RF_CH=40`、`RX_PW_P0=32`、`RF_SETUP=0x0f`。
3. `CONFIG=0x0f`：按代码注释即"PWR_UP,EN_CRC,16BIT_CRC,**接收模式**"。
4. `NRF24L01_CE(1)`：**CE 拉高，芯片进入 RX 监听状态**，之后不用管，收到包就会把 IRQ 拉低。

**发送（`nrf24l01_tx_mode()`）**：

1. `NRF24L01_CE(0)`。
2. 写 `TX_ADDR` 和 `RX_ADDR_P0`（后者为了收 ACK）。
3. `EN_AA`/`EN_RXADDR`/`SETUP_RETR`/`RF_CH`/`RF_SETUP` 逐个配置。
4. `CONFIG=0x0e`：同样的上电+CRC 设置，但**收发位置 0 → 发射模式**。
5. `NRF24L01_CE(1)`：注释写"CE为高,10us后启动发送"——CE 变高只是让芯片进入待发射状态，
   真正发包是每次 `nrf24l01_tx_packet()` 里那个"CE 0 → 写 FIFO → CE 1"的短脉冲。

**发一包（`nrf24l01_tx_packet()`）**：

```c
NRF24L01_CE(0);                                             /* 使能24L01 */
nrf24l01_write_buf(WR_TX_PLOAD, ptxbuf, TX_PLOAD_WIDTH);    /* 写数据到TX BUF  TX_PLOAD_WIDTH个字节 */
NRF24L01_CE(1);                                             /* 启动发送 */

while (NRF24L01_IRQ != 0);                                  /* 等待发送完成 */
sta = nrf24l01_read_reg(STATUS);                            /* 读取状态寄存器的值 */
nrf24l01_write_reg(NRF_WRITE_REG + STATUS, sta);            /* 清除TX_DS或MAX_RT中断标志 */
```

- 数据先进 **TX FIFO**（最多存 3 包 ×32 字节），CE 从 0→1 就是"开火"。
- 芯片自己完成：唤醒 → 前导码 → 发地址/数据 → 算 CRC → 等 ACK；失败就按 `SETUP_RETR` 重发。
- 事件完成后 **IRQ 变低**，主机用 `while (NRF24L01_IRQ != 0);` 死等这个下降沿。
- `STATUS` 的"读出来再原样写回去"是 NRF24L01 的惯例：**写 1 清除对应中断标志**。
- 结果判定：`sta & MAX_TX`（达到最大重发次数）→ 返回 1 并 `FLUSH_TX` 清 FIFO；`sta & TX_OK` → 返回 0 成功。

**收一包（`nrf24l01_rx_packet()`）**：不需要 CE 参与，主循环轮询即可：

```c
sta = nrf24l01_read_reg(STATUS);                            /* 读取状态寄存器的值 */
nrf24l01_write_reg(NRF_WRITE_REG + STATUS, sta);            /* 清除RX_OK中断标志 */
if (sta & RX_OK)                                            /* 接收到数据 */
{
    nrf24l01_read_buf(RD_RX_PLOAD, prxbuf, RX_PLOAD_WIDTH); /* 读取数据 */
    nrf24l01_write_reg(FLUSH_RX, 0xff);                     /* 清除RX FIFO寄存器 */
    rval = 0;                                               /* 标记接收完成 */
}
```

只有 `RX_OK` 置位才去 `RD_RX_PLOAD` 读 32 字节；读完立刻 `FLUSH_RX`，保证 FIFO 不会堆 3 包旧数据。

### 2.6 检测模块在不在：`nrf24l01_check()`

写一个"记号"再读回来比对，是外设在线检测的通用招：

```c
uint8_t buf[5] = {0XA5, 0XA5, 0XA5, 0XA5, 0XA5};
spi1_set_speed(SPI_SPEED_32);                           /* spi速度为7.5Mhz（24L01的最大SPI时钟为10Mhz） */
nrf24l01_write_buf(NRF_WRITE_REG + TX_ADDR, buf, 5);    /* 写入5个字节的地址. */
nrf24l01_read_buf(TX_ADDR, buf, 5);                     /* 读出写入的地址 */
```

往 `TX_ADDR` 写 5 个 0xA5，再读回来，5 个都相等才算存在。
（这一句也是**唯一**调用 `spi1_set_speed()` 的地方：SPI 初始 BR=7 ≈ 72/256 = 281kHz，
检测时提到 2.25MHz 后一直保持。）

## 三、硬件资源与接线

| 资源 | 引脚 | 代码中的宏/位置 | 说明 |
| --- | --- | --- | --- |
| SPI1 SCK / MISO / MOSI | PA5 / PA6 / PA7 | `SPI1_*_GPIO_PIN`，[spi.h](Drivers/BSP/SPI/spi.h) | 复用推挽输出（MISO 也按同一段代码配） |
| SPI1 时钟 | — | `SPI1_SPI_CLK_ENABLE()` → `RCC->APB2ENR bit12` | SPI1 在 APB2 上 |
| CE | PA4 | `NRF24L01_CE_GPIO_PIN` | 推挽输出，触发发送/进入 RX |
| CSN | PC4 | `NRF24L01_CSN_GPIO_PIN`，`RCC->APB2ENR bit4` | 软件片选，低电平期间传输 |
| IRQ | PA1 | `NRF24L01_IRQ_GPIO_PIN` | 上拉输入，事件发生时变低 |
| 25QXX 片选（避让） | PB12 | `nrf24l01_init()` 内直接操作 | 置 1 释放 SPI 总线 |
| KEY0 / KEY1 / WK_UP | PC5 / PA15 / PA0 | [key.h](Drivers/BSP/KEY/key.h) | KEY0=接收、KEY1=发送 |
| LED0 | PA8 | [led.h](Drivers/BSP/LED/led.h) | 心跳指示 |
| TFTLCD（MCU 屏） | PB0~PB15 + PC6~PC10 | [lcd.h](Drivers/BSP/LCD/lcd.h) | 显示收发内容 |
| 串口1 | PA9/PA10 | [usart.h](Drivers/SYSTEM/usart/usart.h) | 115200 |

**外接模块**：NRF24L01 插在开发板 **P2 位置（朝外插）**；需要 **2 套开发板 + 2 个模块**，各自插好、各自下载，
一块选 KEY1（发送），另一块选 KEY0（接收）。

**IRQ 的 PA1 冲突**：实验23 DS18B20、实验24 DHT11 的代码里 DQ 也在 PA1。做本实验时请把那两个模块拔掉。

**时钟树的意义**：`sys_stm32_clock_init(9)` → 72MHz；`PCLK2 = 72MHz` 直接决定 SPI1 的 SCK（÷32 → 2.25MHz）；
`delay_init(72)` 提供 `delay_ms(5)`、`delay_us(100)` 等节拍。

## 四、代码结构与调用流程

```text
实验25 无线通信实验/
├── User/main.c                       选模式(KEY0/KEY1) + 收/发循环
├── Drivers/BSP/NRF24L01/nrf24l01.c/.h  NRF24L01 驱动（寄存器读写、模式切换、收发一包）
├── Drivers/BSP/SPI/spi.c/.h          SPI1 主机驱动（与实验20 同一份代码）
├── Drivers/BSP/KEY/key.c/.h          KEY0/KEY1/WK_UP 扫描
├── Drivers/BSP/LED/led.c/.h          LED0 心跳
├── Drivers/BSP/LCD/lcd.c/.h          MCU 屏显示
├── Drivers/SYSTEM/{sys,delay,usart}  时钟/GPIO 封装、微秒毫秒延时、串口1
└── Projects/MDK-ARM/atk_f103.uvprojx Keil 工程
```

`main()` 流程（[User/main.c](User/main.c)）：

```text
1 sys_stm32_clock_init(9) → delay_init(72) → usart_init(72,115200)
2 led_init() → lcd_init() → key_init() → nrf24l01_init()
3 while (nrf24l01_check())  显示/清除 "NRF24L01 Error"（模块没插好就一直转）
4 显示 "NRF24L01 OK"
5 模式选择循环：key_scan(0)
    KEY0_PRES → mode=0(RX) ；KEY1_PRES → mode=1(TX)
    t==100 闪现提示 "KEY0:RX_Mode  KEY1:TX_Mode"，t==200 清屏（约 1s 一轮）
6a RX：nrf24l01_rx_mode() → 循环 nrf24l01_rx_packet(tmp_buf)，成功就把 tmp_buf[32]=0 并 lcd_show_string 显示
        每累计 10000 圈 LED0 翻转一次（约 1s 心跳）
6b TX：nrf24l01_tx_mode() → 循环 nrf24l01_tx_packet(tmp_buf)
        成功 → 显示 "Sended DATA:" + 内容，并把 32 字节内容整体往后推一个 ASCII 码
        失败 → 清屏并显示 "Send Failed "
        每包之后 LED0_TOGGLE() + delay_ms(200)
```

## 五、核心代码解析

### 5.1 片选与状态字节：一次 SPI 事务长什么样

```c
static uint8_t nrf24l01_read_buf(uint8_t reg, uint8_t *pbuf, uint8_t len)
{
    uint8_t status, i;
    NRF24L01_CSN(0);                            /* 使能SPI传输 */
    status = spi1_read_write_byte(reg);         /* 发送寄存器值(位置),并读取状态值 */
    for (i = 0; i < len; i++)
    {
        pbuf[i] = spi1_read_write_byte(0XFF);   /* 读出数据 */
    }
    NRF24L01_CSN(1);                            /* 关闭SPI传输 */
    return status;                              /* 返回读到的状态值 */
}
```

- `NRF24L01_CSN(0)` → `sys_gpio_pin_set(GPIOC, SYS_GPIO_PIN4, 0)` → `GPIOC->BSRR |= (1<<4)<<16`，片选拉低、开始一次事务。
  因为 CR1 里 `SSM=1/SSI=1`（软件片选），这根线完全由我们手控。
- **第一个字节既发也收**：发的是命令码 `reg`，收回来的是芯片当时的 `STATUS`——NRF24L01 每次事务都会顺带把状态字节吐回来。
- 后续每个字节都要写一个 dummy（读时写 `0XFF`）来"提供 8 个时钟"。
- 结尾 CSN 拉高结束事务。**任何一次读写寄存器都必须成对操作 CSN**，忘了拉高，下一次命令的字节会被当成上一条的延续。

### 5.2 收发模式切换：两个函数只差两处

`nrf24l01_rx_mode()` 与 `nrf24l01_tx_mode()` 的差别只有：

| 项 | RX | TX |
| --- | --- | --- |
| 地址写入 | `RX_ADDR_P0` | `TX_ADDR` + `RX_ADDR_P0`（为了 ACK） |
| `SETUP_RETR` | 不写 | 写 0x1a（重发次数/间隔） |
| `RX_PW_P0` | 写 32（告诉芯片一包多长） | 不写 |
| `CONFIG` | 0x0f（接收模式） | 0x0e（发射模式） |
| CE | 1 → 持续监听 | 1 → 上电待命，等 tx_packet 触发 |

```c
nrf24l01_write_reg(NRF_WRITE_REG + SETUP_RETR, 0x1a);   /* 设置自动重发间隔时间:500us + 86us;最大自动重发次数:10次 */
```

按 NRF24L01 的寄存器划分，`SETUP_RETR` 高 4 位是重发次数 ARC、低 4 位是重发间隔 ARD（时间 = 250×ARD + 86µs）；
`0x1a` 即 **ARC=1、ARD=10 → 间隔约 2586µs**。这里代码注释写的"500us+86us、最多 10 次"对应的高低 nibble 与实写值相反，
**以实际写入的 `0x1a` 为准**（详见第八章）。

### 5.3 发送端的数据是怎么造的（main.c TX 分支）

```c
if (nrf24l01_tx_packet(tmp_buf) == 0)   /* 发送成功 */
{
    lcd_show_string(30, 150, 239, 32, 16, "Sended DATA:", BLUE);
    lcd_show_string(0, 170, lcddev.width - 1, 32, 16, (char*)tmp_buf, BLUE);
    key = mode;
    for (t = 0; t < 32; t++)
    {
        key++;
        if (key > ('~')) key = ' ';
        tmp_buf[t] = key;
    }
    mode++;
    if (mode > '~') mode = ' ';
    tmp_buf[32] = 0; /* 加入结束符 */
}
```

- `mode` 初值 `' '`（0x20），每包整体内容 = 从 `mode+1` 开始的 32 个连续可打印 ASCII 字符，
  超过 `'~'`(0x7E) 就绕回空格。所以接收端会看到 `!"#$%&'()*+,-./0123456789:;<=>?` 这种字符在滚动，
  一眼就能确认"数据在动、没丢包"。
- `tmp_buf[32] = 0` 给 LCD 的字符串函数加结束符（缓冲区是 `uint8_t tmp_buf[33]`，正好放 32 字节数据 + 结束符）。
- 发送失败（`nrf24l01_tx_packet()` 返回 1）时只清屏显示 `Send Failed `，**不更新 tmp_buf**，下一包还是原内容。

### 5.4 接收端：轮询 + 1s 心跳

```c
while (1)
{
    if (nrf24l01_rx_packet(tmp_buf) == 0)   /* 一旦接收到信息,则显示出来. */
    {
        tmp_buf[32] = 0;    /* 加入字符串结束符 */
        lcd_show_string(0, 170, lcddev.width - 1, 32, 16, (char*)tmp_buf, BLUE);
    }
    else delay_us(100);
    t++;
    if (t == 10000) { t = 0; LED0_TOGGLE(); }
}
```

- 没收到数据就 `delay_us(100)` 再轮询，避免把 CPU 100% 占用在 SPI 读寄存器上（也给 LCD 显示留时间）。
- `t` 是 `uint16_t`，累计 10000 次翻转一次 LED0，用来确认程序活着（注意：这个周期跟收包快慢有关，不是严格的 1s）。
- 接收内容只覆盖 `(0,170)` 起两行，旧数据不会自动清掉，看到的是"最近一包"。

## 六、编译、下载与运行

1. Keil MDK 打开 `Projects/MDK-ARM/atk_f103.uvprojx`（工程源文件：startup_stm32f103xe.s、main.c、
   delay/sys/usart、led/lcd/key/spi/nrf24l01）。
2. 器件 `STM32F103RC`，包含路径已配好，直接 Rebuild。
3. 输出目录在工程里设为 `..\..\Output\`，即**实验根目录下的 `Output/`**，产物名 `atk_f103`（`.axf`/`.hex`，已勾选生成 HEX）。
4. 下载用 ST-LINK / J-LINK（SWD 接口）即可；**两块板都要下载同一份程序**，上电后用按键选角色：
   一块按 KEY1（TX），一块按 KEY0（RX）。
5. 串口 115200 可选，本实验现象主要看 LCD；想调试可用 `printf`（`usart.c` 已重定向 `fputc`）。

## 七、实验现象

1. 上电：LCD 依次出现 `STM32` / `NRF24L01 TEST` / `ATOM@ALIENTEK`。
2. 模块没插好/插反：第 4 行反复 `NRF24L01 Error`（`nrf24l01_check()` 读不回 5 个 0xA5），程序卡在检测循环。
3. 检测通过：显示 `NRF24L01 OK`，随后约每 1 秒闪现一次红字 `KEY0:RX_Mode  KEY1:TX_Mode`。
4. 按下 **KEY1** 的板子：蓝底区显示 `NRF24L01 TX_Mode`、`Sended DATA:` 和一串滚动字符，LED0 每包翻转一次（约 200ms 一包）。
5. 按下 **KEY0** 的板子：显示 `NRF24L01 RX_Mode` / `Received DATA:`，170 行位置显示收到的那 32 个字符，内容与发送端一致并同步滚动。
6. 两板距离拉远（十几米、隔墙）：发送端开始出现 `Send Failed `，把距离拉近又恢复——这就是自动应答超时（`SETUP_RETR`）耗尽的表现。
7. 只有一块板时：发送端永远 `Send Failed `（没人给它 ACK），接收端永远收不到——**本实验必须两套板子**。

## 八、注意事项

- **官方 `readme.txt` 与代码不一致（以代码为准）**：readme 写"IRQ(PA4)/CS(PC4)/CE(PA1)"，
  代码是 **CE=PA4、IRQ=PA1、CSN=PC4**。插 P2 座由硬件保证，自己飞线要按代码接。
- **spi.c/nrf24l01.c 的注释有两处与实际值不符**：
  - `spi1_set_speed()` 注释"SPI1时钟选择来自APB1, 即PCLK1, 为36Mhz"——**SPI1 挂在 APB2（PCLK2=72MHz）**（见 `spi.h`
    里 `RCC->APB2ENR |= 1 << 12`）。所以 `SPI_SPEED_32` 实际是 72/32 ≈ **2.25MHz**，而不是 `nrf24l01_check()` 注释写的 7.5MHz。
  - `spi1_init()` 里 `tempreg |= 1 << 1;` 的注释是"CPOL = 0"，实际置的是 CPOL=1（mode 3）；
    随后 `nrf24l01_init()` 才把 CPOL/CPHA 清成 mode 0。另外 `tempreg |= 0 << 10;` 注释写"RXONLY = 1"，实际写的是 0（全双工）。
  - `nrf24l01_tx_mode()` 里 `SETUP_RETR = 0x1a` 的注释（500+86µs、最多 10 次）与按 nibble 拆分的数值（1 次、约 2586µs）不一致。
  这些都不影响功能，但你**改代码时请相信自己算出来的值，不要相信注释**。
- **两边参数必须完全一致**：地址（`TX_ADDRESS`/`RX_ADDRESS`）、`RF_CH`、`RX_PW_P0`/`TX_PLOAD_WIDTH`、`RF_SETUP` 里的空中速率，
  任何一边改动都会"发得出、收不到"。头文件也特别写了这句提醒：
  `发送端&接收端必须保持一致, 否则将导致通信失败!!!!`
- **MISO（PA6）的 GPIO 模式**：`spi1_init()` 把 SCK/MISO/MOSI 三根线都用
  `SYS_GPIO_MODE_AF + SYS_GPIO_OTYPE_PP` 配成了复用推挽；而参考手册对 **SPI 主机**的建议是
  SCK/MOSI 用复用推挽、**MISO 用浮空或上下拉输入**（MISO 的数据方向是从机→主机）。
  本工程的写法能正常跑通（同一份 `spi.c` 在实验20 里也一样用），但你如果要把这段驱动移植到别的项目，
  建议把 MISO 单独配成输入模式，更符合手册、也避免与其它从机争抢总线。
- **NRF24L01 对电源很敏感**：只用 3.3V，发射瞬间电流可达上百 mA，电源纹波大时会表现为"偶尔失败/检测不到"。
  建议模块座插紧、必要时在 VCC/GND 间加 10µF 退耦电容；不要用杜邦线长距离供电。
- **PB12（25QXX 片选）必须为高**：`nrf24l01_init()` 已经帮你置高了；如果你在本实验里又去初始化 SPI FLASH，
  会把两条 SPI 从机同时挂在总线上，出现莫名数据错乱。
- **发送流程里的 `while (NRF24L01_IRQ != 0);` 是死等**：如果模块坏/天线没匹配、连"最大重发次数中断"都不产生，
  程序会卡在这句上。调试时可以用串口打点，或给它加超时。
- **`nrf24l01_check()` 会改 SPI 速度**：它是唯一调用 `spi1_set_speed()` 的地方，跑过一次之后速度就固定为 2.25MHz；
  若你把 check 挪到别处或删掉，SPI 会停在 `spi1_init()` 的最低速（BR=7 ≈ 281kHz），照样能通信，只是慢。
- **KEY1 在 PA15**：PA15 复位后默认被 JTAG 占用（JTDI）。本工程用 SWD 调试，一般可直接当按键；
  若你发现 KEY1 读不到，检查是否启用了 `sys_gpio_remap_set(24, 3, 2)`（关闭 JTAG、保留 SWD）。
- **中断标志要记得清**：STATUS 读出来必须原样写回去清标志，否则会反复判定同一个事件。
- **源码为 GBK 编码**：请用 GBK/GB2312 打开源文件（本 README 为 UTF-8）。

## 九、总结与延伸

**本实验要点：**

1. SPI 主机四步：开时钟（`RCC->APB2ENR bit12`）→ 三个引脚配复用推挽 → 写 `CR1`（MSTR/BR/SSM/SSI/DFF/CPOL/CPHA）→ `SPE=1`；
   读写一体：`while(TXE) → DR=数据 → while(RXNE) → 读DR`。
2. NRF24L01 的"协议"就是命令码：`0x20|reg` 写寄存器、`0x00|reg` 读寄存器、`0xA0` 写 TX FIFO、`0x61` 读 RX FIFO、
   `0xE1/0xE2` 清 FIFO；每次事务用 CSN 包起来，第一字节换回 STATUS。
3. 模式切换靠 `CONFIG`（0x0e 发 / 0x0f 收），**改配置前 CE=0，配置完 CE=1**；发送用 CE 的 0→1 触发一包。
4. 自动应答 + 重发（`EN_AA`/`SETUP_RETR`）让"发送成功"有明确依据：TX_DS 标志＝对方回了 ACK，MAX_RT＝重头发数用完＝失败。
5. 无线通信"两边一致"是铁律：地址、通道 2440MHz、32 字节 payload、2Mbps 速率，四项全对齐才能通。

**延伸小练习：**

1. 把频道从 40 改成 60：在 `nrf24l01_rx_mode()` 与 `nrf24l01_tx_mode()` 里同时改 `RF_CH` 的值
   （中心频率变成 2460MHz），验证"只改一边 → 收不到，两边都改 → 正常"。
2. 把 payload 从 32 字节改成 16 字节：同步改 `TX_PLOAD_WIDTH`/`RX_PLOAD_WIDTH`、`main.c` 的填充循环与 `tmp_buf[]` 下标，
   再顺手把统计丢包率的计数器 `ok_cnt`/`err_cnt` 加上（每 100 包 `printf` 一次），对比 `SETUP_RETR` 高 nibble
   （重发次数，例如 `0x5a`＝5 次）对成功率的影响。
3. 用 IRQ + 外部中断替代轮询：把 PA1 配成下降沿触发的 EXTI（`sys_nvic_ex_config()`），
   在 ISR 里置一个标志位，主循环只在标志位为 1 时调用 `nrf24l01_rx_packet()`，体会"轮询 vs 中断"的差别。

**衔接**：SPI 的同一份 [Drivers/BSP/SPI/spi.c](Drivers/BSP/SPI/spi.c) 在实验20（SPI 基础）、实验25（NRF24L01）、
实验28（SD 卡）、实验29（FATFS）里反复出现；把本实验的 `nrf24l01_write_buf()/read_buf()` 与 25QXX 的读写时序对照看，
你会发现"SPI 从机各不相同，但主机那 4 行代码是一样的"。
