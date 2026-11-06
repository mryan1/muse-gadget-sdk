/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * M5Stack Atom VoiceS3R / Atom EchoS3R (C126, the same board):
 * ESP32-S3-PICO-1-N8R8 (8 MB flash, 8 MB octal PSRAM), one ES8311 codec
 * (I2C 0x18) doing both directions on one duplex I2S bus, a single MEMS mic
 * into its ADC, an NS4150B amp and 1 W speaker, the front button, USB-C power
 * and no battery. No screen and no status LED.
 *
 * Pins are from M5Unified (src/M5Unified.inl, board_M5AtomVoiceS3R: I2S,
 * the ES8311 bus on SDA 45 / SCL 0, the amp enable on GPIO18, the button on
 * GPIO41). Two community ports of the board (ESPHome, a Home Assistant voice
 * client) agree. M5Stack's product page lists several of them wrongly.
 *
 * There's nothing to draw on, but Muse's UI wants a display: the board gives
 * it a 128x128 LVGL display whose flush throws the pixels away. Feedback is
 * the speaker and the Muse app.
 */
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_mem.h"

static const char *TAG = "board";

#define I2C_SDA GPIO_NUM_45
#define I2C_SCL GPIO_NUM_0         /* also the BOOT strap: the bus idles high */
#define I2S_MCLK GPIO_NUM_11
#define I2S_BCLK GPIO_NUM_17
#define I2S_WS GPIO_NUM_3
#define I2S_DOUT GPIO_NUM_48       /* to the ES8311's DAC */
#define I2S_DIN GPIO_NUM_4         /* from its ADC */
#define AMP_EN GPIO_NUM_18         /* NS4150B, active high */
#define TALK_GPIO GPIO_NUM_41      /* the front button, active low */
#define ES8311_ADDR 0x18

/* The UI lays itself out for a screen this size; nothing is shown. */
#define VIEW_W 128
#define VIEW_H 128
#define VIEW_LINES 32

static i2c_master_bus_handle_t s_i2c;
static esp_codec_dev_handle_t s_spk, s_mic;
static muse_gpio_button_t s_talk;
static SemaphoreHandle_t s_lv_lock;

static esp_err_t init(void)
{
    /* Release the amp enable held low through power_off()'s deep sleep, and
     * keep the amp off until the speaker opens: no pop at boot. */
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(AMP_EN);
    const gpio_config_t amp = { .pin_bit_mask = BIT64(AMP_EN), .mode = GPIO_MODE_OUTPUT };
    ESP_RETURN_ON_ERROR(gpio_config(&amp), TAG, "amp enable");
    gpio_set_level(AMP_EN, 0);

    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c");
    esp_err_t err = i2c_master_probe(s_i2c, ES8311_ADDR, 50);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "ES8311 answers at 0x%02x", ES8311_ADDR);
    } else {
        ESP_LOGE(TAG, "ES8311 doesn't answer at 0x%02x (%s)", ES8311_ADDR, esp_err_to_name(err));
    }

    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_talk, TALK_GPIO), TAG, "talk button");
    /* A press held through boot isn't a new one. */
    s_talk.pressed = gpio_get_level(TALK_GPIO) == 0;
    return ESP_OK;
}

/* ---- The display nobody sees ---------------------------------------------- */

static uint32_t lv_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void discard(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    (void)area;
    (void)px;
    lv_display_flush_ready(disp);
}

static bool display_lock(int timeout_ms)
{
    TickType_t t = timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_lv_lock, t) == pdTRUE;
}

static void display_unlock(void)
{
    xSemaphoreGiveRecursive(s_lv_lock);
}

static void lv_task(void *arg)
{
    (void)arg;
    for (;;) {
        display_lock(-1);
        uint32_t next = lv_timer_handler();
        display_unlock();
        if (next < 5) {
            next = 5;
        } else if (next > 100) {
            next = 100;
        }
        vTaskDelay(pdMS_TO_TICKS(next));
    }
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    (void)touch;
    s_lv_lock = xSemaphoreCreateRecursiveMutex();
    if (!s_lv_lock) {
        return NULL;
    }
    lv_init();
    lv_tick_set_cb(lv_ms);
    lv_display_t *disp = lv_display_create(VIEW_W, VIEW_H);
    if (!disp) {
        return NULL;
    }
    size_t bytes = VIEW_W * VIEW_LINES * lv_color_format_get_size(lv_display_get_color_format(disp));
    /* PSRAM: these pixels go nowhere, so leave the internal RAM to the radios. */
    void *buf = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        return NULL;
    }
    lv_display_set_buffers(disp, buf, NULL, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, discard);
    /* Internal stack: settings handlers write NVS from LVGL's task. */
    if (xTaskCreatePinnedToCore(lv_task, "lvgl", 8192, NULL, MUSE_UI_PRIORITY, NULL, MUSE_UI_CORE) != pdPASS) {
        return NULL;
    }
    return disp;
}

static void set_brightness(int pct)
{
    (void)pct;   /* no screen */
}

/* ---- Audio ---------------------------------------------------------------- */

/* One ES8311 does both directions over a duplex I2S bus, clocked from MCLK. */
static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, &rx), TAG, "i2s channel");
    /* Muse records two slots; the mic is on the left one (mic_slot 0). */
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK,
            .bclk = I2S_BCLK,
            .ws = I2S_WS,
            .dout = I2S_DOUT,
            .din = I2S_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), TAG, "i2s tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std_cfg), TAG, "i2s rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "i2s tx on");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "i2s rx on");

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t i2c_cfg = { .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && ctrl_if && gpio_if, ESP_ERR_NO_MEM, TAG, "codec interfaces");

    /* The codec driver raises the amp enable as the speaker opens and drops
     * it as it closes, so the amp is off (no hiss) while Muse's audio rests. */
    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = AMP_EN,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *codec = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(codec, ESP_FAIL, TAG, "ES8311 not responding");

    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec, .data_if = data_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = codec, .data_if = data_if };
    *spk = s_spk = esp_codec_dev_new(&out_cfg);
    *mic = s_mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

/* ---- Buttons and power ---------------------------------------------------- */

static unsigned poll_buttons(void)
{
    return muse_gpio_button_poll(&s_talk);
}

/*
 * No battery and no power latch: power off is the codec closed, the amp held
 * off and deep sleep. GPIO41 isn't an RTC pad, so the talk button can't wake
 * an ESP32-S3 from deep sleep; the side reset button or replugging USB does.
 */
static esp_err_t power_off(void)
{
    if (s_spk) {
        esp_codec_dev_close(s_spk);
    }
    if (s_mic) {
        esp_codec_dev_close(s_mic);
    }
    gpio_set_level(AMP_EN, 0);
    gpio_hold_en(AMP_EN);
    gpio_deep_sleep_hold_en();
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "M5Stack Atom VoiceS3R",
    .width = VIEW_W,
    .height = VIEW_H,
    .round = false,
    .touch = false,
    .talk_button = "front",
    .wake_button = "reset",
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = display_unlock,
    .set_brightness = set_brightness,
    .audio_init = audio_init,
    .mic_slot = 0,              /* one mic, on the left slot */
    .poll_buttons = poll_buttons,
    .power_off = power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
