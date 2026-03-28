/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include "stdio.h"
#include "perf.h"

Crc32Bench g_Bench[_STK_BENCH_TASK_MAX];

Crc32Bench::Crc32Bench()
{
    Initialize();
}

void Crc32Bench::Initialize()
{
    crc32_init(&m_crc);
    m_round = 0;
}

void Crc32Bench::Process()
{
    ++m_round;

    union
    {
        struct
        {
            uint64_t round;
            uint32_t crc;
        };

        uint8_t block[12];
    } u;

    u.round = m_round;
    u.crc   = m_crc.currentCrc;

    crc32_update(&m_crc, u.block, sizeof(u.block));
}

void Crc32Bench::ShowResults()
{
    typedef unsigned int uint;

    uint64_t sum = 0, min = UINT64_MAX, max = 0;
    for (int32_t i = 0; i < _STK_BENCH_TASK_MAX; ++i)
    {
        uint64_t v = g_Bench[i].m_round;

        sum += v;

        if (v > max)
            max = v;
        if (v < min)
            min = v;
    }

    uint64_t average = sum / _STK_BENCH_TASK_MAX;

    uint32_t jitter_min = average - min;
    uint32_t jitter_max = max - average;
    uint32_t jitter = (jitter_max > jitter_min ? jitter_max : jitter_min);

    printf("tasks %d | sum=%u avr=%u min=%u max=%u jitter=%u\n", _STK_BENCH_TASK_MAX, (uint)sum, (uint)average, (uint)min, (uint)max, (uint)jitter);

    for (int32_t i = 0; i < _STK_BENCH_TASK_MAX; ++i)
    {
        printf("task %d = %u\n", (int)i, (uint)g_Bench[i].m_round);
    }
}

