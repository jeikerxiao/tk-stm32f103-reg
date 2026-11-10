# 实验38 UCOSII实验1-任务调度

> 一句话概括：把 LED 流水灯式的"轮询小程序"搬进 uC/OS-II 实时操作系统，用三个任务（开始任务、LED0 任务、LED1 任务）控制两颗 LED 以不同节奏闪烁，从而理解 RTOS 的"任务 + 优先级 + 系统节拍 + 上下文切换"这四件核心事。

## 一、实验简介

- **实验目的**（官方 `readme.txt`）：学习 UCOSII 的任务调度，初步认识 UCOSII。
  通俗地说，前面 37 个实验的代码都是"一段程序从头跑到尾"；本实验开始，程序变成
  好几条**同时**在跑的独立小循环（称为**任务**），由内核决定这一刻执行哪一条。
- **在学习路线中的位置**：前置实验是实验 1（跑马灯，LED 驱动）、实验 2（SYS 与
  延时，`SysTick` 计数）、实验 3（KEY，按键）。本实验是 uC/OS-II 三部曲（38/39/40）
  的第一篇，只讲任务与调度；实验 39 加信号量与邮箱，实验 40 加消息队列、信号量集
  （事件标志组）和软件定时器，实验 41 综合测试实验则整个工程都跑在 uC/OS-II 上。
- **实验平台**：正点原子 MiniSTM32 V4 开发板，主芯片 STM32F103RC（本文所用 Keil
  工程器件设为 STM32F103ZE，详见"八、注意事项"）。
- **RTOS 版本**：uC/OS-II V2.93.01（Silicon Labs 官方 ARMv7-M 移植），**寄存器版**
  工程，不依赖 HAL/标准库。

## 二、实验原理

### 2.1 裸机"前后台"与 RTOS 的区别

| 对比项 | 裸机前后台系统 | uC/OS-II（本实验） |
| --- | --- | --- |
| 程序形态 | 一个 `while(1)` 主循环（后台）+ 若干中断服务函数（前台） | 多个独立任务函数，每个都有自己的 `while(1)` 和独立栈 |
| 谁决定运行谁 | 程序员写死的顺序，靠 `delay_ms()` 死等 | 内核按**优先级**抢占：优先级高的就绪任务立刻运行 |
| 等待时干什么 | CPU 空转，什么也做不了 | `OSTimeDly()` 让出 CPU，别的任务可以跑 |
| 实时性 | 前一个任务不结束，后一个事件就得等 | 高优先级任务可打断低优先级任务 |
| 代码组织 | 函数堆在一起，互相牵制 | 每个任务职责单一，通过事件/队列通信 |

一句话：**RTOS 就是一个"任务调度器 + 一堆同步通信机制"的库**，它把 CPU 时间按
优先级分给各个任务，任务之间通过内核对象（信号量、邮箱、队列……）打交道。

### 2.2 任务、优先级与堆栈

- **任务**：一个永远不会返回的 `void` 函数，内部通常是 `while(1){...}`。
- **优先级**：uC/OS-II 用 0~63 的整数表示，**数字越小优先级越高**。本工程的
  `OS_LOWEST_PRIO = 63u`，即最多 64 个优先级，每个优先级只能有唯一一个任务。
- **任务栈**：每个任务必须有自己的一块数组作堆栈（`OS_STK xxx[N]`，单位是 4 字节），
  用于保存被中断时的寄存器值和局部变量；`OS_STK_GROWTH = 1u` 表示栈**由高地址向
  低地址**增长（Cortex-M 的标准行为），所以创建任务时栈顶传 `&STK[N-1]`、栈底传 `&STK[0]`。
- **系统保留优先级**（由 `os_cfg.h` 宏算出，见 [ucos_ii.h](Middlewares/uC-OS2/uC-OS2/Source/ucos_ii.h)）：

| 优先级 | 宏 | 数值 | 作用 |
| --- | --- | --- | --- |
| 63 | `OS_TASK_IDLE_PRIO`（`= OS_LOWEST_PRIO`） | 63 | 空闲任务，CPU 无事可做时运行，统计占用率的基准 |
| 62 | `OS_TASK_STAT_PRIO`（`= OS_LOWEST_PRIO - 1`） | 62 | 统计任务（`OS_TASK_STAT_EN = 1u` 时存在），算 CPU 占用率 |
| 61 | `OS_TASK_TMR_PRIO`（`app_cfg.h` 中 `= OS_LOWEST_PRIO - 2u`） | 61 | 软件定时器任务（本实验未启用定时器功能） |
| 10 | `START_TASK_PRIO` | 10 | 开始任务：创建其它任务后自我挂起 |
| 7 | `LED0_TASK_PRIO` | 7 | LED0（DS0）任务 |
| 6 | `LED1_TASK_PRIO` | 6 | LED1（DS1）任务，优先级比 LED0 高 |

### 2.3 系统节拍（SysTick tick）

RTOS 需要一个"节拍器"来知道时间过了多久。本实验：

- `OS_TICKS_PER_SEC = 1000u`，即每 1 ms 一个节拍（1 个 tick = 1 ms）。
- 节拍由 Cortex-M3 内部的 **SysTick** 定时器产生，`start_task()` 里计算并重载：

```c
cnts = (CPU_INT32U)((72 * 1000000) / OS_TICKS_PER_SEC);   /* 72000000 / 1000 = 72000 */
OS_CPU_SysTickInit(cnts);
```

即 SysTick 重装载值 = 72000（`SysTick->RVR = cnts - 1 = 71999`），72 MHz 时钟计 72000
次正好 1 ms，溢出一次就进入 `SysTick_Handler()` → `OS_CPU_SysTickHandler()` →
`OSTimeTick()`，把所有处于 `OSTimeDly()` 中的任务的倒计数减 1，减到 0 的就变为就绪态。

### 2.4 上下文切换是怎么发生的（PendSV）

"上下文"就是一个任务此刻的寄存器组合。切换 = 把当前任务的寄存器存进它自己的栈，
再把下一个任务的寄存器从它的栈里取出来。Cortex-M3 用 **PendSV 异常**做这件事，
并被设成**最低优先级**（`NVIC_PENDSV_PRI EQU 0xFF`），这样切换一定发生在所有中断之后，
不会打断别的中断。实现见
[os_cpu_a.asm](Middlewares/uC-OS2/uC-OS2/Ports/ARM-Cortex-M/ARMv7-M/ARM/os_cpu_a.asm)：

```asm
NVIC_INT_CTRL   EQU     0xE000ED04      ; 中断控制与设置寄存器 ICSR 的地址
NVIC_SYSPRI14   EQU     0xE000ED22      ; 优先级14寄存器，里面放着 PendSV 优先级
NVIC_PENDSV_PRI EQU           0xFF      ; PendSV优先级设为最低
NVIC_PENDSVSET  EQU     0x10000000      ; 触发PendSV的位(bit28 PENDSVSET)

OSCtxSw                                 ; 任务级切换
OSIntCtxSw                              ; 中断级切换
    LDR R0,=NVIC_INT_CTRL
    LDR R1,=NVIC_PENDSVSET
    STR R1,[R0]                         ; 写 ICSR.PENDSVSET=1，挂起一次 PendSV
    BX  LR

OS_CPU_PendSVHandler
    CPSID I                             ; 关中断，保护切换过程
    MRS R0,PSP                          ; 取当前任务栈指针 PSP
    STMFD R0!,{R4-R11,R14}              ; 把硬件没自动保存的 R4~R11、LR 压入该任务栈
    LDR R5,=OSTCBCur
    LDR R1,[R5]
    STR R0,[R1]                         ; 栈指针写回 OSTCBCur->OSTCBStkPtr（保存现场）
    ...
    ORR LR,R4,#0x04                     ; 异常返回时使用的是 PSP 栈
    LDMFD R0!,{R4-R11,R14}              ; 恢复新任务的 R4~R11、LR
    MSR PSP,R0                          ; 新任务的 PSP
    BX  LR                              ; 异常返回，硬件再自动恢复 R0~R3/R12/LR/PC/xPSR
```

文字流程（任务主动让出 CPU 时）：

1. 任务调用 `OSTimeDly(80)` → 内核把该任务置为等待态并把 `80` 记入等待时钟数；
2. 内核调度函数 `OS_Sched()` 用 `OSMapTbl`/`OSHighRdy` 找出当前优先级最高且就绪的任务；
3. 若与当前任务不同，则执行 `OS_TASK_SW()`，也就是 `OSCtxSw` → 触发 PendSV；
4. CPU 正在执行的这条指令结束后进入 PendSV 服务函数，按上面汇编保存/恢复现场；
5. 返回后运行的就是另一个任务了，整个过程对任务函数本身完全透明。

### 2.5 临界区保护（`OS_ENTER_CRITICAL`）

创建任务时必须连着修改多个全局表，中途不能被中断打断，所以要用**临界区**。
本移植 `OS_CRITICAL_METHOD = 3u`（方法 3：进入前保存 `BASEPRI`/中断屏蔽状态到局部变量，
退出时还原），配套 `OS_CPU_SR_Cnt` 计数，实现为 `OS_CPU_SR_Save()/OS_CPU_SR_Restore()`
（`CPSID I` + `MSR BASEPRI` + `DSB`/`ISB`），阈值来自
[cpu_cfg.h](Middlewares/uC-OS2/uC-CPU/Cfg/Template/cpu_cfg.h)：
`CPU_CFG_KA_IPL_BOUNDARY = 1u`、`CPU_CFG_NVIC_PRIO_BITS = 4u`，
故 `OS_KA_BASEPRI_Boundary = 1 << (8-4) = 0x10`，即优先级 1 及以上的中断被屏蔽。

## 三、硬件资源与接线

| 资源 | 引脚 | 代码中的宏/位置 | 说明 |
| --- | --- | --- | --- |
| LED0（DS0 红灯） | PA8 | `LED0_GPIO_PORT/PIN`，[led.h](Drivers/BSP/LED/led.h) | 低电平点亮；`LED0(0)` 亮，`LED0(1)` 灭 |
| LED1（DS1 绿灯） | PD2 | `LED1_GPIO_PORT/PIN`，[led.h](Drivers/BSP/LED/led.h) | 低电平点亮；`LED1(0)` 亮，`LED1(1)` 灭 |
| GPIOA 时钟 | — | `RCC->APB2ENR \|= 1 << 2` | `LED0_GPIO_CLK_ENABLE()`，开启 IOA 时钟 |
| GPIOD 时钟 | — | `RCC->APB2ENR \|= 1 << 5` | `LED1_GPIO_CLK_ENABLE()`，开启 IOD 时钟 |
| PA 口引脚配置 | PA8 | `GPIOA->CRH`（由 `sys_gpio_set()` 写入） | 推挽输出、中速、上下拉使能 |
| PD 口引脚配置 | PD2 | `GPIOD->CRL`（由 `sys_gpio_set()` 写入） | 同上 |
| SysTick | 片内 | [delay.c](Drivers/SYSTEM/delay/delay.c)、`os_cpu_c.c` | 1 ms 系统节拍 |
| PendSV | 片内异常 | `os_cpu_a.asm` | 上下文切换执行者 |

- 两颗 LED 都在开发板上，**无需任何外接模块、无需跳线帽**，USB 供电即可。
- 时钟：`sys_stm32_clock_init(9)` 把 HSE(8 MHz) × PLL 倍频 9 倍 = **72 MHz**，
  `delay_init(72)` 告诉延时模块当前 HCLK 是 72 MHz，`SysTick->CTRL |= (1 << 2)`
  选择 SysTick 时钟源为 HCLK（内部 72 MHz），因此 1 个 SysTick 计数 = 1/72 μs。

## 四、代码结构与调用流程

```
实验38 UCOSII实验1-任务调度/
├── User/
│   ├── main.c              只做 4 件事：配时钟、初始化延时、初始化 LED、进 RTOS
│   ├── uc-os2_demo.c       三个任务的定义与创建（本实验主角）
│   └── uc-os2_demo.h
├── Drivers/
│   ├── BSP/LED/led.c/.h    LED0(PA8)/LED1(PD2) 寄存器级驱动
│   ├── SYSTEM/sys/         sys_stm32_clock_init()、sys_gpio_set() 等寄存器工具
│   ├── SYSTEM/usart/       本实验未调用 usart_init()，仅被 include
│   ├── SYSTEM/delay/       SysTick 中断服务函数 + delay_us/delay_ms(OS 感知版)
│   └── CMSIS/              启动文件与寄存器定义（startup_stm32f103xe.s）
└── Middlewares/
    ├── MALLOC/             本实验未使用
    └── uC-OS2/
        ├── uC-OS2/Source/    uC/OS-II 内核源码（os_task.c/os_core.c/...）
        ├── uC-OS2/Ports/ARM-Cortex-M/ARMv7-M/ARM/  os_cpu_c.c + os_cpu_a.asm（切换汇编）
        ├── uC-OS2/Cfg/Template/os_cfg.h            OS 裁剪配置（"OSCfg"）
        ├── uC-CPU/          CPU 相关抽象（cpu_core.c、cpu_cfg.h）
        └── uC-LIB/          通用库（lib_str/lib_mem 等）
```

`main()` → `uc_os2_demo()` 的流程：

1. `sys_stm32_clock_init(9)`：72 MHz。
2. `delay_init(72)`：配置 SysTick 时基，此时 `OSRunning == OS_FALSE`，
   `SysTick_Handler()` 里的 `if (delay_osrunning == OS_TRUE)` 不成立，不会产生节拍。
3. `led_init()`：开 GPIOA/GPIOD 时钟，PA8、PD2 设为推挽输出，两颗灯初始为灭。
4. `uc_os2_demo()`：`OSInit()` → `OSTaskCreateExt(start_task…)` → `OSStart()`，
   **从此不返回**（后面的 `for(;;){}` 永远进不去）。
5. `start_task`（优先级 10）：`OSStatInit()` → 计算并初始化 SysTick 节拍 →
   进临界区创建 `led0_task`(7) 与 `led1_task`(6) → 出临界区 →
   `OSTaskSuspend(START_TASK_PRIO)` 把自己挂起，此后不再运行。
6. `led0_task`/`led1_task` 交替运行，各自用 `OSTimeDly()` 控制闪烁节奏；
   两者都不就绪时，CPU 落到优先级 63 的空闲任务。

## 五、核心代码解析

### 5.1 [User/main.c](User/main.c)（全文）

```c
int main(void)
{
    sys_stm32_clock_init(9);    /* 设置时钟, 72Mhz */
    delay_init(72);             /* 延时初始化 */
    led_init();                 /* 初始化LED */
    uc_os2_demo();              /* 运行uC/OS-II例程 */
}
```

进入 `uc_os2_demo()` 后 CPU 就"交给"内核了，所以 `main` 里**没有** `while(1)`；
另外本实验刻意不调用 `usart_init()`，串口只是被包含而未使用。

### 5.2 内核启动三部曲：`OSInit` → `OSTaskCreateExt` → `OSStart`

```c
void uc_os2_demo(void)
{
    OSInit();                                    /* UCOS初始化 */
    OSTaskCreateExt((void(*)(void *) )start_task,
                    (void *          )0,                                    /* 传给任务的参数 */
                    (OS_STK *        )&START_TASK_STK[START_STK_SIZE - 1],  /* 栈顶 */
                    (INT8U           )START_TASK_PRIO,                      /* 优先级 10 */
                    (INT16U          )START_TASK_PRIO,                      /* 任务ID=优先级 */
                    (OS_STK *        )&START_TASK_STK[0],                   /* 栈底 */
                    (INT32U          )START_STK_SIZE,                       /* 128 个 OS_STK */
                    (void *          )0,
                    (INT16U          )OS_TASK_OPT_STK_CHK | OS_TASK_OPT_STK_CLR | OS_TASK_OPT_SAVE_FP);
    OSStart();                                   /* 开始任务 */
    for (;;) { /* 不会进入这里 */ }
}
```

- `OSInit()` 建立空闲任务与（`OS_TASK_STAT_EN=1` 时的）统计任务，清空就绪表。
- `OSTaskCreateExt` 比 `OSTaskCreate` 多了栈边界检查（`PstkLimit`/`StkSize`）和选项：
  `OS_TASK_OPT_STK_CHK` 允许栈检测、`OS_TASK_OPT_STK_CLR` 创建时清零栈、
  `OS_TASK_OPT_SAVE_FP` 保存浮点寄存器（本工程 `OS_CPU_ARM_FP_EN` 随编译器 `{FPU}`
  宏而定，作者说明"为了保险起见，所有任务都保存浮点寄存器的值"）。
- `OSStart()` 只运行**已就绪的最高优先级任务**，此时只有 `start_task`，所以从它开始。

### 5.3 `start_task`：统计任务与 SysTick 的先后顺序

```c
void start_task(void *pdata)
{
    OS_CPU_SR cpu_sr = 0;
    CPU_INT32U cnts;

    OSStatInit();                   /* 开启统计任务 */
    cnts = (CPU_INT32U)((72 * 1000000) / OS_TICKS_PER_SEC);
    OS_CPU_SysTickInit(cnts);
    OS_ENTER_CRITICAL();            /* 进入临界区(关闭中断) */
    /* ...创建 led0_task(优先级7)、led1_task(优先级6)... */
    OS_EXIT_CRITICAL();             /* 退出临界区(开中断) */
    OSTaskSuspend(START_TASK_PRIO); /* 挂起开始任务 */
}
```

- **为什么先 `OSStatInit()` 再开 SysTick？** 统计任务需要一段"CPU 全给了空闲任务"的
  干净时间来标定 1 个节拍内空闲任务能执行多少次；此时还没有其它任务被创建、SysTick
  还没跑，标定才准确。次序颠倒会让 `OSCPUUsage` 初值失真。
- **为什么创建任务要包在临界区里？** 一开 SysTick 就有 1 ms 中断在跑，若中断正好插在
  "改就绪表"的中间，可能取出错误的最高优先级而切到 uninitialized 的 TCB。
- **`OSTaskSuspend(START_TASK_PRIO)`**：`OS_TASK_SUSPEND_EN = 1u` 才允许使用。
  开始任务把灯任务创建完就永久退出竞争，这就是官方所说"开始任务用于创建其它任务，之后挂起"。

### 5.4 两个 LED 任务：`OSTimeDly()` 是"睡"不是"等"

```c
void led0_task(void *pdata)
{
    while (1)
    {
        LED0(0);   /* 点亮 DS0（低电平有效） */
        OSTimeDly(80);    /* 让出CPU 80 个节拍 = 80 ms */
        LED0(1);   /* 熄灭 DS0 */
        OSTimeDly(920);   /* 再让出 920 ms，合计 1 秒一个周期 */
    }
}

void led1_task(void *pdata)
{
    while (1)
    {
        LED1(0);
        OSTimeDly(300);   /* 300 ms */
        LED1(1);
        OSTimeDly(300);   /* 300 ms */
    }
}
```

- 1 节拍 = 1 ms，所以数字直接就是毫秒数：DS0 亮 80 ms、灭 920 ms（周期 1000 ms）；
  DS1 亮灭各 300 ms（周期 600 ms）。
- `OSTimeDly(n)` 把本任务从就绪表里摘掉并写入 `OSTCBDly = n`，然后调用
  `OS_Sched()` → `OSCtxSw` 触发 PendSV，**CPU 立刻去跑别的任务**，绝不是原地空转，
  这就是"任务调度"最直观的体现。
- 注意 `LED1_TASK_STK` 定义时写的是 `OS_STK LED1_TASK_STK[LED0_STK_SIZE];`——两个
  宏都是 128，行为不变，但属于源码里的复制粘贴痕迹（见"八、注意事项"）。

## 六、编译、下载与运行

1. 用 Keil MDK 打开 [Projects/MDK-ARM/atk_f103.uvprojx](Projects/MDK-ARM/atk_f103.uvprojx)。
2. 器件与宏：工程 Options→Device 选 **STM32F103ZE**，C/C++ 定义为 `STM32F103xE`，
   启动文件 `Drivers/CMSIS/.../arm/startup_stm32f103xe.s`（其向量表把
   `SysTick_Handler`、`PendSV_Handler` 分别指向 `OS_CPU_SysTickHandler` 所在的
   C 处理和汇编 `OS_CPU_PendSVHandler`），不要自行改动这两行。
3. Include Path 需包含 `Middlewares/uC-OS2/uC-OS2/Source`、`.../uC-OS2/Ports/ARM-Cortex-M/ARMv7-M`、
   `.../uC-OS2/Cfg/Template`、`.../uC-CPU/...`、`.../uC-LIB/...`（工程已配好）。
   内核源码是**逐文件**加入工程的（`os_*.c`），并没有把 `ucos_ii.c` 这个"大合集文件"
   放进去编译——若自己新增文件，别同时加 `ucos_ii.c`，否则会重复定义。
4. 编译（本实验 Optim 等级较低，便于调试），输出在 `Output/` 下生成 `atk_f103.axf/hex`。
5. 用 ST-Link/J-Link 下载（Debug 选项选对应仿真器，Flash Download 勾 Add 对应 .FLM）。
6. 烧写后按一下板上 **RESET**，观察两颗 LED 即可；本实验不需要串口助手。

## 七、实验现象

1. 上电/复位后，LED0(DS0 红灯) 与 LED1(DS1 绿灯) 按各自节奏不停闪烁：
   - DS0：**每 1 秒亮一次，亮 80 ms**（肉眼看到一闪一闪的短促红光）；
   - DS1：**亮 300 ms、灭 300 ms**，明显比 DS0 快，像呼吸一样的绿灯。
2. 两者互不影响：DS0 亮着的 80 ms 里，DS1 该灭就灭——说明它们是两个独立任务，
   不是同一段代码顺序执行出来的。
3. 若想验证调度真的在跑，可在 Keil 里 Watch 窗口观察全局变量 `OSRunning`（变 1）、
   `OSPrioCur`（当前运行任务优先级，在 6 与 7 之间跳）、`OSTime`（节拍计数，一直增大）。

## 八、注意事项

1. **SysTick 中断只能有一份实现。** 官方 `readme.txt` 特别强调"UCOS 使用的 Systick
   中断在 `delay.c` 中，不要重复实现了"。本工程的 `SysTick_Handler()` 位于
   [delay.c](Drivers/SYSTEM/delay/delay.c)，内容只有 `if (delay_osrunning == OS_TRUE)
   OS_CPU_SysTickHandler();`；若在启动文件或其它地方再写一个同名函数，节拍会丢，
   表现为"任务卡住不动"或编译时向量冲突。
2. **`OS_CFG.H` 就是所谓的 OSCfg。** uC/OS-II 没有 `OSCfg` 结构体（那是 uC/OS-III 的
   写法），本实验的所有裁剪都是 [os_cfg.h](Middlewares/uC-OS2/uC-OS2/Cfg/Template/os_cfg.h)
   里的宏。关键项（本实验实际取值）：

| 宏 | 值 | 含义 |
| --- | --- | --- |
| `OS_LOWEST_PRIO` | 63u | 最低（数值最大）优先级，决定可用优先级数量 |
| `OS_MAX_TASKS` | 20u | 用户任务上限（不含 idle/stat） |
| `OS_TICKS_PER_SEC` | 1000u | 每秒节拍数，1 tick = 1 ms，本实验的延时常数都基于它 |
| `OS_TASK_STAT_EN` | 1u | 启用统计任务，故必须调用 `OSStatInit()` |
| `OS_TASK_SUSPEND_EN` | 1u | 启用 `OSTaskSuspend()`，`start_task` 自我挂起靠它 |
| `OS_SCHED_LOCK_EN` | 1u | 启用 `OSSchedLock()`，`delay_us()` 靠它防打断 |
| `OS_SEM_EN/OS_MBOX_EN/OS_Q_EN/OS_FLAG_EN/OS_TMR_EN` | 1u | 本实验用不到，但实验 39/40 要用 |
| `OS_STK_GROWTH`（`os_cpu.h`） | 1u | 栈从高地址往低地址增长 |
| `OS_CRITICAL_METHOD`（`os_cpu.h`） | 3u | 用 `CPU_SR_Save/Restore` 保护临界区 |

3. **芯片型号与容量差异**：官方文档/板子是 STM32F103RC（Flash 256 KB、SRAM 48 KB），
   但本实验 Keil 工程器件选的是 STM32F103ZE 并定义 `STM32F103xE`（512 KB Flash）。
   对代码没有影响（只用片内资源，且启动文件与寄存器头文件都是 `xE` 系列，兼容 RC），
   只是别据此认为程序能有 512 KB——超出 RC 的 256 KB 部分下载不进去或运行不了。
4. **源码里的一个小笔误**：`OS_STK LED1_TASK_STK[LED0_STK_SIZE];` 用了 LED0 的宏。
   两者都是 128，功能无差异，但如果你只改 `LED1_STK_SIZE` 会发现栈大小没变。
5. **`delay_ms()` 在 OS 里语义变了**：`OSRunning` 为真且不在中断里时，它内部调用
   `OSTimeDly(nms / g_fac_ms)`（`g_fac_ms = 1000/1000 = 1`），也就是"睡 nms 毫秒并把
   CPU 让出去"；中断里调用则退回纯 SysTick 计数忙等。在任务里滥用 `delay_ms()` 会
   拉长高优先级任务的响应时间。
6. **`delay_us()` 会锁调度**：它用 `OSSchedLock()/OSSchedUnlock()` 包住忙等，
   因此不要在长循环里做微秒延时，否则整个系统看起来"卡住"。
7. **任务优先级不可重复**：给三个任务分别用了 10/7/6，若新加任务撞上已用的数字，
   `OSTaskCreateExt` 会失败（`OS_ERR_PRIO_INVALID`），任务不运行但不会报错停机，
   现象变成"少一颗灯闪"，排查时优先检查优先级表。
8. 本工程所有 `.c/.h` 与 `readme.txt` 都是 **GBK 编码**，编辑器需按 GBK 打开，
   否则中文注释乱码（不影响编译）。

## 九、总结与延伸

**本实验要点**

1. RTOS 与裸机的根本差别：CPU 归谁由内核按优先级决定，任务用"阻塞"代替"空转"。
2. 任务的三要素：任务函数（不返回）、独立栈（`OS_STK` 数组）、唯一优先级（小者高）。
3. uC/OS-II 启动固定套路：`OSInit()` → 创建开始任务 → `OSStart()` → 开始任务里
   `OSStatInit()`、初始化 SysTick、临界区内创建业务任务、最后自我挂起。
4. 系统节拍：`OS_TICKS_PER_SEC=1000` → SysTick 重装 72000 → 1 ms 一次
   `OS_CPU_SysTickHandler()`，`OSTimeDly()` 的计数全靠它递减。
5. 上下文切换发生在 PendSV：`OSCtxSw` 只是把 `0x10000000` 写进 `0xE000ED04` 触发
   PendSV，真正的保存/恢复由 `OS_CPU_PendSVHandler` 用 PSP 完成。

**延伸练习（都在本实验代码上直接改）**

1. 把 DS0 改成"亮 200 ms、灭 200 ms"，同时把 `OS_TICKS_PER_SEC` 从 1000u 改成 100u，
   观察 `OSTimeDly(20)` 的效果与 `start_task` 中 `cnts` 计算值（72000000/100=720000）
   如何随之变化——体会"节拍周期"这个抽象。
2. 新增第三个任务 `led2_task`（优先级 5，栈 128），让 DS0 与 DS1 同步快闪 20 次后
   `OSTaskSuspend(LED1_TASK_PRIO)` 把 DS1 停掉，再 `OSTaskResume(LED1_TASK_PRIO)` 恢复，
   验证 `OS_TASK_SUSPEND_EN` 的作用。
3. 把 `start_task` 里的 `OS_ENTER_CRITICAL()/OS_EXIT_CRITICAL()` 注释掉，思考为什么
   大多数时候现象仍然正常、但存在极小概率的调度异常——把"临界区"的必要性想清楚。

**衔接**：实验 39 在此基础上加入**邮箱**（传递按键值）与**信号量**（控制 LED1），
开始讲任务间通信与互斥；实验 40 再加消息队列、事件标志组（正点原子称"信号量集"）
与软件定时器；实验 41 综合测试实验则演示一个真正的多任务 LCD 应用。
