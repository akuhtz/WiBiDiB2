#ifndef LED_H
#define LED_H

#include <stdbool.h>

typedef enum {
    LED_OFF,
    LED_ON,
    LED_BLINK_SLOW,   // 500ms toggle
    LED_BLINK_FAST,   // 250ms toggle
} led_state_t;

// Set the LED state from any task (safe — uses a volatile shared variable).
void led_set_state(led_state_t state);

// Call after cyw43_arch_init() so the LED task can touch the GPIO.
void led_set_cyw43_ready(void);

// Create the LED control task. Call once before vTaskStartScheduler().
void led_task_init(void);

#endif
