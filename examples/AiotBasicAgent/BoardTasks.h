#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// BLE camera/display workers use the Core's internal task stacks.
inline BaseType_t aiotCreateBackgroundTask(TaskFunction_t entry, const char* name,
                                         uint32_t stackBytes, void* context,
                                         UBaseType_t priority, TaskHandle_t* task) {
  return xTaskCreate(entry, name, stackBytes, context, priority, task);
}
