# 实验34 T9拼音输入法实验

> 一句话概括：在 LCD 上画一个 3×3 数字虚拟键盘（2=abc…9=wxyz），用手指按数字拼拼音，程序在内置"数字串→拼音→汉字"码表里做匹配，把拼音和候选汉字显示出来（同时输出串口），KEY0 翻页、KEY1 清除、触摸屏退格。

## 一、实验简介

- **实验目的**（官方 `readme.txt`）：学习拼音输入法在 STM32 上的实现，实现一个简单的 T9 拼音输入法。
  通俗地说：老手机上的 T9 输入法，一个数字键对应多个字母（比如按 2 代表 a/b/c）。本实验不猜你按了几下，而是让你直接输入**数字串**（如 `726`），程序把它和每个拼音的"数字编码"比对，找出所有匹配的拼音（p 在 7、a 在 2、n 在 6，所以 `726` 能对上 pan/pao/ran/rao/san/sao 共 6 条），再把每条拼音对应的候选汉字串整排展示出来。核心是理解：**输入法 = 码表 + 匹配算法 + 字库渲染**，三样都不神秘。
- **在学习路线中的位置**：前置实验11 TFTLCD、实验21 触摸屏、实验30 汉字显示（GBK 字库/取模显示）、实验33 手写识别（同一套显示框架）。是"人机交互软件栈"类实验的收官。
- **开发板与芯片**：正点原子 MiniSTM32 V4，主芯片 STM32F103RC，72MHz；需带触摸的 ALIENTEK MCU TFTLCD 模块。

## 二、实验原理

### 2.1 T9 数字键盘与"数字串→拼音"编码

经典电话/T9 键位（本工程 [User/main.c](User/main.c) 的两张表就是它）：

| 数字键 | 字母 | 数字键 | 字母 |
| --- | --- | --- | --- |
| 2 | abc | 7 | pqrs |
| 3 | def | 8 | tuv |
| 4 | ghi | 9 | wxyz |
| 5 | jkl | 6 | mno |

一个拼音逐字母替换成数字，就得到它的"数字编码"，例如：`ai` → a=2、i=4 → `"24"`；`shuang` → 7,4,8,2,6,4 → `"748264"`。码表里预先存好约 **400 个音节条目**（[Middlewares/T9INPUT/pymb.h](Middlewares/T9INPUT/pymb.h) 的 `py_index3[]` 共 399 条，含 1 条空表头），每条长这样：

```c
typedef struct
{
    char *py_input;    /* 输入的字符串,如"24" */
    char *py;          /* 对应的拼音,如"ai" */
    char *pymb;        /* 码表:该拼音的全部候选汉字,GBK字节串 */
}py_index;
...
{"24","ai",(char*)PY_mb_ai},
{"748264","shuang",(char*)PY_mb_shuang},
```

### 2.2 拼音库结构：pyinput.c/h + pymb.h（真正参与编译的部分）

| 文件 | 职责 |
| --- | --- |
| [Middlewares/T9INPUT/pyinput.h](Middlewares/T9INPUT/pyinput.h) | 声明 `py_index`、`pyinput` 结构体与接口；全局对象 `extern pyinput t9;` |
| [Middlewares/T9INPUT/pyinput.c](Middlewares/T9INPUT/pyinput.c) | 匹配算法；定义 `pyinput t9 = { get_pymb, 0 };`，`t9.getpymb` 就是函数指针 |
| [Middlewares/T9INPUT/pymb.h](Middlewares/T9INPUT/pymb.h) | 纯数据：每个音节一个 `PY_mb_xx[]` GBK 汉字串 + `py_index3[]` 索引表 |

`t9` 结构体只有两个成员：函数指针 `getpymb` 和结果数组 `pymb[MAX_MATCH_PYMB]`（`MAX_MATCH_PYMB = 10` 条容量，元素是指向 `py_index` 的指针）。

**匹配接口约定**（`get_matched_pymb()` 返回值）：

- bit7 = 0：完全匹配，低 7 位 = 匹配到的拼音条数；
- bit7 = 1：部分匹配（没有任何完全匹配时才出现），低 7 位 = 输入串里"有效匹配的位数"。

匹配用 `str_match()` 逐字符比对：两串同时走到 `'\0'` 返回 `0XFF`（完全匹配）；中途不等则返回已匹配的字符数。`get_matched_pymb()` 对 399 条表项逐一调用它，把所有完全匹配条目收进 matchlist[]；若一条都没有，则取"匹配字符数最多"的那条当最佳部分匹配（`bestmatch` 初值为 `&py_index3[0]`，代码注释写"默认为a的匹配"，实际 `py_index3[0]` 是表头那条空串占位项，它永远匹配不上任何非空输入，只充当安全初值）。

main 对部分匹配的处理很实用：把输入截断到有效位数重新查询，若还是部分匹配就把匹配数记 0——即"输入 4 位、只有前 3 位能对上"时，先按 3 位完全匹配出候选，等你继续补全。

### 2.3 汉字内码与字库渲染

- 码表里的候选汉字串 `PY_mb_xx` 是 **GBK 内码**（每个汉字 2 字节，源文件本身也是 GBK 编码存储）；
- 显示走实验30 的通路：`text_show_string()` 遇到大于 0X80 的字节就取 2 字节作 GBK 码，查 SPI FLASH（W25QXX）里预存的 GBK12/16/24 点阵字库取模、画到 LCD；ASCII 用内置字体；
- FATFS 也配了 `FF_CODE_PAGE 936`（GBK），SD 卡卷 `0:`、FLASH 卷 `1:` 开机挂载（本程序功能上不读 SD 卡）。

一句话：**"拼音库给 GBK 字节串，字库把 GBK 码变成点阵像素"**，MCU 全程不经过 Unicode。

### 2.4 一次输入的完整交互流程

```
触摸按下数字键2~9 → inputstr[] 追加字符 '2'~'9'（最长6位）
   → t9.getpymb(inputstr) 得 result_num/cur_index
   → 屏上刷新: 输入的数字串 | 匹配总数 | 当前序号 | 拼音 | 候选汉字串(两行内)
   → 串口同步打印 拼音/结果
触摸按"←/DEL"键 → 退格(inputlen--)
KEY0 物理键     → 在 result_num 个拼音间翻页(1→2→…→N→1)
KEY1 物理键     → 清除本次全部输入(回 RESTART 重画界面)
KEY_UP 物理键   → 电阻屏触摸校准(电容屏不响应)
```

虚拟键判定由 `py_get_keynum()` 完成：把 3×3 键盘区域 (x,y) 起点的每个格子做矩形命中测试，`key = i*3 + j + 1`；内部 `static key_x` 记住"上一次按住的键"，实现**一次按下只触发一次**（边沿检测），松手时把按住的格子恢复白色。

## 三、硬件资源与接线

| 资源 | 引脚 | 代码中的宏/位置 | 说明 |
| --- | --- | --- | --- |
| LED0（红灯） | PA8 | led.h | 约 300ms 翻转，指示运行 |
| LED1（绿灯） | PD2 | led.h | 未使用 |
| KEY0 | PC5 | key.h | 候选拼音翻页 |
| KEY1 | PA15 | key.h | 清除输入 |
| KEY_UP (WK_UP) | PA0 | key.h | 电阻屏触摸校准（代码判 `tp_dev.touchtype == 0`） |
| 串口1 | PA9/PA10，CH340 | usart.h | 115200 打印匹配结果 |
| LCD 8080 并口 | RD/WR/RS/CS=PC6~PC9，BL=PC10，D0~D15=PB0~PB15 | lcd.h | 虚拟键盘与结果显示 |
| 触摸屏（电阻 XPT2046） | T_CLK=PC0, T_MOSI=PC3, T_MISO=PC2, T_CS=PC13, T_PEN=PC1 | [touch.h](Drivers/BSP/TOUCH/touch.h) | 随 LCD 模块 |
| 触摸屏（电容） | SCL=PC0, SDA=PC3；GT9xxx RST=PC13, INT=PC1 | ctiic.h、gt9xxx.h | 与电阻屏引脚复用 |
| SD 卡（SPI1） | SCK=PA5, MISO=PA6, MOSI=PA7, CS=PA3 | spi_sdcard.h | 挂载卷 `0:` |
| W25QXX SPI FLASH | SPI1 共用，CS=PA2 | norflash.h | 存 GBK 字库，显示汉字必需 |

- 虚拟键盘尺寸按屏自动选择（main 中按 `lcddev.id`）：`0X5310/0X7796` 类 320×480 屏用 86×43；`0X5510` 480×800 屏用 140×70；其余（如 2.8/3.5 寸 240×320）用 60×40。键盘左上角固定在 (30,195)。
- 接线：LCD（带触摸）插右侧接口，插反烧屏；USART1 排针 PA9→RXD、PA10→TXD 跳线；`sys_stm32_clock_init(9)` = HSE 8MHz ×9 = 72MHz。

## 四、代码结构与调用流程

```
实验34 T9拼音输入法实验/
├── User/main.c                 界面/虚拟键盘/交互调度
├── Middlewares/T9INPUT/        pyinput.c/h 匹配引擎, pymb.h 码表, t9input.c/h(遗留,未参与编译)
├── Middlewares/TEXT/           text.c/fonts.c GBK 字库取模显示
├── Middlewares/FATFS/ MALLOC/ USMART/
├── Drivers/BSP/ LCD TOUCH KEY LED SPI SDMMC NORFLASH 24CXX IIC
└── Projects/MDK-ARM/atk_f103.uvprojx
```

`main()` 流程：

1. `sys_stm32_clock_init(9)` → `delay_init(72)` → `usart_init(72,115200)` → `usmart_dev.init(72)` → `led_init()` → `lcd_init()` → `key_init()` → `tp_dev.init()` → `norflash_init()` → `my_mem_init(SRAMIN)` → `exfuns_init()` → 挂载 `0:`/`1:`；
2. `RESTART:` 标签处 `while(fonts_init())` 检查字库，然后画提示文字与两行标签（输入/匹配/拼音/当前/结果），按屏算键盘大小，`py_load_ui(30,195)` 画 3×3 键盘；
3. 主循环每 10ms：`py_get_keynum(30,195)` 取虚拟键 → 更新 `inputstr`（追加数字或退格）→ `t9.getpymb()` 匹配并处理部分匹配截断 → 刷新输入区/匹配数/当前结果；
4. `key_scan(0)` 处理物理键：KEY_UP（电阻屏）校准后 `goto RESTART`；KEY0 调 `py_show_result(++cur_index)` 翻页；KEY1 清结果区并 `goto RESTART`；
5. `i == 30` 翻 LED0。

## 五、核心代码解析

### 5.1 匹配入口与部分匹配处理（User/main.c）

```c
if (inputstr[0] != NULL)
{
    temp = t9.getpymb(inputstr);              /* 得到匹配的结果数 */
    if (temp)                                 /* 有部分匹配/完全匹配的结果 */
    {
        result_num = temp & 0X7F;             /* 总匹配结果 */
        cur_index = 1;                        /* 当前为第一个索引 */
        if (temp & 0X80)                      /* 是部分匹配 */
        {
            inputlen = temp & 0X7F;           /* 有效匹配位数 */
            inputstr[inputlen] = '\0';        /* 不匹配的位数去掉 */
            if (inputlen > 1)
            {
                temp = t9.getpymb(inputstr);  /* 重新获取完全匹配字符数 */
                result_num = (temp & 0X80)? 0 : (temp & 0X7F);
            }
        }
    }
    else { inputlen--; inputstr[inputlen] = '\0'; }   /* 完全没匹配:撤销这次按键 */
}
```

- `temp & 0X7F` 取低 7 位条数、`temp & 0X80` 判断部分匹配标志位；
- 最见功力的最后一行：**按下一个键后如果发现完全无匹配，就把这次按键"吞掉"**（inputlen--），键盘手感上表现为"这个音打不下去"，与手机输入法行为一致。

### 5.2 匹配引擎核心（Middlewares/T9INPUT/pyinput.c）

```c
uint8_t str_match(uint8_t *str1, uint8_t *str2)
{
    uint8_t i = 0;
    while (1)
    {
        if (*str1 != *str2)break;         /* 部分匹配 */
        if (*str1 == '\0') { i = 0XFF; break; }  /* 完全匹配 */
        i++; str1++; str2++;
    }
    return i;
}
```

返回 `0XFF`=完全匹配；返回 n>0=前 n 个字符相同；返回 0=首字符都不符。`get_matched_pymb()` 对 399 条表项逐一调用它，收集所有完全匹配项进 `matchlist[]`；没有完全匹配时挑匹配位数最多的 `bestmatch` 返回"部分匹配 + 有效位数"。注意结果数组 `t9.pymb` 容量是 `MAX_MATCH_PYMB = 10`，而代码写入 `matchlist[mcnt++]` 时**没有做越界检查**——好在本表里同一个数字串最多只有 6 个完全匹配（如 `7426`、`726`），不会溢出。表里同码不同音是有意设计的，如 `"26"` 同时是 an、ao、bo。

### 5.3 虚拟键盘命中与边沿检测（User/main.c，py_get_keynum 摘要）

```c
if (tp_dev.x[0] < (x + j * kbdxsize + kbdxsize) && tp_dev.x[0] > (x + j * kbdxsize) &&
    tp_dev.y[0] < (y + i * kbdysize + kbdysize) && tp_dev.y[0] > (y + i * kbdysize))
{
    key = i * 3 + j + 1;
    ...
    if (key_x == key)key = 0;             /* 还按着同一个键:不重复触发 */
    else
    {
        py_key_staset(x, y, key_x - 1, 0);  /* 旧键抬起(白色) */
        key_x = key;
        py_key_staset(x, y, key_x - 1, 1);  /* 新键按下(绿色) */
    }
}
```

按下时格子填 `GREEN`、抬起恢复 `WHITE`（`py_key_staset()` 里 `lcd_fill` + 重画 caption），配合静态变量 `key_x` 实现"按住只算一次"。

### 5.4 候选显示与翻页（User/main.c，py_show_result）

```c
if (index)
{
    text_show_string(30 + 40, 125, 200, 16, (char *)t9.pymb[index - 1]->py, 16, 0, BLUE);   /* 显示拼音 */
    text_show_string(30 + 40, 145, lcddev.width - 70, 48, (char *)t9.pymb[index - 1]->pymb, 16, 0, BLUE); /* 汉字串 */
    printf("\r\n拼音:%s\r\n", t9.pymb[index - 1]->py);
    printf("结果:%s\r\n", t9.pymb[index - 1]->pymb);
}
```

`t9.pymb[]` 是 `py_index*` 指针数组（`get_pymb` 时由 `get_matched_pymb(str, t9.pymb)` 填充），`index-1` 即"第几页"。KEY0 每按一次 `cur_index++`，超过 `result_num` 回到 1 循环——这就是"候选字（拼音）翻页"。汉字串一次性整行给出（候选字太多屏放不下，本 demo 不再做汉字级的翻页点选）。

## 六、编译、下载与运行

1. Keil MDK 打开 `Projects/MDK-ARM/atk_f103.uvprojx`，器件 STM32F103RC，宏 `STM32F103xE`；注意工程只编译 `pyinput.c`，**没有**也无需 `t9input.c`；
2. ST-Link/J-Link 下载；串口 115200（XCOM/SSCOM）；
3. 板载 W25QXX 中必须先有 GBK 字库（实验30 流程更新），否则卡在 `Font Error!`；
4. USMART 已注册 `test_py(uint8_t *str)`，可串口发 `at+test_py("726")` 直接验证码表匹配。

## 七、实验现象

1. 开机 LCD 显示标题、KEY 说明、"输入/匹配/拼音/当前/结果"标签和 3×3 虚拟键盘（第一格 ←/DEL，其余 2~9 带 abc…wxyz 字母注记）；LED0 闪烁；
2. 依次点 7-2-6-4：屏上"输入"显示 `7264`，"匹配"显示 3，"拼音"显示首个匹配（pang/rang/sang 之一，取决于表中顺序），"结果"区显示对应候选汉字串（汉字来自字库）；电阻屏未校准时点不准，先按 KEY_UP 校准；
3. 按 KEY0：在多个匹配拼音间翻页，"当前"序号跟着变，串口同步打印每页拼音和结果；
4. 按虚拟 ←/DEL 键退格；输入到无匹配时该键被自动忽略；
5. 按 KEY1：清除本次输入并重新绘制界面；
6. 单字母拼音只有三个成表：输入 `2` 匹配 a、`3` 匹配 e、`6` 匹配 o；两位如 `24` 匹配 ai/bi/ci，四位 `7264` 匹配 pang/rang/sang，六位 `748264` 匹配 shuang 等长音节。

## 八、注意事项

- **与官方说明/源文件注释不一致之处（以代码为准）**：
  1. 官方 readme.txt 的硬件资源只列了 LED/按键/串口/LCD，但代码实际还用了**触摸屏**（采键盘输入）和 **SPI FLASH 字库**（显示汉字），电阻屏不校准基本没法玩；
  2. [Middlewares/T9INPUT/t9input.h](Middlewares/T9INPUT/t9input.h) 是 STM32F407 GUI 版遗留文件（`#include "guix.h"` 等本工程并不存在的头文件），`pyinput.c` 文件头注释也误写"实验平台: STM32H750 开发板"——它们都**不参与编译**（uvprojx 源文件列表里只有 `pyinput.c`），阅读时请直接看 pyinput.c/h；
  3. readme.txt 说"KEY_UP 用于触摸屏校准"，代码条件是 `tp_dev.touchtype == 0`，即**只对电阻屏生效**，电容屏按下无反应属正常。
- `py_index3[]` 中存在同码异音（如 `"26"`→an/ao/bo、`"726"`→pan/pao/ran/rao/san/sao、`"94264"`→zhang 等），所以"匹配数"可能不止一个，这正是需要 KEY0 翻页的原因；实测同码最多 6 条，小于结果数组容量 `MAX_MATCH_PYMB = 10`。
- 输入缓冲 `uint8_t inputstr[7]`，按 main.c 注释设计为"最大输入 6 个字符+结束符"；码表里最长音节编码是 6 位（如 `748264`→shuang），第 7 位不可能形成完全匹配，只会被"部分匹配截断"或"无匹配撤销"逻辑退回来。
- 汉字显示依赖 GBK 字库：换了 SPI FLASH 内容或刚更新固件时先跑一遍字库检测；`Font Error!` 不会死机但会一直闪。
- 24CXX/IIC、SD 卡等驱动虽编译进工程，本实验功能上未使用；SD 卡读写时不要掉电的通则仍然适用。
- 电容屏与电阻屏共用 PC0~PC3/PC13 引脚，切勿另接外设冲突。

## 九、总结与延伸

**要点：**

1. T9 输入法的本质是"数字编码表 + 字符串匹配"：`py_input`（数字串）→ `py`（拼音）→ `pymb`（GBK 候选汉字串）三级跳；
2. 完全匹配/部分匹配用返回值的 bit7 与低 7 位编码，配合"截断重查"和"无匹配吞键"两大交互细节；
3. GBK 内码贯穿始终：码表数据是 GBK 字节串，显示靠 GBK 点阵字库，FATFS 也开 936 代码页；
4. 虚拟键盘 = 矩形命中测试 + 静态变量边沿检测 + `lcd_fill` 状态着色；物理键负责翻页/清除/校准分工；
5. 闭源/遗留文件甄别：以 Keil 工程文件列表为准判断哪些源码真正参与编译。

**延伸练习：**

1. 在 `py_show_result()` 里给候选汉字串加"按字编号"，再用触摸点击选择某个字拼到结果行，凑成完整的"选字上屏"流程；
2. 用 USMART 发送 `at+test_py("726")`，观察 6 条完全匹配的打印顺序；再改 `py_show_result()` 让"当前"序号显示更醒目；
3. 结合实验33：把 `alientek_ncr` 识别出的英文字母串映射到 T9 数字，做一个"手写转拼音"的混合输入 demo。

**衔接**：至此，显示（LCD/字库/图片）、存储（SPI FLASH/SD/FATFS）、交互（按键/触摸/USMART）、算法（识别/拼音）四大件都已集齐，实验41 综合测试实验会把它们组装成完整应用。
