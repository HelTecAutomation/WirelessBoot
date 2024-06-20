
#include "esp_log.h"
#include "hal/gpio_hal.h"

/* Function used to tell the linker to include this file
 * with all its symbols.
 */
void bootloader_hooks_include(void){
}


void bootloader_before_init(void) {
    /* Keep in my mind that a lot of functions cannot be called from here
     * as system initialization has not been performed yet, including
     * BSS, SPI flash, or memory protection. */
    ESP_LOGI("HOOK", "This hook is called BEFORE bootloader initialization");
}

void bootloader_after_init(void) {
    // gpio_set_direction(LED_1_PIN, GPIO_MODE_OUTPUT);
    // gpio_set_direction(LED_2_PIN, GPIO_MODE_OUTPUT);
    // // gpio_set_level(LED_1_PIN, 1);
    // gpio_set_level(LED_2_PIN, 1);
    gpio_ll_output_enable (&GPIO, GPIO_NUM_33);
    gpio_ll_set_level (&GPIO, GPIO_NUM_33, 1);
    ESP_LOGI("HOOK", "This hook is called AFTER bootloader initialization");
}
