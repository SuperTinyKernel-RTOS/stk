
/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_CONFIG_H_
#define STK_CONFIG_H_

#include <stdint.h>
#include <risc-v/encoding.h>
#include <pico.h>

// Use RISC-V cores of RP2350
#define _STK_ARCH_RISC_V

// RP2350 erratum RP2350-E6: Hazard3 has R and X bits of pmpcfg swapped ([R, W, X] instead of [X, W, R])
#define STK_RISCV_PMP_RX_SWAPPED (1)

// Enable MPU support
#define STK_MPU              (1)
#define STK_MPU_STACK_GUARD  (1)
#define STK_MPU_TASK_REGIONS (2)

// RP2350 supports max 8 MPU regions.
#define STK_RISCV_PMP_REGIONS_MAX (8)

// Let STK process MemManage and HardFault ISRs
#define STK_USE_MEMMANAGE_HANDLER (1)
#define STK_USE_HARDFAULT_HANDLER (1)

// Define _STK_CPU_COUNT as 2 to use STK on both CPU cores or on CPU1, if 1 then STK can be hosted on CPU0 only
#define STK_ARCH_CPU_COUNT    (2)
#define STK_ARCH_GET_CPU_ID() (*(uint32_t *)(SIO_BASE + SIO_CPUID_OFFSET)) // see get_core_num() in pico/platform.h

// RP2350 implements access to RISC-V CLINT via SIO_BASE which is per calling CPU core
#define STK_RISCV_CLINT_MTIME_ADDR    (SIO_BASE + SIO_MTIME_OFFSET) // MTIME + MTIMEH (see SIO_Type in RP2350.h)
#define STK_RISCV_CLINT_MTIMECMP_ADDR (SIO_BASE + SIO_MTIMECMP_OFFSET) // MTIMECMP + MTIMECMPH (see SIO_Type in RP2350.h)

// SIO_BASE of RP2350  is per calling CPU core, thus there is no classic access via CLINT + hart
#define STK_RISCV_CLINT_MTIMECMP_PER_HART (0)

// RP2350 has no CLINT MSIP: software interrupt is controlled via SIO RISCV_SOFTIRQ
// bits [1:0] = SET (core0/core1), bits [9:8] = CLR (core0/core1)
#define STK_RISCV_SOFTIRQ_ADDR (SIO_BASE + SIO_RISCV_SOFTIRQ_OFFSET)

// RP2350 ISR handlers, see crt0_riscv.S of pico-sdk
#define STK_SYSTICK_HANDLER isr_riscv_machine_timer
#define STK_SVC_HANDLER     isr_riscv_machine_exception
#define STK_MSI_HANDLER     isr_riscv_machine_soft_irq

// Keep STK's ISR handlers in RAM to be compatible with pico-sdk
#define STK_RISCV_ISR_SECTION __not_in_flash("stk")

#endif /* STK_CONFIG_H_ */
