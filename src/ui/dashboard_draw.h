#ifndef BT_DASHBOARD_DRAW_H
#define BT_DASHBOARD_DRAW_H

#include <stdbool.h>
#include <stdint.h>

typedef enum { DASH_BODY, DASH_LABEL, DASH_TITLE } dash_font_t;
typedef enum {
    DASH_CIRCLE,
    DASH_BLUETOOTH,
    DASH_GAMEPAD,
    DASH_SEARCH,
    DASH_ROL_RING,
    DASH_ROL_Q1,
    DASH_ROL_Q2,
    DASH_ROL_Q3,
    DASH_ROL_Q4
} dash_icon_t;
typedef enum { DASH_SILVER, DASH_RIBBON, DASH_GLEAM } dash_art_t;

bool dash_open(void);
void dash_close(void);
bool dash_begin(uint32_t width, uint32_t height);
void dash_end(void);
void dash_rect(float x, float y, float w, float h, uint32_t rgb);
void dash_border(float x, float y, float w, float h, float thickness, uint32_t rgb);
void dash_pill(float x, float y, float w, float h, uint32_t rgb);
void dash_icon(dash_icon_t icon, float x, float y, float size, uint32_t rgb);
void dash_art(dash_art_t art, float x, float y, float w, float h, uint32_t rgb);
float dash_text_width(dash_font_t font, const char *text);
void dash_text(dash_font_t font, float x, float baseline, float max_width,
               uint32_t rgb, const char *text);
#endif
