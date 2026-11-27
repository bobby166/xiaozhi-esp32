#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "cJSON.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"

// ================= अपनी सेटिंग्स यहाँ भरें =================
#define WIFI_SSID      "Bobby baghel"
#define WIFI_PASS      "11111111"
#define GROQ_API_KEY   "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"
#define NVIDIA_API_KEY "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"
// =========================================================

// Speed-First Fallback Order (सबसे तेज़ मॉडल्स सबसे ऊपर)
static const char* NVIDIA_MODELS[] = {
    "nvidia/nemotron-3.5-bolt",
    "nvidia/nemotron-3.5-lightning",
    "meta/llama-3.3-70b-instruct",
    "laguna/laguna-xs-2.1",
    "nvidia/nemotron-3-nano-omni",
    "moonshotai/kimi-k2.6",
    "minimaxai/minimax-m3",
    "minimaxai/minimax-m2.7",
    "nvidia/nemotron-3-ultra"
};
#define NUM_NVIDIA_MODELS (sizeof(NVIDIA_MODELS) / sizeof(NVIDIA_MODELS[0]))

// हार्डवेयर पिन्स (Mic: D25, D26, D32 | Speaker: D33, D14, D27 | SD: 3.3V)
#define MIC_WS_PIN     GPIO_NUM_25
#define MIC_SCK_PIN    GPIO_NUM_26
#define MIC_SD_PIN     GPIO_NUM_32

#define SPK_DOUT_PIN   GPIO_NUM_33
#define SPK_BCLK_PIN   GPIO_NUM_14
#define SPK_LRC_PIN    GPIO_NUM_27

#define BOOT_BTN_PIN   GPIO_NUM_0
#define LED_PIN        GPIO_NUM_2

#define REC_SAMPLE_RATE 8000
#define TTS_SAMPLE_RATE 24000
#define MAX_REC_SECONDS 10
#define WAV_HEADER_LEN  44

#define IDLE_VOICE_THRESHOLD     650
#define SPEAKING_BARGE_THRESHOLD 2600
#define SILENCE_STOP_MS          850

static const char *TAG = "MOTO_FAST";
static i2s_chan_handle_t rx_handle = NULL;
static i2s_chan_handle_t tx_handle = NULL;
static char http_resp_buf[8192];
static int http_resp_len = 0;

static volatile bool session_active = false;
static volatile bool is_speaking = false;
static volatile bool barge_in_triggered = false;

esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (http_resp_len + evt->data_len < (int)sizeof(http_resp_buf) - 1) {
            memcpy(http_resp_buf + http_resp_len, evt->data, evt->data_len);
            http_resp_len += evt->data_len;
            http_resp_buf[http_resp_len] = 0;
        }
    }
    return ESP_OK;
}

void url_encode(const char *src, char *dest, size_t max_len) {
    const char *hex = "0123456789ABCDEF";
    size_t pos = 0;
    for (size_t i = 0; src[i] != '\0' && pos + 4 < max_len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            dest[pos++] = c;
        } else if (c == ' ') {
            dest[pos++] = '+';
        } else {
            dest[pos++] = '%';
            dest[pos++] = hex[c >> 4];
            dest[pos++] = hex[c & 15];
        }
    }
    dest[pos] = '\0';
}

static void wifi_init(void) {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    wifi_config_t wifi_config = {};
    strcpy((char *)wifi_config.sta.ssid, WIFI_SSID);
    strcpy((char *)wifi_config.sta.password, WIFI_PASS);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
    esp_wifi_connect();
}

static void audio_init(void) {
    i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    i2s_new_channel(&tx_chan_cfg, &tx_handle, NULL);
    i2s_std_config_t tx_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(TTS_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPK_BCLK_PIN,
            .ws = SPK_LRC_PIN,
            .dout = SPK_DOUT_PIN,
            .din = I2S_GPIO_UNUSED,
        },
    };
    i2s_channel_init_std_mode(tx_handle, &tx_std_cfg);
    i2s_channel_enable(tx_handle);

    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    i2s_new_channel(&rx_chan_cfg, NULL, &rx_handle);
    i2s_std_config_t rx_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(REC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_SCK_PIN,
            .ws = MIC_WS_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din = MIC_SD_PIN,
        },
    };
    i2s_channel_init_std_mode(rx_handle, &rx_std_cfg);
    i2s_channel_enable(rx_handle);
}

void write_wav_header(uint8_t *header, uint32_t data_size) {
    uint32_t file_size = data_size + 36;
    uint32_t byte_rate = REC_SAMPLE_RATE * 2;
    memcpy(header, "RIFF", 4);
    memcpy(header + 4, &file_size, 4);
    memcpy(header + 8, "WAVEfmt ", 8);
    uint32_t subchunk1 = 16;
    uint16_t audio_fmt = 1, num_ch = 1, block_align = 2, bits = 16;
    uint32_t sr = REC_SAMPLE_RATE;
    memcpy(header + 16, &subchunk1, 4);
    memcpy(header + 20, &audio_fmt, 2);
    memcpy(header + 22, &num_ch, 2);
    memcpy(header + 24, &sr, 4);
    memcpy(header + 28, &byte_rate, 4);
    memcpy(header + 32, &block_align, 2);
    memcpy(header + 34, &bits, 2);
    memcpy(header + 36, "data", 4);
    memcpy(header + 40, &data_size, 4);
}

int32_t calc_energy(int16_t *samples, int count) {
    int64_t sum = 0;
    for (int i = 0; i < count; i++) {
        sum += abs(samples[i]);
    }
    return (count > 0) ? (int32_t)(sum / count) : 0;
}

char* speech_to_text(uint8_t *wav_buf, size_t wav_len) {
    http_resp_len = 0;
    memset(http_resp_buf, 0, sizeof(http_resp_buf));
    const char *boundary = "----ESP32Boundary7MA4YWxkTrZu0gW";
    char head[512];
    int head_len = snprintf(head, sizeof(head),
        "--%s\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\nwhisper-large-v3-turbo\r\n"
        "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\nContent-Type: audio/wav\r\n\r\n",
        boundary, boundary);
    char tail[128];
    int tail_len = snprintf(tail, sizeof(tail), "\r\n--%s--\r\n", boundary);

    char content_type[128];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);
    char auth[160];
    snprintf(auth, sizeof(auth), "Bearer %s", GROQ_API_KEY);

    esp_http_client_config_t config = {};
    config.url = "https://api.groq.com/openai/v1/audio/transcriptions";
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 12000;
    config.event_handler = http_event_handler;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Content-Type", content_type);

    if (esp_http_client_open(client, head_len + wav_len + tail_len) == ESP_OK) {
        esp_http_client_write(client, head, head_len);
        esp_http_client_write(client, (const char*)wav_buf, wav_len);
        esp_http_client_write(client, tail, tail_len);
        esp_http_client_fetch_headers(client);
        esp_http_client_read_response(client, http_resp_buf, sizeof(http_resp_buf) - 1);
    }
    esp_http_client_cleanup(client);

    cJSON *json = cJSON_Parse(http_resp_buf);
    if (!json) return NULL;
    cJSON *text = cJSON_GetObjectItem(json, "text");
    char *result = (text && text->valuestring) ? strdup(text->valuestring) : NULL;
    cJSON_Delete(json);
    return result;
}

char* call_single_nvidia_model(const char *model_name, const char *user_text) {
    http_resp_len = 0;
    memset(http_resp_buf, 0, sizeof(http_resp_buf));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", model_name);
    cJSON_AddNumberToObject(root, "max_tokens", 80);
    cJSON_AddNumberToObject(root, "temperature", 0.6);
    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");

    cJSON *sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content", "Tum Moto ho, Bobby ki fast Hinglish AI assistant. Seedha aur chhota jawab 1-2 line me do.");
    cJSON_AddItemToArray(msgs, sys_msg);

    cJSON *usr_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(usr_msg, "role", "user");
    cJSON_AddStringToObject(usr_msg, "content", user_text);
    cJSON_AddItemToArray(msgs, usr_msg);

    char *post_data = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    char auth[180];
    snprintf(auth, sizeof(auth), "Bearer %s", NVIDIA_API_KEY);

    esp_http_client_config_t config = {};
    config.url = "https://integrate.api.nvidia.com/v1/chat/completions";
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 8000;
    config.event_handler = http_event_handler;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(post_data);

    if (err != ESP_OK || status_code != 200) {
        ESP_LOGW(TAG, "Model %s failed (HTTP %d), next fast fallback...", model_name, status_code);
        return NULL;
    }

    cJSON *json = cJSON_Parse(http_resp_buf);
    if (!json) return NULL;
    cJSON *choices = cJSON_GetObjectItem(json, "choices");
    cJSON *first = cJSON_GetArrayItem(choices, 0);
    cJSON *message = first ? cJSON_GetObjectItem(first, "message") : NULL;
    cJSON *content = message ? cJSON_GetObjectItem(message, "content") : NULL;
    char *reply = (content && content->valuestring && strlen(content->valuestring) > 0)
                  ? strdup(content->valuestring) : NULL;
    cJSON_Delete(json);
    return reply;
}

char* ask_nvidia_llm(const char *user_text) {
    for (int i = 0; i < (int)NUM_NVIDIA_MODELS; i++) {
        ESP_LOGI(TAG, "Trying fast model [%d/%d]: %s", i + 1, (int)NUM_NVIDIA_MODELS, NVIDIA_MODELS[i]);
        char *reply = call_single_nvidia_model(NVIDIA_MODELS[i], user_text);
        if (reply != NULL) {
            ESP_LOGI(TAG, "Fast reply from: %s", NVIDIA_MODELS[i]);
            return reply;
        }
    }
    return NULL;
}

void speak_tts(const char *text, bool allow_barge_in) {
    char encoded[600];
    url_encode(text, encoded, sizeof(encoded));

    char url[750];
    snprintf(url, sizeof(url),
             "https://translate.google.com/translate_tts?ie=UTF-8&q=%s&tl=hi&client=tw-ob",
             encoded);

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = 10000;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "User-Agent", "Mozilla/5.0");

    if (esp_http_client_open(client, 0) != ESP_OK) {
        esp_http_client_cleanup(client);
        return;
    }
    esp_http_client_fetch_headers(client);

    esp_audio_simple_dec_register_default();
    esp_audio_simple_dec_cfg_t dec_cfg = {};
    dec_cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    esp_audio_simple_dec_handle_t dec = NULL;
    esp_audio_simple_dec_open(&dec_cfg, &dec);

    uint8_t *mp3_buf = (uint8_t *)malloc(2048);
    uint8_t *pcm_buf = (uint8_t *)malloc(4096);
    int32_t mic_probe[160];
    int16_t mic_pcm[160];
    int loud_hits = 0;

    is_speaking = true;
    barge_in_triggered = false;

    while (!barge_in_triggered) {
        int r = esp_http_client_read(client, (char *)mp3_buf, 2048);
        if (r <= 0) break;

        esp_audio_simple_dec_raw_t raw = {};
        raw.buffer = mp3_buf;
        raw.len = (uint32_t)r;

        while (raw.len > 0 && !barge_in_triggered) {
            esp_audio_simple_dec_out_t out = {};
            out.buffer = pcm_buf;
            out.len = 4096;

            esp_audio_err_t ret = esp_audio_simple_dec_process(dec, &raw, &out);
            if (ret == ESP_AUDIO_ERR_OK && out.decoded_size > 0) {
                int16_t *spk_samples = (int16_t *)pcm_buf;
                int spk_count = out.decoded_size / 2;
                for (int i = 0; i < spk_count; i++) {
                    spk_samples[i] = (spk_samples[i] * 3) / 4;
                }
                size_t written = 0;
                i2s_channel_write(tx_handle, pcm_buf, out.decoded_size, &written, portMAX_DELAY);

                if (allow_barge_in) {
                    size_t m_bytes = 0;
                    if (i2s_channel_read(rx_handle, mic_probe, sizeof(mic_probe), &m_bytes, 0) == ESP_OK && m_bytes > 0) {
                        int mc = m_bytes / 4;
                        for (int i = 0; i < mc; i++) mic_pcm[i] = (int16_t)(mic_probe[i] >> 14);
                        if (calc_energy(mic_pcm, mc) > SPEAKING_BARGE_THRESHOLD) {
                            loud_hits++;
                            if (loud_hits >= 3) {
                                barge_in_triggered = true;
                            }
                        } else {
                            loud_hits = 0;
                        }
                    }
                }
            }
            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;
            if (raw.consumed == 0) break;
        }
    }

    if (barge_in_triggered) {
        i2s_channel_disable(tx_handle);
        i2s_channel_enable(tx_handle);
    }

    is_speaking = false;
    free(mp3_buf);
    free(pcm_buf);
    if (dec) esp_audio_simple_dec_close(dec);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

extern "C" void app_main(void) {
    nvs_flash_init();
    gpio_set_direction(BOOT_BTN_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOOT_BTN_PIN, GPIO_PULLUP_ONLY);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);

    wifi_init();
    audio_init();

    const size_t max_samples = REC_SAMPLE_RATE * MAX_REC_SECONDS;
    const size_t max_pcm_bytes = max_samples * 2;
    uint8_t *wav_buf = (uint8_t *)malloc(WAV_HEADER_LEN + max_pcm_bytes);
    int32_t *raw_i2s = (int32_t *)malloc(400 * sizeof(int32_t));
    int16_t *chunk_pcm = (int16_t *)malloc(400 * sizeof(int16_t));

    vTaskDelay(pdMS_TO_TICKS(3500));
    speak_tts("Namaste Bobby, button dabakar baat shuru karo.", false);

    while (1) {
        if (gpio_get_level(BOOT_BTN_PIN) == 0) {
            vTaskDelay(pdMS_TO_TICKS(200));
            session_active = !session_active;
            if (session_active) {
                gpio_set_level(LED_PIN, 1);
                speak_tts("Haan bolo.", false);
            } else {
                gpio_set_level(LED_PIN, 0);
                speak_tts("Band kar diya.", false);
            }
        }

        if (session_active) {
            size_t bytes_read = 0;
            i2s_channel_read(rx_handle, raw_i2s, 400 * sizeof(int32_t), &bytes_read, portMAX_DELAY);
            int count = bytes_read / 4;
            for (int i = 0; i < count; i++) {
                chunk_pcm[i] = (int16_t)(raw_i2s[i] >> 14);
            }

            if (barge_in_triggered || calc_energy(chunk_pcm, count) > IDLE_VOICE_THRESHOLD) {
                barge_in_triggered = false;
                int16_t *pcm_out = (int16_t *)(wav_buf + WAV_HEADER_LEN);
                size_t samples_read = 0;
                int silence_ms = 0;

                memcpy(pcm_out, chunk_pcm, count * sizeof(int16_t));
                samples_read += count;

                while (samples_read < max_samples && silence_ms < SILENCE_STOP_MS) {
                    if (gpio_get_level(BOOT_BTN_PIN) == 0) {
                        session_active = false;
                        gpio_set_level(LED_PIN, 0);
                        break;
                    }

                    i2s_channel_read(rx_handle, raw_i2s, 400 * sizeof(int32_t), &bytes_read, portMAX_DELAY);
                    int c = bytes_read / 4;
                    for (int i = 0; i < c && samples_read < max_samples; i++) {
                        int16_t s = (int16_t)(raw_i2s[i] >> 14);
                        chunk_pcm[i] = s;
                        pcm_out[samples_read++] = s;
                    }

                    if (calc_energy(chunk_pcm, c) < IDLE_VOICE_THRESHOLD) {
                        silence_ms += 50;
                    } else {
                        silence_ms = 0;
                    }
                }

                if (session_active && samples_read > (REC_SAMPLE_RATE / 2)) {
                    gpio_set_level(LED_PIN, 0);
                    size_t actual_pcm_bytes = samples_read * 2;
                    write_wav_header(wav_buf, actual_pcm_bytes);

                    char *user_text = speech_to_text(wav_buf, WAV_HEADER_LEN + actual_pcm_bytes);
                    if (user_text && strlen(user_text) > 1) {
                        ESP_LOGI(TAG, "User: %s", user_text);
                        char *ai_reply = ask_nvidia_llm(user_text);
                        if (ai_reply) {
                            ESP_LOGI(TAG, "Moto: %s", ai_reply);
                            speak_tts(ai_reply, true);
                            free(ai_reply);
                        } else {
                            speak_tts("Server vyast hai, phir bolo.", false);
                        }
                        free(user_text);
                    }
                    if (session_active) {
                        gpio_set_level(LED_PIN, 1);
                    }
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    }
}
