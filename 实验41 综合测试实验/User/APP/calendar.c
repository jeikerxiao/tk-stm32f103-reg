/**
 ****************************************************************************************************
 * @file        calendar.h
 * @author      正点原子团队(ALIENTEK)
 * @version     V1.1
 * @date        2022-05-26
 * @brief       APP-日历 代码
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

#include "calendar.h"
#include "stdio.h"
#include "settings.h"
#include "math.h"
#include "camera.h"
#include "./BSP/DS18B20/ds18b20.h"
#include "./BSP/24CXX/24cxx.h"
#include "./BSP/RTC/rtc.h"


_alarm_obj alarm;               /* 闹钟结构体 */
_calendar_obj calendar;         /* 日历结构体 */

static uint16_t TIME_TOPY;      /* 	120 */
static uint16_t OTHER_TOPY;     /* 	200 */

/* 星期 */
uint8_t *const calendar_week_table[GUI_LANGUAGE_NUM][7] =
{
    {"星期天", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"},
    {"星期天", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"},
    {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"},
};

/* 闹钟标题 */
uint8_t *const calendar_alarm_caption_table[GUI_LANGUAGE_NUM] =
{
    "闹钟", "鬧鐘", "ALARM",
};

/* 再响按钮 */
uint8_t *const calendar_alarm_realarm_table[GUI_LANGUAGE_NUM] =
{
    "再响", "再響", "REALARM",
};

/* 提示信息 */
uint8_t *const calendar_loading_str[GUI_LANGUAGE_NUM][3] =
{
    {
        "正在加载,请稍候...",
        "未检测到DS18B20!",
        "启用内部温度传感器...",
    },
    {
        "正在加载,请稍候...",
        "未检测到DS18B20!",
        "启用內部温度传感器...",
    },
    {
        "Loading...",
        "DS18B20 Check Failed!",
        "Use Inside Temp Sensor...",
    },
};

/**
 * @brief       重新初始化闹钟
 * @param       alarmx          : 闹钟结构体
 * @param       calendarx       : 日历结构体
 * @retval      无
 */
void calendar_alarm_init(_alarm_obj *alarmx, _calendar_obj *calendarx)
{
    uint8_t temp;
    calendar_get_date(calendarx);       /* 获取当前日期信息 */

    if (calendarx->week == 7)temp = 1 << 0;
    else temp = 1 << calendarx->week;

    if (alarmx->weekmask & temp)        /* 需要闹铃 */
    {
        rtc_set_alarm(calendarx->year, calendarx->month, calendarx->date, alarmx->hour, alarmx->min, 0);    /* 设置闹铃时间 */
    }
}

/**
 * @brief       闹钟响闹铃
 * @param       type            : 闹铃类型
 *                                0,滴.
 *                                1,滴.滴.
 *                                2,滴.滴.滴
 *                                3,滴.滴.滴.滴
 * @retval      无
 */
void calendar_alarm_ring(uint8_t type)
{
    uint8_t i;

    for (i = 0; i < (type + 1); i++)
    {
        LED1(0);
        delay_ms(50);
        LED1(1);
        delay_ms(70);
    }
}

/**
 * @brief       得到时间
 * @param       calendarx       : 日历结构体
 * @retval      无
 */
void calendar_get_time(_calendar_obj *calendarx)
{
    rtc_get_time(); /* 得到时间,在该函数内,会给calendar全局结构体填充信息 */
}

/**
 * @brief       得到日期
 * @param       calendarx       : 日历结构体
 * @retval      无
 */
void calendar_get_date(_calendar_obj *calendarx)
{
    rtc_get_time(); /* 得到时间,在该函数内,会给calendar全局结构体填充信息 */
}

/**
 * @brief       根据当前的日期,更新日历表
 * @param       无
 * @retval      无
 */
void calendar_date_refresh(void)
{
    uint8_t weekn;   /* 周寄存 */
    uint16_t offx = (lcddev.width - 240) / 2;

    /* 显示阳历年月日 */
    g_back_color = BLACK;

    lcd_show_xnum(offx + 5, OTHER_TOPY + 9, (calendar.year / 100) % 100, 2, 16, 0, 0XF81F);     /* 显示年  20/19 */
    lcd_show_xnum(offx + 21, OTHER_TOPY + 9, calendar.year % 100, 2, 16, 0, 0XF81F);            /* 显示年 */
    lcd_show_string(offx + 37, OTHER_TOPY + 9, lcddev.width, lcddev.height, 16, "-", 0XF81F);   /* "-" */
    lcd_show_xnum(offx + 45, OTHER_TOPY + 9, calendar.month, 2, 16, 0X80, 0XF81F);              /* 显示月 */
    lcd_show_string(offx + 61, OTHER_TOPY + 9, lcddev.width, lcddev.height, 16, "-", 0XF81F);   /* "-" */
    lcd_show_xnum(offx + 69, OTHER_TOPY + 9, calendar.date, 2, 16, 0X80, 0XF81F);               /* 显示日 */

    /* 显示周几? */
    weekn = calendar.week;
    text_show_string(5 + offx, OTHER_TOPY + 35, lcddev.width, lcddev.height, (char *)calendar_week_table[gui_phy.language][weekn], 16, 0, RED); /* 显示周几? */
}

/**
 * @brief       读取日历闹钟信息
 *  @note       闹钟数据保存在:SYSTEM_PARA_SAVE_BASE+sizeof(_system_setings)
 * @param       alarm           : 闹钟信息
 * @retval      无
 */
void calendar_read_para(_alarm_obj *alarm)
{
    at24cxx_read(SYSTEM_PARA_SAVE_BASE + sizeof(_system_setings), (uint8_t *)alarm, sizeof(_alarm_obj));
}

/**
 * @brief       写入日历闹钟信息
 * @param       alarm           : 闹钟信息
 * @retval      无
 */
void calendar_save_para(_alarm_obj *alarm)
{
    OS_CPU_SR cpu_sr = 0;
    alarm->ringsta &= 0X7F;     /* 清空最高位 */
    OS_ENTER_CRITICAL();        /* 进入临界区(无法被中断打断) */
    at24cxx_write(SYSTEM_PARA_SAVE_BASE + sizeof(_system_setings), (uint8_t *)alarm, sizeof(_alarm_obj));
    OS_EXIT_CRITICAL();         /* 退出临界区(可以被中断打断) */
}

/**
 * @brief       闹铃处理(尺寸:44*20)
 * @param       x,y             : 起始坐标
 * @retval      处理结果
 */
uint8_t calendar_alarm_msg(uint16_t x, uint16_t y)
{
    uint8_t rval = 0;
    uint16_t *lcdbuf = 0;   /* LCD显存 */
    
    lcdbuf = (uint16_t *)gui_memin_malloc(44 * 20 * 2); /* 申请内存 */

    if (lcdbuf)             /* 申请成功 */
    {
        app_read_bkcolor(x, y, 44, 20, lcdbuf);         /* 读取背景色 */
        gui_fill_rectangle(x, y, 44, 20, LIGHTBLUE);
        gui_draw_rectangle(x, y, 44, 20, BROWN);
        gui_show_num(x + 2, y + 2, 2, RED, 16, alarm.hour, 0X81);
        gui_show_ptchar(x + 2 + 16, y + 2, x + 2 + 16 + 8, y + 2 + 16, 0, RED, 16, ':', 1);
        gui_show_num(x + 2 + 24, y + 2, 2, RED, 16, alarm.min, 0X81);
        OSTaskSuspend(6); /* 挂起主任务 */

        while (rval == 0)
        {
            tp_dev.scan(0);

            if (tp_dev.sta & TP_PRES_DOWN)  /* 触摸屏被按下 */
            {
                if (app_tp_is_in_area(&tp_dev, x, y, 44, 20))   /* 判断某个时刻,触摸屏的值是不是在某个区域内 */
                {
                    rval = 0XFF;            /* 取消 */
                    break;
                }
            }

            delay_ms(5);

            if (system_task_return)break;   /* 需要返回 */
        }

        app_recover_bkcolor(x, y, 44, 20, lcdbuf);  /* 读取背景色 */
    }
    else rval = 1;

    system_task_return = 0;
    alarm.ringsta &= ~(1 << 7); /* 取消闹铃 */
    
    calendar_alarm_init((_alarm_obj *)&alarm, &calendar);   /* 重新初始化闹钟 */
    
    gui_memin_free(lcdbuf);
    
    OSTaskResume(6);            /* 恢复主任务 */
    
    system_task_return = 0;
    
    return rval;
}

/**
 * @brief       画圆形指针表盘
 * @param       x,y             : 坐标中心点
 * @param       size            : 表盘大小(直径)
 * @param       d               : 表盘分割,秒钟的高度
 * @retval      无
 */
void calendar_circle_clock_drawpanel(uint16_t x, uint16_t y, uint16_t size, uint16_t d)
{
    uint16_t r = size / 2; /* 得到半径 */
    uint16_t sx = x - r;
    uint16_t sy = y - r;
    uint16_t px0, px1;
    uint16_t py0, py1;
    uint16_t i;
    gui_fill_circle(x, y, r, WHITE);        /* 画外圈 */
    gui_fill_circle(x, y, r - 4, BLACK);    /* 画内圈 */

    for (i = 0; i < 60; i++)   /* 画秒钟格 */
    {
        px0 = sx + r + (r - 4) * sin((app_pi / 30) * i);
        py0 = sy + r - (r - 4) * cos((app_pi / 30) * i);
        px1 = sx + r + (r - d) * sin((app_pi / 30) * i);
        py1 = sy + r - (r - d) * cos((app_pi / 30) * i);
        gui_draw_bline1(px0, py0, px1, py1, 0, WHITE);
    }

    for (i = 0; i < 12; i++)   /* 画小时格 */
    {
        px0 = sx + r + (r - 5) * sin((app_pi / 6) * i);
        py0 = sy + r - (r - 5) * cos((app_pi / 6) * i);
        px1 = sx + r + (r - d) * sin((app_pi / 6) * i);
        py1 = sy + r - (r - d) * cos((app_pi / 6) * i);
        gui_draw_bline1(px0, py0, px1, py1, 2, YELLOW);
    }

    for (i = 0; i < 4; i++)   /* 画3小时格 */
    {
        px0 = sx + r + (r - 5) * sin((app_pi / 2) * i);
        py0 = sy + r - (r - 5) * cos((app_pi / 2) * i);
        px1 = sx + r + (r - d - 3) * sin((app_pi / 2) * i);
        py1 = sy + r - (r - d - 3) * cos((app_pi / 2) * i);
        gui_draw_bline1(px0, py0, px1, py1, 2, YELLOW);
    }

    gui_fill_circle(x, y, d / 2, WHITE);    /* 画中心圈 */
}

/**
 * @brief       画圆形指针表盘
 * @param       x,y             : 坐标中心点
 * @param       size            : 表盘大小(直径)
 * @param       d               : 表盘分割,秒钟的高度
 * @param       hour            : 时钟
 * @param       min             : 分钟
 * @param       sec             : 秒钟
 * @retval      无
 */
void calendar_circle_clock_showtime(uint16_t x, uint16_t y, uint16_t size, uint16_t d, uint8_t hour, uint8_t min, uint8_t sec)
{
    static uint8_t oldhour = 0; /* 最近一次进入该函数的时分秒信息 */
    static uint8_t oldmin = 0;
    static uint8_t oldsec = 0;
    float temp;
    uint16_t r = size / 2;      /* 得到半径 */
    uint16_t sx = x - r;
    uint16_t sy = y - r;
    uint16_t px0, px1;
    uint16_t py0, py1;
    uint8_t r1;

    if (hour > 11)hour -= 12;

    /* 清除小时 */
    r1 = d / 2 + 4;
    /* 清除上一次的数据 */
    temp = (float)oldmin / 60;
    temp += oldhour;
    px0 = sx + r + (r - 3 * d - 7) * sin((app_pi / 6) * temp);
    py0 = sy + r - (r - 3 * d - 7) * cos((app_pi / 6) * temp);
    px1 = sx + r + r1 * sin((app_pi / 6) * temp);
    py1 = sy + r - r1 * cos((app_pi / 6) * temp);
    gui_draw_bline1(px0, py0, px1, py1, 2, BLACK);

    /* 清除分钟 */
    r1 = d / 2 + 3;
    temp = (float)oldsec / 60;
    temp += oldmin;
    /* 清除上一次的数据 */
    px0 = sx + r + (r - 2 * d - 7) * sin((app_pi / 30) * temp);
    py0 = sy + r - (r - 2 * d - 7) * cos((app_pi / 30) * temp);
    px1 = sx + r + r1 * sin((app_pi / 30) * temp);
    py1 = sy + r - r1 * cos((app_pi / 30) * temp);
    gui_draw_bline1(px0, py0, px1, py1, 1, BLACK);

    /* 清除秒钟 */
    r1 = d / 2 + 3;
    /* 清除上一次的数据 */
    px0 = sx + r + (r - d - 7) * sin((app_pi / 30) * oldsec);
    py0 = sy + r - (r - d - 7) * cos((app_pi / 30) * oldsec);
    px1 = sx + r + r1 * sin((app_pi / 30) * oldsec);
    py1 = sy + r - r1 * cos((app_pi / 30) * oldsec);
    gui_draw_bline1(px0, py0, px1, py1, 0, BLACK);

    /* 显示小时 */
    r1 = d / 2 + 4;
    /* 显示新的时钟 */
    temp = (float)min / 60;
    temp += hour;
    px0 = sx + r + (r - 3 * d - 7) * sin((app_pi / 6) * temp);
    py0 = sy + r - (r - 3 * d - 7) * cos((app_pi / 6) * temp);
    px1 = sx + r + r1 * sin((app_pi / 6) * temp);
    py1 = sy + r - r1 * cos((app_pi / 6) * temp);
    gui_draw_bline1(px0, py0, px1, py1, 2, YELLOW);

    /* 显示分钟 */
    r1 = d / 2 + 3;
    temp = (float)sec / 60;
    temp += min;
    /* 显示新的分钟 */
    px0 = sx + r + (r - 2 * d - 7) * sin((app_pi / 30) * temp);
    py0 = sy + r - (r - 2 * d - 7) * cos((app_pi / 30) * temp);
    px1 = sx + r + r1 * sin((app_pi / 30) * temp);
    py1 = sy + r - r1 * cos((app_pi / 30) * temp);
    gui_draw_bline1(px0, py0, px1, py1, 1, GREEN);

    /* 显示秒钟 */
    r1 = d / 2 + 3;
    /* 显示新的秒钟 */
    px0 = sx + r + (r - d - 7) * sin((app_pi / 30) * sec);
    py0 = sy + r - (r - d - 7) * cos((app_pi / 30) * sec);
    px1 = sx + r + r1 * sin((app_pi / 30) * sec);
    py1 = sy + r - r1 * cos((app_pi / 30) * sec);
    gui_draw_bline1(px0, py0, px1, py1, 0, RED);
    oldhour = hour;	/* 保存时 */
    oldmin = min;   /* 保存分 */
    oldsec = sec;   /* 保存秒 */
}

/**
 * @brief       时间显示模式
 * @param       无
 * @retval      0, 正常退出; 其他, 错误代码;
 */
uint8_t calendar_play(void)
{
    uint8_t second = 0;
    short temperate = 0;    /* 温度值 */
    uint8_t t = 0;
    uint8_t tempdate = 0;
    uint8_t rval = 0;       /* 返回值 */
    uint8_t res;
    uint16_t xoff = 0;
    uint16_t yoff = 0;      /* 表盘y偏移量 */
    uint16_t r = 0;         /* 表盘半径 */
    uint8_t d = 0;          /* 指针长度 */
    uint16_t color = 0X07FF;/* 颜色 */
    uint32_t br = 0;

    uint8_t TEMP_SEN_TYPE = 0;  /* 默认使用DS18B20 */
    FIL *f_calendar = 0;

    f_calendar = (FIL *)gui_memin_malloc(sizeof(FIL)); /* 开辟FIL字节的内存区域 */

    if (f_calendar == NULL)rval = 1;    /* 申请失败 */
    else
    {
        res = f_open(f_calendar, (const TCHAR *)APP_ASCII_S6030, FA_READ); /* 打开文件 */

        if (res == FR_OK)
        {
            asc2_s6030 = (uint8_t *)gui_memex_malloc(f_calendar->obj.objsize);  /* 为大字体开辟缓存地址 */

            if (asc2_s6030 == 0)rval = 1;
            else
            {
                res = f_read(f_calendar, asc2_s6030, f_calendar->obj.objsize, (UINT *)&br); /* 一次读取整个文件 */
            }

            f_close(f_calendar);
        }

        if (res)rval = res;
    }

    if (rval == 0)   /* 无错误 */
    {
        adc_temperature_init(); /* 重新初始化温度传感器 */
        lcd_clear(BLACK);       /* 清黑屏 */
        second = calendar.sec;  /* 得到此刻的秒钟 */
        text_show_string(48, 60, lcddev.width, lcddev.height, (char *)calendar_loading_str[gui_phy.language][0], 16, 0x01, color); /* 显示进入信息 */

        if (ds18b20_init())
        {
            text_show_string(48, 76, lcddev.width, lcddev.height, (char *)calendar_loading_str[gui_phy.language][1], 16, 0x01, color);
            delay_ms(500);
            text_show_string(48, 92, lcddev.width, lcddev.height, (char *)calendar_loading_str[gui_phy.language][2], 16, 0x01, color);
            TEMP_SEN_TYPE = 1;
        }

        delay_ms(1100);     /* 等待1.1s */
        g_back_color = BLACK;
        lcd_clear(BLACK);   /* 清黑屏 */
        r = lcddev.width / 3;
        d = lcddev.width / 40;
        yoff = (lcddev.height - r * 2 - 140) / 2;
        TIME_TOPY = yoff + r * 2 + 10;
        OTHER_TOPY = TIME_TOPY + 60 + 10;
        xoff = (lcddev.width - 240) / 2;
        calendar_circle_clock_drawpanel(lcddev.width / 2, yoff + r, r * 2, d); /* 显示指针时钟表盘 */
        calendar_date_refresh();    /* 加载日历 */
        tempdate = calendar.date;   /* 天数暂存器 */
        gui_phy.back_color = BLACK;
        gui_show_ptchar(xoff + 70 - 4, TIME_TOPY, lcddev.width, lcddev.height, 0, color, 60, ':', 0);	/* ":" */
        gui_show_ptchar(xoff + 150 - 4, TIME_TOPY, lcddev.width, lcddev.height, 0, color, 60, ':', 0);	/* ":" */
    }

    while (rval == 0)
    {
        calendar_get_time(&calendar);   /* 更新时间 */

        if (system_task_return)break;   /* 需要返回 */

        if (second != calendar.sec)     /* 秒钟改变了 */
        {
            second = calendar.sec;
            calendar_circle_clock_showtime(lcddev.width / 2, yoff + r, r * 2, d, calendar.hour, calendar.min, calendar.sec); /* 指针时钟显示时间 */
            gui_phy.back_color = BLACK;
            gui_show_num(xoff + 10, TIME_TOPY, 2, color, 60, calendar.hour, 0X80);  /* 显示时 */
            gui_show_num(xoff + 90, TIME_TOPY, 2, color, 60, calendar.min, 0X80);   /* 显示分 */
            gui_show_num(xoff + 170, TIME_TOPY, 2, color, 60, calendar.sec, 0X80);  /* 显示秒 */

            if (t % 2 == 0)   /* 等待2秒钟 */
            {
                if (TEMP_SEN_TYPE)temperate = adc_get_temperature() / 10;   /* 得到内部温度传感器采集到的温度,0.1℃ */
                else temperate = ds18b20_get_temperature();                 /* 得到18b20温度 */

                if (temperate < 0)  /* 温度为负数的时候，红色显示 */
                {
                    color = RED;
                    temperate = -temperate; /* 改为正温度 */
                }
                else color = 0X07FF;        /* 正常为棕红色字体显示 */

                gui_show_num(xoff + 90, OTHER_TOPY, 2, color, 60, temperate / 10, 0X80);    /* XX */
                gui_show_ptchar(xoff + 150, OTHER_TOPY, lcddev.width, lcddev.height, 0, color, 60, '.', 0); /* "." */
                gui_show_ptchar(xoff + 180 - 15, OTHER_TOPY, lcddev.width, lcddev.height, 0, color, 60, temperate % 10 + '0', 0); /* 显示小数 */
                gui_show_ptchar(xoff + 210 - 10, OTHER_TOPY, lcddev.width, lcddev.height, 0, color, 60, 95 + ' ', 0);   /* 显示℃ */

                if (t > 0)t = 0;
            }

            calendar_get_date(&calendar);   /* 更新日期 */

            if (calendar.date != tempdate)
            {
                calendar_date_refresh();    /* 天数变化了,更新日历 */
                tempdate = calendar.date;   /* 修改tempdate，防止重复进入 */
            }

            t++;
        }

        delay_ms(20);
    };

    while (tp_dev.sta & TP_PRES_DOWN)tp_dev.scan(0);    /* 等待TP松开 */

    gui_memex_free(asc2_s6030); /* 删除申请的内存 */
    asc2_s6030 = 0;             /* 清零 */
    gui_memin_free(f_calendar); /* 删除申请的内存 */
    g_point_color = BLUE;
    g_back_color = WHITE ;
    return rval;
}




















