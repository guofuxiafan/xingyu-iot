#include "provisioning_button.h"
#include "wifi_cred_store.h"

#include "esp_log.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "provisioning_button";

/* Kconfig defaults (overridable via menuconfig) */
#ifndef CONFIG_EXAMPLE_PROVISION_BUTTON_GPIO
#define CONFIG_EXAMPLE_PROVISION_BUTTON_GPIO 35
#endif

#ifndef CONFIG_EXAMPLE_PROVISION_LONG_PRESS_MS
#define CONFIG_EXAMPLE_PROVISION_LONG_PRESS_MS 3000
#endif

#define POLL_INTERVAL_MS  100
#define REQUIRED_COUNT    (CONFIG_EXAMPLE_PROVISION_LONG_PRESS_MS / POLL_INTERVAL_MS)

static void provisioning_button_task(void *pvParameters)
{
    uint32_t press_count = 0;

    ESP_LOGI(TAG, "button task started on GPIO%d, long-press threshold %d ms (%d ticks)",
             CONFIG_EXAMPLE_PROVISION_BUTTON_GPIO,
             CONFIG_EXAMPLE_PROVISION_LONG_PRESS_MS,
             REQUIRED_COUNT);

    while (1) {
        int level = gpio_get_level(CONFIG_EXAMPLE_PROVISION_BUTTON_GPIO);

        /* GPIO35 (BOOT): internal 45k pull-up → LOW = pressed */
        if (level == 0) {
            press_count++;
            if (press_count >= REQUIRED_COUNT) {
                ESP_LOGI(TAG, "long-press detected (%" PRIu32 " ms), erasing NVS and restarting for provisioning mode",
                         press_count * POLL_INTERVAL_MS);
                wifi_cred_store_erase();
                esp_restart();
                /* never returns */
            }
        } else {
            press_count = 0;  /* reset on release */
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

esp_err_t provisioning_button_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << CONFIG_EXAMPLE_PROVISION_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }

    BaseType_t ret = xTaskCreate(provisioning_button_task, "prov_btn", 4096, NULL, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "initialized on GPIO%d", CONFIG_EXAMPLE_PROVISION_BUTTON_GPIO);
    return ESP_OK;
}
