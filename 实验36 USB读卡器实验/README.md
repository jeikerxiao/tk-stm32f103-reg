# 实验36 USB读卡器实验

> 一句话概括：让 STM32F103 用自带的 USB 外设当"USB 从设备"，把自己伪装成 U 盘（两个可移动磁盘：SD 卡 + 板载 SPI FLASH），电脑直接像读写 U 盘一样读写它们——顺便搞懂 USB 枚举和 Bulk-Only 传输。

## 一、实验简介

- 实验目的（官方 `readme.txt`）：学习 STM32 的 USB SLAVE 功能使用，实现一个简单的 USB 读卡器。
- 通俗展开：USB 世界里分"主机"（Host，电脑）和"设备"（Slave/Device，U 盘、鼠标）。STM32F103 内置一个 USB 全速设备控制器，只要固件按 USB 协议"自我介绍"（枚举）成功，电脑就会把它识别成某种设备。本实验识别类型是"大容量存储设备"（Mass Storage Class，MSC），所以电脑里出现可移动磁盘。
- 学习路线位置：前置知识为实验4（串口，printf 调试）、实验20（SPI）、实验28（SD 卡）、实验27（内存管理，本例初始化了 SRAM 内存池）；后续实验37（USB 虚拟串口）复用同一套 USB 库，只换设备类。
- 开发板与芯片：正点原子 MiniSTM32 V4，STM32F103RC，主频 72MHz。

## 二、实验原理

### 2.1 STM32F103 的 USB 设备外设

先解释两个词：

- **USB SLAVE（从设备）**：不主动发起通信、听候主机调度的那一方。本实验 STM32 就是从设备。
- **OTG_FS**：ST 在芯片手册里给这套 USB 全速（Full Speed，12Mbps）控制器的另一称呼；在 F103 的寄存器头文件里它就叫 `USB`，基址 `USB_BASE = 0x40005C00`（APB1 上），中断号 `USB_LP_CAN1_RX0_IRQn = 20`。F103 的 USB 没有外部 PHY 引脚，D-/D+ 直接复用 **PA11/PA12** 的内置收发器。

代码中的关键寄存器配置（都在 [Middlewares/USB/USB_APP/usbd_conf.c](Middlewares/USB/USB_APP/usbd_conf.c)）：

| 操作 | 寄存器写法 | 含义 |
| --- | --- | --- |
| 使能 USB 时钟 | `RCC->APB1ENR \|= 1 << 23;` | USB 外设时钟（`HAL_PCD_MspInit` 中） |
| USB 时钟源 | PLL 输出 72MHz ÷1.5 = 48MHz | `sys_stm32_clock_init` 里 `RCC->CFGR &= 0xFF80FFFF` 已把 USBPRE 位（bit22）清 0，即选 ÷1.5，正好满足 USB 全速必需的 48MHz |
| 端点 0~7 | `hpcd.Init.dev_endpoints = 8;` | F103 USB 共 8 个端点 |
| 收发票据窗口 | `USB->CNTR` 各中断使能位（库内配置 CTRM/WKUPM/SUSPM/ERRM/SOFM/ESOFM/RESETM） | USB 事件中断来源 |
| 断开/重连 | `USB->CNTR |= 1 << 1;`（PWDN=1）/ `&= ~(1 << 1)` | 见 2.2 |

**端点（Endpoint）**可以理解为设备上的"专用收发窗口"：控制端点 0 用于枚举时一问一答，批量端点用于搬数据。USB 内部还有一块 2KB 的 **PMA（Packet Memory Area）**缓冲，代码里用 `HAL_PCDEx_PMAConfig` 给各端点划了窗口地址：EP0 OUT/IN 在 0x18/0x58，MSC 批量 IN/OUT 在 0x98/0xD8（`USBD_LL_Init` 中）。

### 2.2 连接检测与"先断再接"：D+ 上拉的学问

USB 规定：全速设备把 **D+ 经 1.5kΩ 电阻上拉到 3.3V**，主机看到 D+ 变高就知道"有设备插上来了"，随即发起复位和枚举。F103 的 D+ 上拉由内置收发器完成，固件上电后必须让收发器退出掉电模式（`usbd_port_config(1)`：`USB->CNTR &= ~(1 << 1)` 且 PA11/PA12 切回复用功能），否则主机看不见你；反之 `usbd_port_config(0)` 把 PWDN 置 1、PA11/PA12 拉成推挽输出低电平，制造一次"拔线"效果。main.c 开头：

```c
usbd_port_config(0);    /* USB先断开 */
delay_ms(500);
usbd_port_config(1);    /* USB再次连接 */
delay_ms(500);
```

目的：保证每次重启板子，电脑都重新走一遍枚举，避免上次会话残留导致不识别。硬件上 MiniSTM32 V4 的 **USB_SLAVE 座**已把 PA11/PA12 接到 USB 插头的 D-/D+，用 USB 数据线（不是纯充电线）插这个口即可，无需外接电阻。

### 2.3 枚举过程（设备如何"自报家门"）

插入后电脑与 STM32 的对话由库自动完成（`Middlewares/USB/STM32_USB_Device_Library/Core/Src/usbd_core.c`、`usbd_ctlreq.c`），顺序如下：

1. 主机检测 D+ 电平变化 → 发 **RESET**；设备回调 `HAL_PCD_ResetCallback` 里串口打印 `USB Device Library  [FS]` 并复位协议栈；
2. 主机发 GET_DESCRIPTOR 读**设备描述符**（在 `usbd_desc.c` 里写死：`USBD_VID = 0x0483`，`USBD_PID = 0x5720`，产品字符串 `"Mass Storage in FS Mode"`）；
3. 主机分配地址（SET_ADDRESS）、读**配置描述符**（含 MSC 类需要的接口和批量端点：`MSC_EPIN_ADDR = 0x81`、`MSC_EPOUT_ADDR = 0x01`）；
4. 主机 SET_CONFIGURATION → `USBD_MSC_CLASS` 激活，盘符就绪。

这套 ST USB Device Library（Core + Class/MSC + HAL 的 `stm32f1xx_hal_pcd.c`/`stm32f1xx_ll_usb.c`）是原样复用的第三方库；工程其余部分（时钟/GPIO/串口/SPI/SD/LCD）仍是寄存器风格。

### 2.4 Bulk-Only Transport：U 盘对话的"公文包协议"

MSC 类几乎只用批量端点，规范叫 **BOT（Bulk-Only Transport）**（库文件 `usbd_msc_bot.c`/`usbd_msc.h`/`usbd_msc_bot.h`）。一次完整"命令"= 三段式：

```text
电脑→设备: CBW(命令块, 31字节, 签名0x43425355='USBC', 内含LUN号/数据长度/SCSI命令)
[双向]: 数据阶段(读或写, 批量端点搬运, 单次缓冲 USBD_BOT_MAX_DATA = 256)
设备→电脑: CSW(状态块, 13字节, 签名0x53425355='USBS', 内含命令是否成功)
```

CBW 里的 SCSI 命令决定了具体动作，本工程 `usbd_msc_scsi.c` 支持的子集（宏值摘自 `usbd_msc_scsi.h`）：

| SCSI 命令 | 操作码 | 用途 |
| --- | --- | --- |
| TEST_UNIT_READY | 0x00 | 探活（main 主循环靠它刷新连接状态，见 2.5） |
| REQUEST_SENSE | 0x03 | 取错误详情 |
| INQUIRY | 0x12 | 读设备识别串（`usbd_storage.c` 的 `STORAGE_Inquirydata`，厂商串 'ALIENTEK'） |
| READ10 | 0x28 | 读 512 字节扇区 |
| WRITE10 | 0x2A | 写扇区 |

BOT 状态机（`usbd_msc_bot.h`）：`USBD_BOT_IDLE → DATA_OUT（等电脑发数据）/ DATA_IN（给电脑送数据）→ LAST_DATA_IN → 发 CSW`。CBW 校验不过（长度≠31、签名错、LUN>1 等）直接回错误并复位状态。

### 2.5 两个磁盘：LUN 与存储回调对接

一个 USB 设备可以挂多个"逻辑单元"（LUN，像一盘多分区）。本工程 `STORAGE_LUN_NBR = 2`（`usbd_conf.h`），实际实现见 [Middlewares/USB/USB_APP/usbd_storage.c](Middlewares/USB/USB_APP/usbd_storage.c)：

- **LUN 0 = 板载 SPI FLASH（W25QXX）**：容量取 12MB 给电脑用（`*block_num = (1024 * 1024 * 12) / 512`，`USB_STORAGE_FLASH_BASE = 0`），读写走 `norflash_read/norflash_write`；INQUIRY 产品串是 `'SPI Flash Disk'`。
- **LUN 1 = SD 卡**：容量 `sd_get_sector_count()`（SD 卡按 512 字节扇区寻址），读写走 `sd_read_disk/sd_write_disk`；INQUIRY 产品串 `'SD Flash Disk'`。
- `STORAGE_GetMaxLun()` 很关键：`sd_get_sector_count()` 非 0（SD 卡正常）返回 1 → 电脑见 2 个盘；否则返回 0 → 只见 SPI FLASH 一个盘。**没插 SD 卡也能玩**，只是少一个盘。
- 读写回调顺带维护一个自定义状态字 `g_usb_state_reg`：bit0 正在写、bit1 正在读、bit2 写错误、bit3 读错误、bit4 "电脑仍在轮询"（`STORAGE_IsReady` 里 `|= 0X10`）。

主循环据此显示与判连接：每 200ms（`tct == 200`）清一次状态字；若本轮 bit4 置位说明主机还在轮询 → `g_device_state = 1`，连 LED1 点亮/"USB Connected"；若连续 `offline_cnt > 100`（100×200ms = 20 秒）没轮询 → 判定拔出，显示 "USB DisConnected"。

## 三、硬件资源与接线

| 资源 | 引脚 | 代码中的宏/位置 | 说明 |
| --- | --- | --- | --- |
| USB D-（DM） | PA11 | `usbd_port_config`/`HAL_PCD_MspInit` 中 GPIO 复用设置 | 走板载 USB_SLAVE 座 |
| USB D+（DP） | PA12 | 同上 | 全速上拉由内置收发器完成 |
| USB 中断 | `USB_LP_CAN1_RX0_IRQn`（20） | `sys_nvic_init(0, 3, USB_LP_CAN1_RX0_IRQn, 2)` | 抢占优先级 0，子优先级 3，分组 2 |
| SPI1（SD 卡 + W25QXX 共用） | SCK=PA5，MISO=PA6，MOSI=PA7 | `Drivers/BSP/SPI/spi.h`（`SPI1_SCK/MISO/MOSI_GPIO_PIN`） | 注意：官方 readme.txt 写 norflash 在 SPI2，**实际代码里两个设备都挂在 SPI1**，靠各自片选区分 |
| SD 卡片选 | PA3 | `SD_CS_GPIO_PORT/PIN`，`Drivers/BSP/SDMMC/spi_sdcard.h` | SD 卡插板载卡槽 |
| SPI FLASH 片选 | PA2 | `NORFLASH_CS_GPIO_PORT/PIN`，`Drivers/BSP/NORFLASH/norflash.h` | 板载 W25QXX（`norflash_read_id` 读 ID 判断，如 `W25Q64 = 0xEF16`） |
| LED0（红灯） | PA8 | `led.h` | 每 200ms 闪烁，指示程序在跑 |
| LED1（绿灯） | PD2 | `led.h` | USB 读写时点亮 |
| 串口1（调试 printf） | PA9/PA10 → CH340 | `usart.h`，115200 | 枚举成功、读写错误信息从这里打印 |
| LCD（MCU 屏） | 数据 PB0~PB15；RD=PC6，WR=PC7，RS=PC8，CS=PC9，BL=PC10 | `Drivers/BSP/LCD/lcd.h` | 插板载 LCD 座，显示容量与 USB 状态 |
| 按键 KEY0/KEY1/WK_UP | PC5/PA15/PA0 | `Drivers/BSP/KEY/key.h` | `main.c` 初始化了 `key_init()` 但业务流程未使用 |

接线操作：USB 数据线一端电脑、一端插板子 **USB_SLAVE** 口（不是 USB_UART 口；USB_UART 只负责串口调试，两者互相独立）。SPI FLASH（W25QXX）为板载，SD 卡插入卡槽即可。

## 四、代码结构与调用流程

```text
实验36 USB读卡器实验/
├── Drivers/
│   ├── BSP/SDMMC/spi_sdcard.c|h   # SPI 方式驱动 SD 卡(初始化/容量/读写扇区)
│   ├── BSP/NORFLASH/norflash.c|h  # W25QXX SPI FLASH 驱动
│   ├── BSP/SPI/spi.c|h            # SPI1 底层
│   ├── BSP/LCD|LED|KEY/           # 显示与指示
│   └── SYSTEM/delay|sys|usart/    # 系统层(72MHz 时钟、NVIC、GPIO、串口)
├── Middlewares/
│   ├── MALLOC/                    # 内存池(本例 my_mem_init(SRAMIN))
│   ├── USMART/                    # 本工程未启用调试指令
│   └── USB/
│       ├── USB_APP/usbd_conf.c|h      # 移植层: PA11/12、中断、PMA、usbd_port_config
│       ├── USB_APP/usbd_desc.c|h      # 设备/配置/字符串描述符(MSC)
│       ├── USB_APP/usbd_storage.c|h   # 本工程核心: 两个 LUN 的存储回调
│       ├── USB_HAL/                   # ST 的 PCD/LL USB 驱动(寄存器封装)
│       └── STM32_USB_Device_Library/  # ST 设备库: Core + Class/MSC(usbd_msc*.c/h: BOT/SCSI)
├── User/main.c
└── Projects/MDK-ARM/atk_f103.uvprojx
```

`main()` 流程（[User/main.c](User/main.c)）：

1. `sys_stm32_clock_init(9)` → `delay_init(72)` → `usart_init(72, 115200)` → `led_init` → `lcd_init` → `key_init` → `norflash_init`；`my_mem_init(SRAMIN)`。
2. 检测 SD 卡：`sd_init()` 失败显示 `SD Card Error!`；成功则 `sd_get_sector_count()` 得扇区数，LCD 显示容量（`sd_size >> 11` 即除以 2048 得 MB）。
3. 检测 SPI FLASH：`norflash_read_id()` 为 0/0xFFFF 显示 `25Q64 Error!`，否则显示 `SPI FLASH Size:7.25MB`。
4. USB 先断后接（`usbd_port_config(0/1)` 各隔 500ms）。
5. 初始化 USB 设备栈四连：

```c
USBD_Init(&USBD_Device, &MSC_Desc, 0);                      /* 初始化USB */
USBD_RegisterClass(&USBD_Device, USBD_MSC_CLASS);           /* 添加类 */
USBD_MSC_RegisterStorage(&USBD_Device, &USBD_DISK_fops);    /* 为MSC类添加回调函数 */
USBD_Start(&USBD_Device);                                   /* 开启USB */
```

6. 主循环（每 1ms）：对比 `g_usb_state_reg`/`g_device_state` 刷新 LCD 提示（`USB Writing... / USB Reading... / USB Write Err / USB Read Err / USB Connected / USB DisConnected`）；每 200ms LED1 灭、LED0 翻转，并按 bit4 轮询位做 20 秒拔出判定。

## 五、核心代码解析

### 5.1 USB 口的断开/重连 `usbd_port_config`（usbd_conf.c）

```c
if (state)
{
    USB->CNTR &= ~(1 << 1); /* PWDN = 0, 退出断电模式 */
    sys_gpio_set(GPIOA, SYS_GPIO_PIN11, SYS_GPIO_MODE_AF, ...);  /* D- 复用 */
    sys_gpio_set(GPIOA, SYS_GPIO_PIN12, SYS_GPIO_MODE_AF, ...);  /* D+ 复用 */
}
else
{
    USB->CNTR |= 1 << 1;    /* PWDN = 1, 进入断电模式 */
    sys_gpio_set(GPIOA, SYS_GPIO_PIN11, SYS_GPIO_MODE_OUT, ...);
    sys_gpio_set(GPIOA, SYS_GPIO_PIN12, SYS_GPIO_MODE_OUT, ...);
    sys_gpio_pin_set(GPIOA, SYS_GPIO_PIN11, 0); /* PA11 = 0 */
    sys_gpio_pin_set(GPIOA, SYS_GPIO_PIN12, 0); /* PA12 = 0 */
}
```

`USB->CNTR` 的 bit1 是 PWDN（掉电位）：置 1 时收发器断电，D+ 上拉消失，电脑视为拔出；置 0 并让 PA11/PA12 工作在外设/复用模式时，信号线交给 USB 控制器。这就是"软件模拟插拔"。

### 5.2 存储回调与 SD/FLASH 对接（usbd_storage.c）

```c
int8_t STORAGE_Write (uint8_t lun, uint8_t *buf, uint32_t blk_addr, uint16_t blk_len)
{
    int8_t res = 0;
    g_usb_state_reg |= 0X01;    /* 标记正在写数据 */
    switch (lun)
    {
        case 0: /* SPI FLASH */
            norflash_write(buf, USB_STORAGE_FLASH_BASE + blk_addr * 512, blk_len * 512);
            break;
        case 1: /* SD卡 */
            res = sd_write_disk(buf, blk_addr, blk_len);
            break;
    }
    ...
}
```

USB 库收到 WRITE10 后把 SCSI 参数翻译成 `STORAGE_Write(lun, buf, 起始扇区, 扇区数)`，本工程按 LUN 分流：FLASH 盘把 512 字节×N 换算成字节地址交给 `norflash_write`（W25QXX 内部自动处理擦除）；SD 盘交给 `sd_write_disk`。`STORAGE_Read` 同理（bit1/bit3 标记读与读错误）。错误还会通过 `printf("werr:%d,%d\r\n", ...)` 打到串口 1，便于排查坏卡。

### 5.3 BOT 命令合法性检查（usbd_msc_bot.c 节选）

```c
if ((USBD_LL_GetRxDataSize(pdev, MSC_EPOUT_ADDR) != USBD_BOT_CBW_LENGTH) ||
    (hmsc->cbw.dSignature != USBD_BOT_CBW_SIGNATURE) ||
    (hmsc->cbw.bLUN > 1U) ||
    (hmsc->cbw.bCBLength < 1U) || (hmsc->cbw.bCBLength > 16U))
{
  SCSI_SenseCode(pdev, hmsc->cbw.bLUN, ILLEGAL_REQUEST, INVALID_CDB);
  hmsc->bot_status = USBD_BOT_STATUS_ERROR;
  MSC_BOT_Abort(pdev);
}
```

EP1 OUT 收到的必须是恰好 31 字节、签名 `0x43425355`、LUN≤1、命令长度 1~16 的 CBW，否则回Sense 数据并中止——这就是"读卡器怎么防呆"的实例。

### 5.4 SD 卡容量显示与初始化

`sd_init()` 返回 0 才算正常；容量显示用了移位技巧 `sd_size >> 11`（扇区数×512B÷1MB）。`spi_sdcard.c` 的 `sd_spi_init` 注释写明共享 SPI1 的防冲突手法：把 25Q64 的片选 PA2 拉高（`GPIOA->ODR |= 0X5 << 2`），确保 SD 通信时 FLASH 不抢总线；速度上先 `SPI_SPEED_256` 低速识别、再 `SPI_SPEED_2` 高速传输（`sd_spi_speed_low/high` 宏）。

## 六、编译、下载与运行

1. Keil 打开 [Projects/MDK-ARM/atk_f103.uvprojx](Projects/MDK-ARM/atk_f103.uvprojx)（器件 STM32F103RC，启动文件 `startup_stm32f103xe.s`）。
2. 编译产物在 `Output/atk_f103.hex`；用 ST-Link/J-Link 下载（Tools → Flash Download 勾选 Reset and Run）。
3. 上位机：USB 数据线插 USB_SLAVE；想同步看调试打印，另开串口助手 115200 连 CH340（USART1 排针 PA9/PA10 需跳线帽接 RXD/TXD）。
4. Windows 会自动加载 `usbstor.inf`（大容量存储类是系统自带驱动，免驱）。

## 七、实验现象

1. 开机 LCD 依次出现 `STM32 / USB Card Reader TEST / ATOM@ALIENTEK`，随后显示 SD 卡容量（MB）或 `SD Card Error!`，以及 SPI FLASH 信息或 `25Q64 Error!`。
2. 插 USB 线到 USB_SLAVE 后，串口打印 `USB Device Library  [FS]`；LCD 由 `USB Connecting...` 变 `USB Connected`，电脑"此电脑"出现 **两个可移动磁盘**（未插 SD 卡则只有一个 SPI FLASH 盘）。
3. 往磁盘拷贝/删除文件时，LCD 显示 `USB Writing...` 或 `USB Reading...`，LED1 点亮；每 200ms LED0 翻转表示程序活着。
4. 拔掉 USB 线：最多 20 秒后 LCD 变 `USB DisConnected`。
5. 读写 SD 卡出错时 LCD 出现 `USB Read Err`/`USB Write Err`，串口同步打印 `rerr:/werr:`。

## 八、注意事项

1. **必须用数据线**：很多 USB 线只有电源芯，插上毫无反应；换线是排查枚举失败的第一步。D+/D- 走板载 USB_SLAVE 座，不要插到 USB_UART。
2. **代码与官方 readme.txt 不一致之处**：
   - readme.txt 写 norflash "连接在 SPI2 上"；本工程代码实际用 **SPI1（PA5/PA6/PA7）+ 片选 PA2**，与 SD 卡（片选 PA3）共总线，见 `spi.h/norflash.h/spi_sdcard.h`。以代码为准。
   - readme.txt 称"本例为演示 FATFS 可以挂载多个磁盘……"；本工程并无 FATFS 代码（无文件系统层，电脑端格式化即可用），LCD 的 `SPI FLASH Size:7.25MB` 是写死的字符串，而 USB 盘实际暴露 12MB。两处数字都不等于 W25QXX 全片容量。
   - `usbd_storage.c` 各函数头注释写 "0, SD卡 / 1, SPI FLASH"，但实现 case 恰好相反：**LUN0 = SPI FLASH，LUN1 = SD 卡**（以 `STORAGE_Inquirydata` 产品串与 switch 分支为准），电脑"可移动磁盘 1/2"的对应关系要按代码理解。
   - readme.txt 注意事项第 4 条"请用USB线连接在USB_UART,找到USB转串口后测试本例程"是从串口实验复制的残留，本实验主通道是 USB_SLAVE。
   - readme.txt 称"USB 和电脑连接成功后，LED1 常亮"；代码里 LED1 只在发生读/写的状态变化瞬间被点亮（`LED1(0)`），每 200ms 定时段又会 `LED1(1)` 熄灭，空闲时并不常亮。以代码为准。
3. 官方 readme.txt 提醒：串口助手用 XCOM/SSCOM，波特率 115200，其他助手可能拉 DTR/RTS 导致复位；本例串口仅调试用，不影响 USB。
4. **SPI FLASH 盘读写即擦即写**：对 W25QXX 频繁写小文件会做整扇区读改写，速度慢；**SD 卡读写时切勿拔卡、掉电**，否则可能损坏文件系统。
5. 若电脑不识别：确认 48MHz（USBPRE 位）没被其他时钟改动；重新插拔或复位板子（代码里的先断再接就是为了降低该问题概率）。
6. 只支持 MCU 屏（8080 并口），不支持 RGB 屏（官方 readme.txt）。

## 九、总结与延伸

要点：

1. F103 内置 USB 全速设备控制器（寄存器块 `USB`，0x40005C00，PA11/PA12，48MHz 取自 PLL÷1.5），D+ 上拉=连接声明，`USB->CNTR` 的 PWDN 位可做软件插拔。
2. 枚举 = 主机按描述符逐层认识设备；VID 0x0483/PID 0x5720、类 MSC 决定了电脑加载磁盘驱动。
3. BOT 协议 = CBW(31B) + 数据 + CSW(13B)，SCSI 命令 0x00/0x03/0x12/0x28/0x2A 是最小可用集合。
4. USB 库分层：`USB_HAL`（寄存器）→ `STM32_USB_Device_Library`（核心+类）→ `USB_APP`（移植+业务回调），LUN 回调就是"USB 世界 ↔ 物理存储介质"的桥。
5. 状态位设计 `g_usb_state_reg`（读写标志 + 轮询心跳）是用软件观察 USB 活动的好范式。

延伸练习：

1. 在 `STORAGE_Inquirydata` 修改产品字符串，观察电脑设备管理器里盘符名称变化。
2. 把 LUN0 容量从 12MB 改成 4MB（改 `STORAGE_GetCapacity` 的 `block_num`），验证电脑仍能正常格式化使用。
3. 拔掉 SD 卡运行，对照 `STORAGE_GetMaxLun` 的返回值确认只出一个盘。

衔接：实验37 用同一套库换成 CDC 类（虚拟串口），理解了枚举/BOT 之后再看 CDC 会轻松很多。
