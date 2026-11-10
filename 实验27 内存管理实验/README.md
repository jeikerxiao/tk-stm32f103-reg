# 实验27 内存管理实验

> 一句话概括：在 48KB 的片内 SRAM 里自己搭一个"分块式内存池"，用 `mymalloc`/`myfree` 动态申请和释放内存，为后面 FATFS、汉字显示等大工程打基础。

## 一、实验简介

- **实验目的**（官方 readme.txt）：学习内存管理知识，方便大工程应用。通俗地说：前面的实验里所有数组、缓冲区都是编译时就用全局变量写死的；而 FATFS、字库这类功能需要"运行时按需拿一块内存、用完还回去"，这就是动态内存管理。本实验教你不依赖 C 库 `malloc`，自己用寄存器工程实现一套小型内存分配器。
- **在学习路线中的位置**：
  - 前置实验：实验1 LED（了解 GPIO）、实验4 串口（printf 输出）、实验10/11 OLED/TFTLCD（显示）、实验12 USMART（串口远程调函数）。
  - 本实验是"软件层"实验，后面实验28 SD 卡、实验29 FATFS、实验30 汉字显示都直接依赖本实验的 `mymalloc`。
- **开发板与芯片**：正点原子 MiniSTM32 V4 开发板，主芯片 STM32F103RC（Cortex-M3，72MHz，片内 SRAM 共 48KB，Flash 256KB）。

## 二、实验原理

### 2.1 先弄懂几个基本概念

- **寄存器**：CPU 内部一个个可读写的 32 位"控制格子"，配置外设就是往格子里按位填数。本实验几乎不碰外设寄存器，重点是"内存地址"这件事。
- **SRAM**：芯片内部的随机存取存储器，掉电即失，STM32F103RC 有 48KB，地址从 `0x20000000` 开始。你的全局变量、静态变量、栈、堆全都挤在这 48KB 里。
- **栈（stack）**：局部变量和函数调用现场用的空间，从高地址向低地址生长，自动分配自动回收。
- **堆（heap）**：`malloc` 传统意义上的动态内存区。在只有几十 KB RAM 的单片机上，C 库的堆容易出问题：
  1. 分配/释放顺序不当会产生**碎片**——明明总空闲够，却找不到一块连续的空间；
  2. 分配耗时不确定，可能**跑着跑着才失败**；
  3. 堆和栈相向生长，边界不清容易互相踩。

### 2.2 本工程的做法：静态大数组 + 分块管理表

不向链接器要堆，而是直接定义一个**静态全局大数组当"内存池"**，再把池子按固定大小的块（block）切开，配一张**管理表**记录每块空闲还是被占用：

```text
内存池 mem1base[40960]：切成 32 字节一块，共 1280 块
管理表 mem1mapbase[1280]：每个元素对应一块
  值 = 0        该块空闲
  值 = n (非0)  该块属于某次分配，这次分配一共占了 n 个连续块（起始块记录块数）

申请：在管理表里找 n 个连续的 0，把这 n 格都填成 n，返回起始块对应地址
释放：由地址算出起始块号，读出块数 n，把连续 n 格清 0
```

这样做的好处：行为完全可预测、无递归碎片（只会整块整块用）、代码量小；代价是最多浪费不到 32 字节的"块内尾巴"。

### 2.3 关键宏的真实数值（[Middlewares/MALLOC/malloc.h](Middlewares/MALLOC/malloc.h)）

```c
#define SRAMIN              0             /* 内存池编号：内部SRAM */
#define SRAMBANK            1             /* 本工程的F103只有1个内存池 */
#define MT_TYPE             uint16_t      /* 管理表元素类型 */
#define MEM1_BLOCK_SIZE     32            /* 内存块大小 32 字节 */
#define MEM1_MAX_SIZE       40 * 1024     /* 内存池总大小 40KB */
#define MEM1_ALLOC_TABLE_SIZE  MEM1_MAX_SIZE/MEM1_BLOCK_SIZE   /* = 1280 */
```

- **为什么只拿 40KB 而不用满 48KB？** 源码注释写明：要给其它全局变量、数组（串口缓冲、LCD 参数等）和栈预留空间。40KB 池 + 1280×2B 管理表 = 42.5KB，剩下约 5.5KB 给别的东西。
- **为什么管理表元素用 `uint16_t`？** malloc.h 注释说明：外扩 SDRAM 的大工程必须用 `uint32_t`，片内小池用 `uint16_t` 省内存（管理表本身也占 RAM）。
- **内存对齐**：池子定义为 `static __ALIGNED(64) uint8_t mem1base[MEM1_MAX_SIZE];`（64 字节对齐），块大小 32 字节，任何返回地址都是 32 的倍数，天然满足 Cortex-M3 对 4/8 字节数据的对齐要求，不会出现非对齐访问。
- **关于"多池 M0/M1/M2/M3 / memx.h / USE_MALLOC"**：那是正点原子外扩 SDRAM 工程（如 STM32H7、部分 F103 SDRAM 例程）里 memx.h 的多内存池方案。**本寄存器版 F103 工程没有 SDRAM，也没有 memx.h**，只有一个池（编号 `SRAMIN`，即 mem1），管理开关就是上面这几个 `MEM1_*` 宏；没有名为 `USE_MALLOC` 的宏。后续实验里 FATFS 的长文件名缓冲区（实验29 `ff_memalloc`）和字库缓存都从这个池申请。

### 2.4 my_mem_malloc 的查找方向

`my_mem_malloc`（[malloc.c](Middlewares/MALLOC/malloc.c)）是**从管理表尾（高地址）向头（低地址）**扫描连续空闲块的，所以最早申请到的地址往往靠近池子高地址端；反复申请/释放同一尺寸时地址会复用，便于观察现象。

## 三、硬件资源与接线

| 资源 | 引脚 | 代码中的宏/位置 | 说明 |
| --- | --- | --- | --- |
| LED0（红灯 DS0） | PA8 | `LED0_GPIO_PORT/PIN`，[led.h](Drivers/BSP/LED/led.h) | 低电平点亮，程序用 `LED0_TOGGLE()` 指示运行 |
| LED1（绿灯 DS1） | PD2 | `LED1_GPIO_PORT/PIN`，[led.h](Drivers/BSP/LED/led.h) | 本实验未直接使用 |
| KEY0 | PC5 | `KEY0_GPIO_PORT/PIN`，[key.h](Drivers/BSP/KEY/key.h) | 按下=mymalloc 申请 2048 字节 |
| KEY1 | PA15 | `KEY1_GPIO_PORT/PIN`，[key.h](Drivers/BSP/KEY/key.h) | 按下=向已申请内存重写数据 |
| KEY_UP（WK_UP） | PA0 | `WKUP_GPIO_PORT/PIN`，[key.h](Drivers/BSP/KEY/key.h) | 按下=myfree 释放上一次申请 |
| 串口1 | PA9/PA10 | [usart.h](Drivers/SYSTEM/usart/usart.h) | 接板载 CH340，115200bps，供 USMART/printf |
| TFTLCD 模块 | PD0~PD15 等并口 | [lcd.h](Drivers/BSP/LCD/lcd.h) | 正点原子 2.8/3.5/4.3/7/10 寸 MCU 屏，显示结果 |
| 内存池 | 无引脚（RAM 内） | `mem1base[]`，[malloc.c](Middlewares/MALLOC/malloc.c) | 位于片内 SRAM，不占任何 IO |

- 接线：TFTLCD 模块插在开发板左侧 LCD 插座（注意方向，VCC 对着板子标识）；用 USB 线连板载 USB_UART 口即可看到串口输出。
- 时钟：`main()` 里 `sys_stm32_clock_init(9)` 把 8MHz 晶振经 PLL 9 倍频到 **72MHz**（[sys.c](Drivers/SYSTEM/sys/sys.c) 中 `PCLK1 = HCLK/2`，APB1=36MHz、APB2=72MHz）。内存管理本身与主频无关，但延时常量、USMART 都按 72MHz 配置。

## 四、代码结构与调用流程

```text
实验27 内存管理实验/
├── readme.txt                      官方说明
├── User/main.c                     测试主程序（按键申请/写/释放内存并显示）
├── Middlewares/MALLOC/malloc.c/.h  本实验核心：内存池 + mymalloc/myfree
├── Middlewares/USMART/...          串口远程调用（注册了 mymalloc/myfree）
└── Drivers/BSP/{LED,KEY,LCD}/...   基础外设驱动（寄存器版）
```

`main()` 流程：

1. `sys_stm32_clock_init(9)` → `delay_init(72)` → `usart_init(72,115200)` → `usmart_dev.init(72)` → `led_init()` → `lcd_init()` → `key_init()`；
2. `my_mem_init(SRAMIN)`：把管理表全部清 0，置 `memrdy=1`；
3. LCD 打印提示：`KEY0: Malloc`、`KEY1: Write Data`、`KEY_UP: Free`、`SRAMIN USED:`；
4. 主循环：`key_scan(0)` 判键——
   - KEY0：`p = mymalloc(SRAMIN, 2048)`，成功则 `sprintf` 写入 `"Memory Malloc Test%03d"` 并显示内容；
   - KEY1：若 `p` 非空，重新 sprintf 并显示（演示对已申请内存的复用）;
   - KEY_UP：`myfree(SRAMIN, p)`，然后 `p = 0`；
5. 指针 `p` 变化时刷新 `P Addr:0X%08X` 行；`p==0` 时用 `lcd_fill` 清除内容行；
6. 每 20×10ms 调 `my_mem_perused(SRAMIN)` 显示使用率（返回值扩大 10 倍，0~1000 表示 0.0%~100.0%），并翻转 LED0。

## 五、核心代码解析

### 5.1 内存池与管理表的定义（malloc.c）

```c
/* 内存池(64字节对齐) */
static __ALIGNED(64) uint8_t mem1base[MEM1_MAX_SIZE];   /* 内部SRAM内存池 */

/* 内存管理表 */
static MT_TYPE mem1mapbase[MEM1_ALLOC_TABLE_SIZE];      /* 内部SRAM内存池MAP */

struct _m_mallco_dev mallco_dev =
{
    my_mem_init,        /* 内存初始化 */
    my_mem_perused,     /* 内存使用率 */
    mem1base,           /* 内存池 */
    mem1mapbase,        /* 内存管理状态表 */
    0,                  /* 内存管理未就绪 */
};
```

池子是普通静态数组，编译时就占好 40KB RAM；`mallco_dev` 是"内存管理控制器"，把池首地址、表首地址和操作函数打包，将来加第二个池（如外扩 SDRAM）只需把 `SRAMBANK` 改成 2、多备一套数组即可——这正是多池方案 memx.h 的思路。

### 5.2 分配算法 my_mem_malloc（malloc.c，节选）

```c
nmemb = size / memblksize[memx];    /* 获取需要分配的连续内存块数 */
if (size % memblksize[memx]) nmemb++;

for (offset = memtblsize[memx] - 1; offset >= 0; offset--)  /* 从后往前搜索整个内存控制区 */
{
    if (!mallco_dev.memmap[memx][offset]) cmemb++;   /* 连续空内存块数增加 */
    else cmemb = 0;                                  /* 遇到占用块，重新计数 */

    if (cmemb == nmemb)     /* 找到了连续nmemb个空内存块 */
    {
        for (i = 0; i < nmemb; i++)
            mallco_dev.memmap[memx][offset + i] = nmemb;  /* 每格都记录"这次分配共nmemb块" */
        return (offset * memblksize[memx]);              /* 返回偏移地址 */
    }
}
return 0XFFFFFFFF;  /* 未找到符合分配条件的内存块 */
```

逐段解释：

1. `nmemb`：申请 2048 字节 → 2048/32 = 64 块（不足一块也要占一整块，所以向上取整）。
2. 循环变量 `offset` 是**块号**（从 1279 递减到 0），每格管理表独立计数连续空格。
3. 找到后把 n 个格都写成 `nmemb`——这样释放时只需知道**起始块号**，读一格就知道要连续清多少格，不必保存分配长度。
4. 找不到返回 `0XFFFFFFFF`，外层 `mymalloc` 把它翻译成 `NULL`。

对外接口：

```c
void *mymalloc(uint8_t memx, uint32_t size);      /* 返回池内偏移对应的真实地址 */
void  myfree(uint8_t memx, void *ptr);            /* offset = ptr - membase，再清表 */
void *myrealloc(uint8_t memx, void *ptr, uint32_t size); /* 新分配+拷贝旧内容+释放旧块 */
```

用法就是实验现象的三步：`p = mymalloc(SRAMIN, 2048);` → 判 `p != NULL` 再使用 → `myfree(SRAMIN, p);`。

### 5.3 main.c 的申请/释放演示（节选）

```c
case KEY0_PRES:     /* KEY0按下 */
    p = mymalloc(SRAMIN, 2048); /* 申请2K字节,并写入内容,显示在lcd屏幕上面 */
    if (p != NULL)
    {
        sprintf((char *)p, "Memory Malloc Test%03d", i);
        lcd_show_string(30, 230, 209, 16, 16, (char *)p, BLUE);
    }
    break;
case WKUP_PRES:         /* KEY UP按下 */
    myfree(SRAMIN, p);  /* 释放内存 */
    p = 0;              /* 指向空地址 */
    break;
```

注意程序只用一个变量 `p` 保存指针：连按两次 KEY0，第一次的指针被覆盖，就再也没人知道它在哪——所以官方 readme 说"只能释放上一次申请的内存"。这是**用法演示的限制**，不是 myfree 的缺陷；真实工程里谁申请谁保存指针、谁用完谁释放。

## 六、编译、下载与运行

1. 用 Keil MDK 打开 [Projects/MDK-ARM/atk_f103.uvprojx](Projects/MDK-ARM/atk_f103.uvprojx)（器件 STM32F103RC，启动文件 `startup_stm32f103xe.s`，头文件包含路径已配好 `Drivers`/`Middlewares` 各级目录）。
2. F7 Build 编译，生成物在 `Output/` 目录（atk_f103.axf/hex）。
3. 用 ST-Link 或 J-Link 下载（工程烧写设置已选对应调试器）；串口调试用板载 CH340（USB_UART 口），XCOM/SSCOM，**115200bps**。
4. 本实验源码是 GBK 编码，若 Keil/编辑器里注释乱码，请把编辑器编码切换为 GB2312/GBK 查看。

## 七、实验现象

1. 开机后 LCD 显示标题和 `KEY0: Malloc / KEY1: Write Data / KEY_UP: Free` 提示。
2. 按 KEY0：LCD 显示 `P Addr:0X…`（本次分配到的地址，靠近池子高地址端）和 `Memory Malloc Test×××` 内容；使用率上升 5.0%（2048 字节=64 块，64/1280=5%，`my_mem_perused` 返回值 50，按扩大 10 倍显示为 5.0%）。
3. 再按 KEY0：又拿到一块新地址（旧指针丢失），使用率继续上升。
4. 按 KEY1：不新申请，只向 `p` 指向的内存重写带新计数值的内容并刷新显示。
5. 按 KEY_UP：`myfree` 释放最后一次申请，使用率下降，地址行消失、内容行清白。
6. LED0 每 200ms 翻转一次，指示程序在跑。
7. 通过 USMART 发送 `mymalloc(0,1024)` 之类命令也可远程申请内存（工程已把 `mymalloc`/`myfree` 注册进 [usmart_config.c](Middlewares/USMART/usmart_config.c)）。

## 八、注意事项

- **容量口径**：malloc.h 中 `SRAMIN` 注释写"共64KB"，而 `MEM1_MAX_SIZE` 注释又写"F103RC内部SRAM总共48KB"。以芯片手册为准：F103RC 片内 SRAM 是 **48KB**，64KB 那句是作者按大容量型号（如 F103HD/ZE）写的通用说明。池子取 40KB 正是为 48KB 的芯片预留了全局变量和栈空间。
- **申请必须判空**：`mymalloc` 失败返回 `NULL`，直接 `sprintf` 到 NULL 会跑飞。main.c 里都有 `if (p != NULL)`，模仿时别省。
- **释放只认池内指针**：`myfree` 拿 `ptr - membase` 算偏移，若传入非本池的指针（比如某个局部变量地址），偏移越界会返回错误码或误清管理表，绝不允许。
- **sizeof 与块大小**：分配不是 32 的倍数时向上取整，申请 1 字节也占 32 字节；频繁申请小对象时请评估碎片外的"块粒度浪费"。
- **本工程只有一个池**：所有 `mymalloc` 第一个参数都写 `SRAMIN`（值 0）。
- 仅支持 MCU 并口屏，RGB 屏不适用（官方 readme 注意事项）。
- 本实验代码与官方 readme.txt 的现象描述一致，未发现冲突。

## 九、总结与延伸

- 掌握的要点：
  1. 嵌入式自己做内存管理的原因：RAM 小、行为要可预测、避开 C 库堆的碎片与不确定性；
  2. "静态池 + 分块管理表"算法：申请找连续空格、表项记块数、释放按块数清零；
  3. `MEM1_BLOCK_SIZE=32`、`MEM1_MAX_SIZE=40KB`、1280 项 `uint16_t` 管理表等参数怎么互相约束；
  4. `mymalloc`/`myfree`/`myrealloc`/`my_mem_perused` 的用法与判空习惯；
  5. 指针丢失即内存泄漏——谁申请谁负责保存并释放。
- 延伸小练习：
  1. 把申请大小从 2048 改成 33、65，观察使用率变化，验证"按 32 字节块向上取整"；
  2. 仿照 mem1 再加一个 `mem2`（可把 `MEM1_MAX_SIZE` 调小腾出空间），把 `SRAMBANK` 改为 2 并补齐控制器数组，实现双池；
  3. 用 USMART 发送 `read_addr(0x2000xxxx)` 观察管理表被改写时的数值。
- 衔接：实验28 SD 卡把 `mymalloc` 用于扇区缓冲区；实验29 FATFS 的 `exfuns_init` 用它为文件系统工作区申请内存；实验30/31 的字库读取缓存同样出自这个 40KB 池。
