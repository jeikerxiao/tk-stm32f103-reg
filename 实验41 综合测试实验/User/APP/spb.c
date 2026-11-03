/**
 ****************************************************************************************************
 * @file        spb.c
 * @author      正点原子团队(ALIENTEK)
 * @version     V1.1
 * @date        2022-05-26
 * @brief       SPB效果实现 代码
 * @license     Copyright (c) 2020-2032, 广州市星翼电子科技有限公司
 ****************************************************************************************************
 * @attention
 *
 * 实验平台:正点原子 开发板
 * 在线视频:www.yuanzige.com
 * 技术论坛:www.openedv.com
 * 公司网址:www.alientek.com
 * 购买地址:openedv.taobao.com
 *
 * 修改说明
 * V1.1 20220526
 * 1, 修改注释方式
 * 2, 修改u8/u16/u32为uint8_t/uint16_t/uint32_t
 ****************************************************************************************************
 */

#include "./PICTURE/piclib.h"
#include "./SYSTEM/delay/delay.h"
#include "./SYSTEM/usart/usart.h"
#include "./MALLOC/malloc.h"
#include "./TEXT/text.h"
#include "./BSP/TOUCH/touch.h"
#include "./BSP/SPBLCD/spblcd.h"

#include "spb.h"
#include "string.h"
#include "common.h"
#include "calendar.h"
 
 
/* SPB 控制器 */
m_spb_dev spbdev;

uint8_t *const SPB_REMIND_MSG = "SPB Updating... Please Wait...";

/* 背景图路径,根据不同的lcd选择不同的路径 */
uint8_t *const spb_bkpic_path_tbl[3][4] =
{
    {
        "1:/SYSTEM/SPB/BACKPIC/left_60224.jpg",
        "1:/SYSTEM/SPB/BACKPIC/left_240224.jpg",
        "1:/SYSTEM/SPB/BACKPIC/right_240224.jpg",
        "1:/SYSTEM/SPB/BACKPIC/right_60224.jpg",
    },
    {
        "1:/SYSTEM/SPB/BACKPIC/left_80364.jpg",
        "1:/SYSTEM/SPB/BACKPIC/left_320364.jpg",
        "1:/SYSTEM/SPB/BACKPIC/right_320364.jpg",
        "1:/SYSTEM/SPB/BACKPIC/right_80364.jpg",
    },
    {
        "1:/SYSTEM/SPB/BACKPIC/480610.jpg",
        "",
        "",
        "",
    },
};

/* 界面图标的路径表 */
uint8_t *const spb_icos_path_tbl[3][SPB_ICOS_NUM] =
{
    {
        "1:/SYSTEM/SPB/ICOS/ebook_56.bmp",
        "1:/SYSTEM/SPB/ICOS/picture_56.bmp",
        "1:/SYSTEM/SPB/ICOS/clock_56.bmp",
        "1:/SYSTEM/SPB/ICOS/set_56.bmp",
        "1:/SYSTEM/SPB/ICOS/notepad_56.bmp",
        "1:/SYSTEM/SPB/ICOS/setup_56.bmp",
        "1:/SYSTEM/SPB/ICOS/paint_56.bmp",
        "1:/SYSTEM/SPB/ICOS/usb_56.bmp",
        "1:/SYSTEM/SPB/ICOS/earthnet_56.bmp",
        "1:/SYSTEM/SPB/ICOS/wireless_56.bmp",
        "1:/SYSTEM/SPB/ICOS/keytest_56.bmp",
        "1:/SYSTEM/SPB/ICOS/ledtest_56.bmp",
    },
    {
        "1:/SYSTEM/SPB/ICOS/ebook_70.bmp",
        "1:/SYSTEM/SPB/ICOS/picture_70.bmp",
        "1:/SYSTEM/SPB/ICOS/clock_70.bmp",
        "1:/SYSTEM/SPB/ICOS/set_70.bmp",
        "1:/SYSTEM/SPB/ICOS/notepad_70.bmp",
        "1:/SYSTEM/SPB/ICOS/setup_70.bmp",
        "1:/SYSTEM/SPB/ICOS/paint_70.bmp",
        "1:/SYSTEM/SPB/ICOS/usb_70.bmp",
        "1:/SYSTEM/SPB/ICOS/earthnet_70.bmp",
        "1:/SYSTEM/SPB/ICOS/wireless_70.bmp",
        "1:/SYSTEM/SPB/ICOS/keytest_70.bmp",
        "1:/SYSTEM/SPB/ICOS/ledtest_70.bmp",
    },
    { 
        "1:/SYSTEM/SPB/ICOS/ebook_110.bmp",
        "1:/SYSTEM/SPB/ICOS/picture_110.bmp",
        "1:/SYSTEM/SPB/ICOS/clock_110.bmp",
        "1:/SYSTEM/SPB/ICOS/set_110.bmp",
        "1:/SYSTEM/SPB/ICOS/notepad_110.bmp",
        "1:/SYSTEM/SPB/ICOS/setup_110.bmp",
        "1:/SYSTEM/SPB/ICOS/paint_110.bmp",
        "1:/SYSTEM/SPB/ICOS/usb_110.bmp",
        "1:/SYSTEM/SPB/ICOS/earthnet_110.bmp",
        "1:/SYSTEM/SPB/ICOS/wireless_110.bmp",
        "1:/SYSTEM/SPB/ICOS/keytest_110.bmp",
        "1:/SYSTEM/SPB/ICOS/ledtest_110.bmp",
    },
};

/* 三个主图标的路径表 */
uint8_t *const spb_micos_path_tbl[3][3] =
{
    {
        "1:/SYSTEM/SPB/icos/calc_56.bmp",
        "1:/SYSTEM/SPB/icos/app_56.bmp",
        "1:/SYSTEM/SPB/icos/v meter_56.bmp",
    },
    {
        "1:/SYSTEM/SPB/icos/calc_70.bmp",
        "1:/SYSTEM/SPB/icos/app_70.bmp",
        "1:/SYSTEM/SPB/icos/v meter_70.bmp",
    },
    {
        "1:/SYSTEM/SPB/icos/calc_110.bmp",
        "1:/SYSTEM/SPB/icos/app_110.bmp",
        "1:/SYSTEM/SPB/icos/v meter_110.bmp",
    },
};

/* 界面图标icos的对应功能名字 */
uint8_t *const icos_name_tbl[GUI_LANGUAGE_NUM][SPB_ICOS_NUM] =
{
    {
        "电子图书", "数码相框", "时钟", "系统设置",
        "记事本", "运行器", "手写画笔", "USB 连接",
        "网络通信", "无线传书", "按键测试", "LED测试", 
    },
    {
        "電子圖書", "數碼相框", "時鐘", "系統設置",
        "記事本", "運行器", "手寫畫筆", "USB 連接",
        "網絡通信", "無線傳書", "按鍵測試", "LED測試",
    },
    {
        "EBOOK", "PHOTOS", "TIME", "SETTINGS",
        "NOTEPAD", "EXE", "PAINT", "USB",
        "ETHERNET", "WIRELESS", "KEY", "LED",
    },
};

/* 主图标对应的名字 */
uint8_t *const micos_name_tbl[GUI_LANGUAGE_NUM][3] =
{
    {
        "计算器", "应用中心", "电压表",
    },
    {
        "計算器", "應用中心", "電壓表",
    },
    {
        "CALC", "APPS", "VOLT",
    },
};

/**
 * @brief       初始化spb各个参数
 * @param       mode            : 0, 根据需要自动选择是否更新; 1,强制重新更新;
 * @retval      无
 */
uint8_t spb_init(uint8_t mode)
{

    uint16_t i;
    uint8_t res = 0;
    uint8_t lcdtype = 0;    /* LCD类型 */
    uint8_t icoindex = 0;   /* ico索引编号 */
    uint16_t icowidth;      /* 图标的宽度 */
    uint16_t icoxpit;       /* x方向图标之间的间距 */
    uint16_t micoyoff;

    uint32_t curwidth = 0;  /* 当前宽度 */
    uint32_t curoffset = 0; /* 当前偏移量 */

    memset((void *)&spbdev, 0, sizeof(spbdev));
    spbdev.selico = 0xff;

    if (lcddev.width == 240)        /* 对于240*320的LCD屏幕 */
    {
        icowidth = 56;
        micoyoff = 4;
        lcdtype = 0;
        icoindex = 0;
        spbdev.stabarheight = 20;
        spbdev.spbheight = 224;
        spbdev.spbwidth = 240;
        spbdev.spbfontsize = 12;
    }
    else if (lcddev.width == 320)   /* 对于320*480的LCD屏幕 */
    {
        icowidth = 70;
        micoyoff = 6;
        lcdtype = 1;
        icoindex = 1;
        spbdev.stabarheight = 24;
        spbdev.spbheight = 364;
        spbdev.spbwidth = 320;
        spbdev.spbfontsize = 12;
    }
    else if (lcddev.width == 480)   /* 对于480*800的LCD屏幕 */
    {
        icowidth = 110;
        micoyoff = 18;
        lcdtype = 2;
        icoindex = 2;
        spbdev.stabarheight = 30;
        spbdev.spbheight = 610;
        spbdev.spbwidth = 480;
        spbdev.spbfontsize = 16;
    }

    icoxpit = (lcddev.width - icowidth * 4) / 4;
    spbdev.spbahwidth = spbdev.spbwidth / 4;

    for (i = 0; i < SPB_ICOS_NUM; i++)
    {
        spbdev.icos[i].width = icowidth;    /* 必须 等于图片的宽度尺寸 */
        spbdev.icos[i].height = icowidth + spbdev.spbfontsize + spbdev.spbfontsize / 4;
        
        if(lcddev.width == 480)     /* 对于800*480屏幕,只有一页,16个图标都在这一页里面 */
        {
            spbdev.icos[i].x = icoxpit / 2 + (i % 4) * (icowidth + icoxpit);
            spbdev.icos[i].y = spbdev.stabarheight + 4 + (i / 4) * (spbdev.icos[i].height + icoxpit);
        }
        else                        /* 对于非800*480屏幕,有2页,16个图标都在2个页里面 */
        {
            spbdev.icos[i].x = icoxpit / 2 + (i % 4) * (icowidth + icoxpit);
            spbdev.icos[i].y = spbdev.stabarheight + 4 + ((i % 8) / 4) * (spbdev.icos[i].height + icoxpit);
        }
        
        spbdev.icos[i].path = (uint8_t *)spb_icos_path_tbl[icoindex][i];
        spbdev.icos[i].name = (uint8_t *)icos_name_tbl[gui_phy.language][i];
    }

    for (i = 0; i < 3; i++)
    {
        spbdev.micos[i].x = (spbdev.spbwidth - 3 * icowidth - 2 * icoxpit) / 2 + i * (icowidth + icoxpit);
        spbdev.micos[i].y = spbdev.stabarheight + spbdev.spbheight + micoyoff;
        spbdev.micos[i].width = icowidth;   /* 必须 等于图片的宽度尺寸 */
        spbdev.micos[i].height = icowidth + spbdev.spbfontsize + spbdev.spbfontsize / 4;
        spbdev.micos[i].path = (uint8_t *)spb_micos_path_tbl[icoindex][i];
        spbdev.micos[i].name = (uint8_t *)micos_name_tbl[gui_phy.language][i];
    }

    norflash_read(&res, SPILCD_END_ADDR - 1, 1);    /* 读取标志位（存储在SPILCD存储区域的最后一个字节） */

    if (((res != lcdtype) && (lcddev.id < 0XFF00)) || mode) /* 标志不正确/强制更新 */
    {
        lcd_clear(BLACK);
        gui_show_strmid(0, spbdev.spbheight, lcddev.width, lcddev.height - spbdev.spbheight, WHITE, spbdev.spbfontsize, SPB_REMIND_MSG); /* 显示提示信息 */
        curoffset = 0;

        if(lcdtype == 2)    /* 800*480的屏幕,只有一页 */
        {
            spbdev.spbahwidth = 0;      /* 800*480屏幕 没有滑动区 */
            curwidth = spbdev.spbwidth;
            res = piclib_ai_load_picfile((char *)spb_bkpic_path_tbl[lcdtype][0], 0, 0, curwidth, spbdev.spbheight, 0); /* 解码背景图片 */
            res += spb_load_icos(0);    /* 加载第1页图标 */
            res += spb_load_icos(1);    /* 加载第2页图标 */
            
            if (res == 0)
            {
                slcd_frame_lcd2spi(curoffset, curwidth, spbdev.spbheight);  /* 写入帧,800*480屏幕只有一帧 */
            }
        }
        else                /* 其他屏幕,有4页(含2个边界小图片) */
        {
            for (i = 0; i < 4; i++)
            {
                if (i == 0 || i == 3)   /* 边界宽度等于屏幕滑动区域宽度的1/4 */
                {
                    curwidth = spbdev.spbwidth / 4;
                }
                else
                {
                    curwidth = spbdev.spbwidth;

                }

                res = piclib_ai_load_picfile((char *)spb_bkpic_path_tbl[lcdtype][i], 0, 0, curwidth, spbdev.spbheight, 0); /* 解码图片 */

                if (res == 0)
                {
                    if (i > 0 && i < 3) /* 非边界图片，要加载ICOS */
                    {
                        res = spb_load_icos(i - 1);
                    }
                    
                    if (res == 0)
                    {
                        slcd_frame_lcd2spi(curoffset, curwidth, spbdev.spbheight);  /* 写入在第0帧 */
                        curoffset += curwidth * spbdev.spbheight;
                    }
                }
            }
        }
        
        norflash_write(&lcdtype, SPILCD_END_ADDR - 1, 1);
    }
    
    if(lcddev.width == 480)
    {
        spbdev.spbahwidth = 0;  /* 800*480屏幕 没有滑动区 */
    }
    
    /* 指向LCD */
    pic_phy.read_point = lcd_read_point;
    pic_phy.draw_point = lcd_draw_point;
    pic_phy.fillcolor = piclib_fill_color;
    picinfo.lcdwidth = lcddev.width;
    gui_phy.read_point = lcd_read_point;
    gui_phy.draw_point = lcd_draw_point;

    spbdev.pos = spbdev.spbahwidth; /* 默认是第1页开始位置 */

    return 0;
}

/**
 * @brief       装载icos
 * @param       无
 * @retval      0,成功; 其他,错误代码;
 */
uint8_t spb_load_icos(uint8_t idx)
{
    uint8_t j;
    uint8_t res = 0;
    idx *= 8;

    for (j = 0; j < 8; j++)
    {
        res = minibmp_decode(spbdev.icos[idx].path, spbdev.icos[idx].x, spbdev.icos[idx].y - spbdev.stabarheight, spbdev.icos[idx].width, spbdev.icos[idx].width, 0, 0);

        if (res)return 1;

        gui_show_strmid(spbdev.icos[idx].x, spbdev.icos[idx].y + spbdev.icos[idx].width - spbdev.stabarheight, spbdev.icos[idx].width, spbdev.spbfontsize, SPB_FONT_COLOR, spbdev.spbfontsize, spbdev.icos[idx].name); /* 显示名字 */
        idx++;

        if (idx >= SPB_ICOS_NUM)break;  /* 超过图标总数了,退出 */
    }

    return 0;
}


/**
 * @brief       装载主界面icos
 * @param       无
 * @retval      0,成功; 其他,错误代码;
 */
uint8_t spb_load_micos(void)
{
    uint8_t j;
    uint8_t res = 0;
    gui_fill_rectangle(0, spbdev.stabarheight + spbdev.spbheight, lcddev.width, lcddev.height - spbdev.stabarheight - spbdev.spbheight, SPB_MICO_BKCOLOR);

    for (j = 0; j < 3; j++)
    {
        res = minibmp_decode(spbdev.micos[j].path, spbdev.micos[j].x, spbdev.micos[j].y, spbdev.micos[j].width, spbdev.micos[j].width, 0, 0);

        if (res)return 1;

        gui_show_strmid(spbdev.micos[j].x, spbdev.micos[j].y + spbdev.micos[j].width, spbdev.micos[j].width, spbdev.spbfontsize, SPB_FONT_COLOR, spbdev.spbfontsize, spbdev.micos[j].name); /* 显示名字 */
    }

    return 0;
}

/* SD卡图标 */
/* PCtoLCD2002取模方式:阴码,逐行式,顺向 */
const uint8_t SPB_SD_ICO[60] =
{
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0xFF, 0x00, 0x0F, 0xFF, 0x00, 0x0F, 0xFF, 0x00, 0x0F,
    0xFF, 0x00, 0x0F, 0xFF, 0x00, 0x0F, 0xFF, 0x00, 0x0F, 0xFF, 0x00, 0x0F, 0xFF, 0x00, 0x0F, 0xFC,
    0x00, 0x0F, 0xFE, 0x00, 0x0F, 0xFF, 0x00, 0x0F, 0xFF, 0x00, 0x0F, 0xFC, 0x00, 0x0A, 0xAC, 0x00,
    0x0A, 0xAC, 0x00, 0x0A, 0xAC, 0x00, 0x0F, 0xFC, 0x00, 0x00, 0x00, 0x00,
};

/* USB 图标, 取模方式如上 */
const uint8_t SPB_USB_ICO[60] =
{
    0x00, 0x00, 0x00, 0x00, 0x60, 0x00, 0x00, 0xF0, 0x00, 0x01, 0xF8, 0x00, 0x00, 0x60, 0x00, 0x00,
    0x67, 0x00, 0x04, 0x67, 0x00, 0x0E, 0x62, 0x00, 0x0E, 0x62, 0x00, 0x04, 0x62, 0x00, 0x04, 0x7C,
    0x00, 0x06, 0x60, 0x00, 0x03, 0xE0, 0x00, 0x00, 0x60, 0x00, 0x00, 0x60, 0x00, 0x00, 0x60, 0x00,
    0x00, 0xF0, 0x00, 0x00, 0xF0, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00,
};

/**
 * @brief       显示gsm信号强度,占用20*20像素大小
 * @param       x,y             : 起始坐标
 * @param       signal          : 信号强度,范围:0~30
 * @retval      无
 */
void spb_gsm_signal_show(uint16_t x, uint16_t y, uint8_t signal)
{
    uint8_t t;
    uint16_t color;
    signal /= 5;

    if (signal > 5)signal = 5;

    for (t = 0; t < 5; t++)
    {
        if (signal)   /* 有信号 */
        {
            signal--;
            color = WHITE;
        }
        else color = 0X6B4D;   /* 无信号 */

        gui_fill_rectangle(x + 1 + t * 4, y + 20 - 6 - t * 3, 3, (t + 1) * 3, color);
    }
}

/* LOGO信息 */
uint8_t *const spb_logo_msg[GUI_LANGUAGE_NUM]=
{
    "正点原子", "正點原子", "ALIENTEK",
};

/**
 * @brief       更新顶部信息条信息数据
 * @param       clr             : 0,不清除背景; 1,清楚背景;
 * @retval      无
 */
void spb_stabar_msg_show(uint8_t clr)
{
    uint8_t temp;

    if (clr)
    {
        gui_fill_rectangle(0, 0, lcddev.width, spbdev.stabarheight, BLACK);
        gui_show_string("CPU:  %", 24 + 64 + 20 + 2 + 20 + 2, (spbdev.stabarheight - 16) / 2, 64, 16, 16, WHITE);       /* 显示CPU数据 */
        gui_show_string(":", lcddev.width - 42 + 16, (spbdev.stabarheight - 16) / 2, 8, 16, 16, WHITE);                 /* 显示':' */
        
        
        app_show_mono_icos(2, (spbdev.stabarheight - 20) / 2, 19, 19, (uint8_t *)APP_ALIENTEK_ICO1919, WHITE, BLACK);   /* 显示正点原子LOGO */
        gui_show_string(spb_logo_msg[gui_phy.language], 2 + 19 + 2, (spbdev.stabarheight - 16) / 2, 64, 16, 16, WHITE); /* 显示正点原子字符串 */
    }
    
    /* 更新SD卡信息 */
    if (gui_phy.memdevflag & (1 << 0))app_show_mono_icos(24 + 64 + 2, (spbdev.stabarheight - 20) / 2, 20, 20, (uint8_t *)SPB_SD_ICO, WHITE, BLACK);
    else gui_fill_rectangle(24 + 64 + 2, 0, 20, spbdev.stabarheight, BLACK);

    /* 更新U盘信息 */
    if (gui_phy.memdevflag & (1 << 2))app_show_mono_icos(24 + 64 + 2 + 20, (spbdev.stabarheight - 20) / 2, 20, 20, (uint8_t *)SPB_USB_ICO, WHITE, BLACK);
    else gui_fill_rectangle(24 + 64 + 2 + 20, 0, 20, spbdev.stabarheight, BLACK);

    /* 显示CPU使用率 */
    gui_phy.back_color = BLACK;
    temp = OSCPUUsage;

    if (OSCPUUsage > 99)temp = 99; /* 最多显示到99% */

    gui_show_num(24 + 64 + 2 + 20 + 20 + 2 + 32, (spbdev.stabarheight - 16) / 2, 2, WHITE, 16, temp, 0);    /* 显示CPU使用率 */
    
    /* 显示时间 */
    calendar_get_time(&calendar);
    gui_show_num(lcddev.width - 42, (spbdev.stabarheight - 16) / 2, 2, WHITE, 16, calendar.hour, 0X80);     /* 显示时钟 */
    gui_show_num(lcddev.width - 2 - 16, (spbdev.stabarheight - 16) / 2, 2, WHITE, 16, calendar.min, 0X80);  /* 显示分钟 */
}

extern uint8_t *const sysset_remindmsg_tbl[2][GUI_LANGUAGE_NUM];

/**
 * @brief       加载SPB主界面UI
 * @param       无
 * @retval      0,成功; 其他,错误代码;
 */
uint8_t spb_load_mui(void)
{
    uint8_t res = 0;
    
    if (spbdev.spbheight == 0 && spbdev.spbwidth == 0)
    {
        lcd_clear(BLACK);   /* 黑屏 */
        window_msg_box((lcddev.width - 220) / 2, (lcddev.height - 100) / 2, 220, 100, (uint8_t *)sysset_remindmsg_tbl[1][gui_phy.language], (uint8_t *)sysset_remindmsg_tbl[0][gui_phy.language], 12, 0, 0, 0); /* 显示提示信息 */
        res = spb_init(0);  /* 重新初始化 */
    }
    
    if(res == 0)
    {
        spb_stabar_msg_show(1);         /* 显示状态栏信息,清除所有后显示 */
        slcd_frame_show(spbdev.pos);    /* 显示主界面icos */
        res = spb_load_micos();         /* 加载主图标 */
    }
    return res;
}

/**
 * @brief       移动屏幕
 * @param       dir             : 方向,0:左移;1,右移
 * @param       skips           : 每次跳跃列数
 * @param       pos             : 起始位置
 * @retval      无
 */
void spb_frame_move(uint8_t dir, uint8_t skips, uint16_t pos)
{
    uint8_t i;
    uint16_t x;
    uint16_t endpos = spbdev.spbahwidth;


    for (i = 0; i < SPB_PAGE_NUM; i++)  /* 得到终点位置 */
    {
        if (dir == 0)   /* 左移 */
        {
            if (pos <= (spbdev.spbwidth * i + spbdev.spbahwidth))
            {
                endpos = spbdev.spbwidth * i + spbdev.spbahwidth;
                break;
            }
        }
        else    /* 右移 */
        {
            if (pos >= (spbdev.spbwidth * (SPB_PAGE_NUM - i - 1) + spbdev.spbahwidth))
            {
                endpos = spbdev.spbwidth * (SPB_PAGE_NUM - i - 1) + spbdev.spbahwidth;
                break;
            }
        }
    }

    if (dir)    /* 屏幕右移 */
    {
        for (x = pos; x > endpos;)
        {
            if ((x - endpos) > skips)x -= skips;
            else x = endpos;

            slcd_frame_show(x);
        }
    }
    else        /* 屏幕左移 */
    {
        for (x = pos; x < endpos;)
        {
            x += skips;

            if (x > endpos)x = endpos;

            slcd_frame_show(x);
        }
    }

    spbdev.pos = endpos;
}

/**
 * @brief       清除某个mico图标的选中状态
 * @param       selx            : SPB_ICOS_NUM~SPB_ICOS_NUM+2,表示将要清除选中状态的mico编号
 * @retval      无
 */
void spb_unsel_micos(uint8_t selx)
{
    uint8_t size = 2;                   /* 默认2个像素粗 */
    
    if(lcddev.width > 240)              /* 大于240的屏幕, 矩形框为3个像素粗 */
    {
        size = 3;
    }
    
    if (selx >= SPB_ICOS_NUM && selx < (SPB_ICOS_NUM + 3))  /* 合法的编号 */
    {
        selx -= SPB_ICOS_NUM;
        spb_rect_color(spbdev.micos[selx].x, spbdev.micos[selx].y, spbdev.micos[selx].width, spbdev.micos[selx].height, size, SPB_MICO_BKCOLOR);
    }
}

/**
 * @brief       画一个矩形框
 * @param       x,y             : 起始坐标
 * @param       width,height    : 宽度和高度
 * @param       size            : 矩形框的厚度(往内缩)
 * @param       color           : 矩形框的颜色
 * @retval      无
 */
void spb_rect_color(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint8_t size, uint16_t color)
{
    uint8_t i = 0;
    for(i = 0; i < size; i++)
    {
        gui_draw_rectangle(x + i, y + i, width - (2 * i), height - (2 * i), color);
    }
}

/**
 * @brief       设置选中哪个图标
 * @param       sel             : 0~SPB_ICOS_NUM代表当前页的选中ico
 * @retval      无
 */
void spb_set_sel(uint8_t sel)
{
    uint8_t size = 2;                   /* 默认2个像素粗 */
    
    if(lcddev.width > 240)              /* 大于240的屏幕, 矩形框为3个像素粗 */
    {
        size = 3;
    }
    
    slcd_frame_show(spbdev.pos);        /* 刷新背景 */
    spb_unsel_micos(spbdev.selico);     /* 清除主图标选中状态 */
    spbdev.selico = sel;

    if (sel < SPB_ICOS_NUM)
    { 
        spb_rect_color(spbdev.icos[sel].x, spbdev.icos[sel].y, spbdev.icos[sel].width, spbdev.icos[sel].height, size, RED);
    }
    else
    {
        sel -= SPB_ICOS_NUM;
        spb_rect_color(spbdev.micos[sel].x, spbdev.micos[sel].y, spbdev.micos[sel].width, spbdev.micos[sel].height, size, RED);
    }
}

/**
 * @brief       屏幕滑动及按键检测
 * @param       无
 * @retval      0~15,被双击的图标编号
 *              0xff,没有任何图标被双击或者按下
 */
uint8_t spb_move_chk(void)
{

    uint8_t i = 0xff;
    uint16_t temp = 0;
    uint16_t icosx = 0;
    uint16_t movewidth = spbdev.spbwidth * (SPB_PAGE_NUM - 1) + 2 * spbdev.spbahwidth; /* 可以移动宽度 */
    uint16_t movecnt = 0;   /* 得到滑动点数 */
    uint8_t skips = 8;      /* 默认2.8寸屏, 每次跳8个像素 */

    tp_dev.scan(0);         /* 扫描 */

    if (lcddev.width == 320)    /* 对3.5寸屏 */
    {
         skips = 16;
    }
    
    if (tp_dev.sta & TP_PRES_DOWN)  /* 有按键被按下 */
    {
        if (lcddev.width == 480)    /* 4.3屏,不支持滑动,单独处理 */
        { 
            spbdev.spbsta |= 1 << 15;       /* 标记按下 */
            spbdev.curxpos = tp_dev.x[0];   /* 记录当前坐标 */
            spbdev.curypos = tp_dev.y[0];   /* 记录当前坐标 */ 
        }
        else
        {
            if (spbdev.spbsta & 0X8000) /* 已经被标记了 */
            {
                movecnt = spbdev.spbsta & 0X3FFF;   /* 得到滑动点数 */

                if (gui_disabs(spbdev.curxpos, tp_dev.x[0]) >= SPB_MOVE_WIN)    /* 移动大于等于SPB_MOVE_WIN个点 */
                {
                    if (movecnt < SPB_MOVE_MIN / SPB_MOVE_WIN)spbdev.spbsta++;  /* 点数增加1 */
                }
            }

            spbdev.curxpos = tp_dev.x[0];       /* 记录当前坐标 */
            spbdev.curypos = tp_dev.y[0];       /* 记录当前坐标 */
            
            if (spbdev.curxpos > lcddev.width || spbdev.curypos > lcddev.height)/* 大于屏幕分辨率,则是非法的数据 */
            {
                tp_dev.sta = 0;
                spbdev.spbsta = 0;
                return 0XFF;    
            }

            if ((spbdev.spbsta & 0X8000) == 0)  /* 按键第一次被按下 */
            {
                spbdev.spbsta = 0;              /* 状态清零 */
                spbdev.spbsta |= 1 << 15;       /* 标记按下 */
                spbdev.oldxpos = tp_dev.x[0];   /* 记录按下时的坐标 */
                spbdev.oldpos = spbdev.pos;     /* 记录按下时的帧位置 */
            }
            else if (spbdev.spbsta & 0X4000)        /* 有滑动 */
            {
                if (spbdev.oldxpos > tp_dev.x[0])   /* x左移,屏幕pos右移 */
                {
                    if (spbdev.pos < movewidth)spbdev.pos += spbdev.oldxpos - tp_dev.x[0];

                    if (spbdev.pos > movewidth)spbdev.pos = movewidth;  /* 溢出了 */
                }
                else    /* 右移,屏幕pos左移 */
                {
                    if (spbdev.pos > 0)spbdev.pos -= tp_dev.x[0] - spbdev.oldxpos;

                    if (spbdev.pos > movewidth)spbdev.pos = 0;          /* 溢出了 */
                }

                spbdev.oldxpos = tp_dev.x[0];
                slcd_frame_show(spbdev.pos);
            }
            else if ((gui_disabs(spbdev.curxpos, spbdev.oldxpos) >= SPB_MOVE_MIN) && (movecnt >= SPB_MOVE_MIN / SPB_MOVE_WIN))  /* 滑动距离超过SPB_MOVE_MIN,并且检测到的有效滑动数不少于SPB_MOVE_MIN/SPB_MOVE_WIN */
            {
                spbdev.spbsta |= 1 << 14; /* 标记滑动 */
            }
        }
    }
    else    /* 按键松开了 */
    {
        if (spbdev.spbsta & 0x8000)     /* 之前有按下 */
        {
            if (spbdev.spbsta & 0X4000) /* 有滑动 */
            {
                if (spbdev.pos > spbdev.oldpos) /* 左移 */
                {
                    if (((spbdev.pos - spbdev.oldpos) > SPB_MOVE_ACT) && (spbdev.oldpos < (movewidth - spbdev.spbahwidth)))
                    {
                        spb_frame_move(0, skips, spbdev.pos);
                    }
                    else
                    {
                        spb_frame_move(1, skips, spbdev.pos);
                    }
                }
                else    /* 右移 */
                {
                    if (((spbdev.oldpos - spbdev.pos) > SPB_MOVE_ACT) && (spbdev.pos > spbdev.spbahwidth))
                    {
                        spb_frame_move(1, skips, spbdev.pos);
                    }
                    else
                    {
                        spb_frame_move(0, skips, spbdev.pos);
                    }
                }

                spb_unsel_micos(spbdev.selico); /* 清除主图标选中状态 */
                spbdev.selico = 0xff;           /* 取消spbdev.selico原先的设置 */
            }
            else    /* 属于点按 */
            {
                for (i = 0; i < SPB_ICOS_NUM + 3; i++)  /* 检查按下的图标编号 */
                {
                    if (i < SPB_ICOS_NUM)
                    {
                        temp = spbdev.curxpos + spbdev.pos - spbdev.spbahwidth;     /* 在整个背景图片的实际x坐标 */
                        
                        if(lcddev.width == 480) /* 800*480屏幕, 不支持滑屏,只有1页 */
                        {
                            icosx = spbdev.icos[i].x;
                        }
                        else    /* 非800*480屏幕, 支持滑屏, 有多页 */
                        {
                            icosx = spbdev.icos[i].x + (i / 8) * spbdev.spbwidth;   /* 计算在整个背景图片的实际地址 */
                        }
                        
                        if ((temp > icosx) && (temp < icosx + spbdev.icos[i].width) && (spbdev.curypos > spbdev.icos[i].y) &&
                                (spbdev.curypos < spbdev.icos[i].y + spbdev.icos[i].height))
                        {
                            break;  /* 得到选中的编号 */
                        }
                    }
                    else
                    {
                        if ((spbdev.curxpos > spbdev.micos[i - SPB_ICOS_NUM].x) && (spbdev.curxpos < spbdev.micos[i - SPB_ICOS_NUM].x + spbdev.micos[i - SPB_ICOS_NUM].width) &&
                                (spbdev.curypos > spbdev.micos[i - SPB_ICOS_NUM].y) && (spbdev.curypos < spbdev.micos[i - SPB_ICOS_NUM].y + spbdev.micos[i - SPB_ICOS_NUM].height))
                        {
                            break;  /* 得到选中的编号 */
                        }
                    }
                }

                if (i < (SPB_ICOS_NUM + 3)) /* 有效 */
                {
                    if (i != spbdev.selico) /* 选中了不同的图标,切换图标 */
                    {
                        spb_set_sel(i);
                        i = 0xff;
                    }
                    else spbdev.selico = 0XFF;  /* 发生了双击,重新复位selico */
                }
                else i = 0XFF;  /* 无效的点按 */
            }
        }

        spbdev.spbsta = 0;  /* 清空标志 */
        tp_dev.sta = 0;     /* 清零状态 */
    }
    
    return i;
}


