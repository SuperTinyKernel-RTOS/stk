/*
 * SuperTinyKernel(TM) (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include "example.h"

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

    // activate RISC-V core of RP2350:
    // picotool reboot -c riscv

    RunExample();

    // should not reach here
    return 1;
}
