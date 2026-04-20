#ifndef __FONT_H__
#define __FONT_H__

#include <stdint.h>

typedef struct {
    const uint8_t* table;
    uint16_t width;
    uint16_t height;
} sFONT;

extern const sFONT FontRobotoBold40;
extern const sFONT FontRobotoRegular32;
extern const sFONT Font24;
extern const sFONT Font20;
extern const sFONT Font16;
extern const sFONT Font12;
extern const sFONT Font8;

#endif  //__FONT_H__