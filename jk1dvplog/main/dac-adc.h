#ifndef FILE_DAC_ADC_H
#define FILE_DAC_ADC_H
#include "driver/dac.h"
void dac_cosine(dac_channel_t channel, int enable);
void dac_frequency_set(int clk_8m_div, int frequency_step);
void dac_scale_set(dac_channel_t channel, int scale);
void dac_offset_set(dac_channel_t channel, int offset);
void dac_invert_set(dac_channel_t channel, int invert);
#endif
