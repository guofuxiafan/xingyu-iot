/*
 * Offline Chinese JSON-to-speech output for Waveshare ESP32-P4-WIFI6-DEV-KIT.
 *
 * ES8311 I2C: SDA GPIO7, SCL GPIO8 (shared with camera SCCB)
 * I2S: DOUT GPIO9, WS GPIO10, DIN GPIO11, BCLK GPIO12, MCLK GPIO13
 * NS4150B PA enable: GPIO53
 */

#include "voice_output.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_spiffs.h"
#include "esp_tts.h"
#include "esp_tts_voice_template.h"
#include "example_video_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_EXAMPLE_VOICE_OUTPUT_ENABLE

#define VOICE_I2S_PORT       I2S_NUM_1
#define VOICE_I2S_DOUT_GPIO  9
#define VOICE_I2S_WS_GPIO    10
#define VOICE_I2S_DIN_GPIO   11
#define VOICE_I2S_BCLK_GPIO  12
#define VOICE_I2S_MCLK_GPIO  13
#define VOICE_PA_GPIO        53
#define VOICE_SAMPLE_RATE    16000
#define VOICE_JSON_DIR       "/storage"
#define VOICE_JSON_MAX_BYTES (16 * 1024)
#define VOICE_MAX_FILES      32
#define VOICE_FILE_NAME_MAX  64
#define FNV1A_OFFSET_BASIS    UINT32_C(2166136261)
#define FNV1A_PRIME           UINT32_C(16777619)

static const char *TAG = "voice_output";
static i2s_chan_handle_t s_tx_chan;
static esp_tts_handle_t s_tts;
static esp_partition_mmap_handle_t s_voice_mmap;
static bool s_started;
static SemaphoreHandle_t s_tts_mutex;
static QueueHandle_t s_voice_queue;

typedef struct {
    bool active;
    bool seen;
    char name[VOICE_FILE_NAME_MAX];
    uint32_t fingerprint;
} voice_file_state_t;

static voice_file_state_t s_file_states[VOICE_MAX_FILES];

static esp_err_t init_i2s(void)
{
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(VOICE_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL),
                        TAG, "create I2S TX channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(VOICE_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = VOICE_I2S_MCLK_GPIO,
            .bclk = VOICE_I2S_BCLK_GPIO,
            .ws = VOICE_I2S_WS_GPIO,
            .dout = VOICE_I2S_DOUT_GPIO,
            .din = VOICE_I2S_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_chan, &std_cfg),
                        TAG, "configure I2S");
    return i2s_channel_enable(s_tx_chan);
}

static esp_err_t init_codec(void)
{
    i2c_master_bus_handle_t i2c_bus = example_video_get_i2c_bus();
    ESP_RETURN_ON_FALSE(i2c_bus, ESP_ERR_INVALID_STATE, TAG,
                        "shared camera/codec I2C bus is unavailable");

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_0,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(ctrl_if, ESP_ERR_NO_MEM, TAG, "create codec I2C control");

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = VOICE_I2S_PORT,
        .tx_handle = s_tx_chan,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(data_if, ESP_ERR_NO_MEM, TAG, "create codec I2S data");

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(gpio_if, ESP_ERR_NO_MEM, TAG, "create codec GPIO control");

    es8311_codec_cfg_t codec_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .master_mode = false,
        .use_mclk = true,
        .pa_pin = VOICE_PA_GPIO,
        .pa_reverted = false,
        .hw_gain = {
            .pa_voltage = 5.0,
            .codec_dac_voltage = 3.3,
        },
        .mclk_div = I2S_MCLK_MULTIPLE_256,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&codec_cfg);
    ESP_RETURN_ON_FALSE(codec_if, ESP_ERR_NO_MEM, TAG, "create ES8311 codec");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    esp_codec_dev_handle_t codec = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(codec, ESP_ERR_NO_MEM, TAG, "create codec device");

    esp_codec_dev_sample_info_t sample_cfg = {
        .bits_per_sample = 16,
        .channel = 2,
        .channel_mask = 0x03,
        .sample_rate = VOICE_SAMPLE_RATE,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(codec, &sample_cfg) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "open ES8311");
    ESP_RETURN_ON_FALSE(
        esp_codec_dev_set_out_vol(codec, CONFIG_EXAMPLE_VOICE_OUTPUT_VOLUME) ==
            ESP_CODEC_DEV_OK,
        ESP_FAIL, TAG, "set speaker volume");
    return ESP_OK;
}

static esp_err_t init_tts(void)
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "voice_data");
    ESP_RETURN_ON_FALSE(partition, ESP_ERR_NOT_FOUND, TAG,
                        "voice_data partition not found; use 'idf.py flash'");

    const void *voice_data = NULL;
    ESP_RETURN_ON_ERROR(
        esp_partition_mmap(partition, 0, partition->size,
                           ESP_PARTITION_MMAP_DATA, &voice_data, &s_voice_mmap),
        TAG, "map voice data");

    esp_tts_voice_t *voice =
        esp_tts_voice_set_init(&esp_tts_voice_template, (int16_t *)voice_data);
    ESP_RETURN_ON_FALSE(voice, ESP_FAIL, TAG, "initialize TTS voice");
    s_tts = esp_tts_create(voice);
    ESP_RETURN_ON_FALSE(s_tts, ESP_ERR_NO_MEM, TAG, "create TTS engine");
    return ESP_OK;
}

static esp_err_t play_mono_pcm(const int16_t *mono, size_t samples)
{
    int16_t stereo[512 * 2];
    while (samples > 0) {
        size_t count = samples > 512 ? 512 : samples;
        for (size_t i = 0; i < count; ++i) {
            stereo[i * 2] = mono[i];
            stereo[i * 2 + 1] = mono[i];
        }
        size_t written = 0;
        size_t bytes = count * 2 * sizeof(int16_t);
        ESP_RETURN_ON_ERROR(
            i2s_channel_write(s_tx_chan, stereo, bytes, &written, portMAX_DELAY),
            TAG, "write speaker PCM");
        ESP_RETURN_ON_FALSE(written == bytes, ESP_FAIL, TAG, "short I2S write");
        mono += count;
        samples -= count;
    }
    return ESP_OK;
}

static esp_err_t speak_text(const char *text)
{
    if (!text || text[0] == '\0') {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "speaking: %s", text);
    ESP_RETURN_ON_FALSE(esp_tts_parse_chinese(s_tts, (char *)text),
                        ESP_ERR_INVALID_ARG, TAG, "TTS cannot parse text");

    esp_err_t ret = ESP_OK;
    int samples = 0;
    do {
        int16_t *pcm = esp_tts_stream_play(
            s_tts, &samples, CONFIG_EXAMPLE_VOICE_OUTPUT_SPEED);
        if (samples > 0) {
            ret = play_mono_pcm(pcm, (size_t)samples);
            if (ret != ESP_OK) {
                break;
            }
        }
    } while (samples > 0);
    esp_tts_stream_reset(s_tts);
    return ret;
}

static esp_err_t speak_json_value(const cJSON *value)
{
    if (cJSON_IsString(value)) {
        return speak_text(value->valuestring);
    }
    if (cJSON_IsArray(value)) {
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, value) {
            ESP_RETURN_ON_ERROR(speak_json_value(item), TAG,
                                "speak JSON array item");
        }
        return ESP_OK;
    }
    if (cJSON_IsObject(value)) {
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(value, "text");
        const cJSON *items = cJSON_GetObjectItemCaseSensitive(value, "items");
        if (cJSON_IsString(text)) {
            ESP_RETURN_ON_ERROR(speak_text(text->valuestring), TAG,
                                "speak JSON text");
        }
        if (items) {
            ESP_RETURN_ON_ERROR(speak_json_value(items), TAG,
                                "speak JSON items");
        }
        return (cJSON_IsString(text) || items) ? ESP_OK : ESP_ERR_INVALID_ARG;
    }
    return ESP_ERR_INVALID_ARG;
}

static bool has_json_extension(const char *name)
{
    size_t length = strlen(name);
    return length > 5 && strcmp(name + length - 5, ".json") == 0;
}

static uint32_t fingerprint_bytes(const char *data, size_t length)
{
    uint32_t hash = FNV1A_OFFSET_BASIS;
    for (size_t i = 0; i < length; ++i) {
        hash ^= (uint8_t)data[i];
        hash *= FNV1A_PRIME;
    }
    return hash;
}

static voice_file_state_t *find_file_state(const char *name)
{
    voice_file_state_t *free_slot = NULL;
    for (size_t i = 0; i < VOICE_MAX_FILES; ++i) {
        if (s_file_states[i].active &&
            strcmp(s_file_states[i].name, name) == 0) {
            return &s_file_states[i];
        }
        if (!s_file_states[i].active && !free_slot) {
            free_slot = &s_file_states[i];
        }
    }
    return free_slot;
}

static esp_err_t load_json_file(const char *path, char **json_out,
                                uint32_t *fingerprint_out)
{
    FILE *file = fopen(path, "rb");
    ESP_RETURN_ON_FALSE(file, ESP_ERR_NOT_FOUND, TAG, "cannot open %s", path);
    ESP_RETURN_ON_FALSE(fseek(file, 0, SEEK_END) == 0, ESP_FAIL, TAG,
                        "seek JSON end: %s", path);
    long length = ftell(file);
    ESP_RETURN_ON_FALSE(length > 0 && length <= VOICE_JSON_MAX_BYTES,
                        ESP_ERR_INVALID_SIZE, TAG,
                        "JSON size must be 1..16384: %s", path);
    rewind(file);

    char *json = malloc((size_t)length + 1);
    if (!json) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    size_t read = fread(json, 1, (size_t)length, file);
    fclose(file);
    json[read] = '\0';
    if (read != (size_t)length) {
        free(json);
        return ESP_FAIL;
    }

    *fingerprint_out = fingerprint_bytes(json, read);
    *json_out = json;
    return ESP_OK;
}

static esp_err_t process_json_file(const char *name)
{
    char path[sizeof(VOICE_JSON_DIR) + VOICE_FILE_NAME_MAX + 1];
    int path_len = snprintf(path, sizeof(path), "%s/%s", VOICE_JSON_DIR, name);
    ESP_RETURN_ON_FALSE(path_len > 0 && path_len < (int)sizeof(path),
                        ESP_ERR_INVALID_SIZE, TAG, "JSON path is too long: %s",
                        name);

    char *json = NULL;
    uint32_t fingerprint = 0;
    ESP_RETURN_ON_ERROR(load_json_file(path, &json, &fingerprint), TAG,
                        "load JSON file");

    voice_file_state_t *state = find_file_state(name);
    if (!state) {
        free(json);
        ESP_LOGW(TAG, "tracking limit reached; ignoring %s", name);
        return ESP_ERR_NO_MEM;
    }
    state->seen = true;
    if (state->active && state->fingerprint == fingerprint) {
        free(json);
        return ESP_OK;
    }

    /*
     * Remember this version before parsing. A partially written/invalid file
     * will not spam the log on every poll, but it is retried when it changes.
     */
    state->active = true;
    state->fingerprint = fingerprint;
    strlcpy(state->name, name, sizeof(state->name));

    cJSON *root = cJSON_Parse(json);
    free(json);
    ESP_RETURN_ON_FALSE(root, ESP_ERR_INVALID_ARG, TAG, "invalid JSON: %s",
                        path);
    ESP_LOGI(TAG, "new or updated JSON detected: %s", path);
    xSemaphoreTake(s_tts_mutex, portMAX_DELAY);
    esp_err_t ret = speak_json_value(root);
    xSemaphoreGive(s_tts_mutex);
    cJSON_Delete(root);
    return ret;
}

static esp_err_t poll_json_directory(void)
{
    for (size_t i = 0; i < VOICE_MAX_FILES; ++i) {
        if (s_file_states[i].active) {
            s_file_states[i].seen = false;
        }
    }

    DIR *dir = opendir(VOICE_JSON_DIR);
    ESP_RETURN_ON_FALSE(dir, ESP_ERR_NOT_FOUND, TAG,
                        "cannot open JSON directory " VOICE_JSON_DIR);

    esp_err_t first_error = ESP_OK;
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (!has_json_extension(entry->d_name)) {
            continue;
        }
        esp_err_t ret = process_json_file(entry->d_name);
        if (ret != ESP_OK && first_error == ESP_OK) {
            first_error = ret;
        }
    }
    closedir(dir);

    /* Recreating a removed file with the same content must count as new. */
    for (size_t i = 0; i < VOICE_MAX_FILES; ++i) {
        if (s_file_states[i].active && !s_file_states[i].seen) {
            memset(&s_file_states[i], 0, sizeof(s_file_states[i]));
        }
    }
    return first_error;
}

static void voice_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "watching %s for JSON files every %d ms", VOICE_JSON_DIR,
             CONFIG_EXAMPLE_VOICE_OUTPUT_POLL_INTERVAL_MS);
    while (true) {
        char *json = NULL;
        while (xQueueReceive(s_voice_queue, &json, 0) == pdTRUE) {
            cJSON *root = cJSON_Parse(json);
            free(json);
            if (root) {
                xSemaphoreTake(s_tts_mutex, portMAX_DELAY);
                speak_json_value(root);
                xSemaphoreGive(s_tts_mutex);
                cJSON_Delete(root);
            }
        }

        esp_err_t ret = poll_json_directory();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "JSON directory poll failed: %s",
                     esp_err_to_name(ret));
        }
        vTaskDelay(pdMS_TO_TICKS(
            CONFIG_EXAMPLE_VOICE_OUTPUT_POLL_INTERVAL_MS));
    }
}

esp_err_t voice_output_speak_json(const char *json_str)
{
    if (!s_started || !json_str) {
        return ESP_ERR_INVALID_STATE;
    }
    char *copy = strdup(json_str);
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    if (xQueueSend(s_voice_queue, &copy, 0) != pdTRUE) {
        free(copy);
        ESP_LOGW(TAG, "voice queue full, dropping JSON");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t voice_output_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    s_tts_mutex = xSemaphoreCreateMutex();
    s_voice_queue = xQueueCreate(8, sizeof(char *));

    esp_vfs_spiffs_conf_t fs_cfg = {
        .base_path = "/storage",
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = false,
    };
    ESP_RETURN_ON_ERROR(esp_vfs_spiffs_register(&fs_cfg), TAG,
                        "mount local JSON storage");
    ESP_RETURN_ON_ERROR(init_i2s(), TAG, "initialize speaker I2S");
    ESP_RETURN_ON_ERROR(init_codec(), TAG, "initialize ES8311/NS4150B");
    ESP_RETURN_ON_ERROR(init_tts(), TAG, "initialize offline TTS");

    BaseType_t created = xTaskCreate(voice_task, "voice_json", 8192, NULL, 4, NULL);
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "create voice task");
    s_started = true;
    return ESP_OK;
}

#else

esp_err_t voice_output_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t voice_output_speak_json(const char *json_str)
{
    (void)json_str;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
