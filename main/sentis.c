#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"

void app_main(void)
{
    printf("¡Hola, mundo desde ESP-IDF!\n");

    for (int i = 10; i >= 0; i--) {
        printf("Reiniciando en %d segundos...\n", i);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
    printf("Reiniciando ahora.\n");
    fflush(stdout);
    esp_restart();
}
