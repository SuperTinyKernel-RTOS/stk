/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

/*! \file  stktest_hrt.cpp
    \brief Test suite for the Hard Real-Time (HRT) mode of the kernel (stk::KERNEL_HRT).

    Every test registers 1..4 HRT tasks (periodicity / deadline / start delay) whose Run() performs
    a configurable amount of work with Delay() and then calls Yield() to signal job completion.
    Start times and work durations of every job are recorded and verified after Kernel::Start()
    returns (KERNEL_DYNAMIC: Start() returns when all tasks have exited).

    Scenarios covered by the suite:
     - Basic:             3 tasks, staggered start delays, varying workload (the original test);
     - SingleTask:        one task alone in the system (sleep trap is entered every period);
     - ZeroWork:          a job that completes immediately (Yield() without any work);
     - Simultaneous:      several tasks activated at the same moment (deadline covers waiting time);
     - TightDeadline:     deadline only a few ticks above the workload;
     - LongStartDelay:    start delay longer than the task's own period;
     - DifferentPeriods:  tasks with different periodicities and workloads running side by side;
     - NoDrift:           many periods of a single task: no accumulated drift (period is exact);
     - StaggeredExit:     tasks exit after different number of jobs, the rest keep their schedule;
     - ExitWithoutYield:  the last job ends by returning from Run() instead of Yield();
     - ImmediateExit:     a task returns from Run() without running a single job;
     - FullPool:          all kernel task slots are used;
     - DeadlineMiss:      an overrun is detected (strict "duration > deadline" rule) and reported
                          through ITask::OnDeadlineMissed() + IEventOverrider::OnHardFault();
     - MissIsolated:      a missing task does not disturb the schedule of a healthy one.

    TimerHost in HRT mode (polled mode: no kernel tasks, an HRT job calls TimerHost::Process(), the queues of
    TimerHost need KERNEL_SYNC, therefore the kernel of this suite has it):
     - TimerBasic:        one-shot and periodic timers expire inline from the job, late by up to one job period;
     - TimerCrossTask:    commands (Start/Stop/Restart) issued from another HRT task take effect at the next job;
     - TimerBudget:       callbacks of several timers expiring in one job fit the job deadline (no deadline miss);
     - TimerOverrun:      a callback which exceeds the job deadline is reported as a deadline miss and the
                          schedule of the following jobs is not disturbed.

    Timing notes (tick = 1 ms, the default): Delay() is wall-clock based, therefore the work of tasks
    which are active at the same time overlaps instead of being serialized. A job lasts at least
    its workload (minus one tick of measurement jitter) and typically a few ticks more.

    Deadline-miss scenarios need the hard fault to be survivable: the test installs an
    IPlatform::IEventOverrider whose OnHardFault() counts the fault and reports it as handled,
    so the platform driver does not halt the system. In all other scenarios the fault counter
    must stay zero.
*/

#include <stk_config.h>
#include <stk.h>
#ifndef __ARM_ARCH_6M__
#include <time/stk_time_timer.h>
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "stktest_context.h"

using namespace stk;
using namespace stk::test;

STK_TEST_DECL_ASSERT;

#define _STK_HRT_TEST_TASKS_MAX      4
#define _STK_HRT_TEST_ITRS_MAX       20   // max jobs recorded per task

// Checked timeouts.
#define _STK_HRT_TEST_START_TOL_MS   10   // allowed deviation of a job start from its expected time (ms)
#define _STK_HRT_TEST_WORK_LO_MS     1    // allowed "early" slack of a measured workload (ms)
#define _STK_HRT_TEST_WORK_HI_MS     10   // allowed "late" slack of a measured workload (ms)
#define _STK_HRT_TEST_DEADLINE_SLACK_MS 0 // extra slack of the measured job duration vs deadline (ms)
#define _STK_HRT_TEST_EARLY_TOL_MS   2    // allowed "early" firing of a timer (ms)
#define _STK_HRT_TEST_IDLE_JOB_MS    4    // max duration of a job which runs no timer callback (ms)

#if defined(_STK_ARCH_RISC_V)
#define _STK_HRT_STACK_SIZE          384
#else
#define _STK_HRT_STACK_SIZE          256
#endif

namespace stk {
namespace test {

/*! \namespace stk::test::hrt
    \brief     Namespace of HRT test.
 */
namespace hrt {

/*! \struct TaskCfg
    \brief  Per-task configuration of a scenario (all times in milliseconds).
*/
struct TaskCfg
{
    int32_t period;       //!< HRT periodicity.
    int32_t deadline;     //!< HRT deadline.
    int32_t delay;        //!< Start delay.
    int32_t work;         //!< Workload of one job (busy delay).
    int32_t iterations;   //!< Number of jobs before the task exits.
    bool    yield_last;   //!< Yield() after the last job (false: return from Run() right away).
    bool    expect_miss;  //!< The workload exceeds the deadline: a deadline miss is expected.
};

// Test state (every array element has a single writer)
static TaskCfg          g_Cfg[_STK_HRT_TEST_TASKS_MAX] = {};
static volatile int32_t g_Start[_STK_HRT_TEST_TASKS_MAX][_STK_HRT_TEST_ITRS_MAX] = {};
static volatile int32_t g_Diff[_STK_HRT_TEST_TASKS_MAX][_STK_HRT_TEST_ITRS_MAX] = {};
static volatile int32_t g_Count[_STK_HRT_TEST_TASKS_MAX] = {};      // completed jobs
static volatile int32_t g_MissCount[_STK_HRT_TEST_TASKS_MAX] = {};  // OnDeadlineMissed() calls
static volatile int32_t g_MissFirst[_STK_HRT_TEST_TASKS_MAX] = {};  // duration reported by the first call

// Kernel (KERNEL_SYNC: required by the queues of TimerHost used in polled mode, see the TimerHost tests)
static Kernel<KERNEL_DYNAMIC | KERNEL_HRT | KERNEL_SYNC, _STK_HRT_TEST_TASKS_MAX, SwitchStrategyRoundRobin, PlatformDefault> g_Kernel;

/*! \class HrtOverrider
    \brief Makes a hard fault (HRT deadline miss) survivable and countable.
*/
class HrtOverrider : public IPlatform::IEventOverrider
{
public:
    HrtOverrider() : m_faults(0)
    {}

    bool OnHardFault() override
    {
        ++m_faults;
        return true; // handled: do not halt the system
    }

    volatile int32_t m_faults;
};

static HrtOverrider g_Overrider;

static Timeout MsToTicks(int32_t ms)
{
    return static_cast<Timeout>(GetTicksFromMs(ms, static_cast<uint32_t>(PERIODICITY_DEFAULT)));
}

static int32_t Abs(int32_t v)
{
    return ((v < 0) ? -v : v);
}

static bool InRange(int32_t v, int32_t lo, int32_t hi)
{
    return ((v >= lo) && (v <= hi));
}

/*! \class HrtTask
    \brief HRT test task: runs cfg.iterations jobs, each is a busy delay of cfg.work ms followed by Yield().
    \note  Sleep() is not supported in HRT mode, the workload is simulated with Delay() (busy-wait),
           and the task is put to sleep until its next period by the kernel on Yield().
*/
template <EAccessMode _AccessMode>
class HrtTask : public Task<_STK_HRT_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    HrtTask(uint8_t task_id) : m_task_id(task_id)
    {}

    // called from the tick handler: record only
    void OnDeadlineMissed(uint32_t duration) override
    {
        if (g_MissCount[m_task_id] == 0)
            g_MissFirst[m_task_id] = static_cast<int32_t>(duration);

        g_MissCount[m_task_id] = g_MissCount[m_task_id] + 1;
    }

private:
    void Run()
    {
        const TaskCfg &cfg = g_Cfg[m_task_id];

        for (int32_t i = 0; i < cfg.iterations; ++i)
        {
            const int64_t start = GetTimeNowMs();

            if (cfg.work > 0)
                stk::Delay(MsToTicks(cfg.work));

            const int64_t diff = GetTimeNowMs() - start;

            g_Start[m_task_id][i] = static_cast<int32_t>(start);
            g_Diff[m_task_id][i]  = static_cast<int32_t>(diff);
            g_Count[m_task_id]    = g_Count[m_task_id] + 1;

            // signal job completion, the kernel sleeps the task until its next period
            if (((i + 1) < cfg.iterations) || cfg.yield_last)
                stk::Yield();
        }
    }
};

/*! \enum  EScenario
    \brief Test scenarios, see the file note.
*/
enum EScenario : int32_t
{
    SC_BASIC = 0,
    SC_SINGLE_TASK,
    SC_ZERO_WORK,
    SC_SIMULTANEOUS,
    SC_TIGHT_DEADLINE,
    SC_LONG_START_DELAY,
    SC_DIFFERENT_PERIODS,
    SC_NO_DRIFT,
    SC_STAGGERED_EXIT,
    SC_EXIT_WITHOUT_YIELD,
    SC_IMMEDIATE_EXIT,
    SC_FULL_POOL,
    SC_DEADLINE_MISS,
    SC_MISS_ISOLATED
};

static void Cfg(int32_t id, int32_t period, int32_t deadline, int32_t delay, int32_t work, int32_t iterations,
    bool yield_last = true, bool expect_miss = false)
{
    STK_ASSERT(iterations <= _STK_HRT_TEST_ITRS_MAX);

    TaskCfg &c = g_Cfg[id];

    c.period      = period;
    c.deadline    = deadline;
    c.delay       = delay;
    c.work        = work;
    c.iterations  = iterations;
    c.yield_last  = yield_last;
    c.expect_miss = expect_miss;
}

//! Fill g_Cfg for a scenario, returns the number of tasks.
static int32_t SetupScenario(int32_t scenario)
{
    int32_t tasks = 0;

    switch (scenario)
    {
    case SC_BASIC:
        // tasks activated one after another every 100 ms, varying workload 10/20/30 ms, deadline 50 ms
        Cfg(0, 300, 50, 0,   10, 3);
        Cfg(1, 300, 50, 100, 20, 3);
        Cfg(2, 300, 50, 200, 30, 3);
        tasks = 3;
        break;
    case SC_SINGLE_TASK:
        Cfg(0, 50, 25, 0, 5, 5);
        tasks = 1;
        break;
    case SC_ZERO_WORK:
        // job completes at once: the whole period is spent sleeping
        Cfg(0, 20, 10, 0, 0, 6);
        tasks = 1;
        break;
    case SC_SIMULTANEOUS:
        // all tasks become active at the same time (the deadline also covers the time spent waiting for the CPU)
        Cfg(0, 60, 40, 0, 10, 3);
        Cfg(1, 60, 40, 0, 15, 3);
        Cfg(2, 60, 40, 0, 20, 3);
        tasks = 3;
        break;
    case SC_TIGHT_DEADLINE:
        // deadline is only 8 ms above the workload
        Cfg(0, 100, 28, 0,  20, 4);
        Cfg(1, 100, 28, 50, 20, 4);
        tasks = 2;
        break;
    case SC_LONG_START_DELAY:
        // task 1 first starts after 200 ms: longer than its own period (80 ms)
        Cfg(0, 50, 20, 0,   5,  8);
        Cfg(1, 80, 30, 200, 10, 2);
        tasks = 2;
        break;
    case SC_DIFFERENT_PERIODS:
        Cfg(0, 40,  30, 0,  5,  6);
        Cfg(1, 60,  30, 10, 8,  4);
        Cfg(2, 100, 30, 20, 12, 3);
        tasks = 3;
        break;
    case SC_NO_DRIFT:
        // 20 periods: every start must stay on the (start + i * period) grid
        Cfg(0, 20, 10, 0, 3, 20);
        tasks = 1;
        break;
    case SC_STAGGERED_EXIT:
        // tasks exit after 1, 3 and 6 jobs, the remaining ones must keep their schedule
        Cfg(0, 30, 20, 0,  5, 1);
        Cfg(1, 30, 20, 10, 5, 3);
        Cfg(2, 30, 20, 20, 5, 6);
        tasks = 3;
        break;
    case SC_EXIT_WITHOUT_YIELD:
        // the last job ends by returning from Run() (no final Yield())
        Cfg(0, 40, 20, 0,  5, 3, false);
        Cfg(1, 40, 20, 20, 5, 3, false);
        tasks = 2;
        break;
    case SC_IMMEDIATE_EXIT:
        // task 0 returns from Run() without a single job
        Cfg(0, 40, 20, 0, 5, 0);
        Cfg(1, 40, 20, 0, 5, 3);
        tasks = 2;
        break;
    case SC_FULL_POOL:
        // all kernel slots are used, activations are spread across the common period
        Cfg(0, 200, 40, 0,   10, 3);
        Cfg(1, 200, 40, 50,  20, 3);
        Cfg(2, 200, 40, 100, 30, 3);
        Cfg(3, 200, 40, 150, 10, 3);
        tasks = 4;
        break;
    case SC_DEADLINE_MISS:
        // workload 40 ms vs deadline 20 ms: the first overrun is detected at duration == deadline + 1
        Cfg(0, 100, 20, 0, 40, 2, true, true);
        tasks = 1;
        break;
    case SC_MISS_ISOLATED:
        // task 0 overruns, task 1 must not be affected
        Cfg(0, 100, 20, 0,  40, 3, true, true);
        Cfg(1, 100, 30, 50, 10, 3);
        tasks = 2;
        break;
    default:
        break;
    }

    return tasks;
}

// Helper function to reset test state
static void ResetTestState()
{
    memset(g_Cfg, 0, sizeof(g_Cfg));

    for (int32_t t = 0; t < _STK_HRT_TEST_TASKS_MAX; ++t)
    {
        for (int32_t i = 0; i < _STK_HRT_TEST_ITRS_MAX; ++i)
        {
            g_Start[t][i] = 0;
            g_Diff[t][i]  = 0;
        }

        g_Count[t]     = 0;
        g_MissCount[t] = 0;
        g_MissFirst[t] = 0;
    }

    g_Overrider.m_faults = 0;
}

/*! \brief     Verify the recorded schedule of a scenario.
    \note      The kernel tick counter keeps running between tests, so expected times are relative to
               a base derived from the first job of the first task which ran at least one job:
               base = start[0] - delay. Expected start of job i: base + delay + i * period.
*/
static bool VerifyScenario(int32_t tasks)
{
    bool ok = true;
    bool any_miss = false;

    int32_t ref = -1;
    for (int32_t t = 0; t < tasks; ++t)
    {
        if ((g_Cfg[t].iterations > 0) && (g_Count[t] > 0))
        {
            ref = t;
            break;
        }
    }

    if (ref < 0)
    {
        printf("hrt: no job was executed\n");
        return false;
    }

    const int32_t base = g_Start[ref][0] - g_Cfg[ref].delay;

    for (int32_t t = 0; t < tasks; ++t)
    {
        const TaskCfg &c = g_Cfg[t];

        if (g_Count[t] != c.iterations)
        {
            printf("hrt: id=%d jobs done=%d (expected %d)\n", (int)t, (int)g_Count[t], (int)c.iterations);
            ok = false;
        }

        for (int32_t i = 0; i < g_Count[t]; ++i)
        {
            const int32_t expect     = base + c.delay + (i * c.period);
            const int32_t start      = g_Start[t][i];
            const int32_t diff       = g_Diff[t][i];
            const int32_t diff_start = Abs(start - expect);

            printf("id=%d itr=%d start=%d (expect %d) diff=%d (work %d)\n",
                (int)t, (int)i, (int)(start - base), (int)(expect - base), (int)diff, (int)c.work);

            if (diff_start > _STK_HRT_TEST_START_TOL_MS)
            {
                printf("failed start (%d): id=%d diff_start=%d (>%d)\n", (int)i, (int)t,
                    (int)diff_start, (int)_STK_HRT_TEST_START_TOL_MS);
                ok = false;
            }

            if (!InRange(diff, c.work - _STK_HRT_TEST_WORK_LO_MS, c.work + _STK_HRT_TEST_WORK_HI_MS))
            {
                printf("failed work (%d): id=%d diff=%d work=%d\n", (int)i, (int)t, (int)diff, (int)c.work);
                ok = false;
            }

            // a healthy job completes within its deadline
            if (!c.expect_miss && (diff > (c.deadline + _STK_HRT_TEST_DEADLINE_SLACK_MS)))
            {
                printf("failed deadline (%d): id=%d diff=%d deadline=%d\n", (int)i, (int)t, (int)diff,
                    (int)c.deadline);
                ok = false;
            }
        }

        if (c.expect_miss)
        {
            any_miss = true;

            // detection rule is strict (duration > deadline): the first report is deadline + 1
            printf("hrt: id=%d misses=%d first_duration=%d (expected %d)\n", (int)t, (int)g_MissCount[t],
                (int)g_MissFirst[t], (int)(c.deadline + 1));

            if ((g_MissCount[t] == 0) || (g_MissFirst[t] != (c.deadline + 1)))
                ok = false;
        }
        else if (g_MissCount[t] != 0)
        {
            printf("hrt: id=%d unexpected deadline miss (%d), first duration=%d\n", (int)t,
                (int)g_MissCount[t], (int)g_MissFirst[t]);
            ok = false;
        }
        else
        {
            // no miss expected and none reported
        }
    }

    printf("hrt: hard faults=%d (%s)\n", (int)g_Overrider.m_faults, any_miss ? "expected" : "none expected");

    if (any_miss ? (g_Overrider.m_faults == 0) : (g_Overrider.m_faults != 0))
        ok = false;

    return ok;
}

#ifndef __ARM_ARCH_6M__

// TimerHost tests (polled mode driven by an HRT job)

#define _STK_HRT_TH_JOBS_MAX    24  // max driver jobs recorded
#define _STK_HRT_TH_FIRES_MAX   8   // max expirations recorded per timer

/*! \enum  EThScenario
    \brief TimerHost scenarios, see ThSetup().
*/
enum EThScenario : int32_t
{
    TH_BASIC = 0,
    TH_CROSS_TASK,
    TH_CALLBACK_BUDGET,
    TH_CALLBACK_OVERRUN
};

/*! \struct ThCfg
    \brief  Configuration of a TimerHost scenario (all times in milliseconds).
    \note   The driver is the HRT job which calls TimerHost::Process(), its periodicity is the timer resolution.
            The controller (ctrl_jobs > 0) is a second HRT task which issues timer commands from its own job.
*/
struct ThCfg
{
    int32_t period;        //!< Driver periodicity.
    int32_t deadline;      //!< Driver deadline (covers Process() together with all callbacks it runs).
    int32_t jobs;          //!< Number of driver jobs before it exits.
    int32_t work0;         //!< Workload of the callback of timer 0 (busy delay).
    int32_t work1;         //!< Workload of the callback of timer 1 (busy delay).
    int32_t ctrl_period;   //!< Controller periodicity.
    int32_t ctrl_deadline; //!< Controller deadline.
    int32_t ctrl_delay;    //!< Controller start delay.
    int32_t ctrl_jobs;     //!< Number of controller jobs (0: no controller task).
    bool    expect_miss;   //!< A callback exceeds the driver deadline: a deadline miss is expected.
};

static time::TimerHost g_TimerHost;

static ThCfg               g_ThCfg      = {};
static volatile int32_t    g_ThScenario = 0;
static volatile bool       g_ThInJob    = false; // true while the driver is inside TimerHost::Process()
static volatile int32_t    g_ThJobs     = 0;     // completed driver jobs
static volatile int32_t    g_ThJobStart[_STK_HRT_TH_JOBS_MAX] = {};
static volatile int32_t    g_ThJobDur[_STK_HRT_TH_JOBS_MAX]   = {};
static volatile int32_t    g_ThFireCount[2] = {};                              // expirations per timer
static volatile int32_t    g_ThInline[2]    = {};                              // expirations which ran inside Process()
static volatile int32_t    g_ThFireTime[2][_STK_HRT_TH_FIRES_MAX] = {};        // callback start time
static volatile int32_t    g_ThIssued[2][2] = {};                              // [timer][0: Start(), 1: Restart()] call time

/*! \class ThTimer
    \brief Timer which records every expiration (time, inline or not) and then does \a work ms of busy work.
*/
class ThTimer : public time::TimerHost::Timer
{
    uint8_t m_id;

public:
    ThTimer(uint8_t timer_id) : m_id(timer_id)
    {}

    void OnExpired(time::TimerHost *) override
    {
        const int32_t n = g_ThFireCount[m_id];

        if (n < _STK_HRT_TH_FIRES_MAX)
            g_ThFireTime[m_id][n] = static_cast<int32_t>(GetTimeNowMs());

        g_ThFireCount[m_id] = n + 1;

        if (g_ThInJob)
            g_ThInline[m_id] = g_ThInline[m_id] + 1;

        const int32_t work = (m_id == 0 ? g_ThCfg.work0 : g_ThCfg.work1);

        if (work > 0)
            stk::Delay(MsToTicks(work));
    }
};

static ThTimer g_ThTimer0(0);
static ThTimer g_ThTimer1(1);

static void ThStart(int32_t id, uint32_t delay, uint32_t period = 0)
{
    g_ThIssued[id][0] = static_cast<int32_t>(GetTimeNowMs());
    g_TimerHost.Start((id == 0 ? g_ThTimer0 : g_ThTimer1), delay, period);
}

/*! \class ThDriverTask
    \brief HRT job which drives the polled TimerHost: every job calls Process() (expired callbacks run inline)
           and yields. Job 0 starts the timers of the scenario, the last job releases all timers.
    \note  The job duration (commands + deadline scan + callbacks) must stay within the job deadline.
*/
template <EAccessMode _AccessMode>
class ThDriverTask : public Task<_STK_HRT_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    ThDriverTask(uint8_t task_id) : m_task_id(task_id)
    {}

    // called from the tick handler: record only
    void OnDeadlineMissed(uint32_t duration) override
    {
        if (g_MissCount[m_task_id] == 0)
            g_MissFirst[m_task_id] = static_cast<int32_t>(duration);

        g_MissCount[m_task_id] = g_MissCount[m_task_id] + 1;
    }

private:
    void Run()
    {
        const ThCfg &c = g_ThCfg;

        for (int32_t j = 0; j < c.jobs; ++j)
        {
            const int64_t start = GetTimeNowMs();

            if (j == 0)
            {
                switch (g_ThScenario)
                {
                case TH_BASIC:
                    ThStart(0, 35);     // one-shot
                    ThStart(1, 25, 50); // periodic
                    break;
                case TH_CALLBACK_BUDGET:
                    ThStart(0, 45, 40); // both timers expire in the same job
                    ThStart(1, 45, 40);
                    break;
                case TH_CALLBACK_OVERRUN:
                    ThStart(0, 25);     // its callback runs far longer than the job deadline
                    break;
                default:
                    break; // TH_CROSS_TASK: the commands come from the controller task
                }
            }

            g_ThInJob = true;
            g_TimerHost.Process();
            g_ThInJob = false;

            const int32_t dur = static_cast<int32_t>(GetTimeNowMs() - start);

            g_ThJobStart[j] = static_cast<int32_t>(start);
            g_ThJobDur[j]   = dur;
            g_ThJobs        = j + 1;

            if ((j + 1) < c.jobs)
                stk::Yield();
        }

        // release all timers, the host is shared by the following tests
        g_TimerHost.Shutdown();
        g_TimerHost.Process();
    }
};

/*! \class ThControllerTask
    \brief HRT task which issues timer commands from its own job (cross-task commands): job 0 starts a 40 ms
           one-shot (timer 0) and a 20 ms periodic timer (timer 1), job 1 stops timer 1, job 2 restarts timer 0
           as a 33 ms one-shot.
*/
template <EAccessMode _AccessMode>
class ThControllerTask : public Task<_STK_HRT_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    ThControllerTask(uint8_t task_id) : m_task_id(task_id)
    {}

    // called from the tick handler: record only
    void OnDeadlineMissed(uint32_t duration) override
    {
        if (g_MissCount[m_task_id] == 0)
            g_MissFirst[m_task_id] = static_cast<int32_t>(duration);

        g_MissCount[m_task_id] = g_MissCount[m_task_id] + 1;
    }

private:
    void Run()
    {
        const ThCfg &c = g_ThCfg;

        for (int32_t j = 0; j < c.ctrl_jobs; ++j)
        {
            switch (j)
            {
            case 0:
                ThStart(0, 40);
                ThStart(1, 20, 20);
                break;
            case 1:
                g_TimerHost.Stop(g_ThTimer1);
                break;
            case 2:
                g_ThIssued[0][1] = static_cast<int32_t>(GetTimeNowMs());
                g_TimerHost.Restart(g_ThTimer0, 33);
                break;
            default:
                break;
            }

            if ((j + 1) < c.ctrl_jobs)
                stk::Yield();
        }
    }
};

//! Fill g_ThCfg for a TimerHost scenario.
static void ThSetup(int32_t scenario)
{
    memset(&g_ThCfg, 0, sizeof(g_ThCfg));

    ThCfg &c = g_ThCfg;

    switch (scenario)
    {
    case TH_BASIC:
        // job every 10 ms: a one-shot (35 ms) and a periodic timer (25 ms, then every 50 ms)
        c.period = 10; c.deadline = 8; c.jobs = 20;
        break;
    case TH_CROSS_TASK:
        // the controller (every 50 ms, first job at 5 ms) starts, stops and restarts timers, the driver runs every 10 ms
        c.period = 10; c.deadline = 8; c.jobs = 20;
        c.ctrl_period = 50; c.ctrl_deadline = 20; c.ctrl_delay = 5; c.ctrl_jobs = 4;
        break;
    case TH_CALLBACK_BUDGET:
        // two callbacks of 5 ms each expire in the same job (~10 ms of work), the job deadline is 18 ms
        c.period = 20; c.deadline = 18; c.jobs = 10; c.work0 = 5; c.work1 = 5;
        break;
    case TH_CALLBACK_OVERRUN:
        // a 30 ms callback vs 12 ms job deadline: it still ends before the next job (period 40 ms)
        c.period = 40; c.deadline = 12; c.jobs = 6; c.work0 = 30; c.expect_miss = true;
        break;
    default:
        break;
    }

    g_ThScenario = scenario;
}

static void ResetThState()
{
    g_ThInJob = false;
    g_ThJobs  = 0;

    for (int32_t j = 0; j < _STK_HRT_TH_JOBS_MAX; ++j)
    {
        g_ThJobStart[j] = 0;
        g_ThJobDur[j]   = 0;
    }

    for (int32_t id = 0; id < 2; ++id)
    {
        g_ThFireCount[id] = 0;
        g_ThInline[id]    = 0;
        g_ThIssued[id][0] = 0;
        g_ThIssued[id][1] = 0;

        for (int32_t k = 0; k < _STK_HRT_TH_FIRES_MAX; ++k)
            g_ThFireTime[id][k] = 0;
    }
}

static bool CheckCount(int32_t id, int32_t expected)
{
    printf("th: timer %d expirations=%d (expected %d)\n", (int)id, (int)g_ThFireCount[id], (int)expected);

    return (g_ThFireCount[id] == expected);
}

/*! \brief Check the k-th expiration of a timer: not earlier than its deadline (2 ms jitter) and late by at most
           \a hi ms, i.e. one job period plus tolerance (the resolution of the polled mode is the job period).
*/
static bool CheckFire(int32_t id, int32_t k, int32_t deadline_ms, int32_t hi)
{
    if ((k >= g_ThFireCount[id]) || (k >= _STK_HRT_TH_FIRES_MAX))
    {
        printf("th: timer %d expiration %d is missing\n", (int)id, (int)k);
        return false;
    }

    const int32_t late = g_ThFireTime[id][k] - deadline_ms;

    printf("th: timer %d expiration %d late=%d (expected -%d..%d)\n", (int)id, (int)k, (int)late,
        (int)_STK_HRT_TEST_EARLY_TOL_MS, (int)hi);

    return InRange(late, -_STK_HRT_TEST_EARLY_TOL_MS, hi);
}

static bool VerifyTh(int32_t scenario)
{
    const ThCfg &c = g_ThCfg;
    bool ok = true;

    if (g_ThJobs != c.jobs)
    {
        printf("th: driver jobs done=%d (expected %d)\n", (int)g_ThJobs, (int)c.jobs);
        return false;
    }

    // every callback must have run inline from Process()
    for (int32_t id = 0; id < 2; ++id)
    {
        if (g_ThInline[id] != g_ThFireCount[id])
        {
            printf("th: timer %d: %d of %d callbacks ran outside of Process()\n", (int)id,
                (int)(g_ThFireCount[id] - g_ThInline[id]), (int)g_ThFireCount[id]);
            ok = false;
        }
    }

    // driver schedule (job j starts on the grid base + j * period) and job durations
    const int32_t base = g_ThJobStart[0];
    const int32_t long_threshold = (scenario == TH_CALLBACK_BUDGET ? 9 : (scenario == TH_CALLBACK_OVERRUN ? 28 : 1000));
    int32_t long_jobs = 0;

    for (int32_t j = 0; j < c.jobs; ++j)
    {
        const int32_t expect = base + (j * c.period);
        const int32_t dstart = Abs(g_ThJobStart[j] - expect);
        const int32_t dur    = g_ThJobDur[j];

        printf("th: job=%d start=%d (expect %d) duration=%d\n", (int)j, (int)(g_ThJobStart[j] - base),
            (int)(expect - base), (int)dur);

        if (dstart > _STK_HRT_TEST_START_TOL_MS)
        {
            printf("th: failed job start (%d): diff=%d (>%d)\n", (int)j, (int)dstart, (int)_STK_HRT_TEST_START_TOL_MS);
            ok = false;
        }

        if (dur >= long_threshold)
        {
            ++long_jobs;
        }
        else if (dur > _STK_HRT_TEST_IDLE_JOB_MS)
        {
            printf("th: failed job (%d): duration=%d, no callback expected to run\n", (int)j, (int)dur);
            ok = false;
        }
        else
        {
            // short job: Process() without callbacks
        }

        if (!c.expect_miss && (dur > (c.deadline + _STK_HRT_TEST_DEADLINE_SLACK_MS)))
        {
            printf("th: failed deadline (%d): duration=%d deadline=%d\n", (int)j, (int)dur, (int)c.deadline);
            ok = false;
        }
    }

    // tolerance of an expiration: one job period + start tolerance + callbacks which ran before it in the same job
    const int32_t hi = c.period + _STK_HRT_TEST_START_TOL_MS + c.work0;

    switch (scenario)
    {
    case TH_BASIC:
        ok = CheckCount(0, 1) && ok;
        ok = CheckFire(0, 0, g_ThIssued[0][0] + 35, hi) && ok;

        ok = CheckCount(1, 4) && ok;
        for (int32_t k = 0; k < 4; ++k)
            ok = CheckFire(1, k, g_ThIssued[1][0] + 25 + (50 * k), hi) && ok;
        break;

    case TH_CROSS_TASK:
        // timer 0: one-shot at issue + 40, then restarted as a 33 ms one-shot; timer 1: 20 ms periodic, stopped
        // before its third expiration (the Stop() is applied by the next Process(), before the deadline scan)
        ok = CheckCount(0, 2) && ok;
        ok = CheckFire(0, 0, g_ThIssued[0][0] + 40, hi) && ok;
        ok = CheckFire(0, 1, g_ThIssued[0][1] + 33, hi) && ok;

        ok = CheckCount(1, 2) && ok;
        for (int32_t k = 0; k < 2; ++k)
            ok = CheckFire(1, k, g_ThIssued[1][0] + 20 + (20 * k), hi) && ok;
        break;

    case TH_CALLBACK_BUDGET:
        // both timers expire in the same job (4 times): that job runs both callbacks, the others none
        for (int32_t id = 0; id < 2; ++id)
        {
            ok = CheckCount(id, 4) && ok;
            for (int32_t k = 0; k < 4; ++k)
                ok = CheckFire(id, k, g_ThIssued[id][0] + 45 + (40 * k), hi) && ok;
        }

        printf("th: jobs with callbacks=%d (expected 4)\n", (int)long_jobs);
        if (long_jobs != 4)
            ok = false;
        break;

    case TH_CALLBACK_OVERRUN:
        ok = CheckCount(0, 1) && ok;
        ok = CheckFire(0, 0, g_ThIssued[0][0] + 25, hi) && ok;

        // exactly one job ran the long callback, the schedule of the others is verified above
        printf("th: overrunning jobs=%d (expected 1)\n", (int)long_jobs);
        if (long_jobs != 1)
            ok = false;
        break;

    default:
        ok = false;
        break;
    }

    // deadline misses: only of the driver and only in the overrun scenario (first report is deadline + 1)
    for (int32_t t = 0; t < 2; ++t)
    {
        if (c.expect_miss && (t == 0))
        {
            printf("th: driver misses=%d first_duration=%d (expected %d)\n", (int)g_MissCount[t],
                (int)g_MissFirst[t], (int)(c.deadline + 1));

            if ((g_MissCount[t] == 0) || (g_MissFirst[t] != (c.deadline + 1)))
                ok = false;
        }
        else if (g_MissCount[t] != 0)
        {
            printf("th: id=%d unexpected deadline miss (%d), first duration=%d\n", (int)t,
                (int)g_MissCount[t], (int)g_MissFirst[t]);
            ok = false;
        }
        else
        {
            // no miss expected and none reported
        }
    }

    printf("th: hard faults=%d (%s)\n", (int)g_Overrider.m_faults, c.expect_miss ? "expected" : "none expected");

    if (c.expect_miss ? (g_Overrider.m_faults == 0) : (g_Overrider.m_faults != 0))
        ok = false;

    return ok;
}

#endif // __ARM_ARCH_6M__

} // namespace hrt
} // namespace test
} // namespace stk

/*! \fn    RunTest
    \brief Helper function to run a single test case.
    \param[in] test_name: Name of the test.
    \param[in] scenario:  Scenario id (see EScenario), selects the task configuration.
*/
template <class TaskType>
static int32_t RunTest(const char *test_name, int32_t scenario)
{
    using namespace stk;
    using namespace stk::test;
    using namespace stk::test::hrt;

    printf("Test: %s\n", test_name);

    ResetTestState();

    const int32_t tasks = SetupScenario(scenario);
    STK_ASSERT((tasks > 0) && (tasks <= _STK_HRT_TEST_TASKS_MAX));

    // static: a removed task (and its stack) is reused by the next test
    static TaskType task0(0);
    static TaskType task1(1);
    static TaskType task2(2);
    static TaskType task3(3);

    ITask *const list[_STK_HRT_TEST_TASKS_MAX] = { &task0, &task1, &task2, &task3 };

    for (int32_t t = 0; t < tasks; ++t)
    {
        g_Kernel.AddTask(list[t], MsToTicks(g_Cfg[t].period), MsToTicks(g_Cfg[t].deadline),
            MsToTicks(g_Cfg[t].delay));
    }

    g_Kernel.Start();

    int32_t result = (VerifyScenario(tasks) ? TestContext::SUCCESS_EXIT_CODE : TestContext::DEFAULT_FAILURE_EXIT_CODE);

    printf("Result: %s\n", result == TestContext::SUCCESS_EXIT_CODE ? "PASS" : "FAIL");
    printf("--------------\n");

    return result;
}

#ifndef __ARM_ARCH_6M__
/*! \fn    RunTimerTest
    \brief Helper function to run a single TimerHost test case (polled mode driven by an HRT job).
    \param[in] test_name: Name of the test.
    \param[in] scenario:  Scenario id (see EThScenario).
*/
template <EAccessMode _AccessMode>
static int32_t RunTimerTest(const char *test_name, int32_t scenario)
{
    using namespace stk;
    using namespace stk::test;
    using namespace stk::test::hrt;

    printf("Test: %s\n", test_name);

    ResetTestState();
    ResetThState();
    ThSetup(scenario);

    // no host tasks: the driver job calls Process()
    g_TimerHost.InitializePolled();

    // static: a removed task (and its stack) is reused by the next test
    static ThDriverTask<_AccessMode>     driver(0);
    static ThControllerTask<_AccessMode> controller(1);

    g_Kernel.AddTask(&driver, MsToTicks(g_ThCfg.period), MsToTicks(g_ThCfg.deadline), 0);

    if (g_ThCfg.ctrl_jobs > 0)
    {
        g_Kernel.AddTask(&controller, MsToTicks(g_ThCfg.ctrl_period), MsToTicks(g_ThCfg.ctrl_deadline),
            MsToTicks(g_ThCfg.ctrl_delay));
    }

    g_Kernel.Start();

    int32_t result = (VerifyTh(scenario) ? TestContext::SUCCESS_EXIT_CODE : TestContext::DEFAULT_FAILURE_EXIT_CODE);

    printf("Result: %s\n", result == TestContext::SUCCESS_EXIT_CODE ? "PASS" : "FAIL");
    printf("--------------\n");

    return result;
}
#endif // __ARM_ARCH_6M__

/*! \fn    main
    \brief Entry to the test suite.
*/
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    using namespace stk::test::hrt;

    TestContext::ShowTestSuitePrologue();

    int total_failures = 0, total_success = 0;

    printf("--------------\n");

    g_Kernel.Initialize();

    // hard faults of the HRT mode are counted and survived (must be set before Start())
    g_Kernel.GetPlatform()->SetEventOverrider(&g_Overrider);

#define RUN(TestClass, name, scenario) \
    do { \
        if (RunTest<TestClass<ACCESS_PRIVILEGED>>(name, scenario) \
                != TestContext::SUCCESS_EXIT_CODE) \
            total_failures++; \
        else \
            total_success++; \
    } while (0)

    // Test 1: Basic, 3 tasks with staggered start delays and workloads of 10/20/30 ms
    RUN(HrtTask, "Basic",             SC_BASIC);

    // Test 2: A single task alone in the system (CPU enters the sleep trap every period)
    RUN(HrtTask, "SingleTask",        SC_SINGLE_TASK);

    // Test 3: Job completes immediately, Yield() without any work
    RUN(HrtTask, "ZeroWork",          SC_ZERO_WORK);

    // Test 4: Tasks activated at the same moment, deadline covers the time waiting for the CPU
    RUN(HrtTask, "Simultaneous",      SC_SIMULTANEOUS);

    // Test 5: Deadline only a few ticks above the workload, no miss allowed
    RUN(HrtTask, "TightDeadline",     SC_TIGHT_DEADLINE);

    // Test 6: Start delay longer than the period of the task
    RUN(HrtTask, "LongStartDelay",    SC_LONG_START_DELAY);

    // Test 7: Tasks with different periodicities and workloads
    RUN(HrtTask, "DifferentPeriods",  SC_DIFFERENT_PERIODS);

    // Test 8: 20 consecutive periods, no accumulated drift
    RUN(HrtTask, "NoDrift",           SC_NO_DRIFT);

    // Test 9: Tasks exit after a different number of jobs, the others keep their schedule
    RUN(HrtTask, "StaggeredExit",     SC_STAGGERED_EXIT);

    // Test 10: Last job ends by returning from Run() instead of Yield()
    RUN(HrtTask, "ExitWithoutYield",  SC_EXIT_WITHOUT_YIELD);

    // Test 11: A task returns from Run() without running a single job
    RUN(HrtTask, "ImmediateExit",     SC_IMMEDIATE_EXIT);

    // Test 12: All kernel slots are used
    RUN(HrtTask, "FullPool",          SC_FULL_POOL);

    // Test 13: Deadline overrun is detected (duration == deadline + 1) and reported as a hard fault
    RUN(HrtTask, "DeadlineMiss",      SC_DEADLINE_MISS);

    // Test 14: Overrunning task does not disturb the schedule of a healthy one
    RUN(HrtTask, "MissIsolated",      SC_MISS_ISOLATED);

#undef RUN

#ifndef __ARM_ARCH_6M__
    // Tests 15-18: TimerHost in HRT mode (polled mode: an HRT job calls TimerHost::Process())
    {
        struct ThCase { const char *name; int32_t scenario; };

        static const ThCase cases[] =
        {
            // one-shot and periodic timers expire inline from the job, late by up to one job period
            { "TimerBasic",     TH_BASIC            },
            // Start/Stop/Restart issued from another HRT task take effect at the next job
            { "TimerCrossTask", TH_CROSS_TASK       },
            // callbacks of two timers expiring in one job fit the job deadline
            { "TimerBudget",    TH_CALLBACK_BUDGET  },
            // a callback exceeding the job deadline is a deadline miss, the schedule goes on
            { "TimerOverrun",   TH_CALLBACK_OVERRUN }
        };

#if defined(_STK_ARCH_RISC_V)
        // QEMU translates code on its first execution: the first run of the TimerHost code paths is much slower
        // than the following ones and its job 0 can exceed the job deadline. Run a warm-up pass, result is ignored.
        printf("Warm-up (result ignored)\n");
        (void)RunTimerTest<ACCESS_PRIVILEGED>("TimerWarmUp", TH_BASIC);
#endif

        for (size_t i = 0; i < (sizeof(cases) / sizeof(cases[0])); ++i)
        {
            if (RunTimerTest<ACCESS_PRIVILEGED>(cases[i].name, cases[i].scenario) != TestContext::SUCCESS_EXIT_CODE)
                total_failures++;
            else
                total_success++;
        }
    }
#endif // __ARM_ARCH_6M__

    int32_t final_result = (total_failures == 0
        ? TestContext::SUCCESS_EXIT_CODE
        : TestContext::DEFAULT_FAILURE_EXIT_CODE);

    printf("##############\n");
    printf("Total tests: %d\n", total_failures + total_success);
    printf("Failures: %d\n", (int)total_failures);

    TestContext::ShowTestSuiteEpilogue(final_result);
    return final_result;
}
