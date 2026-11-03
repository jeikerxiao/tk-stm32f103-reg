/**
 ****************************************************************************************************
 * @file        spblcd.c
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

#include "./BSP/SPBLCD/spblcd.h"
#include "./MALLOC/malloc.h"
#include "./BSP/DMA/dma.h"
#include "./BSP/NORFLASH/norflash.h"
#include "./BSP/SPI/spi.h"
#include "spb.h"
#include "ucos_ii.h"


/**
 * @brief       从lcd拷贝到spi flash
 * @param       offset  : 存储地址偏移量
 * @param       width   : 宽度
 * @param       height  : 高度
 * @retval      0,成功; 其他,错误代码;
 */
uint8_t slcd_frame_lcd2spi(uint32_t offset, uint16_t width, uint16_t height)
{
    uint16_t *pbuf;
    uint32_t startx;        /* 起始地址 */
    uint16_t i, j, k;
    uint32_t woffset = 0;   /* 写地址偏移 */
    
    uint8_t *p;
    uint8_t temp;

    startx = SPILCD_BASE + offset * 2;  /* 因为offset是以像素为单位的,而写入是以字节为单位的，所以得*2 */
    pbuf = mymalloc(SRAMIN, 4096);      /* 申请内存 */

    if (!pbuf)return 1;

    woffset = 0;
    k = 0;

    for (i = 0; i < width; i++)
    {
        for (j = 0; j < height; j++)
        {
            pbuf[k++] = lcd_read_point(i, j);    /* 读取点 */

            if (k == 2048)
            {
                p = (uint8_t *)pbuf;

                for (k = 0; k < 2048; k++)      /* 调转高低字节 */
                {
                    temp = p[2 * k];
                    p[2 * k] = p[2 * k + 1];
                    p[2 * k + 1] = temp;
                }
                
                norflash_write((uint8_t *)pbuf, startx + woffset, 4096); /* 写入一个扇区 */
                woffset += 4096;
                k = 0;
            }
        }
    }

    if (k)  /* 还有一点数据要写入 */
    {
        p = (uint8_t *)pbuf;

        for (i = 0; i < k; i++) /* 调转高低字节 */
        {
            temp = p[2 * i];
            p[2 * i] = p[2 * i + 1];
            p[2 * i + 1] = temp;
        }

        norflash_write((uint8_t *)pbuf, startx + woffset, k * 2);    /* 写入最后一次数据 */
    }

    myfree(SRAMIN, pbuf);   /* 释放内存 */
    return 0;
}

/**
 * @brief       SPI2模式设置
 * @param       mode    : 0, 8位普通模式; 1, 16位DMA模式;
 * @param       无
 * @retval      无
 */
void slcd_spi2_mode(uint8_t mode)
{
    uint16_t tempreg = 0;

    if (mode == 0)  /* 8位普通模式,重新配置SPI */
    {
        RCC->APB2RSTR |= 1 << 12;   /* 复位SPI1 */
        RCC->APB2RSTR &= ~(1 << 12);/* 停止复位SPI1 */
        tempreg |= 0 << 10;         /* 全双工模式 */
        tempreg |= 1 << 9;          /* 软件nss管理 */
        tempreg |= 1 << 8;
        tempreg |= 1 << 2;          /* SPI主机 */
        tempreg |= 0 << 11;         /* 8位数据格式 */
        tempreg |= 1 << 1;          /* 空闲模式下SCK为1 CPOL=1 */
        tempreg |= 1 << 0;          /* 数据采样从第2个时间边沿开始,CPHA=1 */
        tempreg |= 0 << 3;          /* 2分频,设置为36Mhz频率 */
        tempreg |= 0 << 7;          /* MSB First */
        tempreg |= 1 << 6;          /* SPI启动 */
        SPI1->CR1 = tempreg;        /* 设置CR1 */
        SPI1->I2SCFGR &= ~(1 << 11);/* 选择SPI模式 */
    }
    else    /* 16位DMA模式 */
    {
        SPI1->CR1 &= ~(1 << 6);     /* 关闭SPI先 */
        SPI1->CR1 |= 1 << 10;       /* 仅接收模式 */
        SPI1->CR1 |= 1 << 11;       /* 16bit数据格式 */
    }
}

/**
 * @brief       QSPI --> LCD_RAM dma配置
 *  @note       PAR和NDTR在后面使能的时候再设置
 * @param       无
 * @retval      无
 */
void slcd_dma_init(void)
{
    RCC->AHBENR |= 1 << 1;  /* 开启DMA2时钟 */
    delay_ms(5);            /* 等待DMA时钟稳定 */
    DMA2_Channel5->CPAR = (uint32_t)&SPI1->DR;      /* 外设地址为:SPI1->DR */
    DMA2_Channel5->CMAR = (uint32_t)&GPIOB->ODR;    /* 目标地址为GPIOB_ODR */
    DMA2_Channel5->CNDTR = 1;           /* DMA2,传输数据量为1 */
    DMA2_Channel5->CCR = 0X00000000;    /* 复位 */
    DMA2_Channel5->CCR |= 0 << 4;       /* 从外设读 */
    DMA2_Channel5->CCR |= 1 << 5;       /* 循环模式 */
    DMA2_Channel5->CCR |= 0 << 6;       /* 外设地址非增量模式 */
    DMA2_Channel5->CCR |= 0 << 7;       /* 存储器非增量模式 */
    DMA2_Channel5->CCR |= 1 << 8;       /* 外设数据宽度为16位 */
    DMA2_Channel5->CCR |= 1 << 10;      /* 存储器数据宽度16位 */
    DMA2_Channel5->CCR |= 1 << 12;      /* 中等优先级 */
    DMA2_Channel5->CCR |= 0 << 14;      /* 非存储器到存储器模式 */
    
    RCC->APB2ENR |= 1 << 4;             /* GPIOC 时钟使能 */
    RCC->APB2ENR |= 1 << 13;            /* TIM8 时钟使能 */

    TIM8->ARR = 16 - 1;                 /* 设定计数器自动重装值 15， 每16个定时器时钟一个周期 */
    TIM8->CCR2 = 16 / 2;                /* 50%占空比 */
    TIM8->PSC = 2 - 1;                  /* 设置预分频器4, 72 / 4 = 18Mhz  */
    TIM8->BDTR |= 1 << 15;              /* 使能MOE位, 高级定时器必须设置MOE才能输出PWM */

    TIM8->CCMR1 |= 6 << 12;             /* CH2 PWM模式1 */
    TIM8->CCMR1 |= 0 << 11;             /* CH2 预装载不使能 */
   

    TIM8->CCER |= 1 << 4;               /* OC2 输出使能 */
    TIM8->CCER |= 0 << 5;               /* OC2 高电平有效 */

    TIM8->CR1 |= 0 << 7;                /* ARPE不使能 */
    TIM8->CR1 |= 1 << 2;                /* URS使能 */
    TIM8->CR2 |= 0 << 3;                /* 发生CCx事件,送出DMA请求 */
    TIM8->DIER |= 1 << 10;              /* 允许CC2的DMA请求 */
    TIM8->DIER |= 0 << 0;               /* 关闭更新中断 */

    //sys_nvic_init(0, 0, TIM8_UP_IRQn, 2);   /* 设置为最高优先级中断， 抢占0，子优先级0，组2 */
}
 
/**
 * @brief       设置LCD WR脚的GPIO模式
 * @param       mode    : 0, 输出模式
 *                        1, 复用功能模式
 * @retval      无
 */
void slcd_gpio_set(uint8_t mode)
{
    if(mode)
    {
        sys_gpio_set(GPIOC, SYS_GPIO_PIN7,
                     SYS_GPIO_MODE_AF, SYS_GPIO_OTYPE_PP, SYS_GPIO_SPEED_HIGH, SYS_GPIO_PUPD_PU);       /* PC7 引脚复用功能模式 */
    }else
    {
        sys_gpio_set(GPIOC, SYS_GPIO_PIN7,
                     SYS_GPIO_MODE_OUT, SYS_GPIO_OTYPE_PP, SYS_GPIO_SPEED_HIGH, SYS_GPIO_PUPD_PU);      /* PC7 引脚输出模式 */
    }
}

/* g_spblcd_ndata_remain表示当前还剩下多少个数据要发送
 * 每次最多发送256个数据
 */
static uint32_t g_spblcd_ndata_remain = 0;
static uint8_t g_rcr_cnt = 0;                   /* RCR启用次数计数器 */

/**
 * @brief       获取剩余数据长度(最大256字节,最小0,0表示没数据要发送了)
 * @param       无
 * @retval      0-256, 待发送的数据长度(次数)
 */
uint16_t slcd_get_data_remain(void)
{
    uint16_t datalen = 0;

    if(g_spblcd_ndata_remain >= 256)
    {
        datalen = 256;
        g_rcr_cnt++;                        /* RCR计数器自增 */
    }else if(g_spblcd_ndata_remain)
    {
        datalen = g_spblcd_ndata_remain;    /* 最后一批数据 */
        g_rcr_cnt++;                        /* RCR计数器自增 */
    }
    
    g_spblcd_ndata_remain -= datalen;
    
    return datalen;
}

/**
 * @brief       开启一次SPI到LCD的DMA的传输
 * @param       x       : 起始传输地址编号
 * @retval      无
 */
void slcd_dma_enable(uint32_t x)
{
    uint32_t lcdsize = spbdev.spbwidth * spbdev.spbheight;    
    uint32_t addr = SPILCD_BASE + (x * spbdev.spbheight) * 2;
    
    uint16_t ndata = 0;

    LCD_RS(1);                          /* RS=1, 表示写数据 */
    LCD_CS(0);                          /* CS=1, 选中LCD */
    slcd_gpio_set(1);                   /* 设置WR脚为复用功能 */

    
    g_spblcd_ndata_remain = lcdsize - 1;/* 每次都是传输lcdsize这么多数据 */
    ndata = slcd_get_data_remain();     /* 可以传输多少数据? */
    
    TIM8->DIER |= 1 << 10;              /* 打开CC2的DMA请求 */
    TIM8->RCR = ndata - 1;              /* 设置发送ndata个数据 */
    TIM8->EGR |= 1 << 0;                /* 产生一次更新事件,以更新RCR寄存器 */

    NORFLASH_CS(0);                                 /* 使能片选 */
    spi1_read_write_byte(FLASH_FastReadData);       /* 发送快速读取命令 */
    spi1_read_write_byte((uint8_t)((addr) >> 16));  /* 发送24bit地址 */
    spi1_read_write_byte((uint8_t)((addr) >> 8));
    spi1_read_write_byte((uint8_t)addr);
    spi1_read_write_byte(0XFF);                     /* 8 dummy clock */
    
    slcd_spi2_mode(1);                  /* SPI 16位 只接受模式 */
    DMA2_Channel5->CCR |= 1 << 0;       /* 开启DMA传输 */
    
    /* 提前设置下一次RCR的值 */
    ndata = slcd_get_data_remain();     /* 可以传输多少数据? */
    
    SPI1->CR1 |= 1 << 6;                /* 启动SPI 接收, 此时SPI 开始接收数据 */
    
    /**
     * 利用判定语句耗时, 来保证SPI先输出数据, 然后才用定时器触发DMA去读取SPI->DR
     * 在不同的优化条件下, 此代码理论上可能需要稍微调整,未做详细验证, 待测试
     */
    if(ndata)
    {
        TIM8->RCR = ndata - 1;          /* 设置发送ndata个数据 */
    }

    TIM8->CR1 |= 1 << 0;                /* 使能定时器TIM8, 此时TIM8可以触发DMA去读取SPI->DR了 */
    
    while(1)    /* 死循环判定 */
    {
        if (TIM8->SR & 0X0001)          /* 溢出中断, 且RCR=0 */
        {
            if(g_rcr_cnt > 1)g_rcr_cnt--;   /* g_rcr_cnt小于等于1表示传输完成 */
            else
            {
                g_rcr_cnt = 0;          /* g_rcr_cnt清零 */
                TIM8->CR1 &= ~(1 << 0); /* 关闭定时器TIMX */
                break;                  /* 传输结束 */
            }
            
            ndata = slcd_get_data_remain();  /* 还有数据要发? */
            
            if (ndata)                  /* 有数据要发送 */
            {
                TIM8->RCR = ndata - 1;  /* 设置重复计数寄存器值为npwm-1, 即npwm个脉冲 */
            }
            
            TIM8->SR &= ~(1 << 0);          /* 清除中断标志位 */
        }
        //printf("DR:%X\r\n",SPI1->DR);
    }

    TIM8->CNT = 0;                      /* 清零计数器,避免下次直接输出低电平 */
    TIM8->DIER &= ~(1 << 10);           /* 关闭CC2的DMA请求 */
    TIM8->SR = 0;                       /* 清除TIM8状态寄存器 */
    DMA2_Channel5->CCR &= ~(1 << 0);    /* 关闭DMA传输 */

    slcd_gpio_set(0);                   /* 设置WR脚为通用输出功能 */
    slcd_spi2_mode(0);                  /* SPI恢复8位全双工模式 */
    LCD_CS(1);                          /* 关闭LCD片选 */
    NORFLASH_CS(1);                     /* 关闭FLASH片选 */
    lcd_scan_dir(DFT_SCAN_DIR);         /* 恢复默认方向 */

}

/**
 * @brief       显示一帧,即启动一次spi到lcd的显示.
 * @param       x       : 坐标偏移量
 * @retval      无
 */
void slcd_frame_show(uint16_t x)
{
    OS_CPU_SR cpu_sr = 0;
    lcd_scan_dir(U2D_L2R);  /* 设置扫描方向 */

    if (lcddev.id == 0X9341 || lcddev.id == 0X7789 || lcddev.id == 0X5310 || lcddev.id == 0X7796 || lcddev.id == 0X5510 || lcddev.id == 0X9806)
    {
        lcd_set_window(spbdev.stabarheight, 0, spbdev.spbheight, spbdev.spbwidth);
        lcd_set_cursor(spbdev.stabarheight, 0); /* 设置光标位置 */
    }
    else
    {
        lcd_set_window(0, spbdev.stabarheight, spbdev.spbwidth, spbdev.spbheight);

        if (lcddev.id != 0X1963)lcd_set_cursor(0, spbdev.stabarheight); /* 设置光标位置 */
    }

    lcd_write_ram_prepare();    /* 开始写入GRAM */
    
    OS_ENTER_CRITICAL();        /* 进入临界区(无法被中断打断) */
    slcd_dma_enable(x);         /* 启动一次SPI到LCD的dma传输 */
    OS_EXIT_CRITICAL();         /* 退出临界区(可以被中断打断) */
    
    lcd_scan_dir(DFT_SCAN_DIR); /* 恢复默认方向 */
    lcd_set_window(0, 0, lcddev.width, lcddev.height);  /* 恢复默认窗口大小 */
}















