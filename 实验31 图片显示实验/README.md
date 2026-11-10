# 实验31 图片显示实验

> 一句话概括：让开发板像"数码相框"一样，从 SD 卡 `PICTURE` 文件夹里逐个读出图片文件（BMP/JPG/JPEG/GIF），在 LCD 上解码显示，用 KEY0/KEY1 切换上下张、KEY_UP 暂停播放。

## 一、实验简介

- **实验目的**（官方 `readme.txt`）：学习在 STM32 上面的图片显示方法，实现一个简单的数码相框。
  通俗地说：图片文件本质上是一堆有格式约定的字节，本实验教你 ① 用 FATFS 文件系统从 SD 卡里"按文件夹找图片"，② 按 BMP 格式约定把头信息读出来、把像素数据一行行解码，③ 每解出一个像素就调用 LCD 画点函数画到屏幕上。难点不在寄存器，而在"图片格式的理解 + 分批读文件 + 坐标缩放"。
- **在学习路线中的位置**：
  - 前置：实验10 OLED / 实验11 TFTLCD（会画点、会清屏）、实验20 SPI + 实验28 SD卡（SPI 读扇区）、实验29 FATFS（f_open/f_read 读文件）、实验27 内存管理（mymalloc）、实验30 汉字显示（text/fonts 字库）。
  - 后续：实验33 手写识别、实验34 T9 拼音输入法都直接复用本实验的"字库 + 触摸 + FATFS"框架。
- **开发板与芯片**：正点原子 MiniSTM32 V4 开发板，主芯片 STM32F103RC（Cortex-M3，512KB Flash / 48KB SRAM），主频 72MHz。

## 二、实验原理

### 2.1 BMP 位图格式：一个图片文件的"三段式"结构

BMP 是最简单的图片格式之一：不压缩（常见情况）、结构直白，相当于把每个像素的颜色按顺序摆在文件里。本实验 [Middlewares/PICTURE/bmp.h](Middlewares/PICTURE/bmp.h) 用三个 `__PACKED_STRUCT`（紧凑结构体，禁止编译器补空隙，保证和文件字节一一对应）描述了它：

```
文件偏移 0 ┌──────────────────────┐
           │ 文件头 14 字节        │ BITMAPFILEHEADER
           ├──────────────────────┤
           │ 信息头 40 字节        │ BITMAPINFOHEADER
           ├──────────────────────┤
           │ 调色板(可选)          │ RGBQUAD × N
           ├──────────────────────┤
           │ 像素数据区            │ 从 bfOffBits 处开始
文件末尾   └──────────────────────┘
```

文件头 `BITMAPFILEHEADER`（bmp.h 真实定义）：

| 字段 | 类型 | 含义（本实验用到的） |
| --- | --- | --- |
| `bfType` | uint16_t | 文件标志，固定为 `'BM'`，用来识别是不是 BMP |
| `bfSize` | uint32_t | 整个文件大小 |
| `bfReserved1/2` | uint16_t | 保留 |
| `bfOffBits` | uint32_t | 从文件头到像素数据区的偏移量——解码时"跳到数据区"就靠它 |

信息头 `BITMAPINFOHEADER`（关键字段）：

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `biSize` | uint32_t | 信息头自身大小 |
| `biWidth` / `biHeight` | long | 图片宽 / 高（像素） |
| `biPlanes` | uint16_t | 恒为 1 |
| `biBitCount` | uint16_t | 每个像素占几位：1/4/8/16/24/32 |
| `biCompression` | uint32_t | 压缩方式：`BI_RGB`(0) 不压缩、`BI_RLE8`(1)、`BI_RLE4`(2)、`BI_BITFIELDS`(3) 由掩码决定位分布 |
| `biClrUsed` | uint32_t | 实际使用的调色板颜色数 |

bmp.h 还额外定义了 `RGBQUAD`（蓝、绿、红、保留 4 字节一项的**调色板**表项）和 `BITMAPINFO`（= 文件头 + 信息头 + `RGB_MASK[3]`，共 66 字节，`minibmp_decode` 一次就读这么多字节拿头信息）。

三个必须知道的 BMP"坑"，代码里都处理了：

1. **行倒序**：BMP 数据区从图片**最底下一行**开始存。所以 stdbmp_decode 里 `y = picinfo.ImgHeight` 然后每读完一行 `y--`。
2. **每行补齐到 4 字节**：一行占的字节数 `rowlen = 宽 × 每像素字节数`，若不是 4 的倍数要向上补齐：
   ```c
   if ((picinfo.ImgWidth * color_byte) % 4)
       rowlen = ((picinfo.ImgWidth * color_byte) / 4 + 1) * 4;
   else
       rowlen = picinfo.ImgWidth * color_byte;
   ```
   `minibmp_decode` 用 `rowadd = rowlen - ImgWidth*color_byte` 记录"每行要跳过的填充字节数"，读完一行 `bmpbuf += rowadd`。
3. **颜色顺序是 BGR**（不是 RGB）：24 位图里字节顺序是 蓝、绿、红。

### 2.2 位深、RGB565 与调色板：图片怎么变成 LCD 的 16 位颜色

MiniSTM32 这块 MCU 屏是 16 位色，用的是 **RGB565**：一个 16 位整数中，高 5 位红色、中 6 位绿色、低 5 位蓝色（R:32 级, G:64 级, B:32 级）。而 BMP 常见是 24 位真彩（每通道 8 位）。**解码 = 把每像素的 BGR 字节重新拼成 RGB565**（见 [Middlewares/PICTURE/bmp.c](Middlewares/PICTURE/bmp.c)）：

```c
color = (*bmpbuf++) >> 3;                          /* B: 8位缩到5位 */
color += ((uint16_t)(*bmpbuf++) << 3) & 0X07E0;    /* G: 8位缩到6位, 放到位[10:5] */
color += (((uint16_t) * bmpbuf++) << 8) & 0XF800;  /* R: 8位缩到5位, 放到位[15:11] */
```

按 `biBitCount/8`（代码变量 `color_byte`）分四种情况：

| 位深 | color_byte | 代码处理方式 |
| --- | --- | --- |
| 24 位 | 3 | BGR 三字节 → RGB565（如上），最常用 |
| 16 位 | 2 | 两字节小端拼成 16 位；`biCompression == BI_RGB` 时按 RGB555 重新拼位，否则直接当 RGB565 用 |
| 32 位 | 4 | 取 BGR 三字节转 565，第 4 字节是 ALPHA 通道，`minibmp_decode` 里用 `piclib_alpha_blend()` 与底色做半透明混合；`stdbmp_decode` 直接忽略 |
| 8 位 | 1 | **调色板**方式：像素值只是颜色表的"编号"，要查 `RGBQUAD` 表得到真实颜色。bmp.c 明确注释"8位色,暂时不支持,需要用到颜色表"，对应分支是空的——8 位 BMP 显示不出来，请转成 24 位再用 |

所谓**调色板（颜色表）**：16/256 色图片不直接存颜色，而是先列一张"编号→颜色"的表，像素里只存编号，省空间。本实验代码路径只处理 16/24/32 位真彩。

`bmp_encode()`（截图功能）反过来：把 LCD 指定区域读出 RGB565，写上头信息和 `RGB_MASK[0..2] = 0xF800/0x07E0/0x001F`（BI_BITFIELDS 方式声明 565 掩码），存成 16 位 BMP 文件。

### 2.3 SD 卡 → FATFS → 解码器 → LCD 的数据通路

```
SD卡(物理) --SPI1协议--> spi_sdcard.c(读写块) --> diskio.c(FATFS磁盘IO glue)
        --> ff.c(FATFS文件系统, 卷0:"0:") --> f_open/f_read(按文件读)
        --> bmp.c/gif.c/tjpgd.c(格式解码) --> piclib.c(缩放/画点适配层)
        --> lcd.c lcd_draw_point(寄存器版8080并口) --> 屏幕像素
```

- SD 卡走 **SPI1 模式**（低速 CMD 握手 + 高速读块），盘符 `0:`；板载 W25QXX SPI FLASH 被 [Middlewares/FATFS/source/diskio.c](Middlewares/FATFS/source/diskio.c) 虚拟成第二个磁盘 `1:`（扇区大小 512、基址 `SPI_FLASH_FATFS_BASE`），演示 FATFS 多卷。`ffconf.h` 中 `FF_VOLUMES 2`、`FF_CODE_PAGE 936`（支持 GBK 中文文件名）、`FF_USE_LFN 3`（长文件名放内存）。
- 大文件不可能一次读进 48KB 的 SRAM，所以解码器都是**分批读**：`stdbmp_decode` 每次 `f_read` 读 `BMP_DBUF_SIZE = 2048` 字节，边读边逐像素解码画点；`minibmp_decode` 一次读整数行（`rowcnt = 2048/rowlen` 行）。这就是"逐行送 LCD"的由来。
- **缩放显示（智能画图）**：数码相框把任意尺寸图片铺满屏幕。`piclib_ai_draw_init()` 算缩放系数 `Div_Fac = min(屏宽/图宽, 屏高/图高, 1) × 8192`（放大 8192 倍转成定点数），显示时实际坐标 `realx = (x * Div_Fac) >> 13`。缩小时多个源像素会落到同一个目标点，`piclib_is_element_ok()` 判断"这个点刚画过没有"，重复就跳过，既省时间又不错位。

### 2.4 数码相框的完整工作流程

1. 开机初始化时钟/串口/LCD/按键/SPI FLASH/内存池；
2. `f_mount` 挂载 SD 卡（`0:`）和 SPI FLASH（`1:`）；
3. `fonts_init()` 检查 SPI FLASH 里的 GBK 字库（图标文字要用）；
4. 打开 `0:/PICTURE` 文件夹，用 `pic_get_tnum()` 统计图片文件总数——判定方法是 `exfuns_file_type()` 返回的高 4 位等于 `0X50`（`T_BMP/T_JPG/T_JPEG/T_GIF` 在文件类型表里排在第 5 大类）；
5. 第二遍遍历目录，把每个图片文件在目录里的偏移 `picdir.dptr` 记进索引表 `picoffsettbl[]`——之后想直接跳第 N 张，用 `dir_sdi()` 把目录指针拨过去即可，不用从头数；
6. 主循环：取出当前索引对应的文件名 → `lcd_clear(BLACK)` → `piclib_ai_load_picfile(pname, 0, 0, lcddev.width, lcddev.height, 1)` 全屏显示；
7. 显示期间每 10ms 扫一次按键：KEY0 下一张、KEY1 上一张、KEY_UP 暂停/继续（LED1 亮表示暂停）；`t` 计到 250（约 2.5 秒）没按键就自动模拟一次"下一张"，实现轮播。

## 三、硬件资源与接线

| 资源 | 引脚 | 代码中的宏/位置 | 说明 |
| --- | --- | --- | --- |
| LED0（红灯 DS0） | PA8 | `LED0_GPIO_PORT/PIN`，[led.h](Drivers/BSP/LED/led.h) | 每约 200ms 翻转一次，指示程序在跑 |
| LED1（绿灯 DS1） | PD2 | `LED1_GPIO_PORT/PIN` | 暂停时亮、播放时灭 |
| KEY0 | PC5 | `KEY0_GPIO_PORT/PIN`，key.h | 上拉输入，按下=0，下一张 |
| KEY1 | PA15 | `KEY1_GPIO_PORT/PIN` | 按下=0，上一张 |
| KEY_UP (WK_UP) | PA0 | `WKUP_GPIO_PORT/PIN` | 下拉输入，按下=1，暂停/继续 |
| 串口1 | PA9(TX)/PA10(RX) | usart.h | 115200，接板载 CH340，USMART 调试用 |
| SD 卡（SPI 模式） | SCK=PA5, MISO=PA6, MOSI=PA7, CS=PA3 | [spi.h](Drivers/BSP/SPI/spi.h)、[spi_sdcard.h](Drivers/BSP/SDMMC/spi_sdcard.h) | 卡插模块上的 TF 卡座 |
| W25QXX SPI FLASH | 与 SD 共用 SPI1，CS=PA2 | [norflash.h](Drivers/BSP/NORFLASH/norflash.h) | 存 GBK 字库 + 虚拟磁盘 `1:` |
| LCD（8080 并口 MCU 屏） | RD=PC6, WR=PC7, RS=PC8, CS=PC9, BL=PC10, D0~D15=PB0~PB15 | [lcd.h](Drivers/BSP/LCD/lcd.h) | RST 与系统复位共用 |

- **寄存器版的 LCD 不是 FSMC**：`lcd.h` 用普通 GPIO 翻转 WR/RD/CS/RS 来**软件模拟 8080 并口时序**（`LCD_WR(x)` 等宏直接写 `GPIOC->BSRR`），16 位数据线整组走 `GPIOB->ODR/IDR`。所以 `lcd_read_point()` 能真的把屏上颜色读回来（BMP 的 32 位 ALPHA 混合、截图编码都依赖它）。
- 接线：TFTLCD 模块插板子右侧 LCD 接口（注意方向）；SD 卡需自行格式化（FAT16/FAT32），根目录建 `PICTURE` 文件夹并把图片放进去；用 USB 线连 `USB_UART` 口看串口信息；USART1 排针上 PA9/PA10 需跳线到 RXD/TXD 才能和电脑通信。
- 时钟：`sys_stm32_clock_init(9)` 把 8MHz 外部晶振经 PLL 9 倍频到 72MHz；SD 卡初始化用 `SPI_SPEED_256`（约 281kHz）低速握手，之后切 `SPI_SPEED_2`（36MHz）高速读卡。

## 四、代码结构与调用流程

```
实验31 图片显示实验/
├── User/main.c                    数码相框主逻辑（找图、索引、轮播、按键）
├── Drivers/BSP/LCD/               lcd.c 8080模拟、画点/读点/清屏；lcd_ex.c 扩展绘图
├── Drivers/BSP/SDMMC/spi_sdcard.c SPI 模式 SD 卡驱动（发命令、读写扇区）
├── Drivers/BSP/NORFLASH/norflash.c W25QXX 驱动（字库存储介质）
├── Drivers/BSP/SPI/spi.c          SPI1 硬件外设，SD 卡与 FLASH 共用
├── Drivers/BSP/KEY/  Drivers/BSP/LED/  按键、LED
├── Middlewares/FATFS/             fatfs 文件系统 + exfuns 扩展（文件类型识别）
├── Middlewares/MALLOC/            SRAM 内存池（管理 40K，块粒度 32 字节）
├── Middlewares/PICTURE/           piclib.c 统一入口；bmp.c/gif.c/tjpgd.c 各格式解码器
├── Middlewares/TEXT/              字库点阵取模显示（GBK）
└── Middlewares/USMART/            串口调用函数调试工具
```

`main()` 流程（编号步骤）：

1. `sys_stm32_clock_init(9)` → `delay_init(72)` → `usart_init(72,115200)` → `usmart_dev.init(72)` → `led_init()` → `lcd_init()` → `key_init()` → `norflash_init()` → `my_mem_init(SRAMIN)`；
2. `exfuns_init()`（给 FATFS 申请内存）→ `f_mount(fs[0],"0:",1)` 挂 SD 卡、`f_mount(fs[1],"1:",1)` 挂 FLASH；
3. `while(fonts_init())` 死循环提示 `Font Error!`，直到字库正常；屏上打印实验标题与操作说明；
4. `while(f_opendir(&picdir,"0:/PICTURE"))` 死循环提示"PICTURE文件夹错误!"，直到找到文件夹；
5. `pic_get_tnum()` 统计图片数；`mymalloc` 申请 `picfileinfo`、带路径文件名缓冲 `pname`（`FF_MAX_LFN*2+1` 字节）、索引表 `picoffsettbl`（4×图片数 字节）；
6. 遍历目录建索引 `picoffsettbl[]`；
7. `piclib_init()` 把画点/读点函数指针绑定到 LCD 驱动，进入显示循环（见 2.4 第 6、7 步）。

## 五、核心代码解析

### 5.1 目录扫描与索引（User/main.c）

```c
res = f_opendir(&picdir, "0:/PICTURE");         /* 打开目录 */
if (res == FR_OK)
{
    curindex = 0;
    while (1)
    {
        temp = picdir.dptr;                     /* 记录当前目录索引 */
        res = f_readdir(&picdir, picfileinfo);  /* 读取目录下的一个文件 */
        if (res != FR_OK || picfileinfo->fname[0] == 0)break;
        res = exfuns_file_type(picfileinfo->fname);
        if ((res & 0XF0) == 0X50)               /* 取高四位,看看是不是图片文件 */
        {
            picoffsettbl[curindex] = temp;
            curindex++;
        }
    }
}
```

- `f_readdir()` 每调一次把下一个文件的信息填进 `picfileinfo`，读完返回 `fname[0]==0`；
- `exfuns_file_type()` 对比后缀与 `FILE_TYPE_TBL` 表，返回 `(大类<<4)|小类`；图片类在 `{"BMP","JPG","JPEG","GIF"}` 这一组（大类 5），所以高 4 位是 `0X50` 就是图片；
- 先记下 `picdir.dptr` 再读文件，存的其实是"这个文件在目录表里的位置"，之后 `dir_sdi(&picdir, picoffsettbl[curindex])` 就能随机跳转到第 curindex 张——翻页时才不用重新遍历整个目录。

### 5.2 显示与按键轮播（User/main.c）

```c
dir_sdi(&picdir, picoffsettbl[curindex]);           /* 改变当前目录索引 */
res = f_readdir(&picdir, picfileinfo);
strcpy((char *)pname, "0:/PICTURE/");
strcat((char *)pname, (const char *)picfileinfo->fname);
lcd_clear(BLACK);
piclib_ai_load_picfile(pname, 0, 0, lcddev.width, lcddev.height, 1); /* 显示图片 */
...
if (t > 250)key = 1;        /* 模拟一次按下KEY0(约2.5s自动换下一张) */
if ((t % 20) == 0)LED0_TOGGLE();
if (key == KEY1_PRES) { ... curindex = totpicnum - 1; break; }   /* 上一张(循环) */
else if (key == KEY0_PRES) { curindex++; if (curindex>=totpicnum)curindex=0; break; }
else if (key == WKUP_PRES) { pause = !pause; LED1(!pause); }     /* 暂停时LED1亮 */
```

内层 `while(1)` 每圈 `delay_ms(10)`；按键判定后 `break` 回到外层重新走"定位目录→拼路径→清屏→解码显示"。

### 5.3 格式分发（Middlewares/PICTURE/piclib.c）

```c
temp = exfuns_file_type(filename);      /* 得到文件的类型 */
switch (temp)
{
    case T_BMP:              res = stdbmp_decode(filename); break;
    case T_JPG:
    case T_JPEG:             res = jpg_decode(filename, fast); break;
    case T_GIF:              res = gif_decode(filename, x, y, width, height); break;
    default:                 res = PIC_FORMAT_ERR; break;
}
```

按**文件后缀**选解码器（不看文件内容，所以扩展名要和真实格式一致）。`piclib_init()` 里的函数指针绑定值得注意：

```c
pic_phy.read_point = lcd_read_point;    /* 读点函数实现,仅BMP需要 */
pic_phy.draw_point = lcd_draw_point;    /* 画点函数实现 */
```

解码库不直接依赖 lcd.c，而是通过 `_pic_phy` 这层"物理接口"回调，方便移植到别的屏。

### 5.4 BMP 逐像素解码（Middlewares/PICTURE/bmp.c，stdbmp_decode 核心段）

```c
count = pbmp->bmfHeader.bfOffBits;              /* 数据偏移,得到数据段的开始地址 */
color_byte = pbmp->bmiHeader.biBitCount / 8;    /* 彩色位 16/24/32 */
y = picinfo.ImgHeight;                          /* 初值=图高，因为BMP从最后一行开始存 */
...
case 0: color = bmpbuf[count] >> 3; break;                       /* B */
case 1: color += ((uint16_t)bmpbuf[count] << 3) & 0X07E0; break; /* G */
case 2: color += ((uint16_t)bmpbuf[count] << 8) & 0XF800; break; /* R */
rgb++; count++;
if (rgb == color_byte)                            /* 凑齐一个像素就画 */
{
    realx = (x * picinfo.Div_Fac) >> 13;          /* 缩放后的x坐标 */
    if (piclib_is_element_ok(realx, realy, 1) && yok)
        pic_phy.draw_point(realx + picinfo.S_XOFF, realy + picinfo.S_YOFF - 1, color);
    x++; color = 0x00; rgb = 0;
}
if (countpix >= rowlen) { y--; ... }              /* 一行结束，往上一行走 */
```

一个字节一个字节的 `switch(rgb)` 就是在"攒像素"：攒够 `color_byte` 个字节拼成一个 RGB565，画一个点。缓冲区读完 2048 字节就再 `f_read` 下一批，实现"流式"解码，SRAM 占用与图片大小无关。

## 六、编译、下载与运行

1. Keil MDK 打开 `Projects/MDK-ARM/atk_f103.uvprojx`；器件为 STM32F103RC，预处理宏 `STM32F103xE`，头文件搜索路径包含 `..\..\Drivers`、`..\..\User`。
2. 编译产物在 `Projects/MDK-ARM/Output/`（atk_f103.axf/bin）。
3. 用 ST-Link 或 J-Link 下载（Settings 里选对应调试器）；也可串口 ISP（BOOT0 置 1 复位后下载）。
4. 串口助手（推荐 XCOM/SSCOM）波特率 **115200**，8N1，接板载 CH340；USMART 可用 `at+exec` 方式手动调用函数（本工程注册的是 `piclib_ai_load_picfile` 和 `bmp_encode`）。

## 七、实验现象

1. 开机 LCD 依次显示"正点原子STM32开发板/图片显示 实验/KEY0:NEXT KEY1:PREV/KEY_UP:PAUSE"；若字库或 PICTURE 文件夹有问题，会闪红字提示并停在对应 `while` 循环。
2. 一切正常时显示"开始显示..."，随后全屏显示 `PICTURE` 里第一张图片，左上角用红字标出当前图片的完整路径。
3. 约 2.5 秒自动切下一张；按 KEY0 立即下一张、KEY1 上一张（到末尾自动回绕）；按 KEY_UP 暂停（LED1 绿灯亮）/恢复（LED1 灭）。
4. LED0 红灯约 200ms 闪烁一次，表示程序活着。
5. JPG/JPEG/GIF 文件同样能显示（各自走 tjpgd/gif 解码器），速度比 BMP 慢（要解压）。

## 八、注意事项

- **代码与官方 readme.txt 不一致之处**：
  1. readme.txt 写"norflash 连接在 SPI2 上"，但本工程寄存器版代码里 [norflash.c](Drivers/BSP/NORFLASH/norflash.c) 的 `norflash_init()` 明确调用 `spi1_init()/spi1_set_speed()`，且 `spi.c/spi.h` 只实现了 SPI1——SPI FLASH 和 SD 卡**共用 SPI1**，靠 CS=PA2（FLASH）与 CS=PA3（SD）区分片选。以代码为准。
  2. readme.txt 说"可通过 USMART 调用 ai_load_picfile 和 minibmp_decode"，而 [usmart_config.c](Middlewares/USMART/usmart_config.c) 实际注册的是 `piclib_ai_load_picfile` 和 `bmp_encode`。
- 图片格式要求：24 位 BMP 最稳妥；**8 位（调色板）BMP 代码不支持**（bmp.c 中对应分支为空）；文件名扩展名必须与真实格式一致，因为解码器按后缀分发。
- 必须先有 GBK 字库：开机 `fonts_init()` 读 SPI FLASH 里的字库信息，字库丢失会一直刷 `Font Error!`。可先用实验30 汉字显示实验的更新流程把 `SYSTEM/FONT/` 下的字库文件从 SD 卡烧录进去。
- SD 卡建议 FAT32/FAT16 格式、图片放**根目录的 PICTURE 文件夹**。SD 卡读写过程中**不要拔卡、不要掉电**，否则 FAT 表可能损坏。
- 大图片（几百万像素）解码时间长，属正常现象（每 2048 字节一批 + 逐点画屏）；想快可以用 `minibmp_decode`（USMART 下不可用，它没注册）。
- 内存都来自内部 SRAM 内存池（40K 管理上限），`mymalloc` 返回 NULL 时程序会提示"内存分配失败!"。
- 4.3/7 寸屏耗电大，USB 供电可能不足，建议外接电源。
- 触摸/摇杆无关；本实验不动任何源码，若 LCD 花屏先检查排针方向与 BL 脚。

## 九、总结与延伸

**要点：**

1. BMP = 文件头(14B)+信息头(40B)+可选调色板+数据区；记住"行倒序、行补 4 字节、BGR 顺序"三大特性。
2. RGB565 与 24 位真彩的转换就是移位+掩码：`B>>3 | (G<<3)&0x7E0 | (R<<8)&0xF800`。
3. FATFS 通过 diskio.c 把 SD 卡（`0:`）和 SPI FLASH（`1:`）挂成两个卷；`dir_sdi()` 随机定位目录项实现图片索引。
4. 大文件流式解码：固定 2048 字节缓冲 + 逐像素攒色 + 逐行画屏，SRAM 占用恒定。
5. `Div_Fac` 定点缩放（放大 8192 倍、右移 13 位还原）+ `piclib_is_element_ok()` 去重，实现全屏缩放显示。

**延伸练习：**

1. 改轮播速度：把 `if (t > 250)key = 1;` 的 250 改成 500，图片变成约 5 秒一换。
2. 显示进度：在外层循环里把 `curindex/totpicnum` 用 `lcd_show_num()` 显示在屏幕角落。
3. 截图试验：用 USMART 发送 `bmp_encode` 命令把当前屏幕存成 16 位 BMP 写到 SD 卡，再放回 PICTURE 里看它能否被自己解码显示。

**衔接**：理解了"FATFS+字库+LCD 显示"这套组合，实验33 手写识别和实验34 T9 拼音输入法就是在其上叠加触摸输入与算法库。
