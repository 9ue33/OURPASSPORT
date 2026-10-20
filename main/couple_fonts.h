#pragma once

/*
 * Heartlink 情侣心动终端 —— 字体别名。
 *
 * 界面里所有 lv_font_t 都从这里取，绝不直接写 lv_font_montserrat_xx：
 * 一旦中文字体接进来，只改本文件（及一个编译开关），所有页面自动生效。
 *
 * 为什么要这么绕：LVGL 自带的 Montserrat 只有 ASCII，中文会整片变成
 * 「missing glyph」方框，而且「编译通过」并不代表屏幕上画得出来。
 * 中文文案必须先由 tools 生成子集字体（只含界面真正用到的码点），
 * 生成物放在 assets/fonts/ 下，再用 COUPLE_USE_APP_FONT 打开。
 *
 * 打开方式（由 main/CMakeLists.txt 在 SRCS 里加上生成物并定义宏）：
 *   target_compile_definitions(${COMPONENT_LIB} PRIVATE COUPLE_USE_APP_FONT=1)
 */

#include "lvgl.h"

#ifdef COUPLE_USE_APP_FONT

LV_FONT_DECLARE(app_font_16)
LV_FONT_DECLARE(app_font_20)

#define COUPLE_FONT_SMALL (&app_font_16) /* 次要说明、状态条 */
#define COUPLE_FONT_BODY (&app_font_16)  /* 正文、按钮提示 */
#define COUPLE_FONT_TITLE (&app_font_20) /* 标题 */
#define COUPLE_FONT_NUM (&app_font_20)   /* 大号数字 */

#else /* 未接入中文字体：退化成官方自带的 Montserrat，界面仍是英文标签 */

#define COUPLE_FONT_SMALL (&lv_font_montserrat_14)
#define COUPLE_FONT_BODY (&lv_font_montserrat_16)
#define COUPLE_FONT_TITLE (&lv_font_montserrat_20)
#define COUPLE_FONT_NUM (&lv_font_montserrat_24)

#endif