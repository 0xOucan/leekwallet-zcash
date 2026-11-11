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

uint16_t camera_tune_shutter_cap(unsigned lines, unsigned vts)
{
    if (vts == 0) {
        vts = 1;
    }
    if (lines < 1) {
        lines = 1;
    }
    if (lines > vts) {
        lines = vts;
    }
    return (uint16_t)(lines > 0xFFFF ? 0xFFFF : lines);
}

const uint8_t CAMERA_TUNE_AEC_CENTRE[4][4] = {
    { 1,  1,  1, 1 },
    { 1, 15, 15, 1 },
    { 1, 15, 15, 1 },
    { 1,  1,  1, 1 },
};

void camera_tune_aec_pack(const uint8_t grid[4][4], uint8_t out[8])
{
    for (int i = 0; i < 8; i++) {
        const int row = i / 2, col = (i % 2) * 2;
        out[i] = (uint8_t)(((grid[row][col + 1] & 0x0F) << 4) | (grid[row][col] & 0x0F));
    }
}

int camera_tune_ae_level(int bias, bool centre_weighted, int offset)
{
    int level = centre_weighted ? bias + offset : bias;
    if (level < -5) { level = -5; }
    if (level > 5)  { level = 5; }
    return level;
}
