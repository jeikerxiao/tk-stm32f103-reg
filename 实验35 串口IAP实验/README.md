# 实验35 串口IAP实验

> 一句话概括：给 STM32 写一个"程序内的程序员"（Bootloader），让它通过串口接收一个 APP 二进制文件，就能把自己升级成新程序——不用 ST-Link，一根 USB 线 + 串口助手即可换固件。

本实验包含 3 个子工程，父目录下并列存放：

| 子工程目录 | 角色 | 说明 |
| --- | --- | --- |
| `IAP Bootloader V1.0/` | 引导程序（Bootloader） | 烧在 FLASH 开头，负责收 APP、存 APP、跳 APP |
| `实验13 RTC实验_FLASH_APP/` | FLASH APP | 常驻内部 FLASH（0x08010000），断电不丢 |
| `实验21 触摸屏实验_SRAM_APP/` | SRAM APP | 链接在 SRAM（0x20001000），由 Bootloader 接收后直接在 SRAM 里运行 |

## 一、实验简介

- 实验目的（官方 `readme.txt`）：学习 STM32 的 IAP 功能，掌握 IAP 的使用。
- 通俗展开：IAP（In-Application Programming，在应用编程）就是"芯片自己给自己刷程序"。以前换固件要用 ST-Link 接线下载；有了 Bootloader 后，设备只要连着串口，用串口助手把 `.bin` 文件发过去就能更新程序。这是产品量产后远程/现场升级的基础。
- 学习路线位置：前置实验为实验4（串口）、实验11/12（LCD 与 USMART）、实验13（RTC）、实验21（触摸屏）、实验26（FLASH 模拟 EEPROM，即内部 FLASH 读写）；本实验把这些串成一个综合应用。
- 开发板：正点原子 MiniSTM32 V4，主芯片 STM32F103RC（内部 FLASH 256KB：`STM32_FLASH_SIZE = 0x40000`，起始 `STM32_FLASH_BASE = 0x08000000`；内部 SRAM 48KB：0x20000000~0x2000BFFF）。

## 二、实验原理

### 2.1 什么是 IAP / Bootloader

把内部 FLASH 想象成一排带编号的储物格。开机（复位）时 CPU 永远从 `0x08000000` 开始取两个字：第 1 个字给主堆栈指针 MSP，第 2 个字（复位向量）决定程序入口。我们人为约定：

1. FLASH 前段放 **Bootloader**（本实验即 `IAP Bootloader V1.0`，官方即 `IAP Bootloader V1.0/`，bin 实际约 38.6KB）；
2. FLASH 后段（本实验从 `0x08010000` 起）放 **APP**（用户程序）；
3. Bootloader 先跑，收到新 APP 后把它写进 FLASH 后段，或者直接放进 SRAM，然后"跳"过去执行。

跳转的本质（寄存器版就 3 步，见 `iap.c` 的 `iap_load_app`）：

```text
读 APP 向量表第 1 个字 → 作为栈顶地址, 执行 MSR MSP 设置主堆栈指针
读 APP 向量表第 2 个字 → 作为复位入口地址(函数指针)
调用该函数指针 → CPU 从此在 APP 的代码里取指执行
```

因为 APP 不在 `0x08000000` 起跑，它的中断向量表也不在默认位置，所以 APP 启动后必须先写 `SCB->VTOR` 做**中断向量表偏移**，否则一中断就跑飞。本实验用 `sys_nvic_set_vector_table(baseaddr, offset)` 完成，其实现在 `Drivers/SYSTEM/sys/sys.c`：

```c
SCB->VTOR = baseaddr | (offset & (uint32_t)0xFFFFFE00);
```

### 2.2 FLASH/SRAM 分区布局（代码中的真实地址）

| 区域 | 地址范围 | 依据（代码/工程文件） |
| --- | --- | --- |
| Bootloader 代码 | 从 0x08000000 起（bin 实际 38632 字节） | `IAP Bootloader V1.0` 工程链接选项 IROM1 = 0x08000000, 0x40000 |
| FLASH APP 起始 | `FLASH_APP1_ADDR = 0x08010000` | [IAP Bootloader V1.0/User/IAP/iap.h](IAP%20Bootloader%20V1.0/User/IAP/iap.h) |
| FLASH APP 链接地址 | IROM1 = 0x08010000, 大小 0x30000 | `实验13 RTC实验_FLASH_APP` 的 Keil 工程"Scatter/ROM 区域 1"设置 |
| FLASH APP 向量偏移 | `sys_nvic_set_vector_table(FLASH_BASE, 0x10000)` | [实验13 RTC实验_FLASH_APP/User/main.c](实验13%20RTC实验_FLASH_APP/User/main.c) 第 38 行 |
| Bootloader 收包缓冲 | `g_usart_rx_buf` 强制定位在 **0x20001000**，大小 44KB | `usart.c` 中 `__attribute__((at(0X20001000)))`，`USART_REC_LEN = 44 * 1024` |
| SRAM APP 运行地址 | IROM1 = 0x20001000（代码从 SRAM 取指执行） | `实验21 触摸屏实验_SRAM_APP` Keil 工程区域设置 |
| SRAM APP 向量偏移 | `sys_nvic_set_vector_table(SRAM_BASE, 0x1000)` | [实验21 触摸屏实验_SRAM_APP/User/main.c](实验21%20触摸屏实验_SRAM_APP/User/main.c) 第 217 行 |
| Bootloader/SRAM APP 自用栈 | 0x20000000~0x20000FFF（4KB 窗口） | SRAM APP 工程 RAM1 = 0x20000000, 0x1000；栈顶由 APP 向量表第 1 字给出 |

要点：**APP 的向量表偏移参数必须与跳转目标地址一致**（FLASH APP 偏移 `0x10000`，SRAM APP 偏移 `0x1000`），官方 readme.txt 也专门提醒"App 需要设置中断向量偏移，偏移位置与 BootLoader 的跳转地址相同"。

### 2.3 串口"协议"：本 V1.0 是无帧纯二进制、一次性接收

以 Bootloader 代码为准（`Drivers/SYSTEM/usart/usart.c` V1.1 修改说明："修改串口接收方式, 用于实现串口IAP升级"）：

- 接收中断 `USART1_IRQHandler` 只做一件事：把 `USART1->DR` 读出的字节依次存入 `g_usart_rx_buf[g_usart_rx_cnt++]`，最多 44KB，**没有帧头、没有命令字、没有 CRC 校验**。
- "一包结束"靠**空闲检测**：`main.c` 主循环每 `delay_ms(10)` 轮询一次，若本轮 `g_usart_rx_cnt` 与上一轮相同且非 0，就认为整个 bin 收完（`oldcount == g_usart_rx_cnt` → 判定"用户程序接收完成"并打印长度）。
- 因此上位机必须**一次性把 .bin 文件发完**（串口助手的"发送文件"功能即可，115200 波特率下发 44KB 约 4 秒），期间不要穿插发其他内容，否则 `applenth` 会把垃圾字节也算进去。

波特率核对（寄存器写法在 `usart_init`）：`temp = (sclk * 1000000 + baudrate / 2) / baudrate`，代入 sclk=72、baud=115200 得 `USART1->BRR = 625`（0x271），即 USARTDIV = 625/16 = 39.0625，72MHz/(16×39.0625) = 115200bps，误差为零。

### 2.4 FLASH_APP 与 SRAM_APP 的区别

| 对比项 | FLASH APP（RTC 实验改） | SRAM_APP（触摸屏实验改） |
| --- | --- | --- |
| 链接/运行位置 | 内部 FLASH 0x08010000 | 内部 SRAM 0x20001000 |
| 断电后是否保留 | 保留（已烧进 FLASH） | 不保留（SRAM 掉电即失） |
| Bootloader 收到后的处理 | 按半字打包写入 FLASH（`iap_write_appbin`） | 不写 FLASH，直接就在缓冲区里跳 |
| 运行操作 | 先 KEY_UP 存入 FLASH，再按 KEY1 运行 | 收完直接按 KEY0 运行 |
| 执行速度 | 从 FLASH 取指, 存在存储器等待周期(可通过 `FLASH->ACR` 的预取/零等待配置优化) | SRAM 取指无等待, 更快 |
| 合法性判据 | 复位向量高字节 = 0x08（`&0xFF000000 == 0x08000000`） | 复位向量高字节 = 0x20（`&0xFF000000 == 0x20000000`） |
| bin 体积（随附成品） | 44508 字节 | 44760 字节（上限 44KB=45056，所以官方裁剪了 7 寸屏兼容代码） |

## 三、硬件资源与接线

公共资源（三个子工程都用）：

| 资源 | 引脚 | 代码中的宏/位置 | 说明 |
| --- | --- | --- | --- |
| LED0（红灯 DS0） | PA8 | `LED0_GPIO_PORT/PIN`，`Drivers/BSP/LED/led.h` | 低电平点亮（`LED0(0)` 亮） |
| LED1（绿灯 DS1） | PD2 | `LED1_GPIO_PORT/PIN` | 低电平点亮 |
| 串口1 | TX=PA9，RX=PA10 | `USART_TX/RX_GPIO_PIN`，`Drivers/SYSTEM/usart/usart.h` | 经板载 CH340 接电脑；**USART1 排针上 PA9/PA10 需跳线帽分别接 RXD/TXD** |
| LCD（2.8/3.5/4.3/7/10 寸 MCU 屏） | 数据 PB0~PB15，RD=PC6，WR=PC7，RS=PC8，CS=PC9，背光 BL=PC10 | `Drivers/BSP/LCD/lcd.h` | 16 位 8080 并口（本寄存器版用 GPIO 模拟时序），插板子右上角 LCD 插座，只支持 MCU 屏不支持 RGB 屏 |
| 时钟 | 主频 72MHz | `sys_stm32_clock_init(9)`（PLL 倍数 9：8MHz HSE×9） | 三个子工程 main 第一行都调用 |

各子工程特有资源：

### 3.1 IAP Bootloader V1.0

| 资源 | 引脚 | 代码位置 | 说明 |
| --- | --- | --- | --- |
| KEY0 | PC5 | `KEY0_GPIO_PORT/PIN`，`Drivers/BSP/KEY/key.h`，宏值 `KEY0_PRES=1` | 运行 SRAM APP |
| KEY1 | PA15 | `KEY1_PRES=2` | 运行 FLASH APP |
| KEY_UP（WK_UP） | PA0 | `WKUP_PRES=3` | 把收到的 APP 拷贝进 FLASH |
| 内部 FLASH | — | `Drivers/BSP/STMFLASH/stmflash.c/h` | 写入目标 0x08010000 |

按键电平：KEY0/KEY1 低有效（内部上拉输入），WK_UP 高有效（下拉输入）。

### 3.2 实验13 RTC实验_FLASH_APP

| 资源 | 引脚 | 代码位置 | 说明 |
| --- | --- | --- | --- |
| RTC | 片上外设 | `Drivers/BSP/RTC/rtc.c` | 时间显示/闹钟，USMART 串口调试 |
| 串口1 + USMART | PA9/PA10 | `Middlewares/USMART/` | 可发指令改时间 |

### 3.3 实验21 触摸屏实验_SRAM_APP

| 资源 | 引脚 | 代码位置 | 说明 |
| --- | --- | --- | --- |
| 触摸屏 | 随 LCD 排插（电阻屏 XPT2046 走 SPI 模拟，电容屏走 IIC） | `Drivers/BSP/TOUCH/` | 电阻屏需校准（按 KEY0） |
| KEY0/KEY1/WK_UP、LED、串口1 | 同 3.1 | `Drivers/BSP/KEY` 等 | 电阻屏校准入口用 KEY0 |

接线总结：只需 USB 线插 **USB_UART**（板载 CH340）供串口收发 + 115200 串口助手；LCD/触摸屏插板载插座；无需其他外部模块。

## 四、代码结构与调用流程

目录树（每个子工程结构相同，只列相关文件）：

```text
实验35 串口IAP实验/
├── IAP Bootloader V1.0/
│   ├── Drivers/
│   │   ├── BSP/LED|KEY|LCD|STMFLASH/    # LED、按键、LCD、内部FLASH读写驱动
│   │   ├── SYSTEM/delay|sys|usart/      # 延时、系统(时钟/NVIC/向量表)、串口
│   │   └── CMSIS/.../startup_stm32f103xe.s  # 启动文件, 复位后从这里进 main
│   ├── Middlewares/USMART/              # 串口命令调试(本工程未在主流程用)
│   ├── User/
│   │   ├── main.c                       # Bootloader 主流程
│   │   └── IAP/iap.c|h                  # 写APP到FLASH + 跳转APP
│   ├── Projects/MDK-ARM/atk_f103.uvprojx
│   └── Output/atk_f103.bin|hex          # 编译产物(fromelf 生成)
├── 实验13 RTC实验_FLASH_APP/            # 普通RTC实验 + 向量偏移 + 链接到0x08010000
└── 实验21 触摸屏实验_SRAM_APP/          # 普通触摸屏实验 + 向量偏移 + 链接到0x20001000
```

Bootloader 的 `main()` 流程：

1. `sys_stm32_clock_init(9)` → 72MHz；`delay_init(72)`；`usart_init(72, 115200)`；`led_init`；`lcd_init`；`key_init`。
2. LCD 显示提示："KEY_UP: Copy APP2FLASH! / KEY1: Run FLASH APP / KEY0: Run SRAM APP"。
3. 主循环（10ms 一轮）：
   - 串口计数 `g_usart_rx_cnt` 连续两轮不变 → 判定 APP 收完，记录 `applenth` 并打印长度；
   - 每 30 轮（约 300ms）翻转 LED0；
   - KEY_UP（`WKUP_PRES`）→ 校验缓冲区首字是 `0x08` 开头的复位向量 → `iap_write_appbin(FLASH_APP1_ADDR, g_usart_rx_buf, applenth)` 写入 FLASH；
   - KEY1（`KEY1_PRES`）→ 校验 `FLASH_APP1_ADDR+4` 处向量合法 → `iap_load_app(FLASH_APP1_ADDR)` 跳转；
   - KEY0（`KEY0_PRES`）→ 校验 `0x20001000+4` 处向量是 `0x20` 开头 → `iap_load_app(0x20001000)` 跳转。

APP 侧流程：`sys_stm32_clock_init(9)` → **立刻设置向量表偏移** → 其余外设初始化 → 各自正常业务循环。

## 五、核心代码解析

### 5.1 IAP Bootloader V1.0

**（1）接收缓冲强制定位 0x20001000**（[IAP Bootloader V1.0/Drivers/SYSTEM/usart/usart.c](IAP%20Bootloader%20V1.0/Drivers/SYSTEM/usart/usart.c)）：

```c
#if (__ARMCC_VERSION >= 6010050)
uint8_t g_usart_rx_buf[USART_REC_LEN] __attribute__((section(".bss.ARM.__at_0x20001000")));
#else
uint8_t g_usart_rx_buf[USART_REC_LEN] __attribute__ ((at(0X20001000)));
#endif
```

含义：AC5 用 `__at`、AC6 用 `.bss.ARM.__at_` 段名，让链接器把这个 44KB 数组钉在 `0x20001000`。为什么？因为 SRAM APP 的**接收缓冲区就是它的运行地址**——串口每收一个字节直接落在最终要执行的位置上，收完即可原地跳转运行，省一次拷贝；同时 Bootloader 自己的栈和变量被挤到 `0x20000000~0x20000FFF` 这 4KB 里。

**（2）写 APP 到 FLASH：`iap_write_appbin`**（[IAP Bootloader V1.0/User/IAP/iap.c](IAP%20Bootloader%20V1.0/User/IAP/iap.c)）：

```c
for (t = 0; t < appsize; t += 2)
{
    temp = (uint16_t)dfu[1] << 8;
    temp |= (uint16_t)dfu[0];
    dfu += 2;
    g_iapbuf[i++] = temp;
    if (i == 1024)          /* 攒满2KB(1024个半字) */
    {
        i = 0;
        stmflash_write(fwaddr, g_iapbuf, 1024);
        fwaddr += 2048;
    }
}
```

逐段解释：STM32F103 内部 FLASH 只能按**半字（16 位）**编程，所以把串口收到的字节流两两合成 `uint16_t`（低字节在前，小端序），攒满 1024 个半字（正好一个 2KB 扇区，本工程 `STM32_SECTOR_SIZE = 2048`，因为 `STM32_FLASH_SIZE = 0x40000` 不小于 256K 分支）批量写入。`stmflash_write`（`Drivers/BSP/STMFLASH/stmflash.c`）内部会先读检查目标扇区，非 `0xFFFF` 就先 `stmflash_erase_sector` 擦除，再 `stmflash_write_halfword` 编程。编程就是三步寄存器操作：`FLASH->CR |= 1<<0`（PG 置位）→ `*(volatile uint16_t*)faddr = data` → 轮询 `FLASH->SR` 的 BSY 位。

**（3）跳转：`iap_load_app`**：

```c
if (((*(volatile  uint32_t *)appxaddr) & 0x2FFE0000) == 0x20000000)  /* 检查栈顶地址是否合法 */
{
    jump2app = (iapfun) * (volatile uint32_t *)(appxaddr + 4);
    sys_msr_msp(*(volatile uint32_t *)appxaddr);
    jump2app();
}
```

- 第 1 行：APP 向量表第 1 个字是**初始栈顶**。任何合法程序的栈顶都必须落在内部 SRAM（`0x20000000` 起始）附近，掩码 `0x2FFE0000` 用来过滤"空 FLASH/乱数据"这种非法镜像。
- `appxaddr + 4` 是向量表第 2 个字——**复位处理函数地址**，强转成 `iapfun`（`typedef void (*iapfun)(void);`）。
- `sys_msr_msp` 内部就一条 `__set_MSP(addr)`（编译成 `MSR MSP, r0` 指令）：先给 APP 搭好它的栈，再跳。
- 官方 V1.0 代码里这一步**没有显式关全局中断**（见"八、注意事项"第 5 条）。

**（4）main.c 的合法性双保险**（[IAP Bootloader V1.0/User/main.c](IAP%20Bootloader%20V1.0/User/main.c)）：

```c
if (((*(volatile uint32_t *)(0X20001000 + 4)) & 0xFF000000) == 0x08000000)  /* FLASH APP */
...
if (((*(volatile uint32_t *)(FLASH_APP1_ADDR + 4)) & 0xFF000000) == 0x08000000)
...
if (((*(volatile uint32_t *)(0x20001000 + 4)) & 0xFF000000) == 0x20000000)  /* SRAM APP */
```

用"复位向量的最高字节"区分两种 APP：FLASH APP 的复位地址在 `0x08xxxxxx`，SRAM APP 在 `0x20xxxxxx`。收错类型立即提示 `Illegal FLASH APP!` / `非SRAM应用程序,无法执行!`，避免跳飞。

### 5.2 实验13 RTC实验_FLASH_APP（关键差异只有 3 处）

（1）main.c 开头重设向量表（[实验13 RTC实验_FLASH_APP/User/main.c](实验13%20RTC实验_FLASH_APP/User/main.c) 第 34~38 行）：

```c
sys_stm32_clock_init(9);    /* 设置时钟, 72Mhz */

/* 设置中断向量表偏移量为0X9000 */
sys_nvic_set_vector_table(FLASH_BASE,0x10000);
```

注意：注释写 0X9000 是旧版遗留，**代码实际偏移 0x10000**，与 Bootloader 的 `FLASH_APP1_ADDR = 0x08010000` 一致，以代码为准。

（2）Keil 工程链接区域：IROM1 起始 0x08010000、大小 0x30000（工程文件 `Projects/MDK-ARM/atk_f103.uvprojx` 的芯片内存设置），这样生成的 bin 第一条指令就对应 0x08010000。
（3）编译后处理：After Build 已勾选 `fromelf.exe --bin -o ..\..\Output\atk_f103.bin ..\..\Output\atk_f103.axf`，直接产出可用于串口发送的 `Output/atk_f103.bin`。

其余内容与《实验13 RTC 实验》相同：LCD 显示时间/日期/星期（100ms 刷新）、USMART 可设置时间和闹钟、LED0 每 200ms 翻转、WAKE UP 中断里 LED1 每 2 秒闪一次。

### 5.3 实验21 触摸屏实验_SRAM_APP（关键差异）

（1）main.c 第 215~217 行：

```c
sys_stm32_clock_init(9);    /* 设置时钟, 72Mhz */

/* 设置中断向量表 在SRAM里面, 且偏移量为 0X1000 */
sys_nvic_set_vector_table(SRAM_BASE, 0x1000);
```

（2）Keil 工程：IROM1 起始改为 0x20001000（代码链接、定位在 SRAM），RAM1 改为 0x20000000、大小 0x1000（只留 4KB 栈，代码自己不放 RAM 区域）。
（3）为塞进 44KB 上限，官方裁剪了 7 寸屏兼容代码（Bootloader 的 `Drivers/SYSTEM/usart/usart.h` 在 `USART_REC_LEN = 44 * 1024` 旁注释"SRAM_APP裁剪了7寸屏的兼容代码，否则内容不够用"），成品 bin 44760 字节，逼近 45056 上限——**换更大的 APP 前先想体积**。
业务部分与《实验21 触摸屏实验》相同：按 LCD ID 分电容/电阻屏，电阻屏可用 KEY0 强制校准（依次点 5 个十字，提示 `Touch Screen Adjust OK!` 完成），进入手写画板，点右上角 RST 区域清屏。

## 六、编译、下载与运行

1. 打开工程：分别用 Keil MDK 打开三个 `Projects/MDK-ARM/atk_f103.uvprojx`（器件 STM32F103RC，启动文件 `startup_stm32f103xe.s`，C 包含路径已配好）。
2. 先编译并下载 **Bootloader**：ST-Link/J-Link 下载到 0x08000000（常规 Download 即可）。之后 **Bootloader 只烧一次**，换 APP 不再动它。
3. 编译两个 APP：注意不要用 ST-Link 直接下载 APP 工程（那会把整个 FLASH 包括 Bootloader 擦掉）；APP 只取编译产物 `Output/atk_f103.bin`。
4. 上位机：任意支持**发送文件（二进制）**的串口助手（官方推荐 XCOM/SSCOM，波特率 115200，8N1），USB 线接板子 **USB_UART** 口，USART1 排针 PA9/PA10 跳线帽接 RXD/TXD。

用串口升级 APP 的完整步骤：

- FLASH APP：Bootloader 上电跑起 → 串口助手"发送文件"选 RTC APP 的 `atk_f103.bin` → 屏显"用户程序接收完成! 代码长度:44508Bytes" → 按 **KEY_UP**（显示 `Copying APP2FLASH... → Copy APP Successed!!`）→ 按 **KEY1** 运行 RTC。
- SRAM APP：同样发送触摸屏 APP 的 bin → 直接按 **KEY0** 运行触摸（重新上电后 SRAM 内容消失，需重发）。
- 想清掉缓冲区重收：复位开发板即可（官方 readme.txt 提到"按 KEY2 手动清除接收到的 APP 程序"，但本工程代码没有 KEY2 处理，见注意事项）。

## 七、实验现象

### 7.1 IAP Bootloader V1.0
1. 上电 LCD 显示 `STM32 / IAP TEST / ATOM@ALIENTEK` 和三行按键提示；LED0 约每 300ms 闪烁。
2. 串口发送任一 APP 的 bin 文件后，串口助手收到 `用户程序接收完成! 代码长度:xxBytes`。
3. 发 FLASH APP 后按 KEY_UP 写入、KEY1 运行进 RTC 画面；发 SRAM APP 后按 KEY0 直接进触摸画面；运行 SRAM APP 时掉电重启回到 Bootloader，缓冲区已空。

### 7.2 实验13 RTC实验_FLASH_APP（从 Bootloader 跳入后）
1. LCD 显示 `STM32 / RTC TEST`，Time/Date/Week 三行每 100ms 刷新。
2. LED0 每 200ms 闪一次；设置 RTC 闹钟唤醒后 LED1 每 2 秒闪一次。
3. 串口（115200）可用 USMART 指令设置时间/闹钟、读写后备 SRAM，串口输入以回车换行结束。

### 7.3 实验21 触摸屏实验_SRAM_APP（从 Bootloader 跳入后）
1. LCD 显示 `STM32 / TOUCH TEST`，电阻屏再显示 `Press KEY0 to Adjust`。
2. 校准（电阻屏）：按 KEY0 → 依次点屏幕 5 个十字中心 → 提示 `Touch Screen Adjust OK!`（校准系数存入 24C02）。
3. 进入手写画板，手指划线；点右上角 `RST` 清屏；电容屏免校准直接手写，支持多点。

## 八、注意事项

1. **向量表偏移是头号坑**：做自己的 APP 时，必须 (a) Keil 里 IROM1 起始改成 APP 地址；(b) main 开头调 `sys_nvic_set_vector_table`，参数与跳转地址一致。忘了 (b) 表现为"能跑但没有中断/一中断就死机"。
2. **APP 体积上限 44KB**（`USART_REC_LEN = 44 * 1024`，且 `g_usart_rx_cnt` 超出后丢弃多余字节，静默截断）。APP 大了要么裁剪代码，要么改用 V2.0 的分包协议版。
3. **检查 APP 地址别压到 Bootloader**（官方 readme.txt 注意事项 6）：Bootloader 已用约 38.6KB，APP 起点 0x08010000（64KB 处）留了余量；不要擅自把 `FLASH_APP1_ADDR` 改小。
4. **发送 .bin 而不是 .hex**：`fromelf --bin` 生成的才是线性镜像；且"无校验一次性接收"意味着发送前不要往串口乱发字符，`applenth` 是累计值。
5. 本 V1.0 跳转未显式关中断：跳 APP 前 Bootloader 的 USART1 中断仍在开着的瞬间窗口里。APP 一进来就 `sys_stm32_clock_init` + 设 VTOR + 重配外设，所以实践中可用；严谨产品建议在跳转前 `__disable_irq()`（原子后续 IAP 版本如此改进）并理解自己 APP 的中断使能责任。
6. **官方文档与代码不一致处（以代码为准）**：
   - Bootloader `readme.txt` 现象描述提到"KEY2 手动清除接收到的 APP 程序"，但 `main.c`/`key.c` 无 KEY2（板子也只有 KEY0/KEY1/KEY_UP 三个键），清缓冲请用复位。
   - `iap.h` 注释"保留 0X08000000~0X08008FFF(36KB) 的空间为 Bootloader 使用"与实际不符：APP 在 0x08010000（即实际保留 64KB），且 Bootloader 的 bin 实测 38632 字节已超 36KB。
   - FLASH APP 的 `main.c` 注释"偏移量为0X9000"与实际参数 `0x10000` 不符。
7. 串口助手选型：务必 XCOM/SSCOM 等不随意拉 DTR/RTS 的助手，否则可能把 MCU 复位（官方 readme.txt 注意事项 2）。
8. 源码中文提示是 GBK 编码：Bootloader 用 `printf` 发的"用户程序接收完成!"等中文字符串按 GBK 字节流输出，串口助手要选 GBK/ANSI 解码才不乱码（选 UTF-8 会看到乱码，不是程序 bug）。
9. SRAM APP 每次复位后都要重新发送，属正常现象；FLASH APP 则常驻 0x08010000，按 KEY1 即可再进。

## 九、总结与延伸

要点：

1. IAP = 应用程序自己读写 FLASH 更新固件；Bootloader 常驻 Flash 前段，负责"收包 → 校验 → 存放 → 跳转"。
2. 跳转三板斧：校验栈顶合法 → `MSR MSP` 设栈 → 跳到向量表第 2 个字指向的地址。
3. APP 必须链接到指定地址并重设 `SCB->VTOR`（FLASH APP：0x08010000/偏移 0x10000；SRAM APP：0x20001000/偏移 0x1000）。
4. V1.0 串口协议为无帧头、无 CRC 的一次性裸传输，用计数空闲判结束；FLASH 按半字编程、按 2KB 扇区擦除。
5. FLASH APP 与 SRAM APP 是两种产品形态：前者掉电保留，后者免擦写 FLASH、上电快、可当"试用版"。

延伸练习：

1. 给 `main.c` 的接收完成判定增加"收到固定起始字节序列才算一包"，体验帧同步的思想（不必照搬完整协议，先做半帧）。
2. 把实验10 OLED 工程改造成 SRAM APP（改链接地址 + 加 `sys_nvic_set_vector_table(SRAM_BASE, 0x1000)`），验证任意工程都能当 APP。
3. 用 `stmflash_read` 回读 0x08010000 前 16 个半字与 bin 文件前 32 字节对比，确认写入正确。

衔接：本实验的"串口传文件升级"思想，正是后续 USB 升级、OTA 的雏形；内部 FLASH 读写与扇区概念可回看实验26。
