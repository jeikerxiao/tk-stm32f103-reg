# 实验19 IIC实验

> 一句话概括：只用两根普通 IO（PC12 当时钟 SCL、PC11 当数据 SDA），靠软件延时"手搓"出 IIC 时序，驱动板载 24C02（EEPROM）完成字节的读写与整块字符串的存取消失。

## 一、实验简介

- **实验目的**（官方 `readme.txt`）：学习 STM32 模拟 IIC 的使用，驱动 24C02 实现 EEPROM 数据读写。
  通俗展开：IIC（也叫 I2C）是一种"两根线带一串芯片"的串行通信总线——SCL 是时钟线（谁主控谁打节拍），SDA 是数据线（一次传 1 位）。本实验**不使用 STM32 的硬件 IIC 外设**，而是用普通 GPIO 按时间点高低电平，完全靠软件复现协议时序。这样做的好处是任何 IO 都能当 IIC 用、逻辑一目了然；代价是速度由延时决定、CPU 要陪着等。
- **在学习路线中的位置**：
  - 前置：实验1 跑马灯（GPIO 输出）、实验2 按键输入（GPIO 输入）、实验11 TFTLCD 实验（显示）、实验12 USMART 调试实验（串口命令调用函数）。
  - 后续：实验21 触摸屏实验（用电容屏时同样用这套软件 IIC 读 GT9xxx/FT5206 寄存器）、实验24 DHT11 等单总线/时序类外设（思路一致：时序 = 延时）、实验26 FLASH 模拟 EEPROM（同样是"掉电保存"的方案）。
- **开发板与芯片**：正点原子 MiniSTM32 V4，主芯片 STM32F103RC，72MHz；板载 AT24C02（2Kbit EEPROM）挂在 PC11/PC12 上。

## 二、实验原理

### 2.1 IIC 的三条基本约定

1. **两根线都是"线与"**：任何设备只能把线拉低，不能"推"高。高电平由上拉电阻给（SCL 在本实验由 STM32 推挽输出主动驱动；SDA 用**开漏输出**，输出 1 时其实相当于不驱动，靠外部上拉回高，所以从机才可能把它拉低返回应答）。
2. **SCL 高电平期间 SDA 必须稳定**，只有在 SCL 低电平时才允许改变 SDA——唯一的例外就是起始/停止信号（见下）。
3. **每传 8 位（1 字节）后接 1 个 ACK 位**：主机释放 SDA，从机把它拉低 = ACK（收到了）；保持高 = NACK（没收到 / 数据到此结束）。

数据位序是**高位在前（MSB first）**，代码里发送用 `(data & 0x80) >> 7`、接收用 `receive <<= 1` 体现。

### 2.2 起始 / 停止 / 应答的电平变化（编号步骤 + 表格）

**起始信号 `iic_start()`**——SCL 保持高时，把 SDA 从高拉低：

```c
void iic_start(void)
{
    IIC_SDA(1);
    IIC_SCL(1);
    iic_delay();
    IIC_SDA(0);     /* START信号: 当SCL为高时, SDA从高变成低, 表示起始信号 */
    iic_delay();
    IIC_SCL(0);     /* 钳住I2C总线，准备发送或接收数据 */
    iic_delay();
}
```

**停止信号 `iic_stop()`**——SCL 保持高时，把 SDA 从低抬到高：

```c
void iic_stop(void)
{
    IIC_SDA(0);     /* STOP信号: 当SCL为高时, SDA从低变成高, 表示停止信号 */
    iic_delay();
    IIC_SCL(1);
    iic_delay();
    IIC_SDA(1);     /* 发送I2C总线结束信号 */
    iic_delay();
}
```

| 信号 | 步骤 | SCL | SDA |
| --- | --- | --- | --- |
| 空闲 | — | 高 | 高 |
| 起始 START | 1 | 高 | 高 |
| | 2 | 高 | **高 → 低**（SCL 高期间的下降沿） |
| | 3 | 低 | 低（钳住总线，准备发数据） |
| 停止 STOP | 1 | 低 | 低 |
| | 2 | 高 | 低 |
| | 3 | 高 | **低 → 高**（SCL 高期间的上升沿） |
| ACK（从机应答） | 1 | 低 | 主机释放（写 1） |
| | 2 | 高 | 从机拉 **低** = ACK；保持高 = NACK |
| | 3 | 低 | 从机释放，进入下一位 |

主机发送 ACK 由 `iic_ack()` 完成（SDA 先拉低再给一个 SCL 脉冲，最后 `IIC_SDA(1)` 释放）；发送 NACK 由 `iic_nack()` 完成（SDA 保持 1 给一个 SCL 脉冲）。

**等待从机应答 `iic_wait_ack()`**：主机释放 SDA、抬高 SCL，然后死看 `IIC_READ_SDA`。若一直是 1（没有 ACK），累加 `waittime`，超过 250 次就 `iic_stop()` 并返回 1（失败），防止从机不在场时永久卡住。

### 2.3 速度由 `iic_delay()` 决定（本实验的核心数字）

所有节拍都由这一个函数控制（[myiic.c](Drivers/BSP/IIC/myiic.c)）：

```c
static void iic_delay(void)
{
    delay_us(2);    /* 2us的延时, 读写速度在250Khz以内 */
}
```

看发送 1 个位的过程（`iic_send_byte()` 的循环体）：`IIC_SDA(bit)` → `iic_delay()`(2µs) → `IIC_SCL(1)` → `iic_delay()`(2µs) → `IIC_SCL(0)` → 移位。也就是 **SCL 高电平约 2µs、低电平约 2µs 再多一点**，一个位周期约 4µs + 若干 GPIO 写入/函数调用开销：

```text
fSCL ≈ 1 / (2 × delay) ≈ 1 / 4µs = 250KHz（上限；实测因开销略低，约 200KHz）
```

与标准 IIC 速率档对照：

| 模式 | SCL 频率 | 手册要求（最小值） | 本工程 delay_us(2) 是否满足 |
| --- | --- | --- | --- |
| 标准模式 Standard | 100KHz | 高电平 ≥ 4.0µs、低电平 ≥ 4.7µs | 不满足（节拍偏快） |
| 快速模式 Fast | 400KHz | 高电平 ≥ 0.6µs、低电平 ≥ 1.3µs | 满足 |

**结论**：代码里的 2µs 延时把总线跑得比 100KHz 更快（约 200～250KHz），24C02 支持到 400KHz，所以完全没问题；但如果你换一个只支持 100KHz 的器件（很多传感器、某些 EEPROM），就要把 `iic_delay()` 改成 `delay_us(5)`（高低各 ≥5µs，频率约 ≤100KHz）。这也是"软件模拟时序"最直接的调参入口。

### 2.4 24C02：器件地址、内存地址与页写限制

- **容量与地址范围**：AT24C02 = 2Kbit = **256 字节**，内存地址 0～255。代码用 `#define EE_TYPE AT24C02`（值为 255）表示末地址，`at24cxx_check()` 就是在地址 255 上写/读一个 0x55 做自检。
- **器件地址（第 1 字节）格式**：`1 0 1 0 A2 A1 A0 R/W`。板子把 A2/A1/A0 都接地，所以写命令 = `0b10100000 = 0XA0`，读命令 = `0b10100001 = 0XA1`（最低位是读写标志：0 写、1 读；7 位地址则是 0x50）。
- **容量更大的型号怎么扩址**（代码注释里讲得很清楚，也是那行 `0XA0 + ((addr >> 8) << 1)` 的由来）：24C04/08/16 会把器件地址里的 A2/A1/A0 借去做**内存地址高位 a8/a9/a10**；24C32 以上则直接两个字节发地址（代码里的 `if (EE_TYPE > AT24C16)` 分支）。对本工程的 24C02，`addr >> 8` 恒为 0，发出去就是 0XA0。
- **一次字节写的完整时序**（`at24cxx_write_one_byte()`）：

```text
START → 0XA0 → ACK → 内存地址(低字节) → ACK → 数据 → ACK → STOP → 等待内部写周期(代码 delay_ms(10))
```

- **一次字节读的完整时序**（`at24cxx_read_one_byte()`，注意中间是"重复起始"而不是先 STOP 再 START）：

```text
START → 0XA0 → ACK → 内存地址 → ACK → 重复START → 0XA1 → ACK → 数据(主机回 NACK) → STOP
```

- **页写（write buffer / page write）**：24C02 支持一次传多个字节（最多 **8 字节 = 1 页**），命令格式是 `START → 0XA0 → 地址 → data0 → data1 … data7 → STOP`，从机收到 STOP 后才一次性搬进 EEPROM 阵列。**限制有两条**：① 一页 8 字节，写入地址必须落在同一页内（页边界是 8 的整数倍），否则地址指针在页内回卷，把本已经写好的字节覆盖掉；② 每个写周期（写缓冲/擦写）需要几毫秒，期间器件不应答，必须等 `tWR`（典型 5ms，本工程保守取 10ms）。
  本工程的 `at24cxx_write()` **没有用页写**，而是逐字节调用 `at24cxx_write_one_byte()`：

```c
void at24cxx_write(uint16_t addr, uint8_t *pbuf, uint16_t datalen)
{
    while (datalen--)
    {
        at24cxx_write_one_byte(addr, *pbuf);
        addr++;
        pbuf++;
    }
}
```

好处是永远不会跨页出错、最稳；代价是每字节都要等 10ms，写 19 字节约需 190ms 以上。用页写可以把 8 字节的等待压缩成 1 次。
- **擦写寿命**：EEPROM 是靠电荷 trapping 存数据的，每个存储位置的写入次数有限（AT24C02 手册典型值为**每位置 100 万次**量级，具体以器件手册为准）。所以不要做"每 10ms 写一次同一个地址"这种程序；要频繁记录的东西先在 RAM 攒着，再批量写、或做磨损均衡。EEPROM 的优点是**掉电不丢**。

### 2.5 寄存器层面：这两根脚是怎么配出来的

`iic_init()`（[myiic.c](Drivers/BSP/IIC/myiic.c)）：

```c
    /* SCL引脚模式设置,推挽输出,上拉 */
    sys_gpio_set(IIC_SCL_GPIO_PORT, IIC_SCL_GPIO_PIN,
                 SYS_GPIO_MODE_OUT, SYS_GPIO_OTYPE_PP, SYS_GPIO_SPEED_MID, SYS_GPIO_PUPD_PU);

    /* SDA引脚模式设置,开漏输出,上拉, 这样就不用再设置IO方向了, 开漏输出的时候(=1), 也可以读取外部信号的高低电平 */
    sys_gpio_set(IIC_SDA_GPIO_PORT, IIC_SDA_GPIO_PIN,
                 SYS_GPIO_MODE_OUT, SYS_GPIO_OTYPE_OD, SYS_GPIO_SPEED_MID, SYS_GPIO_PUPD_PU);

    iic_stop();     /* 停止总线上所有设备 */
```

`sys_gpio_set()` 会按引脚号去改 `GPIOC->CRH`（PC11/PC12 都在高位组，位偏移分别是 `(11-8)*4 = 12` 与 `(12-8)*4 = 16`），组合出：

| 引脚 | 输出位段 | CNF/MODE | 效果 |
| --- | --- | --- | --- |
| PC12 = SCL | `CRH[19:16]` | CNF=00、MODE=01 | 通用**推挽**输出，最大 10MHz |
| PC11 = SDA | `CRH[15:12]` | CNF=01、MODE=01 | 通用**开漏**输出，最大 10MHz（写 1 = 高阻，可读回） |

两点细节：① 时钟来自 `RCC->APB2ENR |= 1 << 4`（IOPCEN）；② 最后的 `iic_stop()` 是把总线置于"空闲（两线皆高）"状态，避免上电时从机残留半截状态机。
另外 `IIC_SCL(x)/IIC_SDA(x)` 宏最终调用 `sys_gpio_pin_set()`，它写的是 `GPIOx->BSRR`（置位/复位寄存器，一次写入原子完成），比读改写 `ODR` 更安全、更快。

> 关于"上拉"：`sys_gpio_set()` 的 `pupd` 参数只在**输入模式**分支里才会去写 `ODR` 形成上下拉，输出模式下这个参数不起作用。因此 SDA（以及外部扩展模块）的高电平必须由**板上的外部上拉电阻**提供——这也是 IIC 总线的硬性要求，把模块插反/接了没有上拉的自制板就会表现为"永远等不到 ACK"。

## 三、硬件资源与接线

| 资源 | 引脚 | 代码中的宏/位置 | 说明 |
| --- | --- | --- | --- |
| IIC SCL | **PC12** | `IIC_SCL_GPIO_PORT/PIN = GPIOC/SYS_GPIO_PIN12`，[myiic.h](Drivers/BSP/IIC/myiic.h) | 推挽输出 |
| IIC SDA | **PC11** | `IIC_SDA_GPIO_PORT/PIN = GPIOC/SYS_GPIO_PIN11` | 开漏输出，读 ACK/读数据也用它 |
| 24C02 (AT24C02) | 板载 | `EE_TYPE = AT24C02`，[24cxx.h](Drivers/BSP/24CXX/24cxx.h) | 器件地址 0XA0/0XA1，容量 256 字节 |
| KEY1 | PA15 | `KEY1_GPIO_PIN`，`KEY1_PRES = 2` | 按下 = 写入 24C02 |
| KEY0 | PC5 | `KEY0_GPIO_PIN`，`KEY0_PRES = 1` | 按下 = 读取 24C02 |
| KEY_UP (WK_UP) | PA0 | `WKUP_GPIO_PIN`，`WKUP_PRES = 3` | 本实验未使用 |
| LED0 | PA8 | `LED0_GPIO_PIN` | 心跳闪烁；检测失败时快速闪烁 |
| 串口1 + USMART | PA9/PA10 | `usart_init(72, 115200)`、`usmart_dev.init(72)` | 115200 8N1，需在 USART1 排针把 PA9/PA10 跳线到 RXD/TXD |
| TFTLCD | 数据 PB0～PB15，RD=PC6、WR=PC7、RS=PC8、CS=PC9、BL=PC10 | [lcd.h](Drivers/BSP/LCD/lcd.h) | 8080 并口 MCU 屏，不支持 RGB 屏 |

- **接线**：AT24C02 已在板载（IIC 接口处），**无需外接**。若你要用杜邦线接外置 24C02 模块：模块的 SCL→PC12、SDA→PC11、VCC→3.3V、GND→GND，并确认模块自带 4.7kΩ 左右的上拉电阻。
- **时钟**：`sys_stm32_clock_init(9)` 把 8MHz 外部晶振经 PLL 9 倍频到 72MHz（APB2=72MHz、APB1=36MHz）。这里 IIC 是 GPIO 软件时序，与总线时钟无关，只与 `delay_us()` 的精度有关——而 `delay_init(72)` 正是用 SysTick（HCLK=72MHz）实现微秒计时的，所以主频设置错了，IIC 速度也会跟着错。

## 四、代码结构与调用流程

```text
实验19 IIC实验/
├── Drivers/BSP/
│   ├── IIC/myiic.c|myiic.h       # 软件 IIC 底层：init/start/stop/ack/nack/wait_ack/send_byte/read_byte
│   ├── 24CXX/24cxx.c|24cxx.h     # EEPROM 上层：init/check/read_one_byte/write_one_byte/read/write
│   ├── KEY/key.c|key.h           # KEY0/KEY1/WK_UP 扫描
│   ├── LED/led.c|led.h           # LED0/LED1
│   └── LCD/lcd.c|lcd.h           # 8080 屏显示
├── Middlewares/USMART/           # 串口命令行（本工程已编译 4 个 usmart 源文件）
├── User/main.c
└── Projects/MDK-ARM/atk_f103.uvprojx
```

层次关系：`24cxx.c`（协议层，"说什么"）→ `myiic.c`（时序层，"怎么拉线"）→ `sys_gpio_pin_set/get`（寄存器层，写 `BSRR`/读 `IDR`）。

`main()` 流程：

1. `sys_stm32_clock_init(9)` → `delay_init(72)` → `usart_init(72, 115200)` → `usmart_dev.init(72)` → `led_init()` → `lcd_init()` → `key_init()` → `at24cxx_init()`（内部只做 `iic_init()`）。
2. 显示标题与提示 `KEY1:Write  KEY0:Read`。
3. `while (at24cxx_check()) { … }`：检测不到 24C02 就一直打印 `24C02 Check Failed!` / `Please Check!` 并闪 LED0；成功则显示 `24C02 Ready!`。
4. 主循环：`key_scan(0)` → KEY1 按下则清半屏、显示 `Start Write 24C02....`、`at24cxx_write(0, g_text_buf, TEXT_SIZE)`、显示 `24C02 Write Finished!`；KEY0 按下则 `at24cxx_read(0, datatemp, TEXT_SIZE)` 并把读到的字符串显示出来。每 20 次循环（约 200ms）翻转 LED0。

## 五、核心代码解析

### 5.1 `User/main.c` 关键片段（全文逻辑即此）

```c
/* 要写入到24c02的字符串数组 */
const uint8_t g_text_buf[] = {"STM32F103 IIC TEST"};

#define TEXT_SIZE       sizeof(g_text_buf)  /* TEXT字符串长度 */
```

`sizeof` 作用于数组会把结尾的 `\0` 也算进去，所以 `TEXT_SIZE = 19`（18 个可见字符 + 1 个结束符），这 19 个字节都会写进 24C02 的地址 0～18。`datatemp[TEXT_SIZE]` 长度与之相同，因此 `lcd_show_string(..., (char *)datatemp, BLUE)` 能按 `\0` 正确截断显示。

### 5.2 发送/接收一个字节

```c
void iic_send_byte(uint8_t data)
{
    uint8_t t;
    
    for (t = 0; t < 8; t++)
    {
        IIC_SDA((data & 0x80) >> 7);    /* 高位先发送 */
        iic_delay();
        IIC_SCL(1);
        iic_delay();
        IIC_SCL(0);
        data <<= 1;     /* 左移1位,用于下一次发送 */
    }
    IIC_SDA(1);         /* 发送完成, 主机释放SDA线 */
}
```

- 先摆 SDA、再抬 SCL：数据在 SCL 上升沿被从机采样，符合"低电平期间改数据、高电平期间读数据"的约定。
- 注意 SDA 写完之后只等了 2µs 就抬 SCL，所以每一位实际时长由 `iic_delay()` 决定（见 2.3）。
- 循环结束 `IIC_SDA(1)` 是**故意**的：把 SDA 交还给从机，紧接着主机就能在 `iic_wait_ack()` 里读到从机的 ACK。

`iic_read_byte(ack)` 反过来：主机在 SCL 低电平期间抬 SCL，在 SCL 高电平期间采样 `IIC_READ_SDA`，8 位边收边 `receive <<= 1`（先收到的是高位），收完根据 `ack` 参数发 ACK 或 NACK。读最后一个字节时一定传 `ack = 0`（NACK），告诉从机"我读完了"。

### 5.3 带"重复起始"的读时序（`at24cxx_read_one_byte`）

```c
    iic_start();                    /* 发送起始信号 */
    ...
    else 
    {
        iic_send_byte(0XA0 + ((addr >> 8) << 1));   /* 发送器件 0XA0 + 高位a8/a9/a10地址,写数据 */
    }
    
    iic_wait_ack();                 /* 每次发送完一个字节,都要等待ACK */
    iic_send_byte(addr % 256);      /* 发送低位地址 */
    iic_wait_ack();                 /* 等待ACK, 此时地址发送完成了 */
    
    iic_start();                    /* 重新发送起始信号 */ 
    iic_send_byte(0XA1);            /* 进入接收模式, IIC规定最低位是0, 表示读取 */
    iic_wait_ack();                 /* 每次发送完一个字节,都要等待ACK */
    temp = iic_read_byte(0);        /* 接收一个字节数据 */
    iic_stop();                     /* 产生一个停止条件 */
    return temp;
```

读操作要"先写地址、再切成读方向"，方向切换不能 STOP→START（那样从机会丢掉当前地址指针），所以要发**重复起始（Sr）**：直接再调一次 `iic_start()`。`addr % 256` 取低 8 位地址（与 `addr & 0XFF` 等价）。

### 5.4 写入后的 10ms 等待与自检

```c
    iic_send_byte(data);            /* 发送1字节 */
    iic_wait_ack();                 /* 等待ACK */
    iic_stop();                     /* 产生一个停止条件 */
    delay_ms(10);                   /* 注意: EEPROM 写入比较慢,必须等到10ms后再写下一个字节 */
```

STOP 之后 24C02 才开始把数据搬进阵列（内部写周期），这期间它不会应答任何请求；本工程用固定 10ms 等待（比典型值 5ms 保守）。更讲究的做法是轮询 ACK：反复发 `START + 0XA0`，直到不再超时为止。

`at24cxx_check()` 的策略是"读末地址看是不是 0x55，不是就写一次 0x55 再读回来验证"，这样首次上电会自动完成一次真实写入，之后开机只做只读检测，省寿命。

## 六、编译、下载与运行

1. Keil MDK 打开 `Projects/MDK-ARM/atk_f103.uvprojx`；器件 **STM32F103RC**，启动文件 `startup_stm32f103xe.s`；包含路径 `..\..\Drivers\CMSIS\Device\ST\STM32F1xx\Include;..\..\Drivers\CMSIS\Include;..\..\Drivers;..\..\User;..\..\Middlewares`。
2. 编译（F7）产物在 `Output/`（`atk_f103.axf/bin`）；下载用 ST-Link / J-Link（SWD）或串口 ISP（BOOT0=1）。
3. 想玩 USMART：USB 线接 **USB_UART**，XCOM/SSCOM 波特率 **115200**、8N1，命令以回车换行结尾。本工程注册的 24C02 函数：
   - `at24cxx_read_one_byte(addr)` —— 读任意地址，如 `at24cxx_read_one_byte(10)`
   - `at24cxx_write_one_byte(addr,data)` —— 写任意地址，如 `at24cxx_write_one_byte(10,99)`
   - 以及 `read_addr/write_addr`（直接读写任意芯片地址，可用来观察寄存器值）、`delay_ms/delay_us`。
   输入 `help` 可列出全部命令。

## 七、实验现象

1. 上电后 LCD 显示 `STM32 / IIC TEST / ATOM@ALIENTEK / KEY1:Write KEY0:Read`，随后出现 **`24C02 Ready!`**（第一次上电时 check 会自动写一次，仍应显示 Ready）。
2. 按 **KEY1**：屏幕下半部被清成白色，先显示 `Start Write 24C02....`，约 0.2～0.3 秒（19 字节 × 每字节 10ms + 时序时间）后显示 `24C02 Write Finished!`。
3. 按 **KEY0**：显示 `Start Read 24C02....`，随后两行是 `The Data Readed Is:` 与 `STM32F103 IIC TEST`——**这时即使按复位键、甚至断电重上电再按 KEY0，内容仍然在**，这就是 EEPROM 的意义。
4. 如果屏幕上循环刷 `24C02 Check Failed!` / `Please Check!` 且 LED0 快速闪烁：说明器件没应答（模块没插好/接线错/无上拉/PC11、PC12 被别的功能占用）。
5. 串口里执行 `at24cxx_write_one_byte(100,123)` 再执行 `at24cxx_read_one_byte(100)`，返回值应为 `7B`（USMART 默认以十六进制显示参数与返回值）。
6. 若用示波器/逻辑分析仪看 PC11、PC12：KEY1 写入期间能看到 SCL 一串约 200KHz 的时钟、起始/停止条件与每字节后的 ACK 低电平位。

## 八、注意事项

1. **SDA 必须是开漏 + 外部上拉**。本工程把 PC11 配成开漏输出，写 1 即释放总线，靠外部上拉回高；如果误配成推挽输出，主机的"高"会和从机的"低"打架（短路竞争），表现为读回全是 0xFF/0x00、器件发热。
2. **`sys_gpio_set()` 在输出模式下不会启用内部上下拉**（`pupd` 参数只在输入模式分支里生效），所以别指望靠内部上拉驱动总线——外接模块务必确认有上拉电阻（典型 4.7kΩ）。
3. **速度取决于 `iic_delay()` 的 `delay_us(2)`**：约 200～250KHz，适配 24C02（≤400KHz）。接 100KHz 器件时改成 `delay_us(5)`。反过来，延时给得太小（比如 0）会因 GPIO 写入开销不足以保证波形建立而读错数据。
4. **写完必须等内部写周期**：`at24cxx_write_one_byte()` 尾部的 `delay_ms(10)` 不能删；自己写页版时要记得"一次页写完也要等 5～10ms"。
5. **页写不要跨页**：24C02 页大小 8 字节，起始地址 `addr % 8` 不为 0 时，一页最多只能写 `8 - addr % 8` 个字节，超出部分会在页内回卷覆盖旧数据。本工程逐字节写，天然规避该问题。
6. **别频繁写同一地址**：EEPROM 有擦写寿命（AT24C02 手册典型 100 万次/位，以器件手册为准），日志类高频数据建议先缓存再批量写，或改用 FLASH 模拟 EEPROM（实验26）/外部 FRAM。
7. **不要在写入过程中掉电**，否则该次写入的数据可能损坏（EEPROM 只保证写完的数据）。
8. **代码与官方说明的一致性**：本实验 `readme.txt` 的引脚分配（`IIC_SCL - PC12`、`IIC_SDA - PC11`）与代码完全一致。需要留意的小差异有两处：① `at24cxx_read_one_byte()` 里 `iic_send_byte(0XA1);` 行尾注释写"IIC规定最低位是0, 表示读取"，表述不准确——0xA1 的最低位是 **1**，1 表示读、0 表示写；② `at24cxx_check()` 的 else 分支里第二次读取写死了 `at24cxx_read_one_byte(255)`，而不是变量 `addr`，对 24C02 恰好相同，但若把 `EE_TYPE` 改成更大型号，这段自检就与前面的写入地址不匹配了。
9. 其它排查：LCD 黑屏先确认 MCU 屏与背光；串口无响应先确认 PA9/PA10 跳线帽和 115200；程序卡在 `iic_wait_ack()` 的 `while` 里但最终返回 1，一般是总线被"拉住"（从机复位时正在输出、或 SDA 短路）。

## 九、总结与延伸

**本实验掌握的要点**：

1. IIC 的三个关键时刻：START（SCL 高时 SDA 下降）、STOP（SCL 高时 SDA 上升）、ACK（第 9 个时钟由从机拉低 SDA）。
2. 软件模拟时序 = GPIO + 延时；`iic_delay()` 的一个数值直接决定总线频率（`f ≈ 1/(2×delay)`），这是移植/适配低速器件的第一旋钮。
3. 24C02 的寻址分两层：器件地址 `1010 A2A1A0 R/W`（选芯片 + 定方向）与内存地址（0～255）；读操作要用**重复起始**切换方向。
4. EEPROM 的两个硬约束：内部写周期（等 5～10ms）与页缓冲（24C02 每页 8 字节，不可跨页）。
5. 开漏输出 + 外部上拉、`BSRR` 原子置复位、`IDR` 读回——这三点是 STM32 上"一线两用"IO 的标准做法。

**延伸小练习**：

1. 把 `at24cxx_write()` 改成页写版本：按 8 字节分组，每组 `START → 0XA0 → 页内地址 → 最多 8 个数据 → STOP → delay_ms(10)`，对比写 19 字节的耗时差异。
2. 在 `main.c` 里加一个"写入次数计数器"：开机读地址 250 的计数器 +1 再写回，LCD 显示复位/开机次数，体会 EEPROM 的掉电保存特性与寿命顾虑。
3. 用 USMART 的 `read_addr(0x40011008)` 观察 `GPIOC->IDR`，在按键与通信过程中看 PC11/PC12 的真实电平（bit11/bit12）。
4. 把 `iic_delay()` 换成空函数（不延时），观察读数据是否开始出错，理解"GPIO 写入本身只有几十纳秒，不足以构成可靠时序"。

**衔接**：这套 `myiic.c` 会被实验21 触摸屏实验直接复用（读写电容屏 GT9xxx/FT5206 的寄存器）；与之对称的"另一根线自己打节拍"的总线是 SPI（实验20），可以对照两者的时序差异。
