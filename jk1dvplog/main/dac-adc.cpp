/* Copyright (c) 2021-2024 Eiichiro Araki
This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program. If not, see http://www.gnu.org/licenses/.
*/

#include "Arduino.h"
#include "driver/i2s.h" //https://github.com/espressif/arduino-esp32/blob/master/tools/sdk/include/driver/driver/i2s.h
#include "driver/adc.h" 

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "soc/rtc_io_reg.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/sens_reg.h"
#include "soc/rtc.h"


#include "driver/dac.h"

int clk_8m_div = 7;      // RTC 8M clock divider (division is by clk_8m_div+1, i.e. 0 means 8MHz frequency)
int frequency_step = 40;  // Frequency step for CW generator
int scale = 3;           // 50% of the full scale
int offset=0;              // leave it default / 0 = no any offset
int invert = 2;          // invert MSB to get sine waveform


/*
 * Enable cosine waveform generator on a DAC channel
 */
void dac_cosine(dac_channel_t channel, int enable)
{
    // Enable tone generator common to both channels
    switch(channel) {
        case DAC_CHANNEL_1:
          if (enable) {
            SET_PERI_REG_MASK(SENS_SAR_DAC_CTRL1_REG, SENS_SW_TONE_EN);
            // Enable / connect tone tone generator on / to this channel
            SET_PERI_REG_MASK(SENS_SAR_DAC_CTRL2_REG, SENS_DAC_CW_EN1_M);
            // Invert MSB, otherwise part of waveform will have inverted
            SET_PERI_REG_BITS(SENS_SAR_DAC_CTRL2_REG, SENS_DAC_INV1, 2, SENS_DAC_INV1_S);
          } else {
            CLEAR_PERI_REG_MASK(SENS_SAR_DAC_CTRL2_REG, SENS_DAC_CW_EN1_M);

          }
            break;
        case DAC_CHANNEL_2:
          if (enable) {
            SET_PERI_REG_MASK(SENS_SAR_DAC_CTRL1_REG, SENS_SW_TONE_EN);
            SET_PERI_REG_MASK(SENS_SAR_DAC_CTRL2_REG, SENS_DAC_CW_EN2_M);
            SET_PERI_REG_BITS(SENS_SAR_DAC_CTRL2_REG, SENS_DAC_INV2, 2, SENS_DAC_INV2_S);

          } else {
            CLEAR_PERI_REG_MASK(SENS_SAR_DAC_CTRL2_REG, SENS_DAC_CW_EN2_M);
          }
            break;
        default :
           printf("Channel %d\n", channel);
    }
}


/*
 * Set frequency of internal CW generator common to both DAC channels
 *
 * clk_8m_div: 0b000 - 0b111
 * frequency_step: range 0x0001 - 0xFFFF
 *
 */
void dac_frequency_set(int clk_8m_div, int frequency_step)
{
    REG_SET_FIELD(RTC_CNTL_CLK_CONF_REG, RTC_CNTL_CK8M_DIV_SEL, clk_8m_div);
    SET_PERI_REG_BITS(SENS_SAR_DAC_CTRL1_REG, SENS_SW_FSTEP, frequency_step, SENS_SW_FSTEP_S);
}


/*
 * Scale output of a DAC channel using two bit pattern:
 *
 * - 00: no scale
 * - 01: scale to 1/2
 * - 10: scale to 1/4
 * - 11: scale to 1/8
 *
 */
void dac_scale_set(dac_channel_t channel, int scale)
{
    switch(channel) {
        case DAC_CHANNEL_1:
            SET_PERI_REG_BITS(SENS_SAR_DAC_CTRL2_REG, SENS_DAC_SCALE1, scale, SENS_DAC_SCALE1_S);
            break;
        case DAC_CHANNEL_2:
            SET_PERI_REG_BITS(SENS_SAR_DAC_CTRL2_REG, SENS_DAC_SCALE2, scale, SENS_DAC_SCALE2_S);
            break;
        default :
           printf("Channel %d\n", channel);
    }
}


/*
 * Offset output of a DAC channel
 *
 * Range 0x00 - 0xFF
 *
 */
void dac_offset_set(dac_channel_t channel, int offset)
{
    switch(channel) {
        case DAC_CHANNEL_1:
            SET_PERI_REG_BITS(SENS_SAR_DAC_CTRL2_REG, SENS_DAC_DC1, offset, SENS_DAC_DC1_S);
            break;
        case DAC_CHANNEL_2:
            SET_PERI_REG_BITS(SENS_SAR_DAC_CTRL2_REG, SENS_DAC_DC2, offset, SENS_DAC_DC2_S);
            break;
        default :
           printf("Channel %d\n", channel);
    }
}


/*
 * Invert output pattern of a DAC channel
 *
 * - 00: does not invert any bits,
 * - 01: inverts all bits,
 * - 10: inverts MSB,
 * - 11: inverts all bits except for MSB
 *
 */
void dac_invert_set(dac_channel_t channel, int invert)
{
    switch(channel) {
        case DAC_CHANNEL_1:
            SET_PERI_REG_BITS(SENS_SAR_DAC_CTRL2_REG, SENS_DAC_INV1, invert, SENS_DAC_INV1_S);
            break;
        case DAC_CHANNEL_2:
            SET_PERI_REG_BITS(SENS_SAR_DAC_CTRL2_REG, SENS_DAC_INV2, invert, SENS_DAC_INV2_S);
            break;
        default :
           printf("Channel %d\n", channel);
    }
}

/*
 * Main task that let you test CW parameters in action
 *
*/
void dactask(void* arg)
{
  float frequency;
    while(1){

        // frequency setting is common to both channels
      //        dac_frequency_set(clk_8m_div, frequency_step);

        /* Tune parameters of channel 2 only
         * to see and compare changes against channel 1
         */
      //        dac_scale_set(DAC_CHANNEL_2, scale);
      //        dac_offset_set(DAC_CHANNEL_2, offset);
      //        dac_invert_set(DAC_CHANNEL_2, invert);

        dac_scale_set(DAC_CHANNEL_1, 0);
        dac_offset_set(DAC_CHANNEL_1, offset);
        dac_invert_set(DAC_CHANNEL_1, invert);

        dac_frequency_set(7, 37);	
        frequency = RTC_FAST_CLK_FREQ_APPROX / (1 + 7) * (float)37/ 65536;
	//	frequency_step = freq/8M*(1+clk_8m_div)*65536
	printf("frequency: %.0f Hz %d\n", frequency,RTC_FAST_CLK_FREQ_APPROX);
        printf("DAC2 scale: %d, offset %d, invert: %d\n", scale, offset, invert);
        vTaskDelay(2000/portTICK_PERIOD_MS);

        frequency = RTC_FAST_CLK_FREQ_APPROX / (1 + 2) * (float) 49 / 65536;
	printf("frequency: %.0f Hz\n", frequency);	
        dac_frequency_set(2, 49);
        vTaskDelay(2000/portTICK_PERIOD_MS);
        frequency = RTC_FAST_CLK_FREQ_APPROX / (1 + 2) * (float) 53 / 65536;
	printf("frequency: %.0f Hz\n", frequency);	
        dac_frequency_set(2, 53);
        vTaskDelay(2000/portTICK_PERIOD_MS);	
	dac_cosine(DAC_CHANNEL_1,0);
	//	dac_cosine(DAC_CHANNEL_2,0);
        vTaskDelay(2000/portTICK_PERIOD_MS);	
	dac_cosine(DAC_CHANNEL_1,1);
	//	dac_cosine(DAC_CHANNEL_2,1);
    }
}


/*
 * Generate a sine waveform on both DAC channels:
 *
 * DAC_CHANNEL_1 - GPIO25
 * DAC_CHANNEL_2 - GPIO26
 *
 * Connect scope to both GPIO25 and GPIO26
 * to observe the waveform changes
 * in response to the parameter change
*/
#ifdef notdef
void app_main()
{
    dac_cosine(DAC_CHANNEL_1,1);
    //    dac_cosine(DAC_CHANNEL_2,1);

    dac_output_enable(DAC_CHANNEL_1); // <-> dac_output_disable()
    //    dac_output_enable(DAC_CHANNEL_2);

    xTaskCreate(dactask, "dactask", 1024*3, NULL, 10, NULL);
}

#endif
#include "esp_adc_cal.h"




esp_adc_cal_characteristics_t adcChar;

void adc_setup(){
  //    Serial.begin(115200);
    // ADCを起動（ほかの部分で明示的にOFFにしてなければなくても大丈夫）
    //    adc_power_acquire();
    // ADC1_CH6を初期化
    //    adc_gpio_init(ADC_UNIT_1, ADC_CHANNEL_0);
    // ADC1の解像度を12bit（0~4095）に設定
    adc1_config_width(ADC_WIDTH_BIT_12);
    // ADC1の減衰を11dBに設定
    adc1_config_channel_atten(ADC1_CHANNEL_0, ADC_ATTEN_DB_11);
    // 電圧値に変換するための情報をaddCharに格納
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &adcChar);
}

void adc_process(){
    uint32_t voltage;
    // ADC1_CH6の電圧値を取得
    esp_adc_cal_get_voltage(ADC_CHANNEL_0, &adcChar, &voltage);
    //    Serial.println(String(voltage));
//    delay(300);
}


#define PIN_MIC_IN 36
#define ADC_INPUT ADC1_CHANNEL_0 //pin 36
//#define ADC_INPUT ADC1_CHANNEL_3 //pin 39

#define THRESH_SCALE (1<<9) //trigger level

#define ADC_RINGBUF_NBANK (4)
#define NUM_MEM_SECT 256*4

#define MAX_STORAGE_SIZE  NUM_MEM_SECT*ADC_RINGBUF_NBANK

uint16_t adc_ringbuf[ADC_RINGBUF_NBANK][NUM_MEM_SECT]; // i2s read will store to this buffer in the background
volatile uint8_t adc_ridx_bank=0,adc_widx_bank=0;
uint16_t adc_ridx_sect=0; // in the ridx_bank where to start read the next sample.
//uint32_t adc_stream_nread=0; 
volatile uint8_t f_adc_i2s_read=0; // flag single i2s read completed 

xTaskHandle gxHandle;
//#define I2S_SAMPLE_RATE 38000*2
//#define I2S_SAMPLE_RATE 16000*2
//#define I2S_SAMPLE_RATE 78125*2
#define I2S_SAMPLE_RATE 8000
//#define I2S_SAMPLE_RATE 100000

void i2sInit()//ref to samplecode HiFreq_ADC.ino
{
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_ADC_BUILT_IN),
    .sample_rate =  I2S_SAMPLE_RATE,              // The format of the signal using ADC_BUILT_IN
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT, // is fixed at 12bit, stereo, MSB
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    //.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, 
    //.channel_format = I2S_CHANNEL_FMT_ALL_LEFT,
    //.communication_format = I2S_COMM_FORMAT_I2S_MSB,
    .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB), 
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8, //num of Bytes, from 2 upto 128
    .dma_buf_len = 64, //num of sample not Bytes, from 8 upto 1024  //https://github.com/espressif/esp-idf/blob/master/components/driver/i2s.c#L922
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  adc1_config_channel_atten(ADC_INPUT, ADC_ATTEN_DB_12);
  adc1_config_width(ADC_WIDTH_BIT_12);

  esp_err_t erReturns;
  erReturns = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  //  Serial.print("i2s_driver_install : ");Serial.println(erReturns);
  erReturns = i2s_set_adc_mode(ADC_UNIT_1, ADC_INPUT);
  //  Serial.print("i2s_set_adc_mode : ");Serial.println(erReturns);
  i2s_adc_enable(I2S_NUM_0);
}

void taskI2sReading(void *arg){
  size_t uiGotLen=0;
  while(1){
    esp_err_t erReturns = i2s_read(I2S_NUM_0, (char *)adc_ringbuf[adc_widx_bank], NUM_MEM_SECT*sizeof(uint16_t), &uiGotLen, portMAX_DELAY);
    adc_widx_bank=(adc_widx_bank+1)%ADC_RINGBUF_NBANK;
    f_adc_i2s_read =1;
  }
}


void i2s_setup() {
  //  Serial.begin(115200);
  pinMode(PIN_MIC_IN, INPUT);

  //guiStorage = (uint16_t *)malloc(MAX_STORAGE_SIZE*sizeof(uint16_t));


  i2sInit();
  xTaskCreate(taskI2sReading, "taskI2sReading", 2048, NULL, 1, &gxHandle);


}

int adc_buffer_count()
{
  // return with number of samples readable in the buffer
  return 0;
}
// read count samples into the storage return 
int adc_read_i2s(uint16_t *storage,int ncount)
{
  int count=0;
  return count;

}


// read adc by i2s
void i2s_loop()
{
}

