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

/*! \def   STK_ARCH_ARMV8_M
    \brief ARMv8-M platform.
*/
#ifndef STK_ARCH_ARMV8_M
    #if defined(__ARM_ARCH_8M_MAIN__) || defined(__ARM_ARCH_8M_BASE__)
        #define STK_ARCH_ARMV8_M (1)
    #else
        #define STK_ARCH_ARMV8_M (0)
    #endif
#else
    #if defined(__ARM_ARCH_8M_MAIN__) || defined(__ARM_ARCH_8M_BASE__)
        #error "STK_ARCH_ARMV8_M must be defined on ARMv8-M platform!"
    #endif
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

/*! \def   STK_CORTEX_M_MPU_TASK_REGION_IDX
    \brief MPU region index reserved for the per-task stack guard (must not collide
           with any statically-configured regions, e.g. flash/RAM/peripheral/null-guard).
    \note  Last 4 regions are reserved for a per-task MPU.
*/
#ifndef STK_CORTEX_M_MPU_TASK_REGION_IDX
    #define STK_CORTEX_M_MPU_TASK_REGION_IDX (STK_CORTEX_M_MPU_REGIONS_MAX - 4U)
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
    void SetEventOverrider(IEventOverrider *overrider) override;
    Word GetCallerSP() const override;
    TId GetTid() const override;
    Timeout Suspend() override;
    void Resume(Timeout elapsed_ticks) override;
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
enum EMpuAccess : uint32_t
{
    ACCESS_NONE             = 0xFFFFFFFFU,  //!< Programmatic sentinel to disable this region slot completely, do not apply to hardware register.

    ACCESS_PRIV_RW_USER_NO  = (0x0U << 1U), //!< Privileged Read/Write, Unprivileged No Access.
    ACCESS_FULL             = (0x1U << 1U), //!< Privileged Read/Write, Unprivileged Read/Write.
    ACCESS_PRIV_RO_USER_NO  = (0x2U << 1U), //!< Privileged Read-Only, Unprivileged No Access.
    ACCESS_PRIV_RO_USER_RO  = (0x3U << 1U)  //!< Privileged Read-Only, Unprivileged Read-Only.
};

/*! \enum    EMpuExec
    \brief   MPU Execute-Never (XN) configuration for ARMv8-M (PMSAv8).
             Controls instruction execution tracking via RBAR bit 0.
*/
enum EMpuExec : uint32_t
{
    EXEC_ALLOWED            = (0x0U << 0U),
    EXEC_NEVER              = (0x1U << 0U)
};

/*! \enum    EMpuType
    \brief   MPU Memory attribute index mappings for ARMv8-M (PMSAv8).
             Maps directly to the allocated index positions within the global MAIR registers.
    \see     MAIR0_PMSAV8_INIT
*/
enum EMpuType : uint32_t
{
    TYPE_STRONGLY_ORDERED   = 0U, //!< Index targeting MAIR0 memory profile for strict ordering (Attr0=0x00).
    TYPE_DEVICE             = 1U, //!< Index targeting MAIR0 memory profile for peripheral registers (Attr1=0x04).
    TYPE_NORMAL_NON_CACHE   = 2U, //!< Index targeting MAIR0 memory profile for non-cacheable spaces (DMA) (Attr2=0x44).
    TYPE_NORMAL_CACHEABLE   = 3U  //!< Index targeting MAIR0 memory profile for standard cached memory (SRAM) (Attr3=0xFF).
};

/*! \var     MAIR0_PMSAV8_INIT.
    \brief   MPU MAIR0 register configuration: Attr3=0xFF, Attr2=0x44, Attr1=0x04, Attr0=0x00.
    \see     EMpuType
*/
static constexpr uint32_t MAIR0_PMSAV8_INIT = 0xFF440400U;

/*! \var     MAIR1_PMSAV8_INIT.
    \brief   MPU MAIR1 register configuration: Reserved/Unused by EMpuType indices.
    \see     EMpuType
*/
static constexpr uint32_t MAIR1_PMSAV8_INIT = 0x00000000U;

/*! \enum    EMpuShare
    \brief   MPU Shareability (SH) configuration for ARMv8-M (PMSAv8).
             Controls hardware data coherency across observers via RBAR bits [4:3].
    \note    Only effective when applied to Normal memory types. Ignored for Device memory.
*/
enum EMpuShare : uint32_t
{
    SHARE_NON               = (0x0U << 3U), //!< Non-shareable (Private to core, maximum cache performance).
    SHARE_OUTER             = (0x2U << 3U), //!< Outer Shareable (Coherent with DMA, GPU, external masters).
    SHARE_INNER             = (0x3U << 3U)  //!< Inner Shareable (Coherent across multi-core CPU clusters).
};

static constexpr Word RLAR_ENABLE_FLAG = (1U << 0U);

static constexpr Word RBAR_DISABLED_REGION(uint32_t /*region_idx*/) { return 0U; }
static constexpr Word RLAR_DISABLED_REGION = 0U;

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

static constexpr Word RASR_ENABLE_FLAG = (1U << 0U);

static constexpr Word RBAR_DISABLED_REGION(uint32_t region_idx) { return (1U << 4U) | (region_idx & 0xFU); }
static constexpr Word RASR_DISABLED_REGION = 0U;

#endif // STK_ARCH_ARMV8_M

/*! \brief     Compute MPU register values for a region without touching hardware.
    \details   Translates a portable, byte-address/byte-size \a cfg descriptor into the
               raw \a reg.addr / \a reg.attr pair the driver later writes verbatim to
               MPU->RBAR and MPU->RASR (ARMv7-M/PMSAv7) or MPU->RBAR and MPU->RLAR
               (ARMv8-M/PMSAv8) via ApplyRegion(). Pure computation only, safe to call
               with the MPU enabled or disabled, from any context.
    \param[out] reg: Destination region descriptor to populate. Any previous contents
               are fully overwritten.
    \param[in] cfg: Region configuration to encode. \c cfg.region_idx is only consumed
               on ARMv7-M/PMSAv7, where it is baked directly into \a reg.addr (the
               legacy RBAR REGION field); on ARMv8-M/PMSAv8, region selection instead
               happens purely via MPU->RNR in ApplyRegion(), so \c cfg.region_idx is
               unused there.
    \note      ARMv8-M/PMSAv8: \c cfg.addr and \c cfg.size must both be 32-byte aligned
               and \c cfg.size must be non-zero; violations trigger STK_ASSERT.
    \note      ARMv7-M/PMSAv7: \c cfg.size must be a power of two and \c cfg.addr must
               be aligned to \c cfg.size; violations trigger STK_ASSERT. As a special
               case, \c cfg.size == 0 produces a descriptor for a *disabled* region
               (RASR_DISABLED_REGION) rather than asserting, used to leave unused
               per-task region slots inert.
    \warning   \a reg must not be reordered or reinterpreted independently of
               ApplyRegion(); its member layout (addr/attr) is driver-internal and
               matches the raw register semantics for the active architecture variant.
    \see       ApplyRegion, ConfigureTable
*/
void ConfigureRegion(MpuRegion &reg, const struct MpuRegionConfig &cfg);

/*! \brief     Configure and apply a full table of static (non per-task) MPU regions in one call.
    \details   Disables the MPU, writes every entry of \a cfg_list to its target region index,
               then re-enables the MPU with \a control_flags (e.g. MPU_CTRL_PRIVDEFENA_Msk).
               Intended for one-shot boot-time setup of global regions (flash/RAM/peripherals/
               null-guard), as opposed to TaskMpu::Configure(), which populates the per-task
               stack-guard region shadow table consumed by STK_ASM_BLOCK_MPU_STACK_GUARD on
               every context switch.
    \param[in] cfg_list: Array of region configurations. Each entry's \c region_idx selects
               its target MPU region index.
    \param[in] cfg_count: Number of entries in \a cfg_list.
    \param[in] control_flags: Extra MPU_CTRL bits to OR in on enable (e.g. PRIVDEFENA_Msk).
    \param[in] non_secure: (ARMv8-M TrustZone only) target the Non-Secure MPU alias.
    \see       ConfigureRegion, ApplyRegion, Enable
*/
void ConfigureTable(const MpuRegionConfig cfg_list[], size_t cfg_count, uint32_t control_flags, bool non_secure = false);

/*! \brief     Configure NPU region of the task.
    \param[in] task_mpu: Reference to \a TaskMpu instance of the task.
    \param[in] cfg_list: Array of MPU region configurations.
    \param[in] cfg_count: Number of entries in the \a cfg_list array.
    \param[in] non_secure: (ARMv8-M TrustZone only) True for a non-secure task, False otherwise. Ignored on
               ARMv7-M/PMSAv7.
*/
void ConfigureTask(TaskMpu &task_mpu, const struct MpuRegionConfig cfg_list[], const size_t cfg_count, bool non_secure = false);

/*! \brief     Write a previously computed region descriptor into a live MPU region slot.
    \details   Selects the region via MPU->RNR = \a index, then writes \a reg.addr to
               MPU->RBAR and \a reg.attr to MPU->RASR (ARMv7-M/PMSAv7) or MPU->RLAR
               (ARMv8-M/PMSAv8).
    \param[in] reg: Region descriptor previously populated by ConfigureRegion().
    \param[in] index: Target MPU region index (written to MPU->RNR). On ARMv7-M/PMSAv7
               this should match the \c region_idx baked into \a reg.addr by
               ConfigureRegion(); passing a mismatched index selects the wrong hardware
               slot while still encoding the original region number in RBAR.
    \param[in] non_secure: (ARMv8-M TrustZone only) if true, target the Non-Secure MPU
               alias (MPU_NS) instead of the Secure MPU. Ignored on ARMv7-M/PMSAv7.
    \warning   Performs no barrier or enable/disable handling of its own, the caller
               is responsible for ensuring the write is safe (typically by bracketing
               a batch of ApplyRegion() calls between \c Enable(false, ...) and
               \c Enable(true, ...), as ConfigureTable() does).
    \see       ConfigureRegion, Enable, ConfigureTable
*/
void ApplyRegion(const MpuRegion &reg, uint32_t index, bool non_secure = false);

/*! \brief     Disable a single MPU region without touching any other region.
    \param[in] index: Region index to disable (written to MPU->RNR).
    \param[in] non_secure: (ARMv8-M TrustZone only) target the Non-Secure MPU alias.
    \note      Writes RASR_DISABLED_REGION / RLAR_DISABLED_REGION at \a index; the region
               slot remains selectable again later via ApplyRegion() with the same index.
    \see       ApplyRegion, ConfigureTable
*/
void DisableRegion(uint32_t index, bool non_secure = false);

/*! \brief     Enable or disable the MPU as a whole, together with MemManage fault reporting.
    \details   On enable: writes \a control_flags | MPU_CTRL_ENABLE_Msk to MPU->CTRL and
               sets SCB->SHCSR.MEMFAULTENA (so MPU violations raise a MemManage fault
               instead of silently escalating to HardFault). On disable: clears
               SHCSR.MEMFAULTENA first, then clears MPU_CTRL_ENABLE_Msk. A DMB precedes
               the change and a DSB+ISB follow it, so the new MPU state is guaranteed to
               be in effect before this function returns.
    \param[in] enable: true to enable the MPU, false to disable it.
    \param[in] control_flags: Extra bits OR'd into MPU->CTRL when enabling (e.g.
               MPU_CTRL_PRIVDEFENA_Msk to fall back to the default background map for
               addresses not covered by any configured region). Ignored when
               \a enable is false.
    \param[in] non_secure: (ARMv8-M TrustZone only) if true, target the Non-Secure MPU
               and SCB aliases (MPU_NS / SCB_NS) instead of the Secure ones. Ignored on
               ARMv7-M/PMSAv7.
    \note      Typically called in pairs around a batch of ApplyRegion() calls:
               \c Enable(false, ...) before reconfiguring, \c Enable(true, ...) after.
               ConfigureTable() does this automatically.
    \see       ApplyRegion, DisableRegion, ConfigureTable
*/
void Enable(bool enable, uint32_t control_flags, bool non_secure = false);

} // namespace mpu
} // namespace hw

/*! \class MpuRegionConfig
    \brief MPU region configuration descriptor.
*/
struct MpuRegionConfig
{
    uint8_t             region_idx;  //!< Region index.
    Word                addr;        //!< Base address pointing to the memory region.
    size_t              size;        //!< Size of the memory region.
    hw::mpu::EMpuAccess access_perm; //!< MPU Access Permissions.
    hw::mpu::EMpuType   mem_type;    //!< MPU Memory attributes.
    hw::mpu::EMpuShare  share;       //!< MPU Memory shareability.
    hw::mpu::EMpuExec   exec;        //!< MPU Execute-Never.
};

/*! Recommended MPU configuration for per-task MPU regions of non-Privileged tasks:

    \code
    const stk::MpuRegionConfig *MyNonSecureTask::GetMpuRegions(uint8_t &out_count) override
    {
        using namespace stk;

        extern char __stk_mpu_shared_code_start[];
        extern char __stk_mpu_shared_code_end[];
        extern char __stk_mpu_shared_data_start[];
        extern char __stk_mpu_shared_data_end[];

        static const MpuRegionConfig s_mpu_shared_regions[] =
        {
            {
              .region_idx  = 1,
              .addr        = hw::PtrToWord(__stk_mpu_shared_code_start),
              .size        = hw::PtrToWord(__stk_mpu_shared_code_end) - hw::PtrToWord(__stk_mpu_shared_code_start),
              .access_perm = hw::mpu::ACCESS_PRIV_RO_USER_RO,
              .mem_type    = hw::mpu::TYPE_NORMAL_CACHEABLE,
              .exec        = hw::mpu::EXEC_ALLOWED
            },
            {
              .region_idx  = 2,
              .addr        = hw::PtrToWord(__stk_mpu_shared_data_start),
              .size        = hw::PtrToWord(__stk_mpu_shared_data_end) - hw::PtrToWord(__stk_mpu_shared_data_start),
              .access_perm = hw::mpu::ACCESS_FULL,
              .mem_type    = hw::mpu::TYPE_NORMAL_CACHEABLE,
              .exec        = hw::mpu::EXEC_NEVER
            },
        };

        out_count = STK_STATIC_ARRAY_SIZE(s_mpu_shared_regions);
        return s_mpu_shared_regions;
    }
    \endcode
*/

/*! \def   STK_MPU_SHARED_DATA_SECTION
    \brief Attribute macro to place global or static variables into the shared MPU data memory section.

    Any variables tagged with this macro are placed in the \a .stk_mpu_shared_data region.
    According to the recommended MPU configuration, this specialized region grants full read and
    write access (\a ACCESS_FULL) to both privileged and unprivileged user tasks, while explicitly
    blocking code execution (\a EXEC_NEVER).
*/
#define STK_MPU_SHARED_DATA_SECTION __attribute__((section(".stk_mpu_shared_data")))

/*! \def   STK_MPU_SHARED_CODE_SECTION
    \brief Attribute macro to place functions into the shared MPU executable code section.

    Any functions tagged with this macro are placed in the \a .stk_mpu_shared_code region.
    According to the recommended MPU configuration, this specialized region allows execution
    privileges (\a EXEC_ALLOWED) and read-only access for privileged and unprivileged user tasks
    (\a ACCESS_PRIV_RO_USER_RO) to facilitate secure system entry points.
*/
#define STK_MPU_SHARED_CODE_SECTION __attribute__((section(".stk_mpu_shared_code")))

/*! \def   STK_MPU_SHARED_BSS_SECTION
    \brief Attribute macro to place zero-initialized global or static variables into the shared
           MPU data memory section without consuming flash storage.

    Any variables tagged with this macro are placed in the \a .stk_mpu_shared_bss region, a
    zero-initialized subrange nested inside the same \a .stk_mpu_shared_data MPU region. As such,
    it carries the same access permissions as \ref STK_MPU_SHARED_DATA_SECTION - full read and
    write access (\a ACCESS_FULL) to both privileged and unprivileged user tasks, with code
    execution explicitly blocked (\a EXEC_NEVER).

    Unlike \ref STK_MPU_SHARED_DATA_SECTION, variables placed here must not have a non-zero
    initializer: the linker stores no image for this region, and startup code zero-fills it at
    boot instead of copying it from flash. Prefer this macro over \ref STK_MPU_SHARED_DATA_SECTION
    for large or scratch shared buffers to avoid wasting flash space on a stored image of zeros.
*/
#define STK_MPU_SHARED_BSS_SECTION __attribute__((section(".stk_mpu_shared_bss")))

#else

#define STK_MPU_SHARED_DATA_SECTION
#define STK_MPU_SHARED_CODE_SECTION
#define STK_MPU_SHARED_BSS_SECTION

// =============================================================================
#endif // STK_MPU
// =============================================================================

/*! \struct FaultContext
    \brief  ARMv7-M/ARMv8-M fault context.
*/
struct FaultContext
{
    struct Mpu
    {
        struct Region
        {
            Word RNR, RBAR, ATTR;
        };

        Word   CTRL, MAIR0, MAIR1;
        Region regions[STK_CORTEX_M_MPU_REGIONS_MAX];
    };

    hw::ExceptionFrame frame;
    Word               CFSR, HFSR, MMFAR, BFAR, AFSR;
    Word               CONTROL;
    Word               EXC_RETURN;
    Mpu                mpu;
    bool               mmfar_valid, bfar_valid;

    void Fill(const Word *stacked_regs, Word exc_return);
};

} // namespace stk

#endif /* STK_ARCH_ARM_CORTEX_M_H_ */
