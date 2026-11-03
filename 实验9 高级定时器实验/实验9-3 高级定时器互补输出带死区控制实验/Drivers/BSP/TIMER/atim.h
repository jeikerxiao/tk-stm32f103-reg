/**
 ****************************************************************************************************
 * @file        atim.h
 * @author      正点原子团队(ALIENTEK)
 * @version     V1.2
 * @date        2020-05-30
 * @brief       高级定时器 驱动代码
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
 * V1.0 20200530
 * 第一次发布
 * V1.1 20200530
 * 1, 新增atim_timx_comp_pwm_init函数, 实现输出比较模式PWM输出功能
 * V1.2 20200530
 * 1, 增加atim_timx_cplm_pwm_init函数
 * 2, 增加atim_timx_cplm_pwm_set函数
 *
 ****************************************************************************************************
 */

#ifndef __ATIM_H
#define __ATIM_H

#include "./SYSTEM/sys/sys.h"

/******************************************************************************************/
/* 高级定时器 定义 */

 /* TIMX 输出指定个数PWM 定义 
 * 这里输出的PWM通过PA8(TIM1_CH1)输出, 可以 看到TIM1_CH1控制LED0的亮灭, 亮灭一次表示一个PWM波
 * 默认使用的是TIM1_CH1.
 * 注意: 通过修改这几个宏定义, 可以支持TIM1/TIM8定时器, 任意一个IO口输出指定个数的PWM
 */
#define ATIM_TIMX_NPWM_CHY_GPIO_PORT            GPIOA
#define ATIM_TIMX_NPWM_CHY_GPIO_PIN             SYS_GPIO_PIN8
#define ATIM_TIMX_NPWM_CHY_GPIO_CLK_ENABLE()    do{ RCC->APB2ENR |= 1 << 2; }while(0)   /* PA口时钟使能 */

#define ATIM_TIMX_NPWM                          TIM1
#define ATIM_TIMX_NPWM_IRQn                     TIM1_UP_IRQn
#define ATIM_TIMX_NPWM_IRQHandler               TIM1_UP_IRQHandler
#define ATIM_TIMX_NPWM_CHY                      1                           /* 通道Y,  1<= Y <=4 */
#define ATIM_TIMX_NPWM_CHY_CCRX                 ATIM_TIMX_NPWM->CCR1        /* 通道Y的输出比较寄存器 */
#define ATIM_TIMX_NPWM_CHY_CLK_ENABLE()         do{ RCC->APB2ENR |= 1 << 11; }while(0)  /* TIM1 时钟使能 */


/* TIMX 输出比较模式 定义 
 * 这里通过TIM8的输出比较模式,控制PC6,PC7,PC8,PC9输出4路PWM,占空比50%,并且每一路PWM之间的相位差为25%
 * 修改CCRx可以修改相位.
 * 默认是针对TIM1
 * 注意: 通过修改这些宏定义,可以支持TIM1/TIM8任意一个定时器,任意一个IO口使用输出比较模式,输出PWM
 */
#define ATIM_TIMX_COMP_CH1_GPIO_PORT            GPIOC
#define ATIM_TIMX_COMP_CH1_GPIO_PIN             SYS_GPIO_PIN6
#define ATIM_TIMX_COMP_CH1_GPIO_CLK_ENABLE()    do{ RCC->APB2ENR |= 1 << 4; }while(0)   /* PC口时钟使能 */

#define ATIM_TIMX_COMP_CH2_GPIO_PORT            GPIOC
#define ATIM_TIMX_COMP_CH2_GPIO_PIN             SYS_GPIO_PIN7
#define ATIM_TIMX_COMP_CH2_GPIO_CLK_ENABLE()    do{ RCC->APB2ENR |= 1 << 4; }while(0)   /* PC口时钟使能 */

#define ATIM_TIMX_COMP_CH3_GPIO_PORT            GPIOC
#define ATIM_TIMX_COMP_CH3_GPIO_PIN             SYS_GPIO_PIN8
#define ATIM_TIMX_COMP_CH3_GPIO_CLK_ENABLE()    do{ RCC->APB2ENR |= 1 << 4; }while(0)   /* PC口时钟使能 */

#define ATIM_TIMX_COMP_CH4_GPIO_PORT            GPIOC
#define ATIM_TIMX_COMP_CH4_GPIO_PIN             SYS_GPIO_PIN9
#define ATIM_TIMX_COMP_CH4_GPIO_CLK_ENABLE()    do{ RCC->APB2ENR |= 1 << 4; }while(0)   /* PC口时钟使能 */

#define ATIM_TIMX_COMP                          TIM8            
#define ATIM_TIMX_COMP_CH1_CCRX                 ATIM_TIMX_COMP->CCR1        /* 通道1的输出比较寄存器 */
#define ATIM_TIMX_COMP_CH2_CCRX                 ATIM_TIMX_COMP->CCR2        /* 通道2的输出比较寄存器 */
#define ATIM_TIMX_COMP_CH3_CCRX                 ATIM_TIMX_COMP->CCR3        /* 通道3的输出比较寄存器 */
#define ATIM_TIMX_COMP_CH4_CCRX                 ATIM_TIMX_COMP->CCR4        /* 通道4的输出比较寄存器 */
#define ATIM_TIMX_COMP_CLK_ENABLE()             do{ RCC->APB2ENR |= 1 << 13; }while(0)  /* TIM8 时钟使能 */


/* TIMX 互补输出模式 定义 
 * 这里设置互补输出相关硬件配置, CHY即正常输出, CHYN即互补输出
 * 修改CCRx可以修改占空比.
 * 默认是针对TIM1, CH1(PA8) & CH1N(PB13)
 * 注意: 通过修改这些宏定义,可以支持TIM1/TIM8定时器, 任意一个IO口输出互补PWM(前提是必须有互补输出功能)
 */

/* 输出通道引脚 */
#define ATIM_TIMX_CPLM_CHY_GPIO_PORT            GPIOA
#define ATIM_TIMX_CPLM_CHY_GPIO_PIN             SYS_GPIO_PIN8
#define ATIM_TIMX_CPLM_CHY_GPIO_CLK_ENABLE()    do{ RCC->APB2ENR |= 1 << 2; }while(0)   /* PA口时钟使能 */

/* 互补输出通道引脚 */
#define ATIM_TIMX_CPLM_CHYN_GPIO_PORT           GPIOB
#define ATIM_TIMX_CPLM_CHYN_GPIO_PIN            SYS_GPIO_PIN13
#define ATIM_TIMX_CPLM_CHYN_GPIO_CLK_ENABLE()   do{ RCC->APB2ENR |= 1 << 3; }while(0)   /* PB口时钟使能 */

/* 刹车输入引脚 */
#define ATIM_TIMX_CPLM_BKIN_GPIO_PORT           GPIOB
#define ATIM_TIMX_CPLM_BKIN_GPIO_PIN            SYS_GPIO_PIN12
#define ATIM_TIMX_CPLM_BKIN_GPIO_CLK_ENABLE()   do{ RCC->APB2ENR |= 1 << 3; }while(0)   /* PB口时钟使能 */

/* 互补输出使用的定时器 */
#define ATIM_TIMX_CPLM                          TIM1            
#define ATIM_TIMX_CPLM_CHY                      1            
#define ATIM_TIMX_CPLM_CHY_CCRY                 ATIM_TIMX_CPLM->CCR1        
#define ATIM_TIMX_CPLM_CLK_ENABLE()             do{ RCC->APB2ENR |= 1 << 11; }while(0)  /* TIM1 时钟使能 */

/******************************************************************************************/

void atim_timx_pwm_chy_init(uint16_t arr, uint16_t psc);    /* 高级定时器 PWM初始化函数 */
void atim_timx_npwm_chy_init(uint16_t arr, uint16_t psc);   /* 高级定时器 输出指定个数PWM初始化函数 */
void atim_timx_npwm_chy_set(uint32_t npwm);                 /* 高级定时器 设置输出PWM的个数 */
void atim_timx_comp_pwm_init(uint16_t arr, uint16_t psc);   /* 高级定时器 输出比较模式输出PWM 初始化函数 */
void atim_timx_cplm_pwm_init(uint16_t arr, uint16_t psc);   /* 高级定时器 互补输出 初始化函数 */
void atim_timx_cplm_pwm_set(uint16_t ccr, uint8_t dtg);     /* 高级定时器 互补输出 设置输出比较值 & 死区时间 */

#endif






















