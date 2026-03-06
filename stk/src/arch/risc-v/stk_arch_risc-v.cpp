/*
 * SuperTinyKernel(TM) (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

// note: If missing, this header must be customized (get it in the root of the source folder) and
//       copied to the /include folder manually.
#include "stk_config.h"

#ifdef _STK_ARCH_RISC_V

#include <stdlib.h>
#include <setjmp.h>

#include "stk_helper.h"
#include "stk_arch.h"
#include "arch/stk_arch_common.h"

using namespace stk;

#ifndef STK_RISCV_ISR_SECTION
    #define STK_RISCV_ISR_SECTION
#endif

// RISC-V does not have PendSV interrupt functionality similar to Arm Cortex-M, so reserve
// this functionality for the future extension
//#define _STK_RISCV_USE_PENDSV
#ifdef _STK_RISCV_USE_PENDSV
#error RISC-V has no PendSV interrupt functionality similar to Arm Cortex-M!
#endif

// Set tick timer (MTIMECMP) per physical CPU core (1) or not (0)
#ifndef STK_RISCV_CLINT_MTIMECMP_PER_HART
    #define STK_RISCV_CLINT_MTIMECMP_PER_HART (1)
#endif

// CLINT
// Details: https://github.com/riscv/riscv-aclint/blob/main/riscv-aclint.adoc
#ifndef STK_RISCV_CLINT_BASE_ADDR
    #define STK_RISCV_CLINT_BASE_ADDR (0x2000000)
#endif
#ifndef STK_RISCV_CLINT_MTIMECMP_ADDR
    #define STK_RISCV_CLINT_MTIMECMP_ADDR (STK_RISCV_CLINT_BASE_ADDR + 0x4000) // 8-byte value, 1 per hart
#endif
#ifndef STK_RISCV_CLINT_MTIME_ADDR
    #define STK_RISCV_CLINT_MTIME_ADDR (STK_RISCV_CLINT_BASE_ADDR + 0xBFF8) // 8-byte value, global
#endif

//! Orders all predecessor Read/Write with all successor Read/Write (similar to ARM's __DSB(DSB ISH)).
static __stk_forceinline void __DSB() { __asm volatile("fence rw, rw" : : : "memory"); }

//! Flushes the instruction cache and pipeline (similar to ARM's __ISB).
static __stk_forceinline void __ISB()
{
#ifdef __riscv_zifencei
    __asm volatile("fence.i" : : : "memory");
#else
    __sync_synchronize();
#endif
}

//! Put core into a low-power state (similar to ARM's __WFI).
static __stk_forceinline void __WFI() { __asm volatile("wfi"); }

#define STK_RISCV_CRITICAL_SECTION_START(SES) do { SES = ::HW_EnterCriticalSection(); __DSB(); __ISB(); } while (0)
#define STK_RISCV_CRITICAL_SECTION_END(SES) do { __DSB(); __ISB(); ::HW_ExitCriticalSection(SES); } while (0)

static __stk_forceinline bool STK_RISCV_SPIN_LOCK_TRYLOCK(volatile bool &LOCK)
{
    return __atomic_test_and_set(&LOCK, __ATOMIC_ACQUIRE);
}
static __stk_forceinline void STK_RISCV_SPIN_LOCK_LOCK(volatile bool &LOCK)
{
    uint32_t timeout = 0xFFFFFF;
    while (STK_RISCV_SPIN_LOCK_TRYLOCK(LOCK))
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
static __stk_forceinline void STK_RISCV_SPIN_LOCK_UNLOCK(volatile bool &LOCK)
{
    // ensure all data writes (like scheduling metadata) are flushed before the lock is released
    __asm volatile("fence rw, w" ::: "memory");
    __atomic_clear(&LOCK, __ATOMIC_RELEASE);
}

#define STK_RISCV_DISABLE_INTERRUPTS() ::HW_DisableIrq()
#define STK_RISCV_ENABLE_INTERRUPTS() ::HW_EnableIrq()

#define STK_RISCV_PRIVILEGED_MODE_ON() ((void)0)
#define STK_RISCV_PRIVILEGED_MODE_OFF() ((void)0)

#define STK_RISCV_EXIT_FROM_HANDLER() __asm volatile("mret")

#define STK_RISCV_START_SCHEDULING() __asm volatile("ecall") // cause exception with RISCV_EXCP_ENVIRONMENT_CALL_FROM_M_MODE

#define STK_RISCV_ISR extern "C" STK_RISCV_ISR_SECTION __attribute__ ((interrupt ("machine")))

//! Use private stack allocated by Context of size STK_RISCV_ISR_STACK_SIZE for handling ISRs.
#define STK_RISCV_ISR_STACK_SIZE 256

//! Timer handler.
#ifndef STK_SYSTICK_HANDLER
    #define STK_SYSTICK_HANDLER riscv_mtvec_mti // see vector_table.h/vector_table.c
#endif

//! Exception handler.
#ifndef STK_SVC_HANDLER
    #define STK_SVC_HANDLER riscv_mtvec_exception // see vector_table.h/vector_table.c
#endif

#if (__riscv_flen == 0)
    #define STK_RISCV_FP 0
#else
    #define STK_RISCV_FP __riscv_flen
#endif

#if (__riscv_32e == 1)
    #define STK_RISCV_REGISTER_COUNT (15 + (STK_RISCV_FP != 0 ? 31 : 0))
#else
    #define STK_RISCV_REGISTER_COUNT (31 + (STK_RISCV_FP != 0 ? 31 : 0))
#endif

#define STK_SERVICE_SLOTS 2 // (0) mepc, (1) mstatus

#define STR(x) #x
#define XSTR(s) STR(s)

#if (__riscv_xlen == 32)
    #define REGBYTES XSTR(4)
    #define LREG     XSTR(lw)
    #define SREG     XSTR(sw)
#elif (__riscv_xlen == 64)
    #define REGBYTES XSTR(8)
    #define LREG     XSTR(ld)
    #define SREG     XSTR(sd)
#else
    #error Unsupported RISC-V platform!
#endif

#if (__riscv_flen == 32)
    #define FREGBYTES XSTR(4)
    #define FLREG     XSTR(flw)
    #define FSREG     XSTR(fsw)
#elif (__riscv_flen == 64)
    #define FREGBYTES XSTR(8)
    #define FLREG     XSTR(fld)
    #define FSREG     XSTR(fsd)
#elif (__riscv_flen != 0)
#error Unsupported FP register count!
#endif

#if (__riscv_32e == 1)
    #define FOFFSET XSTR(68) // FP stack offset = (17 * 4)
    #if (__riscv_flen == 0)
        #define REGSIZE XSTR(((15 + 2) * 4)) // STK_RISCV_REGISTER_COUNT + 2 for mepc, mstatus
    #else
        #if (__riscv_flen == 32)
        #define REGSIZE XSTR((((15 + 2) * 4) + (31 * 4))) // STK_RISCV_REGISTER_COUNT + 2 for mepc, mstatus + 32 fp registers
        #elif (__riscv_flen == 64)
        #define REGSIZE XSTR((((15 + 2) * 4) + (31 * 8))) // STK_RISCV_REGISTER_COUNT + 2 for mepc, mstatus + 32 fp registers
        #endif
    #endif
#elif (__riscv_xlen == 32)
    #define FOFFSET XSTR(132) // FP stack offset = (33 * 4)
    #if (__riscv_flen == 0)
        #define REGSIZE XSTR(((31 + 2) * 4)) // STK_RISCV_REGISTER_COUNT + 2 for mepc, mstatus
    #else
        #if (__riscv_flen == 32)
        #define REGSIZE XSTR((((31 + 2) * 4) + (31 * 4))) // STK_RISCV_REGISTER_COUNT + 2 for mepc, mstatus + 32 fp registers
        #elif (__riscv_flen == 64)
        #define REGSIZE XSTR((((31 + 2) * 4) + (31 * 8))) // STK_RISCV_REGISTER_COUNT + 2 for mepc, mstatus + 32 fp registers
        #endif
    #endif
#elif (__riscv_xlen == 64)
    #define FOFFSET XSTR(264) // FP stack offset = (33 * 8)
    #if (__riscv_flen == 0)
        #define REGSIZE XSTR(((31 + 2) * 8)) // STK_RISCV_REGISTER_COUNT + 2 for mepc, mstatus
    #else
        #if (__riscv_flen == 32)
        #define REGSIZE XSTR((((31 + 2) * 8) + (31 * 4))) // STK_RISCV_REGISTER_COUNT + 2 for mepc, mstatus + 32 fp registers
        #elif (__riscv_flen == 64)
        #define REGSIZE XSTR((((31 + 2) * 8) + (31 * 8))) // STK_RISCV_REGISTER_COUNT + 2 for mepc, mstatus + 32 fp registers
        #endif
    #endif
#endif

#define STK_RISCV_REG_INDEX(REG) (-((STK_RISCV_REGISTER_COUNT + 1) - REG))
#define STK_RISCV_SRV_INDEX(REG) (STK_RISCV_REG_INDEX(REG) - STK_SERVICE_SLOTS)

static __stk_forceinline void HW_DisableIrq()
{
    __asm volatile("csrrci zero, mstatus, %0"
    : /* output: none */
    : "i"(MSTATUS_MIE)
    : /* clobbers: none */);
}

static __stk_forceinline void HW_EnableIrq()
{
    __asm volatile("csrrsi zero, mstatus, %0"
    : /* output: none */
    : "i"(MSTATUS_MIE)
    : /* clobbers: none */);
}

/*! \brief  Enter critical section.
    \return Session value which has to be supplied to HW_ExitCriticalSection().
*/
static __stk_forceinline Word HW_EnterCriticalSection()
{
    Word ses;

    __asm volatile("csrrci %0, mstatus, %1"
    : "=r"(ses)
    : "i"(MSTATUS_MIE)
    :  /* clobbers: none */);

    return ses;
}

/*! \brief     Exit critical section.
    \param[in] ses: Session value obtained by HW_EnterCriticalSection().
*/
static __stk_forceinline void HW_ExitCriticalSection(Word ses)
{
    __asm volatile("csrrs zero, mstatus, %0"
    : /* output: none */
    : "r"(ses)
    : /* clobbers: none */);
}

/*! \brief  Get mtime.
    \return Ticks.
*/
static __stk_forceinline uint64_t HW_GetMtime()
{
#if ( __riscv_xlen > 32)
    return *((volatile uint64_t *)STK_RISCV_CLINT_MTIME_ADDR);
#else
    volatile uint32_t *mtime_hi = ((uint32_t *)STK_RISCV_CLINT_MTIME_ADDR) + 1;
    volatile uint32_t *mtime_lo = ((uint32_t *)STK_RISCV_CLINT_MTIME_ADDR);

    uint32_t hi, lo;
    do
    {
        hi = (*mtime_hi);
        lo = (*mtime_lo);
    }
    while (hi != (*mtime_hi)); // make sure mtime_hi did not tick when read mtime_lo

    return ((uint64_t)hi << 32) | lo;
#endif
}

/*! \brief     Set mtimecmp register.
    \param[in] advance: Time delay (ticks) till the next interrupt.
*/
static __stk_forceinline void HW_SetMtimecmp(uint64_t advance)
{
    uint64_t next = HW_GetMtime() + advance;

    uint32_t hart;
#if STK_RISCV_CLINT_MTIMECMP_PER_HART
    hart = read_csr(mhartid);
#else
    hart = 0;
#endif
#if (__riscv_xlen == 64)
    ((volatile uint64_t *)STK_RISCV_CLINT_MTIMECMP_ADDR)[hart] = next;
#else
    volatile uint32_t *mtime_lo = (uint32_t *)((uint64_t *)STK_RISCV_CLINT_MTIMECMP_ADDR + hart);
    volatile uint32_t *mtime_hi = mtime_lo + 1;

    // expecting 4-byte aligned memory
    STK_ASSERT(((uintptr_t)mtime_lo & (4 - 1)) == 0);
    STK_ASSERT(((uintptr_t)mtime_hi & (4 - 1)) == 0);

    // prevent unexpected interrupt by setting some very large value to the high part
    // details: https://riscv.org/wp-content/uploads/2017/05/riscv-privileged-v1.10.pdf, page 31
    (*mtime_hi) = ~0;

    (*mtime_lo) = (uint32_t)(next & 0xFFFFFFFF);
    (*mtime_hi) = (uint32_t)(next >> 32);
#endif
}

/*! \brief Get SP of the calling process.
*/
static __stk_forceinline Word HW_GetCallerSP()
{
    Word sp;

    // load SP into sp variable
    __asm volatile(
    SREG " sp, %0"
    : "=m"(sp)
    : /* input: none */
    : /* clobbers: none */);

    return sp;
}

/*! \brief Switch context by scheduling PendSV interrupt.
*/
static __stk_forceinline void ScheduleContextSwitch()
{
#ifdef _STK_RISCV_USE_PENDSV
    // TO-DO
#endif
}

//! Define _STK_SYSTEM_CLOCK_VAR privately by the driver if _STK_SYSTEM_CLOCK_EXTERNAL
//! is 0 or undefined.
#if !_STK_SYSTEM_CLOCK_EXTERNAL
volatile uint32_t _STK_SYSTEM_CLOCK_VAR = _STK_SYSTEM_CLOCK_FREQUENCY;
#endif

//! Global lock to synchronize critical sections of multiple cores.
static volatile bool g_CsuLock = false;

//! Internal context.
static struct Context : public PlatformContext
{
    Context() : PlatformContext(), m_stack_main(), m_stack_isr(), m_exit_buf(), m_stack_isr_mem(),
        m_overrider(nullptr), m_specific(nullptr), m_tick_period(0), m_csu(0), m_csu_nesting(0),
        m_starting(false), m_started(false), m_exiting(false)
    {}

    void Initialize(IPlatform::IEventHandler *handler, IKernelService *service, Stack *exit_trap, int32_t resolution_us)
    {
        PlatformContext::Initialize(handler, service, exit_trap, resolution_us);

        // init ISR's stack
        {
            StackMemoryWrapper<STK_RISCV_ISR_STACK_SIZE> stack_isr_mem(&m_stack_isr_mem);
            m_stack_isr.SP   = hw::PtrToWord(InitStackMemory(&stack_isr_mem));
            m_stack_isr.mode = ACCESS_PRIVILEGED;
        }

        // init Main stack
        {
            m_stack_main.SP   = STK_STACK_MEMORY_FILLER;
            m_stack_main.mode = ACCESS_PRIVILEGED;
        }

        m_csu         = 0;
        m_csu_nesting = 0;
        m_overrider   = NULL;
        m_specific    = NULL;
        m_tick_period = STK_TIME_TO_CPU_TICKS_USEC(_STK_SYSTEM_CLOCK_VAR, resolution_us);
        m_starting    = false;
        m_started     = false;
        m_exiting     = false;
    }

    __stk_forceinline void OnTick()
    {
        if (m_handler->OnTick(&m_stack_idle, &m_stack_active))
        {
            ScheduleContextSwitch();
        }
    }

    __stk_forceinline void EnterCriticalSection()
    {
        // disable local interrupts and save state
        Word current_ses;
        STK_RISCV_CRITICAL_SECTION_START(current_ses);

        if (m_csu_nesting == 0)
        {
            // ONLY attempt the global spinlock if we aren't already nested
            STK_RISCV_SPIN_LOCK_LOCK(g_CsuLock);

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
            Word ses_to_restore = m_csu;

            // release global lock
            STK_RISCV_SPIN_LOCK_UNLOCK(g_CsuLock);

            // restore hardware interrupts
            STK_RISCV_CRITICAL_SECTION_END(ses_to_restore);
        }
    }

    typedef IPlatform::IEventOverrider                               eovrd_t;
    typedef PlatformRiscV::ISpecificEventHandler                     sehndl_t;
    typedef StackMemoryWrapper<STK_RISCV_ISR_STACK_SIZE>::MemoryType isrmem_t;

    Stack     m_stack_main;    //!< main stack info
    Stack     m_stack_isr;     //!< isr stack info
    jmp_buf   m_exit_buf;      //!< saved context of the exit point
    isrmem_t  m_stack_isr_mem; //!< ISR stack memory
    eovrd_t  *m_overrider;     //!< platform events overrider
    sehndl_t *m_specific;      //!< platform-specific event handler
    int32_t   m_tick_period;   //!< system tick periodicity (microseconds, ticks)
    Word    m_csu;           //!< user critical session
    uint32_t  m_csu_nesting;   //!< depth of user critical session nesting
    bool      m_starting;      //!< 'true' when in is being started
    bool      m_started;       //!< 'true' when in started state
    bool      m_exiting;       //!< 'true' when is exiting the scheduling process
}
g_Context[STK_ARCH_CPU_COUNT];

void PlatformRiscV::ProcessTick()
{
#ifdef _STK_RISCV_USE_PENDSV
    Word cs;
    STK_RISCV_CRITICAL_SECTION_START(cs);

    GetContext().OnTick();

    STK_RISCV_CRITICAL_SECTION_END(cs);
#else
    // unsupported scenario
    STK_ASSERT(false);
#endif
}

__stk_attr_noinline  // keep out of inlining to preserve stack frame
__stk_attr_noreturn  // never returns - a trap
void STK_PANIC_HANDLER_DEFAULT(EKernelPanicId id)
{
    (void)id;

    // disable all maskable interrupts: this prevents scheduler from running again and corrupting state further
    STK_RISCV_DISABLE_INTERRUPTS();

    // spin forever: with a watchdog active this produces a clean reset, without a watchdog,
    // a debugger can attach and inspect 'id'
    for (;;)
    {
        __stk_relax_cpu();
    }
}

static __stk_forceinline void SaveContext()
{
    __asm volatile(
    // allocate stack memory for registers
    "addi sp, sp, -" REGSIZE       " \n"

    // save regs
    SREG " x1, 2*" REGBYTES "(sp)    \n"
    //SREG " x2, 3*" REGBYTES "(sp)  \n"  // skip saving sp, Stack pointer
    //SREG " x3, 4*" REGBYTES "(sp)  \n"  // skip saving gp, Global pointer (note: slot is used by fsr)
    SREG " x4, 5*" REGBYTES "(sp)    \n"
    SREG " x5, 6*" REGBYTES "(sp)    \n"
    SREG " x6, 7*" REGBYTES "(sp)    \n"
    SREG " x7, 8*" REGBYTES "(sp)    \n"
    SREG " x8, 9*" REGBYTES "(sp)    \n"
    SREG " x9, 10*" REGBYTES "(sp)   \n"
    SREG " x10, 11*" REGBYTES "(sp)  \n"
    SREG " x11, 12*" REGBYTES "(sp)  \n"
    SREG " x12, 13*" REGBYTES "(sp)  \n"
    SREG " x13, 14*" REGBYTES "(sp)  \n"
    SREG " x14, 15*" REGBYTES "(sp)  \n"
    SREG " x15, 16*" REGBYTES "(sp)  \n"
#if (__riscv_32e != 1)
    SREG " x16, 17*" REGBYTES "(sp)  \n"
    SREG " x17, 18*" REGBYTES "(sp)  \n"
    SREG " x18, 19*" REGBYTES "(sp)  \n"
    SREG " x19, 20*" REGBYTES "(sp)  \n"
    SREG " x20, 21*" REGBYTES "(sp)  \n"
    SREG " x21, 22*" REGBYTES "(sp)  \n"
    SREG " x22, 23*" REGBYTES "(sp)  \n"
    SREG " x23, 24*" REGBYTES "(sp)  \n"
    SREG " x24, 25*" REGBYTES "(sp)  \n"
    SREG " x25, 26*" REGBYTES "(sp)  \n"
    SREG " x26, 27*" REGBYTES "(sp)  \n"
    SREG " x27, 28*" REGBYTES "(sp)  \n"
    SREG " x28, 29*" REGBYTES "(sp)  \n"
    SREG " x29, 30*" REGBYTES "(sp)  \n"
    SREG " x30, 31*" REGBYTES "(sp)  \n"
    SREG " x31, 32*" REGBYTES "(sp)  \n"
#endif

#if (__riscv_flen != 0)
    // save FP
    FSREG " f0, " FOFFSET "+0*" FREGBYTES "(sp)  \n"
    FSREG " f1, " FOFFSET "+1*" FREGBYTES "(sp)  \n"
    FSREG " f2, " FOFFSET "+2*" FREGBYTES "(sp)  \n"
    FSREG " f3, " FOFFSET "+3*" FREGBYTES "(sp)  \n"
    FSREG " f4, " FOFFSET "+4*" FREGBYTES "(sp)  \n"
    FSREG " f5, " FOFFSET "+5*" FREGBYTES "(sp)  \n"
    FSREG " f6, " FOFFSET "+6*" FREGBYTES "(sp)  \n"
    FSREG " f7, " FOFFSET "+7*" FREGBYTES "(sp)  \n"
    FSREG " f8, " FOFFSET "+8*" FREGBYTES "(sp)  \n"
    FSREG " f9, " FOFFSET "+9*" FREGBYTES "(sp)  \n"
    FSREG " f10, " FOFFSET "+10*" FREGBYTES "(sp)  \n"
    FSREG " f11, " FOFFSET "+11*" FREGBYTES "(sp)  \n"
    FSREG " f12, " FOFFSET "+12*" FREGBYTES "(sp)  \n"
    FSREG " f13, " FOFFSET "+13*" FREGBYTES "(sp)  \n"
    FSREG " f14, " FOFFSET "+14*" FREGBYTES "(sp)  \n"
    FSREG " f15, " FOFFSET "+15*" FREGBYTES "(sp)  \n"
    FSREG " f16, " FOFFSET "+16*" FREGBYTES "(sp)  \n"
    FSREG " f17, " FOFFSET "+17*" FREGBYTES "(sp)  \n"
    FSREG " f18, " FOFFSET "+18*" FREGBYTES "(sp)  \n"
    FSREG " f19, " FOFFSET "+19*" FREGBYTES "(sp)  \n"
    FSREG " f20, " FOFFSET "+20*" FREGBYTES "(sp)  \n"
    FSREG " f21, " FOFFSET "+21*" FREGBYTES "(sp)  \n"
    FSREG " f22, " FOFFSET "+22*" FREGBYTES "(sp)  \n"
    FSREG " f23, " FOFFSET "+23*" FREGBYTES "(sp)  \n"
    FSREG " f24, " FOFFSET "+24*" FREGBYTES "(sp)  \n"
    FSREG " f25, " FOFFSET "+25*" FREGBYTES "(sp)  \n"
    FSREG " f26, " FOFFSET "+26*" FREGBYTES "(sp)  \n"
    FSREG " f27, " FOFFSET "+27*" FREGBYTES "(sp)  \n"
    FSREG " f28, " FOFFSET "+28*" FREGBYTES "(sp)  \n"
    FSREG " f29, " FOFFSET "+29*" FREGBYTES "(sp)  \n"
    FSREG " f30, " FOFFSET "+30*" FREGBYTES "(sp)  \n"
    FSREG " f31, " FOFFSET "+31*" FREGBYTES "(sp)  \n"
#endif

    // save PC, Status of the current process
    "csrr t0, mepc                   \n"
    "csrr t1, mstatus                \n"
    SREG " t0, 0*" REGBYTES "(sp)    \n"
    SREG " t1, 1*" REGBYTES "(sp)    \n"

#if (__riscv_flen != 0)
    // save FCSR
    "frcsr t0                        \n"
    SREG " t0, 4*" REGBYTES "(sp)    \n"  // use stack memory slot of gp  (see comment for x3 above)
#endif
    );

    // save SP of the idle stack
    __asm volatile(
    LREG " t0, %0                    \n"
    SREG " sp, 0(t0)                 \n"
    : /* output: none */
#ifdef _STK_RISCV_USE_PENDSV
    : "m"(GetContext().m_stack_idle)
#else
    : "m"(GetContext().m_stack_active)
#endif
    : /* clobbers: none */);
}

static __stk_forceinline void LoadContext()
{
    // load SP of the active stack
    __asm volatile(
    LREG " t0, %0                    \n" // load the first member (SP) into t0
    LREG " sp, 0(t0)                 \n" // sp = t0
    : /* output: none */
    : "m"(GetContext().m_stack_active)
    : /* clobbers: none */);

    __asm volatile(
    // load PC, Status
    LREG " t0, 0*" REGBYTES "(sp)    \n"
    LREG " t1, 1*" REGBYTES "(sp)    \n"
    "csrw mepc, t0                   \n"
    "csrw mstatus, t1                \n"

#if (__riscv_flen != 0)
    // load FCSR
    LREG " t0, 4*" REGBYTES "(sp)    \n" // use stack memory slot of gp (see comment for x3 below)
    "fscsr t0                        \n"
#endif

    // load regs
    LREG " x1, 2*" REGBYTES "(sp)    \n"
    //LREG " x2, 3*" REGBYTES "(sp)  \n" // skip loading sp, Stack pointer
    //LREG " x3, 4*" REGBYTES "(sp)  \n" // skip loading gp, Global pointer (note: slot is used by fsr)
    LREG " x4, 5*" REGBYTES "(sp)    \n"
    LREG " x5, 6*" REGBYTES "(sp)    \n"
    LREG " x6, 7*" REGBYTES "(sp)    \n"
    LREG " x7, 8*" REGBYTES "(sp)    \n"
    LREG " x8, 9*" REGBYTES "(sp)    \n"
    LREG " x9, 10*" REGBYTES "(sp)   \n"
    LREG " x10, 11*" REGBYTES "(sp)  \n"
    LREG " x11, 12*" REGBYTES "(sp)  \n"
    LREG " x12, 13*" REGBYTES "(sp)  \n"
    LREG " x13, 14*" REGBYTES "(sp)  \n"
    LREG " x14, 15*" REGBYTES "(sp)  \n"
    LREG " x15, 16*" REGBYTES "(sp)  \n"
#if (__riscv_32e != 1)
    LREG " x16, 17*" REGBYTES "(sp)  \n"
    LREG " x17, 18*" REGBYTES "(sp)  \n"
    LREG " x18, 19*" REGBYTES "(sp)  \n"
    LREG " x19, 20*" REGBYTES "(sp)  \n"
    LREG " x20, 21*" REGBYTES "(sp)  \n"
    LREG " x21, 22*" REGBYTES "(sp)  \n"
    LREG " x22, 23*" REGBYTES "(sp)  \n"
    LREG " x23, 24*" REGBYTES "(sp)  \n"
    LREG " x24, 25*" REGBYTES "(sp)  \n"
    LREG " x25, 26*" REGBYTES "(sp)  \n"
    LREG " x26, 27*" REGBYTES "(sp)  \n"
    LREG " x27, 28*" REGBYTES "(sp)  \n"
    LREG " x28, 29*" REGBYTES "(sp)  \n"
    LREG " x29, 30*" REGBYTES "(sp)  \n"
    LREG " x30, 31*" REGBYTES "(sp)  \n"
    LREG " x31, 32*" REGBYTES "(sp)  \n"
#endif

#if (__riscv_flen != 0)
    // load FP
    FLREG " f0, " FOFFSET "+0*" FREGBYTES "(sp)  \n"
    FLREG " f1, " FOFFSET "+1*" FREGBYTES "(sp)  \n"
    FLREG " f2, " FOFFSET "+2*" FREGBYTES "(sp)  \n"
    FLREG " f3, " FOFFSET "+3*" FREGBYTES "(sp)  \n"
    FLREG " f4, " FOFFSET "+4*" FREGBYTES "(sp)  \n"
    FLREG " f5, " FOFFSET "+5*" FREGBYTES "(sp)  \n"
    FLREG " f6, " FOFFSET "+6*" FREGBYTES "(sp)  \n"
    FLREG " f7, " FOFFSET "+7*" FREGBYTES "(sp)  \n"
    FLREG " f8, " FOFFSET "+8*" FREGBYTES "(sp)  \n"
    FLREG " f9, " FOFFSET "+9*" FREGBYTES "(sp)  \n"
    FLREG " f10, " FOFFSET "+10*" FREGBYTES "(sp)  \n"
    FLREG " f11, " FOFFSET "+11*" FREGBYTES "(sp)  \n"
    FLREG " f12, " FOFFSET "+12*" FREGBYTES "(sp)  \n"
    FLREG " f13, " FOFFSET "+13*" FREGBYTES "(sp)  \n"
    FLREG " f14, " FOFFSET "+14*" FREGBYTES "(sp)  \n"
    FLREG " f15, " FOFFSET "+15*" FREGBYTES "(sp)  \n"
    FLREG " f16, " FOFFSET "+16*" FREGBYTES "(sp)  \n"
    FLREG " f17, " FOFFSET "+17*" FREGBYTES "(sp)  \n"
    FLREG " f18, " FOFFSET "+18*" FREGBYTES "(sp)  \n"
    FLREG " f19, " FOFFSET "+19*" FREGBYTES "(sp)  \n"
    FLREG " f20, " FOFFSET "+20*" FREGBYTES "(sp)  \n"
    FLREG " f21, " FOFFSET "+21*" FREGBYTES "(sp)  \n"
    FLREG " f22, " FOFFSET "+22*" FREGBYTES "(sp)  \n"
    FLREG " f23, " FOFFSET "+23*" FREGBYTES "(sp)  \n"
    FLREG " f24, " FOFFSET "+24*" FREGBYTES "(sp)  \n"
    FLREG " f25, " FOFFSET "+25*" FREGBYTES "(sp)  \n"
    FLREG " f26, " FOFFSET "+26*" FREGBYTES "(sp)  \n"
    FLREG " f27, " FOFFSET "+27*" FREGBYTES "(sp)  \n"
    FLREG " f28, " FOFFSET "+28*" FREGBYTES "(sp)  \n"
    FLREG " f29, " FOFFSET "+29*" FREGBYTES "(sp)  \n"
    FLREG " f30, " FOFFSET "+30*" FREGBYTES "(sp)  \n"
    FLREG " f31, " FOFFSET "+31*" FREGBYTES "(sp)  \n"
#endif

    // shrink stack memory of registers
    "addi sp, sp, " REGSIZE " \n");
}

static __stk_forceinline void EnableFullFpuAccess()
{
#if (__riscv_flen != 0)
    __asm volatile(
    "li t0, %0        \n"
    "csrs mstatus, t0 \n"
    ::
    "i"(MSTATUS_FS | MSTATUS_XS));
#endif
}

static __stk_forceinline void ClearFpuState()
{
#if (__riscv_flen != 0)
    __asm volatile(
    "fssr x0"
    : /* output: none */
    : /* input: none */
    : /* clobbers: none */);
#endif
}

static __stk_forceinline void SaveMainSP()
{
    __asm volatile(
    SREG " sp, %0"
    : "=m"(GetContext().m_stack_main)
    : /* input: none */
    : /* clobbers: none */);
}

static __stk_forceinline void LoadMainSP()
{
    __asm volatile(
    LREG " sp, %0"
    : /* output: none */
    : "m"(GetContext().m_stack_main)
    : /* clobbers: none */);
}

static __stk_forceinline void LoadIsrSP()
{
    __asm volatile(
    LREG " sp, %0"
    : /* output: none */
    : "m"(GetContext().m_stack_isr)
    : /* clobbers: none */);
}

static __stk_forceinline bool IsHandlerMode()
{
    Word current_sp = HW_GetCallerSP();

    // get the bounds of the ISR stack from our Context
    // note: STK uses StackMemoryWrapper, so we check against that memory block
    const Word isr_stack_base = (Word)&GetContext().m_stack_isr_mem;
    const Word isr_stack_top  = isr_stack_base + STK_RISCV_ISR_STACK_SIZE;

    return ((current_sp >= isr_stack_base) && (current_sp < isr_stack_top));
}

__stk_forceinline void OnTaskStart()
{
    LoadContext();

    STK_RISCV_EXIT_FROM_HANDLER();
}

extern "C" STK_RISCV_ISR_SECTION __stk_attr_used void TrySwitchContext() // __stk_attr_used for LTO
{
    // make sure SysTick is enabled by the Kernel::Start(), disable its start anywhere else
    STK_ASSERT(GetContext().m_started);
    STK_ASSERT(GetContext().m_handler != NULL);

    // reschedule timer (note: before OnTick because timer can be stopped in Stop)
    HW_SetMtimecmp(GetContext().m_tick_period);

    // process tick
    GetContext().OnTick();
}

#ifdef _STK_RISCV_USE_PENDSV
STK_RISCV_ISR void STK_SYSTICK_HANDLER()
{
    // save SP before switching to the main
    Word sp = HW_GetCallerSP();

    // load SP of the main stack to handle ISR
    LoadIsrSP();

    // try switch context (do via asm function call to avoid inlining)
    __asm volatile(
    "jal ra, TrySwitchContext"
    : /* output: none */
    : /* input: none */
    : /* clobbers: none */);

    // restore SP
    __asm volatile(
    LREG " sp, %0"
    : /* output: none */
    : "m"(sp)
    : /* clobbers: none */);
}
#else
extern "C" STK_RISCV_ISR_SECTION __stk_attr_naked void STK_SYSTICK_HANDLER()
{
    // save current context (unconditionally)
    SaveContext();

    // internal ISR processing
    {
        // load SP of ISR's stack for handling TrySwitchContext
        LoadIsrSP();

        // try switch context (do via asm function call to avoid inlining)
        __asm volatile(
        "jal ra, TrySwitchContext"
        : /* output: none */
        : /* input: none */
        : /* clobbers: none */);
    }

    // re-load context to restore trashed process's register values by the internal ISRs processing
    LoadContext();

    STK_RISCV_EXIT_FROM_HANDLER();
}
#endif

static __stk_forceinline void StartScheduling()
{
    // save SP of main stack to reuse it for scheduler exit
    SaveMainSP();

    // enable FPU (if available)
    EnableFullFpuAccess();

    // clear FPU usage status if FPU was used before kernel start
    ClearFpuState();

    // notify kernel
    GetContext().m_handler->OnStart(&GetContext().m_stack_active);

    // configure timer
    HW_SetMtimecmp(GetContext().m_tick_period);

    // change state before enabling interrupt
    GetContext().m_started  = true;
    GetContext().m_starting = false;

    // enable timer interrupt
    set_csr(mie, MIP_MTIP);
}

STK_RISCV_ISR void STK_SVC_HANDLER()
{
    Word cause;
    __asm volatile("csrr %0, mcause"
    : "=r"(cause)
    : /* input : none */
    : /* clobbers: none */);

    /*if (cause & (1UL << (__riscv_xlen - 1)))
    {
        cause &= ~(1UL << (__riscv_xlen - 1));

        if (cause == IRQ_M_TIMER)
        {

        }
    }*/

    if (cause == IRQ_M_EXT)
    {
        // not starting scheduler, then try to forward ecall to user
        if (!GetContext().m_starting)
        {
            // forward event to user
            if (GetContext().m_specific != NULL)
                GetContext().m_specific->OnException(cause);

            // switch to the next instruction of the caller space (PC) after the return
            write_csr(mepc, read_csr(mepc) + sizeof(Word));
        }
        else
        {
            // schedule first task
            StartScheduling();
            OnTaskStart();
        }
    }
    else
    {
        if (GetContext().m_specific != NULL)
        {
            // forward event to user
            GetContext().m_specific->OnException(cause);
        }
        else
        {
            // trap further execution
            // note: normally, if trapped here with cause 2 or 4 then check stack memory size of the
            // tasks, scheduler and ISR, they were likely overwritten if your code is 100% correct
            for (;;)
            {
                //assert(false);
                __stk_debug_break();
                __WFI();
            }
        }
    }
}

static void OnTaskRun(ITask *task)
{
    task->Run();
}

static void OnTaskExit()
{
    Word cs;
    STK_RISCV_CRITICAL_SECTION_START(cs);

    GetContext().m_handler->OnTaskExit(GetContext().m_stack_active);

    STK_RISCV_CRITICAL_SECTION_END(cs);

    for (;;)
    {
        __DSB(); // data barrier
        __WFI(); // enter standby mode until time slot expires
        __ISB(); // instruction sync
    }
}

static STK_RISCV_ISR_SECTION void OnSchedulerSleep()
{
#if STK_SEGGER_SYSVIEW
    SEGGER_SYSVIEW_OnIdle();
#endif

    for (;;)
    {
        __DSB(); // data barrier
        __WFI(); // enter sleep until interrupt
        __ISB(); // instruction sync
    }
}

static STK_RISCV_ISR_SECTION void OnSchedulerSleepOverride()
{
    if (!GetContext().m_overrider->OnSleep())
        OnSchedulerSleep();
}

static void OnSchedulerExit()
{
    // switch to main stack
    LoadMainSP();

    // jump to the exit from the IKernel::Start()
    longjmp(GetContext().m_exit_buf, 0);
}

void PlatformRiscV::Initialize(IEventHandler *event_handler, IKernelService *service, uint32_t resolution_us, Stack *exit_trap)
{
    GetContext().Initialize(event_handler, service, exit_trap, resolution_us);
}

void PlatformRiscV::Start()
{
    GetContext().m_exiting = false;

    // save jump location of the Exit trap
    setjmp(GetContext().m_exit_buf);
    if (GetContext().m_exiting)
        return;

    // enable FPU (if available)
    EnableFullFpuAccess();

    // start
    GetContext().m_starting = true;
    STK_RISCV_START_SCHEDULING();
}

bool PlatformRiscV::InitStack(EStackType stack_type, Stack *stack, IStackMemory *stack_memory, ITask *user_task)
{
    STK_ASSERT(stack_memory->GetStackSize() > (STK_RISCV_REGISTER_COUNT + STK_SERVICE_SLOTS));

    // initialize stack memory
    Word *stack_top = PlatformContext::InitStackMemory(stack_memory);

    // initialize Stack Pointer (SP)
    stack->SP = hw::PtrToWord(stack_top - (STK_RISCV_REGISTER_COUNT + STK_SERVICE_SLOTS));

    Word MEPC, RA, X10;
    Word MSTATUS = MSTATUS_MPP | MSTATUS_MPIE | (STK_RISCV_FP != 0 ? (MSTATUS_FS | MSTATUS_XS) : 0);
#if (STK_RISCV_FP != 0)
    Word FSR = 0;
#endif

    // initialize registers for the user task's first start
    switch (stack_type)
    {
    case STACK_USER_TASK: {
        MEPC = hw::PtrToWord(&OnTaskRun);
        RA   = hw::PtrToWord(&OnTaskExit);
        X10  = hw::PtrToWord(user_task);
        break; }

    case STACK_SLEEP_TRAP: {
        MEPC = hw::PtrToWord(GetContext().m_overrider != NULL ? &OnSchedulerSleepOverride : &OnSchedulerSleep);
        RA   = STK_STACK_MEMORY_FILLER; // should not attempt to exit
        X10  = 0;
        break; }

    case STACK_EXIT_TRAP: {
        MEPC = hw::PtrToWord(&OnSchedulerExit);
        RA   = STK_STACK_MEMORY_FILLER; // should not attempt to exit
        X10  = 0;
        break; }

    default:
        return false;
    }

    stack_top[STK_RISCV_SRV_INDEX(1)]  = MEPC;    // mepc (entry function)
    stack_top[STK_RISCV_SRV_INDEX(2)]  = MSTATUS; // mstatus (entry function)

    stack_top[STK_RISCV_REG_INDEX(1)]  = RA;      // x1, ra
#if (STK_RISCV_FP != 0)
    stack_top[STK_RISCV_REG_INDEX(3)]  = FSR;     // x3, fssr (note: x4 is gp register but we use this slot to hold value for fsr register)
#endif
    stack_top[STK_RISCV_REG_INDEX(10)] = X10;     // x10, function argument

    return true;
}

static void SysTick_Stop()
{
    clear_csr(mie, MIP_MTIP);
}

void PlatformRiscV::Stop()
{
    // stop timer
    SysTick_Stop();

    GetContext().m_started = false;
    GetContext().m_exiting = true;

    // make sure all assignments are set and executed
    __sync_synchronize();
}

int32_t PlatformRiscV::GetTickResolution() const
{
    return GetContext().m_tick_resolution;
}

void PlatformRiscV::SwitchToNext()
{
    GetContext().m_handler->OnTaskSwitch(::HW_GetCallerSP());
}

void PlatformRiscV::Sleep(Timeout ticks)
{
    GetContext().m_handler->OnTaskSleep(::HW_GetCallerSP(), ticks);
}

IWaitObject *PlatformRiscV::Wait(ISyncObject *sync_obj, IMutex *mutex, Timeout timeout)
{
    return GetContext().m_handler->OnTaskWait(::HW_GetCallerSP(), sync_obj, mutex, timeout);
}

TId PlatformRiscV::GetTid() const
{
    return GetContext().m_handler->OnGetTid(::HW_GetCallerSP());
}

void PlatformRiscV::ProcessHardFault()
{
    if ((GetContext().m_overrider == NULL) || !GetContext().m_overrider->OnHardFault())
    {
        STK_KERNEL_PANIC(KERNEL_PANIC_HRT_HARD_FAULT);
    }
}

void PlatformRiscV::SetEventOverrider(IEventOverrider *overrider)
{
    STK_ASSERT(!GetContext().m_started);
    GetContext().m_overrider = overrider;
}

Word PlatformRiscV::GetCallerSP() const
{
    return ::HW_GetCallerSP();
}

void PlatformRiscV::SetSpecificEventHandler(ISpecificEventHandler *handler)
{
    STK_ASSERT(!GetContext().m_started);
    GetContext().m_specific = handler;
}

IKernelService *IKernelService::GetInstance()
{
    return GetContext().m_service;
}

void stk::hw::CriticalSection::Enter()
{
    GetContext().EnterCriticalSection();
}

void stk::hw::CriticalSection::Exit()
{
    GetContext().ExitCriticalSection();
}

void stk::hw::SpinLock::Lock()
{
    STK_RISCV_SPIN_LOCK_LOCK(m_lock);
}

void stk::hw::SpinLock::Unlock()
{
    STK_RISCV_SPIN_LOCK_UNLOCK(m_lock);
}

bool stk::hw::SpinLock::TryLock()
{
    return !STK_RISCV_SPIN_LOCK_TRYLOCK(m_lock);
}

bool stk::hw::IsInsideISR()
{
    return IsHandlerMode();
}

#endif // _STK_ARCH_RISC_V
