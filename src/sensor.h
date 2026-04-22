#include <stdlib.h>

#define SENSOR_VAL_FORMAT(val) (val / 100), abs(val) % 100
#define SENSOR_VAL_FORMAT_SHORT(val) (val / 100), (abs(val) % 100) / 10
