/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_ARCH_RISC_V_H_
#define STK_ARCH_RISC_V_H_

#include "stk_common.h"

/*! \file  stk_arch_risc-v.h
    \brief Platform port for RISC-V.
*/

namespace stk {

/*! \class PlatformRiscV
    \brief Concrete implementation of IPlatform driver for RISC-V processors.
*/
class PlatformRiscV : public IPlatform
{
public:
    /*! \brief Destructor.
        \note  MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    STK_VIRT_DTOR ~PlatformRiscV() = default;

    /*! \class ISpecificEventHandler
        \brief RISC-V specific event handler, see SetSpecificEventHandler().
    */
    class ISpecificEventHandler
    {
    public:
        /*! \brief     Called by the trap handler for a RISC-V exception/interrupt cause (IRQ_XXX, see encoding.h).
            \param[in] cause: Value of the \c mcause register.
            \return    \c true if handled, \c false to let the driver handle it.
        */
        virtual bool OnException(Word cause) = 0;
    };

    void Initialize(IEventHandler *event_handler, IKernelService *service, uint32_t resolution_us, Stack *exit_trap) override;
    void Start() override;
    void Stop() override;
    void InitStack(EStackType stack_type, Stack *stack, IStackMemory *stack_memory, ITask *user_task) override;
    uint32_t GetTickResolution() const override;
    Cycles GetSysTimerCount() const override;
    uint32_t GetSysTimerFrequency() const override;
    void ForceContextSwitch(TId id) override;
    void ProcessTick() override;
    void ProcessHardFault() override;
    void SetEventOverrider(IEventOverrider *overrider, bool non_secure) override;
    Word GetCallerSP() const override;
    TId GetTid() const override;
    Timeout Suspend() override;
    void Resume(Timeout elapsed_ticks) override;
    void SetCpuFrequency(uint8_t core_id, uint32_t frequency) override;

    void SetSpecificEventHandler(ISpecificEventHandler *handler);
};

/*! \typedef PlatformDefault
    \brief   Default platform implementation.
*/
typedef PlatformRiscV PlatformDefault;

// Inline TLS via x4/tp. Active when both STK_TLS and STK_TLS_PREFER_REGISTER are enabled.
#if STK_TLS && STK_TLS_PREFER_REGISTER

/*! \brief  Get thread-local storage (TLS).
    \return TLS value.
    \note   tp register is an alias for x4
*/
static __stk_forceinline Word GetTls()
{
    Word tp;
    __asm__ __volatile__("mv %0, tp" : "=r"(tp) : /* input: none */ : /* clobbers: none */);
    return tp;
}

/*! \brief     Set thread-local storage (TLS).
    \param[in] tp: TLS value.
    \note      tp register is an alias for x4
*/
static __stk_forceinline void SetTls(Word tp)
{
    __asm__ __volatile__("mv tp, %0" : /* output: none */ : "r"(tp) : /* clobbers: none */);
}

// Notify stk_arch.h that we defined inline versions of GetTls/SetTls.
#define STK_INLINE_TLS (1)

#endif // STK_TLS && STK_TLS_PREFER_REGISTER

// =============================================================================
#if STK_MPU
// =============================================================================

/*! \def   STK_RISCV_PMP_REGIONS_MAX
    \brief Number of PMP entries implemented by the hart (16 supported by this driver).
*/
#ifndef STK_RISCV_PMP_REGIONS_MAX
    #define STK_RISCV_PMP_REGIONS_MAX (16U)
#endif

/*! \namespace stk::hw::mpu
    \brief     Memory protection (RISC-V PMP) configuration related API.
    \note      Unlocked PMP entries restrict only U-mode, therefore tasks with \a ACCESS_USER run in U-mode
               while \a ACCESS_PRIVILEGED tasks run in M-mode and are not restricted (this is equivalent of
               ARM Cortex-M MPU_CFG_PRIVILEGED_BG_MEM). Regions are NAPOT: size must be a power of two
               (>= 8 bytes) and the base address must be aligned to the size.
    \warning   MPU API is not accessible by non-Privileged contexts.
*/
namespace hw {
namespace mpu {

/*! \enum  EMpuAccess
    \brief Access permissions, value equals to PMP R/W bits of the U-mode. M-mode is never restricted by unlocked entries.
*/
enum EMpuAccess : uint8_t
{
    ACCESS_NONE             = 0xFFU, //!< Programmatic sentinel to disable this region slot completely.

    ACCESS_HW_NO_ACCESS     = 0x00U, //!< No access for U-mode.
    ACCESS_PRIV_RW_USER_NO  = 0x00U, //!< Privileged Read/Write, Unprivileged No Access.
    ACCESS_PRIV_RO_USER_NO  = 0x00U, //!< Same as ACCESS_PRIV_RW_USER_NO (M-mode can not be restricted without locking).
    ACCESS_PRIV_RW_USER_RO  = 0x01U, //!< Unprivileged Read-Only.
    ACCESS_PRIV_RO_USER_RO  = 0x01U, //!< Unprivileged Read-Only.
    ACCESS_FULL             = 0x03U  //!< Unprivileged Read/Write.
};

/*! \enum  EMpuExec
    \brief Execute permission. NOTE: PMP X bit means "allowed" (inverse of ARM XN), the enum hides it.
*/
enum EMpuExec : uint8_t
{
    EXEC_NEVER   = 0x00U, //!< Execution is not allowed.
    EXEC_ALLOWED = 0x04U  //!< Execution is allowed.
};

//! Memory type. Not supported by PMP (attributes are fixed by hardware PMAs), kept for source compatibility, ignored.
enum EMpuType : uint8_t
{
    TYPE_STRONGLY_ORDERED = 0U,
    TYPE_DEVICE           = 1U,
    TYPE_NORMAL_NON_CACHE = 2U,
    TYPE_NORMAL_CACHEABLE = 3U
};

//! Shareability. Not supported by PMP, kept for source compatibility, ignored.
enum EMpuShare : uint8_t
{
    SHARE_NON   = 0U,
    SHARE_OUTER = 1U,
    SHARE_INNER = 2U
};

/*! \enum  EMpuConfigFlags
    \brief MPU control flags for high-level STK configuration (see MpuConfig::mode).
*/
enum EMpuConfigFlags : uint32_t
{
    MPU_CFG_NONE              = 0U,        //!< No configuration flags set (corresponds to MpuConfig::MPU_OFF).
    MPU_CFG_PRIVILEGED_BG_MEM = (1U << 0), //!< Implicit on RISC-V (M-mode is not restricted by unlocked entries), accepted.
    MPU_CFG_IN_FAULTS         = (1U << 1), //!< Reserved (needs Smepmp), not supported (STK_ASSERT).
    MPU_CFG_CLEAR_ON_INIT     = (1U << 2), //!< Wipe all PMP entries before applying the static table.
    MPU_CFG_NONSECURE_MPU     = (1U << 3)  //!< Unused on RISC-V.
};

} // namespace mpu
} // namespace hw

/*! \struct MpuRegionConfig
    \brief  MPU region configuration descriptor (same layout as on ARM Cortex-M for a portable user code).
*/
struct MpuRegionConfig
{
    Word                addr;        //!< Base address pointing to the memory region (aligned to size).
    size_t              size;        //!< Size of the memory region (power of two, >= 8).
    hw::mpu::EMpuAccess access_perm; //!< MPU Access Permissions.
    hw::mpu::EMpuType   mem_type;    //!< Ignored.
    hw::mpu::EMpuShare  share;       //!< Ignored.
    hw::mpu::EMpuExec   exec;        //!< Execute permission.
};

/*! \struct FaultContext
    \brief  Memory access fault (PMP violation) context passed to IPlatform::IEventOverrider::OnException().
*/
struct FaultContext
{
    static constexpr size_t PMP_CFG_WORDS = (STK_RISCV_PMP_REGIONS_MAX + sizeof(Word) - 1U) / sizeof(Word);

    /*! \struct Pmp
        \brief  Hardware Memory Protection Unit (PMP) status register snapshot.
    */
    struct Mpu
    {
        Word cfg[PMP_CFG_WORDS];               //!< Snapshot of packed pmpcfg words.
        Word addr[STK_RISCV_PMP_REGIONS_MAX];  //!< Snapshot of pmpaddr registers.
    };

    Word mcause;                               //!< mcause (1: instruction, 5: load, 7: store/AMO access fault).
    Word mepc;                                 //!< PC of the faulting instruction.
    Word mtval;                                //!< Faulting address (if provided by hardware).
    Word mstatus;                              //!< mstatus at fault entry.
#if STK_MPU
    Mpu  mpu;                                  //!< PMP state.
#endif
};

// =============================================================================
#endif // STK_MPU
// =============================================================================

} // namespace stk

/*! \brief Data memory barrier.
*/
static __stk_forceinline void __stk_dmb() { __asm__ __volatile__("fence rw, rw" ::: "memory"); }

/*! \def   STK_SUBMICORSECOND_PRECISION_TIMER
    \brief Enables sub-microsecond precision timer, see \a hw::HiResClock.
    \note  By default timer precision is 1 microsecond.
*/
#ifndef STK_SUBMICORSECOND_PRECISION_TIMER
    #define STK_SUBMICORSECOND_PRECISION_TIMER 0
#endif

/*! \def   STK_SYSTEM_CORE_CLOCK_FREQUENCY
    \brief System clock frequency in Hz. Default: 150 MHz.
*/
#ifndef STK_SYSTEM_CORE_CLOCK_FREQUENCY
    #define STK_SYSTEM_CORE_CLOCK_FREQUENCY 150000000U
#endif

/*! \var   STK_SYSTEM_CORE_CLOCK_VAR
    \brief System clock frequency in Hz.
*/
extern "C" volatile uint32_t STK_SYSTEM_CORE_CLOCK_VAR;

#endif /* STK_ARCH_RISC_V_H_ */
