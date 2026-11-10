# 实验11 TFTLCD（MCU屏）实验

> 一句话概括：用 16 根 GPIO 数据线手敲 8080 并口时序，先"问出"屏上驱动 IC 的 ID 再灌对应初始化表，然后在彩色屏上画点、刷屏、显示字符——理解"MCU 屏 = 一块可按坐标寻址的 GRAM + 一个索引/数据通道"。

## 一、实验简介

- **实验目的**（官方 `readme.txt`）：学习彩色 TFTLCD 显示屏（MCU 屏）的使用。具体拿下 4 件事：
  1. 并口总线怎么区分"**写寄存器编号**"和"**写数据**"（本实验用 `RS` 引脚；FSMC 版用地址线，见 2.2）；
  2. 一块屏背后可能是 7 种驱动 IC（ILI9341/ST7789/NT35310/ST7796/NT35510/ILI9806/SSD1963），怎么**读 ID 自动适配**；
  3. GRAM 是**按坐标开窗**写的：`0x2A` 设 X、`0x2B` 设 Y、`0x2C` 起连续写颜色；
  4. 像素用 **RGB565**（16 位）表示，清屏/画点/字符本质都是往 GRAM 里灌 16 位数。
- **在学习路线中的位置**：前置实验 1（GPIO）、实验 4（串口打印 ID）、实验 10（OLED，同样是"影子显存 + 手动总线时序"，
  本实验是它的彩色 16 位升级版）；后续实验 12（USMART 用 LCD 函数做在线调试）、21（触摸屏）、30（汉字）、31（图片）都建在这份驱动上。
- **开发板与芯片**：MiniSTM32 V4，`STM32F103RC`（72MHz）。**需自备** ALIENTEK 2.8/3.5/4.3/7/10 寸 TFTLCD 模块，
  **仅限 MCU 屏（16 位 8080 并口驱动），不支持 RGB 屏**。

## 二、实验原理

### 2.1 MCU 屏与 RGB 屏的区别

| 类型 | 谁产生扫描时序 | 接口 | 显存在哪 |
| --- | --- | --- | --- |
| **MCU 屏**（本实验） | 屏上的驱动 IC（ILI9341 等）自己扫 | 8080 并口 / SPI，几根线就能"写一个像素" | 驱动 IC 内部 GRAM，MCU 按坐标喂数据即可 |
| **RGB 屏** | MCU 要连续推像素流（DE/HS/VSYNC + D0~D23） | 需要 LCD 控制器（LTDC 等） | MCU 侧必须有显存并持续刷新，F103 这套接口带不动 |

所以 MCU 屏"像访问外设寄存器那样"操作：给坐标 → 发颜色 → 屏自己保持。这也是官方强调"仅支持 MCU 屏"的原因。

### 2.2 8080 并口的 `RS` 线，与"索引寄存器 / 数据寄存器"的关系

接口线就是 16 根数据线 `D0~D15` + `CS`（片选）+ `RS`（寄存器选择）+ `WR`（写）+ `RD`（读），另有 `RST`、`BL`。
**核心是 `RS`**：`RS=0` 表示此刻总线上是**寄存器编号（索引）**，`RS=1` 表示是**寄存器数据 / GRAM 像素颜色**。
真实的两个写操作（[lcd.c](Drivers/BSP/LCD/lcd.c)、[lcd.h](Drivers/BSP/LCD/lcd.h)）：

```c
__attribute__((always_inline)) void lcd_wr_regno(volatile uint16_t regno)
{
    LCD_RS(0);              /* RS=0,表示写寄存器 */
    LCD_CS(0);
    LCD_DATA_OUT(regno);    /* 写入要写的寄存器序号 */
    LCD_WR(0);
    LCD_WR(1);
    LCD_CS(1);
}
```

```c
#define lcd_wr_data(data)\
    {\
        LCD_RS(1);\
        LCD_CS(0);\
        LCD_DATA_OUT(data);\
        LCD_WR(0);\
        LCD_WR(1);\
        LCD_CS(1);\
    }
```

顺序很关键：**先选通道（RS）→ 再开片选（CS=0）→ 数据摆上总线 → 给一个 WR 脉冲**，
数据在 `WR` 的**上升沿**被锁存，最后释放 CS。先脉冲后摆数据就会锁进一个旧值。
两个写法唯一的差别就是第一句 `RS` 是 0 还是 1。

**与 FSMC 版的对应关系**：FSMC（Flexible Static Memory Controller，静态存储控制器）可以把外接屏当成一块
外部 SRAM——CPU 一句 `*(uint16_t*)地址 = 值`，硬件就自动把地址锁存、NOE/NWE 读选通、建立/保持时间全套时序做完。
FSMC 版通常把 `RS` 接到地址线 `A0`（或 `A16`），于是两个通道被映射成**两个内存半字地址**，也就是资料里常说的
"索引寄存器 `0x0000`、数据寄存器 `0x0002`"（相差 2 是因为 16 位总线一个地址单位是半字，最低那位地址线就是 RS）：

| 语义 | 本工程（8080 GPIO 版） | FSMC 版（对照理解） |
| --- | --- | --- |
| 写寄存器编号 | `lcd_wr_regno(0x2C)` → `LCD_RS(0)` | `*(volatile uint16_t*)(LCD_BASE + 0x0000) = 0x2C` |
| 写数据 / GRAM | `lcd_wr_data(0xF800)` → `LCD_RS(1)` | `*(volatile uint16_t*)(LCD_BASE + 0x0002) = 0xF800` |

**本工程没有使用 FSMC**：`Drivers/BSP/LCD/` 里没有任何 FSMC 寄存器操作，16 根数据线全接在 `PB0~PB15`
（`LCD_DATA_GPIO_PORT = GPIOB`、`LCD_DATA_GPIO_PIN = 0XFFFF`），而 PB 口并不提供 FSMC 数据线功能（F103 的 FSMC 数据线在 PD/PE 组）。
于是全部改成**用 GPIO 手动敲时序**：

```c
#define LCD_WR(x)       LCD_WR_GPIO_PORT->BSRR = LCD_WR_GPIO_PIN << (16 * (!x))     /* 设置WR引脚 */
#define LCD_DATA_OUT(x) LCD_DATA_GPIO_PORT->ODR = x                                 /* 写D0~D15引脚 */
#define LCD_DATA_IN     LCD_DATA_GPIO_PORT->IDR                                     /* 读D0~D15引脚 */
```

`BSRR` 是"位置位/清零寄存器"：低 16 位写 1 = 该脚输出高，高 16 位写 1 = 该脚输出低，**只写不清、不会影响别的脚**，
所以 `pin << (16 * (!x))` 一个表达式同时覆盖了置高与拉低，比"读 ODR→改位→写回"少一步也更安全。

代码里还能看到 `0x2C00`、`0x2A00` 这类"怪的"寄存器号，那是 **NT35510** 的习惯（它的寄存器号是 16 位的，
低字节才是命令码）：`if (lcddev.id == 0x5510) { lcddev.wramcmd = 0X2C00; lcddev.setxcmd = 0X2A00; ... }`。
和"索引/数据分地址"是同一类思路，别顺手改成 `0x2C`。

### 2.3 读数据：数据线是双向的，要临时换方向

写只往 PB 口灌，读 ID / 读点却要屏反过来驱动这 16 根线，所以 `lcd_rd_data()` 里做了两次 GPIO 模式重配：

```c
static uint16_t lcd_rd_data(void)
{
    volatile uint16_t ram;  /* 防止被优化 */

    /* LCD_DATA 引脚模式设置, 上拉输入, 准备接收数据 */
    sys_gpio_set(LCD_DATA_GPIO_PORT, LCD_DATA_GPIO_PIN,
                 SYS_GPIO_MODE_IN, SYS_GPIO_OTYPE_PP, SYS_GPIO_SPEED_HIGH, SYS_GPIO_PUPD_PU);

    LCD_RS(1);              /* RS=1,表示操作数据 */
    LCD_CS(0);
    LCD_RD(0);
    lcd_opt_delay(2);
    ram = LCD_DATA_IN;      /* 读取数据 */
    LCD_RD(1);
    LCD_CS(1);

    /* LCD_DATA 引脚模式设置, 推挽输出, 恢复输出状态 */
    sys_gpio_set(LCD_DATA_GPIO_PORT, LCD_DATA_GPIO_PIN,
                 SYS_GPIO_MODE_OUT, SYS_GPIO_OTYPE_PP, SYS_GPIO_SPEED_HIGH, SYS_GPIO_PUPD_PU);

    return ram;
}
```

三个细节：① `volatile` 防止编译器把这次读优化掉；② `MODE_IN + PUPD_PU` = `CNF=10, MODE=00` 即**上拉输入**，
屏不驱动时线不会飘；③ `lcd_opt_delay(2)` 是个 `while(i--)` 空循环，给 RD 拉低到数据稳定留时间——
这就是读时序里的**建立时间**，优化等级变了可能要调，所以专门留了个函数。

### 2.4 GRAM、坐标与 RGB565

驱动 IC 内部 GRAM 靠 3 条命令访问（命令号随 IC 不同，存在 `lcddev` 结构里）：

| 成员 | ILI9341 等的值 | 作用 |
| --- | --- | --- |
| `setxcmd` | `0x2A` | 设 X 窗口（起始/结束，各 2 字节） |
| `setycmd` | `0x2B` | 设 Y 窗口 |
| `wramcmd` | `0x2C` | 开始写 GRAM；此后每写一个 16 位数，内部光标按扫描方向自动 +1 |

画一个点就是三步（真实代码）：

```c
void lcd_draw_point(uint16_t x, uint16_t y, uint32_t color)
{
    lcd_set_cursor(x, y);       /* 设置光标位置 */
    lcd_write_ram_prepare();    /* 开始写入GRAM */
    lcd_wr_data(color);
}
```

**RGB565**：1 像素 16 位，R 高 5 位、G 中 6 位、B 低 5 位（绿色多一位因为人眼对绿更敏感）：

| 宏 | 值 | 拆解 `RRRRR GGGGGG BBBBB` | 说明 |
| --- | --- | --- | --- |
| `RED` | `0xF800` | `11111 000000 00000` | 只有 R |
| `GREEN` | `0x07E0` | `00000 111111 00000` | 只有 G |
| `BLUE` | `0x001F` | `00000 000000 11111` | 只有 B |
| `YELLOW` / `CYAN` / `MAGENTA` | `0XFFE0` / `0X07FF` / `0XF81F` | 代码注释：R+G / G+B / B+R | 二次色 |
| `WHITE` / `BLACK` | `0xFFFF` / `0x0000` | 全 1 / 全 0 | 另有 `BROWN`/`GRAY`/`LGRAY` 等 |

`lcd_ex_ili9341_reginit()` 里 `0x3A` 写 `0x55` = 选 16bit/pixel（RGB565）；`0x36` 写 `0x48`（含 BGR 位），
配合 `lcd_scan_dir()` 的 "9341 & 7789 & 7796 要设置 BGR 位" → `regval |= 0X08;`。
**不置 BGR 就红蓝互换**，这是新手调 LCD 最常见的"颜色不对"根因。

刷屏数据量：竖屏 240×320 = 76,800 点 × 2 字节 = **153,600 字节/帧**。`lcd_clear()` 的优化实测写在注释里
（-O2、纯刷屏：普通函数 15 帧 → `__forceinline` 39 帧 → 宏 51 帧 → 再把 RS/CS 提到循环外 ≈ 78 帧）：

```c
    LCD_RS(1);                      /* RS=1,表示写数据 */
    LCD_CS(0);

    for (index = 0; index < totalpoint; index++)
    {
        LCD_DATA_OUT(color);        /* 写入要写的数据 */
        LCD_WR(0);
        LCD_WR(1);
    }

    LCD_CS(1);
```

**看懂这段就看懂整个 GRAM 机制**：开窗 + 发 `wramcmd` 之后，只需不断重复"数据上线 + WR 脉冲"，
光标自己按 `0x36` 设定的方向前进，不用逐点重设坐标——这就是连续刷屏比逐点画快几个数量级的原因。

### 2.5 驱动 IC 识别与初始化流程

`lcd_init()` 按顺序做了：

```text
① 开时钟：PC(BL/CS/WR/RD/RS) + PB(数据) → RCC->APB2ENR
② sys_gpio_remap_set(24,3,2)：关 JTAG 保 SWD，释放 PB3/PB4 给数据线
③ 6 次 sys_gpio_set 配推挽输出（数据线一次传 0XFFFF，50MHz），WR/RD/CS/RS/DATA 全置 1（空闲态）
④ ID 探测链：0XD3→9341? → 0X04→7789?(读回 8552 转成 7789) → 0XD4→5310? → 0XD3→7796?
   → 0xF000 密钥 + 0xC500/0xC501→5510? → 0XD3→9806? → 0xA1→(读回 5761 强制设为 1963)?
⑤ printf("LCD ID:%x\r\n", lcddev.id)   ← 串口打印
⑥ 按 ID 调对应 lcd_ex_xxx_reginit() 灌初始化寄存器表
⑦ lcd_display_dir(0) 默认竖屏（设 wramcmd/setxcmd/setycmd 与分辨率）
⑧ LCD_BL(1) 点背光 → lcd_clear(WHITE)
```

第 ④ 步为什么要"先 dummy read 一次"？发完命令号后总线上的第一个读回值通常是残留/无效数据，手册规定要丢掉：

```c
    lcd_wr_regno(0XD3);
    lcddev.id = lcd_rd_data();  /* dummy read */
    lcddev.id = lcd_rd_data();  /* 读到0X00 */
    lcddev.id = lcd_rd_data();  /* 读取0X93 */
    lcddev.id <<= 8;
    lcddev.id |= lcd_rd_data(); /* 读取0X41 */
```

第 ⑥ 步的初始化表在 [lcd_ex.c](Drivers/BSP/LCD/lcd_ex.c)（7 个 `lcd_ex_*_reginit()`）。这个 `.c` **没加进 Keil 工程**，
而是被 `lcd.c` 用 `#include` 带进来的（文件头注释还特别提醒"不要在其他文件再包含该 .c 文件!!否则会报错!"）：

```c
#include "./BSP/LCD/lcd_ex.c"
```

以 ILI9341 为例，几十组"寄存器号 + 参数"可归成 5 类：

| 寄存器 | 代码里的值 | 管什么 |
| --- | --- | --- |
| `0xCF/0xED/0xE8/0xCB/0xF7/0xEA` | 一组厂商参数 | 电源泵与时序内部配置 |
| `0xC0/0xC1/0xC5/0xC7` | `0x1B`/`0x01`/`0x30,0x30`/`0xB7` | VGH/VGL、VCM（影响对比度、闪屏） |
| `0x36`=`0x48` / `0x3A`=`0x55` | Memory Access / Pixel Format | 扫描方向 + BGR；16 位 RGB565 |
| `0xB1/0xB6/0xF2/0x26/0xE0/0xE1` | 含 15 个 gamma 值 | 帧率、显示功能、伽马曲线 |
| `0x2A`→`00 00 00 EF`、`0x2B`→`00 00 01 3F` | 开窗 | 240×320 全屏 |
| `0x11` + `delay_ms(120)` → `0x29` | Exit Sleep / Display On | 唤醒后必须等 120ms 再开显示 |

`lcd_ex.c` 里写参数用真函数 `lcd_wr_xdata()`（功能时序与宏 `lcd_wr_data` 一模一样，但省代码空间、速度慢），
因为初始化只跑一次；刷屏路径才必须用宏（追帧率）。`lcd.h` 的注释把三档实测帧率写得很直白。

### 2.6 参数计算（代入本工程实际数值）

- **主频**：`sys_stm32_clock_init(9)` → HSE 8MHz × 9 = 72MHz，`HCLK = PCLK2 = 72MHz`、`PCLK1 = 36MHz`。
  一次 `ODR/BSRR` 写约几个 72MHz 时钟周期（几十 ns 量级），满足 ILI9341 并口 WR 低脉宽要求，所以不必插延时。
- **串口**：`usart_init(72, 115200)` 里 `temp = (sclk*1000000 + baudrate/2)/baudrate = (72000000+57600)/115200 = 625`，
  `USART1->BRR = 625 (0x271)`，实际波特率 72MHz/625 = 115200，误差 0。
- **清屏耗时**：76,800 次 WR 脉冲；按代码注释 -O2 实测约 78 帧/秒 → 单帧约 12.8ms。
  `main1.c` 在循环里插了 `delay_ms(1)`，于是刷屏变成约 **76.8 秒**一屏——故意做慢让你看清扫描过程。

## 三、硬件资源与接线

### 3.1 片上资源与引脚

| 资源 | 引脚 | 代码中的宏 / 位置 | 说明 |
| --- | --- | --- | --- |
| LCD 数据线 D0~D15 | PB0~PB15 | `LCD_DATA_GPIO_PORT/PIN(0XFFFF)`，[lcd.h](Drivers/BSP/LCD/lcd.h) | 16 位并口，双向，读时临时改输入 |
| LCD_CS | PC9 | `LCD_CS_GPIO_PORT/PIN` | 片选，低有效 |
| LCD_RS | PC8 | `LCD_RS_GPIO_PORT/PIN` | 0=写寄存器号，1=写数据（代替 FSMC 的地址线） |
| LCD_WR | PC7 | `LCD_WR_GPIO_PORT/PIN` | 写脉冲，上升沿锁存 |
| LCD_RD | PC6 | `LCD_RD_GPIO_PORT/PIN` | 读脉冲，读 ID / 读点 |
| LCD_BL（背光） | PC10 | `LCD_BL_GPIO_PORT/PIN` | `LCD_BL(1)` 点亮；SSD1963 亮度另由 `lcd_ssd_backlight_set()` 控制 |
| LCD_RST | 与 MCU 复位脚共用 | `LCD_RST_*` 宏在代码里已注释 | 按复位键即复位屏 |
| LED0（红灯） | **PA8** | `LED0_GPIO_PORT/PIN`，[led.h](Drivers/BSP/LED/led.h) | 低电平点亮，本实验做心跳 |
| LED1（绿灯） | **PD2** | `LED1_GPIO_PORT/PIN` | 本实验未使用 |
| 串口1 | PA9=TX / PA10=RX | [usart.h](Drivers/SYSTEM/usart/usart.h) | 115200，打印 `LCD ID:xxxx` |
| SWD | PA13/PA14 | `sys_gpio_remap_set(24,3,2)` 保留 | 关 JTAG 同时释放 PB3/PB4 |

### 3.2 接线与插法

1. 屏插到板上**丝印 TFTLCD 的排座**（2.8/3.5/4.3/7/10 寸 MCU 屏通用），**必须是 16 位 8080 并口版**；RGB 屏插上去不会亮。
2. 断电插拔，对准排座丝印的 **VCC / Pin1 方向**；这块屏功耗比 OLED 大（背光可达上百 mA），插反烧的是背光和驱动 IC。
3. 要看串口打印：USB 线连板上 `USB_UART`，并把 **USART1 排针上 PA9→RXD、PA10→TXD 各盖一个跳线帽**
   （官方 `readme.txt` 第 5 条专门提醒），串口助手 115200-8-N-1。
4. 数据线占了 PB0~PB15 全部 16 脚，PB3/PB4 靠"关 JTAG"才可用（`lcd_init()` 已做）；自己加外设别再抢 PB 口。

### 3.3 三个 main 文件演示什么，怎么在 Keil 里切换

| 文件 | 演示内容 | 是否在 Keil 工程里 |
| --- | --- | --- |
| `User/main.c` | 官方主示例：12 种底色轮流 `lcd_clear()` + 4 行字符串 + 显示 LCD ID + LED0 每秒翻转 | **在**（User 组，`..\..\User\main.c`） |
| `User/main1.c` | **扫描方式可视化**：自己写 `lcd_clear1()`，在写 GRAM 循环里插 `delay_ms(1)`，慢到能看清点是怎么一行行走的；只跑 5 种颜色，并调 `lcd_scan_dir(L2R_U2D)` | 不在工程里 |
| `User/main2.c` | **8 种扫描方向**：封装 `lcd_direction(x)`，依次 8 个方向各配一种底色，并画 `X--->` / `Y\|…\|V` 箭头示意轴向 | 不在工程里 |

切换步骤（**同时只能有一个 `main()` 参与编译，否则链接器报重复定义**）：

1. 打开 `Projects/MDK-ARM/atk_f103.uvprojx`，`F7` 确认 `main.c` 能编译。
2. 菜单 `Project → Manage → Manage Project Items…`，选中 `User` 组，把 `..\..\User\main1.c` 加进去。
3. 把原 `main.c` 从该组移除；或更稳妥：工程树右键 `main.c` → `Options for File…` → 取消勾选 *Include in Target Build*。
4. 换 `main2.c` 同理（每次只留一个含 `main()` 的文件参与编译），`F7` → `F8`。
5. 顺带注意：`main1.c`/`main2.c` 是 **UTF-8 编码**，而本仓库其余源文件是 **GBK**，Keil 里中文注释可能乱码；
   它们不进工程，乱码不影响编译，但别顺手把 GBK 源文件"另存为 UTF-8"。

## 四、代码结构与调用流程

```
实验11 TFTLCD（MCU屏）实验/
├── User/  main.c / main1.c / main2.c    三套演示（见 3.3）
├── Drivers/
│   ├── BSP/LCD/lcd.h        引脚宏、lcd_wr_data 宏、扫描方向与 RGB565 颜色、_lcd_dev 结构、函数声明
│   ├── BSP/LCD/lcd.c        总线时序、ID 探测、GRAM 读写、画点/线/圆/矩形、字符与字符串
│   ├── BSP/LCD/lcd_ex.c     7 种驱动 IC 的寄存器初始化表（由 lcd.c #include 进来）
│   ├── BSP/LCD/lcdfont.h    ASCII 点阵字库 1206/1608/2412/3216
│   ├── BSP/LED/led.c/.h     LED0(PA8)/LED1(PD2)
│   └── SYSTEM/sys|delay|usart   时钟、SysTick 延时、串口1(printf 重定向)
├── Projects/MDK-ARM/atk_f103.uvprojx   Keil 工程（Target 名：TFTLCD）
├── Output/atk_f103.hex
└── readme.txt
```

`main()` 流程：

```text
1 sys_stm32_clock_init(9) → 72MHz
2 delay_init(72)          → 计 72 下 = 1µs
3 usart_init(72, 115200)  → 必须先跑，否则 lcd_init() 里的 printf 会卡死
4 led_init()              → 心跳灯
5 lcd_init()              → GPIO + 读 ID + 灌初始化表 + 竖屏 + 开背光 + 清屏
6 g_point_color = RED     → 全局画笔颜色
7 sprintf(lcd_id,"LCD ID:%04X",lcddev.id)   ← lcd_id[12] 正好装 11 字符 + '\0'
while(1):
  a lcd_clear(第 x 种颜色)             → 整屏纯色
  b lcd_show_string(...)×4             → STM32 / TFTLCD TEST / ATOM@ALIENTEK / LCD ID:xxxx
  c x++，x==12 归 0
  d LED0_TOGGLE(); delay_ms(1000)
```

## 五、核心代码解析

### 5.1 `User/main.c`

```c
    sys_stm32_clock_init(9);    /* 设置时钟, 72Mhz */
    delay_init(72);             /* 延时初始化 */
    usart_init(72, 115200);     /* 串口初始化为115200 */
    led_init();                 /* 初始化LED */
    lcd_init();                 /* 初始化LCD */
    g_point_color = RED;
    sprintf((char *)lcd_id, "LCD ID:%04X", lcddev.id);  /* 将LCD ID打印到lcd_id数组 */

    while (1)
    {
        switch (x)
        {
            case 0:
                lcd_clear(WHITE);
                break;
            /* case 1~11 依次为 BLACK/BLUE/RED/MAGENTA/GREEN/CYAN/YELLOW/BRRED/GRAY/LGRAY/BROWN */
        }

        lcd_show_string(10, 40, 240, 32, 32, "STM32", RED);
        lcd_show_string(10, 80, 240, 24, 24, "TFTLCD TEST", RED);
        lcd_show_string(10, 110, 240, 16, 16, "ATOM@ALIENTEK", RED);
        lcd_show_string(10, 130, 240, 16, 16, (char*)lcd_id, RED);  /* 显示LCD ID */
        x++;

        if (x == 12) x = 0;

        LED0_TOGGLE();  /* LED0闪烁 */
        delay_ms(1000);
    }
```

- `lcd_show_string(x, y, width, height, size, p, color)` 的第 3、4 参数是**允许显示区域**，越界会换行/退出；
  `width` 写 240 表示"竖屏宽度以内不折行"。
- 底色每 12 秒一轮循环；**文字固定是 `RED`**，所以轮到红色底色那一秒文字会"隐身"——这正是让你直观感受
  "前景/背景同色看不见"的设计。
- `lcd_id` 用 `uint8_t` 数组再强转 `(char*)`，只为演示"数字 → 显示字符串"这条路。

### 5.2 `main1.c` 的 `lcd_clear1()`：把刷屏拆开看时序

```c
    uint32_t totalpoint = lcddev.width;
    totalpoint *= lcddev.height;    /* 得到总点数 */
    lcd_set_cursor(0x00, 0x0000);   /* 设置光标位置 */
    lcd_write_ram_prepare();        /* 开始写入GRAM */

    LCD_RS(1);                      /* RS = 1 表示写数据 */
    LCD_CS(0);

    for (index = 0; index < totalpoint; index++)
    {
        delay_ms(1);  /* 可以通过调节延时函数来观察LCD屏扫描方式 */
        LCD_DATA_OUT(color);        /* 写入要写的数据 */
        LCD_WR(0);
        LCD_WR(1);
    }

    LCD_CS(1);
```

与 `lcd_clear()` 只差一个 `delay_ms(1)`。见 2.4 末尾的说明：连续写 GRAM 时驱动 IC 光标自动前进。

### 5.3 `main2.c` 的 `lcd_direction()` 与 `lcd_scan_dir()`

`lcd_direction(x)` 只是把 8 个宏（`L2R_U2D`/`L2R_D2U`/`R2L_U2D`/`R2L_D2U`/`U2D_L2R`/`U2D_R2L`/`D2U_L2R`/`D2U_R2L`）
翻译成 8 次 `lcd_scan_dir()` 调用。`lcd_scan_dir()` 内部做三件事：

1. 把方向翻译成 `0x36`（NT35510 是 `0x3600`）寄存器的 bit7/bit6/bit5，即 `MY`（上下翻）/`MX`（左右翻）/`MV`（行列交换）；
2. 对 9341/7789/7796 补上 BGR 位 `regval |= 0X08;`；
3. **`MV=1` 时宽高要交换**，于是 `lcddev.width/height` 互换，并重新设置显示窗口（开窗）：

```c
        if (regval & 0X20)
        {
            if (lcddev.width < lcddev.height)   /* 交换X,Y */
            {
                temp = lcddev.width;
                lcddev.width = lcddev.height;
                lcddev.height = temp;
            }
        }
```

`lcd.h`/`lcd.c` 里 `lcd_scan_dir()` 的注释也提醒："注意:其他函数可能会受到此函数设置的影响(尤其是9341)，
所以,一般设置为L2R_U2D即可,如果设置为其他扫描方式,可能导致显示不正常"。**`main2.c` 是观察实验，不是推荐用法。**

### 5.4 `lcd_init()` 里为什么数据线用 50MHz 而 OLED 实验只用 10MHz

`lcd_init()` 的 6 次 `sys_gpio_set(...)` 全部传 `SYS_GPIO_SPEED_HIGH`（50M），因为这里要抢帧率、排座走线短；
代价是边沿更陡，接触不良/插错时更容易表现为花屏。`oled_init()` 用 `SYS_GPIO_SPEED_MID`（10M）求稳。
这两处差异是理解"GPIO 速度档位要按总线速率和走线权衡"的最好例子。

## 六、编译、下载与运行

1. `File → Open Project` → `实验11 TFTLCD（MCU屏）实验/Projects/MDK-ARM/atk_f103.uvprojx`（Target 名 `TFTLCD`）。
2. 关键设置（已配好）：Device `STM32F103RC`，`IRAM(0x20000000,0x0000C000)`、`IROM(0x08000000,0x00040000)`（48KB/256KB）；
   Define **`STM32F103xE`**；Include Paths：`..\..\Drivers\CMSIS\Device\ST\STM32F1xx\Include`、`..\..\Drivers\CMSIS\Include`、
   `..\..\Drivers`、`..\..\User`；分组 `Startup`/`User`/`Drivers/SYSTEM`/`Drivers/BSP`。
   `lcd_ex.c` **不在文件列表里**（靠 `#include` 生效），手动加进工程会重复定义。
3. `F7` 编译 → `F8` 下载（ST-Link/J-Link SWD）；无调试器时 BOOT0=1 用串口 ISP 烧 `Output/atk_f103.hex`。
4. 上位机：XCOM/SSCOM，115200-8-N-1；每按一次复位看一次 `LCD ID:xxxx`。
5. 想复现"51/78 帧"的刷屏速度需开 `-O2`；默认优化等级也能正常显示，只是慢。

## 七、实验现象

1. 上电/复位后屏先白一下（`lcd_init()` 结尾 `lcd_clear(WHITE)`），随后进入主循环。
2. 每 1 秒换一种底色，顺序 白→黑→蓝→红→品红→绿→青→黄→棕红→灰→浅灰→棕，12 秒一轮。
3. 每种底色上固定 4 行红字：`STM32`(32号)、`TFTLCD TEST`(24号)、`ATOM@ALIENTEK`(16号)、`LCD ID:0X9341`(16号，
   随实际 IC 变化，如 `0X7796`/`0X5510`/`0X1963`)；底色与文字同色时那一行"看不见"属正常。
4. 串口（115200）每按一次复位打印一行 `LCD ID:xxxx`（格式是 `%x`，十六进制但不带 0X 前缀）。
5. LED0（PA8 红灯）每 1 秒翻转一次。
6. 换 `main1.c` 编译：屏幕以"看得见的速度"从一角爬到另一角铺满纯色（约 76 秒一屏），直观看到扫描方向。
7. 换 `main2.c`：每 3 秒换一种扫描方向 + 底色，`X--->`/`Y↓` 箭头随方向改变。

## 八、注意事项

1. **只支持 MCU 屏，不支持 RGB 屏**（官方第 6 条）；插 RGB 屏不亮不是板子坏了。
2. **必须先 `usart_init()` 再 `lcd_init()`**，否则卡在 `printf`。代码里写得非常直白：

   ```c
   /* 特别注意, 如果在main函数里面屏蔽串口1初始化, 则会卡死在printf
    * 里面(卡死在f_putc函数), 所以, 必须初始化串口1, 或者屏蔽掉下面
    * 这行 printf 语句 !!!!!!!
    */
   printf("LCD ID:%x\r\n", lcddev.id); /* 打印LCD ID */
   ```

   不用串口就同时屏蔽这句 `printf`。
3. **PB3/PB4 依赖"关 JTAG"**：`sys_gpio_remap_set(24, 3, 2)` 把 `AFIO->MAPR` 的 `SWJ_CFG[2:0]` 设成 2。
   删掉这句 → PB3/PB4 仍是 JTAG 功能 → 数据位 bit3/bit4 写不进去，现象是固定花点/条纹。
   关 JTAG 后 SWD（PA13/PA14）仍可用，下载调试不受影响，但 SWO 跟踪不能用了。
4. **代码与官方说明的差异**：
   - 官方 `readme.txt` 现象段写"**使用战舰开发板**丝印标号为 TFTLCD 的排座"，而同一份 `readme.txt` 开头写的是
     MiniSTM32 V4（本工程平台也是 MiniSTM32 V4）。按自己板子的丝印插即可。
   - `readme.txt` 的 LED0=PA8、LED1=PD2 与 [led.h](Drivers/BSP/LED/led.h) **一致**；不一致的是实验 10 的 `readme.txt`（写成 PB5/PE5）。
   - `readme.txt` 说"不停显示一些信息并不断切换底色，同时显示 LCD 驱动器的 ID"，与 `main.c` 相符；
     本工程默认 `lcd_display_dir(0)`（竖屏），想横屏需自己调（或在实验 12 里用命令在线试）。
5. **红蓝互换 / 颜色诡异** → BGR 位（`0x36` 的 bit2）问题，`lcd_scan_dir()` 已为 9341/7789/7796 处理；
   自己写初始化时要照抄 `regval |= 0X08;`。
6. **NT35510/SSD1963 的寄存器号是 `0x2C00` 这种形式**（见 2.2），别"顺手改成 `0x2C`"，会全黑屏。
7. `lcd_ex.c` **只能被 `lcd.c` include 一次**，不要加入工程文件列表、也不要被第三个文件包含，否则重复定义。
8. **别带电插拔、别插反**；背光常亮会加速衰减，调试时可临时 `LCD_BL(0)` 关灯。
9. **读 GRAM 依赖屏支持读操作**：`lcd_read_point()` 里不同 IC 差别很大（9341/5310/5510/7789/9806 分 2 次读、
   7796 一次读完、1963 直接返回）。若读不回数据，ID 会是 `0xFFFF` 或 `0`，此时任何 `reginit` 分支都不会执行，
   屏幕常黑——先查排座接触与 PB3/PB4。
10. 源码 GBK 编码，`main1.c`/`main2.c` 为 UTF-8，编辑器里要分别设置编码，避免保存时写坏中文注释。

## 九、总结与延伸

**要点**

1. "索引通道 / 数据通道"是 MCU 屏的灵魂：`RS=0` 发寄存器号、`RS=1` 发数据；FSMC 版把它换成 `+0x0000`/`+0x0002` 两个地址。
2. 写 GRAM 的最优姿势：`0x2A/0x2B` 开窗 → 发 `0x2C` → 之后只管连续给 WR 脉冲，光标自动走。
3. 颜色是 RGB565（5-6-5），像素格式由 `0x3A=0x55` 选定，BGR 位决定红蓝是否互换。
4. 一份驱动支持 7 种 IC 的秘诀：先探测 ID（每条命令都带 dummy read），再把
   `width/height/id/dir/wramcmd/setxcmd/setycmd` 缓存进 `lcddev`，其余函数全部围绕它分支。
5. 寄存器版提速靠"少动寄存器"：`BSRR` 单向写、`ODR` 整字写、宏代替函数、CS/RS 提到循环外。

**延伸小练习**

1. 在 `main.c` 循环里加 `lcd_draw_circle(120,160,50,YELLOW);`、`lcd_draw_line(0,0,239,319,GREEN);`、
   `lcd_fill(10,250,100,290,BLUE);`，对比"逐点画"和"区域填充"的耗时差异。
2. 让文字颜色随底色自动取反色（用 `~` 或查色表），做出"永远看得清"的效果，理解 `g_point_color`/`g_back_color` 的作用。
3. 注释掉 `sys_gpio_remap_set(24, 3, 2)` 重编译烧录，观察花屏，再恢复——理解引脚复用最快的实验。
4. 读一遍 `lcd_read_point()` 的 IC 分支，到实验 12 里用命令在线取点验证颜色值。

**衔接下一课**：实验 12 USMART 把本实验的 `lcd_clear/lcd_fill/lcd_draw_line/lcd_show_string` 等注册进串口命令表，
不用重编译就能敲 `lcd_draw_line 0 0 239 319 0XF800` 画图——这是检验这份驱动稳不稳最省事的办法。
再往后：实验 21 在同一块屏上做触摸画板，实验 30 加汉字字库，实验 31 从 TF 卡读 BMP 显示，用的都是这套 `lcd.c` 接口。
