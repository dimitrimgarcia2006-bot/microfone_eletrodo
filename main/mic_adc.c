/*
 * Microfone eletreto no ADC contínuo (DMA)
 *
 * O DMA grava blocos de MIC_READ_SAMPLES amostras num ringbuffer do driver.
 * mic_adc_read() bloqueia a task até haver dados e desempacota o formato
 * TYPE2 (canal + valor de 12 bits) em amostras uint16_t.
 */

#include "mic_adc.h"

#include "esp_err.h"
#include "esp_log.h"
#include "soc/soc_caps.h"
#include "esp_adc/adc_continuous.h"
#include "config.h"

#define MIC_ADC_CHANNEL   ADC_CHANNEL_3      /* GPIO4 no ESP32-S3 */
#define MIC_ADC_ATTEN     ADC_ATTEN_DB_12
#define READ_BYTES        (MIC_READ_SAMPLES * SOC_ADC_DIGI_RESULT_BYTES)
#define READ_TIMEOUT_MS   1000

static const char *TAG = "mic_adc";

static adc_continuous_handle_t adc;
static uint8_t adc_buf[READ_BYTES];

void mic_adc_start(void)
{
    adc_continuous_handle_cfg_t handle_cfg = {
        .max_store_buf_size = 8 * READ_BYTES,   /* folga de ~256 ms */
        .conv_frame_size    = READ_BYTES,
    };
    ESP_ERROR_CHECK(adc_continuous_new_handle(&handle_cfg, &adc));

    adc_digi_pattern_config_t pattern = {
        .atten     = MIC_ADC_ATTEN,
        .channel   = MIC_ADC_CHANNEL,
        .unit      = ADC_UNIT_1,
        .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH,
    };
    adc_continuous_config_t cfg = {
        .pattern_num    = 1,
        .adc_pattern    = &pattern,
        .sample_freq_hz = SAMPLE_RATE_HZ,
        .conv_mode      = ADC_CONV_SINGLE_UNIT_1,
        .format         = ADC_DIGI_OUTPUT_FORMAT_TYPE2,
    };
    ESP_ERROR_CHECK(adc_continuous_config(adc, &cfg));
    ESP_ERROR_CHECK(adc_continuous_start(adc));
}

size_t mic_adc_read(uint16_t *out, size_t max_samples)
{
    if (max_samples > MIC_READ_SAMPLES) {
        max_samples = MIC_READ_SAMPLES;
    }

    uint32_t n_bytes = 0;
    esp_err_t ret = adc_continuous_read(adc, adc_buf, max_samples * SOC_ADC_DIGI_RESULT_BYTES,
                                        &n_bytes, READ_TIMEOUT_MS);
    if (ret == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "Timeout lendo o ADC");
        return 0;
    }
    ESP_ERROR_CHECK(ret);

    size_t n = 0;
    for (uint32_t i = 0; i < n_bytes; i += SOC_ADC_DIGI_RESULT_BYTES) {
        adc_digi_output_data_t *p = (adc_digi_output_data_t *)&adc_buf[i];
        if (p->type2.channel != MIC_ADC_CHANNEL) {
            continue;
        }
        out[n++] = (uint16_t)p->type2.data;
    }
    return n;
}
