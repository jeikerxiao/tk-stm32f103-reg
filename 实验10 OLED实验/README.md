# 实验10 OLED实验

> 一句话概括：先在内存里画好一张 128×64 的"影子显存"，再用 GPIO 手敲 8080 并口时序把它整块搬进 SSD1306 OLED——理解"显示 = 往显存填像素位 + 按总线时序搬运"。

## 一、实验简介

- **实验目的**（官方 `readme.txt`）：学习 OLED 显示屏的使用。展开三件事：
  1. 认识**显存（GRAM）**：程序里先准备一块内存描述"哪个点亮哪个灭"，再整块交给屏幕；
  2. 认识 SSD1306 控制器的**命令/数据双通道**与**页寻址**；
  3. 学会用普通 GPIO **软件模拟并行总线时序**（不需要任何硬件外设参与）。
- **在学习路线中的位置**：前置实验 1（GPIO 输出、`CRL/CRH`）、实验 4（串口，`main()` 里仍初始化它）；
  本实验是"显示与人机交互"的第一站，后面实验 11（TFTLCD 彩色屏，思路同源）、实验 12（USMART 在线调函数）都接在它之后。
- **开发板与芯片**：正点原子 MiniSTM32 V4，`STM32F103RC`（Cortex-M3，72MHz，256KB FLASH / 48KB SRAM）。
  **需自备**正点原子 0.96 寸 OLED 模块（`Output/atk_f103.hex` 已编好，可先烧了看现象）。

## 二、实验原理

### 2.1 显存与"影子显存"

OLED 每个像素自己发光，不需要背光；关键是**屏幕本身不"记得"你要显示什么**，它只有一块由控制器
（本模块是 SSD1306）管理的显存：128 列 × 64 行 = 8192 个像素位，**纵向 8 个像素打包成 1 字节**，共 1024 字节。
[Drivers/BSP/OLED/oled.c](Drivers/BSP/OLED/oled.c) 里准备了一份和硬件一一对应的"影子显存"：

```c
static uint8_t g_oled_gram[128][8];
```

- 第 1 维 `128` = 列（x，0~127）；第 2 维 `8` = 页（page，0~7），每页覆盖 8 行（y）
- 坐标 `(x, y)` → 字节 `g_oled_gram[x][y / 8]` 的第 `y % 8` 位

`oled_draw_point()` 就是照这个公式改位的（真实代码）：

```c
    if (x > 127 || y > 63) return;  /* 超出范围了. */

    pos = y / 8;        /* 计算GRAM里面的y坐标所在的字节, 每个字节可以存储8个行坐标 */
    bx = y % 8;         /* 取余数,方便计算y在对应字节里面的位置,及行(y)位置 */
    temp = 1 << bx;     /* 高位表示高行号, 得到y对应的bit位置,将该bit先置1 */

    if (dot)    /* 画实心点 */
    {
        g_oled_gram[x][pos] |= temp;
    }
    else        /* 画空点,即不显示 */
    {
        g_oled_gram[x][pos] &= ~temp;
    }
```

> 画点/写字符**只改内存**，屏幕此刻什么都没变；必须 `oled_refresh_gram()` 把整块显存搬进 SSD1306，画面才更新——这就是"刷屏"。

### 2.2 SSD1306 的页寻址（为什么一次搬 128 字节）

初始化时驱动选了**页地址模式**（`0x20` 后跟 `0x02`），写法是"先说写到哪一页、哪一列，之后连续喂数据，
列地址由硬件自动 +1"，于是刷新一屏是 8 个相同的小循环：

```c
void oled_refresh_gram(void)
{
    uint8_t i, n;

    for (i = 0; i < 8; i++)
    {
        oled_wr_byte (0xb0 + i, OLED_CMD); /* 设置页地址（0~7） */
        oled_wr_byte (0x00, OLED_CMD);     /* 设置显示位置—列低地址 */
        oled_wr_byte (0x10, OLED_CMD);     /* 设置显示位置—列高地址 */

        for (n = 0; n < 128; n++)
        {
            oled_wr_byte(g_oled_gram[n][i], OLED_DATA);
        }
    }
}
```

| 命令 | 作用 | 备注 |
| --- | --- | --- |
| `0xB0 \| page` | 选中第 page 页（0~7） | 代码写作 `0xb0 + i` |
| `0x00 \| 低4位` | 列地址低 4 位 | 从第 0 列开始，故写 `0x00` |
| `0x10 \| 高4位` | 列地址高 4 位 | 写 `0x10` = 高 4 位为 0 |
| 随后 128 个 `OLED_DATA` | 该页 128 列的数据 | 每字节 = 该列 8 行的亮灭 |

一次完整刷屏 = 8 × (3 命令 + 128 数据) = **1048 次 `oled_wr_byte()`**。字符也一样：
`oled_show_char()` 从 [oledfont.h](Drivers/BSP/OLED/oledfont.h) 的行段式点阵
（`oled_asc2_1206[95][12]` / `oled_asc2_1608[95][16]` / `oled_asc2_2412[95][36]`）取模值逐位写显存。

### 2.3 怎么和模块说话：8080 并口时序（软件模拟）

SSD1306 模块对外提供 IIC、4 线 SPI、8080 并口等多种接口，**用哪一种由模块的接口选择脚决定**；
本工程 [oled.h](Drivers/BSP/OLED/oled.h) 用 `OLED_MODE` 二选一，**默认是 8080 并口**：

```c
/* OLED模式设置
 * 0: 4线串行模式  （模块的BS1，BS2均接GND）
 * 1: 并行8080模式 （模块的BS1，BS2均接VCC）
 */
#define OLED_MODE       1   /* 默认使用8080并口模式 */
```

8080 并口 = 8 根数据线一起放一个字节 + `CS/RS/WR` 三根控制线握手。`RS` 就是"命令还是数据"的开关
（代码里 `OLED_CMD=0` / `OLED_DATA=1`），`CS` 低有效，`WR` 的**上升沿**让模块锁走数据：

```c
static void oled_wr_byte(uint8_t data, uint8_t cmd)
{
    oled_data_out(data);   /* 1. 8 位数据先摆到 PB0~PB7 上 */
    OLED_RS(cmd);          /* 2. RS 表明是命令还是数据 */
    OLED_CS(0);            /* 3. 拉低片选，开始一次写周期 */
    OLED_WR(0);            /* 4. WR 拉低，数据在总线上保持稳定 */
    OLED_WR(1);            /* 5. WR 拉高 -> 上升沿，模块在这一刻锁走数据 */
    OLED_CS(1);            /* 6. 释放片选 */
    OLED_RS(1);
}
```

数据线太多不逐个写宏，于是"拼凑"一次改 8 个引脚，同时保住 PB8~PB15：

```c
static void oled_data_out(uint8_t data)
{
    GPIOB->ODR = (GPIOB->ODR & 0XFF00) | (data & 0X00FF);
}
```

把 `OLED_MODE` 改成 0 就走逐位串行的 SPI 版本（`SCLK` 低电平摆 `SDIN`，`SCLK` 拉高让模块采样，MSB 先行）：

```c
    for (i = 0; i < 8; i++)
    {
        OLED_SCLK(0);

        if (data & 0x80)
        {
            OLED_SDIN(1);
        }
        else
        {
            OLED_SDIN(0);
        }

        OLED_SCLK(1);
        data <<= 1;
    }
```

**"软件模拟时序"的延时从哪来？** 本驱动没写 `delay_us()`：每次 `OLED_WR(0)` 都是一次真函数调用
（`sys_gpio_pin_set()` → 写 `GPIOx->BSRR`），几条指令加起来已有几十~上百 ns，大于 SSD1306 要求的最小脉宽，
所以**靠 CPU 执行时间天然满足时序**。反例是 DS18B20 那种微秒级单总线（实验 23），就必须显式插延时了。

### 2.4 寄存器层面做了哪些事

`oled_init()`（8080 分支）= 开时钟 → 配模式 → 给初始电平 → 灌 SSD1306 配置：

| 代码 | 动了哪个寄存器 | 作用 |
| --- | --- | --- |
| `OLED_SPI_CS_CLK_ENABLE()` | `RCC->APB2ENR \|= 1<<4` | 开 GPIOC 时钟（bit4 = IOPC） |
| `OLED_SPI_RS_CLK_ENABLE()` | `RCC->APB2ENR \|= 1<<4` | 开 GPIOC 时钟 |
| `OLED_WR_CLK_ENABLE()`、`OLED_RD_CLK_ENABLE()` | `RCC->APB2ENR \|= 1<<3` | **位号写错，见注意事项 3**（bit3 是 GPIOB） |
| `RCC->APB2ENR \|= 1 << 3;` | 同上 bit3 | 开 GPIOB 时钟（数据线 PB0~PB7） |
| `sys_gpio_remap_set(24, 3, 2)` | `AFIO->MAPR[26:24] = 2` | `SWJ_CFG=2`：关 JTAG 保 SWD，**释放 PB3、PB4 当普通 IO** |
| `sys_gpio_set(GPIOB, 0XFF, MODE_OUT, OTYPE_PP, SPEED_MID, PUPD_PU)` | `GPIOB->CRL` | PB0~PB7 通用推挽输出 10MHz |
| `sys_gpio_set(PC6/PC7/PC8/PC9, 同上)` | `GPIOC->CRL` | RD/WR/RS/CS 推挽输出 |
| `OLED_WR(1); OLED_RD(1);` | `GPIOC->BSRR` | 空闲态，避免误锁数据 |

`sys_gpio_set()` 是这套寄存器版工程的通用 GPIO 函数（[sys.h](Drivers/SYSTEM/sys/sys.h) 声明、
[sys.c](Drivers/SYSTEM/sys/sys.c) 实现），把每个引脚的 4 位配置算出来后落到 `CRL/CRH` 的对应 4 bit 槽：

```c
config  = ospeed & 0X03;            /* bit1:0 MODE，速度 2M/10M/50M */
config |= (otype & 0X01) << 2;      /* bit2 CNF[0]，推挽/开漏 */
config |= (mode - 1) << 3;          /* bit3 CNF[1]，普通/复用输出 */
...
p_gpiox->CRL |= config << (pinpos * 4);
```

本实验传 `MODE_OUT(1) + OTYPE_PP(0) + SPEED_MID(1)` → `config = 0x1`，即 `CNF=00, MODE=01` =
**10MHz 通用推挽输出**。为什么不选 50M？边沿越陡振铃越大，屏幕走线短、几十 ns 的时序 10M 足够，更稳。

> 顺手记住 `RCC->APB2ENR` 位图（全工程都在用）：bit0=AFIO、bit2=GPIOA、bit3=GPIOB、bit4=GPIOC、
> bit5=GPIOD、bit6=GPIOE、bit8=GPIOG、bit14=USART1。

### 2.5 时钟与参数计算

- `sys_stm32_clock_init(9)`：选 HSE（板载 8MHz）为 PLL 输入，`PLLMUL = 9` → `PLLCLK = 8 × 9 = 72MHz`，
  `HCLK = PCLK2 = 72MHz`、`PCLK1 = 36MHz`，FLASH 插 2 个等待周期并开预取。
  **该函数会把 `RCC->APB2ENR/APB1ENR` 清 0，所以必须是 `main()` 第一句。**
- `delay_init(72)`：SysTick 用 HCLK，`g_fac_us = 72`（1µs = 72 个计数），`delay_ms(500)` ≈ 36,000,000 计数。
- 显示节奏：主循环"改显存 → 刷屏 → `delay_ms(500)` → LED0 翻转"，即**每 500ms 换一个字符**；
  可见 ASCII 从 `' '`(32) 到 `'~'`(126) 共 95 个，一轮约 95 × 0.5s ≈ **47.5 秒**。

## 三、硬件资源与接线

| 资源 | 引脚 | 代码中的宏 / 位置 | 说明 |
| --- | --- | --- | --- |
| 并口数据 D0~D7 | PB0~PB7 | `oled_data_out()` 改 `GPIOB->ODR` 低 8 位 | 8080 模式数据总线 |
| OLED_CS | PC9 | `OLED_SPI_CS_PORT/PIN`，[oled.h](Drivers/BSP/OLED/oled.h) | 片选，低有效 |
| OLED_RS | PC8 | `OLED_SPI_RS_PORT/PIN` | 0=命令，1=数据 |
| OLED_WR | PC7 | `OLED_WR_PORT/PIN` | 写脉冲，上升沿锁存 |
| OLED_RD | PC6 | `OLED_RD_PORT/PIN` | 初始化时置高；本驱动不读模块 |
| OLED_RST | 与 MCU 复位脚共用 | `OLED_SPI_RST_*` 在代码里已注释 | 按板上复位键即复位模块 |
| SPI 模式 SCLK/SDIN | PB0 / PB1 | `OLED_SPI_SCLK_*`、`OLED_SPI_SDIN_*` | 仅 `OLED_MODE=0` 使用 |
| LED0（红灯） | **PA8** | `LED0_GPIO_PORT/PIN`，[led.h](Drivers/BSP/LED/led.h) | 低电平点亮，`LED0_TOGGLE()` 做心跳 |
| LED1（绿灯） | **PD2** | `LED1_GPIO_PORT/PIN` | 本实验未使用 |
| 串口1 | PA9=TX / PA10=RX | [usart.h](Drivers/SYSTEM/usart/usart.h) | `main()` 里初始化，OLED 本身不打印 |

**接线/插法**（官方 `readme.txt` + 代码引脚核对过）：

1. OLED 模块插开发板**左上角的 OLED 模块接口**——它和液晶屏（实验 11）共用同一排座，
   **OLED 靠左插、TFTLCD 靠右插**，插错位引脚就对不上。
2. 断电操作，先对丝印找到 **VCC 位置**（模块 VCC 必须对上排座 VCC），插好检查无误再上电（官方注意事项 2）。
3. 模块若有 BS/接口选择焊盘，按 `oled.h` 注释对齐：8080 并口 = BS1、BS2 均接 VCC；4 线串行 = 均接 GND。
4. 电脑侧一根 USB 线供电即可；下载用 ST-Link/J-Link（SWD）或串口 ISP。

**为什么要关 JTAG？** 数据线要用 PB3、PB4，而它们默认是 JTAG 的 JTDO/NJTRST。
`sys_gpio_remap_set(24, 3, 2)` 把 `AFIO->MAPR` 的 `SWJ_CFG[2:0]` 设成 2（关 JTAG、保留 SWD），
PA13/PA14 仍能下载调试，PB3/PB4 变普通 IO。省掉这句 → PB3/PB4 怎么配都不动 → 乱码或半屏不亮。

## 四、代码结构与调用流程

```
实验10 OLED实验/
├── User/main.c                 主程序：初始化 + 写显存 + 循环刷屏
├── Drivers/
│   ├── BSP/OLED/oled.c/.h      SSD1306 驱动：显存、写字节、页寻址刷新、字符/数字/字符串
│   ├── BSP/OLED/oledfont.h     ASCII 点阵字库（1206 / 1608 / 2412）
│   ├── BSP/LED/led.c/.h        LED0(PA8)/LED1(PD2)
│   ├── SYSTEM/sys/sys.c/.h     72MHz 时钟、GPIO 通用配置、NVIC、JTAG 重映射
│   ├── SYSTEM/delay/delay.c/.h SysTick 时钟摘取法延时
│   ├── SYSTEM/usart/usart.c/.h 串口1（115200），printf 重定向在这
│   └── CMSIS/                  stm32f103xe.h 寄存器定义 + startup_stm32f103xe.s
├── Projects/MDK-ARM/atk_f103.uvprojx   Keil 工程（Target 名：OLED）
├── Output/atk_f103.hex         已编译固件，可直接烧录
└── readme.txt                  官方说明（GBK 编码）
```

`main()` 流程：

```text
1 sys_stm32_clock_init(9)  → 72MHz（必须第一句）
2 delay_init(72)           → 计 72 下 = 1µs
3 usart_init(72, 115200)   → 串口1，为 printf / 后续实验打底
4 led_init()               → PA8/PD2 推挽输出，初始熄灭
5 oled_init()              → 配 GPIO + 灌 SSD1306 命令 + oled_clear()
6 oled_show_string(...)×5  → 只写影子显存
7 oled_refresh_gram()      → 首次搬到屏上，此刻肉眼看到文字
while(1):
  a oled_show_char(36,52,t,12,1)   画 ASCII 字符
  b oled_show_num(94,52,t,3,12)    画它的十进制码值
  c oled_refresh_gram()            刷屏
  d t++；超过 '~' 回到 ' '
  e delay_ms(500); LED0_TOGGLE()   心跳灯
```

## 五、核心代码解析

### 5.1 `User/main.c`（核心部分）

```c
    sys_stm32_clock_init(9);    /* 设置时钟, 72Mhz */
    delay_init(72);             /* 延时初始化 */
    usart_init(72, 115200);     /* 串口初始化为115200 */
    led_init();                 /* 初始化LED */
    oled_init();                /* 初始化OLED */

    oled_show_string(0, 0, "ALIENTEK", 24);
    oled_show_string(0, 24, "0.96' OLED TEST", 16);
    oled_show_string(0, 40, "ATOM 2020/5/30", 12);
    oled_show_string(0, 52, "ASCII:", 12);
    oled_show_string(64, 52, "CODE:", 12);

    oled_refresh_gram();        /* 更新显示到OLED */

    t = ' ';

    while (1)
    {
        oled_show_char(36, 52, t, 12, 1);   /* 显示ASCII字符 */
        oled_show_num(94, 52, t, 3, 12);    /* 显示ASCII字符的码值 */
        oled_refresh_gram();                /* 更新显示到OLED */
        t++;

        if (t > '~') t = ' ';

        delay_ms(500);
        LED0_TOGGLE();  /* LED0闪烁 */
    }
```

- `x` 是列（0~127），`y` 是行（0~63），`size` 是字号（12/16/24），ASCII 字符宽度 = `size/2`。
  `"ASCII:"` 6 个 12 号字符占 6×6 = 36 像素，所以紧接着的字符从 x=36 开始；`y=52 + 12 = 64` 正好贴下边缘。
- `t` 是 `uint8_t`，`' '`=32、`'~'`=126，正好落在字库 `oled_asc2_1206[95][12]` 的 95 项内。
  `oled_show_char()` 第一件事是 `chr = chr - ' '`，即**字库下标 = ASCII − 32**（字库从空格开始存）。
- `mode=1` 正常显示，传 0 就变**反白**（`oled_show_char()` 里点阵位取 `mode` / `!mode`）。
- `OLED_WR/RD` 空闲为高；`oled_clear()` 只在初始化和越界换屏时被调到。

### 5.2 SSD1306 初始化命令分组（`oled_init()` 尾部）

| 命令对 | 代码里的注释 | 通俗理解 |
| --- | --- | --- |
| `0xAE` | 关闭显示 | 先关灯再配置，避免闪花屏 |
| `0xD5`,`0x80` | 设置时钟分频因子,震荡频率 | 模块自身驱动时钟 |
| `0xA8`,`0X3F` | 设置驱动路数 | 1/64，即 64 行 |
| `0xD3`,`0X00` / `0x40` | 显示偏移 / 显示开始行 | 不偏移、从第 0 行开始 |
| `0x8D`,`0x14` | 电荷泵设置 | **OLED 必须开泵才亮**（`oled_display_on()` 也是这两句 + `0XAF`） |
| `0x20`,`0x02` | 设置内存地址模式 | 页地址模式，对应 2.2 节写法 |
| `0xA1` / `0xC8` | 段重定义 / COM 扫描方向 | 决定画面是否上下左右颠倒 |
| `0xDA`,`0x12` | COM 硬件引脚配置 | 与面板接线有关 |
| `0x81`,`0xEF` | 对比度设置 | 1~255，越大越亮（默认 0X7F） |
| `0xD9`,`0xf1` / `0xDB`,`0x30` | 预充电周期 / VCOMH 电压 | 影响亮度与寿命 |
| `0xA4` / `0xA6` | 全局显示开启 / 正常显示 | `0xA6` 改 `0xA7` 就全屏反相 |
| `0xAF` | 开启显示 | 最后一步点亮 |

结尾 `oled_clear()` 把 1024 字节显存清 0 并刷屏，保证上电是干净黑屏。

### 5.3 `oled_show_string()` 的自动换行与越界

```c
while ((*p <= '~') && (*p >= ' '))  /* 判断是不是非法字符! */
{
    if (x > (128 - (size / 2)))     /* 宽度越界 */
    {
        x = 0;
        y += size;          /* 换行 */
    }

    if (y > (64 - size))    /* 高度越界 */
    {
        y = x = 0;
        oled_clear();
    }

    oled_show_char(x, y, *p, size, 1);   /* 显示一个字符 */
    x += size / 2;
    p++;
}
```

- 只接受 32~126 的可打印 ASCII，**中文（多字节）会被直接跳过**——这就是实验 30 要另做点阵汉字库的原因。
- 高度越界时选择"整屏清掉从头写"，调试时方便，做界面时会闪一下，要注意。

## 六、编译、下载与运行

1. `File → Open Project` → `实验10 OLED实验/Projects/MDK-ARM/atk_f103.uvprojx`（Target 名 `OLED`）。
2. 工程关键设置（仓库已配好，别改）：Device `STM32F103RC`、Pack `Keil.STM32F1xx_DFP`（2.3.0）、
   Define **`STM32F103xE`**（缺了就选不到寄存器定义）；Include Paths 为
   `..\..\Drivers\CMSIS\Device\ST\STM32F1xx\Include`、`..\..\Drivers\CMSIS\Include`、`..\..\Drivers`、`..\..\User`
   （注意给的是 `..\..\Drivers` 这一层，所以源码里写 `#include "./BSP/OLED/oled.h"`）；启动文件 `startup_stm32f103xe.s`。
3. `F7` Rebuild，产物在 `Output/`（含 `atk_f103.hex`）。
4. `F8` 用 ST-Link/J-Link 走 SWD（PA13/PA14）下载；无调试器则 BOOT0=1、复位后用串口 ISP 烧 `Output/atk_f103.hex`，烧完 BOOT0 改回 0。
5. 按复位键看现象。本例程不主动打印，想看串口就把 PA9/PA10 用跳线帽连到 USART1 排针的 RXD/TXD，115200-8-N-1。

## 七、实验现象

1. 屏左上显示 `ALIENTEK`（24 号），下面依次 `0.96' OLED TEST`、`ATOM 2020/5/30`。
2. 屏底固定 `ASCII:` 与 `CODE:` 两个标签；`ASCII:` 后每 0.5 秒换一个可见字符，
   `CODE:` 后同步显示它的**十进制码值**（3 位，`oled_show_num()` 把高位的 0 显示成空格，
   所以 `A` 显示 `" 65"`、`0` 显示 `" 48"`、空格显示 `" 32"`）。
3. 字符走到 `~`(126) 后回到空格(32)，一轮约 47 秒。
4. LED0（PA8 红灯）每 500ms 亮灭一次，说明主循环没卡死。
5. 按复位后 `oled_init()` 会先 `oled_clear()`，屏黑一下再出字。

## 八、注意事项

1. **官方 `readme.txt` 的 LED 引脚与代码不一致，以代码为准**：`readme.txt` 写 "LED0-PB5、LED1-PE5"，
   而 [led.h](Drivers/BSP/LED/led.h) 实际是 **LED0=PA8、LED1=PD2**（与实验 11/12 相同，也是本板的接法）。
   PB5/PE5 是另一种板子的丝印，照它接会找不到灯。
2. **本例程不是 IIC 驱动**：`oled.h` 里 `OLED_MODE = 1`，走 **8080 并口**，`#else` 分支是 **4 线 SPI**，
   整个 BSP 没有任何 IIC 代码（GPIO 软件模拟 IIC 在实验 19）。仓库根 README 里"软件模拟 IIC 驱动 SSD1306"
   那句概括与本实验代码不符，别按它去找 `SCL/SDA` 宏。
3. **`oled.h` 里 WR/RD 的时钟宏位号写错**：`OLED_WR_CLK_ENABLE()`、`OLED_RD_CLK_ENABLE()` 都是
   `RCC->APB2ENR |= 1 << 3` 却注释成"PC口时钟使能"，而 **bit3 = GPIOB、GPIOC = bit4**。
   功能上没出问题，是因为前面的 `OLED_SPI_CS/RS_CLK_ENABLE()` 已用 `1<<4` 打开了 GPIOC。
   自己挪动 CS/RS 引脚时要留意这个坑。
4. **`OLED_MODE` 改成 0 会编译不过**：`oled_init()` 的 SPI 分支用了 `OLED_SPI_RST_CLK_ENABLE()`、
   `OLED_SPI_RST_PORT/PIN`，而它们在 `oled.h` 里是被 `//` 注释掉的（RST 与 MCU 复位脚共用）。
   切 SPI 前先取消这几行注释并填好实际引脚，同时模块端 BS1/BS2 也要改成接 GND。
5. **插反/带电插拔会烧模块**：VCC/GND 对错是新手最常见的硬件事故，务必断电操作。
6. 屏完全不亮的排查顺序：`oled_init()` 是否被调用 → PB3/PB4 是否释放（JTAG 有没有关）→
   是否写了显存并调用 `oled_refresh_gram()` → 模块 BS 设置与 `OLED_MODE` 是否匹配 → VCC 是否 3.3V。
7. 显示"上下颠倒/左右镜像"不是 bug，是 `0xA1`（段重映射）与 `0xC8`（COM 扫描方向）和面板的搭配问题，
   换批次模块时可试改这两条命令。
8. **刷屏有代价**：一次 `oled_refresh_gram()` 要发 1048 字节，每字节又是若干次 GPIO 写 + 函数调用，
   全在主循环同步做。动画卡顿就考虑只刷变化区域，或改用实验 11 那种"CS/RS 提到循环外"的写法。
9. 源码是 GBK 编码，用 UTF-8 编辑器打开中文注释会乱码；**别整文件另存为 UTF-8**，否则注释全废。

## 九、总结与延伸

**要点**

1. 显存模型：1 字节 = 8 个纵向像素，`(x,y)` → `gram[x][y/8]` 的第 `y%8` 位；先改内存再刷屏。
2. 页寻址三步：`0xB0|页` + 列低地址 + 列高地址，然后连喂 128 个数据字节。
3. 命令/数据靠一根 `RS` 区分；并口写一字节 = 数据上线 → CS 有效 → WR 低 → WR 上升沿锁存 → CS 释放。
4. GPIO 模拟总线时**指令耗时本身就是延时**；同时注意 `RCC->APB2ENR` 位号与 `AFIO->MAPR` 的 JTAG 释放。
5. 寄存器版配一个脚 = 开时钟 + 算 `CNF/MODE` 填 `CRL/CRH` + 用 `BSRR` 改电平（`ODR ^= pin` 做心跳）。

**延伸小练习**

1. 用 `oled_fill(x1,y1,x2,y2,1)` 画几个矩形块做"进度条"，再用 `dot=0` 逐段擦掉，体会局部改显存与整屏刷新的区别。
2. 把 `oled_show_char(..., mode)` 改成 0 看反白；再把 `if (t > '~') t = ' ';` 的上界改成 `'z'` 之类，
   观察超出字库范围时的乱码（数组越界，读到脏数据）。
3. 把 `OLED_MODE` 试成 SPI 分支（先补 `oled.h` 的 RST 宏），对比两种模式的引脚数量与刷屏耗时。

**衔接下一课**：实验 11 TFTLCD 把"1 位/像素的黑白屏"升级成"16 位/像素的彩色屏"，
控制线仍是同一组 `CS/RS/WR/RD`（引脚还都在 PC6~PC10、PB0~PB15 上），但多了"读 ID + 多种驱动 IC 适配"；
实验 12 USMART 则能让你不重编代码，用串口命令直接调本实验的 `oled_show_string()` 试参数。
