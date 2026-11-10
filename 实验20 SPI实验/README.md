# 实验20 SPI实验

> 一句话概括：用 STM32 硬件 SPI1 外设驱动板载 SPI NOR FLASH（25QXX 系列）芯片，完成"读 ID、写入、读出、擦除"等操作，并在 LCD 上显示结果，学会主机怎么用 SPI 总线跟一颗从机芯片"讲话"。

## 一、实验简介

- **实验目的**（官方 `readme.txt`）：学习 STM32 SPI 外设的使用，驱动板载 SPI FLASH（NM25Q64/NM25Q128/W25Q128 等 25QXX 系列）实现数据读写。
  通俗地说：SPI 是一种"一问一答、时钟驱动"的芯片间通信方式，本实验教你用 STM32 当**主机**，去读写一颗挂在 SPI 总线上的**存储芯片**，把字符串存进去、再取出来显示在屏幕上。
- **在学习路线中的位置**：
  - 前置：实验1 跑马灯（GPIO 输出）、按键实验（GPIO 输入）、串口实验（USART）、LCD 实验（并口驱动）、定时器/延时实验。
  - 后续：实验21 触摸屏（电阻屏就是"软件模拟 SPI"驱动 XPT2046）、实验 SD 卡/文件系统（SPI 模式）。理解了本实验的 SPI 时序，后面的 FLASH/触摸/SD 卡都不再神秘。
- **开发板与芯片**：正点原子 MiniSTM32 V4 开发板，主芯片 STM32F103RC，主频 72MHz。

> 先建立一个概念：**寄存器就是 CPU 内部一个可读写的小格子**。外设（SPI、GPIO、定时器）就是一堆这样的格子，"配置外设"就是按位往这些格子里填数字。本实验最关键的两个格子是 `SPI1->CR1`（控制寄存器1）和 `SPI1->SR`（状态寄存器）。

## 二、实验原理

### 2.1 SPI 是什么

SPI（Serial Peripheral Interface，串行外设接口）用 4 根线在主机和从机之间传数据：

| 信号线 | 全称 | 谁控制 | 作用 |
| --- | --- | --- | --- |
| SCK | Serial Clock 时钟 | 主机 | 提供节拍，每来一个脉冲就交换 1 bit |
| MOSI | Master Out Slave In | 主机输出 | 主机发给从机的数据 |
| MISO | Master In Slave Out | 从机输出 | 从机发回主机的数据 |
| CS/NSS | Chip Select 片选 | 主机 | 拉低=选中这颗从机，拉高=不跟它说话 |

SPI 的精髓：**发送和接收是同时发生的**。主机发一个字节的同时，从机也往回送一个字节，所以硬件上叫"全双工"。写一个函数 `spi1_read_write_byte(txdata)` 既发又收，收不到有用数据时随便填个 `0xFF` 当"哑数据"占位即可。

### 2.2 关键参数：CPOL、CPHA、模式

时钟空闲时是高还是低（CPOL），以及"在时钟的哪个边沿采样数据"（CPHA），组合成 4 种 SPI 模式。**主从必须用同一种模式**，否则读出来全是乱码。本实验代码把 SPI1 配成 CPOL=1、CPHA=1（SPI 模式 3）。

> 提醒：`spi.c` 里 `tempreg |= 1 << 1;` 这一行真正设置的是 **CPOL = 1**，但它后面的注释却写着"CPOL = 0"。以代码为准——实际是 CPOL=1、CPHA=1。

### 2.3 寄存器版怎么配 SPI1

看 [Drivers/BSP/SPI/spi.c](Drivers/BSP/SPI/spi.c) 的 `spi1_init()`，它往 `SPI1->CR1` 里逐位填数：

```c
tempreg |= 0 << 11;   /* DFF    = 0 , 8 位数据格式 */
tempreg |= 0 << 10;   /* RXONLY = 0 , 全双工 */
tempreg |= 1 << 9;    /* SSM    = 1 , 软件管理片选 */
tempreg |= 1 << 8;    /* SSI    = 1 , 禁止变从机，即做主机 */
tempreg |= 0 << 7;    /* LSBFIRST = 0 , 先发 MSB */
tempreg |= 7 << 3;    /* BR[2:0] = 7 , 256 分频，最慢 */
tempreg |= 1 << 2;    /* MSTR   = 1 , 主机模式 */
tempreg |= 1 << 1;    /* 实际 CPOL = 1（注释写 0，以代码为准）*/
tempreg |= 1 << 0;    /* CPHA   = 1 , 第 2 个边沿采样 */

SPI1_SPI->CR1 = tempreg;   /* 一次性写入 CR1 */
SPI1_SPI->CR1 |= 1 << 6;   /* SPE = 1，使能 SPI */
```

CR1 每一位在本实验里的含义：

| CR1 位 | 名称 | 本实验值 | 含义 |
| --- | --- | --- | --- |
| bit11 | DFF | 0 | 数据宽度选 8 位（一帧传 1 字节） |
| bit10 | RXONLY | 0 | 全双工（既发又收） |
| bit9 | SSM | 1 | 用软件控制片选 |
| bit8 | SSI | 1 | 内部把 NSS 拉高，保证自己当主机（否则可能被误判为从机） |
| bit7 | LSBFIRST | 0 | 高位 MSB 先发 |
| bit6 | SPE | 1 | 使能整个 SPI 外设 |
| bit5:3 | BR[2:0] | 0~7 | 时钟分频，决定 SCK 频率 |
| bit2 | MSTR | 1 | 主机模式 |
| bit1 | CPOL | 1 | 空闲时 SCK 保持高电平 |
| bit0 | CPHA | 1 | 在第 2 个时钟边沿采样 |

本实验的片选脚（CS）没有用硬件 NSS，而是**用普通 GPIO PA2 软件控制**（见 3.1 表格与 `NORFLASH_CS(0/1)` 宏），拉低开始、拉高结束，更直观。

### 2.4 时钟频率计算（SCK）

SPI1 挂在 APB2 总线，时钟源是 `PCLK2 = 72MHz`。SCK 频率公式（见 `spi1_set_speed()` 注释）：

```
f(SCK) = PCLK2 / 2^(BR + 1)
```

| BR[2:0] | 分频 | SCK 频率 | 代码宏 |
| --- | --- | --- | --- |
| 0 | 2 | 72/2 = **36 MHz** | `SPI_SPEED_2`（值 0） |
| 1 | 4 | 18 MHz | `SPI_SPEED_4` |
| 2 | 8 | 9 MHz | `SPI_SPEED_8` |
| ... | ... | ... | ... |
| 7 | 256 | 72/256 ≈ **281.25 kHz** | `SPI_SPEED_256` |

`spi1_init()` 默认用 BR=7（最慢 ≈281.25 kHz）先把总线"热身"。真正读写 FLASH 前，`norflash_init()` 调用 `spi1_set_speed(SPI_SPEED_2)` 把 BR 改成 0，**实际工作 SCK = 36 MHz**（25QXX 系列最高能跑到 80~104 MHz，36 MHz 安全且快）。

`spi1_set_speed()` 为什么要先关 SPE、改 BR、再开 SPE？因为改分频时要求 SPI 处于失能状态，否则改动不生效：

```c
SPI1_SPI->CR1 &= ~(1 << 6);   /* SPE = 0，先关 */
SPI1_SPI->CR1 &= ~(7 << 3);   /* BR 清零 */
SPI1_SPI->CR1 |= speed << 3;  /* 设置新分频 */
SPI1_SPI->CR1 |= 1 << 6;      /* SPE = 1，再开 */
```

### 2.5 收发标志 TXE / RXNE

读写一个字节靠轮询 `SPI1->SR` 状态寄存器的两个标志位（[spi.c](Drivers/BSP/SPI/spi.c) 的 `spi1_read_write_byte()`）：

```c
while ((SPI1_SPI->SR & 1 << 1) == 0);   /* 等 TXE(bit1)=1：发送缓冲区已空，可以塞下一个字节 */
SPI1_SPI->DR = txdata;                  /* 写 DR，硬件自动开始发送 */
while ((SPI1_SPI->SR & 1 << 0) == 0);   /* 等 RXNE(bit0)=1：接收缓冲区收到一个完整字节 */
return SPI1_SPI->DR;                     /* 读 DR 即取回从机送来的数据（同时清 RXNE） */
```

- **TXE**（Transmit buffer Empty，发送缓冲空）=1 才允许写新数据，否则上一个还没发完就被覆盖。
- **RXNE**（Receive buffer Not Empty，接收非空）=1 才表示收到 1 字节，读 `DR` 就取走它。

这一发一收就是 SPI 的"握手节拍"。

### 2.6 25QXX SPI FLASH 的读写流程

NOR FLASH（25QXX）不能像内存那样直接改任意字节，必须"发命令 + 发地址 + 收发数据"。常用命令见 [norflash.h](Drivers/BSP/NORFLASH/norflash.h) 指令表：

| 指令宏 | 十六进制码 | 作用 |
| --- | --- | --- |
| `FLASH_WriteEnable` | 0x06 | 写使能（置位 SR1 的 WEL） |
| `FLASH_ReadStatusReg1` | 0x05 | 读状态寄存器1（看 BUSY/WEL） |
| `FLASH_SectorErase` | 0x20 | 扇区擦除（擦掉一个 4KB 扇区） |
| `FLASH_PageProgram` | 0x02 | 页编程（一页最多写 256 字节） |
| `FLASH_ReadData` | 0x03 | 普通读取 |
| `FLASH_ChipErase` | 0xC7 | 整片擦除 |
| `FLASH_ManufactDeviceID` | 0x90 | 读厂商 ID + 容量 ID |

**写入必须遵循的顺序：写使能 → 扇区擦除 → 页写（自动换页）**。为什么要先擦除？因为 NOR FLASH 的物理特性是**只能把位从 1 写成 0，不能把 0 写成 1**；擦除（Erase）才会把整块变成全 `0xFF`（全 1）。所以往一个已经有数据的地址写之前，必须先把那个扇区擦成 `0xFF`，写入才能成功。

`norflash_write()`（带擦除的安全写）内部做了这件事：
1. 把目标 4KB 扇区整块读进缓存 `g_norflash_buf[4096]`；
2. 检查要写的那段地址当前是不是全 `0xFF`；
3. 若不是（说明有旧数据），就 `norflash_erase_sector()` 擦掉整扇区，再把新数据"拼"进缓存，用 `norflash_write_nocheck()` 整扇区写回；
4. 若已经是 `0xFF`，就直接写；
5. 一次写不超过一个扇区，超出就自动翻到下一个扇区继续。

`norflash_write_nocheck()` 又负责**自动换页**：NOR FLASH 一页 256 字节，写入不能跨页，所以它按"单页剩余字节数"切分，一段段调 `norflash_write_page()`（每页内部各自发一次写使能 + 页命令 + 地址 + 数据，最后 `norflash_wait_busy()` 等写完）。

### 2.7 擦除等待与 BUSY 位

`norflash_wait_busy()` 不停读状态寄存器 1，只要最低位 BUSY=1 就说明芯片还在忙（擦除/编程进行中），一直等到它变 0：

```c
static void norflash_wait_busy(void) {
    while ((norflash_read_sr(1) & 0x01) == 0x01);   /* 等 BUSY 位清零 */
}
```

擦一个扇区最少约 150ms（见 `norflash_erase_sector()` 注释），整片擦除时间更长，所以擦除操作要耐心等。

## 三、硬件资源与接线

| 资源 | 引脚 | 代码中的宏/位置 | 说明 |
| --- | --- | --- | --- |
| SPI1_SCK | PA5 | `SPI1_SCK_GPIO_PIN`，[spi.h](Drivers/BSP/SPI/spi.h) | 时钟，复用推挽输出 |
| SPI1_MISO | PA6 | `SPI1_MISO_GPIO_PIN` | 从机→主机数据 |
| SPI1_MOSI | PA7 | `SPI1_MOSI_GPIO_PIN` | 主机→从机数据 |
| SPI FLASH 片选 CS | PA2 | `NORFLASH_CS_GPIO_PIN`，[norflash.h](Drivers/BSP/NORFLASH/norflash.h) | 软件控制，`NORFLASH_CS(x)` 拉低选中 |
| LED0（红灯） | PA8 | `LED0`，[led.h](Drivers/BSP/LED/led.h) | 低电平点亮，程序运行指示 |
| LED1（绿灯） | PD2 | `LED1` | 未在本实验主流程使用 |
| KEY0 | PC5 | `KEY0_PRES`，[key.h](Drivers/BSP/KEY/key.h) | 控制读取 FLASH |
| KEY1 | PA15 | `KEY1_PRES` | 控制写入 FLASH（注意 JTAG 冲突，见八） |
| KEY_UP(WK_UP) | PA0 | `WKUP_PRES` | 本实验未用 |
| 串口1 | PA9/PA10 | [usart.h](Drivers/SYSTEM/usart/usart.h) | USMART 调试，波特率 115200 |
| TFTLCD 模块 | 8080 并口 | [lcd.h](Drivers/BSP/LCD/lcd.h) | 仅支持 MCU 屏，不支持 RGB 屏 |

- SPI1 三个引脚在 `spi1_init()` 里用 `sys_gpio_set(..., SYS_GPIO_MODE_AF, ...)` 配成**复用推挽输出**，并开上拉。
- FLASH 片选 PA2 配成**普通推挽输出**，由软件拉低/拉高。
- 接线：25QXX 芯片已焊在开发板上，无需外接跳线；LCD 模块（如 2.8/3.5/7 寸 MCU 屏）插到对应 LCD 排座即可；串口调试通过板载 CH340，需 USART1 排针上 PA9→TXD、PA10→RXD 跳线帽连好。
- **时钟**：`sys_stm32_clock_init(9)` 把主频设为 72MHz（外部晶振 8MHz × PLL 倍频 9 = 72MHz）。这决定了 APB2(SPI1) 时钟 72MHz，是 2.4 节 SCK 频率计算的源头。

## 四、代码结构与调用流程

```
实验20 SPI实验/
├── User/main.c                    主程序：初始化→读ID→按键触发读写→LCD显示
├── Drivers/BSP/SPI/spi.[ch]       硬件 SPI1 驱动：初始化/调速/收发一字节
├── Drivers/BSP/NORFLASH/norflash.[ch]  25QXX FLASH 驱动：ID/读/写/擦
├── Drivers/BSP/KEY/key.[ch]       按键扫描
├── Drivers/BSP/LED/led.[ch]       LED
├── Drivers/BSP/LCD/lcd.[ch]       LCD 显示
├── Drivers/SYSTEM/{sys,delay,usart} 系统时钟/延时/串口
└── Middlewares/USMART/            串口命令行调试（可远程调用读ID/擦除）
```

`main()` 流程：

1. `sys_stm32_clock_init(9)` → 72MHz；
2. `delay_init / usart_init / usmart_dev.init / led_init / lcd_init / key_init`；
3. `norflash_init()`：配 CS 脚、`spi1_init()`、切到 36MHz、读 FLASH ID（若是 W25Q256 还会进 4 字节地址模式）；
4. LCD 打印标题与提示"KEY1:Write KEY0:Read"；
5. `norflash_read_id()`，若 ID=0 或 0xFFFF 说明没检测到芯片，循环报警 + LED0 闪；
6. 进入主循环 `while(1)`：扫描按键，KEY1→写一段字符串到 FLASH 末尾，KEY0→读出来显示；每循环 `i++`，`i==20` 时 LED0 翻转（约指示运行）。

## 五、核心代码解析

### 5.1 读写一个字节（SPI 的心脏）

见 2.5 节 `spi1_read_write_byte()`。它把"等发送空 → 写数据 → 等接收满 → 读数据"四步串起来，是全实验所有 FLASH 操作的最小单元。

### 5.2 读 ID

```c
uint16_t norflash_read_id(void) {
    NORFLASH_CS(0);
    spi1_read_write_byte(FLASH_ManufactDeviceID);  /* 0x90 命令 */
    spi1_read_write_byte(0); spi1_read_write_byte(0); spi1_read_write_byte(0); /* 24 位地址 0 */
    deviceid = spi1_read_write_byte(0xFF) << 8;    /* 厂商 ID 在高 8 位 */
    deviceid |= spi1_read_write_byte(0xFF);        /* 容量 ID 在低 8 位 */
    NORFLASH_CS(1);
    return deviceid;
}
```

返回的 16 位 = `厂商ID<<8 | 容量ID`，对照 `norflash.h` 芯片列表即可认芯片，例如 `NM25Q128=0x5217`、`W25Q128=0xEF17`、`NM25Q64=0x5216`、`W25Q64=0xEF16`。ID 为 0 或 0xFFFF 即代表没插好/没供上电。

### 5.3 写页（一次最多 256 字节）

```c
static void norflash_write_page(uint8_t *pbuf, uint32_t addr, uint16_t datalen) {
    norflash_write_enable();          /* 先写使能，置 WEL */
    NORFLASH_CS(0);
    spi1_read_write_byte(FLASH_PageProgram);  /* 0x02 */
    norflash_send_address(addr);      /* 发 24/32 位地址 */
    for (i = 0; i < datalen; i++)
        spi1_read_write_byte(pbuf[i]);/* 逐字节写入，最多 256 */
    NORFLASH_CS(1);
    norflash_wait_busy();             /* 等本页写完 */
}
```

`norflash_send_address()` 只有当芯片是 W25Q256 时才多发一个高字节（4 字节地址），其它 25QXX 发 3 字节地址（24 位）即可。

### 5.4 main.c 的读写触发（节选）

```c
id = norflash_read_id();
while ((id == 0) || (id == 0XFFFF)) {   /* 检测不到 FLASH 就一直提示 */
    lcd_show_string(30,130,...,"FLASH Check Failed!", RED);
    LED0_TOGGLE();
}
...
if (key == KEY1_PRES) {                  /* 写入：从倒数第100字节开始写 */
    sprintf((char*)datatemp, "%s%d", (char*)g_text_buf, i);
    norflash_write((uint8_t*)datatemp, flashsize - 100, TEXT_SIZE);
}
if (key == KEY0_PRES) {                  /* 读出并显示 */
    norflash_read(datatemp, flashsize - 100, TEXT_SIZE);
    lcd_show_string(30,170,...,(char*)datatemp, BLUE);
}
```

`g_text_buf = "STM32F103 SPI TEST"`，`flashsize = 8*1024*1024`（代码里按 8MB 计算写入末尾地址）。每次写的内容带序号 `i`，读出来若序号正确就说明读写一致。

## 六、编译、下载与运行

1. 用 Keil MDK 打开 [Projects/MDK-ARM/atk_f103.uvprojx](Projects/MDK-ARM/atk_f103.uvprojx)。
2. 确认器件为 STM32F103RC、启动文件为 `startup_stm32f103xe.s`，Include Path 含 `Drivers/BSP/SPI`、`NORFLASH`、`SYSTEM` 等。
3. Build 生成 `Output/atk_f103.axf/bin`，用 ST-Link/J-Link 下载（SWD 接线即可）。
4. 串口 USMART 用 XCOM/SSCOM，**波特率 115200**，字符串以回车换行结尾。可在串口输入 `norflash_read_id()`、`norflash_erase_chip()`、`norflash_erase_sector(addr)` 远程调用（见 [usmart_config.c](Middlewares/USMART/usmart_config.c)）。

## 七、实验现象

1. 上电后 LCD 显示 "STM32 / SPI TEST / ATOM@ALIENTEK / KEY1:Write KEY0:Read"，并显示 "SPI FLASH Ready!"（若显示 "FLASH Check Failed!" 则是没检测到芯片，见八）。
2. 按 **KEY1**：LCD 提示 "Start Write FLASH...." → "FLASH Write Finished!"，把带序号的字符串写入 FLASH 末尾 100 字节处。
3. 按 **KEY0**：LCD 提示 "The Data Readed Is:"，下方显示刚读出的字符串，序号应与上次写入对应。
4. LED0 周期性闪烁，表示程序在跑。
5. USMART 输入 `norflash_read_id` 回车，可看到返回的芯片 ID 数值。

## 八、注意事项

- **FLASH 没检测到（ID=0/0xFFFF）**：检查供电、SPI 接线（PA5/PA6/PA7）、片选 PA2 是否被占用、芯片型号是否被本驱动支持。
- **CPOL 注释与实际不符**：`spi.c` 把 bit1 置 1（CPOL=1）却注释成"CPOL=0"，以代码为准；换 FLASH 型号若读乱，先核对 SPI 模式是否匹配数据手册（25QXX 支持模式 0 和模式 3，本工程用模式 3）。
- **读写前先擦除**：NOR FLASH 只能 1→0，写非 `0xFF` 区域前必须擦扇区，`norflash_write()` 已自动处理；若直接用 `norflash_write_nocheck()` 写非空白区会失败。
- **擦写不要掉电**：擦除/编程过程中断电可能损坏数据甚至该扇区，务必保持供电稳定。
- **整片擦除很慢**：`norflash_erase_chip()` 可能要几十秒，期间 `wait_busy` 会阻塞，别误判为死机。
- **KEY1 = PA15 与 JTAG 冲突**：PA15 默认是 JTDI，用 JTAG 调试时会被占用。本工程用 SWD（PA13/PA14）调试一般不影响 PA15，但若 KEY1 失灵，可在 `key_init()` 中把调试端口切成 SW 仅用 SWD（本工程未做此 remap）。
- **`norflash.h` 里 `extern uint16_t norflash_TYPE;` 是残留声明**：实际定义/使用的是 `norflash.c` 里的 `g_norflash_type`，全工程没有任何地方引用 `norflash_TYPE`，属于历史遗留，不影响功能。
- **官方 readme.txt 与代码的小差异**：readme 标题写"驱动 NM25Q64"，"实验现象"里又混用 W25Q128/NM25Q64；代码默认变量 `g_norflash_type = NM25Q64` 只是个兜底值，运行时会用 `norflash_read_id()` 读到的真实 ID 覆盖，25QXX 全系列（除 W25Q256 需 4 字节地址外）读写流程一致，不影响正确性。
- 若只插 RGB 屏则本实验不适用——代码仅支持 MCU 8080 并口屏。

## 九、总结与延伸

**本实验掌握的要点：**
1. SPI 主机的工作方式：SCK 节拍 + MOSI/MISO 全双工 + 片选，CPOL/CPHA 决定模式。
2. `SPI1->CR1` 各控制位（MSTR/SPE/BR/CPOL/CPHA/SSM/SSI/DFF）怎么填、填了什么意思。
3. SCK 频率计算 `PCLK2 / 2^(BR+1)`，本工程 FLASH 实际跑 36MHz。
4. 用 TXE/RXNE 两个标志轮询收发一个字节。
5. NOR FLASH "写使能→扇区擦除→页写(自动换页)"流程，以及"擦除是因为只能 1→0"的本质。

**延伸小练习：**
- 把 `spi1_set_speed(SPI_SPEED_2)` 改成 `SPI_SPEED_8`，观察读写是否仍正常，体会分频对速度的影响。
- 修改 `main.c`，连续写入多个扇区，验证 `norflash_write()` 的跨扇区自动处理。
- 用 USMART 输入 `norflash_erase_sector(0)` 擦第 0 扇区后，再读出来确认是否变成全 `0xFF`。

**衔接**：SPI 硬件外设掌握后，实验21 电阻触摸屏会用 GPIO **软件模拟**同样的 SPI 时序驱动 XPT2046，对比硬件 SPI 与软件 SPI 的异同会很有收获。
