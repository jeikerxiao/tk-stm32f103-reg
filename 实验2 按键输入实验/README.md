# 实验2 按键输入实验

> 一句话概括：把三个独立按键（KEY0/KEY1/KEY_UP）配成 GPIO 输入，用轮询 + 软件消抖读出按键并按键值翻转对应的 LED——学会"寄存器版怎么把一个 IO 口当输入用"，顺便把实验 1 的 CRL/CRH 知识在输入模式下补齐。

## 一、实验简介

- **实验目的**（官方 `readme.txt`）：学习 GPIO 作为输入的使用。
  通俗展开：输出是"芯片决定引脚电压"，输入是"芯片去测引脚电压"。输入模式下要操心的事变成三件：① 引脚不接东西时它该停在什么电平（浮空 / 上拉 / 下拉）；② 怎么把这个电平读进 CPU（读 `IDR`）；③ 机械按键按下瞬间会抖动几毫秒，怎么滤掉（消抖）。
- **在学习路线中的位置**：
  - 前置：[实验1 跑马灯实验](../实验1%20跑马灯实验)（本实验直接沿用它的 `sys_gpio_set()`、`led_init()`、`delay_ms()`，只是新增"输入模式"和"读引脚"两件事）。
  - 后续：[实验3 外部中断实验](../实验3%20外部中断实验) —— 按键还是那三个按键、还是那三个引脚，只是把"程序主动去轮询"换成"硬件自动通知 CPU（中断）"。理解了本实验的输入配置，实验 3 只需要再加 EXTI/NVIC。
- **硬件平台**：正点原子 MiniSTM32 V4 开发板，主芯片 **STM32F103RC**（Cortex-M3，72MHz）。

## 二、实验原理

### 2.1 输入模式下 CRL/CRH 的 4 位怎么填（本实验重点）

回忆实验 1：每个 GPIO 引脚在 `GPIOx->CRL`（脚 0~7）或 `GPIOx->CRH`（脚 8~15）里占 **4 位**，排法是

```text
 bit3   bit2   bit1   bit0
 CNF[1] CNF[0] MODE[1] MODE[0]
```

**当 `MODE[1:0] = 00` 时，这个引脚就是输入类**，此时完全由 `CNF[1:0]` 区分四种输入：

| 4 位值（CNF/MODE） | 模式 | 引脚悬空时是什么电平 | 本实验用它吗 |
| --- | --- | --- | --- |
| `0x0` = 00/00 | **模拟输入**（复位默认） | 不确定，且数字输入缓冲器关闭 | 否（实验 16 ADC 用） |
| `0x4` = 01/00 | **输入浮空** | 完全悬空，容易被干扰 | 否 |
| `0x8` = 10/00 | **输入上拉 / 下拉** | 由 `ODR` 位决定：`ODR=1` 上拉到 3.3V，`ODR=0` 下拉到 0V | **是，KEY0/KEY1/KEY_UP 全用这个** |
| `0xC` = 11/00 | 保留 | — | 否 |

> 记住这个"寄存器版的坑"：**输入模式的上拉还是下拉，不是靠某个专门的"上下拉寄存器"，而是复用 `GPIOx->ODR` 的同一位**。这是 STM32F1 的设计，很多初学者以为 ODR 只能控制输出，其实在 `CNF=10` 时它是上下拉选择位。

工程里 [sys.c](Drivers/SYSTEM/sys/sys.c) 的 `sys_gpio_set()` 把这件事写成了一段很直白的代码：

```c
            else if (mode == 0)     /* 如果是普通输入模式 */
            {
                if (pupd == 0)   /* 不带上下拉,即浮空输入模式 */
                {
                    config = 1 << 2;               /* 设置bit2/3 CNF[1:0] = 01   浮空输入模式 */
                }
                else
                {
                    config = 1 << 3;                            /* 设置bit2/3 CNF[1:0] = 10   上下拉输入模式 */
                    p_gpiox->ODR &= ~(1 << pinpos);             /* 清除原来的设置 */
                    p_gpiox->ODR |= (pupd & 0X01) << pinpos;    /* 设置ODR = 0/1 下拉/上拉 */
                }
            }
```

对照 [sys.h](Drivers/SYSTEM/sys/sys.h) 的宏：`SYS_GPIO_PUPD_NONE=0`、`SYS_GPIO_PUPD_PU=1`、`SYS_GPIO_PUPD_PD=2`。所以：
- 传 `PUPD_PU`（1）→ `1 & 0x01 = 1` → ODR 位置 1 → **上拉**；
- 传 `PUPD_PD`（2）→ `2 & 0x01 = 0` → ODR 位置 0 → **下拉**。
- 输入模式下 `otype`（推挽/开漏）和 `ospeed`（速度）两个参数完全无效（`sys_gpio_set()` 注释里也写明了）。

### 2.2 三个按键的电路，决定了它们该配上拉还是下拉

| 按键 | 引脚 | 硬件接法 | 按下时引脚电平 | 松开时（靠内部电阻） | 需要的模式 |
| --- | --- | --- | --- | --- | --- |
| KEY0 | PC5 | 一端接引脚，另一端接 GND | **0（低）** | 悬空 → 需上拉到 1 | 上拉输入 |
| KEY1 | PA15 | 一端接引脚，另一端接 GND | **0（低）** | 悬空 → 需上拉到 1 | 上拉输入 |
| KEY_UP（代码里宏名 `WK_UP`） | PA0 | 一端接引脚，另一端接 3.3V | **1（高）** | 悬空 → 需下拉到 0 | 下拉输入 |

这就是 `key_init()` 里 KEY0/KEY1 用 `SYS_GPIO_PUPD_PU`、WKUP 用 `SYS_GPIO_PUPD_PD` 的原因。上/下拉电阻把"松手时的悬空态"钉死成一个确定电平，读出来的值才可靠；同时也省掉了外部电阻。

### 2.3 按键抖动与消抖

机械按键的金属簧片接触瞬间会抖动 **约 5~10ms**，电平会来回跳好几次。如果不处理，一次按下可能被程序识别成"按了 5 次"。本工程用**软件延时消抖**：检测到电平变化后 `delay_ms(10)` 再确认一次，见 [key.c](Drivers/BSP/KEY/key.c) 的 `key_scan()`。

### 2.4 `key_scan()` 的状态机（编号步骤）

```text
静态变量 key_up（1 = 已松开）
   ↓
① 若 key_up==1 且 (KEY0==0 或 KEY1==0 或 WK_UP==1)  → 认为"刚刚按下"
      delay_ms(10) 消抖 → key_up = 0 → 依次判断三个按键，得到 keyval
② 否则若 KEY0==1 且 KEY1==1 且 WK_UP==0            → 认为"全部松开"，key_up = 1
③ 返回 keyval（0 表示这次调用没有新按键）
```

- 因为 `key_up` 只在"全松开"时才复位为 1，所以按住不放只会第一次返回键值 —— 这就是 `mode=0`（不支持连按）的效果。
- `mode=1` 时函数开头执行 `if (mode) key_up = 1;`，于是每次调用都能返回键值 → 支持连按。本实验 `main.c` 传的是 `0`。
- 三个判断是**连续三个 `if`（不是 else if）**，后面的会覆盖 `keyval`，所以同时按下多个键时的优先级是 **WK_UP > KEY1 > KEY0**（`key_scan()` 的注释也明确写了这个顺序）。

### 2.5 整体流程

```text
上电 → 配时钟72M → delay_init → led_init（PA8/PD2 推挽输出）
    → key_init（PC5/PA15 上拉输入，PA0 下拉输入）→ 点亮 LED0
    ↓
┌─ while(1) ───────────────────────────────┐
│ key = key_scan(0)                          │
│ key != 0 → switch 分发：                   │
│     WKUP_PRES(3) → LED0 翻转               │
│     KEY1_PRES(2) → LED1 翻转               │
│     KEY0_PRES(1) → LED0 与 LED1 同时翻转   │
│ key == 0 → delay_ms(10)（空转节流）        │
└────────────────────────────────────────────┘
```

## 三、硬件资源与接线

| 资源 | 引脚 | 代码中的宏 / 位置 | 说明 |
| --- | --- | --- | --- |
| KEY0（独立按键） | **PC5** | `KEY0_GPIO_PORT=GPIOC` / `KEY0_GPIO_PIN=SYS_GPIO_PIN5`，[key.h](Drivers/BSP/KEY/key.h) | 上拉输入，4 位值 `0x8` 落在 `GPIOC->CRL[23:20]`；按下读 0 |
| KEY1（独立按键） | **PA15** | `KEY1_GPIO_PORT=GPIOA` / `KEY1_GPIO_PIN=SYS_GPIO_PIN15` | 上拉输入，`0x8` 落在 `GPIOA->CRH[31:28]`，ODR bit15=1；按下读 0 |
| KEY_UP / WK_UP | **PA0** | `WKUP_GPIO_PORT=GPIOA` / `WKUP_GPIO_PIN=SYS_GPIO_PIN0` | 下拉输入，`0x8` 落在 `GPIOA->CRL[3:0]`，ODR bit0=0；按下读 1 |
| LED0（红灯） | PA8 | `LED0_GPIO_PORT=GPIOA` / `LED0_GPIO_PIN=SYS_GPIO_PIN8`，[led.h](Drivers/BSP/LED/led.h) | 推挽输出 `0x1`（`GPIOA->CRH[3:0]`），低电平点亮 |
| LED1（绿灯） | PD2 | `LED1_GPIO_PORT=GPIOD` / `LED1_GPIO_PIN=SYS_GPIO_PIN2` | 推挽输出 `0x1`（`GPIOD->CRL[11:8]`），低电平点亮 |
| GPIOA 时钟 | — | `RCC->APB2ENR` bit2 | `KEY1_GPIO_CLK_ENABLE()` / `WKUP_GPIO_CLK_ENABLE()` / `LED0_GPIO_CLK_ENABLE()` |
| GPIOC 时钟 | — | `RCC->APB2ENR` bit4 | `KEY0_GPIO_CLK_ENABLE()` |
| GPIOD 时钟 | — | `RCC->APB2ENR` bit5 | `LED1_GPIO_CLK_ENABLE()` |

- **不需要外接任何东西，也不用插跳线帽**：三个按键和两颗 LED 都在板子上（丝印 KEY0 / KEY1 / KEY_UP）。
- **PA0 的第二个身份**：它同时是 STM32 待机模式的 WKUP 唤醒脚（见 [sys.c](Drivers/SYSTEM/sys/sys.c) 的 `sys_standby()` 里 `PWR->CSR |= 1 << 8`），做实验 14 低功耗时会复用这个按键。
- **时钟树**：`sys_stm32_clock_init(9)` → 8MHz HSE × 9 = 72MHz，`PCLK2 = 72MHz`、`PCLK1 = 36MHz`。GPIO 在 APB2 上，输入采样时钟越快，`key_scan()` 的响应越及时；对按键这种慢速输入，速度档位本身不起作用（输入模式不看 `MODE` 的速度位）。

## 四、代码结构与调用流程

```text
实验2 按键输入实验/
├── Drivers/
│   ├── BSP/
│   │   ├── KEY/key.c / key.h     # 新增：三个按键的引脚定义、key_init()、key_scan()
│   │   └── LED/led.c / led.h     # 与实验1 相同（含 LED0_TOGGLE/LED1_TOGGLE 宏）
│   ├── CMSIS/                    # 芯片头文件 + 启动文件
│   └── SYSTEM/sys | delay | usart# 通用 GPIO / 时钟 / 延时 / printf 重定向
├── Projects/MDK-ARM/atk_f103.uvprojx   # Keil 工程，Target 名：KEY
├── User/main.c                   # 轮询按键并分发到 LED
└── readme.txt                    # 官方说明
```

- `User/main.c`：初始化 + 扫描键值 + `switch` 分发。
- `Drivers/BSP/KEY/key.h`：把"KEY0"绑定成"读 GPIOC 第 5 位"，把键值定义成 `KEY0_PRES=1 / KEY1_PRES=2 / WKUP_PRES=3`。
- `Drivers/BSP/KEY/key.c`：`key_init()` 配输入模式；`key_scan(mode)` 做消抖和松手检测。
- `Drivers/SYSTEM/sys/sys.c`：`sys_gpio_set()`（本实验走的是它的输入分支）、`sys_gpio_pin_get()`（读 `IDR`）。
- `Drivers/SYSTEM/usart/*`：本实验**没有调用 `usart_init()`**，虽然 `main.c` include 了 `usart.h`，但串口功能未启用。

`main()` 流程：

1. `sys_stm32_clock_init(9)` → 72MHz。
2. `delay_init(72)` → SysTick 延时基准（`key_scan()` 的 10ms 消抖要用它）。
3. `led_init()` → PA8/PD2 推挽输出，两灯先灭。
4. `key_init()` → PC5/PA15 上拉输入、PA0 下拉输入。
5. `LED0(0)` → 开机先点亮红灯，作为"初始态"方便观察翻转。
6. `while(1)` → `key = key_scan(0)`；有键值进 `switch`，无键值 `delay_ms(10)`。

## 五、核心代码解析

### 5.1 完整 `User/main.c`（去掉文件头注释）

```c
#include "./SYSTEM/sys/sys.h"
#include "./SYSTEM/usart/usart.h"
#include "./SYSTEM/delay/delay.h"
#include "./BSP/LED/led.h"
#include "./BSP/KEY/key.h"


int main(void)
{
    uint8_t key; 

    sys_stm32_clock_init(9);    /* 设置时钟, 72Mhz */
    delay_init(72);             /* 延时初始化 */
    led_init();                 /* 初始化LED */
    key_init();                 /* 初始化按键 */
    LED0(0);                    /* 先点亮LED0 */

    while (1)
    {
        key = key_scan(0);      /* 得到键值 */

        if (key)
        {
            switch (key)
            {
                case WKUP_PRES:     /* 控制LED0(RED)翻转 */
                    LED0_TOGGLE();  /* LED0状态取反 */
                    break;

                case KEY1_PRES:     /* 控制LED1(GREEN)翻转 */
                    LED1_TOGGLE();  /* LED1状态取反 */
                    break;

                case KEY0_PRES:     /* 同时控制LED0, LED1翻转 */
                    LED0_TOGGLE();  /* LED0状态取反 */
                    LED1_TOGGLE();  /* LED1状态取反 */
                    break;
            } 
        }
        else
        {
            delay_ms(10);
        }
    }
}
```

关键点：
- `key_scan(0)` 的参数 0 = **不支持连按**：按住不放只算一次，必须松手再按才有下一次键值。
- 键值分发用的是 `key.h` 里的三个宏，而不是自己写的数字，可读性和移植性都好。
- `else delay_ms(10)`：没按键时也让出 10ms，避免主循环满速空转（对本实验没有功能影响，但是好习惯，后续实验里可以给看门狗喂狗留出节奏）。
- 代码里 **没有 `default:` 分支**，所以若 `key_scan()` 返回其它值就什么都不做——目前它只会返回 0/1/2/3。

### 5.2 `key_init()`：输入模式的三步

```c
void key_init(void)
{
    KEY0_GPIO_CLK_ENABLE(); /* KEY0时钟使能 */
    KEY1_GPIO_CLK_ENABLE(); /* KEY1时钟使能 */ 
    WKUP_GPIO_CLK_ENABLE(); /* WKUP时钟使能 */

    sys_gpio_set(KEY0_GPIO_PORT, KEY0_GPIO_PIN,
                 SYS_GPIO_MODE_IN, SYS_GPIO_OTYPE_PP, SYS_GPIO_SPEED_MID, SYS_GPIO_PUPD_PU);    /* KEY0引脚模式设置,上拉输入 */

    sys_gpio_set(KEY1_GPIO_PORT, KEY1_GPIO_PIN,
                 SYS_GPIO_MODE_IN, SYS_GPIO_OTYPE_PP, SYS_GPIO_SPEED_MID, SYS_GPIO_PUPD_PU);    /* KEY1引脚模式设置,上拉输入 */

    sys_gpio_set(WKUP_GPIO_PORT, WKUP_GPIO_PIN,
                 SYS_GPIO_MODE_IN, SYS_GPIO_OTYPE_PP, SYS_GPIO_SPEED_MID, SYS_GPIO_PUPD_PD);    /* WKUP引脚模式设置,下拉输入 */
}
```

按步骤拆：
1. **开时钟**：PC → `RCC->APB2ENR |= 1<<4`（bit4 = IOPCEN）；PA → `|= 1<<2`（bit2 = IOPAEN）。KEY1 和 WKUP 都在 PA，所以两次宏展开写的是同一个 bit（`|=` 幂等，无副作用）。
2. **配模式**：`SYS_GPIO_MODE_IN`(0) → `sys_gpio_set()` 走输入分支 → 4 位值 `0x8`（CNF=10 上下拉输入，MODE=00 输入）。
3. **顺手写好 ODR 位选上/下拉**：PU→ODR 位 1，PD→ODR 位 0。这一步在 `sys_gpio_set()` 内部完成，调用者不用操心。
4. **不需要设初始输出电平**：输入模式下 ODR 只用来选上下拉，不驱动引脚。

### 5.3 读引脚：`sys_gpio_pin_get()` 与三个读宏

```c
uint8_t sys_gpio_pin_get(GPIO_TypeDef *p_gpiox, uint16_t pinx)
{
    if (p_gpiox->IDR & pinx)
    {
        return 1;   /* pinx的状态为1 */
    }
    else
    {
        return 0;   /* pinx的状态为0 */
    }
}
```

`IDR`（Input Data Register）是"引脚当前真实电平"的快照，读取即得，不需要任何配置。[key.h](Drivers/BSP/KEY/key.h) 把它包成语义化宏：

```c
#define KEY0        sys_gpio_pin_get(KEY0_GPIO_PORT, KEY0_GPIO_PIN)     /* 读取KEY0引脚 */
#define KEY1        sys_gpio_pin_get(KEY1_GPIO_PORT, KEY1_GPIO_PIN)     /* 读取KEY1引脚 */
#define WK_UP       sys_gpio_pin_get(WKUP_GPIO_PORT, WKUP_GPIO_PIN)     /* 读取WKUP引脚 */
```

**注意这三个宏读到的都是"电平"而不是"是否按下"**：KEY0/KEY1 按下 = 0，WK_UP 按下 = 1。`key_scan()` 里的判断 `KEY0 == 0 || KEY1 == 0 || WK_UP == 1` 正是这个差别的体现，也是初学者最容易写反的地方。

### 5.4 `key_scan()` 全文

```c
uint8_t key_scan(uint8_t mode)
{
    static uint8_t key_up = 1;  /* 按键按松开标志 */
    uint8_t keyval = 0;

    if (mode) key_up = 1;   /* 支持连按 */

    if (key_up && (KEY0 == 0 || KEY1 == 0 || WK_UP == 1))   /* 按键松开标志为1, 且有任意一个按键按下了 */
    {
        delay_ms(10);       /* 去抖动 */
        key_up = 0;

        if (KEY0 == 0)  keyval = KEY0_PRES;

        if (KEY1 == 0)  keyval = KEY1_PRES;

        if (WK_UP == 1) keyval = WKUP_PRES;
    }
    else if (KEY0 == 1 && KEY1 == 1 && WK_UP == 0)  /* 没有任何按键按下, 标记按键松开 */
    {
        key_up = 1;
    }

    return keyval;  /* 返回键值 */
}
```

- `static uint8_t key_up = 1;`：**静态局部变量**让"是否已松开"这个记忆跨调用保留，这是"一次按下只返回一次键值"的核心。
- 消抖后不再判 `mode`，直接把 `key_up` 清 0；松手条件 `KEY0==1 && KEY1==1 && WK_UP==0` 是"三个键都没按"。
- 三个 `if` 顺序是 KEY0 → KEY1 → WK_UP，`keyval` 会被后满足的条件覆盖，所以 **同时按下时 WK_UP 优先，其次 KEY1，最后 KEY0**。
- 消抖用的是 `delay_ms(10)`，会阻塞主循环 10ms——本实验无所谓，但在中断或实时性要求高的场合要避免（实验 3 的中断里也是同样写法，属于教学简化）。

## 六、编译、下载与运行

1. Keil MDK 打开 **[Projects/MDK-ARM/atk_f103.uvprojx](Projects/MDK-ARM/atk_f103.uvprojx)**（Target 名 `KEY`）。
2. 工程设置（与实验 1 一致）：器件 `STM32F103RC`，Pack `Keil.STM32F1xx_DFP.2.3.0`，C 宏 `STM32F103xE`，包含路径 `..\..\Drivers\CMSIS\Device\ST\STM32F1xx\Include;..\..\Drivers\CMSIS\Include;..\..\Drivers;..\..\User`，启动文件 `startup_stm32f103xe.s`。
3. 编译输出在 `..\..\Output\`（文件名 `atk_f103`，如 `Output/atk_f103.hex`）。
4. 用 ST-Link / J-Link 接 SWD 排针下载；本实验不需要串口。
5. 下载后按板上 RESET 键，手指放在 KEY0 / KEY1 / KEY_UP 三个按键上准备操作。

## 七、实验现象

1. 上电/复位后：**LED0（红灯）亮**，LED1（绿灯）灭（这是 `LED0(0)` 设定的初始态）。
2. 按一下 **KEY_UP（PA0）** 并松开：红灯（LED0）状态翻转一次 —— 亮变灭或灭变亮；绿灯不变。
3. 按一下 **KEY1（PA15）** 并松开：绿灯（LED1）翻转一次；红灯不变。
4. 按一下 **KEY0（PC5）** 并松开：红灯和绿灯**同时**翻转（同亮或同灭）。
5. **按住不放不重复触发**：因为 `key_scan(0)` 不支持连按，必须完全松手再按才会再次翻转。想验证连按效果，把 `main.c` 的 `key_scan(0)` 改成 `key_scan(1)` 再下载，按住 KEY1 时绿灯会以扫描节奏持续闪烁。
6. 同时按住 KEY0 和 KEY_UP：由于 `key_scan()` 的优先级 WK_UP > KEY1 > KEY0，实际执行的是 KEY_UP 的动作（只翻转红灯）。

## 八、注意事项

1. **官方 `readme.txt` 与代码不一致（以代码为准）**：
   - `readme.txt` 的"实验现象"写的是 "KEY_UP控制LED1翻转，**KEY2**控制LED0翻转，KEY0控制LED0/LED1同时翻转"。
   - 代码（[User/main.c](User/main.c)）实际是：**KEY_UP(WK_UP) → LED0 翻转，KEY1 → LED1 翻转，KEY0 → LED0 和 LED1 同时翻转**；而且工程里根本没有 `KEY2` 这个键（只有 `KEY0`/`KEY1`/`WK_UP`）。也就是说 `readme.txt` 把 KEY_UP 与 KEY1 控制的灯写反了，`KEY2` 是笔误，应为 `KEY1`。
   - `readme.txt` 的"硬件资源及引脚分配"部分（KEY0-PC5、KEY1-PA15、KEY_UP-PA0 且注明程序宏名为 `WK_UP`）与代码完全一致，可以放心照它接线。
2. **上拉/下拉不能选反**：KEY0/KEY1 若误配成下拉输入，没按键时读到 0，`key_scan()` 会认为一直按着 → 灯一直翻转/不复位。WK_UP 若误配上拉，同理。
3. **忘开 GPIOC 时钟**：`KEY0` 永远读到 0（复位后外设时钟是关的），现象变成"一上电就一直以为 KEY0 按下"。
4. **PA15 的复用身份**：PA15 默认是 JTAG 的 JTDI 功能脚，板上作为 KEY1 使用时依赖复位后释放的数字输入；如果你在其他实验里调用了 `sys_gpio_remap_set(24, 3, 2)`（关 JTAG 只留 SWD），KEY1 会更"干净"；反之如果调试器只用 SWD 连接，一般也没问题。若发现 KEY1 行为异常，先检查是否有别处改了 `AFIO->MAPR` 的 `SWJ_CFG`。
5. **灯状态"看起来不对"**：本板 LED 低电平点亮，`LED0_TOGGLE()` 翻的是 ODR 位，翻转一次就是"亮↔灭"，别把 `LED0(0)` 当成"灭"。
6. **消抖时间**：`delay_ms(10)` 是经验值。有些键老化后抖动更久，可以加到 20ms；但不要加太长，否则快速连按会丢键。
7. **源码是 GBK 编码**：Keil 编辑器编码要设为 GBK（`Edit → Configuration → Editor → Encoding`），否则中文注释乱码；不要顺手存成 UTF-8。
8. 本实验未启用串口，`main.c` 里 `#include "./SYSTEM/usart/usart.h"` 只是模板保留，不要以为可以直接 `printf`（需要先调 `usart_init(72, 115200)`，见实验 4）。

## 九、总结与延伸

**本实验掌握 5 个要点：**

1. 输入模式下 CRL/CRH 的 4 位是 **MODE=00 + CNF=01（浮空）/ 10（上下拉）**，本实验三个键都写成 `0x8`。
2. **`GPIOx->ODR` 在 `CNF=10` 时兼任上/下拉选择位**（1 上拉，0 下拉），`sys_gpio_set()` 用 `pupd & 0x01` 帮你写好了。
3. 读引脚 = 读 `GPIOx->IDR`；KEY0/KEY1 按下为 0，WK_UP 按下为 1，逻辑不要写反。
4. 软件消抖 = 检测到变化 → `delay_ms(10)` → 再确认；用 `static` 变量记录"是否已松开"来实现"一次按下只算一次"。
5. 键值优先级由代码顺序决定（后面的 `if` 覆盖前面的），本工程为 WK_UP > KEY1 > KEY0。

**延伸小练习：**

1. 把 `key_scan(0)` 改成 `key_scan(1)` 观察连按，再把 `main.c` 的 `else` 分支 `delay_ms(10)` 删掉，体会扫描节奏对连按速度的影响。
2. 在 `key.h` 里新增一个键值宏（例如 `KEY_ALL_PRES 4`），在 `key_scan()` 末尾补一段"三个键同时按下则返回 4"的判断，并在 `main.c` 的 `switch` 里给它加一个 `case`（比如让两颗灯都灭）——练"改驱动 + 改应用"的联动。
3. 把"翻转"改成"按住才亮、松开就灭"：不再用 `key_scan()`，而是在主循环里直接 `LED0(KEY0)`——KEY0 按下时读到 0，正好让低电平点亮的红灯亮起来，一眼看懂"输入电平直通输出电平"；顺便试试 `LED1(WK_UP)`，体会 KEY_UP 与其它两个键极性相反（按下为 1）。

**衔接下一步**：轮询的缺点是程序必须不停地问"按了吗"。[实验3 外部中断实验](../实验3%20外部中断实验) 用同样的三个引脚，把它们接到 EXTI 中断线上：按键一按，硬件自动打断主程序去执行 `EXTI0_IRQHandler` 等中断服务函数，主循环可以专心干别的事。
