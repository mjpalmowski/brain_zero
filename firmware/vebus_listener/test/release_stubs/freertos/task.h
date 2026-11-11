#pragma once
#include "FreeRTOS.h"
BaseType_t xTaskCreatePinnedToCore(void (*task)(void *), const char *, unsigned, void *, unsigned, void *, int);
void vTaskDelete(void *);
