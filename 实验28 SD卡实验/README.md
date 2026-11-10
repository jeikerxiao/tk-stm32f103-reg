# 实验28 SD卡实验

> 一句话概括：用 SPI1 以 SPI 模式驱动板载 TF(SD) 卡，完成上电握手、识别卡类型、读取 CSD 容量信息，并实现 512 字节扇区的读/写，为实验29 FATFS 文件系统铺路。

## 一、实验简介

- **实验目的**（官方 readme.txt）：学习 STM32 下使用 SD 卡的方法。通俗地说：SD 卡是一个"按扇区存取"的存储设备（1 扇区 = 512 字节），本实验教你不借助文件系统、直接用一串"命令"跟卡对话，实现最原始可靠的扇区读写。
- **在学习路线中的位置**：前置实验：实验20 SPI、实验27 内存管理（扇区缓冲用 `mymalloc`）、实验11 TFTLCD（显示）；后续实验29 FATFS、实验30 汉字显示、实验31 图片显示都建立在本实验的 `sd_read_disk`/`sd_write_disk` 之上。
- **开发板与芯片**：正点原子 MiniSTM32 V4 开发板，STM32F103RC，72MHz。

## 二、实验原理

### 2.1 SD 卡的两种工作模式，本工程用哪一种

SD 卡规范定义了两类电气接口：

| 模式 | 总线 | 速度 | 需要的单片机外设 |
| --- | --- | --- | --- |
| SDMMC/SDIO 模式 | 命令线 + 4 条数据线，专有时钟 | 高（25/50MHz 起） | 专用 SDIO 控制器（STM32F2/F4/L4 等才有） |
| SPI 模式 | MOSI/MISO/SCK/CS 四线 | 受 SPI 外设限制 | 任意普通 SPI 外设即可 |

STM32F103 **片内没有 SDIO 外设**，所以正点原子 F103 系列例程都用 SPI 模式驱动 SD 卡。本实验代码放在 `Drivers/BSP/SDMMC/` 目录里，但目录内只有 `spi_sdcard.c/.h`（没有 sdmmc.c 文件）——文件名已经说明走的是 SPI 模式，官方 readme.txt 也写明"SD 卡，通过 SPI1 驱动"。

SPI 模式约定：初始化阶段（握手识别）必须用低速时钟（400kHz 以下），识别完成后才能提速；读写数据以 512 字节块为单位。

### 2.2 一次"命令握手"长什么样

SPI 模式下，主机发给卡的每个命令都是一个 **6 字节帧**：

```text
字节0: 0x40 | 命令号      (最高两位01是起始位)
字节1~4: 32位参数(大端)
字节5: CRC + 停止位       (CMD0固定0x95, CMD8固定0x87, 其余0x01——SPI模式可忽略CRC校验)
```

发完后主机继续空转字节（写 0xFF）读取卡的 **R1 响应**（1 字节），`bit7=1` 表示还没准备好要重试，本驱动以 `res & 0x80` 判断、最多重试 0x1F 次。R1 里 `0x00`=正常，`0x01`=卡在 IDLE 状态，`0x05`=数据已被卡接收。

本实验用到的核心命令（定义在 [spi_sdcard.h](Drivers/BSP/SDMMC/spi_sdcard.h)）：

| 命令 | 作用 | 本工程用处 |
| --- | --- | --- |
| CMD0 | 复位进 IDLE 状态 | 握手第一步，期待响应 0x01 |
| CMD8 | 查接口条件（V2 卡） | 参数 0x1AA，判断是否 SD V2.0 |
| ACMD41 | 发送操作条件、轮询卡就绪 | 先自动带发 CMD55；V2 卡参数 bit30=1 |
| CMD1 | MMC 卡就绪轮询 | MMC 卡替代 ACMD41 |
| CMD16 | 设置块长度 | V1.x/MMC 路径设为 512 |
| CMD58 | 读 OCR | 检查 CCS 位区分 SDHC 标准/高容量 |
| CMD9 | 读 CSD（容量信息） | `sd_get_csd`/`sd_get_sector_count` |
| CMD10 | 读 CID（厂商信息） | `sd_get_cid` |
| CMD17 | 读单个块 | `sd_read_disk` 单扇区路径 |
| CMD18 | 连续读多块 | 多扇区路径 |
| CMD12 | 停止多块传输 | 多块读结束后发送 |
| CMD24 | 写单个块 | `sd_write_disk` 单扇区路径 |
| CMD25 | 连续写多块 | 多扇区路径，结束发 0xFD 尾块 |
| ACMD23 | 预置擦除块数 | 多块写前发送，提高写入效率 |

### 2.3 初始化握手流程（sd_init 的编号剧本）

1. `sd_spi_init()` 配置引脚与 SPI1，切**低速** `sd_spi_speed_low()`（SPI_SPEED_256 → 36MHz/256 ≈ **140.6kHz**）；
2. 连发 10 个 0xFF（80 个时钟），满足卡上电要求的最少 74 个时钟；
3. 发 **CMD0**，循环直到响应 `0x01`（进入 IDLE）；
4. 发 **CMD8(0x1AA)**：响应正常 → 读 R7 的 4 字节，若 `ocr[2]==0x01 && ocr[3]==0xAA` 说明是支持 2.7~3.6V 的 **SD V2.0**；
5. V2 路径：循环发 **ACMD41(1<<30)** 直到响应变 0（卡就绪），再发 **CMD58** 读 OCR，`ocr[0] & 0x40`（CCS 位）为 1 → `SD_TYPE_V2HC`（高容量，按扇区号寻址），否则 `SD_TYPE_V2`；
6. V2 识别失败 → V1.x/MMC 路径：发 **ACMD41**（响应≤1 认定 SD V1）或 **CMD1**（认定 MMC），轮询就绪后用 **CMD16** 把块长设为 512；
7. 收尾：`sd_deselect()` 释放总线，`sd_spi_speed_high()`（SPI_SPEED_2 → 36MHz/2 = **18MHz**）。

### 2.4 块读写时序

- 读（`sd_read_disk`）：非 SDHC 卡先把扇区号 `<<9` 换算成字节地址；单块发 **CMD17**+地址，然后 `sd_receive_data`：等数据起始令牌 **0xFE** → 收 512 字节 → 再读 2 个伪 CRC 字节；多块用 **CMD18** 循环收，收完发 **CMD12** 停止。
- 写（`sd_write_disk`）：单块发 **CMD24**+地址，`sd_send_block`：等卡 ready（读回 0xFF）→ 发数据令牌 **0xFE** → 512 字节 → 2 字节伪 CRC → 读回响应，`(t & 0x1F) == 0x05` 才算写入被接受；多块先 **ACMD23** 声明块数，再 **CMD25**，每块前发令牌 **0xFC**，最后补一个 **0xFD** 结束令牌。

### 2.5 卡容量怎么算出来的

`sd_get_sector_count()`（[spi_sdcard.c](Drivers/BSP/SDMMC/spi_sdcard.c)）发 **CMD9** 读回 16 字节 CSD 寄存器，按卡型分支：

```c
if ((csd[0] & 0xC0) == 0x40)    /* V2.00的卡(SDHC) */
{
    csize = csd[9] + ((uint16_t)csd[8] << 8) + ((uint32_t)(csd[7] & 63) << 16) + 1;
    capacity = (uint32_t)csize << 10;       /* 得到扇区数 */
}
else    /* V1.XX的卡 / MMC V3卡 */
{
    n = (csd[5] & 15) + ((csd[10] & 128) >> 7) + ((csd[9] & 3) << 1) + 2;
    csize = (csd[8] >> 6) + ((uint16_t)csd[7] << 2) + ((uint16_t)(csd[6] & 3) << 10) + 1;
    capacity = (uint32_t)csize << (n - 9);  /* 得到扇区数 */
}
```

返回的是**扇区数**；main.c 里 `sd_size >> 11` 换算成 MB（扇区数 × 512B ÷ 1MB = ÷2048，即右移 11 位）。例如 8GB 卡 ≈ 16777216 扇区，右移 11 位显示 8192MB。

## 三、硬件资源与接线

| 资源 | 引脚 | 代码中的宏/位置 | 说明 |
| --- | --- | --- | --- |
| SD 卡片选 SD_CS | **PA3** | `SD_CS_GPIO_PORT/PIN`，[spi_sdcard.h](Drivers/BSP/SDMMC/spi_sdcard.h) | 软件片选，低电平选中 |
| SPI1_SCK | PA5 | `SPI1_SCK_GPIO_PIN`，[spi.h](Drivers/BSP/SPI/spi.h) | 时钟线 |
| SPI1_MISO | PA6 | `SPI1_MISO_GPIO_PIN` | 卡→主机数据 |
| SPI1_MOSI | PA7 | `SPI1_MOSI_GPIO_PIN` | 主机→卡数据 |
| SPI1 外设时钟 | `RCC->APB2ENR |= 1<<12` | spi.h | APB2 上的 SPI1 |
| LED0 | PA8 | [led.h](Drivers/BSP/LED/led.h) | 运行指示 |
| KEY0 | PC5 | [key.h](Drivers/BSP/KEY/key.h) | 读 0 扇区并经串口打印 |
| 串口1 | PA9/PA10，CH340 | [usart.h](Drivers/SYSTEM/usart/usart.h) | 115200bps 打印扇区数据 |
| TFTLCD | MCU 并口 | [lcd.h](Drivers/BSP/LCD/lcd.h) | 显示初始化结果与容量 |

- 接线：开发板有板载 TF(Micro SD) 卡座，接在 SPI1 上，需要自备一张 TF 卡插入（卡座在板子边缘，方向按缺口对准即可，支持热插拔但不保证）。
- `sd_spi_init()` 里还会把 **PA2、PA4 拉高**（配成推挽输出、输出 1）：PA2 是板载 SPI FLASH(W25Qxx) 的片选，PA4 是无线模块相关片选——SPI1 总线上多个设备共用，初始化 SD 卡时必须把其它设备的 CS 拉高，否则会同时响应、总线冲突。
- 时钟：`sys_stm32_clock_init(9)` → 72MHz；SPI1 挂在 APB1 上，PCLK1 = 72/2 = 36MHz，spi.h 注释给出速率公式 `SPI速度 = PCLK1 / 2^(speed+1)`，因此初始化 36/256 ≈ 140.6kHz，运行 36/2 = 18MHz。

## 四、代码结构与调用流程

```text
实验28 SD卡实验/
├── User/main.c                      测试主程序
├── Drivers/BSP/SDMMC/spi_sdcard.c/.h  SD卡SPI模式驱动（本实验核心）
├── Drivers/BSP/SPI/spi.c/.h         SPI1 底层：初始化/变速/收发一字节
├── Middlewares/MALLOC/              扇区缓冲区动态分配（实验27）
└── Drivers/BSP/{LED,KEY,LCD}/...    基础外设
```

`main()` 流程：

1. 基础初始化：时钟 72MHz → delay → 串口 115200 → USMART → LED → LCD → 按键 → `my_mem_init(SRAMIN)`；
2. LCD 打印 "STM32 / SD TEST / ATOM@ALIENTEK / KEY0:Read Sector 0"；
3. `while (sd_init())`：初始化失败就交替显示 `SD Card Error! / Please Check!` 并闪 LED0，直到成功；
4. 成功：显示 `SD Card OK`，`sd_get_sector_count()` 取扇区数，`>>11` 后以 MB 显示容量；
5. 主循环：KEY0 按下 → `buf = mymalloc(0, 512)` → `sd_read_disk(buf, 0, 1)` 读第 0 扇区 → `printf` 以十六进制逐字节发到串口 → `myfree`；
6. 每 20 次 10ms 循环翻转 LED0。

## 五、核心代码解析

### 5.1 SPI1 底层初始化（[spi.c](Drivers/BSP/SPI/spi.c)，节选）

```c
tempreg |= 0 << 11;             /* DFF = 0      , 使用8位数据格式 */
tempreg |= 1 << 9;              /* SSM = 1      , 软件片选(NSS)控制 */
tempreg |= 1 << 8;              /* SSI = 1      , 禁止软件从设备, 即做主机 */
tempreg |= 7 << 3;              /* BR[2:0] = 7  , 默认使用256分频, 速度最低 */
tempreg |= 1 << 2;              /* MSTR = 1     , 主机模式 */
tempreg |= 1 << 1;              /* CPOL = 0     , 空闲状态下, SCK保持高电平 */
tempreg |= 1 << 0;              /* CPHA = 1     , 数据采集从第二个时钟边沿开始 */
SPI1_SPI->CR1 = tempreg;
SPI1_SPI->CR1 |= 1 << 6;        /* SPE = 1      , 使能SPI */
```

- `SSM=1, SSI=1`：STM32 硬件 NSS 全部禁用，片选交给我们软件操作的 PA3——SD 卡驱动全靠 `sd_select()/sd_deselect()` 控制。
- `BR[2:0]=7`：复位后先以 256 分频（≈140kHz）低速跑，符合 SD 卡"识别阶段 ≤400kHz"的要求，识别完成后 `spi1_set_speed()` 只改 CR1 的 BR 位。
- MSTR=1 主机模式，8 位帧格式，MSB 先行。
- 注意（小坑）：注释与位值有出入——代码实际写入的是 `CPOL=1、CPHA=1`（SPI 模式 3），而注释写"CPOL = 0"。SD 卡 SPI 接口允许模式 0 和模式 3，本驱动以模式 3 工作，照抄注释理解会错，**以位值为准**。

### 5.2 收发一字节与命令帧（spi.c / spi_sdcard.c，节选）

```c
uint8_t spi1_read_write_byte(uint8_t txdata)
{
    while ((SPI1_SPI->SR & 1 << 1) == 0);   /* 等待发送区空 */
    SPI1_SPI->DR = txdata;                  /* 发送一个byte */
    while ((SPI1_SPI->SR & 1 << 0) == 0);   /* 等待接收完一个byte */
    return SPI1_SPI->DR;                    /* 返回收到的数据 */
}
```

SPI 是全双工：发 0xFF 的同时就在收。所谓"读响应"其实是边发空字节边看收回来的值。

```c
/* 发送命令包 */
sd_spi_read_write_byte(cmd | 0x40); /* 起始 + 命令索引号 */
sd_spi_read_write_byte(arg >> 24);  /* 参数[31 : 24] */
...
if (cmd == CMD0) crc = 0X95;        /* CMD0 的CRC值固定为 0X95 */
if (cmd == CMD8) crc = 0X87;        /* CMD8 的CRC值固定为 0X87 */
sd_spi_read_write_byte(crc);
...
retry = 0X1F;
do { res = sd_spi_read_write_byte(0xFF); } while ((res & 0X80) && retry--);
```

ACMD 应用命令通过"命令号最高位"表达：`ACMD41 = 0x80+41`，`sd_send_cmd` 检测到 `cmd & 0x80` 会先替你再发 **CMD55**，然后再发第 41 号命令，这是 SPI 模式 ACMD 的标准套路。

### 5.3 读一个扇区（spi_sdcard.c，节选）

```c
uint8_t sd_read_disk(uint8_t *pbuf, uint32_t saddr, uint32_t cnt)
{
    long long lsaddr = saddr;
    if (sd_type != SD_TYPE_V2HC)
        lsaddr <<= 9;   /* 标准容量卡按字节寻址：扇区号×512 */
    if (cnt == 1)
    {
        res = sd_send_cmd(CMD17, lsaddr);       /* 读命令 */
        if (res == 0) res = sd_receive_data(pbuf, 512);
    }
    else
    {
        res = sd_send_cmd(CMD18, lsaddr);       /* 连续读命令 */
        do { res = sd_receive_data(pbuf, 512); pbuf += 512; } while (--cnt && res == 0);
        sd_send_cmd(CMD12, 0);  /* 发送停止命令 */
    }
    sd_deselect();
    return res;
}
```

只有 SDHC（`SD_TYPE_V2HC`）卡才直接按扇区号寻址；老的标准容量卡要传字节地址，这就是 `<<9`（×512）的由来。`sd_receive_data` 里"等 0xFE 令牌 → 循环收 512 字节 → 吃掉 2 字节伪 CRC"就是块数据响应的完整结构。

### 5.4 写与握手细节

`sd_send_block()` 的应答判断是写操作可靠性的关键：

```c
t = sd_spi_read_write_byte(0xFF);   /* 接收响应 */
if ((t & 0x1F) != 0x05)             /* 数据包没有被接收 */
    return SD_ERROR;
```

`0x05` 是数据响应 R1 的固定值，表示"块已成功写入卡"；不等于它就必须重试或报错，绝不能在没收到 0x05 时认为写完了。另外驱动版本注释（V1.1）提到 ACMD 类命令经常要发 6 次以上才能识别，所以 `sd_get_status()`、多块写的 ACMD23 都包在 `retry` 循环里。

## 六、编译、下载与运行

1. Keil MDK 打开 [Projects/MDK-ARM/atk_f103.uvprojx](Projects/MDK-ARM/atk_f103.uvprojx)，器件 STM32F103RC，F7 编译，输出在 `Output/`。
2. ST-Link/J-Link 下载；串口助手（XCOM/SSCOM）连板载 CH340，**115200bps**。
3. 插入 TF 卡后再复位/重新上电，观察 LCD。
4. USMART 已注册 `sd_test_read(扇区号,扇区数)` 与 `sd_test_write(扇区号,扇区数)`（main.c 中定义，[usmart_config.c](Middlewares/USMART/usmart_config.c) 注册），可在串口直接敲 `sd_test_read(0,1)` 验证。

## 七、实验现象

1. 没插卡/卡坏：LCD 交替显示 `SD Card Error!` 和 `Please Check!`，LED0 闪烁，此时串口无输出。
2. 插好正常 TF 卡后复位：LCD 显示 `SD Card OK` 和 `SD Card Size: xxxx MB`（容量与卡标称值接近即正确）。
3. 按 KEY0：LCD 依次出现 `USART1 Sending Data...` → `USART1 Send Data Over!`，串口助手收到 `SECTOR 0 DATA:` 开头、512 个字节的十六进制、`DATA ENDED` 结尾。
4. 第 0 扇区是磁盘分区表/引导扇区，未格式化的新卡读出全 0xFF 属正常。
5. 用 USMART 发 `sd_test_write(扇区,1)` 会向指定扇区写入 `i*3` 图案再读回可对比；**警告**：写真实使用过的卡会破坏该区数据（main.c 注释原文："慎用!! 最好写全是0XFF的扇区,否则可能损坏SD卡"）。
6. LED0 约每 200ms 翻转一次。

## 八、注意事项

- **总线共享**：SD 卡、W25Qxx SPI FLASH（CS=PA2）、无线模块（CS=PA4）共用 SPI1，谁被选中由 CS 决定；移植或同时驱动多设备时，未用的设备 CS 必须保持高电平（`sd_spi_init()` 已示范）。
- **先低速后高速**：跳过 140kHz 识别阶段直接 18MHz 握手，多数卡会初始化失败。
- 官方 readme.txt 与代码一致（"SD 卡，通过 SPI1 驱动"）；唯一容易误解的是 `Drivers/BSP/SDMMC/` 目录名——里面没有 SDIO 代码，只有 SPI 驱动，SDMMC 只是历史命名。
- spi.c 中 CPOL 注释与位值不符（见 5.1），阅读代码时以位值为准。
- 读写期间**不要拔卡、不要掉电**，写操作尤其危险；实验29 之后卡上还有文件系统，乱扇区等于删文件。
- SDHC/SDXC 大卡支持：驱动识别 V2HC；exFAT 格式的大卡在 FATFS 实验里由 `FF_FS_EXFAT=1` 支持（实验29）。
- 部分老卡/MMC 卡在 V1 路径需要 CMD16 设 512 块长，若个别卡失败，检查 `sd_init` 返回值对应的 `sd_type`。

## 九、总结与延伸

- 掌握要点：
  1. SD 卡 = 512 字节扇区的块设备；SPI 模式用 6 字节命令帧 + R1 响应对话；
  2. `sd_init` 的握手剧本：74+ 时钟 → CMD0 → CMD8 → ACMD41/CMD1 → CMD58/CMD16，低速起步、识别后提速；
  3. 标准容量卡按字节寻址（`<<9`），SDHC 按扇区寻址；
  4. 读块 = CMD17/18 + 0xFE 令牌 + 512B + 伪CRC，写块 = CMD24/25 + 0xFC/0xFD + 0x05 应答；
  5. CSD → 扇区数 → 容量的换算链条。
- 延伸小练习：
  1. 按 KEY0 改为循环读出前 8 个扇区并观察哪个扇区出现分区表魔数 `0x55 0xAA`（十六进制 `55 aa`）；
  2. 用 USMART `sd_test_write` 写一个空闲扇区再 `sd_test_read` 读回，验证数据图案 `i*3`；
  3. 把 `sd_get_cid()` 读回的 16 字节经串口打印，尝试找出厂商ID和容量信息所在字节。
- 衔接：实验29 用本实验的 `sd_read_disk/sd_write_disk` 作为 FATFS 的底层"0 号磁盘"，在文件系统之上才谈得上"文件"；实验30 汉字字库则把字库存进同一 SPI1 总线上的 W25Qxx。
