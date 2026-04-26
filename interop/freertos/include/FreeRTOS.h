/*
 * FreeRTOS API wrapper for SuperTinyKernel (STK) RTOS.
 *
 * Maps the standard FreeRTOS C API onto the STK C++ API, allowing existing
 * FreeRTOS-based projects to run on STK with minimal or no application changes.
 *
 * Supported API groups:
 *   - Kernel control        (vTaskStartScheduler, taskENTER/EXIT_CRITICAL,
 *                            xTaskGetTickCount, vTaskDelay, vTaskDelayUntil,
 *                            xTaskDelayUntil, xTaskGetSchedulerState)
 *   - Task management       (xTaskCreate, vTaskDelete, vTaskSuspend, vTaskResume,
 *                            xTaskAbortDelay, vTaskPrioritySet, uxTaskPriorityGet,
 *                            xTaskGetHandle, xTaskGetCurrentTaskHandle, pcTaskGetName,
 *                            uxTaskGetStackHighWaterMark, eTaskGetState,
 *                            uxTaskGetSystemState,
 *                            xTaskCreateRestricted, xTaskCreateRestrictedStatic,
 *                            vTaskList, vTaskGetRunTimeStats)
 *   - Queue                 (xQueueCreate, xQueueCreateStatic, vQueueDelete,
 *                            xQueueSend, xQueueSendToBack, xQueueSendToFront,
 *                            xQueueReceive, xQueuePeek, xQueuePeekFromISR,
 *                            xQueueOverwrite, xQueueOverwriteFromISR,
 *                            uxQueueMessagesWaiting, uxQueueMessagesWaitingFromISR,
 *                            uxQueueSpacesAvailable, xQueueReset,
 *                            xQueueSendFromISR, xQueueReceiveFromISR,
 *                            xQueueSendToBackFromISR, xQueueSendToFrontFromISR,
 *                            xQueueIsQueueEmptyFromISR, xQueueIsQueueFullFromISR,
 *                            xQueueGetMutexHolder, xQueueGetMutexHolderFromISR,
 *                            xQueueCreateSet, xQueueAddToSet, xQueueRemoveFromSet,
 *                            xQueueSelectFromSet, xQueueSelectFromSetFromISR)
 *   - Semaphore / Mutex     (xSemaphoreCreateBinary, xSemaphoreCreateBinaryStatic,
 *                            xSemaphoreCreateCounting, xSemaphoreCreateCountingStatic,
 *                            xSemaphoreCreateMutex, xSemaphoreCreateMutexStatic,
 *                            xSemaphoreCreateRecursiveMutex,
 *                            xSemaphoreCreateRecursiveMutexStatic,
 *                            vSemaphoreDelete, xSemaphoreTake, xSemaphoreTakeFromISR,
 *                            xSemaphoreTakeRecursive, xSemaphoreGive,
 *                            xSemaphoreGiveRecursive, xSemaphoreGiveFromISR,
 *                            uxSemaphoreGetCount,
 *                            xSemaphoreGetMutexHolder,
 *                            xSemaphoreGetMutexHolderFromISR)
 *   - Software timers       (xTimerCreate, xTimerCreateStatic, xTimerDelete,
 *                            xTimerStart, xTimerStop, xTimerReset,
 *                            xTimerChangePeriod, xTimerIsTimerActive,
 *                            pvTimerGetTimerID, pcTimerGetName,
 *                            xTimerStartFromISR, xTimerStopFromISR,
 *                            xTimerResetFromISR, xTimerChangePeriodFromISR,
 *                            xTimerPendFunctionCall, xTimerPendFunctionCallFromISR)
 *   - Event groups          (xEventGroupCreate, xEventGroupCreateStatic,
 *                            vEventGroupDelete,
 *                            xEventGroupSetBits, xEventGroupClearBits,
 *                            xEventGroupGetBits, xEventGroupWaitBits,
 *                            xEventGroupSetBitsFromISR, xEventGroupClearBitsFromISR,
 *                            xEventGroupSync)
 *   - Task notifications    (xTaskNotifyGive, ulTaskNotifyTake,
 *                            xTaskNotify, xTaskNotifyWait, xTaskNotifyFromISR,
 *                            xTaskNotifyGiveIndexed, ulTaskNotifyTakeIndexed,
 *                            xTaskNotifyIndexed, xTaskNotifyWaitIndexed,
 *                            xTaskNotifyFromISRIndexed,
 *                            xTaskNotifyAndQuery, xTaskNotifyAndQueryIndexed,
 *                            xTaskNotifyAndQueryFromISR,
 *                            xTaskNotifyAndQueryFromISRIndexed,
 *                            xTaskNotifyStateClear, xTaskNotifyStateClearIndexed,
 *                            ulTaskNotifyValueClear, ulTaskNotifyValueClearIndexed)
 *   - Stream buffers        (xStreamBufferCreate, xStreamBufferCreateStatic,
 *                            xStreamBufferCreateWithCallback,
 *                            xStreamBufferCreateStaticWithCallback,
 *                            xStreamBufferSend, xStreamBufferReceive,
 *                            xStreamBufferSendFromISR, xStreamBufferReceiveFromISR,
 *                            vStreamBufferDelete, xStreamBufferBytesAvailable,
 *                            xStreamBufferSpacesAvailable, xStreamBufferIsEmpty,
 *                            xStreamBufferIsFull, xStreamBufferReset,
 *                            xStreamBufferResetFromISR,
 *                            xStreamBufferSetTriggerLevel,
 *                            xStreamBufferGetTriggerLevel,
 *                            xStreamBufferNextMessageLengthBytes)
 *   - Message buffers       (xMessageBufferCreate, xMessageBufferCreateStatic,
 *                            xMessageBufferCreateWithCallback,
 *                            xMessageBufferCreateStaticWithCallback,
 *                            xMessageBufferSend, xMessageBufferSendFromISR,
 *                            xMessageBufferReceive, xMessageBufferReceiveFromISR,
 *                            vMessageBufferDelete, xMessageBufferIsEmpty,
 *                            xMessageBufferIsFull, xMessageBufferSpacesAvailable,
 *                            xMessageBufferReset, xMessageBufferResetFromISR,
 *                            xMessageBufferNextLengthBytes)
 *
 * Design notes:
 *   - All objects are heap-allocated with operator new/delete.
 *     For static deployments replace with a static pool allocator.
 *   - One global STK Kernel instance (g_StkKernel) is configured with
 *     KERNEL_DYNAMIC | KERNEL_SYNC and SwitchStrategyFP32 (32 fixed-priority
 *     levels, same strategy used by the CMSIS wrapper).
 *     FreeRTOS priorities (0=lowest .. configMAX_PRIORITIES-1=highest) map
 *     directly to STK priority levels 0..configMAX_PRIORITIES-1.
 *     configMAX_PRIORITIES must be <= 32 (compile-time assertion in the .cpp).
 *     The highest-priority ready task always preempts lower ones, exactly
 *     matching FreeRTOS fixed-priority preemptive scheduling semantics.
 *   - portMAX_DELAY (0xFFFFFFFF) is translated to stk::WAIT_INFINITE.
 *   - Timeout values are in ticks; STK also takes ticks, so no conversion needed
 *     when tick resolution is 1 ms (the default PERIODICITY_DEFAULT).
 *   - Recursive mutexes are backed by stk::sync::Mutex (always recursive in STK).
 *   - Binary semaphores are backed by stk::sync::Semaphore with max_count=1.
 *   - Counting semaphores are backed by stk::sync::Semaphore.
 *   - Event groups are backed by stk::sync::EventFlags (32-bit, bits 0..23 usable
 *     per FreeRTOS convention; bits 24..30 are free, bit 31 is reserved by STK).
 *   - Software timers are backed by stk::time::TimerHost.
 *   - Task notifications are backed by per-task stk::sync::Semaphore.
 *   - Queue sets are backed by a per-set stk::sync::MessageQueue of void*
 *     tokens (sizeof(void*) per slot).  Member queues and semaphores carry a
 *     non-owning back-pointer to their registered set; QueueSetNotify() posts
 *     the member handle into the set's token FIFO after every successful send
 *     or signal.  Type discrimination between FrtosQueue and FrtosSemaphore
 *     uses the fact that SemKind (offset 0 in FrtosSemaphore, values 0 or 1)
 *     is always < the first byte of a MessageQueue vtable pointer (>= 4).
 *
 * Limitations / deviations:
 *   - Priority inheritance is not supported (STK mutex is always recursive,
 *     not priority-inheriting).
 *   - configUSE_PREEMPTION is assumed to be 1; cooperative scheduling is not modelled.
 *   - Task notifications: indexed API supports slots 0 .. configTASK_NOTIFICATION_ARRAY_ENTRIES-1.
 *   - uxTaskGetNumberOfTasks() requires the kernel to be running.
 */

#ifndef FREERTOS_STK_H_
#define FREERTOS_STK_H_

#include <stdint.h>
#include <stddef.h>

#include "FreeRTOSConfig.h"

/* -------------------------------------------------------------------------
 * Portability macros - mirror what FreeRTOS normally provides via
 * FreeRTOSConfig.h + portmacro.h so that application code compiles
 * without changes when those headers are absent.
 * -------------------------------------------------------------------------*/

#ifndef configMAX_PRIORITIES
#define configMAX_PRIORITIES             32U
#endif

#ifndef configMINIMAL_STACK_SIZE
#define configMINIMAL_STACK_SIZE         128U   /*!< Minimum stack depth in Words. */
#endif

#ifndef configTICK_RATE_HZ
#define configTICK_RATE_HZ               1000U
#endif

#ifndef configNUM_THREAD_LOCAL_STORAGE_POINTERS
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS  4U  /*!< Per-task TLS pointer slots. */
#endif

#ifndef configTASK_NOTIFICATION_ARRAY_ENTRIES
#define configTASK_NOTIFICATION_ARRAY_ENTRIES    1U  /*!< Number of per-task notification slots (indexed API). */
#endif

#ifndef configUSE_QUEUE_SETS
#define configUSE_QUEUE_SETS             1U  /*!< Include queue sets API. */
#endif

#ifndef configUSE_MUTEXES
#define configUSE_MUTEXES                1U  /*!< Include mutex and recursive-mutex API. */
#endif

#ifndef configUSE_TIMERS
#define configUSE_TIMERS                 1U  /*!< Include software timer API. */
#endif

#ifndef configUSE_EVENT_GROUPS
#define configUSE_EVENT_GROUPS           1U  /*!< Include event group API. */
#endif

#ifndef configUSE_STREAM_BUFFERS
#define configUSE_STREAM_BUFFERS         1U  /*!< Include stream buffer and message buffer API. */
#endif

#ifndef configUSE_COUNTING_SEMAPHORES
#define configUSE_COUNTING_SEMAPHORES    1U  /*!< Include counting semaphore API. */
#endif

#ifndef configUSE_TASK_NOTIFICATIONS
#define configUSE_TASK_NOTIFICATIONS     1U  /*!< Include task notification API (xTaskNotify etc.). */
#endif

#ifndef configTOTAL_HEAP_SIZE
#define configTOTAL_HEAP_SIZE            10240U /*!< Dynamic heap size in bytes. */
#endif

/*! \def   FREERTOS_STK_MAX_TASKS
    \brief Maximum number of concurrent tasks managed by the kernel.
           Increase if your application creates more tasks simultaneously.
*/
#ifndef FREERTOS_STK_MAX_TASKS
#define FREERTOS_STK_MAX_TASKS           16U
#endif

/*! \def   FREERTOS_STK_DEFAULT_STACK_WORDS
    \brief Default stack depth in Words when the caller passes usStackDepth = 0.
*/
#ifndef FREERTOS_STK_DEFAULT_STACK_WORDS
#define FREERTOS_STK_DEFAULT_STACK_WORDS 256U
#endif

/*! \def   FREERTOS_STK_PEND_CALL_QUEUE_SIZE
    \brief Capacity of the static deferred-call queue used by
           xTimerPendFunctionCall() and xTimerPendFunctionCallFromISR().

    Each slot holds one PendCall record: one function pointer, one void*
    parameter, and one uint32_t parameter — typically 12–16 bytes on
    32-bit targets.  The queue lives in static storage (zero heap), so
    this value determines RAM consumption at link time rather than runtime.

    Choose the maximum number of deferred calls that can be simultaneously
    in-flight before the TimerHost handler task drains them.  A value of 8
    is sufficient for most applications; increase it if your ISR rate is
    high relative to the RTOS tick rate or if several ISRs may pend calls
    concurrently.

    \note  xTimerPendFunctionCall()        returns pdFAIL when the queue is full.
    \note  xTimerPendFunctionCallFromISR() also returns pdFAIL when full (non-blocking).
    \note  Must be >= 1.
*/
#ifndef FREERTOS_STK_PEND_CALL_QUEUE_SIZE
#define FREERTOS_STK_PEND_CALL_QUEUE_SIZE  8U
#endif

/* =========================================================================
 * Single extern "C" block - matches the cmsis_os2.h pattern.
 * All typedefs, enums, macros referencing C functions, and all function
 * declarations are enclosed here so both C and C++ translation units
 * see consistent C linkage with no per-declaration repetition.
 * =========================================================================*/

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * FreeRTOS primitive types
 * -------------------------------------------------------------------------*/

typedef uint32_t      TickType_t;
typedef long          BaseType_t;
typedef unsigned long UBaseType_t;
typedef long          portBASE_TYPE;
typedef uintptr_t     StackType_t;   /*!< Native-word stack element type, matches stk::Word. */

#define portMAX_DELAY        ((TickType_t)0xFFFFFFFFUL) /*!< Block indefinitely. */
#define pdTRUE               ((BaseType_t)1)
#define pdFALSE              ((BaseType_t)0)
#define pdPASS               (pdTRUE)
#define pdFAIL               (pdFALSE)
#define errQUEUE_EMPTY       ((BaseType_t)0)
#define errQUEUE_FULL        ((BaseType_t)0)

#ifndef configSTACK_DEPTH_TYPE
    #define configSTACK_DEPTH_TYPE StackType_t /*!< Stack depth word type. */
#endif

/* -------------------------------------------------------------------------
 * Task state (eTaskState)
 * -------------------------------------------------------------------------*/

/// Task execution state, returned by eTaskGetState().
typedef enum
{
    eRunning   = 0, /*!< Task is actively executing on the CPU.           */
    eReady,         /*!< Task is in the ready list, eligible to run.      */
    eBlocked,       /*!< Task is waiting for an event or timeout.         */
    eSuspended,     /*!< Task is explicitly suspended via vTaskSuspend(). */
    eDeleted,       /*!< Task has been deleted but not yet cleaned up.    */
    eInvalid        /*!< Invalid / unknown state.                         */
} eTaskState;

/* -------------------------------------------------------------------------
 * Task notification actions (xTaskNotify)
 * -------------------------------------------------------------------------*/

/// Action applied to a task's notification value by xTaskNotify().
typedef enum
{
    eNoAction = 0,            /*!< No action; notification sent without modifying the value. */
    eSetBits,                 /*!< OR ulValue into the notification value.                   */
    eIncrement,               /*!< Increment the notification value by 1 (ulValue ignored).  */
    eSetValueWithOverwrite,   /*!< Set notification value to ulValue unconditionally.         */
    eSetValueWithoutOverwrite /*!< Set only if the previous notification was consumed.        */
} eNotifyAction;

/* -------------------------------------------------------------------------
 * Opaque handle types
 *
 * All handles are pointers to internal control block structs defined in
 * freertos_stk.cpp.  Application code treats them as opaque void *.
 * -------------------------------------------------------------------------*/

typedef void *TaskHandle_t;         /*!< Handle for a task.            */
typedef void *QueueHandle_t;        /*!< Handle for a queue.           */
typedef void *SemaphoreHandle_t;    /*!< Handle for a semaphore/mutex. */
typedef void *TimerHandle_t;        /*!< Handle for a software timer.  */
typedef void *EventGroupHandle_t;   /*!< Handle for an event group.    */
typedef void *StreamBufferHandle_t; /*!< Handle for a stream buffer.   */
typedef void *MessageBufferHandle_t;/*!< Handle for a message buffer.  */

/* QueueSet is not supported; typedefs kept for compilation compatibility. */
typedef void *QueueSetHandle_t;
typedef void *QueueSetMemberHandle_t;

/* -------------------------------------------------------------------------
 * Function pointer types
 * -------------------------------------------------------------------------*/

typedef void (*TaskFunction_t)(void *pvParameters);           /*!< Task entry function.          */
typedef void (*TimerCallbackFunction_t)(TimerHandle_t xTimer);/*!< Timer expiry callback.        */
typedef void (*PendedFunction_t)(void *pvParameter1, uint32_t ulParameter2); /*!< Callback for xTimerPendFunctionCall(). */

/*! Stream / message buffer send-complete and receive-complete callback.
 *  Invoked after bytes are successfully written (send) or read (receive)
 *  from the buffer.  \\a xHigherPriorityTaskWoken is always set to pdFALSE
 *  by the STK backend; it is included only for FreeRTOS API compatibility.
 *  May be NULL (no callback).
 */
typedef void (*StreamBufferCallbackFunction_t)(
    StreamBufferHandle_t xStreamBuffer,
    BaseType_t          *pxHigherPriorityTaskWoken);

/* -------------------------------------------------------------------------
 * Task parameters structure
 * -------------------------------------------------------------------------*/

/// Parameters passed to xTaskCreate().
typedef struct
{
    const char  *pcName;       /*!< Human-readable task name for debugging.                  */
    uint32_t     usStackDepth; /*!< Stack depth in Words.                                    */
    void        *pvParameters; /*!< Argument forwarded to pvTaskCode.                        */
    UBaseType_t  uxPriority;   /*!< Priority: 0 = lowest, configMAX_PRIORITIES-1 = highest. */
} TaskParameters_t;

/* -------------------------------------------------------------------------
 * TaskStatus_t — per-task snapshot filled by uxTaskGetSystemState()
 * -------------------------------------------------------------------------*/

/// Per-task status snapshot filled by uxTaskGetSystemState().
///
/// Notes on field availability in this STK wrapper:
///   - ulRunTimeCounter is always 0 (STK has no per-task CPU accounting).
///   - uxBasePriority equals uxCurrentPriority (no priority inheritance).
///   - xTaskNumber is a monotonically increasing serial assigned at creation.
typedef struct
{
    TaskHandle_t  xHandle;             /*!< Opaque handle identifying the task. */
    const char   *pcTaskName;          /*!< Pointer to the task's name string (not a copy). */
    eTaskState    eCurrentState;       /*!< Current execution state. */
    UBaseType_t   uxCurrentPriority;   /*!< Current scheduling priority. */
    UBaseType_t   uxBasePriority;      /*!< Base priority (equals uxCurrentPriority in STK). */
    uint32_t      ulRunTimeCounter;    /*!< Accumulated CPU time (always 0; STK has no per-task accounting). */
    StackType_t  *pxStackBase;         /*!< Pointer to the bottom of the task's stack. */
    configSTACK_DEPTH_TYPE usStackHighWaterMark; /*!< Minimum free stack space seen (Words). */
    uint32_t      xTaskNumber;         /*!< Monotonic serial number assigned at creation. */
} TaskStatus_t;

/* -------------------------------------------------------------------------
 * Event group bits type
 * -------------------------------------------------------------------------*/

typedef uint32_t EventBits_t; /*!< Bitmask for event group operations (bits 0..23 per FreeRTOS convention). */

/* -------------------------------------------------------------------------
 * Critical section and yield - underlying C functions called by the macros
 * below.  Declared here inside the extern "C" block so they are correctly
 * resolved whether the caller is a C or a C++ translation unit.
 * -------------------------------------------------------------------------*/

void vPortEnterCritical(void);
void vPortExitCritical(void);
void taskYIELD_impl(void);

#define taskENTER_CRITICAL()     vPortEnterCritical()
#define taskEXIT_CRITICAL()      vPortExitCritical()
#define taskDISABLE_INTERRUPTS() vPortEnterCritical()
#define taskENABLE_INTERRUPTS()  vPortExitCritical()
#define taskYIELD()              taskYIELD_impl()
#define portYIELD()              taskYIELD_impl()

/* -------------------------------------------------------------------------
 * Kernel control
 * -------------------------------------------------------------------------*/

/// Initialise STK and start the scheduler.
/// Does not return for KERNEL_DYNAMIC until all tasks have exited.
void vTaskStartScheduler(void);

/// End scheduling (KERNEL_DYNAMIC only). Included for API completeness.
void vTaskEndScheduler(void);

/// Suspend the scheduler (disables preemption; interrupts remain enabled).
void vTaskSuspendAll(void);

/// Resume a previously suspended scheduler.
/// \return pdTRUE if a context switch is pending, pdFALSE otherwise.
BaseType_t xTaskResumeAll(void);

/// Return the tick count since the scheduler started.
TickType_t xTaskGetTickCount(void);

/// Return the tick count from ISR context (ISR-safe).
TickType_t xTaskGetTickCountFromISR(void);

/// Return the number of tasks currently under kernel management.
UBaseType_t uxTaskGetNumberOfTasks(void);

/* Scheduler state constants returned by xTaskGetSchedulerState(). */
#define taskSCHEDULER_NOT_STARTED  ((BaseType_t)0) /*!< Scheduler has not yet been started (STATE_INACTIVE / STATE_READY). */
#define taskSCHEDULER_RUNNING      ((BaseType_t)1) /*!< Scheduler is running normally (STATE_RUNNING). */
#define taskSCHEDULER_SUSPENDED    ((BaseType_t)2) /*!< Scheduler is suspended via vTaskSuspendAll() (STATE_SUSPENDED). */

/// Return the current state of the FreeRTOS/STK scheduler.
/// \return One of taskSCHEDULER_NOT_STARTED, taskSCHEDULER_RUNNING,
///         or taskSCHEDULER_SUSPENDED.
BaseType_t xTaskGetSchedulerState(void);

/* -------------------------------------------------------------------------
 * Task management
 * -------------------------------------------------------------------------*/

/// Create a new dynamic task.
/// \param pvTaskCode      Task function pointer.
/// \param pcName          Descriptive name for debugging.
/// \param usStackDepth    Stack depth in Words (not bytes).
/// \param pvParameters    Argument passed to pvTaskCode.
/// \param uxPriority      Priority: 0 = lowest, configMAX_PRIORITIES-1 = highest.
/// \param pxCreatedTask   Optional: receives the handle of the created task.
/// \return pdPASS on success, pdFAIL if the task could not be created.
BaseType_t xTaskCreate(TaskFunction_t  pvTaskCode,
                       const char     *pcName,
                       uint32_t        usStackDepth,
                       void           *pvParameters,
                       UBaseType_t     uxPriority,
                       TaskHandle_t   *pxCreatedTask);

/// Delete a task. Pass NULL to delete the calling task.
void vTaskDelete(TaskHandle_t xTaskToDelete);

/// Opaque buffer type that the caller must supply for xTaskCreateStatic().
/// Must be at least sizeof(StaticTask_t) bytes; declared as a fixed-size
/// array of uintptr_t so the compiler enforces natural alignment without
/// requiring knowledge of FrtosTask internals.
/// Size is conservative: covers FrtosTask on all supported STK targets.
#define STATIC_TASK_TCB_SIZE_WORDS  (19U + configNUM_THREAD_LOCAL_STORAGE_POINTERS \
                                         + (3U * (configTASK_NOTIFICATION_ARRAY_ENTRIES - 1U)))
typedef struct
{
    uintptr_t _opaque[STATIC_TASK_TCB_SIZE_WORDS];
} StaticTask_t;

/// Opaque buffer type that the caller must supply for xQueueCreateStatic().
/// Must be at least sizeof(StaticQueue_t) bytes.  Declared as a fixed-size
/// array of uintptr_t to enforce natural alignment without exposing FrtosQueue
/// internals.  Size is conservative: covers FrtosQueue on all supported targets.
#define STATIC_QUEUE_TCB_SIZE_WORDS  24U
typedef struct
{
    uintptr_t _opaque[STATIC_QUEUE_TCB_SIZE_WORDS];
} StaticQueue_t;

/// Opaque buffer type that the caller must supply for xSemaphoreCreateBinaryStatic()
/// and related static semaphore/mutex creation functions.
/// Must be at least sizeof(FrtosSemaphore) bytes.  Declared as a fixed-size
/// array of uintptr_t to enforce natural alignment without exposing FrtosSemaphore
/// internals.  Size is conservative: covers FrtosSemaphore on all supported targets.
#define STATIC_SEMAPHORE_TCB_SIZE_WORDS  8U
typedef struct
{
    uintptr_t _opaque[STATIC_SEMAPHORE_TCB_SIZE_WORDS];
} StaticSemaphore_t;

/// Opaque buffer type that the caller must supply for xTimerCreateStatic().
/// Must be at least sizeof(FrtosTimer) bytes.  Declared as a fixed-size array
/// of uintptr_t to enforce natural alignment without exposing FrtosTimer internals.
/// Size is conservative: covers FrtosTimer (vtable ptr + base + 5 members) on
/// all supported targets.
#define STATIC_TIMER_TCB_SIZE_WORDS  16U
typedef struct
{
    uintptr_t _opaque[STATIC_TIMER_TCB_SIZE_WORDS];
} StaticTimer_t;

/// Opaque buffer type that the caller must supply for xEventGroupCreateStatic().
/// Must be at least sizeof(FrtosEventGroup) bytes.  Declared as a fixed-size
/// array of uintptr_t to enforce natural alignment without exposing
/// FrtosEventGroup internals.
#define STATIC_EVENT_GROUP_TCB_SIZE_WORDS  10U
typedef struct
{
    uintptr_t _opaque[STATIC_EVENT_GROUP_TCB_SIZE_WORDS];
} StaticEventGroup_t;

/// Opaque buffer type for xStreamBufferCreateStatic() and
/// xStreamBufferCreateStaticWithCallback().
/// Covers FrtosStreamBuffer (Pipe + trigger level + ownership flags +
/// two StreamBufferCallbackFunction_t pointers) on all supported STK targets.
/// Size is conservative (+2 words vs the pre-callback layout).
#define STATIC_STREAM_BUFFER_TCB_SIZE_WORDS  26U
typedef struct
{
    uintptr_t _opaque[STATIC_STREAM_BUFFER_TCB_SIZE_WORDS];
} StaticStreamBuffer_t;

/// Opaque buffer type for xMessageBufferCreateStatic() and
/// xMessageBufferCreateStaticWithCallback().
/// Covers FrtosMessageBuffer (BlockMemoryPool header + envelope MessageQueue header
/// + ownership flags + two StreamBufferCallbackFunction_t pointers) on all
/// supported STK targets. Size is conservative (+2 words vs the pre-callback layout).
#define STATIC_MESSAGE_BUFFER_TCB_SIZE_WORDS  38U
typedef struct
{
    uintptr_t _opaque[STATIC_MESSAGE_BUFFER_TCB_SIZE_WORDS];
} StaticMessageBuffer_t;

/// Create a task using caller-supplied stack and TCB memory (no heap allocation).
/// \param pvTaskCode      Task function pointer.
/// \param pcName          Descriptive name for debugging.
/// \param ulStackDepth    Stack depth in Words (not bytes). Must be >=
///                        configMINIMAL_STACK_SIZE.
/// \param pvParameters    Argument passed to pvTaskCode.
/// \param uxPriority      Priority: 0 = lowest, configMAX_PRIORITIES-1 = highest.
/// \param puxStackBuffer  Caller-allocated stack buffer of ulStackDepth Words.
///                        Must remain valid for the lifetime of the task.
/// \param pxTaskBuffer    Caller-allocated TCB buffer (StaticTask_t).
///                        Must remain valid for the lifetime of the task.
/// \return Task handle. Never NULL if all pointer arguments are non-NULL.
TaskHandle_t xTaskCreateStatic(TaskFunction_t  pvTaskCode,
                               const char     *pcName,
                               uint32_t        ulStackDepth,
                               void           *pvParameters,
                               UBaseType_t     uxPriority,
                               StackType_t    *puxStackBuffer,
                               StaticTask_t   *pxTaskBuffer);

/// MPU memory region descriptor used by xTaskCreateRestrictedStatic().
/// \note STK does not implement MPU support. This struct is provided for
///       source compatibility only; region fields are accepted but ignored.
#ifndef portNUM_CONFIGURABLE_REGIONS
#  define portNUM_CONFIGURABLE_REGIONS 3U
#endif
typedef struct
{
    void       *pvBaseAddress;   /*!< Base address of the MPU region.                */
    uint32_t    ulLengthInBytes; /*!< Length of the region in bytes.                 */
    uint32_t    ulParameters;    /*!< Region attributes (device-specific encoding).  */
} MemoryRegion_t;

/// Task creation parameters for xTaskCreateRestrictedStatic().
/// Mirrors the standard FreeRTOS xTASK_PARAMETERS layout.
/// \note STK does not implement MPU support. The xRegions field is accepted
///       for source compatibility but is otherwise ignored.
typedef struct
{
    TaskFunction_t  pvTaskCode;    /*!< Task entry function pointer.                 */
    const char     *pcName;        /*!< Descriptive task name for debugging.         */
    uint32_t        usStackDepth;  /*!< Stack depth in Words (not bytes).            */
    void           *pvParameters;  /*!< Argument forwarded to pvTaskCode.            */
    UBaseType_t     uxPriority;    /*!< Priority: 0 = lowest, configMAX_PRIORITIES-1 = highest. */
    StackType_t    *puxStackBuffer;/*!< Caller-allocated stack buffer.               */
    StaticTask_t   *pxTaskBuffer;  /*!< Caller-allocated TCB buffer (StaticTask_t).  */
    MemoryRegion_t  xRegions[portNUM_CONFIGURABLE_REGIONS]; /*!< MPU regions (ignored, no STK MPU support). */
} TaskParameters_restricted_t;

/// Create a task with caller-supplied static memory and optional MPU region
/// descriptors (source-compatible with xTaskCreateRestricted).
///
/// \note STK does not implement MPU support. This function is a forward-
///       compatibility stub: it extracts the function, stack, priority, and
///       pxTaskBuffer from \a pxTaskDefinition and delegates to
///       xTaskCreateStatic(), silently ignoring the xRegions table.
///       When STK gains MPU support the implementation will be extended to
///       program the region descriptors.
///
/// \param pxTaskDefinition  Pointer to a TaskParameters_restricted_t descriptor.
///                          Must not be NULL. puxStackBuffer and pxTaskBuffer
///                          inside the struct must also be non-NULL.
/// \param pxCreatedTask     Optional: receives the handle of the created task.
/// \return pdPASS on success, pdFAIL if any mandatory pointer is NULL.
BaseType_t xTaskCreateRestrictedStatic(
    const TaskParameters_restricted_t *pxTaskDefinition,
    TaskHandle_t                       *pxCreatedTask);

/// Create a task with optional MPU region descriptors, allocating the TCB
/// and stack from the heap (source-compatible with the FreeRTOS heap variant
/// of xTaskCreateRestricted).
///
/// \note STK does not implement MPU support. This function extracts
///       pvTaskCode, usStackDepth, pvParameters, and uxPriority from
///       \a pxTaskDefinition and delegates to xTaskCreate(), silently
///       ignoring puxStackBuffer (a heap stack is allocated instead) and
///       the xRegions table.  When STK gains MPU support the implementation
///       will be extended to program the region descriptors.
///
/// \param pxTaskDefinition  Pointer to a TaskParameters_restricted_t descriptor.
///                          Must not be NULL.
/// \param pxCreatedTask     Optional: receives the handle of the created task.
/// \return pdPASS on success, pdFAIL if pxTaskDefinition is NULL or heap
///         allocation fails.
BaseType_t xTaskCreateRestricted(
    const TaskParameters_restricted_t *pxTaskDefinition,
    TaskHandle_t                       *pxCreatedTask);

/// Write a human-readable task-status table into pcWriteBuffer.
///
/// Each row contains: name, state letter (X/R/B/S/D), priority,
/// stack high-water mark (Words), and a 1-based task number.
/// Column widths match the standard FreeRTOS vTaskList() output so that
/// existing tools and log parsers work without modification.
///
/// \param pcWriteBuffer  Caller-allocated destination buffer.  Must be large
///                       enough for all active tasks.  A safe minimum is
///                       uxTaskGetNumberOfTasks() * 40 bytes.
/// \note  The output is a snapshot; it is not thread-safe with respect to
///        concurrent task creation or deletion.
void vTaskList(char *pcWriteBuffer);

/// Write a human-readable CPU run-time statistics table into pcWriteBuffer.
///
/// Each row contains: task name, absolute run-time counter, and percentage
/// of total run time.  Column layout matches the standard FreeRTOS
/// vTaskGetRunTimeStats() output so existing log parsers work unchanged.
///
/// \note  STK has no per-task CPU run-time accumulator.  This wrapper always
///        emits 0 for both the absolute counter and the percentage, matching
///        the FreeRTOS convention for targets where
///        \c configGENERATE_RUN_TIME_STATS is not enabled.  The function is
///        provided for link compatibility; applications that need real CPU
///        accounting must instrument the target with a free-running hardware
///        counter and replace the two MemoryAllocator definitions in
///        freertos_stk.cpp with the appropriate port-layer calls.
///
/// \param pcWriteBuffer  Caller-allocated destination buffer.  A safe minimum
///                       is uxTaskGetNumberOfTasks() * 40 bytes.
void vTaskGetRunTimeStats(char *pcWriteBuffer);

/// Suspend a task indefinitely. Pass NULL to suspend the calling task.
void vTaskSuspend(TaskHandle_t xTaskToSuspend);

/// Resume a previously suspended task.
void vTaskResume(TaskHandle_t xTaskToResume);

/// Resume a previously suspended task from ISR context.
/// \return pdTRUE if the task was successfully resumed.
BaseType_t xTaskResumeFromISR(TaskHandle_t xTaskToResume);

/// Abort a delay that the target task is currently blocked in (vTaskDelay,
/// vTaskDelayUntil, or any timed sync primitive wait).
/// The task is made immediately runnable; its next Wait/Sleep will not be
/// affected.
/// \return pdPASS if the task was in a delayed state and the abort was issued,
///         pdFAIL if the task was not delayed.
/// \note   ISR-safe (delegates to stk::AbortSleep which is ISR-safe).
BaseType_t xTaskAbortDelay(TaskHandle_t xTask);

/// Block the calling task for a number of ticks.
void vTaskDelay(TickType_t xTicksToDelay);

/// Block until an absolute tick deadline for drift-free periodic loops.
/// Updates *pxPreviousWakeTime on each call.
void vTaskDelayUntil(TickType_t *pxPreviousWakeTime, TickType_t xTimeIncrement);

/// Delay until an absolute tick deadline (FreeRTOS 10.2+ name for vTaskDelayUntil).
/// Updates *pxPreviousWakeTime on each call.
/// \return pdTRUE if the task delayed, pdFALSE if the deadline had already passed
///         before the call was made (the task did not block).
BaseType_t xTaskDelayUntil(TickType_t *pxPreviousWakeTime, TickType_t xTimeIncrement);

/// Change the priority of a task. Pass NULL xTask for the calling task.
void vTaskPrioritySet(TaskHandle_t xTask, UBaseType_t uxNewPriority);

/// Query the priority of a task. Pass NULL xTask for the calling task.
UBaseType_t uxTaskPriorityGet(TaskHandle_t xTask);

/// Query the priority of a task from ISR context (ISR-safe).
UBaseType_t uxTaskPriorityGetFromISR(TaskHandle_t xTask);

/// Return the current execution state of a task.
eTaskState eTaskGetState(TaskHandle_t xTask);

/// Return the handle of the currently executing task.
TaskHandle_t xTaskGetCurrentTaskHandle(void);

/// Look up a task handle by name string (O(n) scan).
/// \return Handle of the first matching task, or NULL if not found.
TaskHandle_t xTaskGetHandle(const char *pcNameToQuery);

/// Return the name string of a task.
const char *pcTaskGetName(TaskHandle_t xTaskToQuery);

/// Return the unused stack depth high-water mark in Words.
/// Pass NULL for the calling task.
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t xTask);

/// Return the unused stack depth high-water mark in Words.
/// Pass NULL for the calling task.
configSTACK_DEPTH_TYPE uxTaskGetStackHighWaterMark2(TaskHandle_t xTask);

/// Populate an array of TaskStatus_t with a snapshot of every task's state.
/// \param pxTaskStatusArray  Caller-supplied array of at least uxArraySize elements.
/// \param uxArraySize        Capacity of pxTaskStatusArray
///                           (should be >= uxTaskGetNumberOfTasks()).
/// \param pulTotalRunTime    Receives 0 (STK has no global run-time counter).
///                           May be NULL.
/// \return Number of TaskStatus_t entries written.  May be less than
///         uxTaskGetNumberOfTasks() if pxTaskStatusArray was too small.
UBaseType_t uxTaskGetSystemState(TaskStatus_t    *pxTaskStatusArray,
                                  UBaseType_t      uxArraySize,
                                  uint32_t        *pulTotalRunTime);

/* -------------------------------------------------------------------------
 * Queue API
 * -------------------------------------------------------------------------*/

/// Create a queue capable of holding uxQueueLength items of uxItemSize bytes.
/// \return Queue handle, or NULL on allocation failure.
QueueHandle_t xQueueCreate(UBaseType_t uxQueueLength, UBaseType_t uxItemSize);

/// Create a queue using caller-supplied storage — no heap allocation.
/// \param uxQueueLength   Maximum number of items the queue can hold (>= 1).
/// \param uxItemSize      Size of each item in bytes (>= 1).
/// \param pucQueueStorage Caller-allocated data buffer of at least
///                        uxQueueLength * uxItemSize bytes.  Must remain valid
///                        for the entire lifetime of the queue.
/// \param pxStaticQueue   Caller-allocated queue control block (StaticQueue_t).
///                        Must remain valid for the entire lifetime of the queue.
/// \return Queue handle. Never NULL if all pointer arguments are non-NULL and
///         uxQueueLength / uxItemSize are within range.
QueueHandle_t xQueueCreateStatic(UBaseType_t    uxQueueLength,
                                  UBaseType_t    uxItemSize,
                                  uint8_t       *pucQueueStorage,
                                  StaticQueue_t *pxStaticQueue);

/// Delete a queue and free all associated memory.
void vQueueDelete(QueueHandle_t xQueue);

/// Post an item to the back of a queue (blocking).
/// \param xTicksToWait  Ticks to wait if full. portMAX_DELAY = wait forever.
/// \return pdTRUE on success, pdFALSE on timeout.
BaseType_t xQueueSend(QueueHandle_t xQueue,
                      const void   *pvItemToQueue,
                      TickType_t    xTicksToWait);

/// Post an item to the back of a queue (alias of xQueueSend).
BaseType_t xQueueSendToBack(QueueHandle_t xQueue,
                            const void   *pvItemToQueue,
                            TickType_t    xTicksToWait);

/// Post an item to the front of a queue (blocking).
/// The item becomes the next item returned by xQueueReceive().
/// \param xTicksToWait  Ticks to wait if full. portMAX_DELAY = wait forever.
/// \return pdTRUE on success, pdFALSE on timeout.
BaseType_t xQueueSendToFront(QueueHandle_t xQueue,
                             const void   *pvItemToQueue,
                             TickType_t    xTicksToWait);

/// Receive (dequeue) an item from a queue (blocking).
/// \return pdTRUE on success, pdFALSE on timeout.
BaseType_t xQueueReceive(QueueHandle_t xQueue,
                         void         *pvBuffer,
                         TickType_t    xTicksToWait);

/// Peek at the front item without removing it (blocking).
///
/// Copies the oldest item into \a pvBuffer without consuming it, so a
/// subsequent xQueueReceive() will return the same item.  The operation is
/// fully atomic — backed by stk::sync::MessageQueue::Peek() which holds an
/// internal critical section for the duration of the copy.
///
/// \param xTicksToWait  Ticks to wait if the queue is empty.
///                      portMAX_DELAY waits indefinitely.
/// \return pdTRUE if an item was peeked, pdFALSE on timeout.
/// \warning ISR-safe only with xTicksToWait = 0 (portNO_WAIT).
///          Use xQueuePeekFromISR() for non-blocking ISR access.
BaseType_t xQueuePeek(QueueHandle_t xQueue,
                      void         *pvBuffer,
                      TickType_t    xTicksToWait);

/// Peek at the front item from ISR context without removing it (non-blocking).
///
/// Copies the oldest item into \a pvBuffer atomically without consuming it,
/// backed by stk::sync::MessageQueue::TryPeek() which is ISR-safe.
///
/// \return pdTRUE if an item was available and peeked, pdFALSE if the queue
///         was empty.
/// \warning ISR-safe.
BaseType_t xQueuePeekFromISR(QueueHandle_t  xQueue,
                              void          *pvBuffer);

/// Return the number of items currently in the queue.
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t xQueue);

/// Return the number of items currently in the queue from ISR context.
/// \note ISR-safe on targets where a size_t-aligned read is atomic.
/// \return Point-in-time snapshot of the message count.
UBaseType_t uxQueueMessagesWaitingFromISR(QueueHandle_t xQueue);

/// Return the number of free slots in the queue.
UBaseType_t uxQueueSpacesAvailable(QueueHandle_t xQueue);

/// Reset a queue to the empty state, discarding all pending items.
/// \return pdPASS.
BaseType_t xQueueReset(QueueHandle_t xQueue);

/// Overwrite the value stored in a queue of length 1 (mailbox pattern).
/// If the queue is already full the existing item is discarded before writing.
/// \note Intended for length-1 queues only; behaviour is undefined for longer queues.
/// \return pdPASS always (the write always succeeds after the discard).
BaseType_t xQueueOverwrite(QueueHandle_t xQueue, const void *pvItemToQueue);

/// Overwrite the value stored in a length-1 queue from ISR context.
/// \param pxHigherPriorityTaskWoken  Set pdTRUE if a context switch is needed.
/// \return pdPASS always.
BaseType_t xQueueOverwriteFromISR(QueueHandle_t  xQueue,
                                   const void    *pvItemToQueue,
                                   BaseType_t    *pxHigherPriorityTaskWoken);

/// Post an item from ISR context (non-blocking).
/// \param pxHigherPriorityTaskWoken  Set pdTRUE if a context switch is needed.
/// \return pdTRUE if posted, pdFALSE if the queue was full.
BaseType_t xQueueSendFromISR(QueueHandle_t  xQueue,
                             const void    *pvItemToQueue,
                             BaseType_t    *pxHigherPriorityTaskWoken);

/// Receive an item from ISR context (non-blocking).
/// \param pxHigherPriorityTaskWoken  Set pdTRUE if a context switch is needed.
/// \return pdTRUE if received, pdFALSE if the queue was empty.
BaseType_t xQueueReceiveFromISR(QueueHandle_t  xQueue,
                                void          *pvBuffer,
                                BaseType_t    *pxHigherPriorityTaskWoken);

/// Post an item to the back of a queue from ISR context (non-blocking).
/// Equivalent to xQueueSendFromISR(); provided for source compatibility with
/// code that explicitly names the insertion end.
/// \param pxHigherPriorityTaskWoken  Set pdTRUE if a context switch is needed.
/// \return pdTRUE if posted, pdFALSE if the queue was full.
BaseType_t xQueueSendToBackFromISR(QueueHandle_t  xQueue,
                                   const void    *pvItemToQueue,
                                   BaseType_t    *pxHigherPriorityTaskWoken);

/// Post an item to the front of a queue from ISR context (non-blocking).
///
/// The item becomes the next item returned by xQueueReceive() / xQueuePeek().
/// Backed by stk::sync::MessageQueue::TryPutFront() which retreats the tail
/// pointer atomically under an internal critical section.
///
/// \param pxHigherPriorityTaskWoken  Always set to pdFALSE; STK handles scheduling.
/// \return pdTRUE if posted, pdFALSE if the queue was full.
/// \warning ISR-safe.
BaseType_t xQueueSendToFrontFromISR(QueueHandle_t  xQueue,
                                    const void    *pvItemToQueue,
                                    BaseType_t    *pxHigherPriorityTaskWoken);

/// Query whether a queue is empty from ISR context.
/// \note ISR-safe on targets where a size_t-aligned read is atomic
///       (per stk::sync::MessageQueue::IsEmpty() contract).
/// \return pdTRUE if the queue contains no items, pdFALSE otherwise.
BaseType_t xQueueIsQueueEmptyFromISR(const QueueHandle_t xQueue);

/// Query whether a queue is full from ISR context.
/// \note ISR-safe on targets where a size_t-aligned read is atomic
///       (per stk::sync::MessageQueue::IsFull() contract).
/// \return pdTRUE if the queue holds capacity items, pdFALSE otherwise.
BaseType_t xQueueIsQueueFullFromISR(const QueueHandle_t xQueue);

/// Return the handle of the task that currently holds a mutex-semaphore
/// when that semaphore is referenced by a QueueHandle_t alias.
///
/// In the FreeRTOS reference implementation mutex semaphores are built on top
/// of the internal queue structure, so \c xQueueGetMutexHolder() and
/// \c xSemaphoreGetMutexHolder() are the same function with different handle
/// types.  In this STK wrapper the two object types are separate structs:
///
///  - \c FrtosQueue    — backed by stk::sync::MessageQueue; has no owner field.
///  - \c FrtosSemaphore — backed by stk::sync::Semaphore (counting) or
///                        stk::sync::Mutex (mutex kind).  Only the Mutex kind
///                        carries an owner and supports STK priority inheritance.
///
/// This function accepts a \c QueueHandle_t and uses the same one-byte
/// type-discriminant (offset 0: SemKind ≤ 1 → semaphore, otherwise → queue)
/// already used by the Queue Set implementation to decide which path to take:
///
///  - If the handle actually points to a \c FrtosSemaphore with
///    \c SemKind::Mutex, the call is forwarded to xSemaphoreGetMutexHolder()
///    and the owning task handle (or NULL if unlocked) is returned.
///  - For a plain \c FrtosQueue handle, or for a counting/binary semaphore
///    handle, NULL is returned — the owner concept does not apply.
///
/// \param xQueue  Handle typed as QueueHandle_t.  May point to a mutex
///                semaphore in application code that uses the raw queue handle
///                alias (e.g. FreeRTOS+TCP internal usage).
/// \return Handle of the owning task, or NULL if unlocked / not a mutex.
/// \warning Not ISR-safe. Use xQueueGetMutexHolderFromISR() from ISR context.
/// \note   Requires \c configUSE_MUTEXES == 1.
TaskHandle_t xQueueGetMutexHolder(QueueHandle_t xQueue);

/// ISR-safe variant of xQueueGetMutexHolder().
///
/// Reads the mutex owner field with a single pointer-sized load, which is
/// naturally atomic on all supported STK architectures (Cortex-M and
/// equivalents) without requiring an additional critical section.
///
/// \param xQueue  Handle typed as QueueHandle_t.
/// \return Handle of the owning task, or NULL if unlocked / not a mutex.
/// \warning ISR-safe.
/// \note   Requires \c configUSE_MUTEXES == 1.
TaskHandle_t xQueueGetMutexHolderFromISR(QueueHandle_t xQueue);

/* -------------------------------------------------------------------------
 * Queue Set API
 *
 * A queue set allows a single task to block on multiple queues and/or binary
 * or counting semaphores simultaneously, waking as soon as any member
 * transitions from empty to non-empty.
 *
 * Usage rules (matching the FreeRTOS API contract):
 *   1. Call xQueueCreateSet() with a capacity >= the sum of the capacities
 *      of all queues and semaphores that will be added to the set.
 *   2. Add members with xQueueAddToSet() while they are empty.
 *   3. Call xQueueSelectFromSet() or xQueueSelectFromSetFromISR() to wait for
 *      any member to receive an item.  The returned handle identifies which
 *      member fired; call xQueueReceive() or xSemaphoreTake() on that handle
 *      to consume the item.
 *   4. Remove members with xQueueRemoveFromSet() only while they are empty.
 *   5. Mutexes must not be added to queue sets.
 *
 * \warning  xQueueOverwrite / xQueueOverwriteFromISR should not be used on
 *           queues that are set members, as they generate a set notification
 *           even when replacing an existing item rather than filling a new
 *           slot, which can cause spurious wakeups.
 * -------------------------------------------------------------------------*/

/// Create a queue set that can supervise up to uxEventQueueLength simultaneous
/// item-available notifications from its member queues and semaphores.
///
/// \param uxEventQueueLength  Total event capacity. Must be >= the sum of
///                            the capacities (uxQueueLength or max_count) of
///                            all queues and semaphores that will be added.
/// \return Queue set handle, or NULL on failure.
QueueSetHandle_t xQueueCreateSet(UBaseType_t uxEventQueueLength);

/// Register a queue or binary/counting semaphore as a member of a queue set.
///
/// The member must be empty at the time it is added.  Mutexes are not
/// permitted.  A member may belong to at most one set at a time.
///
/// \param xQueueOrSemaphore  Handle of the queue or semaphore to add.
/// \param xQueueSet          Handle of the target queue set.
/// \return pdPASS on success, pdFAIL if any precondition is violated.
BaseType_t xQueueAddToSet(QueueSetMemberHandle_t xQueueOrSemaphore,
                          QueueSetHandle_t       xQueueSet);

/// Unregister a queue or semaphore from a queue set.
///
/// The member must be empty at the time it is removed.
///
/// \param xQueueOrSemaphore  Handle of the queue or semaphore to remove.
/// \param xQueueSet          Handle of the queue set it currently belongs to.
/// \return pdPASS on success, pdFAIL if the member does not belong to this
///         set or is not empty.
BaseType_t xQueueRemoveFromSet(QueueSetMemberHandle_t xQueueOrSemaphore,
                               QueueSetHandle_t       xQueueSet);

/// Block until any member of the queue set receives an item, then return the
/// handle of the member that fired.
///
/// After xQueueSelectFromSet() returns a non-NULL handle, the caller must
/// call xQueueReceive() or xSemaphoreTake() on that handle to actually consume
/// the item.  The set only unblocks once per item deposited; if multiple items
/// arrive before the caller drains them, xQueueSelectFromSet() will return the
/// same member handle multiple times.
///
/// \param xQueueSet     Handle of the queue set to wait on.
/// \param xTicksToWait  Maximum time to wait. portMAX_DELAY = wait forever,
///                      0 = non-blocking poll.
/// \return Handle of the member that has an item available, or NULL on timeout.
/// \warning ISR-safe only with xTicksToWait = 0.
QueueSetMemberHandle_t xQueueSelectFromSet(QueueSetHandle_t xQueueSet,
                                           TickType_t       xTicksToWait);

/// Non-blocking ISR-safe variant of xQueueSelectFromSet.
///
/// Returns immediately with the handle of a ready member, or NULL if no
/// member currently has an item available.
///
/// \param xQueueSet  Handle of the queue set to poll.
/// \return Handle of a ready member, or NULL if no member is ready.
/// \note ISR-safe.
QueueSetMemberHandle_t xQueueSelectFromSetFromISR(QueueSetHandle_t xQueueSet);

/* -------------------------------------------------------------------------
 * Semaphore / Mutex API
 * -------------------------------------------------------------------------*/

/// Create a binary semaphore (initial count = 0, max = 1).
/// \return Semaphore handle, or NULL on failure.
SemaphoreHandle_t xSemaphoreCreateBinary(void);

/// Create a binary semaphore using caller-supplied storage — no heap allocation.
/// \param pxSemaphoreBuffer  Caller-allocated control block (StaticSemaphore_t).
///                           Must remain valid for the lifetime of the semaphore.
/// \return Semaphore handle. Never NULL if pxSemaphoreBuffer is non-NULL.
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *pxSemaphoreBuffer);

/// Create a counting semaphore.
/// \param uxMaxCount     Maximum count value.
/// \param uxInitialCount Initial count value (must be <= uxMaxCount).
/// \return Semaphore handle, or NULL on failure.
SemaphoreHandle_t xSemaphoreCreateCounting(UBaseType_t uxMaxCount,
                                           UBaseType_t uxInitialCount);

/// Create a counting semaphore using caller-supplied storage — no heap allocation.
/// \param uxMaxCount         Maximum count value.
/// \param uxInitialCount     Initial count value (must be <= uxMaxCount).
/// \param pxSemaphoreBuffer  Caller-allocated control block (StaticSemaphore_t).
///                           Must remain valid for the lifetime of the semaphore.
/// \return Semaphore handle. Never NULL if pxSemaphoreBuffer is non-NULL and
///         count arguments are valid.
SemaphoreHandle_t xSemaphoreCreateCountingStatic(UBaseType_t        uxMaxCount,
                                                 UBaseType_t        uxInitialCount,
                                                 StaticSemaphore_t *pxSemaphoreBuffer);

/// Create a mutex.
/// \note STK Mutex is always recursive; osMutexRecursive is always effective.
/// \return Mutex handle, or NULL on failure.
SemaphoreHandle_t xSemaphoreCreateMutex(void);

/// Create a mutex using caller-supplied storage — no heap allocation.
/// \param pxMutexBuffer  Caller-allocated control block (StaticSemaphore_t).
///                       Must remain valid for the lifetime of the mutex.
/// \return Mutex handle. Never NULL if pxMutexBuffer is non-NULL.
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *pxMutexBuffer);

/// Create a recursive mutex (same implementation as xSemaphoreCreateMutex).
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void);

/// Create a recursive mutex using caller-supplied storage — no heap allocation.
/// \param pxMutexBuffer  Caller-allocated control block (StaticSemaphore_t).
///                       Must remain valid for the lifetime of the mutex.
/// \return Mutex handle. Never NULL if pxMutexBuffer is non-NULL.
SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t *pxMutexBuffer);

/// Delete a semaphore or mutex and free its memory.
void vSemaphoreDelete(SemaphoreHandle_t xSemaphore);

/// Take (acquire) a semaphore or mutex (blocking).
/// \param xTicksToWait  Ticks to wait. portMAX_DELAY = wait forever.
/// \return pdTRUE if acquired, pdFALSE on timeout.
BaseType_t xSemaphoreTake(SemaphoreHandle_t xSemaphore, TickType_t xTicksToWait);

/// Take (acquire) a binary or counting semaphore from ISR context (non-blocking).
/// \note Mutex take from ISR is not permitted and returns pdFALSE.
/// \param pxHigherPriorityTaskWoken  Set pdTRUE if a context switch is needed.
/// \return pdTRUE if acquired, pdFALSE if the count was zero.
BaseType_t xSemaphoreTakeFromISR(SemaphoreHandle_t xSemaphore,
                                  BaseType_t       *pxHigherPriorityTaskWoken);

/// Take a recursive mutex (blocking).
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t xMutex, TickType_t xTicksToWait);

/// Give (release) a semaphore or mutex.
/// \return pdTRUE on success, pdFALSE if max count would be exceeded.
BaseType_t xSemaphoreGive(SemaphoreHandle_t xSemaphore);

/// Give a recursive mutex.
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t xMutex);

/// Give a binary or counting semaphore from ISR context.
/// \note Mutex give from ISR is not permitted and returns pdFALSE.
/// \param pxHigherPriorityTaskWoken  Set pdTRUE if a context switch is needed.
/// \return pdTRUE on success, pdFALSE on error or max-count overflow.
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t xSemaphore,
                                 BaseType_t       *pxHigherPriorityTaskWoken);

/// Return the current count of a counting semaphore, or 1/0 for a mutex
/// (1 = unlocked, 0 = locked).
UBaseType_t uxSemaphoreGetCount(SemaphoreHandle_t xSemaphore);

/// Return the handle of the task that currently holds a mutex.
///
/// Reads the owner field of the underlying stk::sync::Mutex under a
/// ScopedCriticalSection so the snapshot is consistent even if a concurrent
/// Unlock() is in progress.
///
/// \param xMutex  Handle of a mutex created by xSemaphoreCreateMutex() or
///                xSemaphoreCreateRecursiveMutex().  Passing a counting or
///                binary semaphore handle always returns NULL.
/// \return Handle of the owning task, or NULL if the mutex is unlocked or
///         \a xMutex is not a mutex kind.
/// \warning Not ISR-safe. Use xSemaphoreGetMutexHolderFromISR() from ISR context.
TaskHandle_t xSemaphoreGetMutexHolder(SemaphoreHandle_t xMutex);

/// Return the handle of the task that currently holds a mutex (ISR-safe).
///
/// Reads the owner field of the underlying stk::sync::Mutex with a single
/// pointer-sized load, which is naturally atomic on all supported STK
/// architectures (Cortex-M and equivalents).  No additional critical section
/// is acquired beyond what the caller already holds.
///
/// \param xMutex  Handle of a mutex.  Passing a non-mutex semaphore returns NULL.
/// \return Handle of the owning task, or NULL if the mutex is unlocked or
///         \a xMutex is not a mutex kind.
/// \warning ISR-safe.
TaskHandle_t xSemaphoreGetMutexHolderFromISR(SemaphoreHandle_t xMutex);

/* -------------------------------------------------------------------------
 * Software Timer API
 * -------------------------------------------------------------------------*/

/// Create a software timer.
/// \param pcTimerName          Name for debugging.
/// \param xTimerPeriodInTicks  Period in ticks (must be > 0).
/// \param uxAutoReload         pdTRUE = periodic, pdFALSE = one-shot.
/// \param pvTimerID            Application-defined value stored in the timer.
/// \param pxCallbackFunction   Callback invoked on expiry.
/// \return Timer handle, or NULL on failure.
TimerHandle_t xTimerCreate(const char             *pcTimerName,
                           TickType_t              xTimerPeriodInTicks,
                           UBaseType_t             uxAutoReload,
                           void                   *pvTimerID,
                           TimerCallbackFunction_t  pxCallbackFunction);

/// Create a software timer using caller-supplied storage — no heap allocation.
/// \param pcTimerName          Name for debugging.
/// \param xTimerPeriodInTicks  Period in ticks (must be > 0).
/// \param uxAutoReload         pdTRUE = periodic, pdFALSE = one-shot.
/// \param pvTimerID            Application-defined value stored in the timer.
/// \param pxCallbackFunction   Callback invoked on expiry.
/// \param pxTimerBuffer        Caller-allocated control block (StaticTimer_t).
///                             Must remain valid for the lifetime of the timer.
/// \return Timer handle. Never NULL if pxTimerBuffer is non-NULL,
///         pxCallbackFunction is non-NULL and xTimerPeriodInTicks > 0.
TimerHandle_t xTimerCreateStatic(const char             *pcTimerName,
                                  TickType_t              xTimerPeriodInTicks,
                                  UBaseType_t             uxAutoReload,
                                  void                   *pvTimerID,
                                  TimerCallbackFunction_t  pxCallbackFunction,
                                  StaticTimer_t           *pxTimerBuffer);

/// Delete a software timer and free its memory.
/// \return pdPASS.
BaseType_t xTimerDelete(TimerHandle_t xTimer, TickType_t xTicksToWait);

/// Start (or restart) a timer from the current tick.
/// \return pdPASS on success, pdFAIL on error.
BaseType_t xTimerStart(TimerHandle_t xTimer, TickType_t xTicksToWait);

/// Stop a running timer.
/// \return pdPASS on success, pdFAIL if the timer was not running.
BaseType_t xTimerStop(TimerHandle_t xTimer, TickType_t xTicksToWait);

/// Reset a timer (restart its period from the current tick).
/// \return pdPASS on success.
BaseType_t xTimerReset(TimerHandle_t xTimer, TickType_t xTicksToWait);

/// Change the period of a timer and restart it immediately.
/// \return pdPASS on success.
BaseType_t xTimerChangePeriod(TimerHandle_t xTimer,
                              TickType_t    xNewPeriod,
                              TickType_t    xTicksToWait);

/// Query whether a timer is currently active (running).
/// \return pdTRUE if active, pdFALSE if stopped or expired.
BaseType_t xTimerIsTimerActive(TimerHandle_t xTimer);

/// Return the application-defined ID stored in a timer.
void *pvTimerGetTimerID(TimerHandle_t xTimer);

/// Set the application-defined ID stored in a timer.
void vTimerSetTimerID(TimerHandle_t xTimer, void *pvNewID);

/// Return the name string of a timer.
const char *pcTimerGetName(TimerHandle_t xTimer);

/// Return the period of a timer in ticks.
TickType_t xTimerGetPeriod(TimerHandle_t xTimer);

/// Return the absolute tick count at which the timer will next expire.
/// Returns 0 if the timer is not currently running.
TickType_t xTimerGetExpiryTime(TimerHandle_t xTimer);

/* -------------------------------------------------------------------------
 * Timer ISR API
 *
 * These functions are safe to call from an interrupt service routine.  They
 * post a command to the TimerHost command queue (a stk::sync::PipeT write
 * with NO_WAIT) and return immediately without blocking.
 *
 * The xTicksToWait parameter is accepted for FreeRTOS API compatibility but
 * is always ignored: the STK command queue write is non-blocking by design
 * (PipeT::TryWrite / NO_WAIT), so there is no meaningful timeout to honour
 * from ISR context.  If the command queue is full the function returns pdFAIL.
 *
 * pxHigherPriorityTaskWoken is always set to pdFALSE.  STK's tick task wakes
 * itself via the command queue CV, which is handled internally by the
 * scheduler without requiring the ISR to request a manual context switch.
 * -------------------------------------------------------------------------*/

/// Start (or restart) a timer from ISR context.
/// \param pxHigherPriorityTaskWoken Always set to pdFALSE.
/// \return pdPASS on success, pdFAIL if the command queue is full or handle is invalid.
BaseType_t xTimerStartFromISR(TimerHandle_t xTimer,
                               BaseType_t   *pxHigherPriorityTaskWoken);

/// Stop a running timer from ISR context.
/// \param pxHigherPriorityTaskWoken Always set to pdFALSE.
/// \return pdPASS on success, pdFAIL if the timer is not running or handle is invalid.
BaseType_t xTimerStopFromISR(TimerHandle_t xTimer,
                              BaseType_t   *pxHigherPriorityTaskWoken);

/// Reset (restart the period of) a timer from ISR context.
/// \param pxHigherPriorityTaskWoken Always set to pdFALSE.
/// \return pdPASS on success, pdFAIL if the command queue is full or handle is invalid.
BaseType_t xTimerResetFromISR(TimerHandle_t xTimer,
                               BaseType_t   *pxHigherPriorityTaskWoken);

/// Change the period of a timer and restart it from ISR context.
/// \param xNewPeriod                New timer period in ticks (must be > 0).
/// \param pxHigherPriorityTaskWoken Always set to pdFALSE.
/// \return pdPASS on success, pdFAIL if the command queue is full or arguments are invalid.
BaseType_t xTimerChangePeriodFromISR(TimerHandle_t xTimer,
                                      TickType_t    xNewPeriod,
                                      BaseType_t   *pxHigherPriorityTaskWoken);

/// Defer execution of a function to the timer-task context (task-context variant).
///
/// Writes a PendCall record {xFunctionToPend, pvParameter1, ulParameter2} into
/// the static g_PendCallPipe (PipeT<PendCall, FREERTOS_STK_PEND_CALL_QUEUE_SIZE>).
/// The TimerHost handler task drains the pipe on each wake cycle and invokes
/// every callback directly — no heap allocation is performed, no self-deleting
/// timer object is created.
///
/// \note  Requires the TimerHost to have been started (at least one timer
///        created or xTimerPendFunctionCall called after vTaskStartScheduler).
///        If the TimerHost has not yet been initialized this function initializes
///        it implicitly, matching FreeRTOS behaviour.
///
/// \note  If the static pipe is full this function blocks for up to \a xTicksToWait
///        ticks waiting for a free slot (PipeT::Write blocking semantics).
///        Pass 0 for a non-blocking attempt.
///
/// \param xFunctionToPend  Callback to invoke in the timer-task context.
///                         Signature: void cb(void *pvParam1, uint32_t ulParam2).
/// \param pvParameter1     First argument forwarded to the callback.
/// \param ulParameter2     Second argument forwarded to the callback.
/// \param xTicksToWait     Ticks to wait if the pipe is full.
///                         portMAX_DELAY blocks indefinitely.
/// \return pdPASS on success, pdFAIL if the pipe was full and the timeout expired.
/// \warning ISR-unsafe.  Use xTimerPendFunctionCallFromISR() from interrupt context.
BaseType_t xTimerPendFunctionCall(PendedFunction_t xFunctionToPend,
                                   void            *pvParameter1,
                                   uint32_t         ulParameter2,
                                   TickType_t       xTicksToWait);

/// Defer execution of a function to the timer-task context (ISR-safe variant).
///
/// Identical in effect to xTimerPendFunctionCall() but safe to call from an
/// interrupt service routine.  The call record is written into a static
/// PipeT<PendCall, FREERTOS_STK_PEND_CALL_QUEUE_SIZE> with NO_WAIT semantics —
/// no heap allocation is performed.  If the pipe is full the function returns
/// pdFAIL immediately.
///
/// \param xFunctionToPend          Callback to invoke in the timer-task context.
/// \param pvParameter1             First argument forwarded to the callback.
/// \param ulParameter2             Second argument forwarded to the callback.
/// \param pxHigherPriorityTaskWoken Always set to pdFALSE (STK wakes the timer
///                                  task internally; no manual yield required).
/// \return pdPASS on success, pdFAIL if the static queue was full.
BaseType_t xTimerPendFunctionCallFromISR(PendedFunction_t  xFunctionToPend,
                                          void             *pvParameter1,
                                          uint32_t          ulParameter2,
                                          BaseType_t       *pxHigherPriorityTaskWoken);

/* -------------------------------------------------------------------------
 * Event Group API
 * -------------------------------------------------------------------------*/

/// Create an event group (all bits initialised to 0).
/// \return Event group handle, or NULL on failure.
EventGroupHandle_t xEventGroupCreate(void);

/// Create an event group using caller-supplied storage — no heap allocation.
/// \param pxEventGroupBuffer  Caller-allocated control block (StaticEventGroup_t).
///                            Must remain valid for the lifetime of the event group.
/// \return Event group handle. Never NULL if pxEventGroupBuffer is non-NULL.
EventGroupHandle_t xEventGroupCreateStatic(StaticEventGroup_t *pxEventGroupBuffer);

/// Delete an event group and free its memory.
void vEventGroupDelete(EventGroupHandle_t xEventGroup);

/// Set one or more bits in an event group (ISR-safe).
/// \return Value of the event group bits after the set operation.
EventBits_t xEventGroupSetBits(EventGroupHandle_t xEventGroup,
                               EventBits_t        uxBitsToSet);

/// Clear one or more bits in an event group (ISR-safe).
/// \return Value of the bits BEFORE clearing.
EventBits_t xEventGroupClearBits(EventGroupHandle_t xEventGroup,
                                 EventBits_t        uxBitsToClear);

/// Return the current event group bits without blocking or modifying them.
EventBits_t xEventGroupGetBits(EventGroupHandle_t xEventGroup);

/// Block the calling task until the specified bit condition is met.
/// \param uxBitsToWaitFor  Bitmask of bits to watch.
/// \param xClearOnExit     pdTRUE = atomically clear matched bits on return.
/// \param xWaitForAllBits  pdTRUE = AND semantics, pdFALSE = OR semantics.
/// \param xTicksToWait     Maximum ticks to block. portMAX_DELAY = wait forever.
/// \return Bits that caused unblocking (before optional clear), or 0 on timeout.
EventBits_t xEventGroupWaitBits(EventGroupHandle_t xEventGroup,
                                EventBits_t        uxBitsToWaitFor,
                                BaseType_t         xClearOnExit,
                                BaseType_t         xWaitForAllBits,
                                TickType_t         xTicksToWait);

/// Set event group bits from ISR context.
/// \param pxHigherPriorityTaskWoken  Set pdTRUE if a context switch is needed.
/// \return pdPASS on success, pdFAIL on invalid bits argument.
BaseType_t xEventGroupSetBitsFromISR(EventGroupHandle_t  xEventGroup,
                                     EventBits_t         uxBitsToSet,
                                     BaseType_t         *pxHigherPriorityTaskWoken);

/// Clear event group bits from ISR context.
/// \return Value of the bits BEFORE clearing.
EventBits_t xEventGroupClearBitsFromISR(EventGroupHandle_t xEventGroup,
                                        EventBits_t        uxBitsToClear);

/// Task rendezvous / barrier synchronization using an event group.
///
/// Atomically sets \a uxBitsToSet in the event group, then blocks until
/// \a uxBitsToWaitFor are all set (AND semantics).  On success, all bits
/// in \a uxBitsToWaitFor are atomically cleared before returning.
///
/// \note   This is the standard FreeRTOS "event group sync" / barrier
///         primitive.  All N participating tasks call xEventGroupSync() with
///         the same \a uxBitsToWaitFor mask and non-overlapping individual
///         \a uxBitsToSet masks (one bit per task).  The call returns only
///         after every task has set its bit.
///
/// \note   The set and the wait are performed without releasing the CPU
///         between them, so no participating task can observe a state in
///         which this task has not yet entered the wait.  This matches the
///         FreeRTOS reference implementation guarantee.
///
/// \param  xEventGroup     Event group handle (must not be NULL).
/// \param  uxBitsToSet     Bit(s) this task sets to signal it has reached
///                         the sync point (must be a subset of uxBitsToWaitFor).
/// \param  uxBitsToWaitFor Bitmask of ALL bits that must be set before any
///                         participating task is released (AND semantics).
/// \param  xTicksToWait    Maximum ticks to wait. portMAX_DELAY = wait forever.
///                         NO_WAIT (0) performs a non-blocking test.
/// \return Value of the event group bits at the point the condition was met
///         (before the bits were cleared), or 0 if the timeout expired before
///         all bits were set.
/// \warning ISR-unsafe (blocking). Not callable from an ISR context.
EventBits_t xEventGroupSync(EventGroupHandle_t xEventGroup,
                            EventBits_t        uxBitsToSet,
                            EventBits_t        uxBitsToWaitFor,
                            TickType_t         xTicksToWait);

/* -------------------------------------------------------------------------
 * Task Notification API
 *
 * Non-indexed variants always operate on slot 0 and are thin wrappers around
 * the indexed variants below.  Indexed variants accept a slot index in the
 * range [0, configTASK_NOTIFICATION_ARRAY_ENTRIES).
 * -------------------------------------------------------------------------*/

/// Send a notification to a task (slot 0), incrementing its notification value by 1.
/// Equivalent to xTaskNotify(xTaskToNotify, 0, eIncrement).
/// \return pdPASS.
BaseType_t xTaskNotifyGive(TaskHandle_t xTaskToNotify);

/// Receive a notification (slot 0), optionally clearing or decrementing the value.
/// \param ulClearCountOnExit  pdTRUE = clear to 0 on exit, pdFALSE = decrement by 1.
/// \param xTicksToWait        Ticks to wait for a notification.
/// \return Notification value before the clear/decrement, or 0 on timeout.
uint32_t ulTaskNotifyTake(BaseType_t ulClearCountOnExit, TickType_t xTicksToWait);

/// Send a notification to a task (slot 0) with a specific action on its value.
/// \param ulValue   Value applied according to eAction.
/// \param eAction   How ulValue is applied to the task's notification value.
/// \return pdPASS, or pdFAIL if eSetValueWithoutOverwrite and already pending.
BaseType_t xTaskNotify(TaskHandle_t  xTaskToNotify,
                       uint32_t      ulValue,
                       eNotifyAction eAction);

/// Wait for a notification (slot 0), with optional bit-masking on entry and exit.
/// \param ulBitsToClearOnEntry  Bits cleared before blocking.
/// \param ulBitsToClearOnExit   Bits cleared before returning.
/// \param pulNotificationValue  Receives the value before the exit-clear (may be NULL).
/// \param xTicksToWait          Ticks to wait.
/// \return pdTRUE if notified, pdFALSE on timeout.
BaseType_t xTaskNotifyWait(uint32_t   ulBitsToClearOnEntry,
                           uint32_t   ulBitsToClearOnExit,
                           uint32_t  *pulNotificationValue,
                           TickType_t xTicksToWait);

/// Send a notification (slot 0) from ISR context.
/// \param pxHigherPriorityTaskWoken  Set pdTRUE if a context switch is needed (always pdFALSE in STK wrapper).
BaseType_t xTaskNotifyFromISR(TaskHandle_t  xTaskToNotify,
                              uint32_t      ulValue,
                              eNotifyAction eAction,
                              BaseType_t   *pxHigherPriorityTaskWoken);

/* -------------------------------------------------------------------------
 * Task Notification — Indexed API
 *
 * Each task has configTASK_NOTIFICATION_ARRAY_ENTRIES independent notification
 * slots, indexed 0 .. configTASK_NOTIFICATION_ARRAY_ENTRIES-1.  Each slot
 * is backed by its own stk::sync::Semaphore + value word + pending flag,
 * matching the per-slot isolation guarantee of the FreeRTOS reference implementation.
 *
 * Out-of-range uxIndexToNotify / uxIndexToWait causes an assertion in debug
 * builds and is treated as a no-op / failure in release builds.
 * -------------------------------------------------------------------------*/

/// Send a notification to a specific slot of a task, incrementing the slot's value by 1.
/// \param uxIndexToNotify  Notification slot index (0 .. configTASK_NOTIFICATION_ARRAY_ENTRIES-1).
/// \return pdPASS, or pdFAIL if the index is out of range.
BaseType_t xTaskNotifyGiveIndexed(TaskHandle_t xTaskToNotify,
                                  UBaseType_t  uxIndexToNotify);

/// Receive a notification from a specific slot of the calling task.
/// \param uxIndexToWait      Notification slot index.
/// \param ulClearCountOnExit pdTRUE = clear slot value to 0, pdFALSE = decrement by 1.
/// \param xTicksToWait       Ticks to wait.
/// \return Slot value before the clear/decrement, or 0 on timeout or bad index.
uint32_t ulTaskNotifyTakeIndexed(UBaseType_t uxIndexToWait,
                                 BaseType_t  ulClearCountOnExit,
                                 TickType_t  xTicksToWait);

/// Send a notification to a specific slot of a task with a chosen action.
/// \param uxIndexToNotify  Notification slot index.
/// \param ulValue          Value applied according to eAction.
/// \param eAction          How ulValue is applied to the slot's value.
/// \return pdPASS, or pdFAIL on bad index or eSetValueWithoutOverwrite conflict.
BaseType_t xTaskNotifyIndexed(TaskHandle_t  xTaskToNotify,
                               UBaseType_t   uxIndexToNotify,
                               uint32_t      ulValue,
                               eNotifyAction eAction);

/// Wait for a notification on a specific slot of the calling task.
/// \param uxIndexToWait        Notification slot index.
/// \param ulBitsToClearOnEntry Bits cleared in the slot value before blocking.
/// \param ulBitsToClearOnExit  Bits cleared in the slot value before returning.
/// \param pulNotificationValue Receives the slot value before the exit-clear (may be NULL).
/// \param xTicksToWait         Ticks to wait.
/// \return pdTRUE if notified, pdFALSE on timeout or bad index.
BaseType_t xTaskNotifyWaitIndexed(UBaseType_t  uxIndexToWait,
                                   uint32_t     ulBitsToClearOnEntry,
                                   uint32_t     ulBitsToClearOnExit,
                                   uint32_t    *pulNotificationValue,
                                   TickType_t   xTicksToWait);

/// Send a notification to a specific slot from ISR context.
/// \param uxIndexToNotify           Notification slot index.
/// \param pxHigherPriorityTaskWoken Always set to pdFALSE (STK handles wake internally).
BaseType_t xTaskNotifyFromISRIndexed(TaskHandle_t  xTaskToNotify,
                                      UBaseType_t   uxIndexToNotify,
                                      uint32_t      ulValue,
                                      eNotifyAction eAction,
                                      BaseType_t   *pxHigherPriorityTaskWoken);

/* -------------------------------------------------------------------------
 * Task Notification — AndQuery / StateClear / ValueClear extensions
 *
 * These functions extend the notification API with query-on-send and
 * targeted clear operations, matching the FreeRTOS 10.4+ additions.
 * -------------------------------------------------------------------------*/

/// Send a notification to a task (slot 0) and return the previous notification
/// value before the action was applied.
/// \param xTaskToNotify       Target task handle (must not be NULL).
/// \param ulValue             Value applied according to eAction.
/// \param eAction             How ulValue is applied to the notification value.
/// \param pulPreviousNotifyValue  Receives the slot value *before* the action
///                            is applied. May be NULL.
/// \return pdPASS, or pdFAIL if eSetValueWithoutOverwrite and notification was
///         already pending.
/// \note   ISR-safe.
BaseType_t xTaskNotifyAndQuery(TaskHandle_t  xTaskToNotify,
                               uint32_t      ulValue,
                               eNotifyAction eAction,
                               uint32_t     *pulPreviousNotifyValue);

/// Indexed variant of xTaskNotifyAndQuery.
/// \param uxIndexToNotify  Notification slot index (0 .. configTASK_NOTIFICATION_ARRAY_ENTRIES-1).
/// \note   ISR-safe.
BaseType_t xTaskNotifyAndQueryIndexed(TaskHandle_t  xTaskToNotify,
                                      UBaseType_t   uxIndexToNotify,
                                      uint32_t      ulValue,
                                      eNotifyAction eAction,
                                      uint32_t     *pulPreviousNotifyValue);

/// ISR-safe variant of xTaskNotifyAndQuery (slot 0).
/// \param pxHigherPriorityTaskWoken  Always set to pdFALSE (STK handles wake internally).
/// \note   ISR-safe.
BaseType_t xTaskNotifyAndQueryFromISR(TaskHandle_t  xTaskToNotify,
                                      uint32_t      ulValue,
                                      eNotifyAction eAction,
                                      uint32_t     *pulPreviousNotifyValue,
                                      BaseType_t   *pxHigherPriorityTaskWoken);

/// ISR-safe indexed variant of xTaskNotifyAndQuery.
/// \param uxIndexToNotify           Notification slot index.
/// \param pxHigherPriorityTaskWoken Always set to pdFALSE.
/// \note   ISR-safe.
BaseType_t xTaskNotifyAndQueryFromISRIndexed(TaskHandle_t  xTaskToNotify,
                                             UBaseType_t   uxIndexToNotify,
                                             uint32_t      ulValue,
                                             eNotifyAction eAction,
                                             uint32_t     *pulPreviousNotifyValue,
                                             BaseType_t   *pxHigherPriorityTaskWoken);

/// Clear the pending notification state for slot 0 of a task.
///
/// If a notification was pending (i.e. the task had been notified but had not
/// yet called xTaskNotifyWait() or ulTaskNotifyTake() to consume it), this
/// function clears the pending state and returns pdTRUE. If no notification
/// was pending it returns pdFALSE.
///
/// \param xTask  Handle of the task whose notification state is to be cleared.
///               NULL selects the calling task.
/// \return pdTRUE if a notification was pending and has been cleared,
///         pdFALSE if no notification was pending.
/// \note ISR-safe.
BaseType_t xTaskNotifyStateClear(TaskHandle_t xTask);

/// Indexed variant of xTaskNotifyStateClear.
/// \param xTask          Handle of the task. NULL selects the calling task.
/// \param uxIndexToClear Notification slot index (0 .. configTASK_NOTIFICATION_ARRAY_ENTRIES-1).
/// \return pdTRUE if a notification was pending, pdFALSE otherwise.
/// \note ISR-safe.
BaseType_t xTaskNotifyStateClearIndexed(TaskHandle_t xTask,
                                        UBaseType_t  uxIndexToClear);

/// Atomically clear the specified bits in the notification value of slot 0
/// of a task, and return the value of the notification word *before* the bits
/// were cleared.
///
/// \param xTask        Handle of the task. NULL selects the calling task.
/// \param ulBitsToClear  Bitmask of bits to clear (ANDed with complement).
///                     Pass 0xFFFFFFFF to clear all bits.
/// \return Notification value *before* the clear operation, or 0 on bad handle.
/// \note ISR-safe.
uint32_t ulTaskNotifyValueClear(TaskHandle_t xTask,
                                uint32_t     ulBitsToClear);

/// Indexed variant of ulTaskNotifyValueClear.
/// \param xTask          Handle of the task. NULL selects the calling task.
/// \param uxIndexToClear Notification slot index.
/// \param ulBitsToClear  Bitmask of bits to clear.
/// \return Notification value *before* the clear, or 0 on bad handle / index.
/// \note ISR-safe.
uint32_t ulTaskNotifyValueClearIndexed(TaskHandle_t xTask,
                                       UBaseType_t  uxIndexToClear,
                                       uint32_t     ulBitsToClear);

/* -------------------------------------------------------------------------
 * Thread-local storage (TLS) API
 *
 * Each task has a fixed-size array of configNUM_THREAD_LOCAL_STORAGE_POINTERS
 * void* slots, indexed 0 .. configNUM_THREAD_LOCAL_STORAGE_POINTERS-1.
 * The slots are initialised to NULL at task creation.
 * Passing NULL for xTaskToQuery/xTaskToSet selects the calling task.
 * -------------------------------------------------------------------------*/

/// Write a TLS pointer slot for a task.
/// \param xTaskToSet   Task handle, or NULL for the calling task.
/// \param xIndex       Slot index (0 .. configNUM_THREAD_LOCAL_STORAGE_POINTERS-1).
/// \param pvValue      Value to store.
void vTaskSetThreadLocalStoragePointer(TaskHandle_t xTaskToSet,
                                       BaseType_t   xIndex,
                                       void        *pvValue);

/// Read a TLS pointer slot for a task.
/// \param xTaskToQuery Task handle, or NULL for the calling task.
/// \param xIndex       Slot index (0 .. configNUM_THREAD_LOCAL_STORAGE_POINTERS-1).
/// \return Stored pointer, or NULL if the index is out of range.
void *pvTaskGetThreadLocalStoragePointer(TaskHandle_t xTaskToQuery,
                                          BaseType_t   xIndex);

/* -------------------------------------------------------------------------
 * Stream Buffer API
 *
 * A stream buffer is a lightweight, ISR-safe byte-stream FIFO backed by a
 * stk::sync::MessageQueue with msg_size = 1.  Producers write arbitrary-length
 * byte spans; consumers read arbitrary-length spans.  A trigger level can be
 * set so that Receive() blocks until at least N bytes are available.
 *
 * xStreamBufferCreate()       - heap-allocated control block + data buffer.
 * xStreamBufferCreateStatic() - caller-supplied StaticStreamBuffer_t TCB and
 *                               pucStreamBufferStorageArea data buffer (no heap).
 *
 * ISR variants (SendFromISR / ReceiveFromISR) are non-blocking (NO_WAIT).
 * Passing NULL for pxHigherPriorityTaskWoken is safe; it is always set to
 * pdFALSE because STK handles scheduling internally.
 * -------------------------------------------------------------------------*/

/// Create a dynamically allocated stream buffer.
/// \param xBufferSizeBytes   Total byte capacity of the ring buffer.
/// \param xTriggerLevelBytes Minimum bytes that must be present before a
///                           blocking Receive() wakes (1 = wake on any byte).
/// \return Handle, or NULL on allocation failure.
StreamBufferHandle_t xStreamBufferCreate(size_t xBufferSizeBytes,
                                          size_t xTriggerLevelBytes);

/// Create a statically allocated stream buffer (no heap).
/// \param xBufferSizeBytes        Total byte capacity.
/// \param xTriggerLevelBytes      Minimum bytes for Receive() to unblock.
/// \param pucStreamBufferStorageArea Caller-supplied data buffer of at least
///                                xBufferSizeBytes bytes.
/// \param pxStaticStreamBuffer    Caller-supplied TCB (StaticStreamBuffer_t).
/// \return Handle (always non-NULL if arguments are non-NULL).
StreamBufferHandle_t xStreamBufferCreateStatic(
    size_t                xBufferSizeBytes,
    size_t                xTriggerLevelBytes,
    uint8_t              *pucStreamBufferStorageArea,
    StaticStreamBuffer_t *pxStaticStreamBuffer);

/// Delete a stream buffer and free heap resources (if dynamically allocated).
void vStreamBufferDelete(StreamBufferHandle_t xStreamBuffer);

/// Write bytes into the stream buffer.
/// \param xStreamBuffer      Handle.
/// \param pvTxData           Pointer to source data.
/// \param xDataLengthBytes   Number of bytes to write.
/// \param xTicksToWait       Ticks to wait for space (0 = non-blocking).
/// \return Number of bytes actually written (may be less than requested on timeout).
size_t xStreamBufferSend(StreamBufferHandle_t xStreamBuffer,
                          const void          *pvTxData,
                          size_t               xDataLengthBytes,
                          TickType_t           xTicksToWait);

/// Write bytes from ISR context (non-blocking, NO_WAIT).
/// \param pxHigherPriorityTaskWoken Always set to pdFALSE (STK handles scheduling).
/// \return Number of bytes actually written.
size_t xStreamBufferSendFromISR(StreamBufferHandle_t  xStreamBuffer,
                                 const void           *pvTxData,
                                 size_t                xDataLengthBytes,
                                 BaseType_t           *pxHigherPriorityTaskWoken);

/// Read bytes from the stream buffer, blocking until trigger level is reached.
/// \param xStreamBuffer      Handle.
/// \param pvRxData           Destination buffer.
/// \param xBufferLengthBytes Maximum bytes to read.
/// \param xTicksToWait       Ticks to block until at least trigger bytes are available.
/// \return Number of bytes actually read (0 on timeout or empty buffer).
size_t xStreamBufferReceive(StreamBufferHandle_t xStreamBuffer,
                             void                *pvRxData,
                             size_t               xBufferLengthBytes,
                             TickType_t           xTicksToWait);

/// Read bytes from ISR context (non-blocking, NO_WAIT).
/// \param pxHigherPriorityTaskWoken Always set to pdFALSE.
/// \return Number of bytes actually read.
size_t xStreamBufferReceiveFromISR(StreamBufferHandle_t  xStreamBuffer,
                                    void                 *pvRxData,
                                    size_t                xBufferLengthBytes,
                                    BaseType_t           *pxHigherPriorityTaskWoken);

/// Return the number of bytes currently available to read.
size_t xStreamBufferBytesAvailable(StreamBufferHandle_t xStreamBuffer);

/// Return the number of free bytes available for writing.
size_t xStreamBufferSpacesAvailable(StreamBufferHandle_t xStreamBuffer);

/// Return pdTRUE if the stream buffer contains no data.
BaseType_t xStreamBufferIsEmpty(StreamBufferHandle_t xStreamBuffer);

/// Return pdTRUE if the stream buffer is full (no write space remaining).
BaseType_t xStreamBufferIsFull(StreamBufferHandle_t xStreamBuffer);

/// Discard all data and reset the stream buffer to the empty state.
/// \return pdPASS always.
BaseType_t xStreamBufferReset(StreamBufferHandle_t xStreamBuffer);

/// Reset a stream buffer to empty from ISR context.
///
/// Delegates to Pipe::Reset() which holds a ScopedCriticalSection internally.
/// Data in the buffer is discarded; tasks blocked in xStreamBufferSend() that
/// were waiting for space are woken.
///
/// \param pxHigherPriorityTaskWoken Always set to pdFALSE; STK handles scheduling.
/// \return pdPASS on success, pdFAIL if the handle is NULL.
/// \note ISR-safe.
BaseType_t xStreamBufferResetFromISR(StreamBufferHandle_t  xStreamBuffer,
                                      BaseType_t           *pxHigherPriorityTaskWoken);

/// Change the trigger level for a stream buffer.
/// \param xTriggerLevelBytes New minimum bytes for Receive() to unblock (>= 1).
/// \return pdTRUE on success, pdFALSE if xTriggerLevelBytes > buffer capacity.
BaseType_t xStreamBufferSetTriggerLevel(StreamBufferHandle_t xStreamBuffer,
                                         size_t               xTriggerLevelBytes);

/// Return the trigger level currently set on a stream buffer.
///
/// \param xStreamBuffer Handle returned by xStreamBufferCreate[Static][WithCallback]().
/// \return Current trigger level in bytes (always >= 1), or 0 if handle is NULL.
/// \note ISR-safe.
size_t xStreamBufferGetTriggerLevel(StreamBufferHandle_t xStreamBuffer);

/// Return the number of bytes available to read from a stream buffer without blocking.
///
/// For a stream buffer (pure unframed byte stream) this is equivalent to
/// xStreamBufferBytesAvailable(): the entire readable span is the "next message"
/// because stream buffers carry no length-prefix framing.  The function is
/// provided for source compatibility with code written against the FreeRTOS
/// stream/message buffer API where both buffer kinds are used interchangeably.
///
/// \param xStreamBuffer Handle returned by xStreamBufferCreate[Static][WithCallback]().
/// \return Number of bytes currently available to read (0 if empty or handle is NULL).
/// \note ISR-safe (delegates to Pipe::GetCount() which is ISR-safe on targets
///       where a size_t-aligned read is atomic).
size_t xStreamBufferNextMessageLengthBytes(StreamBufferHandle_t xStreamBuffer);

/// Create a heap-allocated stream buffer with optional send/receive callbacks.
///
/// Identical to xStreamBufferCreate() but registers per-instance callbacks
/// invoked after data is successfully written (pxSendCompletedCallback) or
/// read (pxReceiveCompletedCallback).  Either callback may be NULL.
/// Callbacks fire outside any critical section, after the transfer completes.
///
/// \param xBufferSizeBytes           Capacity of the data buffer in bytes.
/// \param xTriggerLevelBytes         Minimum bytes before Receive() unblocks.
/// \param pxSendCompletedCallback    Called after bytes are written (or NULL).
/// \param pxReceiveCompletedCallback Called after bytes are read (or NULL).
/// \return Handle, or NULL on allocation failure.
StreamBufferHandle_t xStreamBufferCreateWithCallback(
    size_t                         xBufferSizeBytes,
    size_t                         xTriggerLevelBytes,
    StreamBufferCallbackFunction_t pxSendCompletedCallback,
    StreamBufferCallbackFunction_t pxReceiveCompletedCallback);

/// Create a statically-allocated stream buffer with optional send/receive callbacks.
///
/// Identical to xStreamBufferCreateStatic() but registers per-instance callbacks.
/// Either callback may be NULL.
///
/// \param xBufferSizeBytes              Capacity of the data buffer in bytes.
/// \param xTriggerLevelBytes            Minimum bytes before Receive() unblocks.
/// \param pucStreamBufferStorageArea    Caller-supplied data buffer (>= xBufferSizeBytes bytes).
/// \param pxStaticStreamBuffer          Caller-supplied TCB (StaticStreamBuffer_t).
/// \param pxSendCompletedCallback       Called after bytes are written (or NULL).
/// \param pxReceiveCompletedCallback    Called after bytes are read (or NULL).
/// \return Handle, or NULL on invalid arguments.
StreamBufferHandle_t xStreamBufferCreateStaticWithCallback(
    size_t                         xBufferSizeBytes,
    size_t                         xTriggerLevelBytes,
    uint8_t                       *pucStreamBufferStorageArea,
    StaticStreamBuffer_t          *pxStaticStreamBuffer,
    StreamBufferCallbackFunction_t pxSendCompletedCallback,
    StreamBufferCallbackFunction_t pxReceiveCompletedCallback);

/* -------------------------------------------------------------------------
 * Message Buffer API
 *
 * A message buffer carries discrete, variable-length messages.  Each message
 * is stored as a pool block (payload) plus an envelope {size, block*} in a
 * separate queue.  Receive() always returns exactly one complete message.
 *
 * Backed by:
 *   - stk::memory::BlockMemoryPool — fixed-size payload blocks
 *     (block size = AlignBlockSize(xMaxMessageSize)).
 *   - stk::sync::MessageQueue      — envelope FIFO of {len, block*} structs,
 *     providing correct blocking on both "pool full" and "queue empty".
 *
 * xMessageBufferCreate()                    - heap-allocated control block + pool storage.
 * xMessageBufferCreateStatic()              - caller-supplied TCB and storage buffer.
 * xMessageBufferCreateWithCallback()        - heap-allocated + send/recv callbacks.
 * xMessageBufferCreateStaticWithCallback()  - static + send/recv callbacks.
 * -------------------------------------------------------------------------*/

/// Create a dynamically allocated message buffer.
/// \param xBufferSizeBytes  Total storage budget.  The implementation derives
///                          slot count as floor(xBufferSizeBytes / (AlignBlockSize(xMaxMessageSize)
///                          + sizeof(envelope))).
/// \param xMaxMessageSize   Maximum payload size per message in bytes.
/// \return Handle, or NULL on allocation failure.
MessageBufferHandle_t xMessageBufferCreate(size_t xBufferSizeBytes,
                                            size_t xMaxMessageSize);

/// Create a statically allocated message buffer (no heap).
/// \param xMaxMessageSize             Maximum payload size per message.
/// \param xMessageCount               Maximum number of in-flight messages.
/// \param pucMessageBufferStorageArea Caller-supplied flat buffer of at least
///        xMessageCount * (AlignBlockSize(xMaxMessageSize) + sizeof(envelope)) bytes.
/// \param pxStaticMessageBuffer       Caller-supplied TCB (StaticMessageBuffer_t).
/// \return Handle (always non-NULL if arguments are non-NULL).
MessageBufferHandle_t xMessageBufferCreateStatic(
    size_t                 xMaxMessageSize,
    size_t                 xMessageCount,
    uint8_t               *pucMessageBufferStorageArea,
    StaticMessageBuffer_t *pxStaticMessageBuffer);

/// Create a heap-allocated message buffer with optional send/receive callbacks.
///
/// Identical to xMessageBufferCreate() but registers per-instance callbacks
/// invoked after a message is successfully enqueued (pxSendCompletedCallback)
/// or dequeued (pxReceiveCompletedCallback).  Either callback may be NULL.
/// Callbacks fire outside any critical section, after the transfer completes.
/// The xStreamBuffer argument passed to the callback is the MessageBufferHandle_t
/// cast to StreamBufferHandle_t, matching FreeRTOS convention.
///
/// \param xBufferSizeBytes           Total storage budget (slot count derived internally).
/// \param xMaxMessageSize            Maximum payload size per message in bytes.
/// \param pxSendCompletedCallback    Called after a message is enqueued (or NULL).
/// \param pxReceiveCompletedCallback Called after a message is dequeued (or NULL).
/// \return Handle, or NULL on allocation failure.
MessageBufferHandle_t xMessageBufferCreateWithCallback(
    size_t                         xBufferSizeBytes,
    size_t                         xMaxMessageSize,
    StreamBufferCallbackFunction_t pxSendCompletedCallback,
    StreamBufferCallbackFunction_t pxReceiveCompletedCallback);

/// Create a statically-allocated message buffer with optional send/receive callbacks.
///
/// Identical to xMessageBufferCreateStatic() but registers per-instance callbacks.
/// Either callback may be NULL.
///
/// \param xMaxMessageSize              Maximum payload size per message in bytes.
/// \param xMessageCount               Maximum number of in-flight messages.
/// \param pucMessageBufferStorageArea  Caller-supplied flat storage buffer.
/// \param pxStaticMessageBuffer        Caller-supplied TCB (StaticMessageBuffer_t).
/// \param pxSendCompletedCallback      Called after a message is enqueued (or NULL).
/// \param pxReceiveCompletedCallback   Called after a message is dequeued (or NULL).
/// \return Handle, or NULL on invalid arguments.
MessageBufferHandle_t xMessageBufferCreateStaticWithCallback(
    size_t                         xMaxMessageSize,
    size_t                         xMessageCount,
    uint8_t                       *pucMessageBufferStorageArea,
    StaticMessageBuffer_t         *pxStaticMessageBuffer,
    StreamBufferCallbackFunction_t pxSendCompletedCallback,
    StreamBufferCallbackFunction_t pxReceiveCompletedCallback);

/// Delete a message buffer and free heap resources (if dynamically allocated).
void vMessageBufferDelete(MessageBufferHandle_t xMessageBuffer);

/// Send a message into the message buffer (task context, may block).
/// \param pvTxData         Pointer to message payload.
/// \param xDataLengthBytes Payload size in bytes (must be <= max message size).
/// \param xTicksToWait     Ticks to wait for a free block + envelope slot.
///                         portMAX_DELAY blocks indefinitely.
/// \return xDataLengthBytes on success, 0 on timeout or size violation.
/// \warning ISR-unsafe. Use xMessageBufferSendFromISR() from ISR context.
size_t xMessageBufferSend(MessageBufferHandle_t xMessageBuffer,
                           const void           *pvTxData,
                           size_t                xDataLengthBytes,
                           TickType_t            xTicksToWait);

/// Send a message into the message buffer from ISR context (non-blocking).
///
/// Attempts a single non-blocking pool allocation followed by a non-blocking
/// envelope enqueue.  Returns 0 immediately if either resource is unavailable.
///
/// \param pvTxData                 Pointer to message payload.
/// \param xDataLengthBytes         Payload size (must be <= max message size).
/// \param pxHigherPriorityTaskWoken Always set to pdFALSE; STK handles scheduling.
/// \return xDataLengthBytes on success, 0 if the buffer was full or arguments invalid.
/// \warning ISR-safe (non-blocking). Must not be called with a non-zero timeout.
size_t xMessageBufferSendFromISR(MessageBufferHandle_t  xMessageBuffer,
                                  const void            *pvTxData,
                                  size_t                 xDataLengthBytes,
                                  BaseType_t            *pxHigherPriorityTaskWoken);

/// Receive a message from the message buffer (task context, may block).
/// \param pvRxData           Destination buffer.
/// \param xBufferLengthBytes Capacity of pvRxData; must be >= the oldest message length.
/// \param xTicksToWait       Ticks to wait for a message. portMAX_DELAY blocks indefinitely.
/// \return Number of bytes written to pvRxData, or 0 on timeout / size violation.
/// \warning ISR-unsafe. Use xMessageBufferReceiveFromISR() from ISR context.
size_t xMessageBufferReceive(MessageBufferHandle_t xMessageBuffer,
                              void                 *pvRxData,
                              size_t                xBufferLengthBytes,
                              TickType_t            xTicksToWait);

/// Receive a message from the message buffer from ISR context (non-blocking).
///
/// Attempts to dequeue one envelope without blocking.  If an envelope is
/// available and fits in the destination buffer the payload is copied out and
/// the pool block is freed.
///
/// \param pvRxData                 Destination buffer.
/// \param xBufferLengthBytes       Capacity of pvRxData; must be >= the oldest message length.
/// \param pxHigherPriorityTaskWoken Always set to pdFALSE; STK handles scheduling.
/// \return Number of bytes written to pvRxData, or 0 if empty / destination too small.
/// \warning ISR-safe (non-blocking).
size_t xMessageBufferReceiveFromISR(MessageBufferHandle_t  xMessageBuffer,
                                     void                  *pvRxData,
                                     size_t                 xBufferLengthBytes,
                                     BaseType_t            *pxHigherPriorityTaskWoken);

/// Return pdTRUE if the message buffer contains no messages.
/// \note ISR-safe.
BaseType_t xMessageBufferIsEmpty(MessageBufferHandle_t xMessageBuffer);

/// Return pdTRUE if no more messages can be enqueued (pool exhausted).
/// \note ISR-safe.
BaseType_t xMessageBufferIsFull(MessageBufferHandle_t xMessageBuffer);

/// Return the number of free envelope slots available for sending.
/// \note ISR-safe.
size_t xMessageBufferSpacesAvailable(MessageBufferHandle_t xMessageBuffer);

/// Return the byte length of the next message that would be returned by
/// xMessageBufferReceive(), without removing it from the buffer.
///
/// Backed by stk::sync::MessageQueue::TryPeek(), which copies the oldest
/// envelope atomically without consuming it.  No dequeue-and-reinsert pair is
/// needed; the queue state is left unchanged in a single ISR-safe operation.
///
/// \return Length in bytes of the oldest pending message, or 0 if the buffer
///         is empty.
/// \note ISR-safe.
size_t xMessageBufferNextLengthBytes(MessageBufferHandle_t xMessageBuffer);

/// Discard all pending messages, return all blocks to the pool, and reset
/// the envelope queue to the empty state (task context).
/// \return pdPASS always.
/// \warning ISR-safe. Prefer xMessageBufferResetFromISR() from interrupt context
///          to properly signal pxHigherPriorityTaskWoken.
BaseType_t xMessageBufferReset(MessageBufferHandle_t xMessageBuffer);

/// Discard all pending messages and reset the buffer from ISR context.
///
/// Drains every pending envelope, frees its pool block, then resets the
/// envelope queue.  Wakes any task blocked in xMessageBufferSend() that was
/// waiting for a free slot.
///
/// \param pxHigherPriorityTaskWoken Always set to pdFALSE; STK handles scheduling.
/// \return pdPASS always.
/// \warning ISR-safe.
BaseType_t xMessageBufferResetFromISR(MessageBufferHandle_t  xMessageBuffer,
                                       BaseType_t            *pxHigherPriorityTaskWoken);

/* -------------------------------------------------------------------------
 * Heap API
 *
 * pvPortMalloc / vPortFree are thin wrappers around
 * stk::memory::MemoryAllocator::Allocate / Free — the same allocation seam
 * used internally by all STK and FreeRTOS-STK objects.  Replacing the two
 * MemoryAllocator definitions in freertos_stk.cpp (e.g. to point at a static
 * pool) automatically redirects both the internal and application-facing heap.
 *
 * xPortGetFreeHeapSize / xPortGetMinimumEverFreeHeapSize / vPortGetHeapStats
 * read from the same s_MemStats accounting structure maintained by
 * MemoryAllocator::Allocate() and ::Free().
 * -------------------------------------------------------------------------*/

/// Heap statistics snapshot filled by vPortGetHeapStats().
///
/// \note Fields xNumberOfSuccessfulAllocations and xNumberOfSuccessfulFrees
///       are reported as 0 in this implementation because the underlying
///       allocator delegates to the system \c malloc and does not maintain
///       per-call counters.  All other fields are derived from s_MemStats.
typedef struct
{
    size_t xAvailableHeapSpaceInBytes;      /*!< Current free bytes remaining.                      */
    size_t xSizeOfLargestFreeBlockInBytes;  /*!< Largest single free block (reported as free total). */
    size_t xSizeOfSmallestFreeBlockInBytes; /*!< Smallest free block (1 if any free bytes exist).   */
    size_t xNumberOfFreeBlocks;             /*!< Number of free blocks (1 if any free bytes exist).  */
    size_t xMinimumEverFreeBytesRemaining;  /*!< Lowest free byte count recorded since system start. */
    size_t xNumberOfSuccessfulAllocations;  /*!< Allocation call count (not tracked; always 0).      */
    size_t xNumberOfSuccessfulFrees;        /*!< Free call count (not tracked; always 0).            */
} HeapStats_t;

/// Allocate \a xWantedSize bytes from the system heap.
/// \return Pointer to the allocated block, or NULL on failure.
/// \note   Thread-safe if the underlying MemoryAllocator::Allocate is.
///         The default implementation delegates to \c malloc.
void *pvPortMalloc(size_t xWantedSize);

/// Free a block previously returned by pvPortMalloc().
/// \param pv Pointer to the block to free. NULL is a no-op.
/// \note  Thread-safe if the underlying MemoryAllocator::Free is.
void vPortFree(void *pv);

/// Return the number of bytes currently available in the heap.
///
/// This is a point-in-time snapshot of
/// stk::memory::MemoryAllocator::Stats::GetAvailable() and may not account
/// for in-flight allocations if called from a non-critical context.
///
/// \return Free bytes remaining.
size_t xPortGetFreeHeapSize(void);

/// Return the minimum number of free heap bytes recorded since system start.
///
/// The watermark is updated inside MemoryAllocator::Allocate() after every
/// successful allocation, so it always reflects the worst-case heap pressure
/// observed up to the point of the call.
///
/// \return Minimum ever free bytes (heap watermark).
size_t xPortGetMinimumEverFreeHeapSize(void);

/// Fill \a pxHeapStats with a snapshot of the heap statistics.
///
/// Fields that require traversal of the internal free-block list are reported
/// conservatively (see HeapStats_t documentation); all other fields are exact.
///
/// \param pxHeapStats Pointer to the HeapStats_t structure to fill.
///                    NULL is silently ignored.
void vPortGetHeapStats(HeapStats_t *pxHeapStats);

/* -------------------------------------------------------------------------
 * Convenience macros matching standard FreeRTOS naming conventions
 * -------------------------------------------------------------------------*/

#define pdMS_TO_TICKS(xTimeInMs)  ((TickType_t)(xTimeInMs)) /*!< 1 ms tick resolution.  */
#define tskIDLE_PRIORITY          ((UBaseType_t)0U)          /*!< Idle task priority.    */
#define xTaskHandle               TaskHandle_t               /*!< Legacy handle alias.   */

#ifdef __cplusplus
}
#endif

#endif /* FREERTOS_STK_H_ */
