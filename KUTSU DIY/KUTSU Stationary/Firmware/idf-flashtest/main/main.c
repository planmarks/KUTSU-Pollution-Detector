/* KUTSU Sense — ESP-IDF flash-test (ESP32-C5-DevKitC-1)
 * ---------------------------------------------------------------------------
 * NOT the real firmware — just a minimal, dependency-free blink to confirm that
 * Espressif IDE can build and flash this board (the Arduino-bundled esptool 5.1.0
 * kept dying on the C5 flash stub). It cycles the three status LEDs, so a good
 * flash also verifies the LED wiring:
 *     GPIO5  = green,  GPIO26 = yellow (strapping pin),  GPIO0 = red
 * Each LED: GPIO -> ~330 ohm -> LED -> GND (active-HIGH), same as WIRING.md.
 *
 * Once this flashes and blinks, we proceed with the full port.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define LED_GREEN   GPIO_NUM_5
#define LED_YELLOW  GPIO_NUM_26
#define LED_RED     GPIO_NUM_0

static const char *TAG = "kutsu_flashtest";

void app_main(void)
{
    const gpio_num_t leds[3] = { LED_GREEN, LED_YELLOW, LED_RED };

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << LED_GREEN) | (1ULL << LED_YELLOW) | (1ULL << LED_RED),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    ESP_LOGI(TAG, "KUTSU flash-test running: cycling G->Y->R on GPIO5/26/0");

    int i = 0;
    while (1) {
        for (int k = 0; k < 3; k++) {
            gpio_set_level(leds[k], 0);
        }
        gpio_set_level(leds[i], 1);
        ESP_LOGI(TAG, "LED %s on",
                 i == 0 ? "GREEN(GPIO5)" : (i == 1 ? "YELLOW(GPIO26)" : "RED(GPIO0)"));
        i = (i + 1) % 3;
        vTaskDelay(pdMS_TO_TICKS(400));
    }
}
