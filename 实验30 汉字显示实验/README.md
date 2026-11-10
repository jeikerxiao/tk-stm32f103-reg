# 实验30 汉字显示实验

> 一句话概括：搞明白"汉字 = 点阵图 + 查表偏移"的原理，把正点原子字库文件（GBK12/16/24.FON）从 SD 卡经 FATFS 搬进板载 SPI FLASH，再驱动 LCD 逐字显示所有 GBK 汉字。

## 一、实验简介

- **实验目的**（官方 readme.txt）：学习汉字显示原理，掌握字库制作以及 LCD 显示汉字的方法。
- **通俗解释**：LCD 只认识"哪个像素点亮"，不认识"字"。显示任何字都要先有一张黑白点阵图（字模），再把成千上万张点阵图按编号排成一个文件（字库），显示时按文字的编码算出它在字库里的位置、读出点阵、逐点画屏。ASCII 码表小（95 个可见字符），点阵可以直接编译进程序；汉字成千上万，必须放外部存储按需读取——这就是本实验的全部主线。
- **在学习路线中的位置**：前置实验11 TFTLCD（画点/显示 ASCII）、实验20 SPI 与实验26（W25Qxx 读写）、实验27 内存管理、实验28 SD 卡、实验29 FATFS——本实验是它们的"会师"之作；后续实验31 图片显示、实验33 手写识别继续使用 TEXT/fonts 这套设施。
- **开发板与芯片**：MiniSTM32 V4，STM32F103RC，72MHz。

## 二、实验原理

### 2.1 点阵字模：一个汉字就是一张小 bitmap

以 16×16 为例：把汉字放进 16 行 16 列的格子里，有笔画的格点 1、没笔画的 0。正点原子字模采用**列行式**存储：按列扫描，每列从上到下 16 个点对应 2 个字节，**高位在上**（bit7 = 最上面的点）。所以：

```text
1个点阵字符的字节数 csize = (size/8 + ((size%8) ? 1 : 0)) × size   （汉字是 size×size 的点阵）
12号: (12/8+1)×12 = 24 字节      16号: 2×16 = 32 字节      24号: 3×24 = 72 字节
```

ASCII 字符是 size/2 宽、size 高（如 1608 = 高16 宽8），字节数公式中再乘 `size/2`（见 lcd.c 的 `lcd_show_char`）。

### 2.2 内码与区位码：怎么从"字"算出"它在字库第几个"

- **区位码（GB2312 的思路）**：GB2312 把 6763 个汉字排进一张 94 区×94 位的表，"区号+位号"就是坐标。为了和 ASCII 区分，汉字机内码 = 区码加 0xB0（实际区号从 16 区起用）、位码加 0xA0。例："中"在 54 区 48 位 → 内码高字节 = 54+0xB0 = 0xD6，低字节 = 48+0xA0 = 0xD0，即 `0xD6D0`。
- **GBK 全表（本工程字库的排法）**：高字节 0x81~0xFE、低字节 0x40~0xFE（去掉 0x7F），每个高字节下正好 **190** 个有效低字节。字库文件就严格按"内码从 0x8140 开始顺序编号"排列，第 n 个汉字的点阵在文件内偏移 `n × csize`。[text.c](Middlewares/TEXT/text.c) 的 `text_get_hz_mat` 原文公式：

```c
qh = *code; ql = *(++code);
if (ql < 0x7f) ql -= 0x40;   else ql -= 0x41;   /* 低字节换算，每区190个 */
qh -= 0x81;                                       /* 高字节从0x81起 */
foffset = ((unsigned long)190 * qh + ql) * csize; /* 得到字库中的字节偏移量 */
```

- 算一遍"中"(0xD6D0)：qh=0xD6-0x81=85；ql=0xD0≥0x80 → 0xD0-0x41=143；序号 = 190×85+143 = 16293；16 号字模偏移 = 16293×32 = **521376 字节**，从 GBK16 在 FLASH 中的基址 `ftinfo.f16addr` 加上去读 32 字节即得点阵。
- 非法/超范围编码（`qh<0x81 || ql<0x40 || ql==0xff || qh==0xff`）一律填充空格子（全 0），所以显示未收录的字只会"留白"不会跑飞。

### 2.3 字库文件从哪来（本工程没有 font.h 内置汉字库）

- **ASCII 点阵**：直接编译在程序里，来自 [Drivers/BSP/LCD/lcdfont.h](Drivers/BSP/LCD/lcdfont.h) 的 `asc2_1206/asc2_1608/asc2_2412/asc2_3216` 四个数组（下标 = 字符-' '）。
- **GBK 汉字点阵**：用正点原子"字库提取工具"生成的 4 个文件，按开发指南规定放在 SD 卡 `/SYSTEM/FONT/` 目录。本工程代码实际使用（[fonts.c](Middlewares/TEXT/fonts.c) 的 `FONT_GBK_PATH`）：

```c
char *const FONT_GBK_PATH[4] =
{
    "/SYSTEM/FONT/UNIGBK.BIN",   /* Unicode→GBK 内码转换表 */
    "/SYSTEM/FONT/GBK12.FON",    /* 12×12 点阵库 */
    "/SYSTEM/FONT/GBK16.FON",    /* 16×16 点阵库 */
    "/SYSTEM/FONT/GBK24.FON",    /* 24×24 点阵库 */
};
```

  更新程序把这 4 个文件依次搬进 W25Qxx，并在 `FONTINFOADDR = 4.2MB` 处保存一张 33 字节的"字库目录"（`_font_info ftinfo`：`fontok` 标志 + UNIGBK/GBK12/16/24 各自的地址和大小），此后上电只要读回 `ftinfo.fontok == 0xAA` 就认为字库在位。UNIGBK.BIN 是 Unicode 转 GBK 的映射表，供需要接收 UTF-8/UTF-16 文本的场景（如后续实验的输入法/网络文本）使用，本实验直接以 GBK 内码取模，暂不查询它。
- **为什么搬进 SPI FLASH 而不是留 SD 卡上用？** SD 卡经 FATFS 读一个点阵要走文件系统层层查表，慢且缓冲区占用大；搬进 W25Qxx 后是"裸地址"读，16 号字模 32 字节一次 SPI 事务到位，逐字刷屏才跟得上。

### 2.4 ASCII 与汉字如何分流（显示层）

两套入口，别混淆：

1. **纯 ASCII**：`lcd_show_string`（lcd.c）只接受 `' '`~`'~'` 范围字符，逐调 `lcd_show_char` 查内置表 `asc2_xxxx[chr - ' ']`，宽度按 `size/2` 前进——给它传中文会直接停在不认识的字节上。
2. **混合文本**：`text_show_string`（text.c）逐字节扫描，**`*pstr > 0x80` 即判定汉字**：走 `text_show_font`（内部 `text_get_hz_mat` → `norflash_read` 取点阵 → 逐位 `lcd_draw_point` 画点），指针 +2、x 前进 `size`；否则该字节是 ASCII，调 `lcd_show_char`，指针 +1、x 前进 `size/2`。遇到 `\r` 或超出 width/height 自动换行。

显示模式的位含义（两个 show 函数共用）：`mode=0` 正常显示，空白点用背景色 `g_back_color` 覆盖；`mode=1` 叠加显示，空白点不画（可叠加在图片上）。

## 三、硬件资源与接线

| 资源 | 引脚 | 代码中的宏/位置 | 说明 |
| --- | --- | --- | --- |
| SPI FLASH W25Qxx（存字库） | SPI1: PA5/PA6/PA7，CS=**PA2** | `NORFLASH_CS_GPIO_PIN`，[norflash.h](Drivers/BSP/NORFLASH/norflash.h) | 板载；字库区从 4.2MB 起，占 791 个 4KB 扇区 |
| SD 卡（字库来源） | SPI1 同上，CS=**PA3** | [spi_sdcard.h](Drivers/BSP/SDMMC/spi_sdcard.h) | 自备 TF 卡，内含 /SYSTEM/FONT/ 字库文件 |
| TFTLCD | MCU 16 位并口 | [lcd.h](Drivers/BSP/LCD/lcd.h) | 显示汉字的主战场，仅支持 MCU 屏 |
| LED0 | PA8 | [led.h](Drivers/BSP/LED/led.h) | 每显示一个字翻转一次，指示在跑 |
| KEY0 | PC5 | [key.h](Drivers/BSP/KEY/key.h) | 运行中按下 → `goto UPD` 强制重新更新字库 |
| KEY1 / WK_UP | PA15 / PA0 | key.h | 本实验逻辑未使用 |
| 串口1（USMART 备用） | PA9/PA10 → CH340 | [usart.h](Drivers/SYSTEM/usart/usart.h) | 115200bps，可远程调用 mf_*/fonts_* 函数 |

- 接线：LCD 模块插板载 LCD 插座（注意 VCC 方向，插反烧模块）；TF 卡插板载卡座；W25Qxx 已焊在板上。正点原子资料盘提供的卡上文件系统（含 EXLIB 字库目录）直接拷入 SD 卡即可。
- 时钟：`sys_stm32_clock_init(9)` → 72MHz；SPI1 在 APB1(36MHz) 上，norflash 初始化后跑高速档，SD 卡识别期低速、运行期 18MHz。

## 四、代码结构与调用流程

```text
实验30 汉字显示实验/
├── User/main.c                       字库检查/更新 + 全GBK汉字扫描显示
├── Middlewares/TEXT/fonts.c/.h       字库管理：搬运、字库信息ftinfo、在位检测
├── Middlewares/TEXT/text.c/.h        显示引擎：text_show_string / text_show_font
├── Middlewares/FATFS/...             文件系统（读SD卡字库文件用）
├── Middlewares/MALLOC/               4K搬运缓冲、FIL对象等动态内存
└── Drivers/BSP/{NORFLASH,SDMMC,SPI,LCD,KEY,LED}/
```

`main()` 流程：

1. 基础初始化（时钟/delay/串口/USMART/LED/LCD/按键）→ `norflash_init()` → `my_mem_init(SRAMIN)` → `exfuns_init()` → `f_mount` "0:" 与 "1:"；
2. `fonts_init()`：连续 10 次从 FLASH 4.2MB 处读 `ftinfo`，看 `fontok==0xAA`；
3. **字库缺失 → 进入 UPD 标签**：`while(sd_init())` 等卡 → `fonts_update_font(20,90,16,"0:",RED)`：先探测 4 个字库文件都在 → 擦除 791 个扇区（先读 4096 字节校验，全 0xFF 则跳过擦除）→ 逐个文件 `f_open/f_read` + `norflash_write` 搬运，LCD 显示百分比进度 → 写回 `ftinfo`（fontok=0xAA）；失败循环重试；
4. 字库 OK 后显示说明文字（全部用 `text_show_string`，中英文混排）；
5. 双重循环穷举 GBK 内码：高字节 i 从 0x81 到 0xFE，低字节 j 从 0x40 到 0xFD（跳过 0x7F），每取一个内码：显示内码高低字节、汉字计数器，并用 `text_show_font` 分别按 **24/16/12 号** 三种字号画出当前汉字；每个字约 200ms 延时并扫描 KEY0（按下则回第 3 步强制更新字库）。

## 五、核心代码解析

### 5.1 取点阵：text_get_hz_mat 的地址计算（text.c，节选）

```c
uint8_t csize = (size / 8 + ((size % 8) ? 1 : 0)) * (size); /* 一个汉字点阵字节数 */
...
foffset = ((unsigned long)190 * qh + ql) * csize;   /* 得到字库中的字节偏移量 */
switch (size)
{
    case 12: norflash_read(mat, foffset + ftinfo.f12addr, csize); break;
    case 16: norflash_read(mat, foffset + ftinfo.f16addr, csize); break;
    case 24: norflash_read(mat, foffset + ftinfo.f24addr, csize); break;
}
```

- 公式含义见 2.2；`ftinfo.f12addr/f16addr/f24addr` 是更新字库时记录进 FLASH 的**各库基址**，所以代码不需要把文件名/大小写死，全靠 `ftinfo` 这张"目录"。
- `190*qh` 用 `unsigned long` 强转防止溢出（190×125×72 > 16 位范围）。

### 5.2 画点：text_show_font 的逐位扫描（text.c，节选）

```c
for (t = 0; t < csize; t++)
{
    temp = dzk[t];                 /* 得到点阵数据（一列的前/后8个点） */
    for (t1 = 0; t1 < 8; t1++)
    {
        if (temp & 0x80) lcd_draw_point(x, y, color);        /* 有效点,画字色 */
        else if (mode == 0) lcd_draw_point(x, y, g_back_color); /* 空白点,画背景 */
        temp <<= 1;
        y++;
        if ((y - y0) == size) { y = y0; x++; break; }        /* 画满一列,换列 */
    }
}
```

高位在先是关键：`temp & 0x80` 取最上面的点，每移一位 y+1 往下画；一列画满 `size` 个点后 x+1 换下一列。`dzk` 行缓冲用 `mymalloc(SRAMIN, size)` 申请、用完 `myfree`——实验27 的知识在这里落地。

### 5.3 ASCII/汉字分流：text_show_string（text.c，节选）

```c
while (*pstr != 0)
{
    if (!bHz)
    {
        if (*pstr > 0x80) bHz = 1;             /* 中文: 标记,下次循环处理 */
        else { lcd_show_char(x, y, *pstr, size, mode, color); pstr++; x += size / 2; }
    }
    else            /* 中文 */
    {
        bHz = 0;
        text_show_font(x, y, pstr, size, mode, color);  /* 显示这个汉字 */
        pstr += 2;
        x += size;      /* 下一个汉字偏移 */
    }
}
```

`bHz` 是"当前是否已见到汉字首字节"的状态位：首字节 >0x80 时只置 `bHz=1` 且指针不动，下一轮就进 else 分支，把**指向两字节内码首地址**的 `pstr` 传给 `text_show_font`（`text_get_hz_mat` 内部读 `*code` 和 `*(code+1)` 得到高低字节），随后 `pstr += 2`。注意汉字宽度是 ASCII 的两倍（x 分别前进 `size` 和 `size/2`），所以混排文本天然对齐。

### 5.4 字库搬运：fonts_update_fontx 与 ftinfo（fonts.c，节选）

```c
ftinfo.f16addr = ftinfo.f12addr + ftinfo.gbk12size; /* GBK12之后，紧跟GBK16字库 */
ftinfo.gbk16size = fftemp->obj.objsize;             /* GBK16字库大小 */
flashaddr = ftinfo.f16addr;                         /* GBK16的起始地址 */
...
while (res == FR_OK)
{
    res = f_read(fftemp, tempbuf, 4096, (UINT *)&bread);    /* 读取数据 */
    if (res != FR_OK) break;
    norflash_write(tempbuf, offx + flashaddr, bread);       /* 写入FLASH */
    offx += bread;
    fonts_progress_show(x, y, size, fftemp->obj.objsize, offx, color);
    if (bread != 4096) break;    /* 读完了 */
}
```

以 4096 字节（恰好 FLASH 一个扇区）为节拍：FATFS 从 SD 卡读 → `mymalloc` 的缓冲 → W25Qxx 写，并用 `fftemp->obj.objsize`（文件总大小）算进度百分比。搬完 4 个库后 `ftinfo.fontok = 0XAA` 并 `norflash_write` 回 4.2MB 处。`fonts_init()` 读回校验，连续 10 次都失败才判定字库丢失——容错偶发读错。

## 六、编译、下载与运行

1. Keil MDK 打开 [Projects/MDK-ARM/atk_f103.uvprojx](Projects/MDK-ARM/atk_f103.uvprojx)，器件 STM32F103RC，编译输出在 `Output/`。
2. ST-Link/J-Link 下载。串口助手 115200bps（USMART 备用）。
3. 准备 TF 卡：把正点原子提供的 SD 卡系统文件（含 `/SYSTEM/FONT/UNIGBK.BIN、GBK12.FON、GBK16.FON、GBK24.FON`）拷入卡根目录后再插卡。
4. 首次上电会自动走"更新字库"流程（约 3MB 数据，需要一些时间，LCD 显示进度百分比），更新完成后才开始显示汉字。

## 七、实验现象

1. 第一次烧录（FLASH 无字库）：先显示 `SD Card OK / Font Updating...`，接着"擦扇区→依次更新 UNIGBK.BIN、GBK12/16/24"，右侧有百分比进度；完成后显示 `Font Update Success!`，随后清屏进入汉字演示。
2. 再次上电（字库已在 FLASH）：直接跳过更新，屏幕显示四行混排文本：`正点原子STM32开发板`、`GBK字库测试程序`、`正点原子@ALIENTEK`、`按KEY0,更新字库`，说明 12/16 字号中英文同屏正常。
3. 主演示：左列逐行显示 `内码高字节: D6 / 内码低字节: D0 / 汉字计数器: 16293`；右列同一汉字分别以 **24、16、12 号** 三种字号画出；约每 0.2 秒切换到下一个内码，LED0 同步闪烁。
4. 演示中途按 KEY0：画面跳到更新流程（`goto UPD`），重新擦写并搬运字库一遍，完成后继续显示——可用来修复字库损坏。
5. 若无字库且无 SD 卡：循环显示 `SD Card Failed!`，无法进入演示。

## 八、注意事项

- **代码与官方 readme.txt 的两处不一致（以代码为准）**：
  1. readme 说查找 `UNIGBK.BIN、GBK12.FON、GBK16.FON、GBK24.FON 和 GBK32.FON` 五个文件、"四种字体都显示"；本寄存器版代码 `FONT_GBK_PATH` 只有 **4 个文件（无 GBK32.FON）**，`ftinfo` 也无 gbk32 字段，主程序只演示 **24/16/12 三种字号**（`text_get_hz_mat` 的 switch 也只到 24，size 传 32 取不到点阵）。readme 描述的是标准库大容量版本。
  2. readme 说 norflash "连接在 SPI1 上"——这点与代码一致；实验29 的 readme 写 SPI2 才是笔误（见实验29 注意事项）。
- **字库与 FATFS 分区共存**：W25Qxx 前 4.2MB 是"1:" FATFS 盘，4.2MB 起 791 个扇区是字库区（`FONTSECSIZE 791`，注释写明共 3238700 字节）；往 "1:" 盘写满 4.2MB 不会伤字库，但**绝不要手工改动 4.2MB 以后的 FLASH 内容**。
- 更新字库期间**严禁拔卡、掉电**：搬一半断掉会造成 ftinfo 标志仍为 0xAA 但点阵残缺，出现乱字，按 KEY0 强制重更一次即可修复。
- SD 卡上的字库文件名必须严格是 `/SYSTEM/FONT/GBK16.FON` 这类**大写、无长名差异**的路径（代码用 `f_open` 按字面路径打开；LFN 已开，但推荐用标准目录包）。
- 若只看到 ASCII 看不到汉字且满屏空白格，多半是 `fonts_init` 失败或 `ftinfo` 地址读错；先用 USMART `fonts_init()` 看返回值。
- 源码 GBK 编码，编辑器需选 GB2312/GBK，否则 `main.c` 里的测试中文串会乱码（乱码的内码不在 GBK 范围，显示成空格）。

## 九、总结与延伸

- 掌握要点：
  1. 点阵字模与"内码 → 序号 → 字节偏移"的查表数学：`偏移 = (190×(高-0x81)+低换算)×csize`；
  2. ASCII 内置表（lcdfont.h）与 GBK 外置字库（SD→FATFS→W25Qxx）两套来源的分工；
  3. `text_show_string` 用"字节 >0x80"分流 ASCII/汉字的实现；
  4. `ftinfo` 目录头 + `fontok` 标志的字库在位检测/更新机制；
  5. FATFS、内存池、SPI FLASH 三个前置知识在真实功能里的闭环。
- 延伸小练习：
  1. 把 main.c 的字号演示加上 24 号居中（用 `text_show_string_middle`），体会 `mode=1` 叠加效果；
  2. 修改双重循环的起止内码，只遍历 `0xB0A1~0xF7FE`（GB2312 汉字区），观察"汉字计数器"与区位码表中序号的关系（每区 94 个 vs 每区 190 个的区别）；
  3. 故意删掉 SD 卡里的 GBK24.FON 再更新字库，观察 `fonts_update_font` 返回值与提示，理解"先探测 4 文件齐全才擦除"的保护逻辑。
- 衔接：实验31 图片显示继续用 FATFS 从 SD 卡读 BMP/JPG；实验12/USMART + 本实验的 `text_show_string` 也是后续综合测试（实验41）显示界面的基础。
