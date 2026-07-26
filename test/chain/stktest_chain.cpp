/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <stk_config.h>
#include <stk.h>
#include <assert.h>

#include "stktest_context.h"

using namespace stk;
using namespace stk::test;

STK_TEST_DECL_ASSERT;

#define _STK_CHAIN_TEST_TASKS_MAX  3U
#define _STK_CHAIN_TEST_DELAY_TIME 100
#ifdef _STK_ARCH_RISC_V
#define _STK_CHAIN_TEST_DIFF       30 // QEMU RISC-V is very coarse in timing
#else
#define _STK_CHAIN_TEST_DIFF       10
#endif

namespace stk {
namespace test {

/*! \namespace stk::test::chain
    \brief     Namespace of Chain test.
 */
namespace chain {

static volatile uint8_t g_TaskSwitch = 0U;

/*! \class TestTask
    \brief Chain test task.
    \note  Counts __STK_CHAIN_TEST_CYCLES_MAX cycles with _STK_CHAIN_TEST_TASKS_MAX. Succeeds if counter incremented correctly.
*/
template <EAccessMode _AccessMode>
class TestTask : public Task<256, _AccessMode>
{
    uint8_t m_task_id;

public:
    TestTask(uint8_t task_id) : m_task_id(task_id)
    {}

private:
    void Run();
};

//! Kernel.
static Kernel<KERNEL_DYNAMIC, _STK_CHAIN_TEST_TASKS_MAX, SwitchStrategyRoundRobin, PlatformDefault> kernel;

//! Tasks (threads).
static TestTask<ACCESS_PRIVILEGED> task1(0), task2(1), task3(2);

//! Execution time of the task.
static Time g_Time[_STK_CHAIN_TEST_TASKS_MAX] = {};

template<> void TestTask<ACCESS_PRIVILEGED>::Run()
{
    uint8_t task_id = m_task_id;

    g_Time[task_id] = stk::GetTimeNowMs();

    printf("id=%d time=%d\n", task_id, (int)g_Time[task_id]);

    Delay(_STK_CHAIN_TEST_DELAY_TIME);

    // activate next task and exit
    g_TaskSwitch = (task_id + 1) % 3U;
    if (g_TaskSwitch == 1U)
        kernel.AddTask(&task2);
    else
    if (g_TaskSwitch == 2U)
        kernel.AddTask(&task3);
}

} // namespace chain
} // namespace test
} // namespace stk

/*! \fn    main
    \brief Entry to the test case.
*/
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    TestContext::ShowTestSuitePrologue();

    using namespace stk;
    using namespace stk::test;
    using namespace stk::test::chain;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.Start();

    int32_t result = TestContext::SUCCESS_EXIT_CODE;
    for (int32_t i = 0; i < _STK_CHAIN_TEST_TASKS_MAX; ++i)
    {
        Time diff = g_Time[i] - (i * _STK_CHAIN_TEST_DELAY_TIME);

        if (diff < 0)
            diff = -diff;

        // check if time difference for every task is not more than _STK_CHAIN_TEST_DIFF ms
        if (diff > _STK_CHAIN_TEST_DIFF)
        {
            printf("failed time: id=%d diff=%d (>%d)\n", (int)i, (int)diff, (int)_STK_CHAIN_TEST_DIFF);
            result = TestContext::DEFAULT_FAILURE_EXIT_CODE;
        }
    }

    TestContext::ShowTestSuiteEpilogue(result);
    return result;
}
