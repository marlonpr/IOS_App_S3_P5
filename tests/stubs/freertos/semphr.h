#pragma once

#include "freertos/FreeRTOS.h"

typedef struct {
    int unused;
} StaticSemaphore_t;

typedef StaticSemaphore_t *SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(
    StaticSemaphore_t *storage)
{
    return storage;
}

static inline int xSemaphoreTake(SemaphoreHandle_t semaphore,
                                 TickType_t timeout)
{
    (void)timeout;
    return semaphore != nullptr ? pdTRUE : pdFALSE;
}

static inline int xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    return semaphore != nullptr ? pdTRUE : pdFALSE;
}
