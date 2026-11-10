# 实验37 USB虚拟串口实验

> 一句话概括：用 STM32F103 自带的 USB 外设冒充一个"串口设备"（CDC 类），电脑插上 USB 线就多出一个 COM 口——没有 CH340、没有波特率同步问题，照样收发字符串。

## 一、实验简介

- 实验目的（官方 `readme.txt`）：学习 STM32 的 USB 功能使用，实现一个 USB 虚拟串口功能。
- 通俗展开："虚拟串口"（Virtual COM Port，VCP）指设备上并没有真的 UART 外设，而是固件按 USB 的 CDC（Communication Device Class，通信设备类）规范把自己描述成调制解调器/串口，电脑装了驱动后凭空出现一个 COM 口。之后串口助手对这个 COM 口的每一次读写，都被封装成 USB 数据包在总线上传递。
- 与实验4（串口实验）的关系：官方 readme.txt 说本例程"功能完全和《串口通信实验》相同，只不过串口变成了 STM32 的 USB 虚拟串口"——协议格式（回车换行结尾、200 字节上限、状态机判包）原样保留。
- 学习路线位置：与实验36（USB 读卡器）共用同一套 ST USB 设备库，只是把 MSC 类换成 CDC 类；建议先看实验36 的枚举原理，或至少读完本文 2.1/2.2。
- 开发板与芯片：正点原子 MiniSTM32 V4，STM32F103RC，72MHz。

## 二、实验原理

### 2.1 硬件基础：还是那颗内置 USB 全速设备控制器

- 寄存器块 `USB`（`USB_TypeDef`），基址 **0x40005C00**（APB1），时钟使能 `RCC->APB1ENR |= 1 << 23`（`HAL_PCD_MspInit`，[Middlewares/USB/USB_APP/usbd_conf.c](Middlewares/USB/USB_APP/usbd_conf.c)）。
- 数据线复用 **PA11 = USB DM（D-）、PA12 = USB DP（D+）**，切换代码在 `usbd_port_config()`：连接 = `USB->CNTR &= ~(1 << 1)`（PWDN=0，退出掉电，内置收发器生效并带上 D+ 上拉）+ 两脚设为复用推挽；断开 = PWDN=1 + 两脚推挽输出低。main.c 上电先 `usbd_port_config(0)` 延时 500ms 再 `(1)`，人为制造一次"拔插"，保证每次开机都重新枚举。
- 时钟：USB 全速必须 48MHz。本工程主 PLL 输出 72MHz，`RCC->CFGR` 的 USBPRE 位（bit22）保持 0，即 USB 时钟 = 72 ÷ 1.5 = 48MHz，硬件自动完成。
- 中断：`USB_LP_CAN1_RX0_IRQn`（编号 20），`sys_nvic_init(0, 3, USB_LP_CAN1_RX0_IRQn, 2)`，服务函数 `USB_LP_CAN1_RX0_IRQHandler` 调 `HAL_PCD_IRQHandler(&hpcd)` 处理所有 USB 事件。
- 8 个端点、2KB PMA 报文存储；本工程给控制端点 0 和数据/命令端点分配的 PMA 地址（`USBD_LL_Init`）：EP0 OUT=0x40、EP0 IN=0x80、CDC IN=0xC0、CDC OUT=0x110、CDC 命令=0x100。

注意：F103 只有"USB 设备"控制器（手册/头文件里有时标 OTG_FS 系列名，但没有 Host/OTG 功能），所以本实验只能插电脑当从设备，不能插 U 盘当主机。

### 2.2 枚举与 CDC 类：电脑凭什么给你 COM 口

插上后主机依次读取设备描述符 → 配置描述符（含 CDC 类特有的两个"接口"）→ 加载驱动。本工程描述符定义在 [Middlewares/USB/USB_APP/usbd_desc.c](Middlewares/USB/USB_APP/usbd_desc.c)：

| 字段 | 值 | 含义 |
| --- | --- | --- |
| `USBD_VID` / `USBD_PID` | 0x0483 / 0x5740 | ST 官方 VCP 的厂商标识/产品标识，Win10/11 自带驱动可识别；老系统需装原子资料里的 `VCP_V1.4.0_Setup.exe` |
| 设备类字节 | `bDeviceClass = 0x02` | 2 = Communications Device Class（CDC） |
| 产品字符串 | `"STM32 Virtual ComPort in FS Mode"` | 设备管理器/COM 口属性里看到的名词 |

CDC 设备固定三组端点（定义在 `STM32_USB_Device_Library/Class/CDC/Inc/usbd_cdc.h`）：

| 端点 | 地址宏 | 类型 | 用途 |
| --- | --- | --- | --- |
| EP0 | 0x00/0x80 | 控制 | 枚举、SET_LINE_CODING 等类请求 |
| EP1 IN | `CDC_IN_EP = 0x81` | 批量 | 设备→电脑搬数据（本例 TX 缓冲） |
| EP1 OUT | `CDC_OUT_EP = 0x01` | 批量 | 电脑→设备搬数据（本例 RX 缓冲） |
| EP2 IN | `CDC_CMD_EP = 0x82` | 中断（8 字节） | 上报状态（串口的 DTR/CTS 之类"调制解调器信号"） |

全速批量包最大 64 字节（`CDC_DATA_FS_MAX_PACKET_SIZE = 64`）。

### 2.3 为什么"波特率随便设、都不影响通信"

真实 UART 两端必须约好波特率，因为它是**异步**通信，靠比特时序对齐。USB 是**总线**，数据包有自己的时钟与 CRC，两端不需要波特率概念。CDC 规范出于兼容仍保留了一个"线路编码"信箱：主机打开 COM 口时可发 `SET_LINE_CODING` 类请求，把 115200/8/1/无校验 这组参数传给设备（本工程存在 `LineCoding` 结构体里，见 `usbd_cdc_interface.c` 的 `CDC_Itf_Control`，并顺手用物理串口 printf 打印出来）；设备也可以回 `GET_LINE_CODING`。但**这些参数只是存档，不参与任何实际收发节拍**。所以：串口助手上选 9600 还是 921600，USB 虚拟串口照常工作——这是它与 CH340 串口最直观的区别。

### 2.4 接收/发送的软件流水线（本工程的真实实现）

数据流：电脑串口助手 → USB 批量 OUT → 库收包 → 应用缓冲 → 帧状态机 → 业务；反方向同理。对应文件：库层 `STM32_USB_Device_Library/Class/CDC/Src/usbd_cdc.c` + `Core/Src/usbd_core.c`，应用层移植文件 [Middlewares/USB/USB_APP/usbd_cdc_interface.c](Middlewares/USB/USB_APP/usbd_cdc_interface.c)。

缓冲区一共三块，都是 `USB_USART_REC_LEN = 200` 字节（`usbd_cdc_interface.h`）：

| 缓冲区 | 变量 | 角色 |
| --- | --- | --- |
| USB 硬件收包区 | `g_usb_rx_buffer` | 交给 `USBD_CDC_SetRxBuffer`，批量 OUT 的原始落点 |
| 应用帧组装区 | `g_usb_usart_rx_buffer` | 逐字节解析后的"一条消息" |
| 发送暂存区 | `g_usb_usart_printf_buffer` | `usb_printf` 格式化结果，从这里批量 IN 出去 |

帧协议与实验4完全同款，`cdc_vcp_data_rx` 用 `g_usb_usart_rx_sta` 记录：bit15=接收完成，bit14=已收到 `0x0d`（回车），bit13~0=长度；必须以 **`\r\n`（0x0D 0x0A）**结尾才算一包，先 `\r` 后若不是 `\n` 则整包作废重来；超过 199 字节溢出复位。

## 三、硬件资源与接线

| 资源 | 引脚 | 代码中的宏/位置 | 说明 |
| --- | --- | --- | --- |
| USB DM / DP | PA11 / PA12 | `usbd_conf.c`（GPIO 模式设置） | 插板载 **USB_SLAVE** 口，用 USB 数据线；D+ 上拉由片内收发器在 PWDN=0 时提供 |
| USB 中断 | `USB_LP_CAN1_RX0_IRQn`(20) | `HAL_PCD_MspInit` | 组 2，抢占 0 子 3 |
| LED0（红灯） | PA8 | `led.h`，`LED0_TOGGLE()` | 约 300ms 翻转，指示程序运行 |
| LED1（绿灯） | PD2 | `led.h` | `main.c` 检测到连接状态变化时 `LED1(0)` 点亮，断开灭 |
| 串口1（仅作调试打印） | PA9/PA10 → CH340 | `Drivers/SYSTEM/usart/`，`usart_init(72, 115200)` | 打印枚举复位信息、`SET_LINE_CODING` 参数 |
| LCD（MCU 屏） | 数据 PB0~PB15；RD=PC6，WR=PC7，RS=PC8，CS=PC9，BL=PC10 | `Drivers/BSP/LCD/lcd.h` | 显示 `USB Connected/Disconnected` |
| 时钟 | 72MHz，USB=48MHz | `sys_stm32_clock_init(9)` | USBPRE=0 → PLL÷1.5 |

接线/操作：USB 数据线接电脑与 **USB_SLAVE**；如需看调试打印，USART1 排针 PA9/PA10 用跳线帽接 RXD/TXD，CH340 的 USB_UART 口另插一台电脑或同一台电脑第二个口。

## 四、代码结构与调用流程

```text
实验37 USB虚拟串口实验/
├── Drivers/
│   ├── BSP/LCD|LED/                # 显示与指示灯
│   └── SYSTEM/delay|sys|usart/     # 72MHz时钟、NVIC/GPIO、物理串口(调试printf)
├── Middlewares/
│   ├── MALLOC/malloc.c|h           # 内存池(工程保留, 本例仅初始化SRAMIN池)
│   ├── USMART/                     # 本例未启用
│   └── USB/
│       ├── USB_APP/
│       │   ├── usbd_conf.c|h       # 移植层: 时钟/中断/PMA/usbd_port_config
│       │   ├── usbd_desc.c|h       # VCP 描述符(VID 0x0483, PID 0x5740)
│       │   └── usbd_cdc_interface.c|h  # 本工程核心: CDC 收发与帧状态机
│       ├── USB_HAL/                # ST 的 PCD + LL USB 驱动
│       └── STM32_USB_Device_Library/  # Core + Class/CDC(usbd_cdc.c/h)
├── User/main.c
└── Projects/MDK-ARM/atk_f103.uvprojx
```

`main()` 流程（[User/main.c](User/main.c)）：

1. 初始化：`sys_stm32_clock_init(9)` → `delay_init(72)` → `usart_init(72, 115200)` → `led_init` → `lcd_init` → `my_mem_init(SRAMIN)`；LCD 打 `USB Virtual USART TEST` 和 `USB Connecting...`。
2. USB 先断后接（`usbd_port_config(0)` → 500ms → `usbd_port_config(1)`）。
3. 启动设备栈：

```c
USBD_Init(&USBD_Device, &VCP_Desc, 0);
USBD_RegisterClass(&USBD_Device, USBD_CDC_CLASS);
USBD_CDC_RegisterInterface(&USBD_Device, &USBD_CDC_fops);
USBD_Start(&USBD_Device);
```

4. 主循环：
   - `g_device_state` 变化 → 更新 LCD 文本并 `LED1` 亮/灭；
   - `g_usb_usart_rx_sta` bit15 置位（收到完整一行）→ 取长度 `& 0x3FFF`，回显 `您发送的消息长度为:%d`，再 `cdc_vcp_data_tx` 原样回发数据（回环测试），清状态；
   - 空闲时周期性 `usb_printf` 发提示（约每 2 秒"请输入数据,以回车键结束"，约每 50 秒实验名），每 300ms 翻转 LED0。

## 五、核心代码解析

### 5.1 回调注册：库与业务之间的插座（usbd_cdc_interface.c）

```c
USBD_CDC_ItfTypeDef USBD_CDC_fops =
{
    CDC_Itf_Init,
    CDC_Itf_DeInit,
    CDC_Itf_Control,
    CDC_Itf_Receive
};
```

CDC 类驱动（`usbd_cdc.c`）不碰业务数据，所有事情都通过这四个回调做：`CDC_Itf_Init` 里一句 `USBD_CDC_SetRxBuffer(&USBD_Device, g_usb_rx_buffer)` 指定收包落点；`CDC_Itf_Receive` 收到一包就重新挂接收并转交解析：

```c
static int8_t CDC_Itf_Receive(uint8_t *buf, uint32_t *len)
{
    USBD_CDC_ReceivePacket(&USBD_Device);   /* 继续接收下一包 */
    cdc_vcp_data_rx(buf, *len);
    return USBD_OK;
}
```

### 5.2 发送路径：cdc_vcp_data_tx 与 usb_printf

```c
void cdc_vcp_data_tx(uint8_t *data, uint32_t Len)
{
    USBD_CDC_SetTxBuffer(&USBD_Device, data, Len);
    USBD_CDC_TransmitPacket(&USBD_Device);
    delay_ms(CDC_POLLING_INTERVAL);   /* 轮询周期, 宏值=1ms */
}
```

发送 = "挂缓冲区 → 触发批量 IN 传输 → 等 1ms 让库轮询发完"。`CDC_POLLING_INTERVAL` 注释写明最大 65、最小 1，本工程取 1。`usb_printf` 是它的格式化封装：`vsprintf` 进 `g_usb_usart_printf_buffer`（**单次输出别超过 200 字节**，缓冲区只有 `USB_USART_REC_LEN` 大小），再 `strlen` 求长发送。

### 5.3 接收帧状态机（复刻串口实验，逐字节）

```c
if ((g_usb_usart_rx_sta & 0x8000) == 0)     /* 接收未完成 */
{
    if (g_usb_usart_rx_sta & 0x4000)        /* 接收到了0x0d */
    {
        if (res != 0x0a)
            g_usb_usart_rx_sta = 0;         /* 接收错误,重新开始 */
        else
            g_usb_usart_rx_sta |= 0x8000;   /* 接收完成了 */
    }
    else
    {
        if (res == 0x0d)
            g_usb_usart_rx_sta |= 0x4000;
        else
        {
            g_usb_usart_rx_buffer[g_usb_usart_rx_sta & 0X3FFF] = res;
            g_usb_usart_rx_sta++;
            if (g_usb_usart_rx_sta > (USB_USART_REC_LEN - 1))
                g_usb_usart_rx_sta = 0;     /* 接收数据溢出 重新开始接收 */
        }
    }
}
```

一个细节：`cdc_vcp_data_rx` 遍历的是"一整包"（一次批量 IN/OUT 最多 64 字节，可能含多个字符），而 UART 一次中断一个字节——逻辑结构完全一致，说明帧格式与物理介质无关。

### 5.4 线路编码请求（为什么波特率无关的证据）

```c
case CDC_SET_LINE_CODING:
    LineCoding.bitrate = (uint32_t) (pbuf[0] | (pbuf[1] << 8) |
                                     (pbuf[2] << 16) | (pbuf[3] << 24));
    ...
    printf("linecoding.bitrate:%d\r\n", LineCoding.bitrate);
```

电脑串口助手一改波特率，驱动就发这条请求；固件只是**存进结构体并用物理串口打印出来**，收发路径没有任何一处用到 `LineCoding`——这就是"波特率无关"的代码级证明。结构体默认值 `{115200, 0x00, 0x00, 0x08}` 仅是给 GET_LINE_CODING 的应答模板。

## 六、编译、下载与运行

1. Keil 打开 [Projects/MDK-ARM/atk_f103.uvprojx](Projects/MDK-ARM/atk_f103.uvprojx)（器件 STM32F103RC）。
2. ST-Link/J-Link 下载 `Output/atk_f103.hex`。
3. 驱动：按官方 readme.txt 要求，先安装光盘 `6，软件资料\1，软件\STM32 USB虚拟串口驱动\VCP_V1.4.0_Setup.exe`；装不上（设备管理器带惊叹号）时手动指定驱动路径，如 Win7 的 `C:\Program Files (x86)\STMicroelectronics\Software\Virtual comport driver\Win7`。部分联网的 Win10/11 会通过 Windows Update 自动匹配 ST 的 VCP 驱动，但仍以安装官方驱动最稳妥。
4. 上位机：任意串口助手打开新生成的 COM 口（波特率任意）；工程默认推荐 XCOM/SSCOM。官方 readme.txt 里"波特率必须 115200 / 请用USB线连接在USB_UART"两条是串口实验模板的残留说明——本实验真正的数据通道是 USB_SLAVE 口的虚拟 COM 口，115200 只是物理调试串口的设置。

## 七、实验现象

1. 上电 LCD 显示 `STM32 / USB Virtual USART TEST / ATOM@ALIENTEK / USB Connecting...`，LED0 约 300ms 闪烁。
2. USB 线插入 USB_SLAVE，电脑数秒内枚举完成（物理串口会打印 `USB Device Library [FS]`），LCD 变 `USB Connected`，LED1 常亮。
3. 打开串口助手对应 COM 口，设备端立刻收到一串提示（`STM32F103开发板USB虚拟串口实验 / 正点原子@ALIENTEK`、`请输入数据,以回车键结束`）；在助手输入 `hello` 并以回车换行结尾发送，板子回显：

```text
您发送的消息长度为:5

hello
```

4. 拔掉 USB 线，LCD 变 `USB disConnected`，LED1 灭。
5. 若用串口助手抓 `SET_LINE_CODING`：改波特率时物理调试串口会打印新的 bitrate/format/parity/datatype，但虚拟串口通信不受影响。

## 八、注意事项

1. 驱动是最大门槛：Win7 等老系统必须装 VCP 驱动；有惊叹号时按 readme 指的路径手动选驱动。设备管理器出现 `STM32 Virtual ComPort` 且无感叹号才算成功。
2. 必须数据线 + USB_SLAVE 口；插 USB_UART 口只会出现 CH340 串口，永远不会出现虚拟 COM 口。
3. 发送内容**必须以回车换行结束**（帧尾判 0x0D 0x0A），且一条消息 ≤199 字符，超长溢出后整包丢弃重来；中途只发 `\r` 不发 `\n` 也会重启状态机。
4. `usb_printf` 一次发送 ≤200 字节（`g_usb_usart_printf_buffer` 大小），且发送内部固定 `delay_ms(1)` 等库轮询，别把它放进更高频率的中断里用。
5. `CDC_Itf_Control` 里的调试 `printf` 走的是**物理 USART1**，不是虚拟串口——别在"为什么电脑没收到 linecoding 打印"上纠结，插 CH340 看即可。
6. 官方 readme.txt 与代码的出入：注意事项 1/4 条（波特率必须匹配、连 USB_UART）是模板残留，实际以第 6、7 条（插 USB_SLAVE、装驱动）为准；本工程 `main.c` 也保留了 `usart_init`/USMART 头文件包含，但 USMART 调试指令在本实验未初始化使用。
7. 本实验仅支持 MCU 屏（官方 readme.txt）。
8. 中文字符串（如"您发送的消息长度为"）在源码里是 GBK 字节：串口助手接收编码要选 GBK/ANSI，选 UTF-8 会显示乱码，并非通信错误。
9. 枚举异常排查顺序：换数据线 → 换 USB 口（别走 HUB）→ 复位开发板（固件自带的先断再接已尽量降低概率）→ 看物理串口是否打印 `USB Device Library [FS]`。

## 九、总结与延伸

要点：

1. CDC 类让 MCU 用一组描述符+端点（0x81/0x01 批量数据、0x82 中断命令）"扮演"串口，电脑以 COM 口形式使用它。
2. 虚拟串口的波特率只是兼容信息（SET/GET_LINE_CODING 存档），真正搬运靠 USB 批量传输，因此两端设置互不影响。
3. 工程分层清晰：`usbd_conf.c`（片上资源/中断/PMA/软件插拔）→ `usbd_desc.c`（自报家门）→ `usbd_cdc_interface.c`（缓冲区+回调+帧协议）→ `main.c`（业务）。
4. 帧状态机与实验4完全同构，印证"协议与物理层解耦"的思想。
5. 收发缓冲各司其职：`g_usb_rx_buffer`（库收包）→ `g_usb_usart_rx_buffer`（组帧）→ `g_usb_usart_printf_buffer`（发送），均为 200 字节。

延伸练习：

1. 把 `USB_USART_REC_LEN` 改大到 1000，观察一次发 500 字符是否仍按 `\r\n` 正确收包。
2. 在收到完整行后，不复读原文，而是把每个字母 `+1` 再发回（凯撒密码），练应用层改写。
3. 对照 `CDC_Itf_Control` 的 `SET_LINE_CODING` 分支，把波特率参数用 `lcd_show_num` 显示到 LCD 上，直观看到电脑端设置的变化。

衔接：实验36（读卡器，MSC 类）与本实验是同一 USB 框架的两种"人设"；理解了这里的中断/PMA/端点，再回看 USB 读卡器的 BOT/SCSI 会顺很多。USB 方向学完后，可进入实验38~40（UCOSII）用操作系统重新组织这些外设任务。
