# 实验21 触摸屏实验

> 一句话概括：让 LCD 能"摸"——用软件模拟 SPI 驱动电阻屏 XPT2046 采集坐标并做五点校准，用 IIC 驱动电容屏 GT9xxx/FT5206 直接读像素坐标，程序根据 LCD 型号自动识别屏类型后进入手写绘图。

## 一、实验简介

- **实验目的**（官方 `readme.txt`）：学习电阻式 / 电容式触摸屏的使用。
  通俗展开：触摸屏就是一块"能感应手指位置"的板子，后面接一颗控制芯片把位置读出来。电阻屏靠"两层导电膜被按到一起"测电压、算出坐标，需要**校准**；电容屏靠"手指改变电容阵列"直接给出像素坐标，**免校准**。本实验把两种屏都跑通，并教你一个通用套路：先问 LCD 是谁，再决定用哪种触摸方式。
- **在学习路线中的位置**：
  - 前置：GPIO 输出/输入、实验20 SPI 实验（理解 SPI 时序）、IIC（软件 IIC）实验、LCD 显示实验、24CXX（EEPROM）实验。
  - 后续：综合测试实验、GUI（如 Touch 系列界面）等都会复用这里的触摸读取接口 `tp_dev`。
- **开发板与芯片**：正点原子 MiniSTM32 V4，主芯片 STM32F103RC，72MHz；配 ALIENTEK 电阻/电容触摸屏（接 MCU 8080 并口屏模块）。

## 二、实验原理

### 2.1 电阻屏 XPT2046：软件 SPI + 压力检测

电阻屏表面是一层可形变的导电膜，下面还有一层导电膜，平时不接触。按下时两层在触点处接通，控制芯片 **XPT2046（兼容 ADS7843/7846/TSC2046）** 通过对 X、Y 电阻分压网络做 **AD 转换**，量出电压，从而得到 0~4095 的"物理坐标（AD 值）"。它还有一根 **PENIRQ(T_PEN)** 线：一旦检测到有按压就拉低，用来告诉 STM32"现在有人摸了"。

本实验**不用硬件 SPI**，而是用 GPIO **软件模拟 SPI 时序**（自己拉时钟/数据线）去驱动 XPT2046。读写时序见 [touch.c](Drivers/BSP/TOUCH/touch.c)：

```c
static void tp_write_byte(uint8_t data) {        /* MOSI 在时钟上升沿被读走 */
    for (count = 0; count < 8; count++) {
        if (data & 0x80) T_MOSI(1); else T_MOSI(0);
        data <<= 1;
        T_CLK(0); delay_us(1);
        T_CLK(1);                                /* 上升沿有效 */
    }
}
static uint16_t tp_read_ad(uint8_t cmd) {        /* 发命令→等转换→读16位→取高12位 */
    T_CLK(0); T_MOSI(0); T_CS(0);                /* 拉低片选，选中 XPT2046 */
    tp_write_byte(cmd);                          /* 发送控制字节 */
    delay_us(6);                                 /* ADS7846 转换最长 6µs */
    ... 给一个时钟清除 BUSY ...
    for (count = 0; count < 16; count++) {       /* 读 16 位，只有高 12 位有效 */
        num <<= 1; T_CLK(0); delay_us(1); T_CLK(1);
        if (T_MISO) num++;
    }
    num >>= 4; T_CS(1);                          /* 右移 4 位得 12 位 AD 值 */
    return num;
}
```

控制字节（命令字）：`0XD0` 读 X 轴、`0X90` 读 Y 轴（竖屏约定；横屏时 XY 对调，见 `tp_read_xy`）。AD 值是 12 位，范围 0~4095。

**压力/有无触摸**：读 `T_PEN` 引脚即可——

```c
if (T_PEN == 0) { /* 有按压，读坐标 */ } else { /* 松开 */ }
```

### 2.2 电阻屏五点校准：`xfac / yfac / xc / yc`

XPT2046 给出的是 0~4095 的 AD 值，跟 LCD 的真实像素坐标（0~宽/高）不是一回事，而且不同屏、不同方向的映射都不同，必须**校准**建立线性关系。本工程用经典**五点校准法**（见 `tp_adjust()`），物理/逻辑定义如下：

- 物理坐标：AD 采集值，范围 0~4095；
- 逻辑坐标：LCD 屏幕像素坐标，范围 0~`lcddev.width/height`。

程序在屏幕 4 个角附近 `(20,20)`、`(W-20,20)`、`(20,H-20)`、`(W-20,H-20)` 和正中心 `(W/2,H/2)` 依次画十字，让你点 5 次，得到 5 组物理 AD 坐标 `pxy[5][2]`，再算 4 个参数：

| 参数 | 含义 | 计算式（代码原文） |
| --- | --- | --- |
| s1/s3 | 上/下两条边的 X 轴 AD 跨度 | `s1=pxy[1][0]-pxy[0][0]`，`s3=pxy[3][0]-pxy[2][0]` |
| s2/s4 | 右/左两条边的 Y 轴 AD 跨度 | `s2=pxy[3][1]-pxy[1][1]`，`s4=pxy[2][1]-pxy[0][1]` |
| `xfac` | X 方向每像素对应多少 AD（比例因子） | `xfac = (s1+s3) / (2*(W-40))` |
| `yfac` | Y 方向每像素对应多少 AD | `yfac = (s2+s4) / (2*(H-40))` |
| `xc / yc` | 屏幕中心对应的物理 AD 值（基准点） | `xc = pxy[4][0]`，`yc = pxy[4][1]` |

> 代码里这两根"AD 方向比例因子" `xfac/yfac` 和中心 AD 值 `xc/yc`，就是你常说的 `tp_ad_x/tp_ad_y` 一类的校准量（不同资料命名略有差异，本工程的字段名是 `xfac/yfac/xc/yc`）。

校验：先算 `px = s1/s3`、`py = s2/s4`，两者都应接近 1（代码要求落在 0.95~1.05，且各跨度非 0、不超 4095），否则认为点偏了，**自动重来**。通过后：

```c
/* 物理 AD → 屏幕逻辑坐标（见 tp_scan 模式 0）*/
tp_dev.x[0] = (signed short)(tp_dev.x[0] - tp_dev.xc) / tp_dev.xfac + lcddev.width  / 2;
tp_dev.y[0] = (signed short)(tp_dev.y[0] - tp_dev.yc) / tp_dev.yfac + lcddev.height / 2;
```

**为什么要先减中心 AD 再除以比例、再加半屏？** 因为把中心点当成坐标原点最稳：(当前AD - 中心AD) 得到相对中心的 AD 偏移，除以"每像素多少 AD"换算成像素偏移，再加上"中心像素位置 W/2、H/2"，就还原到全屏坐标。

校准得到的 4 个参数会存进 **24C02 EEPROM**（`TP_SAVE_ADDR_BASE = 40` 起，写 12 字节参数 + 1 字节标记 `0X0A`），下次开机 `tp_get_adjust_data()` 读到标记 `0X0A` 就知道"已校准过"，直接用、不再让你点 5 次。

### 2.3 电阻屏读数滤波

单次 AD 会抖，代码做两级滤波：

1. `tp_read_xoy()`：连续读 `TP_READ_TIMES(5)` 次，冒泡升序排列，去掉最低 1 个、最高 1 个（`TP_LOST_VAL=1`），取中间 3 个平均。
2. `tp_read_xy2()`：连读两整组 (X,Y)，要求两次 X 差、Y 差都在 `TP_ERR_RANGE(50)` 以内才采信，再取两次平均。

这样能有效剔除毛刺，坐标稳定得多。

### 2.4 电容屏 GT9xxx / FT5206：IIC + 中断线

电容屏芯片（**GT9147/GT1151 等 GT 系列**、**FT5206/FT5426/CST340 等 FT 系列**）内部已经把触摸点算成了**像素坐标**，STM32 只需用 **IIC（I²C 两线总线）** 去读它的寄存器即可，**不需要校准**。

- 走 IIC：GT 用 16 位寄存器地址（如 `GT9XXX_GSTID_REG = 0X814E` 状态寄存器、`GT9XXX_TP1_REG = 0X8150` 第 1 点坐标），FT 用 8 位地址（`FT5206_REG_NUM_FINGER = 0x02`、`FT5206_TP1_REG = 0X03`）。IIC 用专门的软件总线 `ctiic.c`（PC0=SCL、PC3=SDA）。
- 中断线 INT（PC1）：有触摸时芯片会拉 INT 通知主机；本驱动采用**查询方式**读坐标（见 `gt9xxx_scan` 注释），INT/RST 两脚在 `gt9xxx_init()` 里仍被配置，RST(PC13) 用于复位芯片。
- 读一帧：先看状态寄存器（bit7=数据就绪、低 4 位=触摸点数），清标志后，从各点寄存器读 4 字节 `(XL,XH,YL,YH)` 拼成坐标，按屏型号/横竖屏做翻转。

> 注意：本工程里"FT 系列"电容屏的驱动文件与宏命名是 **FT5206**（兼容 FT5426/CST340），有些资料把它叫 FT6206/FT6236，指同一类 FT 芯片；本工程实际调用的是 `ft5206_init()/ft5206_scan()`。

### 2.5 程序如何自动识别屏类型

识别入口在 `tp_init()`，靠的是 **LCD 的 ID**（`lcddev.id`，由 `lcd_init()` 读回）。逻辑用大白话列一遍：

1. 先设 `touchtype = 竖屏/横屏`（bit0 = `lcddev.dir & 1`）。
2. `lcddev.id == 0x7796`（3.5 寸 ST7796，有两种）：先试 `gt9xxx_init()`，成功→是电容屏（GT），失败→落到电阻屏分支。
3. `id ∈ {0x5510, 0x9806, 0x4342, 0x4384, 0x1018}`（4.3/7/10.1 寸等）：**GT 系列电容屏**，`scan = gt9xxx_scan`，`touchtype |= 0X80`。
4. `id ∈ {0x1963, 0x7084, 0x7016}`（7 寸 MCU/RGB 屏）：先试 `ft5206_init()`，成功用 **FT 系列** `ft5206_scan`；失败改用 **GT 系列**。置 `touchtype |= 0X80`。
5. 其它 ID：**电阻屏**——初始化 PC0~PC3/PC13 软件 SPI 脚、初始化 24CXX、读取校准参数；没校准过就跑 `tp_adjust()` 校准并保存。

`main()` 再根据 `tp_dev.touchtype & 0X80` 决定进 `ctp_test()`（电容）还是 `rtp_test()`（电阻）。

## 三、硬件资源与接线

> 关键点：**电阻屏和电容屏复用同一组 PC 引脚**，但同一时刻只会按识别结果初始化其中一种，互不冲突（对应关系见下）。

电阻屏（软件 SPI + 压力）引脚（[touch.h](Drivers/BSP/TOUCH/touch.h)）：

| 信号 | 引脚 | 宏 | 方向 |
| --- | --- | --- | --- |
| T_PEN(PENIRQ) | PC1 | `T_PEN` | 上拉输入，有按压拉低 |
| T_MISO | PC2 | `T_MISO` | 上拉输入，读 AD |
| T_CLK | PC0 | `T_CLK` | 推挽输出，软件时钟 |
| T_MOSI | PC3 | `T_MOSI` | 推挽输出，写命令 |
| T_CS | PC13 | `T_CS` | 推挽输出，片选 |

电容屏引脚（[gt9xxx.h](Drivers/BSP/TOUCH/gt9xxx.h) / [ft5206.h](Drivers/BSP/TOUCH/ft5206.h) / [ctiic.h](Drivers/BSP/TOUCH/ctiic.h)）：

| 信号 | 引脚 | 宏 | 说明 |
| --- | --- | --- | --- |
| INT | PC1 | `GT9XXX_INT` / `FT5206_INT` | 触摸通知（与 T_PEN 同脚复用） |
| RST | PC13 | `GT9XXX_RST` / `FT5206_RST` | 复位（与 T_CS 同脚复用） |
| CT_IIC_SCL | PC0 | `CT_IIC_SCL` | 电容屏 IIC 时钟（与 T_CLK 同脚复用） |
| CT_IIC_SDA | PC3 | `CT_IIC_SDA` | 电容屏 IIC 数据（与 T_MOSI 同脚复用） |

其它资源：

| 资源 | 引脚 | 代码位置 | 说明 |
| --- | --- | --- | --- |
| 24C02 SCL/SDA | PC12/PC11 | [myiic.h](Drivers/BSP/IIC/myiic.h)，[24cxx.h](Drivers/BSP/24CXX/24cxx.h) | 保存电阻屏校准参数（独立于电容屏 IIC） |
| KEY0 | PC5 | `KEY0_PRES`，[key.h](Drivers/BSP/KEY/key.h) | 电阻屏强制校准 |
| LED0（红灯） | PA8 | `LED0`，[led.h](Drivers/BSP/LED/led.h) | 运行指示 |
| 串口1 | PA9/PA10 | [usart.h](Drivers/SYSTEM/usart/usart.h) | 115200 |
| TFTLCD | 8080 并口 | [lcd.h](Drivers/BSP/LCD/lcd.h) | 仅 MCU 屏 |

- 触摸屏随 LCD 模块一起插在屏排座上（FPC 排线），无需单独飞线；24C02 与 SPI FLASH 是板载器件。
- **时钟**：`sys_stm32_clock_init(9)` = 72MHz；`delay_us()` 精度直接决定软件 SPI/IIC 时序，故依赖此主频。

## 四、代码结构与调用流程

```
实验21 触摸屏实验/
├── User/main.c                    开机初始化→识别屏类型→进入电阻/电容测试
├── Drivers/BSP/TOUCH/
│   ├── touch.[ch]                 电阻屏：软件SPI、AD滤波、五点校准、读坐标
│   ├── gt9xxx.[ch]                GT 系列电容屏（IIC 读写寄存器+扫描）
│   ├── ft5206.[ch]                FT 系列电容屏
│   └── ctiic.[ch]                 电容屏专用软件 IIC（PC0/PC3）
├── Drivers/BSP/IIC/myiic.[ch]      通用软件 IIC（PC11/PC12，给 24CXX 用）
├── Drivers/BSP/24CXX/24cxx.[ch]     保存/读取校准参数
├── Drivers/BSP/{KEY,LED,LCD}      按键/LED/LCD
├── Drivers/SYSTEM/{sys,delay,usart}
└── Middlewares/USMART/
```

`main()` 流程：

1. `sys_stm32_clock_init(9)`、`delay_init`、`usart_init`、`led_init`、`lcd_init`、`key_init`；
2. `tp_dev.init()`（即 `tp_init()`）：**按 LCD ID 自动识别**电阻/电容屏，必要时读/做校准；
3. LCD 打印 "STM32 / TOUCH TEST"，电阻屏额外提示按 KEY0 校准；
4. `load_draw_dialog()`：清屏并在右上角画 "RST" 清除区；
5. `touchtype & 0X80` 为真 → `ctp_test()`（多点画线）；否则 → `rtp_test()`（单点画点 + KEY0 强制校准）。

## 五、核心代码解析

### 5.1 电阻屏扫描 `tp_scan()`（坐标换算的落点）

```c
uint8_t tp_scan(uint8_t mode) {
    if (T_PEN == 0) {                                   /* 有按压 */
        if (mode) tp_read_xy2(&tp_dev.x[0], &tp_dev.y[0]);        /* 物理坐标(校准时用) */
        else if (tp_read_xy2(&tp_dev.x[0], &tp_dev.y[0])) {        /* 屏幕坐标 */
            tp_dev.x[0] = (signed short)(tp_dev.x[0] - tp_dev.xc) / tp_dev.xfac + lcddev.width  / 2;
            tp_dev.y[0] = (signed short)(tp_dev.y[0] - tp_dev.yc) / tp_dev.yfac + lcddev.height / 2;
        }
        if ((tp_dev.sta & TP_PRES_DOWN) == 0) {         /* 首次按下：记起点坐标 */
            tp_dev.sta = TP_PRES_DOWN | TP_CATH_PRES;
            tp_dev.x[CT_MAX_TOUCH-1] = tp_dev.x[0];
            tp_dev.y[CT_MAX_TOUCH-1] = tp_dev.y[0];
        }
    } else {                                            /* 松开 */
        if (tp_dev.sta & TP_PRES_DOWN) tp_dev.sta &= ~TP_PRES_DOWN;
        else { tp_dev.x[0] = 0xffff; tp_dev.y[0] = 0xffff; }
    }
    return tp_dev.sta & TP_PRES_DOWN;
}
```

`mode=0` 输出屏幕像素坐标（绘图用），`mode=1` 输出原始 AD 坐标（校准用）。`sta` 的 bit15 表示按下/松开，供上层判断。

### 5.2 校准核心判定（`tp_adjust()` 第五点后）

```c
s1 = pxy[1][0]-pxy[0][0];  s3 = pxy[3][0]-pxy[2][0];   /* X 上下边跨度 */
s2 = pxy[3][1]-pxy[1][1];  s4 = pxy[2][1]-pxy[0][1];   /* Y 左右边跨度 */
px = (double)s1 / s3;      py = (double)s2 / s4;       /* 比例，应≈1 */

if (px<0.95||px>1.05||py<0.95||py>1.05 || ... ) { cnt=0; /* 重来 */ continue; }

tp_dev.xfac = (float)(s1+s3) / (2*(lcddev.width  - 40));
tp_dev.yfac = (float)(s2+s4) / (2*(lcddev.height - 40));
tp_dev.xc = pxy[4][0];     tp_dev.yc = pxy[4][1];      /* 中心 AD */
lcd_show_string(...,"Touch Screen Adjust OK!",...);
tp_save_adjust_data();                                  /* 存 24C02 */
```

用两对边 AD 跨度之比 `px/py` 检验线性度，越接近 1 说明点得越准；通过后取平均得到 xfac/yfac，中心 AD 作为原点 xc/yc。

### 5.3 校准参数的存/取（写的是结构体内存）

```c
void tp_save_adjust_data(void) {
    uint8_t *p = (uint8_t *)&tp_dev.xfac;            /* 指向 4 个参数首地址 */
    at24cxx_write(TP_SAVE_ADDR_BASE, p, 12);         /* 一次存 12 字节 */
    at24cxx_write_one_byte(TP_SAVE_ADDR_BASE + 12, 0X0A); /* 已校准标记 */
}
uint8_t tp_get_adjust_data(void) {
    uint8_t *p = (uint8_t *)&tp_dev.xfac;
    at24cxx_read(TP_SAVE_ADDR_BASE, p, 12);
    temp = at24cxx_read_one_byte(TP_SAVE_ADDR_BASE + 12);
    return (temp == 0X0A) ? 1 : 0;
}
```

巧妙之处：不逐字段序列化，而是直接按内存把 `xfac/yfac/xc/yc`（2 个 float + 2 个 short = 12 字节）整块存到 EEPROM，读回时再整块写回同一地址，自动还原。

### 5.4 电容屏扫描 `gt9xxx_scan()`（读多点坐标）

```c
gt9xxx_rd_reg(GT9XXX_GSTID_REG, &mode, 1);           /* 读 0x814E 状态 */
if ((mode & 0x80) && ((mode & 0XF) <= g_gt_tnum)) {   /* 数据就绪且点数合法 */
    i = 0; gt9xxx_wr_reg(GT9XXX_GSTID_REG, &i, 1);    /* 清 buffer-ready 标志 */
}
if ((mode & 0XF) && ((mode & 0XF) <= g_gt_tnum)) {    /* 有点按下 */
    for (i = 0; i < g_gt_tnum; i++)
        if (tp_dev.sta & (1<<i))
            gt9xxx_rd_reg(GT9XXX_TPX_TBL[i], buf, 4); /* 读该点 XL,XH,YL,YH */
    /* 再根据 lcddev.id 与横竖屏把 buf 拼成 tp_dev.x[i]/y[i] */
}
```

`g_gt_tnum` 默认 5（GT9271 为 10），逐点寄存器地址放在表 `GT9XXX_TPX_TBL[]` 里。电容屏给的就是像素坐标，所以**无需 xfac 换算**。

### 5.5 自动识别入口 `tp_init()`（节选）

```c
tp_dev.touchtype = 0;
tp_dev.touchtype |= lcddev.dir & 0X01;               /* 横/竖屏位 */
if (lcddev.id == 0x7796) {                           /* 3.5寸需试 GT */
    if (gt9xxx_init() == 0) { tp_dev.scan = gt9xxx_scan; tp_dev.touchtype |= 0X80; return 0; }
}
if (lcddev.id==0X5510||...||lcddev.id==0X1018) { gt9xxx_init(); tp_dev.scan=gt9xxx_scan; tp_dev.touchtype|=0X80; return 0; }
else if (lcddev.id==0X1963||0X7084||0X7016) { if(!ft5206_init()) tp_dev.scan=ft5206_scan; else gt9xxx_init(); tp_dev.touchtype|=0X80; return 0; }
else { /* 电阻屏：初始化软件SPI脚 + 24CXX + 读/做校准 */ }
```

## 六、编译、下载与运行

1. Keil MDK 打开 [Projects/MDK-ARM/atk_f103.uvprojx](Projects/MDK-ARM/atk_f103.uvprojx)。
2. 器件 STM32F103RC，启动文件 `startup_stm32f103xe.s`，包含路径含 `BSP/TOUCH`、`BSP/IIC`、`BSP/24CXX`、`SYSTEM` 等。
3. Build 后 ST-Link/J-Link 下载。
4. 串口 XCOM/SSCOM **波特率 115200**（主要靠屏幕观察，串口备用）。

## 七、实验现象

1. 开机 LCD 显示 "STM32 / TOUCH TEST / ATOM@ALIENTEK"，随后清屏、右上角出现 "RST"。
2. **电阻屏**：
   - 若 24C02 里没有校准标记，会自动进入五点校准：依次用笔尖点 5 个十字中心，成功后显示 "Touch Screen Adjust OK!"，保存参数；
   - 之后用手指/笔在屏上滑动即可**画红色点**；点右上角 "RST" 区域清屏；按 **KEY0** 可**强制重新校准**。
3. **电容屏**：直接进入手写，滑动按各点颜色**连续画线**（最多 5 点，部分屏/10.1 寸可 10 点），点 "RST" 清屏，无需校准。
4. LED0 周期性闪烁表示程序运行。

> 官方电阻屏校准步骤（readme.txt）：下载代码→按 KEY0→依次点 5 个十字最中央（最好用笔尖）→直到出现 "Touch Screen Adjust OK!"→校准完成。

## 八、注意事项

- **只支持 MCU 8080 并口屏**，RGB 屏不适用（本实验依赖 lcddev.id 识别）。
- **PC 引脚复用**：T_PEN/INT 同为 PC1，T_CS/RST 同为 PC13，T_CLK/SCL 同为 PC0，T_MOSI/SDA 同为 PC3——这是设计好的"分时复用"，同一块屏只会被识别成一种并初始化对应一组功能；**不要把电阻屏和电容屏同时接**。
- **校准要一次点准**：五点中若某点点偏导致 `px/py` 不在 0.95~1.05，程序会**自动重来**；建议用圆头笔尖、垂直按压，减少视差。
- **24C02 保存参数别掉电**：校准后写 EEPROM 期间断电可能丢失标记，导致下次又重新校准。
- **坐标不更新 / 全 0xffff**：电阻屏没按下的时候 `x[0]/y[0]` 被置 0xffff 属正常；若一直无反应，检查屏排线是否插到位、`lcddev.id` 是否被正确读回（识别分支才不会走错）。
- **代码与注释/说明不一致，以代码为准**：
  - `main.c` 用 `if (tp_dev.touchtype != 0XFF)` 决定是否显示 "Press KEY0 to Adjust"，注释写"电阻屏才显示"，但 `touchtype` 取值（电阻 0/1、电容 0x80/0x81）永远不会等于 0xFF，故**两种屏都会显示这句提示**——按代码事实理解即可。
  - `touch.h` 宏 `CT_MAX_TOUCH` 为 10，同行注释却写"固定为5点"；实际点数按芯片而定（GT 默认 5，GT9271/10.1 寸为 10）。
  - `gt9xxx.c` 注释把 `GT9XXX_CMD_WR = 0X28` 称作"写命令"，它是 7 位地址 0x14 左移 1 位后的 IIC 写字节（配合 INT 电平决定从机地址），非标准 0x5D。
  - 电容屏驱动文件命名为 `ft5206`（兼容 FT5426/CST340），与某些资料里的 "FT6206/FT6236" 指同一类 FT 芯片。
- **误触清屏区**：绘图时点到右上角 RST 会清屏，属正常交互。

## 九、总结与延伸

**本实验掌握的要点：**
1. 电阻屏 XPT2046 用软件 SPI 读 12 位 AD（0xD0/0x90），靠 T_PEN 判断有无按压。
2. 五点校准法与 `xfac/yfac/xc/yc` 四个参数的推导及"AD→像素"换算公式。
3. 两级去抖动滤波（去极值取平均 + 两次读数一致性）。
4. 电容屏 GT9xxx/FT5206 用 IIC 读寄存器直接得到像素坐标，免校准。
5. 程序用 `lcddev.id` + `touchtype` 标志位实现电阻/电容**自动识别**与统一 `tp_dev` 接口。

**延伸小练习：**
- 把 `rtp_test()` 里的 `tp_draw_big_point(...,RED)` 换成不同颜色或按 X 坐标变色，做一个简单"调色板"。
- 在电容屏 `ctp_test()` 里显示当前触摸点数（统计 `tp_dev.sta` 的低若干位）。
- 修改 `TP_READ_TIMES`/`TP_LOST_VAL`/`TP_ERR_RANGE`，观察坐标抖动与灵敏度权衡。

**衔接**：本实验把 SPI（实验20）、IIC、24CXX、LCD 串成一个真实交互应用；继续可做综合测试实验或简易 GUI，直接复用 `tp_dev.scan()` 作为输入源。
