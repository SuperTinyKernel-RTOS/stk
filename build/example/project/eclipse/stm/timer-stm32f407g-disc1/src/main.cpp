/*
 * SuperTinyKernel™ (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include "example.h"

/* QEMU
   Machine name: STM32F4-Discovery
   CPU name: cortex-m4
*/

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

    RunExample();

    // should not reach here
    return 1;
}
