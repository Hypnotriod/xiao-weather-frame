#ifndef __EPD_H__
#define __EPD_H__

#include <stdbool.h>
#include <stdint.h>

#include "fonts.h"
#include "icons.h"

#define EPD_WIDTH 400
#define EPD_HEIGHT 300
#define EPD_FRAME_BUFFER_SIZE ((EPD_WIDTH * EPD_HEIGHT) / 8)

typedef enum {
    EPD_COLOR_BLACK = 0,
    EPD_COLOR_WHITE = 1,
} epd_color_t;

int epd_init(void);
void epd_clear(void);
void epd_display(uint8_t* image);
void epd_sleep(void);

void epd_fill(uint8_t* buffer, epd_color_t color);
void epd_draw_char(const char ch, const sFONT* font, uint8_t* buffer, int16_t x, int16_t y, epd_color_t color,
                   bool transparent);
void epd_draw_string(const char* str, const sFONT* font, uint8_t* buffer, int16_t x, int16_t y, epd_color_t color,
                     bool transparent);
void epd_draw_icon(const sICON* icon, uint8_t* buffer, int16_t x, int16_t y, epd_color_t color, bool transparent);
void epd_draw_image(const uint8_t* image, uint8_t* buffer, uint16_t width, uint16_t height, int16_t x, int16_t y,
                    epd_color_t color, bool transparent);
void epd_draw_pixel(uint8_t* buffer, int16_t x, int16_t y, epd_color_t color);
void epd_draw_line(uint8_t* buffer, int16_t from_x, int16_t from_y, int16_t to_x, int16_t to_y, epd_color_t color);
void epd_draw_rectangle(uint8_t* buffer, int16_t from_x, int16_t from_y, int16_t to_x, int16_t to_y, epd_color_t color,
                        bool fill);
void epd_draw_circle(uint8_t* buffer, int16_t x, int16_t y, int16_t radius, epd_color_t color, bool fill);

#endif  //__EPD_H__