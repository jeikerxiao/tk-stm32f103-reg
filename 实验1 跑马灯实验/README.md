# 实验1 跑马灯实验

> 一句话概括：让板子上的两颗 LED（LED0 红灯 / LED1 绿灯）交替点亮，学会"怎么用寄存器把一个 IO 口配成输出、并让它输出高/低电平"——这是所有 STM32 外设编程的第一块基石。

## 一、实验简介

- **实验目的**（官方 `readme.txt`）：学习 GPIO 作为输出的使用。
  通俗地说：GPIO 就是芯片引出的一堆"可编程小开关"，每个开关既能当输出（芯片决定引脚是 3.3V 还是 0V）也能当输入（芯片去读引脚上的电压）。本实验只干一件事——把 PA8 和 PD2 配成输出，然后轮流把这两个脚拉低，于是两颗灯就交替亮了。
- **在学习路线中的位置**：
  - 前置：无（这是整个寄存器版教程的第 1 个实验，但你需要先装好 Keil MDK 和驱动）。
  - 本实验打通的三件事：**开外设时钟 → 配置 GPIO 模式寄存器 → 操作输出数据寄存器**。后面实验 2（按键输入）、实验 3（外部中断）、实验 4（串口）全都建立在这三步之上，只是把"输出"换成"输入""中断线""复用功能"。
  - 后续：实验 2 按键输入（同一套 CRL/CRH 知识，只是模式换成输入）、实验 4 串口（模式换成复用输出）。
- **硬件平台**：正点原子 MiniSTM32 V4 开发板，主芯片 **STM32F103RC**（Cortex-M3 内核，主频 72MHz，256KB Flash / 48KB SRAM）。

## 二、实验原理

### 2.1 先搞懂"寄存器"是什么

寄存器就是**内存里贴着外设的一堆"控制格子"**。STM32 把 4GB 地址空间中的一段分给片上外设（从 `0x40000000` 开始），每个格子的地址固定、每一位（bit）的含义固定：

- 往某一位写 1，外设就打开对应功能；写 0 就关闭。
- 读某一位，就能知道外设当前的状态。

所以"配置外设"= **按位往这些格子里填数**。本实验涉及的寄存器都在这三个外设基地址上（来自 [stm32f103xe.h](Drivers/CMSIS/Device/ST/STM32F1xx/Include/stm32f103xe.h)）：

| 外设 | 基地址 | 本工程用到的寄存器 |
| --- | --- | --- |
| RCC | `0x40021000` | `RCC->APB2ENR`（给 GPIOA/GPIOD 开时钟） |
| GPIOA | `0x40010800` | `GPIOA->CRH`、`GPIOA->BSRR`、`GPIOA->ODR` |
| GPIOD | `0x40011400` | `GPIOD->CRL`、`GPIOD->BSRR`、`GPIOD->ODR` |

在 C 语言里能这样写 `GPIOA->CRH`，是因为头文件把一个结构体指针强制定位到了这个地址上：

```c
#define GPIOA               ((GPIO_TypeDef *)GPIOA_BASE)
```

### 2.2 为什么第一步永远是"开时钟"

STM32 为了省电，每个外设都挂在一个"时钟开关"下面，**开关没打开时这个外设的寄存器读写的值不动**。GPIOA、GPIOD 挂在 APB2 总线上，开关就是 `RCC->APB2ENR` 的对应 bit（本工程用的宏就是直接置位）：

| 宏（[led.h](Drivers/BSP/LED/led.h)） | 展开 | 含义 |
| --- | --- | --- |
| `LED0_GPIO_CLK_ENABLE()` | `RCC->APB2ENR |= 1 << 2;` | bit2 = IOPAEN，开 GPIOA 时钟 |
| `LED1_GPIO_CLK_ENABLE()` | `RCC->APB2ENR |= 1 << 5;` | bit5 = IOPDEN，开 GPIOD 时钟 |

> 常见坑：忘开时钟，代码编译下载都正常，但 LED 死活不亮 / 按键死活读不到。

### 2.3 GPIO 的"模式寄存器"：CRL 与 CRH（本实验重点）

每个 GPIO 端口（GPIOA~GPIOD…）有 16 个引脚，**每个引脚需要 4 位配置**（CNF[1:0] + MODE[1:0]）。4 位 × 16 脚 = 64 位，超过了一个 32 位寄存器能装的范围，于是 ST 拆成两个：

- `GPIOx->CRL`：管 **引脚 0~7**，第 x 号脚的配置放在 bit[4x+3 : 4x]（每个脚占 4 位）。
- `GPIOx->CRH`：管 **引脚 8~15**，第 x 号脚的配置放在 bit[4(x-8)+3 : 4(x-8)]。

一个引脚的 4 位长这样（bit3~bit0）：

```text
 bit3  bit2 | bit1  bit0
  CNF[1] CNF[0] | MODE[1] MODE[0]
```

**MODE[1:0] 决定"速度和是不是输入"，CNF[1:0] 决定"哪一类模式"**，两者组合出 16 种模式，实际有效的组合如下：

| MODE[1:0] | CNF[1:0] | 输入（MODE=00 时看 CNF） | 输出（MODE≠00 时看 CNF） |
| --- | --- | --- | --- |
| 00 | 00 | **模拟输入**（复位后的默认值） | —（保留） |
| 00 | 01 | **输入浮空** | — |
| 00 | 10 | **输入上拉/下拉**（由 ODR 位选：1 上拉，0 下拉） | — |
| 00 | 11 | 保留 | — |
| 01（10MHz） | 00 / 01 / 10 / 11 | — | 推挽 / 开漏 / 复用推挽 / 复用开漏 |
| 10（2MHz） | 00 / 01 / 10 / 11 | — | 推挽 / 开漏 / 复用推挽 / 复用开漏 |
| 11（50MHz） | 00 / 01 / 10 / 11 | — | 推挽 / 开漏 / 复用推挽 / 复用开漏 |

本工程的 [sys.c](Drivers/SYSTEM/sys/sys.c) 用一组宏把这 4 位"拼"出来（这就是寄存器版和标准库版本的差别：拼位的工作自己写）：

```c
if ((mode == 0X01) || (mode == 0X02))   /* 如果是普通输出模式/复用功能模式 */
{
    config = ospeed & 0X03;             /* 设置bit0/1 MODE[1:0] = 2/1/3 速度参数 */
    config |= (otype & 0X01) << 2;      /* 设置bit2   CNF[0]    = 0/1   推挽/开漏输出 */
    config |= (mode - 1) << 3;          /* 设置bit3   CNF[1]    = 0/1   普通/复用输出 */
}
```

把常用组合"翻译"成 4 位十六进制值，这张表后面几个实验都会反复用到：

| 用途 | 传给 `sys_gpio_set()` 的参数 | 写入的 4 位值 | 对应模式 |
| --- | --- | --- | --- |
| **驱动 LED（本实验）** | `MODE_OUT, OTYPE_PP, SPEED_MID, PUPD_PU` | `0x1`（MODE=01, CNF=00） | 推挽输出，10MHz |
| 按键上拉输入 | `MODE_IN, OTYPE_PP, SPEED_MID, PUPD_PU` | `0x8`（MODE=00, CNF=10, ODR=1） | 上拉输入 |
| 按键下拉输入 | `MODE_IN, ..., PUPD_PD` | `0x8`（MODE=00, CNF=10, ODR=0） | 下拉输入 |
| 串口 TX | `MODE_AF, OTYPE_PP, SPEED_HIGH, PUPD_PU` | `0xB`（MODE=11, CNF=10） | 复用推挽，50MHz |
| ADC 采样脚 | `MODE_AIN, ...` | `0x0` | 模拟输入 |

**本实验的具体数值**：
- LED0 是 **PA8**（第 8 号脚）→ 落在 `GPIOA->CRH` 的低 4 位，即 `CRH[3:0] = 0x1`。
- LED1 是 **PD2**（第 2 号脚）→ 落在 `GPIOD->CRL`，位置是 `CRL[11:8] = 0x1`。

> 驱动 LED 用"中速 10MHz"完全够（LED 开关频率极低），速度档位只是限制输出级的驱动能力/边沿陡峭程度，越高开关损耗和辐射越大。

### 2.4 让 LED 亮起来：ODR / BSRR，以及"低电平点亮"

MiniSTM32 板上的 LED 是**低电平点亮**（VCC → 限流电阻 → LED → 引脚，引脚拉低才有电流）。所以：

- `LED0(0)` → 引脚输出 0V → **灯亮**
- `LED0(1)` → 引脚输出 3.3V → **灯灭**

写电平用 `GPIOx->BSRR`（BSRR 是"位置位/复位寄存器"，高 16 位写 1 复位对应脚，低 16 位写 1 置位对应脚）：

```c
void sys_gpio_pin_set(GPIO_TypeDef *p_gpiox, uint16_t pinx, uint8_t status)
{
    if (status & 0X01)
    {
        p_gpiox->BSRR |= pinx;  /* 设置GPIOx的pinx为1 */
    }
    else
    {
        p_gpiox->BSRR |= (uint32_t)pinx << 16;  /* 设置GPIOx的pinx为0 */
    }
}
```

为什么用 BSRR 而不用 `ODR |= / &=`？因为 BSRR 是**一条写操作就能原子地改变单个引脚**，不需要"读-改-写"，中断打断也不会误改别的引脚。

### 2.5 跑马灯的流程

```text
初始化时钟(72M) → 初始化延时(SysTick) → led_init()配置 PA8/PD2 为推挽输出并熄灭
        ↓
   ┌── 主循环 while(1) ──────────────────────┐
   │ PA8=0(LED0 亮) , PD2=1(LED1 灭) → 延时500ms │
   │ PA8=1(LED0 灭) , PD2=0(LED1 亮) → 延时500ms │
   └──────── 回到开头 ────────────────────────┘
```

## 三、硬件资源与接线

| 资源 | 引脚 | 代码中的宏 / 位置 | 说明 |
| --- | --- | --- | --- |
| LED0（红灯 RED） | **PA8** | `LED0_GPIO_PORT=GPIOA` / `LED0_GPIO_PIN=SYS_GPIO_PIN8`，[led.h](Drivers/BSP/LED/led.h) | 低电平点亮；占 `GPIOA->CRH[3:0]` |
| LED1（绿灯 GREEN） | **PD2** | `LED1_GPIO_PORT=GPIOD` / `LED1_GPIO_PIN=SYS_GPIO_PIN2`，[led.h](Drivers/BSP/LED/led.h) | 低电平点亮；占 `GPIOD->CRL[11:8]` |
| GPIOA 时钟 | — | `RCC->APB2ENR` bit2 | 由 `LED0_GPIO_CLK_ENABLE()` 置位 |
| GPIOD 时钟 | — | `RCC->APB2ENR` bit5 | 由 `LED1_GPIO_CLK_ENABLE()` 置位 |
| SysTick（提供 `delay_ms`） | 片内 | [delay.c](Drivers/SYSTEM/delay/delay.c) | 24 位递减计数器，用 HCLK=72MHz |

- **无需任何外接模块、排针或跳线帽**，插上 USB 线供电即可。
- **时钟树在本实验意味着什么**：`main()` 第一行 `sys_stm32_clock_init(9)` 把 8MHz 外部晶振（HSE）经 PLL ×9 得到 72MHz 系统时钟，AHB 不分频，`PCLK2 = HCLK = 72MHz`、`PCLK1 = HCLK/2 = 36MHz`（见 [sys.c](Drivers/SYSTEM/sys/sys.c) 中 `sys_clock_set()` 的注释）。GPIO 挂在 APB2 上，所以它拿到的是 72MHz 的总线时钟；`delay_init(72)` 里的参数 `72` 就是在告诉延时函数"现在是 72MHz"，两者必须一致，否则 `delay_ms(500)` 就不是 500ms。

## 四、代码结构与调用流程

```text
实验1 跑马灯实验/
├── Drivers/
│   ├── BSP/LED/led.c / led.h          # 本实验唯一新增的"板级驱动"：LED 引脚定义 + led_init()
│   ├── CMSIS/                         # ST 芯片头文件 stm32f103xe.h + 启动文件 startup_stm32f103xe.s
│   └── SYSTEM/
│       ├── sys/sys.c / sys.h          # 通用 GPIO 设置(sys_gpio_set)、时钟配置、NVIC 封装
│       ├── delay/delay.c / delay.h    # delay_us / delay_ms，基于 SysTick
│       └── usart/usart.c / usart.h    # printf 重定向（本实验没主动调用 usart_init）
├── Projects/MDK-ARM/atk_f103.uvprojx  # Keil 工程
├── User/main.c                        # 主程序
└── readme.txt                         # 官方说明
```

各文件职责一句话：
- `User/main.c`：初始化 + 交替点亮 LED 的死循环。
- `Drivers/BSP/LED/led.h`：用宏把"LED0"绑定到"GPIOA 的第 8 位"，改板子只改这几个宏。
- `Drivers/BSP/LED/led.c`：`led_init()`，开时钟 → 配模式 → 设初始电平。
- `Drivers/SYSTEM/sys/sys.c`：把 6 个语义化参数翻译成 CRL/CRH 的 4 位配置，并提供 `sys_gpio_pin_set()` 写电平。
- `Drivers/SYSTEM/delay/delay.c`：提供 500ms 延时。

`main()` 调用流程（编号步骤）：

1. `sys_stm32_clock_init(9)` —— 配 PLL，主频 72MHz，并设置中断向量表偏移。
2. `delay_init(72)` —— 按 72MHz 初始化 SysTick 延时基准。
3. `led_init()` —— 开 GPIOA/GPIOD 时钟 → PA8/PD2 设推挽输出 10MHz → `LED0(1)`、`LED1(1)` 先熄灭两颗灯。
4. `while(1)` —— `LED0(0)+LED1(1)` → `delay_ms(500)` → `LED0(1)+LED1(0)` → `delay_ms(500)` → 无限循环。

## 五、核心代码解析

### 5.1 完整 `User/main.c`（去掉文件头注释）

```c
#include "./SYSTEM/sys/sys.h"
#include "./SYSTEM/usart/usart.h"
#include "./SYSTEM/delay/delay.h"
#include "./BSP/LED/led.h"


int main(void)
{
    sys_stm32_clock_init(9);    /* 设置时钟, 72Mhz */
    delay_init(72);             /* 延时初始化 */
    led_init();                 /* 初始化LED */

    while (1)
    {
        LED0(0);                /* LED0 亮 */
        LED1(1);                /* LED1 灭 */
        delay_ms(500);
        LED0(1);                /* LED0 灭 */
        LED1(0);                /* LED1 亮 */
        delay_ms(500);
    }
}
```

逐行说明：
- `sys_stm32_clock_init(9)` 里的 **9 是 PLL 倍频系数**（`plln`，合法范围 2~16），8MHz × 9 = 72MHz。它内部还会先把 `RCC->APB2ENR`、`RCC->APB1ENR` 清 0（关掉所有外设时钟），**所以它必须放在最前面**，否则后面配好的 GPIO 会被它清空。
- `LED0(0)` / `LED1(1)` 是**宏带参数**的写法，等价于"给这个引脚输出 0/1"；参数就是目标电平，不是"亮/灭"——因为本板低电平点亮，0 才是亮。
- `while (1)` 不能少：如果 main 直接返回，程序会走退出流程，Keil 里表现为复位/卡死，LED 就停住了。

### 5.2 `led_init()`：输出的三步配置

```c
void led_init(void)
{
    LED0_GPIO_CLK_ENABLE(); /* LED0时钟使能 */
    LED1_GPIO_CLK_ENABLE(); /* LED1时钟使能 */

    sys_gpio_set(LED0_GPIO_PORT, LED0_GPIO_PIN,
                 SYS_GPIO_MODE_OUT, SYS_GPIO_OTYPE_PP, SYS_GPIO_SPEED_MID, SYS_GPIO_PUPD_PU);   /* LED0引脚模式设置 */

    sys_gpio_set(LED1_GPIO_PORT, LED1_GPIO_PIN,
                 SYS_GPIO_MODE_OUT, SYS_GPIO_OTYPE_PP, SYS_GPIO_SPEED_MID, SYS_GPIO_PUPD_PU);   /* LED1引脚模式设置 */

    LED0(1);    /* 关闭 LED0 */
    LED1(1);    /* 关闭 LED1 */
}
```

拆解成初学者能背下来的顺序：

1. **开时钟**：`RCC->APB2ENR` bit2（GPIOA）、bit5（GPIOD）。
2. **选模式 + 速度 + 输出类型**：`sys_gpio_set()` 收到 `MODE_OUT / OTYPE_PP / SPEED_MID`，拼出 4 位值 `0x1` 写进 `GPIOA->CRH[3:0]` 和 `GPIOD->CRL[11:8]`。这里传了 `SYS_GPIO_PUPD_PU`，但**输出模式下上下拉参数无效**（`sys_gpio_set()` 的注释里明确写了"在输出模式下, pupd 参数无效"），所以只是为了让 6 个参数凑齐才写的。
3. **给定初始电平**：`LED0(1)` / `LED1(1)` 写 BSRR 高 16 位，先把两脚拉高保证开机瞬间两颗灯都不亮，避免上电"闪一下"。

### 5.3 `sys_gpio_set()` 里 CRL/CRH 的落位代码

```c
            /* 根据IO口位置 设置CRL / CRH寄存器 */
            if (pinpos <= 7)
            {
                p_gpiox->CRL &= ~(0X0F << (pinpos * 4));        /* 清除原来的设置 */
                p_gpiox->CRL |= config << (pinpos * 4);         /* 设置CNFx[1:0] 和 MODEx[1:0], x = pinpos = 0~7 */
            }
            else
            {
                p_gpiox->CRH &= ~(0X0F << ((pinpos - 8) * 4));  /* 清除原来的设置 */
                p_gpiox->CRH |= config << ((pinpos - 8) * 4);   /* 设置CNFx[1:0] 和 MODEx[1:0], x = pinpos = 8~15 */

            }
```

- **先"与"清 0 再"或"写值**：因为 CRL/CRH 里每个引脚只占 4 位，直接 `|=` 会把旧值和新值混在一起；先把这 4 位清 0（`&= ~(0x0F << shift)`），再 `|=` 写新值，才能改一个脚而不动另外 15 个脚。
- `pinpos` 是通过 `for` 循环逐位检查 `pinx` 得到的（`pinx` 是"引脚掩码"，如 `SYS_GPIO_PIN8 = 1<<8`），所以一个函数调用可以同时配置多个引脚。

### 5.4 `LED0_TOGGLE()`：读改写 ODR 的技巧

```c
#define LED0_TOGGLE()   do{ LED0_GPIO_PORT->ODR ^= LED0_GPIO_PIN; }while(0)      /* LED0 = !LED0 */
```

`ODR` 保存输出锁存器的值，`^= (1<<8)` 就是把 PA8 取反。本实验用不到它（main 里是直接给定电平），但实验 2/3/4 的按键翻转 LED 全靠这个宏。`do{...}while(0)` 是宏的常见写法，目的是让 `if (x) LED0_TOGGLE();` 这样的单行调用不出语法问题。

## 六、编译、下载与运行

1. 用 Keil MDK 打开 **[Projects/MDK-ARM/atk_f103.uvprojx](Projects/MDK-ARM/atk_f103.uvprojx)**（Target 名为 `LED`）。
2. 工程关键设置（已配好，若新建工程需照抄）：
   - 器件：`STM32F103RC`，Vendor `STMicroelectronics`，依赖 Pack `Keil.STM32F1xx_DFP.2.3.0`。
   - CPU：`Cortex-M3`，RAM `0x20000000` 大小 `0xC000`（48KB），ROM `0x08000000` 大小 `0x40000`（256KB）。
   - C 编译宏（Define）：`STM32F103xE`。
   - 包含路径（Include Paths）：`..\..\Drivers\CMSIS\Device\ST\STM32F1xx\Include`、`..\..\Drivers\CMSIS\Include`、`..\..\Drivers`、`..\..\User`。
     这就是代码里能写 `#include "./SYSTEM/sys/sys.h"` 的原因——`Drivers` 目录本身在搜索路径里。
   - 启动文件：`Drivers/CMSIS/Device/ST/STM32F1xx/Source/Templates/arm/startup_stm32f103xe.s`（里面列了中断向量表，`Reset_Handler` 会调用 `SystemInit` 后进入 `main`）。
3. 编译：`Rebuild`，输出目录由工程设为 `..\..\Output\`，产物文件名 `atk_f103`（例如 `Output/atk_f103.hex`，仓库里已带着这个 hex）。
4. 下载：用 ST-Link 或 J-Link 接板上 SWD 排针（SWDIO/SWCLK/GND/3.3V），点 `Download`；也可以直接用 `File → Download` 快捷键 F8。本实验不需要串口。
5. 上电/复位后即可观察现象。

## 七、实验现象

1. 板子 USB 线插上供电（或下载完成后复位）后，**LED0（红灯，PA8）和 LED1（绿灯，PD2）交替点亮**。
2. 时间上可核对：红灯亮约 0.5 秒 → 绿灯亮约 0.5 秒 → 红灯再亮，如此循环，看起来像灯光在两颗灯之间"跑"，即跑马灯。
3. 用秒表大致数一下 10 秒内切换次数，应该在 20 次左右（每次切换 500ms），可以用来粗略验证 `delay_ms(500)` 是否准确（也间接验证主频真的是 72MHz）。
4. 只有这两颗灯参与，板子上的其它 LED（如电源灯）不受影响。

## 八、注意事项

排查清单（按顺序检查，基本能覆盖 90% 的"灯不亮"）：

1. **时钟开了没**：`led_init()` 里两个 `xxx_GPIO_CLK_ENABLE()` 必须在 `sys_gpio_set()` 之前。
2. **`sys_stm32_clock_init()` 必须第一个调用**：它会清空 `RCC->APB2ENR/APB1ENR`，写在它之后的 GPIO 配置会被抹掉。
3. **LED 是低电平点亮**：`LED0(1)` 是灭，`LED0(0)` 才是亮。想反过来只把 main 里的 0/1 对调即可。
4. **`delay_init(72)` 的参数要和实际主频一致**；如果 PLL 配置失败（例如外部 8MHz 晶振没起振），`delay_ms(500)` 的时间会明显不对，灯闪得飞快或极慢。
5. **MDK 5.29 及以后的仿真器 bug**：[sys.c](Drivers/SYSTEM/sys/sys.c) 的 `sys_clock_set()` 里特意加了至少 `0X8000` 个 `nop` 延时，注释说明"使能 HSEON 以后如果不加延时，下载完代码延时函数会不正常，需要按复位按键才正常"。如果你自己改这个函数，**别把这段等待删掉**；遇到下载后延时异常，先按一下复位键。
6. **PA8 / PD2 有没有被别的东西占用**：在这份仓库里，只有 LED 驱动使用这两个脚（全工程的 BSP 中 `GPIOD` 只出现在 `led.h`），所以做本实验不用担心冲突。但如果你自己飞线、外接模块，记住这两只脚已经接着 LED 的限流电阻，外接信号会分走电流、让灯亮度变化甚至互相顶牛——先查原理图确认那个脚是否空闲，再接东西。
7. **源码编码是 GBK**：如果 Keil 里中文注释显示为乱码，把编辑器编码设置为 GBK/Chinese GB2312（`Edit → Configuration → Editor → Encoding`），不要存成 UTF-8，否则会破坏源文件。
8. **官方 `readme.txt` 与代码的一致性**：本实验 `readme.txt` 里的引脚分配（LED0 = PA8、LED1 = PD2）与"LED0/LED1 交替闪烁"的现象，和 [led.h](Drivers/BSP/LED/led.h)、[User/main.c](User/main.c) 完全一致，`注意事项` 一栏为"无"。没有需要修正的差异。

## 九、总结与延伸

**本实验掌握 4 个要点：**

1. STM32 = 往固定地址的寄存器按位填数；`#define GPIOA ((GPIO_TypeDef *)GPIOA_BASE)` 就是 C 语言操作寄存器的桥。
2. 配置任何外设的第一件事是开 `RCC->APB2ENR` / `APB1ENR` 里对应的时钟位。
3. GPIO 模式在 `CRL`（脚 0~7）/`CRH`（脚 8~15）里，**每个脚 4 位 = CNF[1:0] + MODE[1:0]**；本实验写进去的值是 `0x1`（推挽输出 10MHz）。
4. 输出用电平写 `BSRR`（原子置/复位），读状态看 `IDR`，`ODR` 保存输出锁存值、`^=` 可取反。

**延伸小练习（都在本实验代码上改）：**

1. 把两颗灯的闪烁节奏改成"红 1 秒 / 绿 0.2 秒"——只需要改 `delay_ms()` 的两个参数，顺便体会参数上限（`delay_ms` 形参是 `uint16_t`，最大 65535ms）。
2. 用 `LED0_TOGGLE()` / `LED1_TOGGLE()` 重写主循环，把 4 行写电平改成 2 行取反，理解 ODR 异或的用法。
3. 加入第三颗 LED：把 `Drivers/BSP/LED/led.h` 里仿照 `LED1_xxx` 宏再写一组（提示：MiniSTM32 上其它可自由使用的 IO 口需先查原理图），实现三灯流水，并在 `led_init()` 里加对应的 `sys_gpio_set()` 调用。

**衔接下一步**：本实验解决了"输出"。下一个 [实验2 按键输入实验](../实验2%20按键输入实验) 讲"输入"——同样是 CRL/CRH，只是 MODE/CNF 组合换成 `0x8`/`0x4`，并多了一个"用 ODR 位选择上拉还是下拉"的技巧；再往后实验 3 会把输入和中断控制器 EXTI/NVIC 串起来。
