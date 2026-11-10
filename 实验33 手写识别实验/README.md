# 实验33 手写识别实验

> 一句话概括：在电阻/电容触摸屏上"写字"，程序把笔迹记录成一串坐标点，交给正点原子闭源手写识别库（ATKNCR）识别出数字或字母，结果显示在 LCD 上并经串口打印。

## 一、实验简介

- **实验目的**（官方 `readme.txt`）：学习 ALIENTEK 手写识别库的使用，实现一个简单的数字/字符手写识别程序。
  通俗地说：识别本身是个"黑盒"——本实验教你围绕这个黑盒搭起完整链路：触摸采点 → 笔迹画到屏上 → 攒成轨迹数组 → 喂给识别库 → 取回字符并展示。这也是嵌入式 AI/算法库集成的典型模式（以后接语音识别、视觉模块同理）。
- **在学习路线中的位置**：前置实验11 TFTLCD、实验21 触摸屏、实验27 内存管理、实验29 FATFS、实验30 汉字显示、实验31 图片显示（同一套 text/fonts + FATFS 框架）。是实验34 T9 拼音输入法的前奏（34 里手写作为输入法的姊妹功能出现）。
- **开发板与芯片**：正点原子 MiniSTM32 V4，主芯片 STM32F103RC，72MHz；需配带触摸功能的 ALIENTEK 2.8/3.5/4.3/7/10 寸 MCU TFTLCD 模块。

## 二、实验原理

### 2.1 手写识别的整体思路

先建立一个概念：机器不认识"字"，只认识**数字特征**。典型流程分四步：

1. **采集**：把笔迹变成数据。最简单是"点阵图"（把字框进一个固定大小的方格，比如把字等比缩放进 N×N 的格子里，有笔迹的格子涂黑，得到一张黑白位图，再进一步可以统计每格/每块的"像素密度"作为特征向量）；
2. **归一化**：手写同一个"3"，有人写大、有人写小、有人写歪。归一化就是消除这些无关差异——常见做法：把轨迹的**重心平移到中心**、按外接框**等比缩放到标准尺寸**、必要时**旋转校正**、对笔画**重采样**使点数均匀；
3. **特征比对**：把归一化后的特征与库内训练好的模板/神经网络权重比较，按相似度打分；
4. **输出 Top-N**：得分最高的若干候选字符返回。

本实验使用的 **ATKNCR 库把 2~4 步做成了闭源库**（`ATKNCR_M_V2.0.lib`），[atk_ncr.h](Middlewares/ATKNCR/atk_ncr.h) 头文件只暴露接口，没有公开内部特征尺寸（所谓"48×48 位图"之类的说法在代码里找不到依据，一切以头文件声明为准）。**我们能控制的只有输入**：给它一串按书写顺序排列的屏幕坐标点，它返回字符串。

### 2.2 ATKNCR 库接口（atk_ncr.h 真实声明）

| 函数 | 作用 |
| --- | --- |
| `unsigned char alientek_ncr_init(void)` | 初始化识别器，返回 0 成功 |
| `void alientek_ncr(atk_ncr_point *track, int potnum, int charnum, unsigned char mode, char *result)` | 识别：输入轨迹数组/点数/期望输出候选数/模式，结果写进 result |
| `void alientek_ncr_stop(void)` | 终止识别器（本工程持续使用，未调用） |
| `void *alientek_ncr_malloc(unsigned int size)` 等 | 移植挂钩：M 版库要求用户实现 malloc/free/memset |

- 轨迹点类型：`atk_ncr_point = __PACKED_STRUCT { short x; short y; }`；头文件注明**至少要有 2 个不同的点**才能识别；
- `mode`：1 仅数字、2 仅大写字母、3 仅小写字母、4 混合（全部识别）；
- `result` 缓冲区**至少 charnum+1 字节**；本工程 `charnum=6`，main 里给了 `char sbuf[10]`；
- 库有两个版本：`ATKNCR_M_Vx.x.lib`（用动态内存，必须实现 `alientek_ncr_malloc/free`）和 `ATKNCR_N_Vx.x.lib`（静态，需配置 `ATK_NCR_TRACEBUF1_SIZE/TRACEBUF2_SIZE` 两个内部缓冲宏）。**Keil 工程实际链接的是 M 版**（uvprojx 中 `Middlewares\ATKNCR\ATKNCR_M_V2.0.lib`），这三个挂钩函数由 [atk_ncr.c](Middlewares/ATKNCR/atk_ncr.c) 实现，内部转调本工程内存池：`alientek_ncr_malloc → mymalloc(SRAMIN, size)`。

### 2.3 触摸采点与"笔迹"的形成

1. `tp_dev.init()` 自动识别屏型：电阻屏（XPT2046，软件模拟 SPI，引脚 T_CLK=PC0/T_MOSI=PC3/T_MISO=PC2/T_CS=PC13/T_PEN=PC1）或电容屏（GT9xxx/FT5206，软件 I2C：SCL=PC0、SDA=PC3，GT9xxx 另有 RST=PC13、INT=PC1，与电阻屏引脚复用同一组）；
2. 主循环 `tp_dev.scan(0)` 后读 `tp_dev.x[0]/tp_dev.y[0]`；
3. 只有落在**手写区**内的点才有效（代码判定 `x ∈ [22, lcddev.width-22)`、`y ∈ [117, lcddev.height-7)`，红框画在 `(19,114)~(width-20,height-5)`）；
4. 相邻两个采样点之间用 `lcd_draw_bline(...)` 画**粗线**（Bresenham 直线 + 沿线每隔一段 `lcd_fill_circle` 盖章，`size=2`，模拟笔锋）；
5. 去重后把点存入 `ncr_input_buf[]`（上限 200 点）：代码要求新点与前一点 **x 和 y 都不相等**才记录，避免静止按压堆积重复点。

### 2.4 "写完自动识别"的判定

手离开屏幕后程序不立刻识别，而是等笔"凉"一下：

```
松开触摸 → tcnt 每 10ms 加 1 → tcnt == 40（约400ms没碰屏）
        → 若 pcnt>0：打印总点数 → alientek_ncr(...) → LCD/串口显示结果
        → 清空手写区 → pcnt 归零，等待下一个字
```

这样写一个字中途手指短暂离开 0.4 秒内不会被误断，写完稍等即出结果。

## 三、硬件资源与接线

| 资源 | 引脚 | 代码中的宏/位置 | 说明 |
| --- | --- | --- | --- |
| LED0（红灯） | PA8 | led.h `LED0_GPIO_PORT/PIN` | 约 300ms 翻转，指示运行 |
| LED1（绿灯） | PD2 | led.h | 本工程未使用 |
| KEY0 | PC5 | key.h | 上拉输入；切换识别模式 |
| KEY1 | PA15 | key.h | 进入触摸屏校准（仅电阻屏） |
| KEY_UP | PA0 | key.h（宏名 WK_UP） | 下拉输入；本工程未用 |
| 串口1 | PA9/PA10（CH340） | usart.h | 115200，打印识别结果 |
| LCD 8080 并口 | RD/WR/RS/CS=PC6~PC9，BL=PC10，D0~D15=PB0~PB15 | lcd.h | 显示界面与笔迹 |
| 电阻触摸 XPT2046 | T_CLK=PC0, T_MOSI=PC3, T_MISO=PC2, T_CS=PC13, T_PEN=PC1 | [touch.h](Drivers/BSP/TOUCH/touch.h) | 随 LCD 模块引入 |
| 电容触摸 I2C | SCL=PC0, SDA=PC3；GT9xxx：RST=PC13, INT=PC1 | [ctiic.h](Drivers/BSP/TOUCH/ctiic.h)、gt9xxx.h | 与电阻屏引脚复用 |
| SD 卡（SPI1） | SCK=PA5, MISO=PA6, MOSI=PA7, CS=PA3 | spi_sdcard.h | 挂载为卷 `0:`（本程序仅挂载） |
| W25QXX SPI FLASH | SPI1 共用，CS=PA2 | norflash.h | 存 GBK 字库；官方 readme.txt 明确"连接在 SPI1 上"，与代码一致 |

- 接线：LCD 模块（带触摸屏）插右侧 LCD 接口，注意插反会烧屏；TF 卡可选；串口跳线帽 PA9→RXD、PA10→TXD。
- 时钟：`sys_stm32_clock_init(9)` = 8MHz HSE ×9 = 72MHz，触摸扫描节奏、LED 闪烁节奏都基于此。

## 四、代码结构与调用流程

```
实验33 手写识别实验/
├── User/main.c                界面绘制、采点、粗线笔迹、识别调度
├── Middlewares/ATKNCR/        atk_ncr.h 接口 + atk_ncr.c 移植挂钩 + ATKNCR_M/N_V2.0.lib
├── Drivers/BSP/TOUCH/         touch.c 屏型仲裁；gt9xxx/ft5206/ctiic 驱动
├── Drivers/BSP/LCD/           8080 并口屏驱动（画点/画框/填圆/显示字符）
├── Drivers/BSP/KEY/ LED/ SPI/ SDMMC/ NORFLASH/
├── Middlewares/FATFS/ MALLOC/ TEXT/ USMART/
└── Projects/MDK-ARM/atk_f103.uvprojx
```

`main()` 流程：

1. `sys_stm32_clock_init(9)` → `delay_init(72)` → `usart_init(72,115200)` → `led_init()` → `lcd_init()` → `key_init()` → `tp_dev.init()`（触摸初始化，识别屏型）→ `norflash_init()` → `my_mem_init(SRAMIN)`；
2. `exfuns_init()` → `f_mount(fs[0],"0:",1)`（SD）→ `f_mount(fs[1],"1:",1)`（FLASH）；
3. `alientek_ncr_init()` 初始化识别库；`while(fonts_init())` 检查字库；
4. 画界面：标题、"识别结果:"行、红色手写区边框、"手写区"标签（默认 `mode = 4` 全部识别）；
5. 主循环：`key_scan(0)` → KEY1 校准（电阻屏）/ KEY0 切模式 → `tp_dev.scan(0)` → 采点/画线/存点 → 松开后计时识别 → 串口+LCD 输出。

## 五、核心代码解析

### 5.1 完整的 User/main.c 关键段：采点存轨迹

```c
tp_dev.scan(0);
if (tp_dev.sta & TP_PRES_DOWN)              /* 触屏被按下 */
{
    delay_ms(1);
    tcnt = 0;                               /* 松开时的计数器清空 */
    if ((tp_dev.x[0] < (lcddev.width - 20 - 2) && tp_dev.x[0] >= (20 + 2)) &&
        (tp_dev.y[0] < (lcddev.height - 5 - 2) && tp_dev.y[0] >= (115 + 2)))
    {
        if (lastpos[0] == 0XFFFF)
        {
            lastpos[0] = tp_dev.x[0];
            lastpos[1] = tp_dev.y[0];
        }
        lcd_draw_bline(lastpos[0], lastpos[1], tp_dev.x[0], tp_dev.y[0], 2, BLUE);
        lastpos[0] = tp_dev.x[0];
        lastpos[1] = tp_dev.y[0];
        if (pcnt < 200)                     /* 总点数少于200 */
        {
            if (pcnt)
            {
                if ((ncr_input_buf[pcnt - 1].y != tp_dev.y[0]) &&
                    (ncr_input_buf[pcnt - 1].x != tp_dev.x[0]))  /* x,y不相等 */
                {
                    ncr_input_buf[pcnt].x = tp_dev.x[0];
                    ncr_input_buf[pcnt].y = tp_dev.y[0];
                    pcnt++;
                }
            }
            else { ...首点直接记录... }
        }
    }
}
```

- `lastpos[0]==0XFFFF` 是"这一笔的第一个点"哨兵值：刚按下时没有前点可连线，先把它设为起点；松开时又恢复 `0XFFFF`；
- 入队条件用的是 `&&`（前点 x、y 都与新点不同才记录），斜向抖动会被滤掉一部分；
- 轨迹数组 `ncr_input_buf[200]` 是普通全局数组，**每个字最多 200 点**，一笔超长写不完就丢点（识别率下降），这是"归一化重采样"由库做、但**密度控制要靠你运笔速度**的含义。

### 5.2 识别触发与结果输出

```c
if (tcnt == 40)                     /* 约400ms 无触摸，判定"写完" */
{
    if (pcnt)
    {
        printf("总点数:%d\r\n", pcnt);
        alientek_ncr(ncr_input_buf, pcnt, 6, mode, sbuf);  /* 请求6个候选 */
        printf("识别结果:%s\r\n", sbuf);
        pcnt = 0;
        lcd_show_string(60 + 72, 90, 200, 16, 16, sbuf, BLUE);
    }
    lcd_fill(20, 115, lcddev.width - 20 - 1, lcddev.height - 5 - 1, WHITE);  /* 清手写区 */
}
```

`charnum=6` 表示最多想要 6 个候选，返回的 `sbuf` 是按匹配度排列的字符串（结果显示在"识别结果:"后并同步打印串口）。

### 5.3 移植挂钩（Middlewares/ATKNCR/atk_ncr.c）

```c
void *alientek_ncr_malloc(unsigned int size)
{
    return mymalloc(SRAMIN, size);
}
void alientek_ncr_free(void *ptr)  { myfree(SRAMIN, ptr); }
void alientek_ncr_memset(char *p, char c, unsigned long len)
{
    my_mem_set((uint8_t*)p, (uint8_t)c, (uint32_t)len);
}
```

闭源库不直接调用 C 库 malloc，而是留了这三个"钩子"让宿主工程决定内存从哪来——本工程把它接到自己的 SRAM 内存池（管理上限 40KB、块粒度 32 字节）。这就是"集成第三方库"的标准姿势。

### 5.4 画粗线 lcd_draw_bline（User/main.c）

Bresenham 直线算法按 `delta_x/delta_y` 选主轴逐步推进，**每走一步用 `lcd_fill_circle(row, col, size, color)` 盖一个半径 2 的圆**，圆与圆连起来就成了粗笔迹。相比 1 像素细线，既好看又给识别库提供了更"胖"的笔画。

## 六、编译、下载与运行

1. Keil MDK 打开 `Projects/MDK-ARM/atk_f103.uvprojx`，器件 STM32F103RC，宏 `STM32F103xE`；工程已包含 `ATKNCR_M_V2.0.lib`，勿删；
2. ST-Link/J-Link 下载；
3. 串口助手 115200（XCOM/SSCOM），USART1 排针 PA9/PA10 跳至 RXD/TXD；
4. 若开机显示 `Font Error!`，先用实验30 的字库更新流程把 GBK 字库写入板载 W25QXX。

## 七、实验现象

1. 开机 LCD 显示标题、"识别结果:"、红框手写区和"手写区"提示；LED0 持续闪烁；
2. 在手写区用指尖写一个数字（如 `3`），抬笔约 0.4 秒后：手写区清空，"识别结果:"后出现识别出的字符，串口同步打印"总点数:xx"与"识别结果:x"；
3. 按 KEY0 在四种模式间切换：`仅识别数字 → 仅识别大写字母 → 仅识别小写字母 → 全部识别`（循环），手写区旁有当前模式文字；模式为"仅数字"时写字母基本识别不准，属正常；
4. 电阻屏下按 KEY1 进入触摸屏校准十字流程，校准完自动重启界面（`goto RESTART`）；
5. 写得工整、一笔写完，识别率明显高于潦草连笔。

## 八、注意事项

- **识别库内部是闭源的**：一切特征尺寸、归一化细节都以 `atk_ncr.h` 接口为准；网上流传的"48×48 归一化图"之类的描述无法在本工程代码中证实，不要当成事实背。
- 输入必须**至少 2 个不同点**（头文件注释），单点戳一下不会识别（pcnt<2 时库可能返回空/错）。
- 一笔最多 200 点：写大幅慢笔会被截断；尽量**一笔写完一个字**、速度均匀。
- 电容屏不需要校准，代码里 `KEY1` 校准分支有 `(tp_dev.touchtype & 0X80) == 0` 保护；电阻屏**必须校准**，否则点偏出界，采不到笔迹。
- 电阻/电容触摸与 LCD 共用 PC0~PC3、PC13 等引脚，说明书写错或杜邦线乱接极易引起 I2C/SPI 冲突——用配套 LCD 模块直插即可。
- SD 卡读写过程不要掉电；本工程虽然挂载了 `0:`/`1:` 两个卷，识别功能本身不读 SD 卡，但字库在 SPI FLASH 里，**别把 FLASH 里字库区弄坏**（更新字库要用实验30 流程）。
- `sbuf[10]` 对 `charnum=6` 是够的（≥6+1），如果自己改大 charnum，记得同步加大 sbuf。
- 官方 readme.txt 与代码一致的点特意确认过：本实验 SPI FLASH 在 **SPI1**（31 实验的 readme 写 SPI2 才是与代码不符）。

## 九、总结与延伸

**要点：**

1. "第三方算法库集成"三件套：接口头文件、移植挂钩（malloc/free/memset）、把库文件加进 Keil 工程；
2. 手写识别的通用原理：采集 → 归一化（平移/缩放/重采样）→ 特征比对 → Top-N 候选；本实验输入是**有序轨迹点**而非位图；
3. 笔迹显示 = Bresenham + 圆形印章粗线；写"完"判定 = 松手后 400ms 静默；
4. 识别模式（数字/大写/小写/混合）通过 `mode` 参数限定候选集，能显著提高针对性准确率；
5. 电阻/电容双模触摸驱动的引脚复用关系。

**延伸练习：**

1. 把 `alientek_ncr(..., 6, ...)` 的 `charnum` 改为 1，观察结果变化，体会 Top-N 的意义；
2. 把 `if (tcnt == 40)` 改成 10，实现"写完 100ms 就出结果"的快节奏，但注意运笔停顿会误触发；
3. 结合实验12 USMART：把 `alientek_ncr` 注册进 usmart_config，在串口直接调用识别接口做离线测试。

**衔接**：实验34 T9 拼音输入法同样依赖"触摸 + 字库 + text 显示"框架，可对照学习"输入设备→软件码表→字库显示"这条完整链路。
