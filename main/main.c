#include <stdio.h>
#include "my_led.h"
#include "moisture.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

const char *TAG = "watering_main";
double level = 0;

void app_main(void)
{
    led_strip_handle_t my_led;
    configure_led(&my_led);
    adc_oneshot_unit_handle_t adc_handle = config_adc(ADC_CHANNEL_4);
    while (1)
    {
        level = measure_moisture(&adc_handle, ADC_CHANNEL_4);
        inv_jet(&my_led, level, 0, 100, 0.3);
        vTaskDelay(CONFIG_MEASURE_PERIOD / portTICK_PERIOD_MS);
        
    }  
}
