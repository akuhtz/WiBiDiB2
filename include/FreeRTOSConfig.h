/*
 * FreeRTOSConfig.h  —  WiBiDiB2 Pico 2W
 *
 * FreeRTOS kernel configuration for RP2350 (Cortex-M33).
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* RP2350 has 4 NVIC priority bits */
#define configPRIO_BITS    4

/* Assert handler */
#define configASSERT(x) do { if (!(x)) { portDISABLE_INTERRUPTS(); for(;;); } } while(0)

/* Cortex-M33 port configuration */
#define configENABLE_MPU                        0
#define configENABLE_FPU                        1
#define configENABLE_TRUSTZONE                  0
#define configMINIMAL_SECURE_STACK_SIZE         ( 1024 )
#define configRUN_FREERTOS_SECURE_ONLY          1

#define configUSE_PREEMPTION                    1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0
#define configCPU_CLOCK_HZ                      150000000
#define configTICK_RATE_HZ                      ( 1000 )
#define configMAX_PRIORITIES                    ( 5 )
#define configMINIMAL_STACK_SIZE                ( 256 )
#define configTOTAL_HEAP_SIZE                   ( 32 * 1024 )
#define configMAX_TASK_NAME_LEN                 16
#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             1
#define configUSE_COUNTING_SEMAPHORES           1
#define configQUEUE_REGISTRY_SIZE               0
#define configUSE_QUEUE_SETS                    0
#define configUSE_TIME_SLICING                  0
#define configUSE_NEWLIB_REENTRANT              0
#define configENABLE_BACKWARD_COMPATIBILITY     1
#define configSTACK_ALLOCATION_FROM_SEPARATE_HEAP 0

#define configSUPPORT_STATIC_ALLOCATION         0
#define configSUPPORT_DYNAMIC_ALLOCATION        1

/* Pico SDK FreeRTOS interop */
#define configSUPPORT_PICO_SYNC_INTEROP         1
#define configSUPPORT_PICO_TIME_INTEROP         1

/* Hook function related definitions */
#define configUSE_IDLE_HOOK                    0
#define configUSE_TICK_HOOK                    0
#define configUSE_MALLOC_FAILED_HOOK           0
#define configCHECK_FOR_STACK_OVERFLOW         2
#define configCHECK_HANDLER_INSTALLATION       0

/* Run time and task stats */
#define configGENERATE_RUN_TIME_STATS          0
#define configUSE_TRACE_FACILITY               0
#define configUSE_STATS_FORMATTING_FUNCTIONS   0

/* Co-routine definitions */
#define configUSE_CO_ROUTINES                  0
#define configMAX_CO_ROUTINE_PRIORITIES        2

/* Software timer related definitions */
#define configUSE_TIMERS                       1
#define configTIMER_TASK_PRIORITY              (configMAX_PRIORITIES-2)
#define configTIMER_QUEUE_LENGTH               32
#define configTIMER_TASK_STACK_DEPTH           configMINIMAL_STACK_SIZE

/* Optional functions */
#define INCLUDE_vTaskPrioritySet               0
#define INCLUDE_uxTaskPriorityGet              0
#define INCLUDE_vTaskDelete                    1
#define INCLUDE_vTaskSuspend                   1
#define INCLUDE_xResumeFromISR                 0
#define INCLUDE_vTaskDelayUntil                1
#define INCLUDE_vTaskDelay                     1
#define INCLUDE_xTaskGetSchedulerState         1
#define INCLUDE_xTaskGetCurrentTaskHandle      1
#define INCLUDE_uxTaskGetStackHighWaterMark    1
#define INCLUDE_xTaskGetIdleTaskHandle         0
#define INCLUDE_xTimerGetTimerDaemonTaskHandle 0
#define INCLUDE_pcTaskGetTaskName              1
#define INCLUDE_eTaskGetState                  1
#define INCLUDE_xEventGroupSetBitFromISR       1
#define INCLUDE_xTimerPendFunctionCall         1
#define INCLUDE_xSemaphoreGetMutexHolder       1

/* FreeRTOS hooks to NVIC vectors */
#define xPortPendSVHandler    PendSV_Handler
#define xPortSysTickHandler   SysTick_Handler
#define vPortSVCHandler       SVC_Handler

/* Interrupt nesting priorities */
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY         ((1<<configPRIO_BITS) - 1)
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY    2
#define configKERNEL_INTERRUPT_PRIORITY                 ( configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS) )
#define configMAX_SYSCALL_INTERRUPT_PRIORITY            ( configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS) )

#endif /* FREERTOS_CONFIG_H */
