# 实验32 DSP测试实验

> 一句话概括：给 STM32F103 挂上 CMSIS-DSP 数学库，用两个子实验分别对比"DSP 版 sin/cos 与标准库版的速度差"和"1024 点 FFT 的实际耗时与频谱结果"，学会用定时器给运算"掐表"。

本实验包含两个独立子工程：

- `实验32_1 DSP BasicMath测试`——基础数学函数（三角函数）速度对比；
- `实验32_2 DSP FFT测试`——基 4 快速傅里叶变换（FFT）测试。

## 一、实验简介

- **实验目的**（官方 readme.txt）：
  - 32_1：学习 DSP 库基础数学函数功能，比较 `arm_sin_f32/arm_cos_f32` 与标准库 `sinf/cosf` 的速度差别；
  - 32_2：学习 DSP 功能（FFT 计算）的使用。
  通俗地说：STM32 名字里虽然带"M"（媒体处理），但拿它做信号处理时，裸写 C 的数学函数其实很慢。CMSIS-DSP 是 ARM 官方提供的"手工优化的数学函数库"，本实验让你**亲手测出**它快多少，并体验一次真正的频谱分析。
- **在学习路线中的位置**：前置实验11 TFTLCD（显示结果）、实验4 串口（输出数据）、实验7/8 定时器（掐表计时）。是后面理解音频/传感器信号处理的铺垫。
- **开发板与芯片**：正点原子 MiniSTM32 V4，主芯片 STM32F103RC（Cortex-M3 @72MHz，无 FPU 浮点单元，浮点运算全靠软件模拟——这正是 DSP 库优化空间所在）。

## 二、实验原理

### 2.1 CMSIS-DSP 库是什么

CMSIS-DSP 是 ARM 提供的开源数学库，本工程使用的是**预编译好的静态库**：

- 库文件：`Drivers/CMSIS/Lib/ARM/arm_cortexM3l_math.lib`（`M3l` = Cortex-M3 小端序版本），已作为 `arm_cortexM3l_math.lib` 加入 Keil 工程；
- 头文件：[Drivers/CMSIS/DSP/Include/arm_math.h](实验32_1%20DSP%20BasicMath测试/Drivers/CMSIS/DSP/Include/arm_math.h)，函数声明都在这里（本实验用到 `arm_sin_f32`、`arm_cos_f32`、`arm_cfft_radix4_init_f32`、`arm_cfft_radix4_f32`、`arm_cmplx_mag_f32` 等）；
- 必须在 Keil 工程里定义宏 `ARM_MATH_CM3`（两个子工程的 `.uvprojx` 预处理宏均为 `STM32F103xE,ARM_MATH_CM3,__CC_ARM,...`），头文件才会选择正确的内核分支。

库里除了三角函数，还有成系列的"基本数学函数"（数组加/减/乘/点积 `arm_add_f32/arm_mult_f32/arm_dot_prod_f32`、均值方差、排序滤波等）。本工程代码只实测了 sin/cos，其余函数的用法同理：给指针、给长度、库内循环算完。

### 2.2 为什么 Cortex-M3 上 DSP 库能更快（MAC 与"SIMD"的真相）

- **MAC（Multiply-Accumulate，乘加）**：`acc += a*b` 这种"先乘再累加"是信号处理里出现频率最高的动作。Cortex-M3 内置**单周期 32 位硬件乘法器**，支持 `MUL`（乘）、`MLA`（乘加）以及半字乘 `SMULxy`、字乘加 `SMLAWy/SMLAxy` 等指令；而 C 语言里的 `float a*b+c` 在**没有 FPU** 的 M3 上要拆成几十条指令的软件浮点例程。
- **关于 SIMD**：真正的"单指令多数据"饱和运算（QADD 之类）是 Cortex-M4 的 DSP 扩展才有的，**M3 没有**。所以 M3 版库的优化思路是：定点 Q15/Q31 格式让数据进 32 位寄存器 + 上面那批乘加指令 + 查表（`arm_common_tables.h` 里的正弦/旋转因子表）+ 手工循环展开。
- 具体到 `arm_sin_f32/arm_cos_f32`：标准库 `sinf/cosf` 用泰勒级数迭代求值，慢；DSP 库把 0~2π 做成**查找表 + 线性插值**，几次乘加就出结果，精度对多数工程够用，速度快好几倍——这正是 32_1 要你测出来的东西。

### 2.3 FFT 是什么（通俗版）

任何一个随时间变化的信号，都可以看成"一堆不同频率的正弦波叠加"。FFT（快速傅里叶变换）就是把**时间域**的 N 个采样点，换算成**频率域**的 N 个复数——告诉你"每个频率成分各占多少"。

- 直接按定义算（DFT）要 N² 次乘加：1024 点 ≈ 105 万次；
- FFT 的分治思想：把 N 点变换拆成小变换逐层合并。N=1024=4×4×4×4×4，可以拆成 **5 层、每层 4 点**的"蝶形"运算，复杂度降到约 (N/4)·log₄N ≈ 1.3 万次量级，快约百倍；
- "radix4（基 4）"就是指每级蝶形处理 4 个点。CMSIS-DSP 的 `arm_cfft_radix4_instance_f32` 结构体里预存了旋转因子表、位反转表（`fftLen`、`pTwiddle`、`pBitRevTable`、`bitRevLength`、`onebyfftLen` 等字段）；
- 输入输出都是 `float32_t` 数组，**复数交错存储**：`[Re0, Im0, Re1, Im1, ...]`，长度 2N；`ifftFlag=0` 做正变换，`bitReverseFlag=1` 让输出按频率自然顺序排列；
- 最后用 `arm_cmplx_mag_f32()` 对每个复数求模 `sqrt(Re²+Im²)` 得到幅值谱。

### 2.4 耗时是怎么测出来的——基本定时器当"秒表"

两个子实验都用 [Drivers/BSP/TIMER/btim.c](实验32_1%20DSP%20BasicMath测试/Drivers/BSP/TIMER/btim.c) 的 TIM6（基本定时器）做计时器，而**不是** DWT：

1. `btim_timx_int_init(arr, psc)`：TIM6 时钟 = 72MHz（APB1 为 36MHz，但定时器时钟自动 ×2），计数频率 `Ft = 72MHz/(psc+1)`；
2. 被测运算前把 `BTIM_TIMX_INT->CNT = 0` 清零、溢出计数 `g_timeout = 0`；运算完读取 `time = CNT + g_timeout*65536`，单位就是"计数拍数"；
3. 溢出中断服务函数里 `g_timeout++`，解决 16 位计数器数不下的问题。

```c
void BTIM_TIMX_INT_IRQHandler(void)
{
    if (BTIM_TIMX_INT->SR & 0X0001)   /* 溢出中断 */
    {
        g_timeout++;
    }
    BTIM_TIMX_INT->SR &= ~(1 << 0);    /* 清除中断标志位 */
}
```

溢出周期公式（btim.c 注释）：`Tout = (arr+1)*(psc+1)/Ft (us)`，Ft 单位 MHz。

### 2.5 公共前提：时钟与显示

`sys_stm32_clock_init(9)`：8MHz HSE × PLL9 = 72MHz 系统时钟——所有时间测算都建立在这 72MHz 上；LCD 为 8080 并口 MCU 屏（GPIO 模拟），串口 115200 用于 printf 输出。

## 三、硬件资源与接线

两个子实验的公共部分：

| 资源 | 引脚 | 代码中的宏/位置 | 说明 |
| --- | --- | --- | --- |
| LED0（红灯） | PA8 | `LED0_GPIO_PORT/PIN`，led.h | 低电平点亮，循环中翻转指示运行 |
| LED1（绿灯） | PD2 | led.h | 本工程未使用（btim 中断里是注释掉的翻转） |
| 串口1 | PA9/PA10，CH340 | usart.h | 115200，32_2 用它打印 FFT 结果 |
| LCD（8080） | RD/WR/RS/CS=PC6~PC9, BL=PC10, D0~D15=PB0~PB15 | lcd.h | 显示计时结果 |
| 计时定时器 | 片上 TIM6 | `BTIM_TIMX_INT`，btim.h | 基本定时器+更新中断 |

### 3.1 实验32_1 DSP BasicMath测试

- 额外资源：无按键、无 SD 卡，只用 LCD+LED+串口+TIM6。
- 计时参数：`btim_timx_int_init(65535, 7200 - 1)`——计数频率 72MHz/7200 = **10kHz**，1 拍 = **0.1ms**；计满 65536 拍溢出，即 **6.5536 秒**一次中断。

### 3.2 实验32_2 DSP FFT测试

- 额外资源：KEY0=PC5（触发一次 FFT）、KEY1=PA15、KEY_UP=PA0（本工程未使用后两个）。

| 资源 | 引脚 | 代码位置 |
| --- | --- | --- |
| KEY0 | PC5（上拉输入，按下=0） | `KEY0_GPIO_PORT/PIN`，key.h |

- 计时参数：`btim_timx_int_init(65535, 72 - 1)`——计数频率 72MHz/72 = **1MHz**，1 拍 = **1μs**，可测出小数点后三位的毫秒耗时；计满 65536 拍溢出一次，即每 **65.536ms** `g_timeout` 加 1（一次 1024 点 FFT 远不到 65ms，通常不会溢出）。
- 接线：LCD 模块插右侧接口；如需看串口输出，USART1 排针跳线帽把 PA9→TXD、PA10→RXD。

## 四、代码结构与调用流程

```
实验32 DSP测试实验/
├── 实验32_1 DSP BasicMath测试/
│   ├── User/main.c              两轮"掐表"对比测试 + LCD 显示
│   └── Drivers/BSP/TIMER/btim.* 基本定时器 TIM6 秒表
└── 实验32_2 DSP FFT测试/
    ├── User/main.c              造信号→掐表做 FFT→串口打印幅值
    └── Drivers/BSP/TIMER/btim.* 同上
两工程共用：Drivers/CMSIS/DSP/Include/（头文件）、Drivers/CMSIS/Lib/ARM/arm_cortexM3l_math.lib（库）
```

**32_1 的 main() 流程**：

1. 初始化：时钟 72MHz → 延时 → 串口 → LED → LCD → `btim_timx_int_init(65535, 7200-1)`；
2. LCD 画提示文字 " No DSP runtime:" / "Use DSP runtime:"；
3. 循环体：清 CNT 和 g_timeout → `sin_cos_test(PI/6, 10000, 0)`（标准库版跑 1 万次）→ 读 CNT 算时间 → sprintf 显示；再用 mode=1（DSP 版）做同样一轮；LED0 翻转。

**32_2 的 main() 流程**：

1. 初始化（含 `key_init()`）→ `btim_timx_int_init(65535, 72-1)`；
2. `arm_cfft_radix4_init_f32(&scfft, FFT_LENGTH, 0, 1)`：初始化 FFT 实例（1024 点、正变换、输出位反转）；
3. 等 KEY0：按下的瞬间——生成 1024 点输入序列 → 掐表 → `arm_cfft_radix4_f32()` → 显示耗时 → `arm_cmplx_mag_f32()` 求幅值 → 串口逐个打印；
4. 未按键时每 10ms 扫描一次，每约 200ms 翻 LED0。

## 五、核心代码解析

### 5.1 实验32_1：正确性校验 + 双计时

```c
uint8_t sin_cos_test(float angle, uint32_t times, uint8_t mode)
{
    ...
    for (i = 0; i < times; i++)
    {
        cosx = arm_cos_f32(angle);          /* 使用DSP优化的sin，cos函数 */
        sinx = arm_sin_f32(angle);
        result = sinx * sinx + cosx * cosx; /* 计算结果应该等于1 */
        result = fabsf(result - 1.0f);      /* 对比与1的差值 */
        if (result > DELTA)return 0XFF;     /* 判断失败 */
        angle += 0.001f;                    /* 角度自增 */
    }
    return 0;
}
```

- 巧妙之处：用恒等式 **sin²x + cos²x ≡ 1** 顺带校验算得对不对（`DELTA = 0.0001f`），一次循环既测速又查错；`mode` 分支里换成 `sinf/cosf`，其余完全相同，保证对比公平；
- 每次角度加 0.001f，让 10000 次调用落在不同象限，避免测的是"同一表项"的运气值。

计时读取（main.c）：

```c
BTIM_TIMX_INT->CNT = 0;                     /* 重设定器计数器值 */
g_timeout = 0;
res = sin_cos_test(PI / 6, 10000, 0);
time = BTIM_TIMX_INT->CNT + (uint32_t)g_timeout * 65536;
sprintf(buf, "%0.1fms\r\n", time / 10);     /* 1拍=0.1ms，除以10得毫秒 */
```

`time` 是拍数，1 拍 = 0.1ms，所以毫秒数 = 拍数/10。LCD 上"error！"表示精度校验没过。

### 5.2 实验32_2：造一个"已知答案"的信号

```c
#define FFT_LENGTH      1024

for (i = 0; i < FFT_LENGTH; i++)
{
    fft_inputbuf[2 * i] = 100 +
                          10 * arm_sin_f32(2 * PI * i / FFT_LENGTH) +
                          30 * arm_sin_f32(2 * PI * i * 4 / FFT_LENGTH) +
                          50 * arm_cos_f32(2 * PI * i * 8 / FFT_LENGTH);  /* 实部 */
    fft_inputbuf[2 * i + 1] = 0;            /* 虚部全部为0 */
}
```

信号 = 直流 100 + 基频正弦(幅度10) + 4 倍频正弦(幅度30) + 8 倍频余弦(幅度50)。数组按"实部、虚部"交错存放，所以长度是 `FFT_LENGTH*2`。

FFT 实例初始化与运算：

```c
arm_cfft_radix4_instance_f32 scfft;
arm_cfft_radix4_init_f32(&scfft, FFT_LENGTH, 0, 1); /* 1024点、正变换(ifftFlag=0)、位反转输出(bitReverseFlag=1) */
arm_cfft_radix4_f32(&scfft, fft_inputbuf);          /* 原地计算，结果写回 fft_inputbuf */
arm_cmplx_mag_f32(fft_inputbuf, fft_outputbuf, FFT_LENGTH); /* 复数求模得幅值谱 */
```

结果如何核对（理论值，供比对串口输出）：1024 点 DFT 后，直流分量幅值 = 100×1024 = 102400（索引 0）；正弦/余弦分量幅值 = 幅度×N/2，即索引 1 ≈ 5120、索引 4 ≈ 15360、索引 8 ≈ 25600，并在 1023、1020、1016 处出现镜像峰。如果你在这几个下标看到接近这些数，说明 FFT 链路全通。

### 5.3 Keil 工程里 DSP 库怎么接入

`.uvprojx` 中：

- 分组里添加了 `..\..\Drivers\CMSIS\Lib\ARM\arm_cortexM3l_math.lib`（链接时自动挑选所需目标文件）；
- C 宏定义：`STM32F103xE,ARM_MATH_CM3,__CC_ARM,ARM_MATH_MATRIX_CHECK,ARM_MATH_ROUNDING,__weak=__WEAK`；
- 包含路径：`..\..\Drivers\CMSIS\Device\ST\STM32F1xx\Include;..\..\Drivers\CMSIS\Include;..\..\Drivers;..\..\User`，因此源码能写 `#include "./CMSIS/DSP/Include/arm_math.h"`。

## 六、编译、下载与运行

1. 分别打开各子工程的 `实验32_1 DSP BasicMath测试/Projects/MDK-ARM/atk_f103.uvprojx`、`实验32_2 DSP FFT测试/Projects/MDK-ARM/atk_f103.uvprojx`（两个独立工程，一次编译一个）。
2. 器件 STM32F103RC；若报 `arm_math.h not found` 或链接不到 `arm_cfft_radix4_f32`，检查上文第 5.3 节的宏与库文件是否还在。
3. ST-Link/J-Link 下载；32_2 需串口 115200 接收 FFT 数据（每按一次 KEY0 打印 1024 行浮点数，波特率别嫌慢）。

## 七、实验现象

### 7.1 实验32_1

1. LCD 显示 "STM32 / DSP BasicMath TEST / ATOM@ALIENTEK" 与两行提示；
2. " No DSP runtime:" 后显示标准库 `sinf/cosf` 算 10000 次总耗时；"Use DSP runtime:" 后显示 `arm_sin_f32/arm_cos_f32` 的总耗时；后者明显更小（常见实测快 3~5 倍以上，具体数值以你板上实测为准）；
3. 每完成一轮 LED0 翻转一次；任一轮若精度校验失败则该行显示 "error！"。

### 7.2 实验32_2

1. LCD 显示 "DSP FFT TEST / KEY0:Run FFT / FFT runtime:"；
2. 每按一次 KEY0：执行一次 1024 点 FFT，LCD 显示耗时，串口输出 `1024 point FFT runtime:...ms` 和 1024 个幅值；
3. 把 `FFT_LENGTH` 改成 16/64/256 重编译，可对比不同点数耗时（官方说明可选范围 16, 64, 256, 1024）；
4. 未按 KEY0 时 LED0 约 200ms 闪烁。

## 八、注意事项

- **代码与注释/readme 不一致之处（以代码为准）**：
  1. 两个子工程 `main.c` 里注释都写"重设 **TIM3** 定时器的计数器值"，但 [btim.h](实验32_1%20DSP%20BasicMath测试/Drivers/BSP/TIMER/btim.h) 定义 `BTIM_TIMX_INT = TIM6`（中断号 `TIM6_DAC_IRQn`），实际用的是**基本定时器 TIM6**；
  2. 32_2 的 `btim_timx_int_init(65535, 72 - 1)` 后面注释照抄了"10Khz计数频率"，实际是 72MHz/72 = **1MHz（1μs/拍）**；由此 32_2 中 LCD 显示用的 `time / 10` 配 "%0.3fms" 标签，数值并不是毫秒（是拍数/10），而串口里 `time / 1000` 才是正确的毫秒——看耗时**以串口输出为准**；
  3. 32_1 的 readme.txt 硬件资源里列了独立按键，但该工程代码未初始化、未使用按键。
- `arm_cfft_radix4_init_f32/arm_cfft_radix4_f32` 在新版 CMSIS-DSP 中标注了 `/* Deprecated */`，但本工程所用库版本仍完整提供，可正常使用；
- 计时区间只包住"被测函数"：32_1 的 CNT 清零紧贴 `sin_cos_test(...)`（10000 次三角函数），32_2 的造信号循环（每次按 KEY0 要调用 2048 次 `arm_sin_f32` + 1024 次 `arm_cos_f32`）发生在 CNT 清零**之前**，不计入 FFT 耗时——读代码时注意 `BTIM_TIMX_INT->CNT = 0` 的位置；
- 浮点 `sprintf` 打印需要完整的 C 运行时库，本工程默认未开 MicroLib；若自行改开 MicroLib，`%f` 可能不工作；
- 运算期间 TIM6 溢中断照常响应，若被测运算超过 6.5536s（不会），结果要靠 g_timeout 修正，这也是 `time = CNT + g_timeout*65536` 的原因。

## 九、总结与延伸

**要点：**

1. CMSIS-DSP 接入三件事：`.lib` 进工程、`ARM_MATH_CM3` 宏、`arm_math.h` 可见；
2. Cortex-M3 的加速资本是单周期乘加（MUL/MLA/SMULxy/SMLAxy）+ 查表，而不是 M4 的 SIMD；sin/cos 靠"查表+插值"击败标准库的多项式迭代；
3. FFT 把 N² 降为 ~N·logN；基 4 算法要求 N 是 4 的幂（16/64/256/1024）；复数交错数组 `x[2i]=Re, x[2i+1]=Im` 是使用时的最大坑；
4. 用定时器 CNT+溢出计数做秒表，测得 72MHz 无 FPU 的 M3 上一次 1024 点 float FFT 的毫秒级真实耗时；
5. 结果校验思想：sin²+cos²=1 校验三角函数，"已知频率成分的合成信号"校验 FFT。

**延伸练习：**

1. 把 32_1 的 `times` 改成 100000 再对比两行时间，验证加速比是否稳定；
2. 在 32_2 的信号公式里增加一项 `20*arm_sin_f32(2*PI*i*20/FFT_LENGTH)`，然后在串口输出里找索引 20 附近的峰，看理论幅值 20×1024/2=10240 是否吻合；
3. 把 32_2 的 `mode` 思路反向用：把 `arm_cfft_radix4_init_f32` 的 `ifftFlag` 置 1 做一次逆变换（需把输入虚部补齐成共轭对称谱），观察能否还原时域波形。

**衔接**：会用 DSP 库"算得快"之后，实验33 手写识别（识别库同样是"黑盒 lib + 接口函数"的用法）和实验41 综合测试会把外设与算法串起来。
