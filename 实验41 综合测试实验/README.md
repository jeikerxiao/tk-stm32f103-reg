# 实验41 综合测试实验

> 一句话概括：把前面 40 个实验学到的外设驱动全部组装起来，跑一个像智能手机一样可以滑动、点按图标操作的"迷你操作系统"，用来对整本书的学习成果做综合验收。

## 一、实验简介

- **实验目的**（官方 [readme.txt](readme.txt)）：体验 STM32 的强大。通俗地说，前面的实验每个只练一个外设（点一个 LED、读一个按键、驱动一块屏……），而这个实验把 LCD、触摸屏、按键、LED、RTC、SPI FLASH、SD 卡、FATFS、字库、图片解码、红外、无线、USB、UCOSII 实时操作系统等全部装进同一个工程，做出一个带主界面（SPB 滑动动画）、15 个功能应用的"板上小系统"。它不教新寄存器知识，教的是**如何把模块拼成一个完整产品**。
- **在学习路线中的位置**：这是全工程最后一个实验。前置实验是 11（TFTLCD）、12（USMART）、13（RTC）、15（DMA）、16（ADC）、17（内部温度传感器）、19（IIC）、20（SPI）、21（触摸屏）、22（红外遥控）、23（DS18B20）、25（无线通信）、27（内存管理）、28（SD 卡）、29（FATFS）、30（汉字显示）、31（图片显示）、33（手写识别）、34（T9 输入法）、35（串口 IAP）、36（USB 读卡器）、38~40（UCOSII）。做完本实验后，课程（寄存器版）全部结束。
- **实验平台**：正点原子 MiniSTM32 V4 开发板，主芯片 STM32F103RCT6（Cortex-M3，主频 72MHz，官方标称 256KB Flash / 48KB SRAM，本实验实际"借用"到 384KB Flash，见第六章）。
- **本实验包含的功能项**（即主界面上的图标，双击进入，见第四章代码依据）：
  电子图书、数码相框、时钟、系统设置、记事本、运行器、手写画笔、USB 连接、网络通信、无线传书、按键测试、LED 测试、科学计算器、应用中心（内含红外遥控）、电压表。

## 二、实验原理

### 2.1 从"裸机主循环"到"操作系统 + 应用框架"

前面实验的 `main()` 都是"初始化 → while(1) 轮询"。本工程引入了 UCOSII 实时操作系统（可以在 [Drivers/SYSTEM/sys/sys.h](Drivers/SYSTEM/sys/sys.h) 第 39 行看到 `#define SYS_SUPPORT_OS 1`，这个宏为 1 时，`delay.c`/`usart.c` 等底层代码会走"支持 OS"的分支，比如 `delay_ms()` 内部改用 `OSTimeDly()`，让出 CPU 而不是死等）。

工程共有 4 个任务（优先级数字越小越高，节拍 `OS_TICKS_PER_SEC = 1000`，即 1ms 一次，见 [Middlewares/UCOSII/uC-OS2/Cfg/Template/os_cfg.h](Middlewares/UCOSII/uC-OS2/Cfg/Template/os_cfg.h)）：

| 任务 | 优先级 | 干什么 |
| --- | --- | --- |
| `start_task` | 10 | 创建其余 3 个任务后自我挂起 |
| `main_task` | 6 | 主界面（SPB）显示、图标双击检测、功能分发 |
| `usart_task` | 7 | 闹钟扫描 + 每秒串口打印内存占用率 |
| `watch_task` | 3 | LED0 心跳闪烁、KEY0 全局退出、SD 卡热插拔检测、停止 GIF 解码 |

**寄存器视角**：UCOSII 的任务切换靠 Cortex-M3 的 SysTick 定时器每 1ms 产生一次中断。`start_task` 里的 `OS_CPU_SysTickInit(cnts)` 计算重载值：`cnts = 72MHz / 1000 = 72000`，即 SysTick->LOAD 装 72000-1，正好 1ms 溢出一次（第 563 行 `(72 * 1000000) / OS_TICKS_PER_SEC`）。这与实验 7~9 学的定时器"ARR = 溢出时间 × 计数频率"是同一个公式。

### 2.2 SPB 手机式主界面为什么能"滑"得动

LCD（实验 11）是 8080 并口屏：16 根数据线 PB0~PB15 + 读写控制线，逐像素写屏在 72MHz 单片机上并不快。若滑动时每一帧都重新画图，会明显卡顿。本工程的思路（[Drivers/BSP/SPBLCD/spblcd.c](Drivers/BSP/SPBLCD/spblcd.c)、[User/APP/spb.c](User/APP/spb.c)）：

1. 首次运行时，把整幅（含所有图标、背景）的每一列像素**预先"抄写"到 SPI FLASH**（实验 20 的 W25Q64/BY25Q64，存于 7.3MB 起的 `SPILCD_BASE` 区域，见 [spblcd.h](Drivers/BSP/SPBLCD/spblcd.h)）；
2. 滑动时只需从 SPI FLASH 按偏移读出当前帧数据，配合 DMA（实验 15）快速搬运刷屏；
3. 触摸坐标由 [Drivers/BSP/TOUCH/touch.c](Drivers/BSP/TOUCH/touch.c)（电阻屏 XPT2046）或 gt9xxx/ft5206（电容屏，实验 21）提供。

所以官方 readme 说"像手机一样操作"，本质是**预渲染帧 + 高速搬帧**，而不是每帧实时重画。

### 2.3 存储体系：一张 SD 卡 + 一片 SPI FLASH 分工

| 存储 | FATFS 盘符 | 内容 | 用到的知识（实验号） |
| --- | --- | --- | --- |
| SD 卡（SPI 模式） | `0:` | 用户图片、电子书、`/SYSTEM` 系统文件源包 | 28、29 |
| SPI FLASH W25Q64 | `1:` | 字库（4.2MB 起）、SPB 帧缓存（7.3MB 起）、运行器 BIN 源（7.9MB 起）、FATFS 磁盘 | 20、29、30 |
| 24C02 EEPROM（IIC） | 无盘符 | 系统参数、闹钟参数（`SYSTEM_PARA_SAVE_BASE = 100`，[common.h](User/APP/common.h)） | 19 |

盘符映射写死在 [Middlewares/FATFS/source/diskio.c](Middlewares/FATFS/source/diskio.c)：`SD_CARD = 0`、`EX_FLASH = 1`。字库不放在 SD 卡而放 SPI FLASH，是为了让汉字显示（实验 30 的 [fonts.c](Middlewares/TEXT/fonts.c)/[text.c](Middlewares/TEXT/text.c)）随开随用；首次开机 FLASH 里没有字库/文件系统时，会自动从 SD 卡的 `/SYSTEM/FONT/GBK12.FON、GBK16.FON、GBK24.FON、UNIGBK.BIN` 更新（代码见 `fonts_update_font()` 调用与 [fonts.c](Middlewares/TEXT/fonts.c) 的 `FONT_GBK_PATH`）。

### 2.4 时钟、串口等基础参数

- `sys_stm32_clock_init(9)`：外部 8MHz 晶振 ×9 倍频 = 72MHz（`RCC->CFGR` 的 PLL 设置，实验 1 就配置过）。
- `usart_init(72, 115200)`： USART1，波特率计算 `USARTDIV = 72MHz/(16×115200) ≈ 39.06`，与实验 4 相同；调试时 PC 串口助手需设 **115200**。
- LED0 心跳：`watch_task` 每 10ms 计数一次，`t == 119` 时点亮 LED0、`t == 4` 时熄灭，注释原文为"2.5 秒钟左右亮一次"，等价于裸机的"跑马灯式心跳"（实验 1）。

## 三、硬件资源与接线

### 3.1 片上资源

| 资源 | 关键宏/寄存器位置 | 说明 |
| --- | --- | --- |
| 系统时钟 72MHz | `sys_stm32_clock_init(9)`，[sys.c](Drivers/SYSTEM/sys/sys.c) | PLL = 8MHz × 9 |
| SysTick（OS 节拍 1ms） | `OS_CPU_SysTickInit(72000)`，[main.c](User/main.c) | UCOSII 心跳 |
| USART1 | PA9 TX / PA10 RX，[usart.h](Drivers/SYSTEM/usart/usart.h) | 115200，printf + USMART（实验 12） |

### 3.2 外接模块与引脚

| 资源 | 引脚 | 代码中的宏/位置 | 说明 |
| --- | --- | --- | --- |
| LED0 | PA8 | `LED0_GPIO_PORT/PIN`，[led.h](Drivers/BSP/LED/led.h) | 心跳指示 |
| LED1 | PD2 | `LED1_GPIO_PORT/PIN`，[led.h](Drivers/BSP/LED/led.h) | 错误提示/启动指示 |
| KEY0 | PC5 | [key.h](Drivers/BSP/KEY/key.h) | **全局退出键**，任何应用里按一下即返回主界面 |
| KEY1 | PA15 | [key.h](Drivers/BSP/KEY/key.h) | 确认更新字库 |
| WK_UP | PA0 | [key.h](Drivers/BSP/KEY/key.h) | 开机按住 = 擦除 SPI FLASH 系统区 |
| TFTLCD 并口 | 数据 PB0~PB15；RD=PC6、WR=PC7、RS=PC8、CS=PC9、BL=PC10 | [lcd.h](Drivers/BSP/LCD/lcd.h) | 支持 2.8/3.5/4.3 寸 MCU 屏 |
| 电阻触摸屏 | CLK=PC0、CS=PC13、DIN=PC3、DOUT=PC2、PEN=PC1 | [touch.h](Drivers/BSP/TOUCH/touch.h) | XPT2046，软件 SPI |
| 24C02 / 电容触摸屏 | SCL=PC12、SDA=PC11 | [myiic.h](Drivers/BSP/IIC/myiic.h) | 软件 IIC 共用 |
| SPI1 总线 | SCK=PA5、MISO=PA6、MOSI=PA7 | [spi.h](Drivers/BSP/SPI/spi.h) | 挂多个从设备，靠片选区分 |
| SPI FLASH W25Q64 | CS=PA2 | [norflash.h](Drivers/BSP/NORFLASH/norflash.h) | SPI1 从机 |
| SD 卡（SPI 模式） | CS=PA3 | [spi_sdcard.h](Drivers/BSP/SDMMC/spi_sdcard.h) | SPI1 从机，需自备 TF 卡 |
| NRF24L01 无线 | CE=PA4、CSN=PC4、IRQ=PA1 | [nrf24l01.h](Drivers/BSP/NRF24L01/nrf24l01.h) | SPI1 从机，需自备模块 ×2 |
| ATK-MO395Q 网络 | CS=PC4、RST=PA4、INT=PA1 | [ch395q.h](Drivers/BSP/CH395Q/ch395q.h) | SPI1 从机，需自备模块 |
| 红外接收头 | PA1 + TIM5_CH2 输入捕获 | [remote.h](Drivers/BSP/REMOTE/remote.h) | 应用中心→红外遥控（实验 22） |
| DS18B20 | PA1 单总线 | [ds18b20.h](Drivers/BSP/DS18B20/ds18b20.h) | 时钟应用显示温度（实验 23） |
| 电压表 ADC | PC4 = ADC1 通道 14，DMA1_Channel1 | [adc.h](Drivers/BSP/ADC/adc.h) | 需把 P1 排针的 RV1 电位器连到 ADC（见 vmeterplay.c 提示语） |
| USB（Mini USB 口） | PA11 DM / PA12 DP（硬件固定） | [usbd_conf.c](Middlewares/USB/USB_APP/usbd_conf.c) | MSC 大容量存储，把 SPI FLASH 前 12MB 当 U 盘（实验 36） |

**接线注意**：NRF24L01 与 ATK-MO395Q 的片选/复位/中断引脚**完全相同**（PC4/PA4/PA1，共用同一个插座），两者**不能同时插**；红外接收头与 DS18B20 也都占 PA1。测试哪个功能就插哪个模块，方向别反（模块插反会烧模块）。

## 四、代码结构与调用流程

### 4.1 目录树（只列与本实验相关的部分）

```text
实验41 综合测试实验/
├── User/
│   ├── main.c               系统初始化自检 + 4个UCOSII任务 + HardFault处理
│   └── APP/                 各"应用程序"（功能项）
│       ├── spb.c/.h             SPB主界面：图标布局、滑动检测、双击判定
│       ├── common.c/.h          通用文件浏览、系统文件检查/更新、字库大字加载
│       ├── ebook.c              电子图书        picviewer.c   数码相框
│       ├── calendar.c           时钟/闹钟/温度   settings.c    系统设置(13项菜单)
│       ├── notepad.c            记事本(T9输入)   paint.c       手写画笔
│       ├── calculator.c         科学计算器       vmeterplay.c  电压表(ADC)
│       ├── exeplay.c            运行器(执行SRAM BIN) usbplay.c  USB连接
│       ├── netplay.c            网络通信(CH395Q)  wirelessplay.c 无线传书(NRF24L01)
│       ├── keyplay.c/ledplay.c  按键/LED测试
│       └── appplay.c + appplay_remote.c  应用中心 + 红外遥控
├── Drivers/
│   ├── BSP/                 LED/KEY/LCD/TOUCH/RTC/ADC/DAC…SPI/IIC/NORFLASH/SDMMC/
│   │                        NRF24L01/CH395Q/REMOTE/DS18B20/DMA/TIMER/SPBLCD 驱动
│   ├── SYSTEM/              sys(时钟/GPIO)、delay、usart
│   └── CMSIS/               头文件 + startup_stm32f103xe.s 启动文件
└── Middlewares/
    ├── GUI/                 window/button/listbox/edit/progressbar 等控件库
    ├── FATFS/ + MALLOC/     文件系统(实验29) + 内存管理(实验27)
    ├── TEXT/ PICTURE/ T9INPUT/  汉字(30)/图片(31, BMP/GIF/JPEG)/T9输入法(34)
    ├── USB/                 MSC设备栈(36)
    ├── UCOSII/              实时系统内核(38~40)
    └── USMART/ ATKNCR/      串口调试(12) / 手写识别库(33)
```

### 4.2 开机自检流程（`system_init()`，[main.c](User/main.c) 第 240~532 行）

上电后屏幕逐行显示检测结果，任何一项致命失败都会进入红字循环闪烁 + LED0 快闪的 `system_error_show()`：

1. `sys_stm32_clock_init(9)` → `usart_init(72,115200)` → `norflash_init()` → `exeplay_app_check()`（若 RTC 备份寄存器 BKP1 = 0x5050，说明上次在"运行器"里点了执行，把 SPI FLASH 7.9MB 处的 BIN 拷到 `0x20001000` 并跳转执行，思路同实验 35 串口 IAP）
2. `delay_init(72)`、`usmart_dev.init(72)`、`lcd_init()`、`led_init()`、`key_init()`、`at24cxx_init()`、`adc_temperature_init()`、`my_mem_init(SRAMIN)`（内部 SRAM 内存池，实验 27）
3. `tp_dev.init()`、`gui_init()`、`piclib_init()`、`slcd_dma_init()`、`exfuns_init()`
4. 显示版权/版本/LCD ID → 检测 SPI FLASH ID（须为 W25Q64/BY25Q64/NM25Q64 之一）→ 若开机按住了 WK_UP，则确认后擦除系统文件区（约 195 秒）并**从第 4 步重新初始化**（`goto REINIT`）
5. `rtc_init()` 检测 RTC（实验 13）→ `f_mount(fs[0],"0:")`、`f_mount(fs[1],"1:")` 挂载 SD 与 FLASH 文件系统 → 检测 SD 容量；若 FLASH 无文件系统（返回 0X0D）则 `f_mkfs("1:")` 并设卷标 ALIENTEK
6. `at24cxx_check()` 检测 24C02 → `fonts_init()` 检测字库，缺失则从 SD 卡 `0:` 或 FLASH `1:` 的 `/SYSTEM/FONT/` 自动更新
7. `app_system_file_check("1")` 检查 SPB 背景图/图标等系统文件是否齐全，不齐全就从 SD 卡（或 USB U盘）`app_system_update()` 拷入
8. `tp_init()` 触摸屏检测；电阻屏若按 KEY0 会进 `tp_adjust()` 校准并重新初始化
9. `app_system_parameter_init()` 加载系统参数（语言、闹铃等，存 24C02）→ LED1 闪一下提示启动完成

### 4.3 任务启动与主循环

```text
main() → system_init() → OSInit() → 创建start_task(prio10) → OSStart()
start_task: 配SysTick(72000) → OSStatInit() → 临界区内创建 main_task(6)/usart_task(7)/watch_task(3) → 挂起自己
main_task: spb_init(0) → spb_load_mui() → while(1){
    selx = spb_move_chk();          /* 滑动翻页; 同一个图标按第2次才返回图标号(双击) */
    switch(selx){ case 0: ebook_play(); ... case SPB_ICOS_NUM+2: vmeter_play(); }
    spb_load_mui();                 /* 应用退出后回主界面 */
}
```

`selx` 图标编号与 [main.c](User/main.c) 第 624~689 行 `switch` 一一对应（0 电子图书、1 数码相框、2 时钟、3 系统设置、4 记事本、5 运行器、6 手写画笔、7 USB 连接、8 网络通信、9 无线传书、10 按键测试、11 LED 测试、12 计算器、13 应用中心、14 电压表）——图标名字表见 [spb.c](User/APP/spb.c) 的 `icos_name_tbl`/`micos_name_tbl`。主界面共 2 页 12 个滑动图标 + 3 个固定大图标（`SPB_PAGE_NUM = 2`、`SPB_ICOS_NUM = 12`，[spb.h](User/APP/spb.h)）。

每个应用内部都轮询全局标志 `system_task_return`：优先级更高的 `watch_task` 检测到 KEY0 按下就把它置 1，应用层看到后立刻 `break` 回主界面——这就是"KEY0 = 万能退出键"的实现。

## 五、核心代码解析

### 5.1 入口：先裸机自检，再开操作系统

[main.c](User/main.c) 第 534~548 行：

```c
int main(void)
{
    system_init();  /* 系统初始化 */
    OSInit();
    OSTaskCreateExt((void(*)(void *) )start_task,               /* 任务函数 */
                    (void *          )0,                        /* 传递给任务函数的参数 */
                    (OS_STK *        )&START_TASK_STK[START_STK_SIZE - 1], /* 任务堆栈栈顶 */
                    (INT8U          )START_TASK_PRIO,           /* 任务优先级 */
                    ...
    OSStart();
}
```

要点：所有硬件检测、字库更新、文件系统挂载都发生在 `OSStart()` **之前**——此时还是裸机环境，顺序执行、没有任务抢占，逻辑最简单也最不容易出岔子；启动成功后才把 CPU 交给 UCOSII（实验 38 的套路：`OSInit → OSTaskCreate → OSStart`）。注意堆栈数组用 `__ALIGNED_8` 定义（如 `OS_STK START_TASK_STK[64]`），因为 Cortex-M4 开浮点存储（选项里带 `OS_TASK_OPT_SAVE_FP`）要求 8 字节对齐。

### 5.2 开机检测 SPI FLASH 与"按住 WK_UP 擦除系统"

[main.c](User/main.c) 第 332~350 行（节选）：

```c
    /* SPI FLASH检测 */
    if (norflash_read_id() != BY25Q64 && norflash_read_id() != W25Q64 && norflash_read_id() != NM25Q64)  /* 读取QSPI FLASH ID */
    {
        system_error_show(5, ypos + fsize * j++, "SPI Flash Error!!", fsize);
    }
    else temp = 8 * 1024;  /* 8M字节大小 */
    ...
    res = key_scan(1);

    if (res == WKUP_PRES)   /* 启动的时候，按下WKUP按键，则擦除SPI FLASH字库和文件系统区域 */
    {
        res = system_files_erase(5, ypos + fsize * j, fsize);
        if (res)goto REINIT;
    }
```

逐段解释：
- `norflash_read_id()` 通过 SPI1 读 JEDEC ID（实验 20 的方法），三种 8M 字节兼容片任选其一，ID 不对就死循环报错（红字 + LED0 400ms 闪烁），因为整个系统的字库、图标、文件系统都在 FLASH 里，缺它不可。
- 开机瞬间按住 WK_UP（PA0）=  recovery 模式：`system_files_erase()` 里循环 `norflash_erase_sector(i)`（扇区 200~4095），源码注释估算"擦除一个扇区约 50ms，3895 个，大概要 195 秒"，屏幕上有进度条（`fonts_progress_show`）。擦完 `goto REINIT` 重新走一遍自检，此时会触发 `f_mkfs("1:")` 重建文件系统和从 SD 卡重灌字库/系统文件。**系统玩坏了就按住 WK_UP 上电重来**，这是本实验最重要的自救手段。

### 5.3 主任务：双击判定 + 功能分发

[main.c](User/main.c) 第 618~689 行（节选）：

```c
    while (1)
    {
        selx = spb_move_chk();
        system_task_return = 0; /* 清退出标志 */

        switch (selx)   /* 发生了双击事件 */
        {
            case 0:
                ebook_play();
                break;  /* 电子图书 */
            ...
            case 8:
                sd_check_en = 0;    /* 关闭SD卡检测 */
                net_play();
                sd_check_en = 1;    /* 打开SD卡检测 */
                break;  /* 网络通信 */
            ...
            case SPB_ICOS_NUM+2:
                vmeter_play();
                break;  /* 电压表 */
        }

        if (selx != 0XFF)
        {
            spb_load_mui();     /* 显示主界面 */
        }
```

- `spb_move_chk()`（[spb.c](User/APP/spb.c) 第 634 行起）先 `tp_dev.scan(0)` 读触摸坐标：按下点移动超过 `SPB_MOVE_ACT(50)` 像素就翻页滑动；同一图标**第二次**按下才返回图标号。这就是 readme.txt 提示"开发板图标是点 2 次才进入"的原因——第一次点击只是选中画框，第二次才执行。
- 进入网络/无线功能前把 `sd_check_en = 0`：因为 CH395Q、NRF24L01 与 SD 卡共用 SPI1 总线（不同片选），watch_task 后台若同时去查 SD 卡会打断通信，所以临时关掉热插拔检测。
- 任何应用返回后 `spb_load_mui()` 重画主界面（帧数据直接从 SPI FLASH 搬，很快）。

### 5.4 监视任务：心跳、退出键、SD 卡热插拔

[main.c](User/main.c) 第 758~818 行（节选）：

```c
    while (1)
    {
        ...
        if (ledplay_ds0_sta == 0)   /* 仅当ledplay_ds0_sta等于0的时候,正常熄灭LED0 */
        {
            if (t == 4)LED0(1);     /* 亮100ms左右 */
            if (t == 119)
            {
                LED0(0);            /* 2.5秒钟左右亮一次 */
                t = 0;
            }
        }
        t++;

        if(key0_scan())      /* KEY0按下了 */
        {
            system_task_return = 1;
            ...
        }

        if ((t % 60) == 0)  /* 900ms左右检测1次 */
        {
            if(sd_check_en) /* SD卡检测使能了? */
            {
                OS_ENTER_CRITICAL();        /* 进入临界区(无法被中断打断) */
                res = sd_get_status();      /* 查询SD卡状态 */
                OS_EXIT_CRITICAL();         /* 退出临界区(可以被中断打断) */

                if (res == 0XFF)
                {
                    gui_phy.memdevflag &= ~(1 << 0);    /* 标记SD卡不在位 */
                    ...
                    sd_init();          /* 重新检测SD卡 */
                    ...
                }
                else if ((gui_phy.memdevflag & (1 << 0)) == 0)     /* SD不在位? */
                {
                    f_mount(fs[0], "0:", 1);        /* 重新挂载sd卡 */
                    gui_phy.memdevflag |= 1 << 0;   /* 标记SD卡在位了 */
                }
            }
        }

        delay_ms(10);
    }
```

- 该任务优先级 3，比界面任务高，所以无论界面多忙，LED0 心跳和 KEY0 退出都保证响应——这就是"RTOS 优先级抢占"的实际用途（实验 38）。
- `LED0(0)` 点亮、`LED0(1)` 熄灭（低电平点亮，与 led.h 一致）；`ledplay_ds0_sta` 是运行"LED 测试"应用时让心跳让位的标志。
- SD 卡检测包在 `OS_ENTER_CRITICAL()/OS_EXIT_CRITICAL()` 临界区里（开关中断），防止读卡中途被别的中断打断；插卡后重新 `f_mount(fs[0], "0:", 1)` 即可继续用文件类应用——**支持运行中拔插 TF 卡**。
- 任务末尾还有闹钟响铃处理和 GIF 解码急停（readme 里"数码相框"播 GIF 时按 KEY0 立即停止的实现处）。

## 六、编译、下载与运行

1. 用 Keil MDK 打开 [Projects/MDK-ARM/atk_f103.uvprojx](Projects/MDK-ARM/atk_f103.uvprojx)，工程目标名为 `TEST`，器件为 STM32F103RC（启动文件 `startup_stm32f103xe.s`，全局宏 `STM32F103xE, USE_USB_FS`，编译器为 AC5/ARMCC 5.06，优化等级 Level 2（-O2））。
2. **务必确认 Options → Target → IROM1 的 Size 为 0x60000（384KB）**，这是工程里已改好的值。官方 readme.txt 解释：本例程 -O2 编译后约 302KB，超过 STM32F103RC 官方标称的 256KB，靠"借用"厂家未承诺的额外容量才放得下。能编译下载不影响开发板使用，但**做产品不要这样设计**。
3. 编译（F7），生成物在 [Output/atk_f103.hex](Output/atk_f103.hex)；用 ST-Link/J-Link 烧录，或串口 ISP（BOOT0 置 1 复位后用飞线/串口下载器，波特率同实验 35）。
4. 烧好后恢复 BOOT0 = 0，复位。接上串口（PA9/PA10，CH340 跳线帽 RXD→TX 那一组），波特率 **115200**：能看到 `usart_task` 每秒打印的内存占用 `in:xx.x,ex:xx.x`，出错复位时 `HardFault_Handler` 也会把 CFSR/HFSR 等寄存器打印到串口；USMART（实验 12）同步可用，注册函数见 [usmart_config.c](Middlewares/USMART/usmart_config.c)。
5. 运行前：插入**已存放 `/SYSTEM` 系统文件包**（含 `FONT/GBKxx.FON`、`SPB/...`、`APP/...` 目录）的 TF 卡到板载卡座；按要测的功能插入 NRF24L01 **或** ATK-MO395Q（二者共用插座，只能插一个）；测电压表把 P1 排针上的 RV1 电位器输出接到 ADC 脚；测红外/DS18B20 注意都在 PA1，一次只插一个。

## 七、实验现象

1. **开机自检画面**：黑底逐行打印 `ALIENTEK STM32F103`、硬件/软件版本号、`LCD ID:xxxx`、`CPU:STM32F103RCT6 72Mhz`、`FLASH:256KB + 8MB SRAM:48KB`，随后 `SPI Flash / RTC Check / FATFS Check / SD Card / SPI Flash Disk / 24C02 / Font Check / SYSTEM Files Check / Touch Check / SYSTEM Parameter Load` 每行右侧依次变绿显示 `OK`（颜色为白字，出错行为红字闪烁）；首次使用且 TF 卡里有系统包时，会看到字库更新和系统文件更新的进度条。启动结束 LED1 闪一下。
2. **主界面**：出现手机式 SPB 界面，左右滑动可翻页（2 页共 12 个应用图标 + 底部计算器/应用中心/电压表 3 个大图标）；**点 2 次**同一个图标才进入对应应用（官方 readme 特别提示了这一点）。
3. **LED0 心跳**：无论在哪个界面，LED0 约 2.5 秒短闪一次（`watch_task` 所为）；进入"LED 测试"时由应用接管，退出后恢复心跳。
4. 逐个功能验收（对应实验号）：
   - 电子图书（28/29/30）：浏览 TF 卡 TXT，汉字、翻页正常；
   - 数码相框（31）：显示 JPG/BMP/GIF，GIF 自动播放，KEY0 停；
   - 时钟（13/23/17）：实时走时、闹钟到点屏幕弹闹铃窗并由 `usart_task` 驱动响铃计时，温度显示（插 DS18B20 用外置，否则自动用内部温度传感器）；
   - 系统设置：13 项菜单（时间/日期/闹钟/语言/数码相框设置/屏幕校准/系统文件更新/恢复默认/关于等），参数存 24C02，断电保留（19）；
   - 记事本（34）：T9 拼音输入法打字并保存；手写画笔（33）：触屏画画；计算器：四则运算；
   - 运行器（35 类比）：浏览并执行 SPI FLASH 里的合法 BIN（复位地址须是 0x20xxxxxx），执行后只能按复位键回来；
   - USB 连接（36）：点进入并插上 Mini USB 线连电脑，电脑出现可移动磁盘（内容为 SPI FLASH 的文件系统区），可拔插不影响系统；
   - 网络通信（需 ATK-MO395Q）：TCP Server/TCP Client/UDP 三种模式互发字符；无线传书（需两块板各插 NRF24L01，25）：一台发送模式手画轨迹，另一台接收模式同步显示；
   - 按键测试 / LED 测试（1/2）：按下哪个键、LED 如何闪一目了然；
   - 电压表（15/16）：接好 RV1 后旋转电位器，数码管样式实时显示 ADC 原始值与电压值（`vol = (float)adcx * (3.3 / VMETER_ADC_MAX_VAL)`，`VMETER_ADC_MAX_VAL` 为 4095，见 [vmeterplay.c](User/APP/vmeterplay.c) 与 [vmeterplay.h](User/APP/vmeterplay.h)）。
5. **运行中拔插 TF 卡**：拔出后文件类应用提示卡不在位，重新插入约 1 秒内自动重新挂载恢复（`watch_task` 热插拔检测）。
6. 串口助手 115200 每秒打印 `in:%3.1f,ex:%3.1f`（内部 SRAM 内存池占用率，27）。

## 八、注意事项

1. **必须自备一张放好 `/SYSTEM` 文件包的 TF 卡**（官方 readme.txt 第 1 条）。没有它：首次开机字库检测失败会停在 `Font Error!` 红字循环。解决办法：关机 → 按住 WK_UP 重新上电 → KEY1 确认擦除 → 放入含系统包的卡后复位。
2. **网络与无线功能需单独购买的模块**（官方 readme 第 2、3 条）；且 CH395Q 与 NRF24L01 引脚定义完全相同（CS=PC4、RST/CE=PA4、INT/IRQ=PA1），**不能同时插**；无线传书需要两套开发板各插一块 24L01。红外接收头、DS18B20、24L01 中断都占 PA1，测试前注意拔换。
3. **384KB Flash 是"借用"容量**：编译产物约 302KB，超出官方标称 256KB，工程已把 IROM1 Size 改为 0x60000。绝大多数芯片可用，但厂家不保证；产品级设计应换更大 Flash 的型号（如 F103ZE）或裁剪功能。
4. **SPI FLASH 擦写、字库更新、FATFS 写入过程中严禁断电**，否则文件系统/字库损坏，需按第 1 条的 WK_UP 擦除流程重建；"LED 测试""按键测试"之外尽量少用 WK_UP 上电，避免误擦。
5. 运行器执行 BIN 后系统被用户代码接管，**只能按复位键**才能回到本系统（[exeplay.c](User/APP/exeplay.c) 提示语原文）。
6. **代码与注释/官方说明不一致之处（以代码为准）**：
   - `system_init()` 末尾注释写"蜂鸣器短叫,提示正常启动"，实际代码是 `LED1(0/1)` 闪一下——MiniSTM32 板载没有蜂鸣器（`beepplay.c` 虽在源码树中但**未加入 Keil 编译列表**）；
   - readme.txt 说"几乎所有资源都有用到"，但工程编译列表（[atk_f103.uvprojx](Projects/MDK-ARM/atk_f103.uvprojx)）只包含 15 个主功能 + 应用中心里的红外遥控；`camera.c`、`nesplay.c`、`audioplay.c`、`facereco.c`、`gyroscope.c` 等文件存在但未编译，应用中心里其余 15 个格子显示"应用程序"占位；
   - "应用中心"能进的只有第 1 个图标"红外遥控"（[appplay.c](User/APP/appplay.c) 中 case 1~3 被测注释掉）；
   - `usart_task` 打印的 `ex:` 对应的 `psex` 赋值行被注释（`//psex = my_mem_perused(SRAMEX);`），该值是无意义变量，只有 `in:` 真实反映内存池占用；
   - 电压表帮助界面注释 `adc_get_result_average(ADC_ADCX_CHY, 10)` 旁写"获取通道5的转换值"，实际宏 `ADC_ADCX_CHY = 14`（PC4，ADC1 通道 14），注释是历史遗留，以宏为准。
7. 若触摸失灵或图标点不中：进"系统设置 → 8.屏幕校准"重新校准；电阻屏也可开机时按 KEY0 触发 `tp_adjust()`。触摸屏类型检测失败时 `g_tpad_failed_flag` 提供了用 WK_UP 代替退出键的思路（本工程未再引用，仅作了解）。
8. 本工程编译体积大、任务多，**不要用低主频/降频配置测试**；`sys_stm32_clock_init(9)` 必须保持 72MHz，否则 SPI FLASH 高速读、SPB 帧搬运和串口波特率全部跑偏。

## 九、总结与延伸

### 本实验掌握的要点

1. **模块集成方法**：Drivers/BSP 提供寄存器级驱动，Middlewares 提供与硬件无关的服务（GUI/FATFS/TEXT/PICTURE/T9INPUT/MALLOC/USB/UCOSII），User/APP 只做"应用逻辑"——三层结构是产品代码的雏形。
2. **裸机自检 + RTOS 运行**的分层启动：`system_init()` 顺序做硬件验收，`OSStart()` 之后靠优先级保证心跳、退出键等实时性。
3. **性能技巧**：SPB 动画靠"预渲染帧存 SPI FLASH + DMA 搬运"，避开 72MHz MCU 逐点画屏的瓶颈。
4. **存储分工**：SD 卡（用户数据/安装包）、SPI FLASH（字库+文件系统+帧缓存+BIN，注意 4.2MB 字库区、7.3MB 帧区、7.9MB 运行器区的地址规划）、24C02（小参数）各司其职。
5. **前 40 个实验的落点对照**：LCD=实验11、触摸=21、SD/FATFS=28/29、RTC=13、红外=22、无线=25、内存管理=27、UCOSII=38~40、串口/USMART=4/12、ADC/DMA=16/15、IIC/SPI=19/20、汉字/图片=30/31、T9=34、USB=36——每个都能在 `Drivers/BSP` 与 `Middlewares` 里找到对应源文件。

### 延伸小练习

1. 把 [main.c](User/main.c) `watch_task` 中 `t == 119` 改为 `t == 49`，LED0 心跳从约 2.5 秒变为约 1 秒，观察是否影响界面流畅度（体会任务优先级与时间片）。
2. 在 [appplay.c](User/APP/appplay.c) 里取消 `case 2: led_play(...)` 的注释，把"LED 测试"再挂到应用中心入口，练一遍"图标名表 + case 分发"的扩展套路。
3. 修改 [spb.h](User/APP/spb.h) 的 `SPB_MOVE_ACT(50)` 为 20，感受滑动翻页触发的灵敏度变化；再试着把 `SPB_PAGE_NUM` 改回 1 观察界面布局代码如何自适应。
4. 用 USMART（实验 12）向 [usmart_config.c](Middlewares/USMART/usmart_config.c) 追加注册 `led_init` 类函数，从串口直接调用，验证系统运行时的底层可控性。

### 衔接

至此寄存器版全部实验结束。可转向：正点原子《STM32F103 标准库/HAL 库例程》对比同一功能的库函数写法；或裁剪本工程（关掉 USB/网络/无线源码），把剩余部分搬进 256KB 预算做"合规"产品原型；更进一步可研究 [os_cfg.h](Middlewares/UCOSII/uC-OS2/Cfg/Template/os_cfg.h) 的节拍/统计配置与 [malloc.c](Middlewares/MALLOC/malloc.c) 内存池参数，为后续更大工程（如实验 32 的 DSP 优化思路）打底。
