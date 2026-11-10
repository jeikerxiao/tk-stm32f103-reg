# 实验13 RTC实验

> 一句话概括：让 STM32 内部的"实时时钟（RTC）"开始走时，把秒数换算成年月日时分秒显示在 LCD 上，并用 USMART 从串口改时间、设闹钟、读写后备区 SRAM —— 断电重上电后时间依然不丢。

## 一、实验简介

- **实验目的（官方）**：学习 STM32 自带 RTC 的使用。
- **通俗展开**：RTC（Real Time Clock，实时时钟）是芯片里一个"独立的小钟表"。它的核心其实非常简单：
  一个每秒钟加 1 的计数器。只要每隔 1 秒给计数器 +1，再数一数"从某个起点到现在一共过了多少秒"，
  就能倒推出今天几号、现在几点。本实验要做的三件事：① 把这个钟表起振并开始走；② 把"秒数"翻译成人能看懂的
  年/月/日/时/分/秒/星期；③ 让它能在主电源断开后靠纽扣电池（VBAT 脚）继续走。
- **学习路线中的位置**：
  - 前置：实验4 串口实验（USMART 靠串口工作）、实验11 TFTLCD（MCU屏）实验（时间显示在屏上）、实验12 USMART调试实验。
  - 后续：实验14 低功耗实验（RTC + 待机模式是电池产品的标准搭配）、实验41 综合测试实验。
- **实验平台**：正点原子 MiniSTM32 V4 开发板，主芯片 STM32F103RC（Cortex-M3，本实验跑 72MHz）。
- **代码风格**：寄存器版。全文没有出现 HAL/标准库函数，所有配置都是直接读写寄存器，例如 `RCC->BDCR |= 1 << 16;`。
  寄存器就是一个 32 位的小格子，每一位（bit）代表一个开关或一段编码值，"配置外设"就是按位往格子里填数。

## 二、实验原理

### 2.1 RTC 由哪几块组成

F103 的 RTC 只有 4 类寄存器（见 `RTC_TypeDef`，[stm32f103xe.h](Drivers/CMSIS/Device/ST/STM32F1xx/Include/stm32f103xe.h)）：

| 寄存器 | 全称 | 在本实验里的作用 |
| --- | --- | --- |
| `RTC->CRH` / `RTC->CRL` | 控制高/低 16 位 | 开秒中断、进配置模式(INIT)、等同步(RSF)、等写完成(RTOFF)、查标志(SECF/ALRF/OWF) |
| `RTC->PRLH` / `RTC->PRLL` | 预分频器高/低 16 位 | 把 32.768kHz 或 40kHz 分频成 1Hz |
| `RTC->CNTL` / `RTC->CNTH` | 秒计数器低/高 16 位 | 存"从 1970-1-1 0:0:0 起走过的总秒数" |
| `RTC->ALRL` / `RTC->ALRH` | 闹钟低/高 16 位 | 计数器追上这个值时置 ALRF 标志并中断 |

因为 32 位的值被拆成了高 16 位 + 低 16 位两个寄存器，所以代码里到处都是 `seccount >> 16` 和 `seccount & 0xffff` 这样的配对写法。

### 2.2 时钟源：LSE 还是 LSI

RTC 不能用 72MHz 的主时钟（太贵、太费电），它要用一个极低频的独立时钟：

| 时钟源 | 频率 | 特点 | 代码中的操作 |
| --- | --- | --- | --- |
| LSE（外部低速晶振） | 32.768kHz | 又准又省电，但需要板上有 32.768kHz 晶振 | `RCC->BDCR \|= 1 << 0;` 开启，等 `RCC->BDCR & 0X02`（LSERDY）置 1 |
| LSI（内部低速 RC） | 典型约 40kHz | 不用外部晶振，但精度差（手册只给典型值，芯片间离散大），走时会偏 | `RCC->CSR \|= 1 << 0;` 开启，等 `RCC->CSR & 0x02`（LSIRDY） |

选哪个由 `RCC->BDCR` 的 **RTCSEL[1:0]（第 9:8 位）** 决定：`10` 选 LSE，`01` 选 LSI，`00` 关闭 RTC 时钟。
`rtc_init()` 的策略是"**先试 LSE，起振失败就退到 LSI**"，等待起振最多重试 200 次、每次 5ms。

### 2.3 备份域访问：为什么要动 PWR 和 RCC

RTC 和后备寄存器（BKP）都住在**备份域（Backup Domain）**里，只要 VBAT 有电，即使主电源 VDD 断掉，
备份域也在供电，RTC 就不停、数据就不丢。但为了防误写，备份域默认是"写保护"的，所以**任何一次配置 RTC 之前，
都必须先做这三步**（[rtc.c](Drivers/BSP/RTC/rtc.c) 的 `rtc_init`/`rtc_set_time`/`rtc_set_alarm` 里都重复出现了这段）：

```c
RCC->APB1ENR |= 1 << 28;            /* 使能电源时钟 */
RCC->APB1ENR |= 1 << 27;            /* 使能备份时钟 */
PWR->CR |= 1 << 8;                  /* 取消备份区写保护 */
```

- `RCC->APB1ENR` 第 28 位 = PWREN：电源控制器（PWR）的总线时钟，不开就读写不到 `PWR->CR`。
- `RCC->APB1ENR` 第 27 位 = BKPEN：备份区外设的总线时钟。
- `PWR->CR` 第 8 位 = DBP（Disable Backup protection）：置 1 才允许写 `RCC->BDCR` 和备份区寄存器。

想"从头再来"（比如从 LSI 换成 LSE）时，用 `RCC->BDCR` 第 16 位 **BDRST** 对整个备份域做软复位：

```c
RCC->BDCR |= 1 << 16;           /* 备份区域软复位 */
RCC->BDCR &= ~(1 << 16);        /* 备份区域软复位结束 */
```

注意：这一复位会把 RTC 计数器、预分频值和**全部 42 个备份寄存器**一起清零，时间/日期立刻丢失。

### 2.4 预分频 PR 得到 1Hz

RTC 内部先做一级预分频：

```
RTC 计数频率 = RTCCLK 频率 / (PRL + 1)      （PRL 是 PRLL:PRLH 组成的分频值）
```

代入本工程实际数值：

- 用 LSE：`32768 / (32767 + 1) = 1Hz`，所以 `clockfreq = 32768 - 1 = 32767 = 0x7FFF`，写入 `RTC->PRLL`，`RTC->PRLH = 0`。
- 用 LSI：`40000 / (39999 + 1) = 1Hz`，代码里 `clockfreq = 40000 - 1;`（0x9C3F）。LSI 频率本身不准，所以走时偏差比 LSE 大得多。

**PR 寄存器有影子寄存器**：你写进去的值先进"预装载（影子）"里，要等当前这一秒计满、预分频器递减到 0 时，
影子值才真正装进工作寄存器。这就是为什么改分频值必须走"INIT 模式"这个特殊流程（见 2.5）。

同理，`RTC->CNTL/CNTH`、`RTC->PRLH/PRLL`、`RTC->ALRH/ALRL` 都是双缓冲的，读的时候读到的是工作寄存器。

### 2.5 INIT 模式与 RTOFF：RTC 的"写操作协议"

RTC 工作在跨时钟域（72MHz 的 PCLK ↔ 32.768kHz 的 RTCCLK），所以它规定了一套严格的写流程，
用 `RTC->CRL` 的三个位来控制：第 4 位 INIT（进入配置模式）、第 5 位 RTOFF（写操作完成标志，1 = 空闲）、
第 3 位 RSF（寄存器已同步到 RTCCLK 域，1 = 同步好了）。

本实验的实际步骤（摘自 `rtc_init()`）：

1. `while (!(RTC->CRL & (1 << 3)));` —— 等 RSF = 1，确保 APB 侧读到的值和 RTC 内部是同步一致的。
2. `RTC->CRH |= 1 << 0;` —— 打开秒中断允许位（CRH 的第 0 位，手册名 SECIE；源码注释写作 "SECF"，见第八章）。
3. `while (!(RTC->CRL & (1 << 5)));` —— 等 RTOFF = 1，上一次操作写完。
4. `RTC->CRL |= 1 << 4;` —— INIT = 1，进入配置模式（此时计数器会暂停）。
5. 写 `RTC->PRLH = 0X0000; RTC->PRLL = clockfreq;` —— 设分频值。
6. `RTC->CRL &= ~(1 << 4);` —— INIT = 0，退出配置模式，新分频值在下一个周期装载。
7. `while (!(RTC->CRL & (1 << 5)));` —— 再等 RTOFF = 1。

`rtc_set_time()` 改 `CNTL/CNTH`、`rtc_set_alarm()` 改 `ALRL/ALRH` 走的是同一套流程，只是不用再开秒中断（第 2 步）。
记住一句话：**改 RTC 的计数器/分频器/闹钟，必须 INIT=1 写、INIT=0 收尾、最后等 RTOFF**。

### 2.6 秒数 ↔ 日历时间的换算

RTC 只会告诉你"总共走了多少秒"，日历是人算出来的。核心逻辑在 `rtc_get_time()`：

1. 取总秒数：`seccount = (RTC->CNTH << 16) + RTC->CNTL;`
2. 算天数：`temp = seccount / 86400;`（一天 = 24×60×60 = 86400 秒）
3. 从 1970 年开始，一年一年往上减：闰年减 366，平年减 365（闰年规则见 `rtc_is_leap_year()`：
   "四年闰百年不闰，四百年又闰"，即 `(year % 4 == 0 && year % 100 != 0) || year % 400 == 0`），减完剩下的 `temp` 是"今年的第几天"。
4. 再按月份天数表 `month_table[12] = {31,28,31,30,31,30,31,31,30,31,30,31}` 一个月一个月减（2 月逢闰年按 29 天特判），得到月和日。
5. 剩下的秒：`temp = seccount % 86400;` → `hour = temp / 3600`、`min = (temp % 3600) / 60`、`sec = (temp % 3600) % 60`。
6. 星期用**基姆拉尔森计算公式**算出来：`rtc_get_week()` 里的
   `week = (day + 1 + 2*month + 3*(month+1)/5 + year + (year>>2) - year/100 + year/400) % 7;`，返回 0~6 对应周日~周六。

反方向（设时间）在 `rtc_date2sec()`：把年月日先做一个"1、2 月挪到上一年年底"的变换（`monx -= 2`，
这样 2 月的闰日就落在最后，好处理），再用 `X = Y + M + D - 719162;`（减掉公元元年→1970 年的总天数）
得到 1970-1-1 以来的天数，最后 `T = ((X * 24 + hour) * 60 + min) * 60 + sec;`。

因为总秒数存的是 32 位无符号数，最大 `4294967295` 秒 ≈ 136 年，所以代码注释限定合法年份是 **1970 ~ 2105**。

### 2.7 中断、闹钟与后备区 SRAM

- **秒中断**：`RTC->CRH` 的秒中断允许位打开后，计数器每 +1 就把 `RTC->CRL` 第 0 位 SECF 置 1 并触发 `RTC_IRQn`。
  `RTC_IRQHandler()` 里查 SECF → 调 `rtc_get_time()` 更新时间 → 写 0 清 SECF。所以哪怕主循环不主动读，`calendar` 结构体每秒也会被刷新一次。
- **闹钟**：`rtc_set_alarm()` 把目标时刻换算成秒写进 `ALRH:ALRL`；计数器追上它时 `RTC->CRL` 第 1 位 ALRF = 1，
  中断里打印 `Alarm Time:...` 并清标志。第 2 位 OWF 是 16 位计数器溢出标志，中断里也顺手清掉。
- **后备区 SRAM（BKP DR1~DR42）**：1 块掉电不丢的小存储区，`BKP_BASE = 0x40006C00`，第一个数据寄存器 DR1 在 `BKP_BASE + 4`。
  `rtc_write_bkr()/rtc_read_bkr()` 用 `BKP_BASE + 4 + bkrx * 4` 直接寻址，每个数据寄存器占 4 字节、只用低 16 位。
  `bkrx` 传 **0~9 时正好落在 DR1~DR10**；驱动注释写的"范围 0~41"其实是按这个线性公式算的，
  而备份区在 DR10 之后插了 `RTCCR`/`CR`/`CSR` 三个控制寄存器和 2 个保留字，所以 `bkrx ≥ 10` 并不是 DR11（详见第八章）。
  本实验只用到 `bkrx = 0`（DR1）这一个格子做标记，读写练习也建议只在 0~9 里选。
- **BKP0（即 DR1）被驱动当成"时钟源身份证"用**：写 `0X5050` 表示已用 LSE 初始化过，写 `0X5051` 表示用的是 LSI，
  写 `0XFFFF` 表示初始化失败。正因为有这个标记，开发板重新上电（主电源断电但 VBAT 仍供电）时 `rtc_init()` 会直接跳过配置，让 RTC 继续走。

## 三、硬件资源与接线

| 资源 | 引脚 | 代码中的宏/位置 | 说明 |
| --- | --- | --- | --- |
| LED0（红灯） | PA8 | `LED0_GPIO_PORT/PIN`，[led.h](Drivers/BSP/LED/led.h) | 推挽输出，低电平点亮，每 200ms 翻转表示程序在跑 |
| LED1（绿灯） | PD2 | `LED1_GPIO_PORT/PIN`，[led.h](Drivers/BSP/LED/led.h) | 本实验未使用 |
| 串口1（USMART 通道） | TX=PA9，RX=PA10 | `USART_TX/RX_GPIO_PIN`，[usart.h](Drivers/SYSTEM/usart/usart.h) | 接板载 CH340，115200bps |
| TFTLCD（MCU 屏，16 位 8080 并口） | D0~D15=PB0~PB15，WR=PC7，RD=PC6，CS=PC9，RS=PC8，BL=PC10 | [lcd.h](Drivers/BSP/LCD/lcd.h) | RESET 与开发板复位脚共用，未单独占用 IO |
| RTC + 备份域 | 片内外设 | [rtc.c](Drivers/BSP/RTC/rtc.c)、`RTC_BASE = 0x40002800` | 由 LSE 或 LSI 驱动，VBAT 供电保持 |
| LSE 晶振 | PC14/PC15（OSC32_IN/OUT） | 板载 32.768kHz 晶振 | `RCC->BDCR` 第 0 位 LSEON 开启 |

**外接与跳线**：

1. LCD 模块插在开发板的 LCD 插座上（2.8/3.5/4.3/7/10 寸 MCU 屏均可，**不支持 RGB 屏**），插之前确认 VCC 位置对齐，插反会烧屏。
2. 要用板载 USB 转串口调试 USMART：USB 线插 **USB_UART** 口，并把丝印 **USART1** 排针上的 PA9→RXD、PA10→TXD 各用一个跳线帽连上。
3. 主电源断开后想保住时间，要给芯片的 **VBAT 脚**供电（板上备份电池座装 3V 纽扣电池，或从排针外部给 VBAT 供 3V）。
   没有 VBAT 时，拔掉 USB 备份域就整块掉电，时间必然清零。

**关于 72MHz**：`sys_stm32_clock_init(9)` 把内部 8MHz HSE 经 PLL 9 倍频到 72MHz，只影响 CPU/外设跑多快。
RTC 的走时**与主频无关**，它只认 LSE/LSI——这也是 RTC 能在主时钟全停时照常计时的原因。

## 四、代码结构与调用流程

```
实验13 RTC实验/
├── User/main.c                    主程序：初始化 → 循环刷新 LCD 时间显示
├── Drivers/BSP/RTC/rtc.c|.h       RTC 驱动（本实验主角）
├── Drivers/BSP/LED/led.c|.h       LED0/LED1 驱动
├── Drivers/BSP/LCD/lcd.c|.h       LCD 驱动，负责显示年月日时分秒
├── Drivers/SYSTEM/sys/sys.c|.h    时钟初始化、GPIO/NVIC 通用配置
├── Drivers/SYSTEM/usart/…         串口 + printf 重定向
├── Drivers/SYSTEM/delay/…         SysTick 延时
└── Middlewares/USMART/…           串口函数调用组件（改时间/闹钟/读写 BKP）
```

`main()` 流程：

1. `sys_stm32_clock_init(9)` → 72MHz
2. `delay_init(72)` → 延时可用
3. `usart_init(72, 115200)` → 串口 + printf
4. `usmart_dev.init(72)` → USMART 起来，等待串口指令
5. `led_init()` / `lcd_init()` → 灯和屏可用
6. `rtc_init()` → 起 RTC（详见第五章；返回 0 成功，1 失败）
7. 屏上打三行标题：`STM32` / `RTC TEST` / `ATOM@ALIENTEK`
8. `while(1)`：`t` 每 10ms 加 1
   - `t % 10 == 0`（约每 100ms）：`rtc_get_time()` → 三行 `sprintf` → 刷新 Time/Date/Week
   - `t % 20 == 0`（约每 200ms）：`LED0_TOGGLE()`

## 五、核心代码解析

### 5.1 主程序 [User/main.c](User/main.c)（完整）

```c
char* weekdays[]={"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saterday"};

int main(void)
{
    uint8_t tbuf[40];
    uint8_t t = 0;

    sys_stm32_clock_init(9);    /* 设置时钟, 72Mhz */
    delay_init(72);             /* 延时初始化 */
    usart_init(72, 115200);     /* 串口初始化为115200 */
    usmart_dev.init(72);        /* 初始化USMART */
    led_init();                 /* 初始化LED */
    lcd_init();                 /* 初始化LCD */
    rtc_init();                 /* 初始化RTC */
    ...
    while (1)
    {
        t++;
        if ((t % 10) == 0)  /* 每100ms更新一次显示数据 */
        {
            rtc_get_time();
            sprintf((char *)tbuf, "Time:%02d:%02d:%02d", calendar.hour, calendar.min, calendar.sec);
            lcd_show_string(30, 120, 210, 16, 16, (char*)tbuf, RED);
            sprintf((char *)tbuf, "Date:%04d-%02d-%02d", calendar.year, calendar.month, calendar.date);
            lcd_show_string(30, 140, 210, 16, 16, (char*)tbuf, RED);
            sprintf((char *)tbuf, "Week:%s", weekdays[calendar.week]);
            lcd_show_string(30, 160, 210, 16, 16, (char *)tbuf, RED);
        }
        if ((t % 20) == 0)
        {
            LED0_TOGGLE();  /* 每200ms,翻转一次LED0 */
        }
        delay_ms(10);
    }
}
```

- `%02d`/`%04d` 表示不足位数时前面补 0，所以显示成 `2020-05-31` 而不是 `2020-5-31`。
- `weekdays[calendar.week]` 用 `rtc_get_week()` 返回的 0~6 去查字符串表。
- 显示刷新靠轮询（100ms 一次）而不是靠中断，好处是主循环逻辑简单；即使把秒中断关掉，画面照样每秒跳。

### 5.2 RTC 初始化 `rtc_init()` —— 最关键的一段

第一步：进备份域并读身份证。

```c
RCC->APB1ENR |= 1 << 28;            /* 使能电源时钟 */
RCC->APB1ENR |= 1 << 27;            /* 使能备份时钟 */
PWR->CR |= 1 << 8;                  /* 取消备份区写保护 */

bkpflag = rtc_read_bkr(0);          /* 读取BKP0的值 */
```

第二步：**首次配置分支**（`bkpflag != 0X5050`）。先复位备份域，从 LSE 起步：

```c
RCC->BDCR |= 1 << 16;           /* 备份区域软复位 */
RCC->BDCR &= ~(1 << 16);        /* 备份区域软复位结束 */
RCC->BDCR |= 1 << 0;            /* 开启外部低速振荡器 */

while (retry && ((RCC->BDCR & 0X02) == 0))  /* 等待LSE准备好 */
{
    retry--;
    delay_ms(5);
}
```

`retry` 初值 200，最多等 1 秒。`0X02` 就是 BDCR 第 1 位 LSERDY。加 `retry` 是为了**板上晶振没焊/没起振时不至于死机等住**。

第三步：根据起振结果选时钟源并算分频值。

```c
tempreg = RCC->BDCR;            /* 读取BDCR的值 */
tempreg &= ~(3 << 8);           /* 清零8/9位 */

if (retry == 0)                 /* 开启LSE失败? */
{
    RCC->CSR |= 1 << 0;         /* LSI使能 */
    while (!(RCC->CSR & 0x02)); /* 等待LSI就绪 */
    tempreg |= 1 << 9;          /* LSE开启失败,启动LSI. */
    clockfreq = 40000 - 1;      /* LSI频率约40Khz(参考F103数据手册说明) */
    rtc_write_bkr(0, 0X5051);   /* 标记已经初始化过了,使用LSI */
}
else
{
    tempreg |= 1 << 8;          /* 选择LSE,作为RTC时钟 */
    clockfreq = 32768 - 1;      /* LSE频率为32.769Khz */
    rtc_write_bkr(0, 0X5050);   /* 标记已经初始化过了,使用LSE */
}

tempreg |= 1 << 15;             /* 使能RTC时钟 */
RCC->BDCR = tempreg;            /* 重新设置BDCR寄存器 */
```

先把 RTCSEL[1:0] 两位清零再写，避免原来残留的值干扰；最后一次性 `RCC->BDCR = tempreg` 整体写回。

第四步：按 INIT 流程装载分频值（见 2.5 的步骤 1~7）。之后：

```c
if (bkpflag != 0X5051)          /* 既不是0X5050也不是0X5051，说明是第一次配置，需要设置时间日期 */
{
    rtc_set_time(2020, 5, 31, 6, 59, 55);   /* 设置时间 */
}
```

第五步：**非首次分支**（`bkpflag == 0X5050`，VBAT 保着电重新上电）——什么都不重配，只做一次"活着检测"：

```c
tempreg = RTC->DIVL;            /* 读取DIVL寄存器的值 */
while (retry)
{
    delay_ms(5);
    retry--;
    if (tempreg != RTC->DIVL)   /* 对比DIVL和tempreg, 如果有差异, 则退出 */
    {
        break;                  /* DIVL != tempreg, 说明RTC在计数, 说明晶振没问题 */
    }
}
```

`RTC->DIVL` 是预分频器的**当前计数值**，只要 RTC 在走，它就一直在变；连续 100 次（500ms）读到的值都一样，
说明晶振停了或 RTC 死了，于是把 BKP0 写成 `0XFFFF`、复位 `RCC->BDCR` 并 `return 1`，下次调用就会走首次配置分支重来一遍。
最后 `sys_nvic_init(0, 0, RTC_IRQn, 2)` 打开 RTC 中断（组 2，抢占 0 子 0），再 `rtc_get_time()` 取一次初始时间。

### 5.3 秒中断服务函数

```c
void RTC_IRQHandler(void)
{
    if (RTC->CRL & (1 << 0))    /* SECF = 1, 秒钟中断 */
    {
        rtc_get_time();         /* 更新时间 */
        RTC->CRL &= ~(1 << 0);  /* SECF = 0, 清秒钟中断 */
    }
    if (RTC->CRL & (1 << 1))    /* ALRF = 1, 闹钟标志 */
    {
        RTC->CRL &= ~(1 << 1);  /* ALRF = 0, 清闹钟标志 */
        printf("Alarm Time:%d-%d-%d %d:%d:%d\n", calendar.year, calendar.month, calendar.date, calendar.hour, calendar.min, calendar.sec);
    }
    RTC->CRL &= ~(1 << 2);      /* OWF = 0, 清除溢出中断标志 */
    while (!(RTC->CRL & (1 << 5))); /* 等待RTC寄存器操作完成, 即等待RTOFF == 1 */
}
```

要点：**标志必须软件清 0**，不清就会反复进中断把主循环饿死；末尾等 RTOFF 是 RTC 写操作的收尾规矩。

### 5.4 读写后备区 SRAM

```c
void rtc_write_bkr(uint32_t bkrx, uint16_t data)
{
    uint32_t temp = 0;
    PWR->CR |= 1 << 8;  /* 取消备份区写保护 */
    temp = BKP_BASE + 4 + bkrx * 4;
    (*(uint16_t *)temp) = data;
}
```

`BKP_BASE`（0x40006C00）+ 4 就是 DR1 的地址，`bkrx * 4` 是因为每个数据寄存器占 4 字节。
`*(uint16_t *)temp = data` 是寄存器版常见套路：把一个数值当成地址、再按 16 位宽度去访问它。
写之前那句 `PWR->CR |= 1 << 8`（DBP）不能省，否则写不进去。

## 六、编译、下载与运行

1. 用 Keil MDK 打开 [Projects/MDK-ARM/atk_f103.uvprojx](Projects/MDK-ARM/atk_f103.uvprojx)。
2. 工程设置核对：器件 **STM32F103RC**（Flash 0x08000000 起 256KB，RAM 0x20000000 起 48KB），
   C 宏定义 `STM32F103xE`，头文件包含路径 `..\..\Drivers\CMSIS\Device\ST\STM32F1xx\Include;..\..\Drivers\CMSIS\Include;..\..\Drivers;..\..\User;..\..\Middlewares`。
3. 启动文件为 `startup_stm32f103xe.s`（内含 `RTC_IRQHandler`、`PVD_IRQHandler` 等向量名，函数名必须与之完全一致）。
4. Build 后产物在 `Output/` 目录（`atk_f103.axf` / `atk_f103.hex`，工程已勾选 Create HEX File）。
5. 用 ST-Link 或 J-Link 下载；没有调试器时也可用串口 ISP（BOOT0 置 1 上电，用 FlyMcu 烧 hex）后恢复 BOOT0。
6. 上位机：XCOM 或 SSCOM，**波特率 115200，8N1**，务必避开会自动拉 DTR/RTS 把 MCU 复位的串口助手。

## 七、实验现象

1. 上电后 LED0 每 200ms 闪一次，说明程序在跑。
2. LCD 显示标题三行，随后三行数据实时刷新，例如：
   `Time:06:59:56`、`Date:2020-05-31`、`Week:Sunday`。秒每来一次 RTC 中断就 +1。
3. 打开串口助手（115200），发送 `rtc_set_time(2022,12,21,10,30,0)` 并回车换行，
   LCD 上的时间立刻跳到 2022-12-21 10:30:00，星期也会同步变（`rtc_set_time` 内部会再调 `rtc_get_time()`）。
   输入 `list` 可以看到全部受控函数清单；输入 `dec` 可把参数显示从十六进制切成十进制。
4. 发送 `rtc_set_alarm(2022,12,21,10,30,20)`，等时间走到 10:30:20，串口打印一行
   `Alarm Time:2022-12-21 10:30:20`（闹钟只置标志、不响铃）。
5. 发送 `rtc_write_bkr(1,1234)`，再发 `rtc_read_bkr(1)`，返回值就是刚写进去的数
   （USMART 默认按十六进制显示参数，会看到 `0X4D2` = 1234；输入 `dec` 切成十进制后直接显示 1234）。
   `bkrx = 1` 对应备份区的 DR2，掉电后内容仍在。
6. 断电（拔掉 USB）静置十几秒后再上电：
   - 装了 VBAT 电池：时间从断电处继续，不回到 2020-05-31 —— 因为 `rtc_init()` 读到 BKP0 = 0x5050，直接走"继续计时"分支。
   - 没装 VBAT 电池：备份域彻底掉电，BKP0 被读成 0，于是重新初始化并把时间重置为 2020-05-31 06:59:55。

## 八、注意事项

- **RTC 时间对不上/一直在改**：检查板上 32.768kHz 晶振是否起振。`rtc_init()` 会用 `retry` 判断，
  失败就自动切 LSI（代码按 40kHz 算分频；LSI 是内部 RC，手册只给"典型 40kHz"，离散很大，走时可能一天差几十分钟，只能当兜底用）。
  想确认当前用的谁，串口执行 `rtc_read_bkr(0)`：`0x5050` = LSE，`0x5051` = LSI，`0xFFFF` = 上次初始化失败。
- **切换 LSE/LSI 会丢时间**：备份区软复位（BDRST）清零一切，必须重新 `rtc_set_time()`。
- **写 RTC 寄存器没生效**：十有八九是忘了开 `PWR->CR` 第 8 位 DBP，或没按 INIT → 写 → INIT=0 → 等 RTOFF 的顺序走。
- **卡在 `while (!(RTC->CRL & (1 << 3)));`**：RSF 一直是 0，说明 RTC 时钟源根本没起来（时钟源没选/没使能/晶振坏）。
- **中断不进来**：确认 `sys_nvic_init(0, 0, RTC_IRQn, 2)` 有执行，且中断服务函数名必须写成 `RTC_IRQHandler`，
  和启动文件里的向量名一字不差；另外 `RTC->CRH` 的秒中断允许位要置 1。
- **闹钟到点没打印**：`rtc_set_alarm()` 设的时刻必须是**将来的时间**，且年份要在 1970~2105 内；
  另外代码只使能了秒中断允许位，闹钟是靠轮询 ALRF 标志时顺带处理的，秒中断没开的话 ALRF 也没人查。
- **代码注释与手册的小出入（以手册/代码行为为准）**：
  1. `rtc_init()` 里 `RTC->CRH |= 1 << 0;` 的注释写的是 "`SECF = 1, 允许秒中断`"，
     严格讲 CRH 的第 0 位名字是 **SECIE（秒中断允许）**，而 **SECF（秒标志）在 CRL 的第 0 位**
     ——`RTC_IRQHandler` 里判的那个才是 SECF。功能没问题，只是位名标错了。
  2. 注释写 "LSE频率为32.769Khz"，是笔误，实际是 **32.768kHz**（代码里也确实按 `32768 - 1` 算的）。
  3. `weekdays[]` 里 "Saterday" 拼写有误，正确是 Saturday（不影响功能，只影响 LCD 显示）。
  4. `main.c` 的 `t` 是 `uint8_t`，每 256 次（约 2.56s）会溢出回 0，因此"每 200ms 翻转"是近似的，不影响观察。
- **官方 readme.txt 与本代码一致**：目的、引脚（LED0=PA8/LED1=PD2）、串口 115200、仅支持 MCU 屏等说明均与代码相符。
- **USMART 指令以回车换行结尾**；参数支持十进制/十六进制（`0x` 前缀）。
- **`rtc_read_bkr/rtc_write_bkr` 的编号坑（代码与注释不一致，以地址为准）**：注释说 `bkrx` 范围 0~41，
  可寻址公式是线性的 `BKP_BASE + 4 + bkrx * 4`，而 `BKP_TypeDef` 里 DR10 之后紧跟着 `RTCCR`、`CR`、`CSR`
  三个控制寄存器和 2 个保留字，之后才是 DR11。也就是说：
  `bkrx = 0~9` → DR1~DR10（正确），`bkrx = 10/11/12` → 命中的是备份区的 RTCCR / BKP_CR / BKP_CSR（RTC 输出校准、
   Tamper 使能等功能位），`bkrx = 13/14` 落在保留字，一直到 `bkrx = 15` 才又回到 DR11。
  **初学阶段请把 `bkrx` 限制在 0~9**，需要更多空间时再自己按结构体偏移（或直接用 `BKP->DR11` 这样的名字）改写寻址。

## 九、总结与延伸

**本实验掌握的要点**：

1. RTC 的本质是"预分频器 + 秒计数器"，1Hz 靠 `RTCCLK / (PRL + 1)` 算出来，LSE 下 PRL = 0x7FFF = 32767。
2. 备份域三道门：`RCC->APB1ENR` 的 PWREN(28)/BKPEN(27) + `PWR->CR` 的 DBP(8)，`RCC->BDCR` 管时钟源选择(LSEON/RTCSEL/RTCEN/BDRST)。
3. RTC 的写操作协议：等 RSF → INIT=1 → 写 → INIT=0 → 等 RTOFF，根源是影子寄存器和跨时钟域。
4. 日历换算是纯软件活：总秒数 ↔ 年月日时分秒，闰年判断 + 月份天数表 + 基姆拉尔森星期公式。
5. VBAT/备份 SRAM 让"掉电不失"成为可能，工程上用 BKP0 的标记值来区分"首次配置"与"继续计时"两种上电场景。

**延伸小练习**：

1. 在 `RTC_IRQHandler` 里取消那句 `//printf("sec:%d\r\n", calendar.sec);` 的注释，直观看每秒一次的中断节奏，再对比 LCD 刷新。
2. 用 `rtc_write_bkr(2, 某值)` + `rtc_read_bkr(2)` 做一个"上电次数计数器"：每次上电读一次、+1 后写回，
   LCD 显示第几次开机——这就是掉电记忆的典型用法（再配一个 `rtc_set_alarm()` 定闹钟就更完整了）。
3. 挑战：把 `rtc_init()` 里 LSE 分支的 `clockfreq` 从 `32768 - 1` 改成 `32768 * 2 - 1`（= 65535，16 位刚好装满），
   分频后变成 0.5Hz，秒计数器每 2 秒才 +1，LCD 上的时间就会"走慢一倍"——亲手验证 `RTC 计数频率 = RTCCLK / (PRL + 1)`。
   （记得先把 BKP0 改成别的值或做一次备份区软复位，否则 `rtc_init()` 会跳过配置分支，改动看不到效果。）

**衔接下一课**：RTC 让"时间在断电后依然存在"，而实验14 低功耗实验教你"怎样让芯片在等待时几乎不耗电"。
把两者合起来——RTC 定时 + 待机/停止模式——就是所有电池供电设备（智能手表、烟雾报警器）的骨架。
