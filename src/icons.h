#ifndef __ICONS_H__
#define __ICONS_H__

#include <stdint.h>

typedef struct {
    const uint8_t* data;
    uint16_t width;
    uint16_t height;
} sICON;

extern const sICON IconBattery;
extern const sICON IconTemperature;
extern const sICON IconHumidity;
extern const sICON IconPressure;

#endif  //__ICONS_H__