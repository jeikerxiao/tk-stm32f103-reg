# 实验26 FLASH模拟EEPROM实验

> 一句话概括：不加任何外部芯片，直接拿 STM32 自己的内部 FLASH 当"掉电不丢数据"的 EEPROM 用——学会解锁、擦页、写半字这一套 FLASH 操作，并明白为什么"不能像 RAM 那样随便写"。

## 一、实验简介

- **实验目的**（官方 `readme.txt`）：学习 STM32 内部 FLASH 模拟成 EEPROM 的使用方法。
  通俗讲：EEPROM 是"掉电不丢、还能按字节改写"的小存储（比如保存你的配置、上次关机时的音量）。
  STM32 内部 FLASH 也能掉电保存，但它**只能整页擦、只能按半字（2 字节）写、寿命有限**，
  所以要用它模拟 EEPROM，就必须遵守一套固定流程：解锁 → 擦 → 写 → 上锁。本实验就是手写这套流程。
- **在学习路线中的位置**：
  - 前置：实验2 按键（KEY0/KEY1 触发读写）、实验4 串口 + **实验12 USMART**（本实验把 FLASH 读写函数挂到 USMART 上调试）、
    实验11 TFTLCD（显示结果）。
  - 后续：实验28 SD 卡、实验29 FATFS——那是"外加大容量存储"的路线；
    本实验的"内部 FLASH 保存少量关键数据"在实际项目里更常用（校准值、故障计数、Bootloader 标志）。
- **开发板与芯片**：正点原子 MiniSTM32 V4，主芯片 STM32F103RC（Keil 工程里器件为 `STM32F103RC`，
  `IROM(0x08000000,0x00040000)` = 256KB FLASH，`IRAM(0x20000000,0x0000C000)` = 48KB RAM）。
  本工程是**寄存器版**：直接操作 `FLASH->KEYR / CR / AR / SR`。

> 小概念：**寄存器**就是芯片里可读写的小格子。FLASH 控制器的几个格子（`CR` 控制、`SR` 状态、`AR` 地址、`KEYR` 钥匙）
> 决定了"现在能擦哪一页、能不能写"。而"读 FLASH"不需要任何寄存器——它和代码一样直接按地址取。

## 二、实验原理

### 2.1 FLASH 与 EEPROM 的差别（为什么要"模拟"）

| 项目 | EEPROM（如 24C02） | STM32 内部 FLASH |
| --- | --- | --- |
| 最小擦除单位 | 不用擦，直接按字节改写 | **一整页**（本工程 2KB） |
| 最小写入单位 | 1 字节 | **半字 = 2 字节**，地址必须偶数 |
| 写之前 | 可覆盖 | 只能把 1 写成 0；要变回 1 必须先擦 |
| 速度 | 慢（IIC 总线 + 内部写周期约 5~10ms） | 快（编程约几十 µs/半字，擦一页约 20ms） |
| 擦写寿命 | 约 10 万~100 万次 | **约 1 万次**（ST 闪存手册标称，25℃） |
| 成本/接线 | 要外接芯片、占 IIC 引脚 | **0 额外成本，不占引脚** |
| 数据保持 | 典型 100 年 | 典型 10 年 |

结论：**内部 FLASH 适合"很少改、不能丢"的小数据**（配置、标定值），不适合高频记录。
用它的正确姿势就是本实验的驱动：先检查目标位置是否已经非空，非空就整页擦掉再写。

### 2.2 本芯片的页大小：以代码里的宏为准

[stmflash.h](Drivers/BSP/STMFLASH/stmflash.h) 里并没有叫 `PAGE_SIZE` 的宏，页大小由这两个宏推导：

```c
#define STM32_FLASH_SIZE        0x40000         /* STM32 FLASH 总大小 */
#define STM32_FLASH_BASE        0x08000000      /* STM32 FLASH 起始地址 */

 /* STM32F103 扇区大小 */
#if STM32_FLASH_SIZE < 256 * 1024
#define STM32_SECTOR_SIZE   1024                /* 容量小于256K的 F103, 扇区大小为1K字节 */
#else
#define STM32_SECTOR_SIZE   2048                /* 容量大于等于于256K的 F103, 扇区大小为2K字节 */
#endif
```

把数一代进去：`STM32_FLASH_SIZE = 0x40000 = 262144 = 256×1024`，条件 `256×1024 < 256×1024` **不成立**，
所以走 `#else` 分支，**`STM32_SECTOR_SIZE = 2048`，即一页 2KB**。

| 芯片类别 | 容量 | 页（扇区）大小 | 说明 |
| --- | --- | --- | --- |
| STM32F103 中容量（Z：128KB 等） | ≤128KB | **1KB/页** | 代码里 `< 256*1024` 那条分支给的就是它 |
| STM32F103 高密度（**RC/RD/RE**，本工程） | 256KB/384KB | **2KB/页** | 本工程实际生效值 |

于是 256KB FLASH = 262144 / 2048 = **128 页**，编号 0~127。
`main.c` 选的保存地址 `FLASH_SAVE_ADDR = 0X08020000`：偏移 `0x20000 = 128KB`，
页号 = `(0x08020000 - 0x08000000) / 2048 = 64` → **正好是第 64 页的页首**，且离代码区（从 0x08000000 往上几 KB~几十 KB）很远，安全。

### 2.3 擦与写的寄存器流程（顺序不能错）

STM32F1 的 FLASH 控制器有 4 个关键寄存器：

| 寄存器 | 关键位 | 含义 |
| --- | --- | --- |
| `FLASH->CR` | bit0 `PG` | 编程（写）使能：置 1 后往 FLASH 地址写数据才会真的编程 |
| | bit1 `PER` | 页擦除使能 |
| | bit6 `STRT` | 启动擦除（写 1 后硬件自动开始擦整页） |
| | bit7 `LOCK` | 1＝`CR`/`AR` 被锁（复位后的默认状态），0＝解锁 |
| `FLASH->KEYR` | — | 钥匙寄存器：连写两个固定值才解锁 |
| `FLASH->AR` | 页内地址 | 要擦除的目标地址（**擦除是按页的**：给页内任意地址即可） |
| `FLASH->SR` | bit0 `BSY` / bit2 `PGERR` / bit4 `WRPRTERR` | 忙 / 编程错误 / 写保护错误 |

**正确顺序（本实验代码就是这么写的）**：

```text
1 CR.LOCK=1（复位默认） → KEYR 写 0x45670123 → KEYR 写 0xCDEF89AB → LOCK=0，解锁成功
2 擦：CR.PER=1 → AR=页内地址 → CR.STRT=1 → 等 SR.BSY 变 0 → CR.PER=0
3 写：CR.PG=1 → 直接把 16 位数写到目标地址（半字写）→ 等 SR.BSY 变 0 → CR.PG=0
4 CR.LOCK=1 上锁，避免程序跑飞时误擦
```

钥匙值来自代码：

```c
#define STM32_FLASH_KEY1        0X45670123
#define STM32_FLASH_KEY2        0XCDEF89AB
```

为什么要两个不同的值？**防误操作**：单字节/单值的偶然写入几乎不可能凑出这 8 个字节的序列，
只有程序"成心"解锁才能改 FLASH。

### 2.4 擦写寿命与"掉电保存"的设计思路

STM32 闪存手册标称内部 FLASH **约 10,000 次擦写周期**、数据保持 10 年（典型值）。
本工程的 `stmflash_write()` 是"要写就先擦整页"，所以：**每调用一次写入（只要页里有非 0xFFFF 的字节）就消耗一次该页的寿命**。

如果你的应用要频繁保存（例如每 10 秒存一次运行计数），1 万次只够约 28 小时，很快就报废。
工业上的常规做法有两条，本实验代码**没实现**，但值得理解（延伸练习 3 会让你动手加）：

1. **页内顺序追加（不重复擦）**：一页 2KB 有 1024 个半字槽位，把"数据 ID + 值"一条条往后写，
   只在整页写满时才擦一次；读的时候从后往前找同 ID 的最新一条。寿命从"每写一次擦一次"变成"每 1024 次写擦一次"。
2. **双页交换 + 传输计数（经典 EEPROM 模拟法）**：
   1. 分配 A、B 两页：一页当"数据页"，一页当"交换页"，初始全 0xFFFF。
   2. 平时所有新值都追加写进当前数据页。
   3. 当数据页**装不下**时：把每个变量的"最新值"完整搬抄到另一页，同时写一个**传输计数器**（每交换一次 +1）。
   4. 搬运完成后再擦除旧页，两页角色互换。
   5. **掉电安全**：万一搬运途中掉电，重新上电时靠"计数器 + 传输标志 + 每条数据的 ID/校验"判断
      哪一页是完整的，从那一页恢复；交换计数器还能顺带统计寿命、做磨损均衡（两页轮流擦，寿命翻倍）。

一句话概括设计思想：**"擦"是稀缺资源，所以要尽量少擦、并且让擦除过程可回退（幂等）**。

### 2.5 擦写期间 CPU 会"卡住"

FLASH 是程序存放的地方：一旦 `PG`/`PER` 生效，硬件会**暂停从 FLASH 取指**，直到操作完成。
所以擦一页（约 20ms）期间，主程序、中断（除 RAM 中的代码外）都停摆。
这就是 `stmflash_wait_done(0X5FFFFF)` 用这么大的循环上限、以及代码注释写">20ms"的原因，
也是"擦除时串口可能丢字节"的根因（见第八章）。

## 三、硬件资源与接线

| 资源 | 引脚 | 代码中的宏/位置 | 说明 |
| --- | --- | --- | --- |
| 内部 FLASH | **不占引脚** | `STM32_FLASH_BASE=0x08000000`，[stmflash.h](Drivers/BSP/STMFLASH/stmflash.h) | 256KB，128 页 × 2KB |
| 保存地址 | — | `FLASH_SAVE_ADDR = 0X08020000`，[main.c](User/main.c) | 第 64 页页首 |
| KEY1（写入） | PA15 | `KEY1_GPIO_PORT/PIN`，[key.h](Drivers/BSP/KEY/key.h) | `KEY1_PRES`=2 |
| KEY0（读出） | PC5 | `KEY0_GPIO_PORT/PIN` | `KEY0_PRES`=1 |
| WK_UP | PA0 | `WKUP_GPIO_PORT/PIN` | 本实验未使用 |
| LED0 | PA8 | [led.h](Drivers/BSP/LED/led.h) | `LED0_TOGGLE()` 心跳 |
| LED1 | PD2 | 同上 | 未使用 |
| 串口1 / USMART | PA9/PA10 | [usart.h](Drivers/SYSTEM/usart/usart.h)、[usmart_config.c](Middlewares/USMART/usmart_config.c) | 115200，USMART 用 TIM4 扫描 |
| TFTLCD（MCU 屏） | PB0~PB15 + PC6~PC10 | [lcd.h](Drivers/BSP/LCD/lcd.h) | 显示读写结果 |

不需要外接任何模块，也不用插跳线（除了USART1 排针上 PA9/PA10 → RXD/TXD 的跳线帽，若要使用串口/USMART）。

**时钟树对本实验意味着什么**：`sys_stm32_clock_init(9)` → 72MHz，`delay_ms(10)` 是按键扫描节拍；
`usmart_dev.init(72)` 里的 72 用来把 TIM4 计数分频到 10kHz（`PSC = 72*100 - 1 = 7199`），
`ARR = 1000` → 每 100ms 进一次 TIM4 中断扫描串口命令。FLASH 控制器本身不需要你配时钟（它是内核/总线自带的，不经过 APBENR）。

## 四、代码结构与调用流程

```text
实验26 FLASH模拟EEPROM实验/
├── User/main.c                          主程序：KEY1 写、KEY0 读，LCD 显示，USMART 调试
├── Drivers/BSP/STMFLASH/stmflash.c/.h   内部 FLASH 驱动（解锁/上锁/擦页/写半字/读写接口）
├── Drivers/BSP/KEY/key.c/.h             按键扫描
├── Drivers/BSP/LED/led.c/.h             LED0 心跳
├── Drivers/BSP/LCD/lcd.c/.h             TFTLCD 驱动（MCU 8080 并口）
├── Drivers/SYSTEM/{sys,delay,usart}     时钟/GPIO 寄存器封装、延时、串口1
├── Middlewares/USMART/*                 串口函数调用器（本实验已加入编译）
└── Projects/MDK-ARM/atk_f103.uvprojx    Keil 工程
```

`main()` 流程（[User/main.c](User/main.c)）：

```text
1 sys_stm32_clock_init(9) → delay_init(72) → usart_init(72,115200) → usmart_dev.init(72)
2 led_init() → lcd_init() → key_init()
3 LCD 显示标题 + "KEY1:Write  KEY0:Read"
4 主循环（每 10ms 一轮）：
    key_scan(0)
      KEY1_PRES → lcd_fill 清屏 → "Start Write FLASH...."
                  → stmflash_write(FLASH_SAVE_ADDR, (uint16_t*)g_text_buf, SIZE)
                  → "FLASH Write Finished!"
      KEY0_PRES → "Start Read FLASH.... "
                  → stmflash_read(FLASH_SAVE_ADDR, (uint16_t*)datatemp, SIZE)
                  → "The Data Readed Is:  " + 显示读到的字符串
    i++ 到 20 → LED0_TOGGLE()（约 200ms 一次心跳）
```

`stmflash_write()` 内部流程：

```text
地址合法性检查 → stmflash_unlock() → 逐半字扫描目标区间：
    读到 0xFFFF？ 是 → 跳过；否 → stmflash_erase_sector(页号) 擦掉整页
擦除全部成功 → 从 waddr 起逐个半字 stmflash_write_halfword()（PG=1 → 写 → 等 BSY）
→ stmflash_lock()
```

## 五、核心代码解析

### 5.1 解锁与上锁：`stmflash_unlock()` / `stmflash_lock()`

```c
static void stmflash_unlock(void)
{
    FLASH->KEYR = STM32_FLASH_KEY1; /* FLASH 写入解锁序列. */
    FLASH->KEYR = STM32_FLASH_KEY2;
}

static void stmflash_lock(void)
{
    FLASH->CR |= 1 << 7;    /* FLASH 上锁 */
}
```

- 解锁靠**连续两次按顺序写 `KEYR`**：先 `0x45670123`，再 `0xCDEF89AB`，硬件才清掉 `CR.LOCK`（bit7）。
  顺序颠倒、只写一个、或写了别的值，都会失败（还可能置错误标志），后续对 `CR`/`AR` 的写会被硬件忽略。
- 复位后 `LOCK` 默认是 1，所以每次操作前都必须先解锁——这就是"**LOCK → KEYR → 才动 PER/PG/STRT**"的顺序由来。
- 上锁只要把 bit7 置 1，一行搞定；`stmflash_write()` 结尾一定会执行，避免误擦。

### 5.2 状态查询与超时：`stmflash_wait_done()`

```c
static uint8_t stmflash_get_error_status(void)
{
    uint32_t res;
    res = FLASH->SR;
    if (res & (1 << 0)) return 1;   /* BSY = 1, 忙 */
    if (res & (1 << 2)) return 2;   /* PGERR = 1, 编程错误 */
    if (res & (1 << 4)) return 3;   /* WRPRTERR = 1, 写保护错误 */
    return 0;                       /* 没有任何错误 操作完成. */
}
```

`stmflash_wait_done(0X5FFFFF)` 就是一个"递减次数的轮询"：`BSY=1` 就继续等，出现 `PGERR`/`WRPRTERR` 立刻返回错误码，
次数耗尽返回 `0XFF`（超时）。
**PGERR 最常见的成因**：目标地址没擦干净（原值不是 0xFFFF）就试图编程——FLASH 只能 1→0，写不进第二次。

### 5.3 擦一页：`stmflash_erase_sector()`

```c
static uint8_t stmflash_erase_sector(uint32_t saddr)
{
    uint8_t res = 0;
    saddr = (saddr * STM32_SECTOR_SIZE) + STM32_FLASH_BASE;     /* STM32F103擦除的时候是指定半字地址 */
    res = stmflash_wait_done(0X5FFFFF);                         /* 等待上次操作结束, >20ms */

    if (res == 0)
    {
        FLASH->CR |= 1 << 1;                    /* 页擦除 */
        FLASH->AR = saddr;                      /* 设置页地址(实际是半字地址) */
        FLASH->CR |= 1 << 6;                    /* 开始擦除 */
        res = stmflash_wait_done(0X5FFFFF);     /* 等待操作结束, >20ms */

        if (res != 1)                           /* 非忙 */
        {
            FLASH->CR &= ~(1 << 1);             /* 清除页擦除标志 */
        }
    }

    return res;
}
```

- 入参 `saddr` 是**页号**（函数注释写 0~256），先换算成地址：`页号 × 2048 + 0x08000000`。
  例：页 64 → `64 × 2048 = 0x20000` → `0x08020000`，正是 `FLASH_SAVE_ADDR`。
- 三步：`PER=1`（声明"要擦页"）→ `AR=` 目标地址 → `STRT=1`（启动）。之后硬件自己擦 **整页 2KB**，
  擦完把页内全部 1024 个半字都变成 `0xFFFF`。
- 等 `BSY` 结束后清掉 `PER`，让 `CR` 回到干净状态。

### 5.4 写半字与整段写：`stmflash_write_halfword()` / `stmflash_write()`

```c
static uint8_t stmflash_write_halfword(uint32_t faddr, uint16_t data)
{
    uint8_t res;
    res = stmflash_wait_done(0X5FFFFF);
    if (res == 0)                           /* OK */
    {
        FLASH->CR |= 1 << 0;                /* 编程使能 */
        *(volatile uint16_t *)faddr = data; /* 写入数据 */
        res = stmflash_wait_done(0X5FFFFF); /* 等待操作完成 */
        if (res != 1)                       /* 操作成功 */
        {
            FLASH->CR &= ~(1 << 0);         /* 清除PG位 */
        }
    }
    return res;
}
```

**寄存器版最"魔法"的一行是 `*(volatile uint16_t *)faddr = data;`**：
CPU 以为自己在给内存写数，其实 `PG=1` 时硬件会把这个总线写操作转成 FLASH 编程脉冲。
所以：地址必须 **2 的倍数**（半字对齐），数据宽度必须 16 位，且写之前必须已解锁。

`stmflash_write()` 的自动擦除逻辑：

```c
if (waddr < STM32_FLASH_BASE || waddr % 2 ||        /* 写入地址小于 STM32_FLASH_BASE, 或不是2的整数倍, 非法. */
    waddr > (STM32_FLASH_BASE + STM32_FLASH_SIZE))  /* 写入地址大于 STM32_FLASH_BASE + STM32_FLASH_SIZE, 非法. */
{
    return;
}
stmflash_unlock();              /* FLASH解锁 */
addrx = waddr;                  /* 写入的起始地址 */
endaddr = waddr + length * 2;   /* 写入的结束地址 */

while (addrx < endaddr)         /* 扫清一切障碍 (对非0XFFFF的地方, 先擦除) */
{
    secnum = (addrx - STM32_FLASH_BASE) / STM32_SECTOR_SIZE;    /* 得到扇区编号 */
    if (stmflash_read_halfword(addrx) != 0XFFFF)    /* 有非0XFFFF的地方,要擦除这个扇区 */
    {
        status = stmflash_erase_sector(secnum);     /* 擦除这个扇区 */
        if (status) break;                          /* 发生错误了 */
    }
    else
    {
        addrx += 4;
    }
}

if (status == 0)            /* 前面操作正常 */
{
    while (waddr < endaddr) /* 写数据 */
    {
        if (stmflash_write_halfword(waddr, *pbuf)) break;
        waddr += 2;
        pbuf++;
    }
}
stmflash_lock();            /* 上锁 */
```

要点：

1. 合法性检查：起始地址必须 ≥`0x08000000` 且是偶数。
2. "扫障碍"阶段是**探测式擦除**：只要发现某个半字不是 `0xFFFF`，就把它所在的整页擦掉；
   擦完不推进 `addrx`，下一轮再读一次确认已是 `0xFFFF` 才继续。`else addrx += 4` 表示"这里没东西要擦，跳两个半字看看"，
   粗筛而已——真正的保证是"擦就擦整页"。
3. 写阶段每 2 字节推进一次，`pbuf++` 因为 `pbuf` 是 `uint16_t*`，自增正好跨过 2 字节。
4. **整页擦除意味着同页的旧数据全部丢失**：如果你在 `0x08020000 ~ 0x080207FF` 这一页里存了 3 组数据，
   改第 1 组会把第 2、3 组一起擦掉。这是"内部 FLASH 当 EEPROM 用"最容易踩的坑，也正是 2.4 节那些设计要解决的问题。

### 5.5 main.c 里的长度宏与缓冲区

```c
const uint8_t g_text_buf[] = {"STM32 FLASH TEST"};
#define TEXT_LENTH sizeof(g_text_buf)       /* 数组长度 */
/* SIZE表示半字长(2字节), 大小必须是2的整数倍, 如果不是的话, 强制对齐到2的整数倍 */
#define SIZE  TEXT_LENTH / 2 + ((TEXT_LENTH % 2) ? 1 : 0)
#define FLASH_SAVE_ADDR     0X08020000      /* 设置FLASH 保存地址(必须为偶数，且其值要大于本代码所占用FLASH的大小 + 0X08000000) */
```

算一遍：`"STM32 FLASH TEST"` 有 16 个可见字符，`sizeof` 含结尾 `\0` → `TEXT_LENTH = 17`；
`SIZE = 17/2 + 1 = 8 + 1 = 9`（半字个数）→ 实际读写 **18 字节**。
`main()` 里 `uint8_t datatemp[SIZE];` 只有 **9 字节**，而 `stmflash_read()` 会往里写 9 个半字＝18 字节，
这是一个**栈越界**隐患（详见第八章）。

### 5.6 USMART 调试入口（本实验已加入编译）

[usmart_config.c](Middlewares/USMART/usmart_config.c) 把这几个函数挂给了串口：

```c
    (void *)delay_ms, "void delay_ms(uint16_t nms)",
    (void *)delay_us, "void delay_us(uint32_t nus)",

    (void *)stmflash_read_halfword, "uint16_t stmflash_read_halfword(uint32_t faddr)",
    (void *)test_write, "void test_write(uint32_t waddr, uint16_t wdata)",
```

外加 `USMART_USE_WRFUNS == 1` 打开的 `read_addr(addr)` / `write_addr(addr,val)`。
用法示例（115200，回车换行结束）：

```text
stmflash_read_halfword(0x08020000)   → 读出保存地址开头那个半字
test_write(0x08030000,0x1234)        → 往第 96 页写一个半字（会先擦整页）
```

## 六、编译、下载与运行

1. Keil MDK 打开 `Projects/MDK-ARM/atk_f103.uvprojx`（源文件含 startup_stm32f103xe.s、main.c、
   delay/sys/usart、led/lcd/key/**stmflash**、**USMART 四个 .c**）。
2. 器件 `STM32F103RC`，包含路径已配好，直接 Rebuild。
3. 输出目录为工程里设置的 `..\..\Output\`（实验根目录下的 `Output/`），产物 `atk_f103.axf`/`.hex`。
4. 下载：ST-LINK / J-LINK（SWD）。
5. 想用 USMART：USB 线接 `USB_UART`，串口助手 **115200**（推荐 XCOM/SSCOM，其它助手可能因 DTR/RTS 复位 MCU），
   并确认 USART1 排针上 PA9→RXD、PA10→TXD 的跳线帽已接。
6. **下载前先确认代码没占用到 0x08020000**：看 Keil Build 输出里的 `Program Size: code=...,const=...`，
   两者之和要远小于 `0x20000`（128KB）。本工程只有 LCD+USMART，通常在几十 KB 量级，落在第 64 页之前是安全的。

## 七、实验现象

1. 上电后 LCD 显示 `STM32` / `FLASH EEPROM TEST` / `ATOM@ALIENTEK` / `KEY1:Write  KEY0:Read`；LED0 约每 200ms 翻转一次。
2. **按一下 KEY1（PA15）**：屏幕下半屏清掉 → 红字 `Start Write FLASH....` → 约几十毫秒后变成 `FLASH Write Finished!`
   （中间那一下"卡顿"就是擦 2KB 页 + 编程 9 个半字耗时，属于正常现象）。
3. **按一下 KEY0（PC5）**：显示 `Start Read FLASH....` → `The Data Readed Is:` → 下一行显示 `STM32 FLASH TEST`
   （没写过就读，读到的是全 0xFFFF，会显示成乱码字符，这也是正常现象）。
4. **按复位键重新上电，再按 KEY0**：内容依然是 `STM32 FLASH TEST` ——这就是"掉电保存"得到验证的时刻，
   也是本实验与"实验14 低功耗/内存实验"最直观的区别（RAM 掉电即失）。
5. 反复按 KEY1：每次都会重新擦第 64 页再写，功能正常，但每次消耗一次擦写寿命（不要拿它做压力测试）。
6. USMART：串口发 `help` 列出可调用函数；发 `stmflash_read_halfword(0x08020000)` 会返回一个十六进制半字，
   写过之后应为 `5354`（`'S'`、`'T'` 两个字符组成的小端半字）。

## 八、注意事项

- **绝对不能往代码区写**（官方 `readme.txt` 第 6 条）：擦掉正在运行的代码所在页 → 取指失败 → 立即死机/跑飞，只能重新烧录。
  `FLASH_SAVE_ADDR` 必须大于"代码 + 常量占用量 + 0x08000000"，且**留一点余量**（编译结果会随功能增加而变大）。
- **地址必须偶数、数据宽度必须是半字**：`waddr % 2` 不合法会被 `stmflash_write()` 直接 return；
  用 `stmflash_write_halfword()` 写奇数地址会置 `PGERR`。
- **擦除是"整页"级别**：一页 2KB 里只要有一处要改，**整页数据一起消失**。
  同一页里既放配置又放日志的话，改一个就会丢另一个——请不同用途分不同页。
- **擦写次数有限（约 1 万次）**：不要把内部 FLASH 当高速日志用。频繁保存请用"页内追加 + 满了才擦"或双页交换计数方案（2.4 节），
  或直接用外部 24C02（实验19 IIC）/ SPI FLASH。
- **擦写期间 CPU 停摆**：擦一页约 20ms，这期间中断、串口接收、触摸屏扫描都会延迟或丢数据。
  现象上表现为"按 KEY1 写的瞬间串口丢几个字节"，不是坏了。
- **代码与官方说明不一致 / 潜在瑕疵（以代码为准）**：
  1. [User/main.c](User/main.c) 文件头 `@brief` 写的是"**无线通信 实验**"，是模板复制残留，实际就是本实验（FLASH 模拟 EEPROM）。
  2. `uint8_t datatemp[SIZE];`（9 字节）被 `stmflash_read(..., (uint16_t*)datatemp, SIZE)` 当作 9 个半字（18 字节）填充，
     **越界 9 字节**，可能踩到相邻栈变量。稳妥改法：`datatemp[SIZE * 2]`，或读取时把长度改成 `SIZE`（半字数）同时缓冲区按 2 倍开。
  3. 写 `g_text_buf`（17 字节）时按 18 字节读，最后一个字节越过数组末尾——因为 `SIZE` 向上取整，实际影响很小，但要明白它多取了 1 字节。
  4. `stmflash.h` 里没有 `PAGE_SIZE` 宏，页大小叫 **`STM32_SECTOR_SIZE`**（本工程生效值 2048）；
     `stmflash.c` 中 `uint16_t g_flashbuf[STM32_SECTOR_SIZE / 2];`（2KB RAM）**声明了但从未被使用**，白占 2KB。
  5. `stmflash_unlock()` 没有先检查 `CR.LOCK` 就写 `KEYR`，功能上没问题（复位后默认是锁着的），
     但如果某处提前解锁过，重复写钥匙并不会报错，读代码时容易误以为"这里没解锁成功"。
- **USMART 是在 TIM4 中断里执行函数的**（`USMART_TIMX_IRQHandler → usmart_dev.scan()`）：
  如果你在 USMART 里调用 `test_write()`，它会在中断上下文里擦 2KB 页（约 20ms），期间按键扫描、LCD、串口都会停。
  **不要同时用 KEY1 写入和串口 USMART 写入**，两条路径都动 FLASH 会互相打断。
- **`read_addr/write_addr` 是双刃剑**：`write_addr` 能直接改任意寄存器/内存地址，玩错了会当场死机（比如改掉栈、改掉 GPIO），
  这是 USMART 演示的固有代价，别在正式产品里开着。
- **别在擦写的中途拔电**：擦除/编程过程中掉电，该页数据处于中间状态，重启后要按"数据无效"处理
  （真实项目里就是靠 2.4 节那种传输计数/标志来判断）。
- **KEY1 在 PA15**：PA15 复位后默认是 JTAG 的 JTDI。本工程只用 SWD 调试，一般不受影响；
  若发现 KEY1 失灵，可用 `sys_gpio_remap_set(24, 3, 2)` 关闭 JTAG、保留 SWD。
- **源码是 GBK 编码**：请用 GBK/GB2312 打开源文件，否则中文注释乱码（本 README 是 UTF-8）。

## 九、总结与延伸

**本实验要点：**

1. 内部 FLASH 当 EEPROM 的三条铁律：**擦以页为单位（本工程 2KB/页）、写以半字为单位且地址必须偶数、擦写寿命约 1 万次**。
2. 页大小由代码里的 `STM32_FLASH_SIZE` + `#if` 推导：256KB 的 STM32F103RC 走 `#else` 分支，`STM32_SECTOR_SIZE = 2048`；
   中容量（≤128KB）器件才是 1024。
3. 寄存器顺序：`CR.LOCK` 锁着 → `KEYR` 连写 `0x45670123`、`0xCDEF89AB` 解锁 → `PER`+`AR`+`STRT` 擦 → `PG`+半字写 →
   全程用 `SR.BSY/PGERR/WRPRTERR` 判忙与错 → 结束置回 `LOCK`。
4. `stmflash_write()` 的"探测式擦除"（读到非 0xFFFF 就擦该页）是内部 FLASH 模拟 EEPROM 的最小可用实现。
5. 掉电保存的完整工程方案要靠**页内追加**或**双页交换 + 传输计数**，把"擦"当成稀缺资源、把"擦到一半掉电"当成必须回退的情况。

**延伸小练习：**

1. 先修缓冲区 bug：把 `datatemp` 改成 `uint8_t datatemp[SIZE * 2 + 1];`（多留一个 `\0` 位置），
   再用 USMART 读 `stmflash_read_halfword(0x08020002)` 验证第二个半字是 `4D33`（"M3"）。
2. 加"写入次数"计数：在第 64 页的第 0 个半字位置存一个累加值——每次 KEY1 写入前先把它读出来、+1、
   连同字符串一起重新写到页首，掉电重启后读回来，你会看到计数在增长（同时体会"每次都要擦整页"的代价）。
3. 自己做一版"少擦几次"的 mini-EEPROM：先用 `0x08020800`（第 65 页）练"页内追加"——每条记录写 `{ID, 值}` 两个半字，
   从页首开始找第一个等于 `0xFFFF` 的槽位写入，页满才擦整页（算一下：一页能写多少条？答案 1024 条）；
   再升级成 2.4 节的双页方案：`0x08020000` 与 `0x08020800` 轮换，加一个"交换计数器"半字，
   人为在搬运中途按复位键，验证重启后仍能读出正确数据（掉电安全）。

**衔接**：本实验处理的是"片内、少量、关键数据"；
实验19 IIC（24C02）、实验21/28/29（SPI FLASH、SD 卡 + FATFS）则把存储搬到片外、容量从 256KB 涨到 GB 级。
把"页擦除 + 半字编程"和"扇区 + 块 + 文件系统"两套模型对比着记，存储这条线就完整了。
