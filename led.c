#include "led.h"
#include "pico/cyw43_arch.h"
#include "FreeRTOS.h"
#include "task.h"

volatile led_state_t g_led_state = LED_OFF;
static volatile bool g_cyw43_ready = false;

void led_set_state(led_state_t state) {
    // Sticky error: only led_clear_error() can leave LED_ERROR.
    if (g_led_state == LED_ERROR && state != LED_ERROR) return;
    g_led_state = state;
}

void led_clear_error(void) {
    if (g_led_state == LED_ERROR) g_led_state = LED_OFF;
}

void led_set_cyw43_ready(void) {
    g_cyw43_ready = true;
}

static void led_task(void *param) {
    (void)param;
    bool led = false;

    for (;;) {
        if (!g_cyw43_ready) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        switch (g_led_state) {
        case LED_OFF:
            led = false;
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
            vTaskDelay(pdMS_TO_TICKS(100));
            break;
        case LED_ON:
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, true);
            vTaskDelay(pdMS_TO_TICKS(100));
            break;
        case LED_BLINK_SLOW:
            led = !led;
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led);
            vTaskDelay(pdMS_TO_TICKS(500));
            break;
        case LED_BLINK_FAST:
            led = !led;
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led);
            vTaskDelay(pdMS_TO_TICKS(250));
            break;
        case LED_ERROR:
            // Same waveform as fast blink, but sticky (see led_set_state).
            led = !led;
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led);
            vTaskDelay(pdMS_TO_TICKS(250));
            break;
        }
    }
}

void led_task_init(void) {
    xTaskCreate(led_task, "led", 256, NULL, 1, NULL);
}
