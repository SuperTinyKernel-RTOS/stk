/*
 * SuperTinyKernel(TM) (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
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

#define _STK_SWITCH_TEST_TASKS_MAX 3
#define _STK_SWITCH_TEST_CYCLES_MAX 3

namespace stk {
namespace test {

/*! \namespace stk::test::switch_
    \brief     Namespace of Switch test.
 */
namespace switch_ {

static volatile uint8_t g_TaskSwitch = 0;
static volatile uint8_t g_Cycles[_STK_SWITCH_TEST_TASKS_MAX] = {};

/*! \class TestTask
    \brief Switch test task.
    \note  Counts __STK_SWITCH_TEST_CYCLES_MAX cycles with _STK_SWITCH_TEST_TASKS_MAX. Succeeds if counter incremented correctly.
*/
template <EAccessMode _AccessMode>
class TestTask : public Task<256, _AccessMode>
{
    uint8_t m_task_id;

public:
    TestTask(uint8_t task_id) : m_task_id(task_id) {}
    RunFuncType GetFunc() { return forced_cast<RunFuncType>(&TestTask::RunInner); }
    void *GetFuncUserData() { return this; }

private:
    void RunInner()
    {
        uint8_t task_id = m_task_id;
        volatile float count_skip = 0;

        while (true)
        {
            if (g_TaskSwitch != task_id)
            {
                ++count_skip;
                continue;
            }

            ++g_Cycles[task_id];
            printf("id=%d c=%d\n", task_id, g_Cycles[task_id]);

            // count total workload of all tasks
            uint32_t total = 0;
            for (int32_t i = 0; i < _STK_SWITCH_TEST_TASKS_MAX; ++i)
            {
                total += g_Cycles[i];
            }

            // check if it is a time to evaluate workload counters
            if (total >= (_STK_SWITCH_TEST_CYCLES_MAX * _STK_SWITCH_TEST_TASKS_MAX))
            {
                STK_TEST_CHECK_EQUAL((_STK_SWITCH_TEST_CYCLES_MAX * _STK_SWITCH_TEST_TASKS_MAX), total);

                // check if workload is spread equally between all tasks
                for (int32_t i = 0; i < _STK_SWITCH_TEST_TASKS_MAX; ++i)
                {
                    STK_TEST_CHECK_EQUAL(_STK_SWITCH_TEST_CYCLES_MAX, g_Cycles[i]);
                }

                // success, exit process
                TestContext::ForceExitTestSuite(TestContext::SUCCESS_EXIT_CODE);
                break;
            }

            stk::Delay(100);

            g_TaskSwitch = (task_id + 1) % 3;
        }
    }
};

//! Kernel.
static Kernel<KERNEL_STATIC, _STK_SWITCH_TEST_TASKS_MAX, SwitchStrategyRoundRobin, PlatformDefault> kernel;

//! Tasks (threads).
static TestTask<ACCESS_PRIVILEGED> task1(0), task2(1), task3(2);

} // namespace switch_
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
    using namespace stk::test::switch_;

    kernel.Initialize();

    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.AddTask(&task3);

    kernel.Start();

    return TestContext::DEFAULT_FAILURE_EXIT_CODE;
}
