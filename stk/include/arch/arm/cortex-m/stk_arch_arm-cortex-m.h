/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_ARCH_ARM_CORTEX_M_H_
#define STK_ARCH_ARM_CORTEX_M_H_

#include "stk_common.h"
#if defined(__ARM_FEATURE_CMSE) && (__ARM_FEATURE_CMSE != 0)
    #include <arm_cmse.h> // for ARM TrustZone
#endif

/*! \def   STK_ARCH_ARMV6_M
    \brief ARMv6-M platform (Cortex-M0, Cortex-M0+, Cortex-M1).
*/
#ifndef STK_ARCH_ARMV6_M
    #if defined(__ARM_ARCH_6M__)
        #define STK_ARCH_ARMV6_M (1)
    #else
        #define STK_ARCH_ARMV6_M (0)
    #endif
#else
    #if (STK_ARCH_ARMV6_M == 0) && defined(__ARM_ARCH_6M__)
        #error "STK_ARCH_ARMV6_M must be defined as 1 on ARMv6-M platform!"
    #endif
#endif

/*! \def   STK_ARCH_ARMV7_M
    \brief ARMv7-M platform (Cortex-M3, Cortex-M4, Cortex-M7).
*/
#ifndef STK_ARCH_ARMV7_M
    #if defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__)
        #define STK_ARCH_ARMV7_M (1)
    #else
        #define STK_ARCH_ARMV7_M (0)
    #endif
#else
    #if (STK_ARCH_ARMV7_M == 0) && (defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__))
        #error "STK_ARCH_ARMV7_M must be defined as 1 on ARMv7-M platform!"
    #endif
#endif

/*! \def   STK_ARCH_ARMV8_M
    \brief ARMv8-M platform (Cortex-M23, Cortex-M33, Cortex-M55, Cortex-M85).
*/
#ifndef STK_ARCH_ARMV8_M
    #if defined(__ARM_ARCH_8M_MAIN__) || defined(__ARM_ARCH_8M_BASE__)
        #define STK_ARCH_ARMV8_M (1)
    #else
        #define STK_ARCH_ARMV8_M (0)
    #endif
#else
    #if (STK_ARCH_ARMV8_M == 0) && (defined(__ARM_ARCH_8M_MAIN__) || defined(__ARM_ARCH_8M_BASE__))
        #error "STK_ARCH_ARMV8_M must be defined as 1 on ARMv8-M platform!"
    #endif
#endif

// Expect at least one supported target architecture.
#if !STK_ARCH_ARMV6_M && !STK_ARCH_ARMV7_M && !STK_ARCH_ARMV8_M
    #error "Unsupported ARM architecture target!"
#endif

// Enforce single active target architecture state.
#if (STK_ARCH_ARMV6_M + STK_ARCH_ARMV7_M + STK_ARCH_ARMV8_M) > 1
    #error "Multiple STK_ARCH_ARMvX flags active simultaneously! Check build environment definitions."
#endif

/*! \def   STK_TZ_SECURE
    \brief ARM TrustZone: Defines Secure (1) build.
*/
#if defined(__ARM_FEATURE_CMSE) && (__ARM_FEATURE_CMSE == 3)
    #define STK_TZ_SECURE (1)
#else
    #define STK_TZ_SECURE (0)
#endif

/*! \def   STK_TZ_NON_SECURE
    \brief ARM TrustZone: Defines Non-Secure (1) build.
*/
#if defined(__ARM_FEATURE_CMSE) && (__ARM_FEATURE_CMSE == 1)
    #define STK_TZ_NON_SECURE (1)
#else
    #define STK_TZ_NON_SECURE (0)
#endif

/*! \def   __stk_tz_nsc_entry
    \brief ARM TrustZone: attribute for Non-Secure callable gateway functions.
    \note  Places the function in the .nsc_entry section mapped to the NSC region.
*/
#if defined(__ARM_FEATURE_CMSE) && (__ARM_FEATURE_CMSE == 3)
    #define __stk_tz_nsc_entry __attribute__((cmse_nonsecure_entry))
#else
    #define __stk_tz_nsc_entry
#endif

/*! \def   __stk_tz_ns_call
    \brief ARM TrustZone: attribute for calling Non-Secure functions from Secure state.
*/
#if defined(__ARM_FEATURE_CMSE) && (__ARM_FEATURE_CMSE == 3)
    #define __stk_tz_ns_call __attribute__((cmse_nonsecure_call))
#else
    #define __stk_tz_ns_call
#endif

/*! \def   STK_NSC_GATEWAY
    \brief ARM TrustZone: Non-secure gateway to Secure API.
*/
#define STK_TZ_NSC_GATEWAY extern "C" __stk_tz_nsc_entry

// ARM TrustZone Non-Secure binary configuration validation.
#ifdef _STK_CORTEX_M_TRUSTZONE_NON_SECURE
#if !STK_TZ_NON_SECURE
    #error "Do not use -cmse compiler flag for Non-Secure binary compilation!"
#endif
#endif

// Task MPU is supported only when MPU is enabled globally.
#if STK_MPU_STACK_GUARD && !STK_MPU
    #error "Enable MPU support (STK_MPU=1) to use per-task MPU feature (STK_MPU_STACK_GUARD=1)!"
#endif

/*! \def   STK_CORTEX_M_MPU_REGIONS_MAX
    \brief Number of MPU regions supported by MPU peripheral.
*/
#ifndef STK_CORTEX_M_MPU_REGIONS_MAX
    #define STK_CORTEX_M_MPU_REGIONS_MAX (8U)
#endif

/*! \brief Hardware memory barrier: ensures visibility across cores and bus masters.
*/
static __stk_forceinline void __stk_dmb() { __asm volatile("dmb sy" ::: "memory"); }

/*! \file  stk_arch_arm-cortex-m.h
    \brief Platform port for ARM Cortex-M.
*/

namespace stk {

/*! \class PlatformArmCortexM
    \brief Concrete implementation of IPlatform driver for the Arm Cortex-M0, M3, M4, M7 processors.
*/
class PlatformArmCortexM final : public IPlatform
{
public:
    /*! \brief Destructor.
        \note  MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    STK_VIRT_DTOR ~PlatformArmCortexM() = default;

    void Initialize(IEventHandler *event_handler, IKernelService *service, uint32_t resolution_us, Stack *exit_trap) override;
    void Start() override;
    void Stop() override;
    void InitStack(EStackType stack_type, Stack *stack, IStackMemory *stack_memory, ITask *user_task) override;
    uint32_t GetTickResolution() const override;
    Cycles GetSysTimerCount() const override;
    uint32_t GetSysTimerFrequency() const override;
    void SwitchToNext() override;
    void Sleep(Timeout ticks) override;
    bool SleepUntil(Ticks timestamp) override;
    EWaitResult Wait(ISyncObject *sync_obj, IMutex *mutex, Timeout timeout) override;
    void ProcessTick() override;
    void ProcessHardFault() override;
    void SetEventOverrider(IEventOverrider *overrider, bool non_secure) override;
    Word GetCallerSP() const override;
    TId GetTid() const override;
    Timeout Suspend() override;
    void Resume(Timeout elapsed_ticks) override;
    void SetCpuFrequency(uint8_t core_id, uint32_t frequency) override;
};

/*! \typedef PlatformDefault
    \brief   Default platform implementation.
*/
typedef PlatformArmCortexM PlatformDefault;

// Inline TLS via r9. Active when both STK_TLS and STK_TLS_PREFER_REGISTER are
// enabled. Requires -ffixed-r9 on all translation units containing task code
// (see GetTls/SetTls warnings below). If -ffixed-r9 is unavailable or
// undesirable, leave STK_TLS_PREFER_REGISTER disabled; the kernel will fall back
// to a memory-based TLS slot with a small additional load/store per access.
// =============================================================================
#if STK_TLS && STK_TLS_PREFER_REGISTER
// =============================================================================

/*! \brief   Get thread-local storage (TLS).
    \return  TLS value.
    \note    Uses r9 as the TLS base pointer per the ARM EABI "platform register"
             convention (AAPCS 5.2.2).  The value is saved and restored by PendSV
             on every context switch, so each task sees its own TLS pointer on entry.
    \warning **Requires \c -ffixed-r9 for every translation unit that contains task
             code** - including any library code a task calls that touches r9.
             Without it, the compiler treats r9 as an ordinary callee-saved register:
             it may spill r9 to the stack in a function prologue, overwrite it with an
             intermediate value (e.g. during 64-bit arithmetic), and restore it in the
             epilogue.
    \see     SetTls, stk::hw::GetTlsPtr, stk::hw::SetTlsPtr
*/
static __stk_forceinline Word GetTls()
{
    Word tp;
    __asm volatile("MOV %0, r9" : "=r"(tp) : /* input: none */ : /* clobbers: none */);
    return tp;
}

/*! \brief     Set thread-local storage (TLS).
    \param[in] tp: TLS value to store in r9.
    \note      Uses the r9 register as the TLS base pointer, following the ARM EABI
               "platform register" convention (AAPCS §5.2.2).
    \warning   **Requires \c -ffixed-r9 for every translation unit that contains task
               code.** See \c GetTls() for the full explanation.
    \see       GetTls, stk::hw::GetTlsPtr, stk::hw::SetTlsPtr
*/
static __stk_forceinline void SetTls(Word tp)
{
    __asm volatile("MOV r9, %0" : /* output: none */ : "r"(tp) : /* clobbers: none */);
}

// Notify stk_arch.h that we defined inline versions of GetTls/SetTls.
#define STK_INLINE_TLS 1

// =============================================================================
#endif // STK_TLS_PREFER_REGISTER
// =============================================================================

namespace hw {

/*! \struct ExceptionFrame
    \brief  ARMv7-M/ARMv8-M hardware exception frame (8 words, highest address on the stack).
*/
struct ExceptionFrame
{
    Word R0;
    Word R1;
    Word R2;
    Word R3;
    Word R12;
    Word LR;
    Word PC;
    Word xPSR;
};

} // namespace hw

// =============================================================================
#if STK_MPU
// =============================================================================

/*! \namespace stk::hw::mpu
    \brief     Memory Protection Unit (MPU) configuration related API.
    \warning   MPU API is not accessible by non-Priviligeded, non-Secure contexts.
*/
namespace hw {
namespace mpu {

#if STK_ARCH_ARMV8_M

// ARMv8-M MPU Variant (PMSAv8 Layout)

/*! \enum    EMpuAccess
    \brief   MPU Access Permissions (AP) for ARMv8-M (PMSAv8).
             Controls Privileged/Unprivileged read/write states via RBAR bits [2:1].
    \note    ARMv8-M hardware natively drops the Privileged R/W + User RO combination available in ARMv7-M.
*/
enum EMpuAccess : uint8_t
{
    ACCESS_NONE             = 0xFFU,  //!< Programmatic sentinel to disable this region slot completely, do not apply to hardware register.

    ACCESS_PRIV_RW_USER_NO  = (0x0U << 1U), //!< Privileged Read/Write, Unprivileged No Access.
    ACCESS_FULL             = (0x1U << 1U), //!< Privileged Read/Write, Unprivileged Read/Write.
    ACCESS_PRIV_RO_USER_NO  = (0x2U << 1U), //!< Privileged Read-Only, Unprivileged No Access.
    ACCESS_PRIV_RO_USER_RO  = (0x3U << 1U)  //!< Privileged Read-Only, Unprivileged Read-Only.
};

/*! \enum    EMpuExec
    \brief   MPU Execute-Never (XN) configuration for ARMv8-M (PMSAv8).
             Controls instruction execution tracking via RBAR bit 0.
*/
enum EMpuExec : uint8_t
{
    EXEC_ALLOWED            = (0x0U << 0U), //!< XN = 0 (Execution Allowed)
    EXEC_NEVER              = (0x1U << 0U)  //!< XN = 1 (Execute-Never)
};

/*! \enum    EMpuType
    \brief   MPU Memory attribute index mappings for ARMv8-M (PMSAv8).
             Maps directly to the allocated index positions within the global MAIR memory profile registers.
    \see     MAIR0_PMSAV8_INIT
*/
enum EMpuType : uint8_t
{
    TYPE_STRONGLY_ORDERED   = 0U, //!< MAIR0 index for strict ordering (Attr0=0x00 -> Device-nGnRnE).
    TYPE_DEVICE             = 1U, //!< MAIR0 index for peripheral registers (Attr1=0x04 -> Device-nGnRE (or Normal Non-Cacheable)).
    TYPE_NORMAL_NON_CACHE   = 2U, //!< MAIR0 index for non-cacheable spaces (DMA) (Attr2=0x44 -> Normal, Outer/Inner Write-Through Non-Transient).
    TYPE_NORMAL_CACHEABLE   = 3U  //!< MAIR0 index for standard cached memory (SRAM) (Attr3=0xFF -> Normal, Outer/Inner Write-Back Read/Write-Allocate).
};

/*! \enum    EMpuShare
    \brief   MPU Shareability (SH) configuration for ARMv8-M (PMSAv8).
             Controls hardware data coherency across observers via RBAR bits [4:3].
    \note    Only effective when applied to Normal memory types. Ignored for Device memory.
*/
enum EMpuShare : uint8_t
{
    SHARE_NON               = (0x0U << 3U), //!< Non-shareable (Private to core, maximum cache performance).
    SHARE_OUTER             = (0x2U << 3U), //!< Outer Shareable (Coherent with DMA, GPU, external masters).
    SHARE_INNER             = (0x3U << 3U)  //!< Inner Shareable (Coherent across multi-core CPU clusters).
};

#else // !STK_ARCH_ARMV8_M

// Legacy ARMv7-M MPU Variant (PMSAv7 Layout)

/*! \enum    EMpuAccess
    \brief   MPU Access Permissions (AP) for controlling Privileged/Unprivileged read/write states.
*/
enum EMpuAccess : uint32_t
{
	ACCESS_NONE             = 0xFFFFFFFFU,   //!< Programmatic sentinel to disable this region slot completely, do not apply to hardware register.

    ACCESS_HW_NO_ACCESS     = (0x0U << 24U), //!< No access permitted (incompatible with ARMv8-M, actual hardware bits for an ACTIVE region with NO access permissions).
    ACCESS_PRIV_RW_USER_NO  = (0x1U << 24U), //!< Privileged Read/Write, Unprivileged No Access.
    ACCESS_PRIV_RW_USER_RO  = (0x2U << 24U), //!< Privileged Read/Write, Unprivileged Read-Only (incompatible with ARMv8-M).
    ACCESS_FULL             = (0x3U << 24U), //!< Privileged Read/Write, Unprivileged Read/Write.
    ACCESS_PRIV_RO_USER_NO  = (0x5U << 24U), //!< Privileged Read-Only, Unprivileged No Access.
    ACCESS_PRIV_RO_USER_RO  = (0x6U << 24U)  //!< Privileged Read-Only, Unprivileged Read-Only.
};

/*! \enum    EMpuExec
    \brief   MPU Execute-Never (XN) configuration to permit or block code execution.
*/
enum EMpuExec : uint32_t
{
    EXEC_ALLOWED            = (0x0U << 28U),
    EXEC_NEVER              = (0x1U << 28U)
};

/*! \enum    EMpuType
    \brief   MPU Memory attribute definitions (TEX, C, B bit combinations).
*/
enum EMpuType : uint32_t
{
    TYPE_STRONGLY_ORDERED   = 0x000000U, //!< TEX=000,C=0,B=0 -> Strongly ordered.
    TYPE_DEVICE             = 0x010000U, //!< TEX=000,C=0,B=1 -> Device, Shareable.
    TYPE_NORMAL_NON_CACHE   = 0x080000U, //!< TEX=001,C=0,B=0 -> Normal, non-cacheable (DMA buffers and etc).
    TYPE_NORMAL_CACHEABLE   = 0x030000U  //!< TEX=000,C=1,B=1 -> Normal, Write-Back no-write-allocate.
};

/*! \enum    EMpuShare
    \brief   MPU Shareability (S) configuration for ARMv7-M (PMSAv7).
             Controls hardware data coherency across observers via RASR bit [18].
    \note    Only effective when applied to Normal memory types. Ignored for Device and Strongly-ordered memory.
             ARMv7-M treats INNER and OUTER identically as a single Shareable attribute.
*/
enum EMpuShare : uint32_t
{
    SHARE_NON               = (0x0U << 18U), //!< Non-shareable (Private to the local core).
    SHARE_OUTER             = (0x1U << 18U), //!< Maps to Shareable on ARMv7-M.
    SHARE_INNER             = (0x1U << 18U)  //!< Maps to Shareable on ARMv7-M.
};

#endif // STK_ARCH_ARMV8_M

/*! \class EMpuConfigFlags
    \brief MPU control flags for high-level STK configuration.
*/
enum EMpuConfigFlags : uint32_t
{
    MPU_CFG_NONE              = 0U,        //!< No configuration flags set (corresponds to MpuConfig::MODE_OFF).
    MPU_CFG_PRIVILEGED_BG_MEM = (1U << 0), //!< Enable default background memory map for privileged access (Cortex-M CTRL.PRIVDEFENA).
    MPU_CFG_IN_FAULTS         = (1U << 1), //!< Keep MPU active during HardFault and NMI handlers (Cortex-M CTRL.HFNMIENA).
    MPU_CFG_CLEAR_ON_INIT     = (1U << 2), //!< Wipe/reset all existing region registers upon initialization, before applying the static table below.
    MPU_CFG_NONSECURE_MPU     = (1U << 3)  //!< Target Non-Secure MPU instance (ARMv8-M TrustZone context). Absent means Secure MPU instance (default). Ignored outside a TrustZone build. Must match the overrider (Secure/Non-Secure) the config was returned from, verified via STK_ASSERT.
};

} // namespace mpu
} // namespace hw

/*! \class MpuRegionConfig
    \brief MPU region configuration descriptor.
*/
struct MpuRegionConfig
{
    Word                addr;        //!< Base address pointing to the memory region.
    size_t              size;        //!< Size of the memory region.
    hw::mpu::EMpuAccess access_perm; //!< MPU Access Permissions.
    hw::mpu::EMpuType   mem_type;    //!< MPU Memory attributes.
    hw::mpu::EMpuShare  share;       //!< MPU Memory shareability.
    hw::mpu::EMpuExec   exec;        //!< MPU Execute-Never.
};

/*! Recommended MPU configuration for per-task MPU regions of non-Privileged tasks:

    \code
    stk::MpuRegionConfigResult MyNonSecureTask::GetMpuRegions() override
    {
        using namespace stk;

        extern char __stk_mpu_shared_code_start[];
        extern char __stk_mpu_shared_code_end[];
        extern char __stk_mpu_shared_data_start[];
        extern char __stk_mpu_shared_data_end[];

        static const MpuRegionConfig s_mpu_shared_regions[] =
        {
            {
              .addr        = hw::PtrToWord(__stk_mpu_shared_code_start),
              .size        = hw::PtrToWord(__stk_mpu_shared_code_end) - hw::PtrToWord(__stk_mpu_shared_code_start),
              .access_perm = hw::mpu::ACCESS_PRIV_RO_USER_RO,
              .mem_type    = hw::mpu::TYPE_NORMAL_CACHEABLE,
              .exec        = hw::mpu::EXEC_ALLOWED
            },
            {
              .addr        = hw::PtrToWord(__stk_mpu_shared_data_start),
              .size        = hw::PtrToWord(__stk_mpu_shared_data_end) - hw::PtrToWord(__stk_mpu_shared_data_start),
              .access_perm = hw::mpu::ACCESS_FULL,
              .mem_type    = hw::mpu::TYPE_NORMAL_CACHEABLE,
              .exec        = hw::mpu::EXEC_NEVER
            },
        };

        return stk::MpuRegionConfigResult(s_mpu_shared_regions, STK_STATIC_ARRAY_SIZE(s_mpu_shared_regions));
    }
    \endcode
*/

// =============================================================================
#endif // STK_MPU
// =============================================================================

/*! \struct  FaultContext
    \brief   ARMv7-M/ARMv8-M system fault exception context state capture.
    \details Preserves exception stack frames, core system control fault status registers (CFSR, HFSR, MMFAR, BFAR, AFSR),
             execution context status (CONTROL, EXC_RETURN), and hardware Memory Protection Unit (MPU) state snapshots
             at the time of fault occurrence for post-mortem diagnostics.
    \see     hw::ExceptionFrame, Fill
*/
struct FaultContext
{
    /*! \struct Mpu
        \brief  Hardware Memory Protection Unit (MPU) status register snapshot.
    */
    struct Mpu
    {
        /*! \struct Region
            \brief  Register snapshot for an individual hardware MPU region slot.
        */
        struct Region
        {
            Word RNR;  //!< Region Number Register (RNR).
            Word RBAR; //!< Region Base Address Register (RBAR).
            Word ATTR; //!< Region Attribute and Size Register (RASR / RLAR / MPU_RLAR).
        };

        Word   CTRL;  //!< MPU Control Register (MPU_CTRL).
    #if STK_ARCH_ARMV8_M
        Word   MAIR0; //!< Memory Attribute Indirection Register 0 (MAIR0, ARMv8-M).
        Word   MAIR1; //!< Memory Attribute Indirection Register 1 (MAIR1, ARMv8-M).
    #endif
        Region regions[STK_CORTEX_M_MPU_REGIONS_MAX]; //!< Active hardware MPU region register state table.
    };

    hw::ExceptionFrame frame;       //!< Saved hardware exception stack frame.
    Word               CFSR;        //!< Configurable Fault Status Register (CFSR: MemManage, BusFault, UsageFault).
    Word               HFSR;        //!< HardFault Status Register (HFSR).
    Word               MMFAR;       //!< Memory Management Fault Address Register (MMFAR).
    Word               BFAR;        //!< BusFault Address Register (BFAR).
    Word               AFSR;        //!< Auxiliary Fault Status Register (AFSR).
    Word               CONTROL;     //!< Core CONTROL register state snapshot.
    Word               EXC_RETURN;  //!< Exception return magic value (EXC_RETURN).
#if STK_MPU
    Mpu                mpu;         //!< Active or Secure MPU hardware configuration snapshot.
    #if STK_ARCH_ARMV8_M && STK_TZ_SECURE
    Mpu                mpu_ns;      //!< Non-Secure MPU hardware configuration snapshot (ARMv8-M TrustZone).
    #endif
#endif
    bool               mmfar_valid; //!< Flag indicating validity of MMFAR address value (derived from CFSR.MMARVALID).
    bool               bfar_valid;  //!< Flag indicating validity of BFAR address value (derived from CFSR.BFARVALID).

    /*! \brief     Populate fault context registers and hardware MPU state from an exception stack frame.
        \param[in] stacked_regs: Pointer to the hardware-stacked exception register frame.
        \param[in] exc_return: EXC_RETURN execution state value captured at fault entry.
    */
    void Fill(const Word *stacked_regs, Word exc_return);
};

} // namespace stk

#endif /* STK_ARCH_ARM_CORTEX_M_H_ */
