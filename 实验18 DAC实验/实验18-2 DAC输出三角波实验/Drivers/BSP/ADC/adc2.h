/**
 ****************************************************************************************************
 * @file        adc2.h
 * @author      正点原子团队(ALIENTEK)
 * @version     V1.0
 * @date        2020-05-31
 * @brief       ADC2 驱动代码
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
 * 修改说明
 * V1.0 20200531
 * 第一次发布
 ****************************************************************************************************
 */

#ifndef __ADC2_H
#define __ADC2_H

#include "./SYSTEM/sys/sys.h"


/******************************************************************************************/
/* ADC2及引脚 定义 */

#define ADC2_CHY_GPIO_PORT              GPIOC
#define ADC2_CHY_GPIO_PIN               SYS_GPIO_PIN4
#define ADC2_CHY_GPIO_CLK_ENABLE()      do{ RCC->APB2ENR |= 1 << 4; }while(0)   /* PC口时钟使能 */

#define ADC2_CHY                        14                                      /* 通道Y,  0 <= Y <= 17 */ 

/******************************************************************************************/

void adc2_init(void);                               /* ADC3初始化 */
void adc2_channel_set(uint8_t ch, uint8_t stime);   /* ADC3通道设置 */
uint32_t adc2_get_result(uint8_t ch);               /* 获得某个通道值  */
uint32_t adc2_get_result_average(uint8_t ch, uint8_t times);/* 得到某个通道给定次数采样的平均值 */

#endif 















