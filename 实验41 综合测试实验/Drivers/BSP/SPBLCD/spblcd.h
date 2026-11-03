/**
 ****************************************************************************************************
 * @file        spblcd.h
 * @author      正点原子团队(ALIENTEK)
 * @version     V1.0
 * @date        2022-10-26
 * @brief       SPB效果实现 驱动代码
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
 * V1.0 20221026
 * 第一次发布
 *
 ****************************************************************************************************
 */

#ifndef __SPBLCD_H
#define	__SPBLCD_H

#include "./BSP/LCD/lcd.h"
#include "./SYSTEM/delay/delay.h"

#include "./SYSTEM/sys/sys.h"



#define SPILCD_BASE         1024*1024*7.3  /**
                                            * SPB界面,从SPI FLASH的第7.4M字节开始存储,占用最大为571.875KB字节

* SPILCD_BASE，存放液晶分辨率标志：
                                            * 0，表示240*320的屏；1，表示320*480的屏；2，表示480*800的屏；其他值，非法
                                            * 对于320*240的屏,最大占用   224*600*2≈262K字节
                                            * 对于480*320的屏,最大占用   364*800*2≈568K字节
                                            * 对于800*480的屏,最大占用   610*480*2≈571K字节
                                            */

#define SPILCD_END_ADDR     SPILCD_BASE + 580*1024  /* SPB界面数据结束位置,用于存放lcd类型,必须大于滑屏存储数据（571.8KB） */

 
 
uint8_t slcd_frame_lcd2spi(uint32_t offset,uint16_t width,uint16_t height);


void slcd_spi2_mode(uint8_t mode);
void slcd_dma_init(void);
void slcd_dma_enable(uint32_t x);
void slcd_frame_show(uint16_t x);

uint16_t slcd_get_data_remain(void);


void slcd_test_data_init
    

(uint16_t color);


#endif

























