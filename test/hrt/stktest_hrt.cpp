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

#define _STK_HRT_TEST_TASKS_MAX   3
#define _STK_HRT_TEST_PERIODICITY 100
#define _STK_HRT_TEST_SLEEP       10
#define _STK_HRT_TEST_ITRS        3

namespace stk {
namespace test {

/*! \namespace stk::test::hrt
    \brief     Namespace of HRT test.
 */
namespace hrt {

/*! \class TimeInfo
    \brief Task pass time info.
*/
struct TimeInfo
{
    uint8_t id;
    int32_t start;
    int32_t diff;
};
static TimeInfo g_Time[_STK_HRT_TEST_TASKS_MAX][_STK_HRT_TEST_ITRS] = {};

/*! \class TestTask
    \brief HRT test task.
    \note  Tests hard-real time (HRT) capability of the Kernel.
*/
template <EAccessMode _AccessMode>
class TestTask : public Task<256, _AccessMode>
{
    uint8_t m_task_id;

public:
    TestTask(uint8_t task_id) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        for (int32_t i = 0; i < _STK_HRT_TEST_ITRS; ++i)
        {
            int64_t start = GetTimeNowMsec();

            // add varying workload (10, 20, 30 ms) with deadline max 50 ms
            Delay(_STK_HRT_TEST_SLEEP * (m_task_id + 1));

            int64_t diff = GetTimeNowMsec() - start;

            printf("id=%d start=%d diff=%d\n", m_task_id, (int)start, (int)diff);

            g_Time[m_task_id][i].id    = m_task_id;
            g_Time[m_task_id][i].start = start;
            g_Time[m_task_id][i].diff  = diff;

            Yield();
        }
    }
};

//! Kernel.
static Kernel<KERNEL_DYNAMIC | KERNEL_HRT, _STK_HRT_TEST_TASKS_MAX, SwitchStrategyRoundRobin, PlatformDefault> kernel;

//! Tasks (threads).
static TestTask<ACCESS_PRIVILEGED> task1(0), task2(1), task3(2);

} // namespace sleep
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
    using namespace stk::test::hrt;

    kernel.Initialize();

#define TICKS(MS) GetTicksFromMsec((MS), PERIODICITY_DEFAULT)

    kernel.AddTask(&task1, TICKS(_STK_HRT_TEST_PERIODICITY * _STK_HRT_TEST_TASKS_MAX), TICKS(_STK_HRT_TEST_PERIODICITY / 2), TICKS(_STK_HRT_TEST_PERIODICITY * 0));

    if (_STK_HRT_TEST_TASKS_MAX >= 2)
        kernel.AddTask(&task2, TICKS(_STK_HRT_TEST_PERIODICITY * _STK_HRT_TEST_TASKS_MAX), TICKS(_STK_HRT_TEST_PERIODICITY / 2), TICKS(_STK_HRT_TEST_PERIODICITY * 1));

    if (_STK_HRT_TEST_TASKS_MAX >= 3)
        kernel.AddTask(&task3, TICKS(_STK_HRT_TEST_PERIODICITY * _STK_HRT_TEST_TASKS_MAX), TICKS(_STK_HRT_TEST_PERIODICITY / 2), TICKS(_STK_HRT_TEST_PERIODICITY * 2));

    kernel.Start();

    int32_t result = TestContext::SUCCESS_EXIT_CODE;
    for (int32_t t = 0; t < _STK_HRT_TEST_TASKS_MAX; ++t)
    {
        for (int32_t i = 0; i < _STK_HRT_TEST_ITRS; ++i)
        {
            int32_t expect = i * (_STK_HRT_TEST_TASKS_MAX * _STK_HRT_TEST_PERIODICITY) + (t * _STK_HRT_TEST_PERIODICITY);

            int32_t diff_start = g_Time[t][i].start - expect;
            if (diff_start < 0)
                diff_start = -diff_start;

            int32_t diff_sleep = g_Time[t][i].diff - (_STK_HRT_TEST_SLEEP * (t + 1));
            if (diff_sleep < 0)
                diff_sleep = -diff_sleep;

            // check if time difference for every task is not more than 3 ms
            if ((diff_start > 10) || (diff_sleep > 10))
            {
                printf("failed time (%d): id=%d diff_start=%d diff_sleep=%d (>10)\n", (int)i,
                        g_Time[t][i].id, (int)diff_start, (int)diff_sleep);

                result = TestContext::DEFAULT_FAILURE_EXIT_CODE;
            }
        }
    }

    TestContext::ShowTestSuiteEpilogue(result);
    return result;
}
