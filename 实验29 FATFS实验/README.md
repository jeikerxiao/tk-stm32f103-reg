# 实验29 FATFS实验

> 一句话概括：把实验28 的"扇区读写"升级为"文件读写"——给 SD 卡和 SPI FLASH 挂上 FatFs（R0.14b）文件系统，用 `f_mount/f_open/f_read/f_write/f_close` 等 API 操作文件，并可通过串口 USMART 远程测试全部文件系统功能。

## 一、实验简介

- **实验目的**（官方 readme.txt）：学习 FATFS，掌握文件系统使用（读/写/删除/创建文件等操作）。
- **通俗解释**：SD 卡本身只认识"第 N 个 512 字节扇区"，不认识"文件"。文件系统就是卡上的一套"记账规则"（引导区 + FAT 分配表 + 目录项 + 数据区），有了它，"a.txt 存放在第 8192~8195 簇"这类信息才能被记录和管理。FatFs 是工程师 ChaN 实现的、专为单片机设计的开源 FAT 文件系统代码，支持 FAT12/FAT16/FAT32/exFAT，代码小、纯 C、无操作系统也能跑。
- **在学习路线中的位置**：前置实验27（内存管理，FatFs 的动态内存全靠它）、实验28（SD 卡扇区驱动）；后续实验30 汉字显示（从 FLASH/SD 读字库）、实验31 图片显示、实验33 手写识别等都要求文件系统就绪。
- **开发板与芯片**：MiniSTM32 V4，STM32F103RC，72MHz。

## 二、实验原理

### 2.1 分层结构：文件 API 如何一路落到扇区

```text
你的应用 / USMART命令(mf_*) / exfuns / fattester
        ↓ f_mount f_open f_read f_write f_close f_getfree ...
FatFs 核心  Middlewares/FATFS/source/ff.c  (版本 R0.14b，见 00history.txt)
        ↓ 只认三个函数: disk_initialize / disk_read / disk_write / disk_ioctl
磁盘适配层  Middlewares/FATFS/source/diskio.c
        ↓ pdrv=0 "0:"          ↓ pdrv=1 "1:"
sd_read_disk/sd_write_disk   norflash_read/norflash_write
（实验28 SPI1+PA3 的 SD 卡）  （SPI1+PA2 的 W25Qxx，虚拟磁盘）
```

- **卷（volume）/盘符**：一个可被独立格式化的逻辑磁盘。本工程 `FF_VOLUMES = 2`："0:" 是 SD 卡，"1:" 是 SPI FLASH 前 4.2MB 划出的虚拟磁盘（同一份代码加一块"U 盘"，演示 FatFs 多卷能力）。
- **簇（cluster）**：文件系统分配空间的最小单位，= 若干个 512B 扇区。`disk_ioctl(GET_BLOCK_SIZE)` 回报擦除块大小 8 扇区，`f_mkfs` 据此对齐。

### 2.2 diskio.c 怎么对接 SD 卡与 SPI FLASH

[diskio.c](Middlewares/FATFS/source/diskio.c) 定义 `#define SD_CARD 0`、`#define EX_FLASH 1`，每个接口函数按 `pdrv` 分派：

- `disk_read`：`SD_CARD` → `sd_read_disk(buff, sector, count)`；出错时循环里先 `sd_init()` 重连再重试（应对卡瞬间不响应）；`EX_FLASH` → 逐扇区 `norflash_read(buff, SPI_FLASH_FATFS_BASE + sector*512, 512)`。
- `disk_write`：同理落到 `sd_write_disk` / `norflash_write`。
- `disk_ioctl`：`GET_SECTOR_SIZE` 回报 512；`GET_BLOCK_SIZE` 回报 8；`GET_SECTOR_COUNT` 对 SD 卡调实验28 的 `sd_get_sector_count()`，对 FLASH 回报 `SPI_FLASH_SECTOR_COUNT`（宏值 `4.2 * 1024 * 2`，即前 4.2MB）。
- FLASH 布局（源码注释原文）：前 4.2M 给 FATFS；4.2M 以后紧跟字库（3 个字库 + UNIGBK.BIN 约 3.09M），7.29M 以后用户自由使用。

### 2.3 关键配置：8.3 短文件名与长文件名（[ffconf.h](Middlewares/FATFS/source/ffconf.h) 的真实开关）

FAT 目录项天生只存"8 个字符名 + 3 个字符扩展名"（即 8.3 格式，如 `README.TXT`）。长文件名（LFN）是后来用一系列特殊目录项"叠"在短名之前的扩展机制。本工程开关：

| 宏 | 值 | 含义 |
| --- | --- | --- |
| `FF_USE_LFN` | **3** | 使能长文件名；0=只支持8.3，1=静态缓冲区，2/3=栈/堆动态分配（3 用 `ff_memalloc`，见 2.4） |
| `FF_MAX_LFN` | 255 | 长文件名最大字符数 |
| `FF_LFN_UNICODE` | 0 | 以 ANSI/OEM 字节串处理文件名（非 UTF-16） |
| `FF_CODE_PAGE` | **936** | 代码页 936 = GBK 简体中文，中文文件名才能正确转换 |
| `FF_VOLUMES` | 2 | "0:" 与 "1:" 两个卷 |
| `FF_MIN_SS`/`FF_MAX_SS` | 512/512 | 固定 512 字节扇区 |
| `FF_FS_EXFAT` | 1 | 支持 exFAT（大卡/大文件） |
| `FF_USE_MKFS` | 1 | 使能 `f_mkfs` 格式化 |
| `FF_USE_LABEL` | 1 | 使能 `f_setlabel/f_getlabel` |
| `FF_USE_STRFUNC` | 1 | 使能 `f_gets/f_puts/f_putc`（fattester 的 mf_gets/mf_puts 用到） |
| `FF_USE_FASTSEEK` | 1 | 使能快速寻址 |
| `FF_FS_READONLY` | 0 | 可写 |
| `FF_FS_REENTRANT` | 0 | 裸机单任务，不加同步锁 |
| `FF_FS_LOCK` | 0 | 不做文件共享锁定 |
| `FF_FS_MINIMIZE` | 0 | 全部 API 开放（含 f_unlink/f_mkdir/f_rename） |

### 2.4 FatFs 的"内存从哪来"——本工程没有 USE_MALLOC 开关

老版 FatFs 有 `_USE_MY_MEMMGR`/`USE_MALLOC` 之类的宏；本工程用的是 R0.14b，对应机制换成了 **`ffsystem.c` 里的 `ff_memalloc/ff_memfree`**。本工程已把它们接到实验27 的内存池：

```c
void *ff_memalloc (UINT size) { return (void*)mymalloc(SRAMIN,size); }
void  ff_memfree (void* mf)   { myfree(SRAMIN,mf); }
```

因为 `FF_USE_LFN=3`（堆分配），每打开一个带长文件名的目录/文件时，FatFs 会向 40KB 的 SRAMIN 池借约 (255×2+2) 字节的 LFN 工作区；用完归还。这就是"实验27 必须先掌握"的原因。同理，卷工作区 `fs[0]/fs[1]`、文件对象 `file` 也是 `exfuns_init()` 里 `mymalloc(SRAMIN, sizeof(FATFS))` 申请来的——**任何 FatFs API 调用前必须先 exfuns_init**，否则 `fs[]` 是空指针。

### 2.5 本工程实际用到的文件 API 与调用位置

| API | 作用 | 工程里的调用点 |
| --- | --- | --- |
| `f_mount(fs[i], "x:", 1)` | 注册卷工作区并立即挂载检查文件系统 | [User/main.c](User/main.c)、fattester 的 `mf_mount` |
| `f_mkfs("1:", 0, 0, FF_MAX_SS)` | 格式化卷 | main.c：`f_mount` 返回 0x0D（FR_NO_FILESYSTEM）时自动格式化 FLASH 盘 |
| `f_setlabel("1:ALIENTEK")` | 设置卷标 | main.c 格式化成功后 |
| `f_open(fp, path, mode)` | 打开/创建文件（`FA_READ`/`FA_WRITE`/`FA_OPEN_ALWAYS` 等按位组合） | fattester `mf_open`（USMART）、exfuns 拷贝、fonts 更新 |
| `f_read(fp, buf, btr, &br)` | 读 btr 字节，实读 br | fattester `mf_read`（每 512 字节一次） |
| `f_write(fp, buf, btw, &bw)` | 写 btw 字节，实写 bw | fattester `mf_write`；exfuns 文件拷贝里 8192 字节分块搬运 |
| `f_sync(fp)` | 把缓存立即刷回磁盘但不关闭文件 | 本工程业务代码未直接调用；`f_close` 内部先隐式执行 f_sync（ff.c） |
| `f_close(fp)` | 同步并释放文件对象 | fattester `mf_close` |
| `f_getfree("x:", &nc, &fs)` | 查总簇/空闲簇 | `exfuns_get_free`（main.c 显示容量）、`mf_showfree` |
| `f_opendir/f_readdir/f_closedir` | 目录枚举 | fattester `mf_opendir/mf_readdir/mf_scan_files` |
| `f_mkdir/f_unlink/f_rename/f_lseek/f_tell` | 目录创建/删除/改名/移动读写指针 | fattester `mf_mkdir/mf_unlink/mf_rename/mf_lseek/mf_tell` |
| `f_gets/f_puts/f_putc` | 字符串文件 IO | fattester `mf_gets/mf_puts/mf_putc`（`FF_USE_STRFUNC=1`） |

## 三、硬件资源与接线

| 资源 | 引脚 | 代码中的宏/位置 | 说明 |
| --- | --- | --- | --- |
| SD 卡（卷 "0:"） | SPI1: PA5/PA6/PA7，CS=**PA3** | [spi_sdcard.h](Drivers/BSP/SDMMC/spi_sdcard.h) | 实验28 同款驱动 |
| SPI FLASH W25Qxx（卷 "1:"） | SPI1: PA5/PA6/PA7，CS=**PA2** | [norflash.h](Drivers/BSP/NORFLASH/norflash.h) | 板载，前 4.2MB 当磁盘 |
| LED0 | PA8 | [led.h](Drivers/BSP/LED/led.h) | 每 200ms 闪烁 |
| KEY0/KEY1/WK_UP | PC5/PA15/PA0 | [key.h](Drivers/BSP/KEY/key.h) | main 仅初始化，未参与逻辑 |
| 串口1（USMART） | PA9/PA10 → CH340 | [usart.h](Drivers/SYSTEM/usart/usart.h) | 115200bps，输入文件系统测试命令 |
| TFTLCD | MCU 并口 | [lcd.h](Drivers/BSP/LCD/lcd.h) | 显示挂载结果与容量 |

- 接线与实验28 完全相同，只是多用到板载 W25Qxx（出厂焊在板上，无需接线）。
- 时钟同上：72MHz、APB1=36MHz（SPI1 高速档 18MHz）。
- **纠正一处官方说明**：readme.txt 写"norflash 连接在 SPI2 上"，但代码（[norflash.c](Drivers/BSP/NORFLASH/norflash.c)）调用的是 `spi1_init()/spi1_read_write_byte()`，CS 为 PA2——与 SD 卡**共用 SPI1**，以代码为准。

## 四、代码结构与调用流程

```text
实验29 FATFS实验/
├── User/main.c                        挂载与容量显示主程序
├── Middlewares/FATFS/source/          ff.c/ff.h/ffconf.h/diskio.c/ffsystem.c  FatFs核心(R0.14b)
├── Middlewares/FATFS/exfuns/          exfuns.c 内存申请/容量查询/文件拷贝
│                                      fattester.c mf_* 测试函数(供USMART调用)
├── Middlewares/MALLOC/                内存池（ff_memalloc 的来源）
├── Middlewares/USMART/                串口命令行
└── Drivers/BSP/{SDMMC,SPI,NORFLASH,LCD,LED,KEY}/
```

`main()` 流程：

1. 基础初始化 + `norflash_init()` + `my_mem_init(SRAMIN)`；
2. `while (sd_init())` 等待 SD 卡就绪；
3. `exfuns_init()`：为 `fs[0]`、`fs[1]` 各申请一个 `FATFS` 工作区，并 `mf_init()` 为 fattester 申请 `file/ftemp/dir/fatbuf`；
4. `f_mount(fs[0], "0:", 1)` 挂载 SD 卡；`f_mount(fs[1], "1:", 1)` 挂载 FLASH 盘；
5. 若 FLASH 盘返回 `0x0D`（无文件系统）→ LCD 提示并 `f_mkfs("1:", 0, 0, FF_MAX_SS)` 格式化 → `f_setlabel("1:ALIENTEK")`；
6. `while (exfuns_get_free("0", &total, &free))` 循环取 SD 容量直到成功；
7. LCD 显示 `FATFS OK!`、`SD Total Size: xxxx MB`、`SD  Free Size: xxxx MB`（total/free 单位 KB，`>>10` 得 MB）；
8. 主循环只剩 LED0 每 200ms 翻转——**其余全部功能通过 USMART 串口命令测试**。

## 五、核心代码解析

### 5.1 main.c 的挂载与自动格式化（节选）

```c
exfuns_init();                  /* 为fatfs相关变量申请内存 */
f_mount(fs[0], "0:", 1);        /* 挂载SD卡 */
res = f_mount(fs[1], "1:", 1);  /* 挂载FLASH. */

if (res == 0X0D)    /* FLASH磁盘,FAT文件系统错误,重新格式化FLASH */
{
    lcd_show_string(30, 130, 200, 16, 16, "Flash Disk Formatting...", RED);
    res = f_mkfs("1:", 0, 0, FF_MAX_SS);   /* 格式化FLASH,1:,盘符;0,使用默认格式化参数 */
    if (res == 0)
        f_setlabel((const TCHAR *)"1:ALIENTEK");    /* 设置Flash磁盘的名字为：ALIENTEK */
}
```

- `f_mount` 第三参 `1` = 立即挂载：FatFs 会马上去读卷的引导扇区，返回码就是文件系统状态码。返回 `0x0D`（FR_NO_FILESYSTEM）说明 FLASH 是"裸"的，工程选择当场格式化，用户无需手动操作。
- `f_mkfs("1:", 0, 0, FF_MAX_SS)`：opt=0、au=0 让 FatFs 自选 FAT 类型和簇大小，工作缓冲区大小用 `FF_MAX_SS`(512)。

### 5.2 exfuns_get_free：容量与剩余空间（exfuns.c，节选）

```c
res = (uint32_t)f_getfree((const TCHAR *)pdrv, (DWORD *)&fre_clust, &fs1);
if (res == 0)
{
    tot_sect = (fs1->n_fatent - 2) * fs1->csize;    /* 得到总扇区数 */
    fre_sect = fre_clust * fs1->csize;              /* 得到空闲扇区数 */
    *total = tot_sect >> 1;     /* 单位为KB */
    *free = fre_sect >> 1;      /* 单位为KB */
}
```

`n_fatent` 是 FAT 表项总数（簇数+2 个保留项），乘每簇扇区数 `csize` 得总扇区数；扇区数 `>>1` 即 KB（512B×2=1KB），main.c 再 `>>10` 显示 MB。

### 5.3 mf_open/mf_write/mf_close：一条 USMART 命令的完整旅程（fattester.c，节选）

```c
uint8_t mf_open(uint8_t *path, uint8_t mode)
{
    res = f_open(fattester.file, (const TCHAR *)path, mode);    /* 打开文件 */
    return res;
}
uint8_t mf_write(uint8_t *pdata, uint16_t len)
{
    res = f_write(fattester.file, pdata, len, &bw);
    ...
}
uint8_t mf_close(void) { f_close(fattester.file); return 0; }
```

在串口（回车结尾）输入例如：

```text
mf_open("0:/TEST.TXT",FA_CREATE_ALWAYS|FA_WRITE)
mf_write("Hello FATFS",11)
mf_close()
```

执行链：usmart 解析 → `mf_open` → `f_open` → FatFs 查目录、分配簇 → `disk_write` → `norflash_write` 或 `sd_write_disk` → SPI1 寄存器。所有 `mf_*` 都已注册进 [usmart_config.c](Middlewares/USMART/usmart_config.c)（`mf_mount/mf_open/mf_close/mf_read/mf_write/mf_opendir/mf_readdir/mf_scan_files/mf_showfree/mf_lseek/mf_tell/mf_size/mf_mkdir/mf_fmkfs/mf_unlink/mf_rename/mf_getlabel/mf_setlabel/mf_gets/mf_putc/mf_puts` 等），把 `USMART_USE_WRFUNS` 打开时还能读写任意地址。

### 5.4 长文件名缓冲区的申请

`FF_USE_LFN=3` 时，FatFs 在需要时调用 ffsystem.c 的 `ff_memalloc(FF_MAX_LFN*2+2)`；本工程把它接到 `mymalloc(SRAMIN,…)`。若 40KB 池被耗尽（USMART 连续开很多目录、或实验27 里忘了 `myfree`），`f_open` 会返回 `FR_NOT_ENOUGH_CORE`(17，见 ff.h 枚举)——这是新手最常撞的"FatFs 报错"之一，先查内存池而不是查卡。

## 六、编译、下载与运行

1. Keil MDK 打开 [Projects/MDK-ARM/atk_f103.uvprojx](Projects/MDK-ARM/atk_f103.uvprojx)（器件 STM32F103RC；工程分组 Middlewares 下含 FATFS/MALLOC/USMART）。
2. 编译输出在 `Output/`；ST-Link/J-Link 下载。
3. 串口助手 XCOM/SSCOM 连 CH340，115200bps，8N1，字符串输入以回车换行结束（USMART 要求）。
4. 建议：先用电脑把 TF 卡格式化成 FAT32/exFAT 再插入，可观察"挂载已存在文件系统"的路径；SPI FLASH 盘第一次运行会自动格式化。

## 七、实验现象

1. 开机 LCD 依次显示标题、（等待卡时）`SD Card Error!` 或 FLASH 盘格式化提示 `Flash Disk Formatting... → Format Finish`。
2. 正常后 LCD 显示 `FATFS OK!`、`SD Total Size: xxxx MB`、`SD  Free Size: xxxx MB`，数值与电脑上看卡的结果一致。
3. 串口发送 `mf_showfree("0:")` 打印剩余簇数换算信息；发送 `mf_scan_files("0:")` 枚举根目录文件并逐个打印名字/大小/属性。
4. 按 5.3 的三步可新建 TEST.TXT 写入内容，再用 `mf_open("0:/TEST.TXT",FA_READ)` + `mf_read(11)` 读回，串口打印 `Read fattester.file data is: Hello FATFS`。
5. `mf_mkdir("0:/NEWDIR")`、`mf_unlink("0:/TEST.TXT")`、`mf_rename(...)` 逐条试验，SD 卡插回电脑核对结果即可确认 API 生效。
6. LED0 每 200ms 闪烁表示主循环在跑。

## 八、注意事项

- **必须先 `exfuns_init()` 再调任何 f_* 函数**；`fs[]/file/ftemp/fatbuf` 都是指针，忘了申请就是空指针 HardFault。
- **USMART 引号与逗号**：参数是 C 风格字符串，路径要带卷号（"0:"/"1:"），回车结尾；打错会回 `Wrong Sequence`。
- 代码与官方 readme.txt 的不一致：readme 称 norflash 接 **SPI2**，实际代码与 SD 卡共用 **SPI1**（CS=PA2/PA3），以代码为准。
- `f_mount` 返回值 ≠0 时文件操作全部会失败，常见码（ff.h 枚举）：`FR_NO_FILESYSTEM`(13，即 0x0D)、`FR_NOT_READY`(3)、`FR_NO_FILE`(4)、`FR_NOT_ENOUGH_CORE`(17)。
- **读写期间严禁拔卡/掉电**：FatFs 有写缓存，突然掉电可能把 FAT 表写坏，整卡文件丢失；删除/新建/改名后尽量安全弹出（本工程无同步按钮，可依赖 `f_close` 内的隐式 `f_sync`，长期写大量数据建议周期性 `f_sync`）。
- 长文件名依赖 `FF_CODE_PAGE=936`：中文文件名在 GBK 环境创建，改成 ANSI 英文环境可能显示乱码；8.3 短名则始终可用（`mf_show_sfn` 风格验证可用 mf_readdir 观察）。
- FLASH 盘只有 4.2MB 且簇小，适合存放配置文件，不适合大文件；`SPI_FLASH_SECTOR_COUNT` 宏写作 `4.2 * 1024 * 2`（浮点乘再隐式截断），移植时注意。
- 本源码为 GBK 编码，编辑器选 UTF-8 会注释乱码。

## 九、总结与延伸

- 掌握要点：
  1. 文件系统 = 扇区之上的记账规则；簇/FAT/目录项三件套；
  2. FatFs 分层：f_* API → diskio → `sd_read_disk`/`norflash_read`，移植新设备只需改 diskio.c；
  3. `f_mount/f_mkfs/f_open/f_read/f_write/f_close/f_getfree` 的真实用法与返回码；
  4. ffconf.h 的 `FF_USE_LFN=3 + FF_CODE_PAGE=936` 让 GBK 中文长文件名可用，LFN 缓冲区与所有工作区都来自实验27 的 mymalloc 池；
  5. 双卷（"0:" SD、"1:" SPI FLASH）并存的工作方式。
- 延伸小练习：
  1. 用 `mf_puts("0:/ABC.TXT")` 系列字符串函数写一首短诗，再 `mf_gets` 逐行读回；
  2. 把 main.c 增加开机自动创建 `0:/BOOTLOG.TXT` 并追加一行 ASCII 文本（练 `FA_OPEN_ALWAYS|FA_WRITE` + `f_lseek(fp, f_size(fp))` 追加写法）；
  3. 故意在 USMART 里连续 `mf_opendir` 不 `mf_closedir`，观察内存使用率与 `FR_NOT_ENOUGH_CORE`。
- 衔接：实验30 汉字显示会用本实验的文件系统从 SD 卡 `/SYSTEM/FONT/` 目录读取字库文件并搬进 W25Qxx；实验31 图片显示则用 FatFs + `exfuns` 拷贝/识别 .jpg/.bmp。
