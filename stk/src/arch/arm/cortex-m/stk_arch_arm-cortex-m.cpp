/*
 * SuperTinyKernel™ (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

// note: If missing, this header must be customized (get it in the root of the source folder) and
//       copied to the /include folder manually.
#include "stk_config.h"

#ifdef _STK_ARCH_ARM_CORTEX_M

#include <stdlib.h>
#include <setjmp.h>

//#define STK_CORTEX_M_TRUSTZONE

#ifdef STK_CORTEX_M_TRUSTZONE
#include <arm_cmse.h>
#endif

#include "stk_arch.h"
#include "arch/stk_arch_common.h"

using namespace stk;

#if (defined(__clang__) && defined(__ARMCOMPILER_VERSION)) || defined(__ICCARM__)
#define STK_CORTEX_M_DISABLE_INTERRUPTS() __asm volatile("CPSID i" : : : "memory")
#define STK_CORTEX_M_ENABLE_INTERRUPTS() __asm volatile("CPSIE i" : : : "memory")
#else
#define STK_CORTEX_M_DISABLE_INTERRUPTS() __disable_irq()
#define STK_CORTEX_M_ENABLE_INTERRUPTS() __enable_irq()
#endif

#define STK_CORTEX_M_CRITICAL_SECTION_START(SES)\
    do { SES = __get_PRIMASK(); STK_CORTEX_M_DISABLE_INTERRUPTS(); __DSB(); __ISB(); } while (0)
#define STK_CORTEX_M_CRITICAL_SECTION_END(SES)\
    do { __DSB(); __ISB(); __set_PRIMASK(SES); } while (0)

#ifdef CONTROL_nPRIV_Msk
    static __stk_forceinline bool STK_CORTEX_M_SPIN_LOCK_TRYLOCK(volatile bool &LOCK)
    {
        return __atomic_test_and_set(&LOCK, __ATOMIC_ACQUIRE);
    }
    static __stk_forceinline void STK_CORTEX_M_SPIN_LOCK_LOCK(volatile bool &LOCK)
    {
        uint32_t timeout = 0xFFFFFF;
        while (STK_CORTEX_M_SPIN_LOCK_TRYLOCK(LOCK))
        {
            if (--timeout == 0)
            {
                // Invariant violated: the lock owner exited without releasing,
                // Kernel state is suspect, enter defined safe state.
                STK_KERNEL_PANIC(KERNEL_PANIC_SPINLOCK_DEADLOCK);
            }
            __stk_relax_cpu();
        }
    }
    static __stk_forceinline void STK_CORTEX_M_SPIN_LOCK_UNLOCK(volatile bool &LOCK)
    {
        // ensure all data writes (like scheduling metadata) are flushed before the lock is released
        __asm volatile("dmb ishst" ::: "memory");
        __atomic_clear(&LOCK, __ATOMIC_RELEASE);
    }
#elif defined(RP2040_H)
    // Raspberry RP2040 dual-core M0+ implementation, using Hardware Spinlock 0 (SIO base 0xd0000000 + offset)
    #define STK_SIO_SPINLOCK SIO->SPINLOCK31
    static __stk_forceinline bool STK_CORTEX_M_SPIN_LOCK_TRYLOCK(volatile bool &LOCK)
    {
        return (STK_SIO_SPINLOCK == 0 ? true : ((LOCK) = true, false));
    }
    static __stk_forceinline void STK_CORTEX_M_SPIN_LOCK_LOCK(volatile bool &LOCK)
    {
        uint32_t timeout = 0xFFFFFF;
        while (STK_CORTEX_M_SPIN_LOCK_TRYLOCK(LOCK))
        {
            if (--timeout == 0)
            {
                // Invariant violated: the lock owner exited without releasing,
                // Kernel state is suspect, enter defined safe state.
                STK_KERNEL_PANIC(KERNEL_PANIC_SPINLOCK_DEADLOCK);
            }
            __stk_relax_cpu();
        }
    }
    static __stk_forceinline void STK_CORTEX_M_SPIN_LOCK_UNLOCK(volatile bool &LOCK)
    {
        __DMB();
        (LOCK) = false;
        STK_SIO_SPINLOCK = 1; /* writing any value releases the hardware lock */
    }
    #undef STK_SIO_SPINLOCK
#else
    // Standard single-core Cortex-M0 implementation:
    static __stk_forceinline bool STK_CORTEX_M_SPIN_LOCK_TRYLOCK(volatile bool &LOCK)
    {
        if (LOCK)
            return true;

        LOCK = true;
        __DMB();

        return false;
    }
    static __stk_forceinline void STK_CORTEX_M_SPIN_LOCK_LOCK(volatile bool &LOCK)
    {
        uint32_t timeout = 0xFFFFFF;
        while (LOCK == true)
        {
            if (--timeout == 0)
            {
                // Invariant violated: the lock owner exited without releasing,
                // Kernel state is suspect, enter defined safe state.
                STK_KERNEL_PANIC(KERNEL_PANIC_SPINLOCK_DEADLOCK);
            }
            __stk_relax_cpu();
        }

        LOCK = true;
    }
    static __stk_forceinline void STK_CORTEX_M_SPIN_LOCK_UNLOCK(volatile bool &LOCK)
    {
        __DMB();
        LOCK = false;
    }
#endif

#define STK_CORTEX_M_EXC_RETURN_HANDLR_MSP 0xFFFFFFF1 // Handler mode, MSP stack (non-secure)
#define STK_CORTEX_M_EXC_RETURN_THREAD_MSP 0xFFFFFFF9 // Thread mode, MSP stack (non-secure)
#define STK_CORTEX_M_EXC_RETURN_THREAD_PSP 0xFFFFFFFD // Thread mode, PSP stack (non-secure)

#define STK_CORTEX_M_ISR_PRIORITY_HIGHEST 0
#define STK_CORTEX_M_ISR_PRIORITY_LOWEST 0xFF

enum ESvc
{
    SVC_START_SCHEDULING = 0,
    SVC_FORCE_SWITCH,
    SVC_ENTER_CRITICAL,
    SVC_EXIT_CRITICAL
};

#define STK_CORTEX_M_EXIT_FUNCTION() __asm volatile("BX LR")
#define STK_CORTEX_M_START_SCHEDULING() __asm volatile("SVC %0" : : "I"(SVC_START_SCHEDULING));
#define STK_CORTEX_M_FORCE_SWITCH() __asm volatile("SVC %0" : : "I"(SVC_FORCE_SWITCH));
#define STK_CORTEX_M_UNPRIV_ENTER_CRITICAL() __asm volatile("SVC %0" : : "I"(SVC_ENTER_CRITICAL));
#define STK_CORTEX_M_UNPRIV_EXIT_CRITICAL() __asm volatile("SVC %0" : : "I"(SVC_EXIT_CRITICAL));

//! Do sanity check for a compiler define, __CORTEX_M must be defined.
#ifndef __CORTEX_M
#error Expecting __CORTEX_M with value corresponding to Cortex-M model (0, 3, 4, ...)!
#endif

//! If defined, assume CPU may support FPU.
#if (__FPU_PRESENT == 1) && (__FPU_USED == 1)
    #define STK_CORTEX_M_EXPECT_FPU
#endif

//! If defined, manage Link Register (LR) per task.
#if (__CORTEX_M >= 3)
    #define STK_CORTEX_M_MANAGE_LR
#endif

//! Number of registers kept in stack.
#ifdef STK_CORTEX_M_MANAGE_LR
    #define STK_CORTEX_M_REGISTER_COUNT 17
#else
    #define STK_CORTEX_M_REGISTER_COUNT 16
#endif

//! SysTick_Handler
#ifndef STK_SYSTICK_HANDLER
    #define STK_SYSTICK_HANDLER SysTick_Handler
#endif

//! PendSV_Handler
#ifndef STK_PENDSV_HANDLER
    #define STK_PENDSV_HANDLER PendSV_Handler
#endif

//! SVC_Handler
#ifndef STK_SVC_HANDLER
    #define STK_SVC_HANDLER SVC_Handler
#endif

// Declarations:
extern "C" void SVC_Handler_Main(Word *svc_args) __stk_attr_used; // __stk_attr_used required for Link-Time Optimization (-flto)

//! Check if caller is in Handler Mode (IPSR != 0), i.e. inside ISR.
static __stk_forceinline bool IsHandlerMode() { return (__get_IPSR() != 0); }

//! Check if caller is in Privileged Thread Mode (nPRIV == 0), note that ARM Cortex-M0 is always Privileged Thread Mode
static __stk_forceinline bool IsPrivilegedThreadMode()
{
#ifdef CONTROL_nPRIV_Msk
    return ((__get_CONTROL() & CONTROL_nPRIV_Msk) == 0);
#else
    return true;
#endif
}

/*! \brief  Enter critical section.
    \note   Unprivileged mode only. No input, but returns a value (will be read from R0).
    \return Previous value of BASEPRI.
*/
__stk_attr_naked Word SVC_EnterCritical()
{
    STK_CORTEX_M_UNPRIV_ENTER_CRITICAL();
    STK_CORTEX_M_EXIT_FUNCTION();
}

/*! \brief     Exit critical section.
    \param[in] prev: Previous value of BASEPRI.
    \note      Unprivileged mode only. Input 'state' is passed in R0; no return value.
    \see       SVC_EnterCritical
*/
__stk_attr_naked void SVC_ExitCritical(Word /*prev*/)
{
    STK_CORTEX_M_UNPRIV_EXIT_CRITICAL();
    STK_CORTEX_M_EXIT_FUNCTION();
}

/*! \brief  Get SP of the calling process.
    \return SP register value.
*/
static __stk_forceinline Word GetCallerSP()
{
    // __get_PSP() returns 0 in unprivileged mode, thus get SP (R13) which is available for both modes
#if 0
    return __get_PSP();
#else
    uint32_t sp;
    __asm volatile("MOV %0, SP" : "=r" (sp));
    return sp;
#endif
}

/*! \brief Schedule context switch via the PendSV interrupt.
*/
static __stk_forceinline void ScheduleContextSwitch()
{
    SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;
}

/*! \brief Clear FPU state.
*/
static __stk_forceinline void ClearFpuState()
{
#ifdef STK_CORTEX_M_EXPECT_FPU
    __set_CONTROL(__get_CONTROL() & ~CONTROL_FPCA_Msk);
#endif
}

/*! \brief Enable FPU.
*/
static __stk_forceinline void EnableFullFpuAccess()
{
#ifdef STK_CORTEX_M_EXPECT_FPU
    // enable FPU CP10/CP11 Secure and Non-secure register access
    SCB->CPACR |= (0b11 << 20) | (0b11 << 22);
#endif
}

//! Global lock to synchronize critical sections of multiple cores.
static volatile bool g_CsuLock = false;

//! Internal context.
static struct Context : public PlatformContext
{
    Context() : PlatformContext(), m_exit_buf(), m_overrider(nullptr), m_csu(0), m_csu_nesting(0),
        m_started(false), m_exiting(false)
    {}

    void Initialize(IPlatform::IEventHandler *handler, IKernelService *service, Stack *exit_trap, int32_t resolution_us)
    {
        PlatformContext::Initialize(handler, service, exit_trap, resolution_us);

        // we expect Stack::mode member at offset of 0 (first member).
        STK_STATIC_ASSERT(offsetof(Stack, SP) == 0);
        // we expect Stack::mode member at offset of 4 (second member).
        STK_STATIC_ASSERT(offsetof(Stack, mode) == 4);

        m_csu         = 0;
        m_csu_nesting = 0;
        m_overrider   = nullptr;
        m_started     = false;
        m_exiting     = false;

    #if STK_SEGGER_SYSVIEW
        SEGGER_SYSVIEW_Init(SystemCoreClock, SystemCoreClock, nullptr, SendSysDesc);
    #endif
    }

    __stk_forceinline void OnTick()
    {
        STK_CORTEX_M_DISABLE_INTERRUPTS();

        if (m_handler->OnTick(&m_stack_idle, &m_stack_active))
        {
        #if STK_SEGGER_SYSVIEW
            SEGGER_SYSVIEW_OnTaskStopExec();
            if (GetContext().m_stack_active->tid != SYS_TASK_ID_SLEEP)
                SEGGER_SYSVIEW_OnTaskStartExec(GetContext().m_stack_active->tid);
        #endif

            ScheduleContextSwitch();
        }

        STK_CORTEX_M_ENABLE_INTERRUPTS();
    }

    __stk_forceinline void EnterCriticalSection()
    {
        // disable local interrupts first to prevent self-deadlock
        uint32_t current_ses;
        STK_CORTEX_M_CRITICAL_SECTION_START(current_ses);

        if (m_csu_nesting == 0)
        {
            // ONLY attempt the global spinlock if we aren't already nested
            STK_CORTEX_M_SPIN_LOCK_LOCK(g_CsuLock);

            // store the hardware interrupt state to restore later
            m_csu = current_ses;
        }

        ++m_csu_nesting;
    }

    __stk_forceinline void ExitCriticalSection()
    {
        STK_ASSERT(m_csu_nesting != 0);
        --m_csu_nesting;

        if (m_csu_nesting == 0)
        {
            // capture the state before releasing lock
            uint32_t ses_to_restore = m_csu;

            // release global lock
            STK_CORTEX_M_SPIN_LOCK_UNLOCK(g_CsuLock);

            // restore hardware interrupts
            STK_CORTEX_M_CRITICAL_SECTION_END(ses_to_restore);
        }
    }

    // Unprivileged
    __stk_forceinline void UnprivEnterCriticalSection()
    {
        // elevate to privileged/disabled state via SVC
        uint32_t current_ses = SVC_EnterCritical();

        if (m_csu_nesting == 0)
        {
            // ONLY attempt the global spinlock if we aren't already nested
            STK_CORTEX_M_SPIN_LOCK_LOCK(g_CsuLock);

            // store the hardware interrupt state to restore later
            m_csu = current_ses;
        }

        ++m_csu_nesting;
    }

    // Unprivileged
    __stk_forceinline void UnprivExitCriticalSection()
    {
        STK_ASSERT(m_csu_nesting != 0);
        --m_csu_nesting;

        if (m_csu_nesting == 0)
        {
            // capture the state before releasing lock
            uint32_t ses_to_restore = m_csu;

            // release global lock
            STK_CORTEX_M_SPIN_LOCK_UNLOCK(g_CsuLock);

            // restore hardware interrupts via SVC
            SVC_ExitCritical(ses_to_restore);
        }
    }

    void Start()
    {
        m_exiting = false;

        // save jump location of the Exit trap
        setjmp(m_exit_buf);
        if (m_exiting)
            return;

        STK_CORTEX_M_START_SCHEDULING();
    }

    void OnStart();
    void OnStop();

    typedef IPlatform::IEventOverrider eovrd_t;

    jmp_buf       m_exit_buf;    //!< saved context of the exit point
    eovrd_t      *m_overrider;   //!< platform events overrider
    uint32_t      m_csu;         //!< user critical session
    uint32_t      m_csu_nesting; //!< depth of user critical session nesting
    volatile bool m_started;     //!< 'true' when in started state
    bool          m_exiting;     //!< 'true' when is exiting the scheduling process
}
g_Context[STK_ARCH_CPU_COUNT];

void PlatformArmCortexM::ProcessTick()
{
    GetContext().OnTick();
}

__stk_attr_noinline  // keep out of inlining to preserve stack frame
__stk_attr_noreturn  // never returns - a trap
void STK_PANIC_HANDLER_DEFAULT(EKernelPanicId id)
{
    (void)id;

    // disable all maskable interrupts: this prevents scheduler from running again and corrupting state further
    STK_CORTEX_M_DISABLE_INTERRUPTS();

    // spin forever: with a watchdog active this produces a clean reset, without a watchdog,
    // a debugger can attach and inspect 'id'
    for (;;)
    {
        __stk_relax_cpu();
    }
}

extern "C" void STK_SYSTICK_HANDLER()
{
#if STK_SEGGER_SYSVIEW
    SEGGER_SYSVIEW_RecordEnterISR();
#endif

#ifdef HAL_MODULE_ENABLED // STM32 HAL
    // make sure STM32 HAL get timing information as it depends on SysTick in delaying procedures
    HAL_IncTick();

    // STM32 HAL is starting SysTick on its initialization that will cause a crash on NULL,
    // therefore use additional check if HAL_MODULE_ENABLED is defined
    if (GetContext().m_started)
    {
#else
    {
        // make sure SysTick is enabled by the Kernel::Start(), disable its start anywhere else
        STK_ASSERT(GetContext().m_started);
        STK_ASSERT(GetContext().m_handler != nullptr);
#endif
        GetContext().OnTick();
    }

#if STK_SEGGER_SYSVIEW
    SEGGER_SYSVIEW_RecordExitISR();
    SEGGER_SYSVIEW_IsStarted();
#endif
}

/*! \def   STK_ASM_BLOCK_PRIVILEGE_MODE
    \brief Set privileged mode, an equivalent of the following C code:

    if (GetContext().m_stack_active->mode == ACCESS_PRIVILEGED)
        __set_CONTROL(__get_CONTROL() & ~CONTROL_nPRIV_Msk);
    else
        __set_CONTROL(__get_CONTROL() | CONTROL_nPRIV_Msk)
*/
#define STK_ASM_BLOCK_PRIVILEGE_MODE\
    "LDR        r1, %[st_active]\n" /* r1 = Stack* (m_stack_active) */ \
    "LDR        r1, [r1, #4]    \n" /* r1 = m_stack_active->mode (offset 4), reuse r1 */ \
    "CMP        r1, %[priv_val] \n" /* compare with ACCESS_PRIVILEGED */ \
    "BEQ        1f              \n" /* if equal, privileged mode */\
    /* unprivileged mode: set CONTROL.nPRIV = 1 */\
    "MRS        r0, CONTROL     \n"\
    "ORR        r0, r0, #1      \n"\
    "MSR        CONTROL, r0     \n"\
    "B          2f              \n"\
    "1:                         \n"\
    /* privileged mode: set CONTROL.nPRIV = 0 */\
    "MRS        r0, CONTROL     \n"\
    "BIC        r0, r0, #1      \n"\
    "MSR        CONTROL, r0     \n"\
    "2:                         \n"

extern "C" __stk_attr_naked void STK_PENDSV_HANDLER()
{
    __asm volatile(
    ".syntax unified             \n"

    "CPSID      i                \n" /* inline STK_CORTEX_M_DISABLE_INTERRUPTS */

    // save stack to inactive (idle) task
    "MRS        r0, psp          \n"

#ifdef STK_CORTEX_M_EXPECT_FPU
    // save FP registers
    "TST        LR, #16          \n" /* test LR for 0xffffffe_, e.g. Thread mode with FP data */
    "IT         EQ               \n"
    "VSTMDBEQ   r0!, {s16-s31}   \n" /* store 16 SP registers */
#endif

    // save general registers
#ifdef STK_CORTEX_M_MANAGE_LR
    // save r4-r11 and LR
    // note: for Cortex-M3 and higher save LR to keep correct Thread state of the task when it is restored
    "STMDB      r0!, {r4-r11, LR}\n"
#else
    // note: STMIA is limited to r0-r7 range, therefore save via stack memory
    "SUBS       r0, r0, #16      \n" /* decrement for r4-r7 */
    "STMIA      r0!, {r4-r7}     \n"
    "MOV        r4, r8           \n"
    "MOV        r5, r9           \n"
    "MOV        r6, r10          \n"
    "MOV        r7, r11          \n"
    "SUBS       r0, r0, #32      \n" /* decrement for r8-r11 and pointer reset */
    "STMIA      r0!, {r4-r7}     \n"
    "SUBS       r0, r0, #16      \n" /* final pointer adjustment */
#endif

    // store in GetContext().m_stack_idle
    "LDR        r1, %[st_idle]   \n"
    "STR        r0, [r1]         \n" /* store the first member (SP) from r0 */

    // set privileged/unprivileged mode for the active stack
#ifdef CONTROL_nPRIV_Msk
    STK_ASM_BLOCK_PRIVILEGE_MODE
#endif

    // load stack of the active task from GetContext().m_stack_active (note: keep in sync with OnTaskStart)
    "LDR        r1, %[st_active] \n"
    "LDR        r0, [r1]         \n" /* load the first member of Stack (Stack::SP) into r0 */

    // restore general registers
#ifdef STK_CORTEX_M_MANAGE_LR
    // restore r4-r11 and LR
    "LDMIA      r0!, {r4-r11, LR}\n"
#else
    // note: LDMIA is limited to r0-r7 range, therefore load via stack memory
    "LDMIA      r0!, {r4-r7}     \n"
    "MOV        r8, r4           \n"
    "MOV        r9, r5           \n"
    "MOV        r10, r6          \n"
    "MOV        r11, r7          \n"
    "LDMIA      r0!, {r4-r7}     \n"
#endif

#ifdef STK_CORTEX_M_EXPECT_FPU
    // restore FP registers
    "TST        LR, #16         \n" /* test LR for 0xffffffe_, e.g. Thread mode with FP data */
    "IT         EQ              \n" /* if result is positive */
    "VLDMIAEQ   r0!, {s16-s31}  \n" /* restore FP registers */
#endif

    "MSR        psp, r0         \n" /* restore psp */

    "CPSIE      i               \n" /* inline STK_CORTEX_M_ENABLE_INTERRUPTS */
    "BX         LR              \n" /* inline STK_CORTEX_M_EXIT_FUNCTION */

    : /* output: none */
    : [st_idle]   "m" (GetContext().m_stack_idle),\
      [st_active] "m" (GetContext().m_stack_active),\
      [priv_val]  "i" (ACCESS_PRIVILEGED)\
    : "r0", "r1" /* only r0, r1 are used as a scratchpad */ );
}

__stk_attr_naked void OnTaskStart()
{
    // note: STK_CORTEX_M_DISABLE_INTERRUPTS() must be called prior calling this function

    __asm volatile(
    ".syntax unified             \n"

    // set privileged/unprivileged mode for the active stack
#ifdef CONTROL_nPRIV_Msk
    STK_ASM_BLOCK_PRIVILEGE_MODE
#endif

    // load stack of the active task from GetContext().m_stack_active (note: keep in sync with OnTaskStart)
    "LDR        r1, %[st_active] \n"
    "LDR        r0, [r1]         \n" /* load the first member of Stack (Stack::SP) into r0 */

    // restore general registers
#ifdef STK_CORTEX_M_MANAGE_LR
    // restore r4-r11 and LR
    "LDMIA      r0!, {r4-r11, LR}\n"
#else
    // note: LDMIA is limited to r0-r7 range, therefore load via stack memory
    "LDMIA      r0!, {r4-r7}     \n"
    "MOV        r8, r4           \n"
    "MOV        r9, r5           \n"
    "MOV        r10, r6          \n"
    "MOV        r11, r7          \n"
    "LDMIA      r0!, {r4-r7}     \n"
#endif

#ifdef STK_CORTEX_M_EXPECT_FPU
    // restore FP registers
    "TST        LR, #16         \n" /* test LR for 0xffffffe_, e.g. Thread mode with FP data */
    "IT         EQ              \n" /* if result is positive */
    "VLDMIAEQ   r0!, {s16-s31}  \n" /* restore FP registers */
#endif

    // restore psp
    "MSR        psp, r0         \n"

#ifndef STK_CORTEX_M_MANAGE_LR
    // M0: set LR to Thread mode, use PSP state and stack
    "LDR        r0, =%[exc_ret] \n"
    "MOV        LR, r0          \n"
#endif

    "CPSIE      i               \n" /* inline STK_CORTEX_M_ENABLE_INTERRUPTS */
    "BX         LR              \n" /* inline STK_CORTEX_M_EXIT_FUNCTION */

    : /* output: none */
    : [st_active] "m" (GetContext().m_stack_active),
      [priv_val]  "i" (ACCESS_PRIVILEGED),
      [exc_ret]   "i" (STK_CORTEX_M_EXC_RETURN_THREAD_PSP)
    : "r0", "r1" /* only r0, r1 are used as a scratchpad */ );
}

void Context::OnStart()
{
    // FPU
    EnableFullFpuAccess();

    // clear FPU usage status if FPU was used before kernel start
    ClearFpuState();

    // notify kernel
    m_handler->OnStart(&m_stack_active);

    // schedule ticks
    uint32_t result = SysTick_Config((uint32_t)STK_TIME_TO_CPU_TICKS_USEC(SystemCoreClock, m_tick_resolution));
    STK_ASSERT(result == 0);
    (void)result;

    // set priority (after SysTick_Config because it may change SysTick priority)
    NVIC_SetPriority(PendSV_IRQn, STK_CORTEX_M_ISR_PRIORITY_LOWEST);
    NVIC_SetPriority(SysTick_IRQn, STK_CORTEX_M_ISR_PRIORITY_LOWEST);

    // set highest priority for SVC interrupts to support critical section for unprivileged tasks
#ifdef CONTROL_nPRIV_Msk
    NVIC_SetPriority(SVCall_IRQn, STK_CORTEX_M_ISR_PRIORITY_HIGHEST);
#endif

    m_started = true;
}

void SVC_Handler_Main(Word *svc_args)
{
    // Stack frame layout: r0, r1, r2, r3, r12, r14, the return address and xPSR, First argument (r0) is svc_args[0]
    // - R0 = stack[0]
    // - R1 = stack[1]
    // - R2 = stack[2]
    // - R3 = stack[3]
    // - R12 = stack[4]
    // - LR = stack[5]
    // - PC = stack[6]
    // - xPSR= stack[7]

    // The SVC instruction is 2 bytes before the stacked PC
    uint8_t svc_number = ((uint8_t *)svc_args[6])[-2];

    switch (svc_number)
    {
    case SVC_START_SCHEDULING: {
        // disallow any duplicate attempt
        STK_ASSERT(!GetContext().m_started);
        if (GetContext().m_started)
            return;

        STK_CORTEX_M_DISABLE_INTERRUPTS();
        GetContext().OnStart();
        OnTaskStart();
        break; }

    case SVC_FORCE_SWITCH: {
        ScheduleContextSwitch();
        break; }

#ifdef CONTROL_nPRIV_Msk
    case SVC_ENTER_CRITICAL: {
        // save current BASEPRI to return to the user (into stacked R0)
        svc_args[0] = __get_BASEPRI();
        // block all interrupts except priority 0 (to be able to invoke SVC SVC_EXIT_CRITICAL)
        __set_BASEPRI(1 << __NVIC_PRIO_BITS);
        __DSB();
        __ISB();
        break; }

    case SVC_EXIT_CRITICAL: {
        // restore previous BASEPRI state (passed in R0)
        __set_BASEPRI(svc_args[0]);
        __DSB();
        __ISB();
        break; }
#endif

    default: {
        STK_ASSERT(false);
        break; }
    }
}

// source:
// ARM: How to Write an SVC Function, https://developer.arm.com/documentation/ka004005/latest
extern "C" __stk_attr_naked void STK_SVC_HANDLER()
{
    __asm volatile(
    ".syntax unified            \n"
    ".global SVC_Handler_Main   \n"
    ".align 2                   \n" // ensure the entry point is aligned

#ifdef STK_CORTEX_M_TRUSTZONE
    // ARMv8-M TrustZone (Cortex-M33)
    #if (__CORTEX_M >= 33)
    "TST    LR, #4              \n" // bit 2: 0 = Main Stack (MSP), 1 = Process Stack (PSP)
    "BNE    .use_psp            \n"
    "TST    LR, #1              \n" // check CONTROL.SFPA (secure state), bit 0: 0 = Secure, 1 = Non-Secure
    "ITE    EQ                  \n"
    "MRSEQ  r0, MSP             \n" // r0 = secure MSP
    "MRSNE  r0, MSP_NS          \n" // r0 = non-secure MSP
    "B      .call_main          \n"

    ".use_psp:                  \n"
    "TST    LR, #1              \n"
    "ITE    EQ                  \n"
    "MRSEQ  r0, PSP             \n" // r0 = secure PSP
    "MRSNE  r0, PSP_NS          \n" // r0 = non-secure MSP
    #else
    // ARMv8-M Baseline (Cortex-M23) - no IT instructions
    "MOV    r1, LR              \n"
    "LSLS   r1, r1, #29         \n" // bit 2: 0 = Main Stack (MSP), 1 = Process Stack (PSP)
    "BMI    .use_psp_v8m_b      \n"
    "MOV    r1, LR              \n"
    "LSLS   r1, r1, #31         \n" // check CONTROL.SFPA (secure state), bit 0: 0 = Secure, 1 = Non-Secure
    "BMI    .use_msp_ns         \n"
    "MRS    r0, MSP             \n" // r0 = secure MSP
    "B      .call_main          \n"

    ".use_msp_ns:               \n"
    "MRS    r0, MSP_NS          \n" // r0 = non-secure MSP
    "B      .call_main          \n"

    ".use_psp_v8m_b:            \n"
    "MOV    r1, LR              \n"
    "LSLS   r1, r1, #31         \n"
    "BMI    .use_psp_ns         \n"
    "MRS    r0, PSP             \n" // r0 = secure PSP
    "B      .call_main          \n"

    ".use_psp_ns:               \n"
    "MRS    r0, PSP_NS          \n" // r0 = non-secure MSP
    #endif

    ".call_main:                \n"
#elif (__CORTEX_M >= 3)
    // Cortex-M3/M4/M7
    "TST    LR, #4              \n" // check EXC_RETURN bit 2
    "ITE    EQ                  \n"
    "MRSEQ  r0, MSP             \n" // r0 = MSP
    "MRSNE  r0, PSP             \n" // else r0 = PSP
#else
    // Cortex-M0/M0+ (limited ISA)
    "MOV    r0, LR              \n" // r0 = LR
    "LSLS   r0, r0, #29         \n" // if (r0 & 4)
    "BMI    .use_psp_m0         \n" // else
    "MRS    r0, MSP             \n" // r0 = MSP
    "B      .call_main_m0       \n"

    ".use_psp_m0:               \n"
    "MRS    r0, PSP             \n" // else r0 = PSP

    ".call_main_m0:             \n"
#endif

    // even on Cortex-M3+, a long jump is safer when using LTO, we load address into register to allow far jump (>2KB)
    "LDR    r1, =SVC_Handler_Main \n"
    "BX     r1                  \n"

    ".align 2                   \n"   // ensure literal pool is aligned
    ".pool                      \n"); // ensure literal pool is reachable
}

static void OnTaskRun(ITask *task)
{
    task->Run();
}

static void OnTaskExit()
{
    uint32_t cs;
    STK_CORTEX_M_CRITICAL_SECTION_START(cs);

    GetContext().m_handler->OnTaskExit(GetContext().m_stack_active);

    STK_CORTEX_M_CRITICAL_SECTION_END(cs);

    for (;;)
    {
        __DSB();
        __WFI(); // enter standby mode until time slot expires
    }
}

static void OnSchedulerSleep()
{
    for (;;)
    {
    #if STK_SEGGER_SYSVIEW
        SEGGER_SYSVIEW_OnIdle();
    #endif

        SCB->SCR &= ~SCB_SCR_SLEEPDEEP_Msk; // disable deep-sleep, go into a WAIT mode (sleep)
        __DSB();                            // ensure store takes effect (see ARM info)
        __WFI();                            // enter sleep mode
    }
}

static void OnSchedulerSleepOverride()
{
    if (!GetContext().m_overrider->OnSleep())
        OnSchedulerSleep();
}

static void OnSchedulerExit()
{
    __set_CONTROL(0); // switch to MSP
    __set_PSP(0);     // clear PSP (for a clean register state)

    // jump to the exit from the IKernel::Start()
    longjmp(GetContext().m_exit_buf, 0);
}

#if STK_SEGGER_SYSVIEW
static void SendSysDesc()
{
    SEGGER_SYSVIEW_SendSysDesc("SuperTinyKernel (STK)");
}
#endif

void PlatformArmCortexM::Initialize(IEventHandler *event_handler, IKernelService *service, uint32_t resolution_us,
    Stack *exit_trap)
{
    GetContext().Initialize(event_handler, service, exit_trap, resolution_us);
}

void PlatformArmCortexM::Start()
{
    GetContext().Start();
}

bool PlatformArmCortexM::InitStack(EStackType stack_type, Stack *stack, IStackMemory *stack_memory, ITask *user_task)
{
    STK_ASSERT(stack_memory->GetStackSize() > STK_CORTEX_M_REGISTER_COUNT);

    // initialize stack memory
    Word *stack_top = Context::InitStackMemory(stack_memory);

    // initialize Stack Pointer (SP)
    stack->SP = hw::PtrToWord(stack_top - STK_CORTEX_M_REGISTER_COUNT);

    // xPSR, PC, LR, R12, R3, R2, R1, R0
    // -1    -2  -3  -4   -5  -6  -7  -8

    Word xPSR = (1 << 24); // set T bit of EPSR sub-regiser to enable execution of instructions (https://developer.arm.com/documentation/ddi0413/c/programmer-s-model/registers/special-purpose-program-status-registers--xpsr-)
    Word PC, LR, R0;

    stack_top[-1] = xPSR;

    // initialize registers for the user task's first start
    switch (stack_type)
    {
    case STACK_USER_TASK: {
        PC = hw::PtrToWord(&OnTaskRun) & ~0x1UL; // "Bit [0] is always 0, so instructions are always aligned to halfword boundaries" (https://developer.arm.com/documentation/ddi0413/c/programmer-s-model/registers/general-purpose-registers)
        LR = hw::PtrToWord(&OnTaskExit);
        R0 = hw::PtrToWord(user_task);
        break; }

    case STACK_SLEEP_TRAP: {
        PC = hw::PtrToWord(GetContext().m_overrider != nullptr ? &OnSchedulerSleepOverride : &OnSchedulerSleep) & ~0x1UL;
        LR = STK_STACK_MEMORY_FILLER; // should not attempt to exit
        R0 = 0;
        break; }

    case STACK_EXIT_TRAP: {
        PC = hw::PtrToWord(&OnSchedulerExit) & ~0x1UL;
        LR = STK_STACK_MEMORY_FILLER; // should not attempt to exit
        R0 = 0;
        break; }

    default:
        return false;
    }

    stack_top[-2] = PC;
    stack_top[-3] = LR;
    stack_top[-8] = R0;

    // Exception exit value (LR) if FP is present
#ifdef STK_CORTEX_M_MANAGE_LR
    stack_top[-9] = STK_CORTEX_M_EXC_RETURN_THREAD_PSP;
#endif

    return true;
}

static void SysTick_Stop()
{
    SysTick->CTRL = 0;
    SCB->ICSR |= SCB_ICSR_PENDSTCLR_Msk;
}

void Context::OnStop()
{
#if STK_SEGGER_SYSVIEW
    SEGGER_SYSVIEW_Stop();
#endif

    // stop SysTick timer
    SysTick_Stop();

    // clear pending PendSV exception
    SCB->ICSR |= SCB_ICSR_PENDSVCLR_Msk;

    m_started = false;
    m_exiting = true;

    // make sure all assignments are set and executed
    __DSB();
    __ISB();
}

void PlatformArmCortexM::Stop()
{
    GetContext().OnStop();

    // load context of the Exit trap
    STK_CORTEX_M_DISABLE_INTERRUPTS();
    OnTaskStart();
}

int32_t PlatformArmCortexM::GetTickResolution() const
{
    return GetContext().m_tick_resolution;
}

void PlatformArmCortexM::SwitchToNext()
{
    GetContext().m_handler->OnTaskSwitch(::GetCallerSP());
}

void PlatformArmCortexM::Sleep(Timeout ticks)
{
    GetContext().m_handler->OnTaskSleep(::GetCallerSP(), ticks);
}

IWaitObject *PlatformArmCortexM::Wait(ISyncObject *sync_obj, IMutex *mutex, Timeout timeout)
{
    return GetContext().m_handler->OnTaskWait(::GetCallerSP(), sync_obj, mutex, timeout);
}

TId PlatformArmCortexM::GetTid() const
{
    return GetContext().m_handler->OnGetTid(::GetCallerSP());
}

void PlatformArmCortexM::ProcessHardFault()
{
    if ((GetContext().m_overrider == nullptr) || !GetContext().m_overrider->OnHardFault())
    {
        STK_KERNEL_PANIC(KERNEL_PANIC_HRT_HARD_FAULT);
    }
}

void PlatformArmCortexM::SetEventOverrider(IEventOverrider *overrider)
{
    STK_ASSERT(!GetContext().m_started);
    GetContext().m_overrider = overrider;
}

Word PlatformArmCortexM::GetCallerSP() const
{
    return ::GetCallerSP();
}

IKernelService *IKernelService::GetInstance()
{
    return GetContext().m_service;
}

void stk::hw::CriticalSection::Enter()
{
    // if we are in Handler or Privileged Thread Mode, we can skip the SVC and take the fast path
    if (IsHandlerMode() || IsPrivilegedThreadMode())
        GetContext().EnterCriticalSection();
    else
        GetContext().UnprivEnterCriticalSection();
}

void stk::hw::CriticalSection::Exit()
{
    // if we are in Handler or Privileged Thread Mode, we can skip the SVC and take the fast path
    if (IsHandlerMode() || IsPrivilegedThreadMode())
        GetContext().ExitCriticalSection();
    else
        GetContext().UnprivExitCriticalSection();
}

void stk::hw::SpinLock::Lock()
{
    STK_CORTEX_M_SPIN_LOCK_LOCK(m_lock);
}

void stk::hw::SpinLock::Unlock()
{
    STK_CORTEX_M_SPIN_LOCK_UNLOCK(m_lock);
}

bool stk::hw::SpinLock::TryLock()
{
    return !STK_CORTEX_M_SPIN_LOCK_TRYLOCK(m_lock);
}

bool stk::hw::IsInsideISR()
{
    return IsHandlerMode();
}

#endif // _STK_ARCH_ARM_CORTEX_M
