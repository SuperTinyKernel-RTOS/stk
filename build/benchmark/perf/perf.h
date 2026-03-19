/*
 * SuperTinyKernel(TM) (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef BENCH_H_
#define BENCH_H_

#include "crc32.h"

#define _STK_BENCH_TASK_MAX   4
#define _STK_BENCH_STACK_SIZE 256
#define _STK_BENCH_WINDOW     1000

struct Crc32Bench
{
    Crc32Bench();

    void Initialize();
    __attribute__((noinline)) void Process();
    static void ShowResults();

    crc32_data_t m_crc;
    uint64_t     m_round;
};

extern Crc32Bench g_Bench[_STK_BENCH_TASK_MAX];

#endif /* BENCH_H_ */
