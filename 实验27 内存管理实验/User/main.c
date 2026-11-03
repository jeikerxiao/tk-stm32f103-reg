/**
 ****************************************************************************************************
 * @file        main.c
 * @author      正点原子团队(ALIENTEK)
 * @version     V1.0
 * @date        2020-06-01
 * @brief       内存管理 实验
 * @license     Copyright (c) 2020-2032, 广州市星翼电子科技有限公司
 ****************************************************************************************************
 * @attention
 *
 * 实验平台:正点原子 STM32F103开发板
 * 在线视频:www.yuanzige.com
 * 技术论坛:www.openedv.com
 * 公司网址:www.alientek.com
 * 购买地址:openedv.taobao.com
 *
 ****************************************************************************************************
 */

#include "./SYSTEM/sys/sys.h"
#include "./SYSTEM/usart/usart.h"
#include "./SYSTEM/delay/delay.h"
#include "./USMART/usmart.h"
#include "./BSP/LED/led.h"
#include "./BSP/LCD/lcd.h"
#include "./BSP/KEY/key.h"
#include "./MALLOC/malloc.h"


int main(void)
{
    uint8_t paddr[20];  /* 存放P Addr:+p地址的ASCII值 */
    uint16_t memused = 0;
    uint8_t key;
    uint8_t i = 0;
    uint8_t *p = 0;
    uint8_t *tp = 0;

    sys_stm32_clock_init(9);    /* 设置时钟, 72Mhz */
    delay_init(72);             /* 延时初始化 */
    usart_init(72, 115200);     /* 串口初始化为115200 */
    usmart_dev.init(72);        /* 初始化USMART */
    led_init();                 /* 初始化LED */
    lcd_init();                 /* 初始化LCD */
    key_init();                 /* 初始化按键 */
    my_mem_init(SRAMIN);        /* 初始化内部SRAM内存池 */

    lcd_show_string(30, 50, 200, 16, 16, "STM32", RED);
    lcd_show_string(30, 70, 200, 16, 16, "MALLOC TEST", RED);
    lcd_show_string(30, 90, 200, 16, 16, "ATOM@ALIENTEK", RED);
    lcd_show_string(30, 110, 200, 16, 16, "KEY0: Malloc", RED);
    lcd_show_string(30, 130, 200, 16, 16, "KEY1: Write Data", RED);
    lcd_show_string(30, 150, 200, 16, 16, "KEY_UP: Free", RED);
    lcd_show_string(30, 170, 200, 16, 16, "SRAMIN   USED:", BLUE);

    while (1)
    {
        key = key_scan(0);      /* 不支持连按 */

        switch (key)
        {
            case KEY0_PRES:     /* KEY0按下 */
                p = mymalloc(SRAMIN, 2048); /* 申请2K字节,并写入内容,显示在lcd屏幕上面 */

                if (p != NULL)
                {
                    sprintf((char *)p, "Memory Malloc Test%03d", i);        /* 向p写入一些内容 */
                    lcd_show_string(30, 230, 209, 16, 16, (char *)p, BLUE); /* 显示P的内容 */
                }

                break;

            case KEY1_PRES:     /* KEY1按下 */
                if (p != NULL)
                {
                    sprintf((char *)p, "Memory Malloc Test%03d", i);        /* 更新显示内容 */
                    lcd_show_string(30, 230, 209, 16, 16, (char *)p, BLUE); /* 显示P的内容 */
                }

                break;

            case WKUP_PRES:         /* KEY UP按下 */
                myfree(SRAMIN, p);  /* 释放内存 */
                p = 0;              /* 指向空地址 */
                break;
        }

        if (tp != p )
        {
            tp = p;
            sprintf((char *)paddr, "P Addr:0X%08X", (uint32_t)tp);
            lcd_show_string(30, 210, 209, 16, 16, (char *)paddr, BLUE); /* 显示p的地址 */

            if (p)
            {
                lcd_show_string(30, 230, 280, 16, 16, (char *)p, BLUE); /* 显示P的内容 */
            }
            else
            {
                lcd_fill(30, 230, 209, 296, WHITE);                     /* p=0,清除显示 */
            }
        }

        delay_ms(10);
        i++;

        if ((i % 20) == 0)
        {
            memused = my_mem_perused(SRAMIN);                                   /* 获取内存使用率 */
            sprintf((char *)paddr, "%d.%01d%%", memused / 10, memused % 10);
            lcd_show_string(30 + 112, 170, 200, 16, 16, (char *)paddr, BLUE);   /* 显示内部内存使用率 */

            LED0_TOGGLE();                                                      /* LED0闪烁 */
        }
    }
}




















