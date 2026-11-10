# 实验4 串口实验

> 一句话概括：用 USART1（PA9 发 / PA10 收）和电脑串口助手"对话"——学会串口寄存器的三件事：**用 `BRR` 算波特率**、**用 `SR` 的 TXE/RXNE/TC 标志判断收发状态**、**用接收中断把一个个字节拼成一行字符串**。这是后面所有"能看到打印输出"的实验的基础。

## 一、实验简介

- **实验目的**（官方 `readme.txt`）：学习串口的使用（接收与发送）。
  通俗展开：串口（UART）是芯片之间最简单的"两个人对讲"方式——一根线我说你听（TX），一根线你说我听（RX），没有时钟线，双方靠事先约好的**波特率**（每秒传多少个 bit）来对齐节奏。STM32 和电脑之间不能直接连（电平不同），所以板上放了一颗 **CH340 USB 转串口芯片**，USB 插电脑就虚拟出一个 COM 口。
- **在学习路线中的位置**：
  - 前置：[实验1](../实验1%20跑马灯实验)（GPIO 复用输出：TX 脚要把 USART 的外设信号"接管"引脚）、[实验3 外部中断实验](../实验3%20外部中断实验)（本实验的接收走中断，NVIC 那套完全一样）。
  - 后续：实验 12 USMART 调试、实验 35 串口 IAP（用本实验的收发做固件升级）、实验 25 无线通信、以及所有"用 printf 调试"的实验都直接依赖 `Drivers/SYSTEM/usart/` 这份代码。
- **硬件平台**：正点原子 MiniSTM32 V4 开发板，主芯片 **STM32F103RC**（Cortex-M3，72MHz），板载 CH340 USB 转串口。

## 二、实验原理

### 2.1 一帧数据长什么样，波特率又是什么

STM32F1 默认帧格式（本工程就是默认值）：**1 位起始位 + 8 位数据位 + 无校验 + 1 位停止位 = 每发 1 个字节要占 10 个 bit 时间**。

- 波特率 115200 表示线上每秒翻转 115200 个 bit，所以传 1 个字节约 `10 / 115200 ≈ 86.8µs`。
- 起始位是低电平（空闲时线是高），接收方看到下降沿开始计时、每位隔 1/115200 秒采一次样。这就是为什么**两边波特率必须一致**，否则收到的全是乱码。

### 2.2 `USART1->BRR` 波特率分频值怎么算（重点）

USART 自己从总线时钟（USART1 在 APB2 上，时钟 = `PCLK2` = 72MHz）分频出 bit 时钟。硬件公式（参考手册）：

```text
波特率 = USARTCLK / (16 × USARTDIV)      （OVERSAMPLING=16，本工程用默认值）
BRR = (DIV_Mantissa << 4) | DIV_Fraction
```

工程里 [usart.c](Drivers/SYSTEM/usart/usart.c) 直接把"16 分频"合并进了一次除法，并做了四舍五入：

```c
    temp = (sclk * 1000000 + baudrate / 2) / baudrate;  /* 得到BRR, 采用四舍五入计算 */
    /* 波特率设置 */
    USART_UX->BRR = temp;       /* 波特率设置 */
```

代入本实验的实际调用 `usart_init(72, 115200)`（第一个参数是**串口时钟源频率，单位 MHz**）：

1. `sclk * 1000000 = 72 × 10⁶ = 72,000,000`
2. `baudrate / 2 = 115200 / 2 = 57,600`
3. `temp = (72,000,000 + 57,600) / 115,200 = 72,057,600 / 115,200 = 625`（整数除法，等价于四舍五入到 625）
4. `BRR = 625 = 0x271` → 高 12 位 `DIV_Mantissa = 0x27 = 39`，低 4 位 `DIV_Fraction = 1` → `USARTDIV = 39 + 1/16 = 39.0625`
5. 验算实际波特率：`72,000,000 / (16 × 39.0625) = 72,000,000 / 625 = 115,200 bps`，**误差 0%**

> 记忆法：因为 `BRR ≈ 串口时钟 / 波特率`（16 倍过采样已经抵消掉了），所以 `72M / 115200 = 625`。如果换 9600 波特率：`(72,000,000 + 4800) / 9600 = 7500`，`BRR = 7500 = 0x1D2C`。
> 另一个要点：**USART1 传 72，USART2~UART5 要传 36**（它们在 APB1，`PCLK1 = HCLK/2 = 36MHz`），传错了波特率就直接偏一倍。

### 2.3 发送流程与三个状态标志：TXE / TC / RXNE

`USART_TypeDef` 只有 7 个寄存器：`SR`、`DR`、`BRR`、`CR1`、`CR2`、`CR3`、`GTPR`（见 [stm32f103xe.h](Drivers/CMSIS/Device/ST/STM32F1xx/Include/stm32f103xe.h)）。收发全靠 `SR`（状态）+ `DR`（数据）+ `CR1`（控制）。

`SR` 里本工程用到的三位：

| 标志 | bit | 掩码 | 含义 | 谁来清 |
| --- | --- | --- | --- | --- |
| **RXNE** | bit5 | `0x20` | 接收数据寄存器**非空**，收到一个字节了，该去读 `DR` | 读 `DR` 自动清 |
| **TC** | bit6 | `0x40` | 发送**完成**：移位寄存器里的最后一个 bit 也发出去了，整条发送彻底结束 | 读 SR 再写 DR，或软件清 |
| **TXE** | bit7 | `0x80` | 发送数据寄存器**空**：可以往 `DR` 写下一个字节了 | 写 `DR` 自动清 |

区别要讲清楚：**TXE 变 1 只说明"你可以把下一个字节交给硬件了"，那个字节可能还在移位寄存器里往外一位一位地发；TC 变 1 才说明"线上真的发完了"**。所以：
- 追求吞吐量的连续发送，等 TXE 就能写下一个（可以提前一格流水）；
- 本工程为了简单可靠（尤其在 RS485 换向、确保末位完整发出的场合），统一等 **TC**：

```c
int fputc(int ch, FILE *f)
{
    while ((USART_UX->SR & 0X40) == 0);     /* 等待上一个字符发送完成 */

    USART_UX->DR = (uint8_t)ch;             /* 将要发送的字符 ch 写入到DR寄存器 */
    return ch;
}
```

**发送编号步骤**（`printf("OK\r\n")` 背后发生的事）：

1. C 库的 `printf` 把格式化结果**一个字符一个字符**地交给它调用的 `fputc`。
2. `fputc` 死等 `SR` 的 bit6（TC）为 1，也就是上一字节彻底发完。
3. 把字符写进 `DR` → 硬件搬到移位寄存器 → 线上出现起始位 + 8 位数据 + 停止位。
4. 重复 1~3 直到字符串结束。

### 2.4 接收流程：中断 + 一个 16 位状态字

接收不是主循环去问"有数据吗"，而是 `CR1.RXNEIE = 1` 打开"接收缓冲区非空中断"：每收到一个字节，CPU 被打断，进入 `USART1_IRQHandler`。

字节是"一个一个"到达的，而上位机发的是"一行字符串"，所以需要一个**拼包状态机**。本工程用 `uint16_t g_usart_rx_sta` 一个变量同时存三件事（[usart.c](Drivers/SYSTEM/usart/usart.c) 注释）：

| 位 | 含义 |
| --- | --- |
| bit15（`0x8000`） | 一帧接收完成标志（收到 `\r\n` 后置 1） |
| bit14（`0x4000`） | 已经收到过 `\r`（0x0d） |
| bit13~0（`0x3FFF`） | 已收到的有效字节个数（也是写入缓冲区的下标） |

判定规则：**必须以 `\r\n`（回车+换行）两个字符结尾才算一行**；先等 `\r`，见到 `\r` 后设 bit14；紧接着必须是 `\n`，否则整包作废重来（`g_usart_rx_sta = 0`）。收到 `\n` 就置 bit15 = 完成，等主循环处理。数据处理完由主循环把 `g_usart_rx_sta` 清 0，才开始接收下一行。

```text
中断里每来一个字节 rxdata：
  ① bit15 已置 1？ → 丢弃（上一行还没被主循环处理，不收新数据）
  ② bit14 已置 1（说明刚收到过 \r）？
        ├─ rxdata == 0x0a → 置 bit15，整行接收完成
        └─ rxdata != 0x0a → g_usart_rx_sta = 0，作废重来
  ③ 还没收到 \r：
        ├─ rxdata == 0x0d → 置 bit14
        └─ 其它 → rx_buf[计数] = rxdata，计数++；超过 USART_REC_LEN-1 则清零重来
```

**"回显（echo）"的完整链路**：

```text
上位机敲键盘 → USB → CH340 → PA10(RX) → USART1 逐字节收 → RXNE 中断 → 存进 g_usart_rx_buf
    → 收到 \r\n 置 bit15 → 主循环检测到 → 把缓冲区逐字节写 DR 从 PA9(TX) 发回去 → 清 g_usart_rx_sta
```

### 2.5 `CR1` 控制寄存器本实验填了什么

`usart_init()` 先把 `CR1 = 0` 再逐位置 1，最终值是 `0x202C`：

| bit | 宏/名称 | 本实验设置 | 作用 |
| --- | --- | --- | --- |
| 0 | OVER8 | 0 | 16 倍过采样（默认） |
| 2 | RE | `1 << 2` = 1 | 接收使能 |
| 3 | TE | `1 << 3` = 1 | 发送使能 |
| 5 | RXNEIE | `1 << 5` = 1 | **RXNE 中断使能**（收到字节就中断） |
| 10 | PCE | 0 | 不用校验位 |
| 12 | M | `0 << 12` = 0 | 8 位数据字长 |
| 13 | UE | `1 << 13` = 1 | USART 总开关（**必须最后打开**） |

停止位不在 CR1 而在 `CR2` 的 `STOP[1:0]`，本工程没动它，保持复位默认 0 = **1 个停止位**。合起来就是串口助手上常见的 **115200, 8, N, 1**。

### 2.6 TX/RX 两个引脚的模式为什么不一样

延续实验 1/2 的 CRL/CRH 表：

| 引脚 | 传给 `sys_gpio_set()` 的参数 | 4 位值 | 模式 | 为什么 |
| --- | --- | --- | --- | --- |
| **PA9 = TX**（发送） | `SYS_GPIO_MODE_AF, OTYPE_PP, SPEED_HIGH` | `0xB`（MODE=11, CNF=10） | **复用推挽输出，50MHz** | 引脚要输出的是 USART 硬件产生的波形，必须让外设"接管"这个脚（CNF[1:0]=10 = 复用输出），推挽保证边沿干净，50MHz 才跑得动 115200 以上的速率 |
| **PA10 = RX**（接收） | `SYS_GPIO_MODE_IN, PUPD_PU` | `0x8`（MODE=00, CNF=10） | **上拉输入** | 串口空闲线本来就是高电平，上拉既符合空闲态、又在没插 USB/没接 CH340 时把 RX 钉在高电平，避免浮空乱收噪声。代码注释特意写了"串口RX脚 必须设置成输入模式" |

落位地址：PA9 → `GPIOA->CRH[7:4] = 0xB`；PA10 → `GPIOA->CRH[11:8] = 0x8`（同时 `GPIOA->ODR` 的 bit10 被置 1 表示上拉）。

## 三、硬件资源与接线

| 资源 | 引脚 | 代码中的宏 / 位置 | 说明 |
| --- | --- | --- | --- |
| USART1_TX（STM32 发出） | **PA9** | `USART_TX_GPIO_PORT/PIN`，[usart.h](Drivers/SYSTEM/usart/usart.h) | 复用推挽输出，`CRH[7:4]=0xB` |
| USART1_RX（STM32 收进） | **PA10** | `USART_RX_GPIO_PORT/PIN` | 上拉输入，`CRH[11:8]=0x8` |
| USART1 外设本体 | — | `USART_UX = USART1`（基址 `0x40013800`） | `SR/DR/BRR/CR1/CR2/CR3/GTPR` |
| USART1 时钟 | — | `USART_UX_CLK_ENABLE()` → `RCC->APB2ENR |= 1<<14` | bit14 = USART1EN |
| GPIOA 时钟 | — | `RCC->APB2ENR` bit2 | TX/RX/LED0 都在 PA |
| GPIOD 时钟 | — | `RCC->APB2ENR` bit5 | LED1 |
| LED0（红灯） | PA8 | `LED0_GPIO_PORT/PIN`，[led.h](Drivers/BSP/LED/led.h) | 低电平点亮，用作"程序在跑"的心跳灯 |
| LED1（绿灯） | PD2 | `LED1_GPIO_PORT/PIN` | 本实验代码未使用 |
| 板载 USB 转串口 | CH340 ↔ PA9/PA10 | 排针丝印 `USART1`（RXD/TXD） | 必须戴跳线帽才连通 |
| USART1 中断 | `USART1_IRQn = 37` | `USART_UX_IRQn` / `USART_UX_IRQHandler` | 使能位在 `NVIC->ISER[1]` bit5 |

**接线（本实验最容易出错的一步）**：
1. USB 线一头插电脑，另一头插到板子上丝印为 **USB_UART** 的那个 Micro USB 口（官方 `readme.txt` 的说法："请用 USB 线连接在 USB_UART，找到 USB 转串口后测试本例程"）。插错口（例如插到给单片机供电/ST-Link 的那个口）电脑上是看不到 CH340 COM 口的。
2. **戴跳线帽**：板子上丝印 `USART1` 的排针有 4 根左右（3V3/5V、TXD、RXD、GND 之类），需要把 **PA9 与 RXD 连一个跳线帽、PA10 与 TXD 连一个跳线帽**。之所以叫"交叉"，是因为 MCU 的发送脚（PA9=TX）要接到 CH340 的接收脚（RXD），MCU 的接收脚（PA10=RX）接 CH340 的发送脚（TXD）——**串口永远 TX 接 RX、RX 接 TX**。不戴跳线的话现象是：单片机在跑（灯闪），但串口助手什么都收不到。
3. 用外部 USB-TTL 模块代替板载 CH340 也一样：模块 GND 接板 GND、模块 RXD 接 PA9、模块 TXD 接 PA10、模块不要接 VCC 到 5V 脚（3.3V 电平即可）。
4. **时钟树**：`sys_stm32_clock_init(9)` → 72MHz，`PCLK2 = HCLK = 72MHz`，这正是 USART1 的时钟源，也是 `usart_init(72, 115200)` 第一个参数必须写 72 的原因。若误写成 36，`BRR` 会算成 `(36×10⁶ + 57600)/115200 = 313`，而硬件仍按 72MHz 分频，实际波特率变成 `72,000,000 / 313 ≈ 230000`（约为目标值的 2 倍），串口助手按 115200 收就是一屏乱码；反过来 USART2~UART5 挂在 APB1（36MHz）上，却传 72，实际波特率就变成一半（约 57600）。

## 四、代码结构与调用流程

```text
实验4 串口实验/
├── Drivers/
│   ├── BSP/LED/led.c / led.h         # LED0 心跳灯（本实验只用 LED0）
│   ├── CMSIS/                        # 芯片头文件 + 启动文件（含 USART1_IRQHandler 向量）
│   └── SYSTEM/
│       ├── usart/usart.c / usart.h   # 本实验主角：usart_init()、fputc 重定向、接收中断服务函数
│       ├── sys/sys.c / sys.h         # sys_gpio_set()（AF/输入模式）、sys_nvic_init()
│       └── delay/delay.c / delay.h   # delay_ms()
├── Projects/MDK-ARM/atk_f103.uvprojx # Keil 工程，Target 名：USART
├── User/main.c                       # 主循环：判收到完成标志→回显；空闲→提示+闪灯
└── readme.txt
```

- `Drivers/SYSTEM/usart/usart.h`：12 个 `USART_xxx` 宏把"用哪个串口、哪两个脚"抽出来，注释说明"通过修改这 12 个宏定义，可以支持 USART1~UART7 任意一个串口"（本芯片 STM32F103RC 实际只有 USART1~USART3 加 UART4/UART5；换到 USART2 及以后时记得它们在 APB1 上，`usart_init()` 的第一个参数要传 36）；另外定义缓冲区上限 `USART_REC_LEN = 200` 和接收使能开关 `USART_EN_RX = 1`。
- `Drivers/SYSTEM/usart/usart.c`：三块内容——printf 支持（禁半主机 + 重定义 `fputc`）、接收缓冲与中断服务函数、`usart_init()`。
- `User/main.c`：只处理"整行收完了怎么办"，收发细节都交给驱动。

`main()` 流程：

1. `sys_stm32_clock_init(9)` → 72MHz（决定 USART1 时钟 = 72MHz）。
2. `delay_init(72)` → SysTick 基准。
3. `usart_init(72, 115200)` → 开 GPIOA/USART1 时钟 → PA9 复用推挽 / PA10 上拉输入 → `BRR=625` → `CR1=0x202C`（含 RXNEIE）→ NVIC 组 2、优先级(3,3)、使能 `USART1_IRQn` → 最后 `UE=1` 打开串口。
4. `led_init()` → PA8/PD2 推挽输出，两灯先灭。
5. `while(1)`：
   - 若 `g_usart_rx_sta & 0x8000`（收到完整一行）→ 取长度 → 打印提示 → 逐字节回写 `DR` 并把每个字符等到 TC → 补两个换行 → `g_usart_rx_sta = 0`。
   - 否则 `times++`，按 `times % 5000 / % 200 / % 30` 分别打印标题、打印提示语、翻转 LED0，然后 `delay_ms(10)`。

## 五、核心代码解析

### 5.1 完整 `User/main.c`（去掉文件头注释）

```c
#include "./SYSTEM/sys/sys.h"
#include "./SYSTEM/usart/usart.h"
#include "./SYSTEM/delay/delay.h"
#include "./BSP/LED/led.h"


int main(void)
{
    uint8_t t;
    uint8_t len;
    uint16_t times = 0; 

    sys_stm32_clock_init(9);    /* 设置时钟, 72Mhz */
    delay_init(72);             /* 延时初始化 */
    usart_init(72, 115200);     /* 串口初始化为115200 */
    led_init();                 /* 初始化LED */

    while (1)
    {
        if (g_usart_rx_sta & 0x8000)        /* 接收到了数据? */
        {
            len = g_usart_rx_sta & 0x3fff;  /* 得到此次接收到的数据长度 */
            printf("\r\n您发送的消息为:\r\n");

            for (t = 0; t < len; t++)
            {
                USART_UX->DR = g_usart_rx_buf[t];
                
                while ((USART_UX->SR & 0X40) == 0); /* 等待发送结束 */
            }

            printf("\r\n\r\n"); /* 插入换行 */
            g_usart_rx_sta = 0;
        }
        else
        {
            times++;

            if (times % 5000 == 0)
            {
                printf("\r\n正点原子 STM32 开发板 串口实验\r\n");
                printf("正点原子@ALIENTEK\r\n\r\n\r\n");
            }

            if (times % 200 == 0) printf("请输入数据,以回车键结束\r\n");

            if (times % 30  == 0) LED0_TOGGLE(); /* 闪烁LED,提示系统正在运行. */

            delay_ms(10);
        }
    }
}
```

逐段说明：
- `g_usart_rx_sta & 0x8000`：只看 bit15（收完整行的标志），主循环**不需要知道中断什么时候发生的**，这就是"中断收、主循环处理"的经典分工。
- `len = g_usart_rx_sta & 0x3fff;`：`0x3fff` 正好屏蔽掉 bit15/bit14 两个标志位，留下 bit13~0 的字节计数。
- 回显没有用 `printf`，而是 `USART_UX->DR = ...` 直接写数据寄存器 + `while ((USART_UX->SR & 0X40) == 0);` 等 TC —— 这是**轮询发送**的最小写法，和 `fputc` 里的逻辑一模一样，目的是让初学者看清"发一个字节要做哪两件事"。注意这里写的是 `USART_UX`（宏，等于 `USART1`），换成别的串口时这行不用改。
- `g_usart_rx_sta = 0;`：必须清掉，否则清完 bit15 之前一直重复回显同一串；清 0 同时把 bit14 和计数清零，等于"重新开始收一行"。
- `else` 分支的三个取模：循环每圈约 10ms（`delay_ms(10)` + printf 本身耗时），所以
  - `times % 30 == 0` → 约每 **300ms** 翻转一次 LED0（看到的现象是红灯以约 0.6 秒为周期闪烁）；
  - `times % 200 == 0` → 约每 **2 秒** 打印一次 `请输入数据,以回车键结束`；
  - `times % 5000 == 0` → 约每 **50 秒** 打印一次实验标题。
  这些都是近似值，实际间隔会因串口发送占用时间而略长。
- `uint8_t len`：`USART_REC_LEN` 是 200，14 位长度字段实际最大也就 199，用 8 位存足够；若把缓冲区改大到 >255，这里要改成 `uint16_t`。

### 5.2 `usart_init()`：发送/接收的整套寄存器配置

```c
void usart_init(uint32_t sclk, uint32_t baudrate)
{
    uint32_t temp;
    /* IO 及 时钟配置 */
    USART_TX_GPIO_CLK_ENABLE(); /* 使能串口TX脚时钟 */
    USART_RX_GPIO_CLK_ENABLE(); /* 使能串口RX脚时钟 */
    USART_UX_CLK_ENABLE();      /* 使能串口时钟 */

    sys_gpio_set(USART_TX_GPIO_PORT, USART_TX_GPIO_PIN,
                 SYS_GPIO_MODE_AF, SYS_GPIO_OTYPE_PP, SYS_GPIO_SPEED_HIGH, SYS_GPIO_PUPD_PU);   /* 串口TX脚 模式设置 */

    sys_gpio_set(USART_RX_GPIO_PORT, USART_RX_GPIO_PIN,
                 SYS_GPIO_MODE_IN, SYS_GPIO_OTYPE_PP, SYS_GPIO_SPEED_HIGH, SYS_GPIO_PUPD_PU);   /* 串口RX脚 必须设置成输入模式 */

    temp = (sclk * 1000000 + baudrate / 2) / baudrate;  /* 得到BRR, 采用四舍五入计算 */
    /* 波特率设置 */
    USART_UX->BRR = temp;       /* 波特率设置 */
    USART_UX->CR1 = 0;          /* 清零CR1寄存器 */
    USART_UX->CR1 |= 0 << 12;   /* M = 0, 1个起始位, 8个数据位, n个停止位(由USART_CR2 STOP[1:0]指定, 默认是0, 表示1个停止位) */
    USART_UX->CR1 |= 1 << 3;    /* TE = 1, 串口发送使能 */
#if USART_EN_RX  /* 如果使能了接收 */
    /* 使能接收中断 */
    USART_UX->CR1 |= 1 << 2;    /* RE = 1, 串口接收使能 */
    USART_UX->CR1 |= 1 << 5;    /* RXNEIE = 1, 接收缓冲区非空中断使能 */
    sys_nvic_init(3, 3, USART_UX_IRQn, 2); /* 组2，最低优先级 */
#endif
    USART_UX->CR1 |= 1 << 13;   /* UE = 1, 串口使能 */
}
```

按步骤拆（这个顺序对任何外设都通用）：

1. **开时钟**：`RCC->APB2ENR` bit2（GPIOA，两次宏展开都是它）+ bit14（USART1）。
2. **配引脚模式**：TX = 复用推挽 50MHz（`CRH[7:4]=0xB`），RX = 上拉输入（`CRH[11:8]=0x8`）。
3. **算并写 `BRR`**：`temp = 625`（详见 2.2 节推算）。
4. **`CR1` 先清零再逐位填**：`CR1 = 0` 这一步顺带把 OVER8（过采样倍数）、校验位等回到默认，保证帧格式是 8N1。`|= 0 << 12` 这行是"写 0 的或运算"，其实什么也没改，只是**用代码注释的形式把 M 位的含义写出来**。
5. **TE/RE/RXNEIE**：发使能、收使能、收中断使能。`USART_EN_RX` 是编译开关，改成 0 就变成纯发送（printf 依然能用）。
6. **`sys_nvic_init(3, 3, USART1_IRQn, 2)`**：组 2 下抢占 3、子 3 = 该组最低优先级（`temp = 3<<2 | 3 = 0xF`，`NVIC->IP[37] |= 0xF0`，`NVIC->ISER[1] |= 1<<5`）。把串口放最低优先级，是为了不让打印打断按键等更紧要的中断。
7. **最后才 `UE = 1`**：串口总开关。先配完波特率/帧格式再开，避免用错误的分频值去采样线上数据。

### 5.3 接收中断服务函数：拼出一行

```c
void USART_UX_IRQHandler(void)
{
    uint8_t rxdata;
    ...
    if (USART_UX->SR & (1 << 5))                /* 接收到数据 */
    {
        rxdata = USART_UX->DR;

        if ((g_usart_rx_sta & 0x8000) == 0)     /* 接收未完成? */
        {
            if (g_usart_rx_sta & 0x4000)        /* 接收到了0x0d? */
            {
                if (rxdata != 0x0a)             /* 接收到了0x0a? (必须先接收到到0x0d,才检查0x0a) */
                {
                    g_usart_rx_sta = 0;         /* 接收错误, 重新开始 */
                }
                else
                {
                    g_usart_rx_sta |= 0x8000;   /* 收到了0x0a,标记接收完成了 */
                }
            }
            else      /* 还没收到0x0d */
            {
                if (rxdata == 0x0d)
                {
                    g_usart_rx_sta |= 0x4000;   /* 标记接收到了 0x0d */
                }
                else
                {
                    g_usart_rx_buf[g_usart_rx_sta & 0X3FFF] = rxdata;   /* 存储数据到 g_usart_rx_buf */
                    g_usart_rx_sta++;

                    if (g_usart_rx_sta > (USART_REC_LEN - 1))g_usart_rx_sta = 0;/* 接收数据溢出, 重新开始接收 */
                }
            }
        }
    }
    ...
}
```

- `if (USART_UX->SR & (1 << 5))`：判 RXNE（bit5）。**读 `DR` 这一动作会自动把 RXNE 清 0**，所以 ISR 里不需要手动清标志——这是串口中断和 EXTI 中断（必须手动清 `EXTI->PR`）的最大差别，也是为什么这个 ISR 看起来"少了清标志那一步"。
- `rxdata = USART_UX->DR;` 必须执行，哪怕后面判定"上一行还没处理完"要丢弃数据：**不读 DR，RXNE 会一直保持，中断会被反复触发**。
- 数据只存"正文"，`\r`/`\n` 两个结束符不进缓冲区，所以回显的内容和收到的正文一致，长度字段也就等于正文字数。
- 溢出保护：`g_usart_rx_sta > (USART_REC_LEN - 1)` 时整个状态清零重收，避免写坏数组。
- `#if SYS_SUPPORT_OS` 里的 `OSIntEnter()/OSIntExit()` 本实验为 0（`sys.h` 里 `SYS_SUPPORT_OS = 0`），到实验 38~40 的 UCOSII 里才会启用。

### 5.4 让 `printf` 走串口：为什么要禁半主机

MDK 默认的标准库 `printf` 会走**半主机（semihosting）**：通过断点指令让调试器代它做 IO。脱离调试器单独运行时，半主机调用会把程序卡死。所以 [usart.c](Drivers/SYSTEM/usart/usart.c) 里做了几件事（不使用 MicroLIB 也能用 printf）：

```c
#pragma import(__use_no_semihosting)

struct __FILE
{
    int handle;
    /* Whatever you require here. If the only file you are using is */
    /* standard output using printf() for debugging, no file handling */
    /* is required. */
};
...
/* MDK下需要重定义fputc函数, printf函数最终会通过调用fputc输出字符串到串口 */
int fputc(int ch, FILE *f)
{
    while ((USART_UX->SR & 0X40) == 0);     /* 等待上一个字符发送完成 */

    USART_UX->DR = (uint8_t)ch;             /* 将要发送的字符 ch 写入到DR寄存器 */
    return ch;
}
```

外加 `_ttywrch()`、`_sys_exit()`、`_sys_command_string()` 三个占位函数（AC6 编译器下改用 `__asm(".global __use_no_semihosting\n\t");` 那一支）。理解要点只有一句：**`printf` → `fputc` → 写 `USART1->DR`**，把 `fputc` 改了，所有 `printf` 的输出就都从串口出来。

## 六、编译、下载与运行

1. Keil MDK 打开 **[Projects/MDK-ARM/atk_f103.uvprojx](Projects/MDK-ARM/atk_f103.uvprojx)**（Target 名 `USART`）。
2. 工程设置（与实验 1~3 一致）：器件 `STM32F103RC`，Pack `Keil.STM32F1xx_DFP.2.3.0`，C 宏 `STM32F103xE`，包含路径 `..\..\Drivers\CMSIS\Device\ST\STM32F1xx\Include;..\..\Drivers\CMSIS\Include;..\..\Drivers;..\..\User`，启动文件 `startup_stm32f103xe.s`；`Drivers/SYSTEM` 组下含 `delay.c`、`sys.c`、`usart.c`。
3. 编译输出到 `..\..\Output\`，文件名 `atk_f103`（`Output/atk_f103.hex`）。
4. 下载：ST-Link / J-Link 走 SWD 排针。**下载用的 SWD 和串口是两条独立通道**，下载时不需要动跳线帽；但如果在 Keil 里把串口当成调试输出（或助手一直占着 COM 口），下载/复位时容易看到半行乱码，建议下载前先断开串口助手连接。
5. 上位机串口助手（官方推荐 **XCOM** 或 **SSCOM**）：
   - 端口选 CH340 对应 COM（设备管理器里"端口(COM 和 LPT)"下能看到 `USB-SERIAL CH340 (COMx)`）。
   - 波特率 **115200**，数据位 8，停止位 1，校验 None。
   - 发送设置里勾"发送新行 / 加回车换行"，让发出的字符串以 `\r\n` 结尾。
   - 中文显示：源文件是 GBK，`printf("您发送的消息为:")` 发出去的也是 GBK 字节，串口助手编码要选 **GBK/ANSI**，否则中文是乱码。

## 七、实验现象

1. 复位/上电后：**LED0（红灯）持续闪烁**（约 300ms 翻转一次），提示程序在跑；LED1 不参与。
2. 打开串口助手并连接对应 COM 口，立刻能看到周期性的提示文字：
   - 约每 2 秒一行 `请输入数据,以回车键结束`；
   - 约每 50 秒一段标题 `正点原子 STM32 开发板 串口实验` / `正点原子@ALIENTEK`。
3. 在发送框输入 `123` 并让结尾带回车换行，点发送 → 板子回显：

   ```
   
   您发送的消息为:
   123
   
   
   ```

4. 输入一串汉字（如 `你好`，GBK 编码）发送，回显原样返回，证明按字节收发与内容无关。
5. 发送**不以 `\r\n` 结尾**的一串字符（比如助手设置成"不加新行"）：板子不会回显，且会一直不收新数据 —— 因为状态机在等 `\r\n`；此时再发一次带 `\r\n` 的完整行，前面那些字符会一起被回显出来（它们已经存进缓冲区了）。
6. 只发 `\r` 再发 `A`：会看到 `A` 被吞掉/重来，因为 `\r` 之后紧跟的不是 `\n` 就执行 `g_usart_rx_sta = 0`，之前的正文被作废。
7. 一次发送超过 200 字节：计数器溢出后状态清零重新接收，表现为"这一行不回显，下一行正常"。

## 八、注意事项

1. **跳线帽**：官方 `readme.txt` 明确提醒"要使用板载的 USB 转串口电路连接到串口助手，需要把丝印为 USART1 的排针上的 PA9/PA10 通过跳线帽等方式分别连接到 RXD/TXD 上"。忘戴跳线是本实验第一大坑，现象是**灯在闪但串口助手一屏空白**。
2. **交叉连接原则**：MCU 的 TX（PA9）一定接对端的 RXD，MCU 的 RX（PA10）接对端的 TXD。接反的典型现象是"能收不能发"或"能发不能收"。
3. **波特率两端必须一致**：助手设 115200，代码 `usart_init(72, 115200)`。乱码基本三种原因：助手波特率不对、`sclk` 参数传错（USART1 是 72，USART2~UART5 是 36）、外部 8MHz 晶振没起振导致主频跑在 HSI 上。
4. **别用会拉 DTR/RTS 的串口助手**：官方 `readme.txt` 特别提醒"请使用 XCOM/SSCOM 串口调试助手，其他串口助手可能控制 DTR/RTS 导致 MCU 复位/程序不运行"。现象是串口助手一连上，板子就重启、只有开机那一刻打印。
5. **发送要带 `\r\n`**：官方说明"串口输入字符串以回车换行结束"，只带 `\n`（LF）或只带 `\r`（CR）都不算完整一行。
6. **`usart_init()` 必须在 `sys_stm32_clock_init(9)` 之后**：波特率分频值依赖 PCLK2 = 72MHz；而且 `sys_stm32_clock_init()` 会把 `RCC->APB2ENR` 清 0，写在它前面的时钟使能全部作废。
7. **中断优先级与分组要统一**：`sys_nvic_init(3, 3, USART_UX_IRQn, 2)` 用的是组 2，和实验 3 的 EXTI 相同。混用不同分组时 `NVIC->IP[ch]` 是 `|=` 写入，旧值残留会让优先级不符合预期。
8. **`printf` 是阻塞的**：115200 下一行 30 个字符要占用约 2.6ms，主循环里的 `times` 计数因此不是精确的 10ms 一格；如果在中断里调用 `printf`，请格外小心（长字符串会把低优先级中断拖在后面，实时性差的根源往往就在这）。
9. **主循环处理期间不收新数据**：`g_usart_rx_sta` 的 bit15 没清之前，中断里 `if ((g_usart_rx_sta & 0x8000) == 0)` 不成立，新字节会被直接丢弃（但仍被读走以清 RXNE）。这是简化的"单缓冲"方案，要边收边处理就得改成双缓冲或 DMA（见实验 15）。
10. **`fputc` 等的是 TC（bit6，`0X40`），不是 TXE（bit7，`0X80`）**：这样能保证最后一个字符彻底上线，代价是每个字符慢一点。若追求连续发送效率，可以改成等 TXE。
11. **官方 `readme.txt` 与代码的细微差异**（以代码为准）：
    - "实验现象"里两条都编号为 `1`（应为 1、2），属排版笔误。
    - `readme.txt` 只说"每隔一定时间，通过串口 1 输出一段信息"，具体间隔在代码里：约每 2 秒一行提示、约每 50 秒一段标题、LED0 约每 300ms 翻转一次（见 [User/main.c](User/main.c) 的 `% 200 / % 5000 / % 30`）。
    - `readme.txt` 未提到需要 GBK 编码才能正常显示中文提示；实际 `printf` 的中文是 GBK 字节，助手编码要选 GBK。
    - `readme.txt` 提到 LED1（PD2）为硬件资源，但本实验代码只在 `led_init()` 里初始化它，主逻辑只操作 LED0。
12. **源码 GBK 编码**：Keil 编辑器编码设为 GBK，避免中文注释和 `printf` 的中文字符串被存成 UTF-8（存成 UTF-8 后串口输出会跟着变成乱码）。

## 九、总结与延伸

**本实验掌握 5 个要点：**

1. 串口 = 帧格式（8N1）+ 波特率；波特率靠 `BRR`，`BRR ≈ 串口时钟 / 波特率`，本工程 `(72×10⁶ + 57600)/115200 = 625 = 0x271`（Mantissa 39、Fraction 1），实际 115200bps 零误差。
2. TX 要配成**复用推挽输出**（`0xB`），RX 要配成**输入**（本工程上拉输入 `0x8`）——同一个 USART，两个脚的 CRL/CRH 模式不同。
3. `CR1` 的关键位：RE(bit2)/TE(bit3)/RXNEIE(bit5)/UE(bit13)，本工程最终 `CR1 = 0x202C`；`UE` 最后开。
4. 三个标志各有分工：**RXNE(bit5) 收到** → 读 `DR`（自动清标志）、**TXE(bit7) 可写**、**TC(bit6) 发完**；本工程发送用等 TC 的轮询方式，串口中断不用手动清标志。
5. "中断收字节 + 状态字 `g_usart_rx_sta` 拼整行 + 主循环处理 + 处理完清 0"是嵌入式里最通用的**行协议接收范式**，后面 IAP、GPS、无线模块解析全靠它。

**延伸小练习（都在本实验代码上改）：**

1. 把波特率改成 9600：`usart_init(72, 9600)`，并把助手同步改到 9600，顺手验证 `BRR` 应该等于 7500（`0x1D2C`）。
2. 用 `fputc` 的思路改写 `main.c` 里的回显循环，把 `USART_UX->DR = ...; while(...)` 换成 `putchar(g_usart_rx_buf[t])`，体会"驱动函数复用"与"直接操作寄存器"两种写法等价。
3. 把状态机改成"以 `#` 结尾算一行"（或不看结尾符、收到 10 个字符就上报），需要同时改 [usart.c](Drivers/SYSTEM/usart/usart.c) 的 ISR 和 `main.c` 的判据——做完你就掌握了自定义协议。
4. 加一个命令解析：回显后判断缓冲区内容是否为 `"led0"`，是就翻转 LED0；这实际上就是实验 35 串口 IAP 与实验 12 USMART 的雏形。

**衔接下一步**：本实验把"printf 调试通道"打通了。之后 [实验5 独立看门狗实验](../实验5%20独立看门狗实验) 会用它观察"程序跑飞后被复位"的打印；[实验15 DMA实验](../实验15%20DMA实验) 把逐字节中断接收换成 DMA，效率更高；[实验35 串口IAP实验](../实验35%20串口IAP实验) 直接建立在本实验的接收状态机之上。
