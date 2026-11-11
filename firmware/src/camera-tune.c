/* See camera-tune.h. */

#include "camera-tune.h"

uint16_t camera_tune_gain_ceiling_reg(unsigned times)
{
    /* Below 1x is not a ceiling the sensor can honour; above 63x does not
       fit the ten bits. */
    if (times < 1) {
        times = 1;
    }
    const unsigned v = times * 16u;
    return (uint16_t)(v > CAMERA_TUNE_GAIN_REG_MAX ? CAMERA_TUNE_GAIN_REG_MAX : v);
}

uint8_t camera_tune_band_3c00(unsigned hz)
{
    return hz == 50 ? CAMERA_TUNE_BAND_3C00_MASK : 0x00;
}

uint8_t camera_tune_band_3c01(unsigned hz)
{
    /* Manual for a named frequency, automatic for 0. */
    return hz == 0 ? 0x00 : CAMERA_TUNE_BAND_3C01_MASK;
}
