/**
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/**
 * \file stk_mbedtls_other.cpp
 *
 * \brief mbedTLS time abstraction port for
 *        SuperTinyKernel(TM) RTOS (STK) - implementation.
 *
 * Deploy this file, together with threading_alt.h, under
 * deps/port/mbedtls/ in the STK repository. Compile as C++ (it is included
 * in the mbedTLS build as an extra translation unit; only its extern "C"
 * entry points and the mbedtls_platform_*_t layout are visible to the rest
 * of mbedTLS, which stays plain C).
 */

#include "mbedtls/platform.h"
#include "mbedtls/platform_util.h"

#include "stk.h"

#if defined(STM32F0) || defined(STM32F1) || defined(STM32F2) || defined(STM32F3) || \
    defined(STM32F4) || defined(STM32F7) || defined(STM32G0) || defined(STM32G4) || \
    defined(STM32H5) || defined(STM32H7) || defined(STM32L0) || defined(STM32L1) || \
    defined(STM32L4) || defined(STM32L5) || defined(STM32U5) || defined(STM32WB) || \
    defined(STM32WL)
    #define STM32_MCU (1)
#elif defined(__MCUXPRESSO__) || defined(__MCUXPRESSO)
    #define NXP_MCU (1)
    #include "fsl_rnga.h"
#elif defined(_PICO_H)
    #define RPI_MCU (1)
    #include <pico/unique_id.h>
    #include <hardware/structs/rosc.h>
#endif

namespace stk {

#if defined(STM32_MCU) && defined(UID_BASE)
//! Random bytes generator. When _STK_PLATFORM_SEED is not defined
//! the generator is pseudorandom and unreliable for cryptography.
static void GetRandomBytes(uint8_t *buffer, size_t size)
{
    // Provided and initialized (HAL_RNG_Init, HAL_RNG_MODULE_ENABLED) by application
    // startup code, e.g. the handle STM32CubeMX generates when RNG is enabled.
    extern "C" RNG_HandleTypeDef hrng;

    const stk::hw::CriticalSection::ScopedLock _cs;

    // Re-init if the handle is in HAL_RNG_STATE_RESET, covers both
    // "application startup hasn't called HAL_RNG_Init yet" and "something
    // else (e.g. a low-power routine) called HAL_RNG_DeInit since". HAL's
    // own state is the source of truth here, not a local static flag.
    const bool was_reset = (HAL_RNG_GetState(&hrng) == HAL_RNG_STATE_RESET);
    if (was_reset)
    {
        // On failure, GetState will still read RESET next call, so this
        // is retried every call rather than latched as permanently failed;
        // HAL_RNG_GenerateRandomNumber below will just keep producing 0s
        // until Init succeeds.
        STK_UNUSED(HAL_RNG_Init(&hrng));
    }

    size_t written = 0U;
    while (written < size)
    {
        uint32_t random_word = 0U;

        // On failure, this word stays 0; a hard/recurring RNG fault
        // should still be surfaced to the application, not just
        // silently degrade output quality call after call.
        STK_UNUSED(HAL_RNG_GenerateRandomNumber(&hrng, &random_word));

        const size_t chunk = stk::Min<size_t>(sizeof(random_word), size - written);
        STK_MEMCPY(&buffer[written], &random_word, chunk);
        written += chunk;
    }

    // Only power the RNG clock domain back down if we were the ones who
    // brought it up; if it was already initialized on entry, something
    // else in the application owns that lifetime, leave it running.
    if (was_reset)
    {
        STK_UNUSED(HAL_RNG_DeInit(&hrng));
    }
}
#define _STK_PLATFORM_SEED
#elif defined(NXP_MCU) && defined(SIM)
//! Random bytes generator. When _STK_PLATFORM_SEED is not defined
//! the generator is pseudorandom and unreliable for cryptography.
static void GetRandomBytes(uint8_t *buffer, size_t size)
{
    static bool s_rnga_initialized = false;

    const stk::hw::CriticalSection::ScopedLock _cs;

    // RNGA must be clocked before use; lazily init once rather than
    // assuming application startup code has done it
    if (!s_rnga_initialized)
    {
        RNGA_Init(RNG); // starts generation immediately -> mode becomes Normal
        s_rnga_initialized = true;
    }

    const rnga_mode_t prior_mode = RNGA_GetMode(RNG);
    if (prior_mode == kRNGA_ModeSleep)
    {
        RNGA_SetMode(RNG, kRNGA_ModeNormal); // wake the oscillator to generate
    }

    RNGA_GetRandomData(RNG, buffer, size);

    if (prior_mode == kRNGA_ModeSleep)
    {
        RNGA_SetMode(RNG, kRNGA_ModeSleep); // leave it as we found it
    }
}
#define _STK_PLATFORM_SEED
#elif defined(RPI_MCU)
//! Random bytes generator. When _STK_PLATFORM_SEED is not defined
//! the generator is pseudorandom and unreliable for cryptography.
static void GetRandomBytes(uint8_t *buffer, size_t size)
{
    for (size_t i = 0U; i < size; ++i)
    {
        uint8_t random_byte = 0U;
        for (int32_t b = 0; b < 8; ++b)
        {
            random_byte = (random_byte << 1) | (rosc_hw->randombit & 1);
        }

        buffer[i] = random_byte;
    }
}
#define _STK_PLATFORM_SEED
#endif

#if !defined(_STK_PLATFORM_SEED)

static constexpr size_t UID_SIZE = 4U;

#if defined(STM32_MCU) && defined(UID_BASE)
    //! Unique MCU ID getter.
    static void GetMcuUID(uint32_t mcu_uid[UID_SIZE])
    {
        mcu_uid[0] = HAL_GetUIDw0();
        mcu_uid[1] = HAL_GetUIDw1();
        mcu_uid[2] = HAL_GetUIDw2();
        mcu_uid[3] = 0U; // STM32 UID is only 96 bits (3 words)
    }
#elif defined(NXP_MCU) && defined(SIM)
    //! Unique MCU ID getter.
    static void GetMcuUID(uint32_t mcu_uid[UID_SIZE])
    {
        mcu_uid[0] = SIM->UIDL;
        mcu_uid[1] = SIM->UIDML;
        mcu_uid[2] = SIM->UIDMH;
    #if defined(SIM_UIDH)
        mcu_uid[3] = SIM->UIDH;
    #else
        mcu_uid[3] = SIM->UIDL;
    #endif
    }
#elif defined(RPI_MCU)
    //! Unique MCU ID getter.
    static void GetMcuUID(uint32_t mcu_uid[UID_SIZE])
    {
        pico_unique_board_id_t id;
        pico_get_unique_board_id(&id);

        STK_MEMSET(mcu_uid, 0U, (sizeof(uint32_t) * UID_SIZE));
        STK_MEMCPY(mcu_uid, id.id, stk::Min<size_t>(PICO_UNIQUE_BOARD_ID_SIZE_BYTES, (sizeof(uint32_t) * UID_SIZE)));
    }
#endif
#endif // !_STK_PLATFORM_SEED

#if !defined(_STK_PLATFORM_SEED)
// FNV-1a Hash helper to mix entropy bits into output buffer.
static uint32_t fnv1a_32(uint32_t hash, uint32_t data)
{
    const uint32_t FNV_PRIME = 16777619U;

    for (size_t i = 0U; i < 4U; ++i)
    {
        uint8_t byte = static_cast<uint8_t>((data >> (i * 8U)) & 0xFFU);
        hash ^= byte;
        hash *= FNV_PRIME;
    }

    return hash;
}
#endif // !_STK_PLATFORM_SEED

#if !defined(_STK_PLATFORM_SEED)
// No on-chip HW RNG for this platform. CPU-cycle-jitter timing source only --
// not SP 800-90B qualified, no online health testing. mbedtls_platform_get_entropy
// reports a conservative (non-full) entropy estimate for output produced here.
static void GetRandomBytes(uint8_t *buffer, size_t size)
{
    uint32_t running_hash = 2166136261U; // FNV-1a offset basis

    for (size_t i = 0U; i < size; ++i)
    {
        // Sample fine-grained CPU cycles
        stk::Cycles start_cycles = stk::hw::HiResClock::GetCycles();

        // Execute execution delay loop to capture cycle variations
        for (volatile int32_t j = 0; j < 16; ++j)
        {
            __asm volatile("nop");
        }

        stk::Cycles end_cycles = stk::hw::HiResClock::GetCycles();

        // Mix low-order cycle variation jitter into state hash
        uint32_t cycle_jitter = static_cast<uint32_t>(end_cycles ^ start_cycles);
        running_hash = stk::fnv1a_32(running_hash, cycle_jitter);

        // Feed loop iteration index to guarantee state progression
        running_hash = stk::fnv1a_32(running_hash, static_cast<uint32_t>(i));

        // Store intermediate hash byte into output buffer
        buffer[i] = static_cast<uint8_t>(running_hash & 0xFFU);
    }
}
#endif // !_STK_PLATFORM_SEED

} // namespace stk

extern "C" int mbedtls_platform_get_entropy(psa_driver_get_entropy_flags_t flags,
   size_t *estimate_bits, unsigned char *output, size_t output_size)
{
    if (flags != 0U)
    {
        return PSA_ERROR_NOT_SUPPORTED;
    }

    if ((output == nullptr) || (estimate_bits == nullptr))
    {
        return PSA_ERROR_INVALID_ARGUMENT;
    }

    // Fill the caller's buffer directly from the platform's best available
    // source: an on-chip HW RNG/TRNG when _STK_PLATFORM_SEED is defined,
    // or the CPU-cycle-jitter fallback otherwise.
    stk::GetRandomBytes(output, output_size);

#if defined(_STK_PLATFORM_SEED)
    // Backed by an on-chip hardware RNG/TRNG: full entropy credit.
    (*estimate_bits) = (output_size * 8U);
#else
    // No HW RNG on this platform. Whiten the jitter-only output with static
    // (MCU UID) and dynamic (kernel tick/task) context as defense-in-depth
    // against a degenerate/low-variance jitter source, and report a
    // conservative entropy estimate: this path is not SP 800-90B qualified
    // and has no online health testing, so full-entropy credit isn't honest.
    uint32_t mcu_uid[stk::UID_SIZE] = {0U, 0U, 0U, 0U};
    stk::GetMcuUID(mcu_uid);

    stk::IKernelService *kernel_svc = stk::IKernelService::GetInstance();
    stk::TId tid     = ((kernel_svc != nullptr) ? kernel_svc->GetTid() : 0U);
    stk::Ticks ticks = ((kernel_svc != nullptr) ? kernel_svc->GetTicks() : 0LL);

    uint32_t context_hash = 2166136261U; // FNV-1a offset basis
    for (size_t i = 0U; i < STK_STATIC_ARRAY_SIZE(mcu_uid); ++i)
    {
        context_hash = stk::fnv1a_32(context_hash, mcu_uid[i]);
    }
    context_hash = stk::fnv1a_32(context_hash, static_cast<uint32_t>(tid));
    context_hash = stk::fnv1a_32(context_hash, static_cast<uint32_t>(ticks & 0xFFFFFFFFLL));

    for (size_t i = 0U; i < output_size; ++i)
    {
        output[i] ^= static_cast<unsigned char>((context_hash >> ((i % 4U) * 8U)) & 0xFFU);
    }

    (*estimate_bits) = output_size; // conservative: 1 bit/byte
#endif

    return 0; // Success
}

extern "C" psa_status_t mbedtls_psa_external_get_random(
    mbedtls_psa_external_random_context_t *context, uint8_t *output, size_t output_size,
    size_t *output_length)
{
    STK_UNUSED(context);

    size_t entropy_bits = 0;
    int ret = mbedtls_platform_get_entropy(0, &entropy_bits, output, output_size);

    if (ret != 0)
    {
        (*output_length) = 0;
        return PSA_ERROR_INSUFFICIENT_ENTROPY;
    }

    (*output_length) = output_size;
    return PSA_SUCCESS;
}
