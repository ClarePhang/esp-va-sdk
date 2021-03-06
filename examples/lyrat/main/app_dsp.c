// Copyright 2018 Espressif Systems (Shanghai) PTE LTD
// All rights reserved.

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <string.h>

#include <esp_log.h>
#include <speech_recognizer.h>
#include <va_mem_utils.h>
#include <i2s_stream.h>
#include <media_hal.h>
#include <audio_board.h>
#include <media_hal.h>
#include <resampling.h>

#include "app_dsp.h"

#define DETECT_SAMP_RATE 16000UL
#define SAMP_RATE 48000UL
#define SAMP_BITS 16
#define PCM_SIZE (4 * 1024)
#define SAMPLE_SZ ((DETECT_SAMP_RATE * I2S_STREAM_BUFFER_SIZE) / (SAMP_RATE))

static const char *TAG = "dsp";

static struct dsp_data {
    i2s_stream_t *read_i2s_stream;
    audio_resample_config_t resample;
    int pcm_stored_data;
    bool write_to_store;
    char pcm_store[PCM_SIZE];
    int16_t data_buf[SAMPLE_SZ];
    bool dsp_inited;
} dd;

static media_hal_config_t media_hal_conf = {
    .op_mode    = MEDIA_HAL_MODE_SLAVE,
    .adc_input  = MEDIA_HAL_ADC_INPUT_LINE1,
    .dac_output = MEDIA_HAL_DAC_OUTPUT_ALL,
    .codec_mode = MEDIA_HAL_CODEC_MODE_BOTH,
    .bit_length = MEDIA_HAL_BIT_LENGTH_16BITS,
    .format     = MEDIA_HAL_I2S_NORMAL,
    .port_num = 0,
};

static esp_err_t reader_stream_event_handler(void *arg, int event, void *data)
{
    ESP_LOGI(TAG, "Reader stream event %d", event);
    return ESP_OK;
}

static ssize_t dsp_write_cb(void *h, void *data, int len, uint32_t wait)
{
    ssize_t sent_len;
    if (len <= 0) {
        return len;
    }
    sent_len = audio_resample((short *)data, (short *)dd.data_buf, SAMP_RATE, DETECT_SAMP_RATE, len / 2, SAMPLE_SZ, 2, &dd.resample);
    sent_len = audio_resample_down_channel((short *)dd.data_buf, (short *)dd.data_buf, DETECT_SAMP_RATE, DETECT_SAMP_RATE, sent_len, SAMPLE_SZ, 0, &dd.resample);
    sent_len = sent_len * 2;  //convert 16bit lengtth to number of bytes

    if (dd.write_to_store) {
        if ( (dd.pcm_stored_data + sent_len) < sizeof(dd.pcm_store)) {
            //printf("Writing to store at pcm_stored_data %d %d sizeof %d\n", dd.pcm_stored_data, sent_len, sizeof(dd.pcm_store));
            memcpy(dd.pcm_store + dd.pcm_stored_data, dd.data_buf, sent_len);
            dd.pcm_stored_data += sent_len;
            return sent_len;
        } else {
            /* store buffer is full, raise the 'Recognize' event, and flush the data */
            ESP_LOGI(TAG, "Sending recognize command");
            speech_recognizer_recognize(0, TAP);
            speech_recognizer_record(dd.pcm_store, dd.pcm_stored_data);
            ESP_LOGI(TAG, "Flushed store data: %d\n", dd.pcm_stored_data);
            dd.write_to_store = false;
        }
    }
    sent_len = speech_recognizer_record(dd.data_buf, sent_len);
    //printf("recorded speech %d\n", sent_len);
    return sent_len;
}

int va_app_speech_stop()
{
    ESP_LOGI(TAG, "Sending stop command");
    if (audio_stream_stop(&dd.read_i2s_stream->base) != 0) {
        ESP_LOGE(TAG, "Failed to stop I2S audio stream");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Stopped I2S audio stream");
    return ESP_OK;
}

int va_app_speech_start()
{
    if (!dd.dsp_inited)
        return ESP_FAIL;

    ESP_LOGI(TAG, "Sending start command");
    if (audio_stream_start(&dd.read_i2s_stream->base) != 0) {
        ESP_LOGE(TAG, "Failed to start I2S audio stream");
        return ESP_FAIL;
    }
    return ESP_OK;
}

void app_dsp_send_recognize()
{
    if (!dd.dsp_inited)
        return;

    ESP_LOGI(TAG, "Sending start command");
    if (audio_stream_start(&dd.read_i2s_stream->base) != 0) {
        ESP_LOGE(TAG, "Failed to start I2S audio stream");
    }
    dd.pcm_stored_data = 0;
    dd.write_to_store = true;
}

void app_dsp_reset()
{
    return;
}

void app_dsp_init(void)
{
    i2s_stream_config_t i2s_cfg;
    memset(&i2s_cfg, 0, sizeof(i2s_cfg));
    i2s_cfg.i2s_num = 0;
    audio_board_i2s_init_default(&i2s_cfg.i2s_config);
    i2s_cfg.media_hal_cfg = media_hal_init(&media_hal_conf);

    dd.read_i2s_stream = i2s_reader_stream_create(&i2s_cfg);
    if (dd.read_i2s_stream) {
        ESP_LOGI(TAG, "Created I2S audio stream");
    } else {
        ESP_LOGE(TAG, "Failed creating I2S audio stream");
    }
    i2s_stream_set_stack_size(dd.read_i2s_stream, 5000);

    audio_io_fn_arg_t stream_reader_fn = {
        .func = dsp_write_cb,
        .arg = NULL,
    };
    audio_event_fn_arg_t stream_event_fn = {
        .func = reader_stream_event_handler,
    };
    if (audio_stream_init(&dd.read_i2s_stream->base, "i2s_reader", &stream_reader_fn, &stream_event_fn) != 0) {
        ESP_LOGE(TAG, "Failed creating audio stream");
        i2s_stream_destroy(dd.read_i2s_stream);
        dd.read_i2s_stream = NULL;
    }
    audio_stream_start(&dd.read_i2s_stream->base);
    vTaskDelay(10/portTICK_RATE_MS);
    audio_stream_stop(&dd.read_i2s_stream->base);
    i2s_set_clk(I2S_NUM_0, SAMP_RATE, SAMP_BITS, I2S_CHANNEL_STEREO);
    dd.dsp_inited = true;
}
