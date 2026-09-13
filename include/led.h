#ifndef LED_H
#define LED_H

#include <stdbool.h>

typedef enum {
    LED_OFF,
    LED_ON,
    LED_BLINK_SLOW,   // 500ms toggle
    LED_BLINK_FAST,   // 250ms toggle
    LED_ERROR,        // sticky fast blink — cannot be cleared by led_set_state()
} led_state_t;

// Set the LED state from any task (safe — uses a volatile shared variable).
// If the current state is LED_ERROR, only led_clear_error() can leave it.
void led_set_state(led_state_t state);

// Explicitly clear the sticky LED_ERROR state.
void led_clear_error(void);

// Call after cyw43_arch_init() so the LED task can touch the GPIO.
void led_set_cyw43_ready(void);

// Create the LED control task. Call once before vTaskStartScheduler().
void led_task_init(void);

#endif
