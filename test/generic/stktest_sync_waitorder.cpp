/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include "stktest.h"

namespace stk {
namespace test {

// ============================================================================ //
// ======= Sync: wait order, IKernelService::SelectWaiter (Kernel) ============ //
// ============================================================================ //
//
// The selection is implemented by the kernel (Kernel::KernelService::SelectWaiter), which compiles in only the
// orders that can differ from WAIT_ORDER_FIFO for its strategy and mode:
//  - WAIT_ORDER_PRIORITY, WAIT_ORDER_PRIORITY_PACKED need WEIGHT_API (a weighted strategy),
//  - WAIT_ORDER_DEADLINE, WAIT_ORDER_PERIOD, WAIT_ORDER_DEADLINE_FIXED need KERNEL_HRT,
//  - the other combination degenerates to FIFO and reads no key at all.
// Weighted strategies are not supported in HRT mode, therefore the tests run against real kernels of different kinds:
//  - SelKernelPT32 / SelKernelFP32: weighted, SelKernelEDF / SelKernelRM / SelKernelDM: HRT, SelKernelRR: neither.
// ISyncObject::FindNextWaiter (the layer above) is tested against a mock kernel service.

// ---------------------------------------------------------------------------- //
// Kernels
// ---------------------------------------------------------------------------- //

static const uint8_t SEL_MODE_SRT = (KERNEL_STATIC | KERNEL_SYNC);
static const uint8_t SEL_MODE_HRT = (KERNEL_STATIC | KERNEL_HRT | KERNEL_SYNC);

typedef Kernel<SEL_MODE_SRT, 1U, sched::SwitchStrategyPT32, PlatformTestMock> SelKernelPT32;
typedef Kernel<SEL_MODE_SRT, 1U, sched::SwitchStrategyFP32, PlatformTestMock> SelKernelFP32;
typedef Kernel<SEL_MODE_SRT, 1U, sched::SwitchStrategyRR,   PlatformTestMock> SelKernelRR;
typedef Kernel<SEL_MODE_HRT, 1U, sched::SwitchStrategyEDF,  PlatformTestMock> SelKernelEDF;
typedef Kernel<SEL_MODE_HRT, 1U, sched::SwitchStrategyRM,   PlatformTestMock> SelKernelRM;
typedef Kernel<SEL_MODE_HRT, 1U, sched::SwitchStrategyDM,   PlatformTestMock> SelKernelDM;

// Initialized kernel which is the current IKernelService::GetInstance() for the lifetime of the object.
// Declare it first in a test: it must outlive everything which uses the service.
template <class TKernel> class SelKernelScope
{
public:
    explicit SelKernelScope() : m_kernel()
    {
        m_kernel.Initialize();
    }

    IKernelService *Service() { return IKernelService::GetInstance(); }

private:
    TKernel m_kernel;
};

// What a kernel is able to rank by
struct SelCaps
{
    bool weights; //!< WEIGHT_API: WAIT_ORDER_PRIORITY, WAIT_ORDER_PRIORITY_PACKED
    bool hrt;     //!< KERNEL_HRT: WAIT_ORDER_DEADLINE, WAIT_ORDER_PERIOD, WAIT_ORDER_DEADLINE_FIXED
};

static const SelCaps SEL_CAPS_WEIGHTED = { true,  false };
static const SelCaps SEL_CAPS_HRT      = { false, true  };
static const SelCaps SEL_CAPS_PLAIN    = { false, false };

// ---------------------------------------------------------------------------- //
// Stubs
// ---------------------------------------------------------------------------- //

// Kernel task stub: holds the keys which the wait order selectors read and counts every read, which lets
// the tests check that a selector reads only the key of its own order (and only once per waiter).
class SelKernelTask final : public IKernelTask
{
public:
    explicit SelKernelTask()
        : m_weight(DEFAULT_WEIGHT), m_base_weight(DEFAULT_WEIGHT), m_rel_deadline(0), m_period(0), m_deadline(0)
    {
        ResetReads();
    }

    ITask *GetUserTask() override { return NULL; }
    Stack GetUserStack() const override { Stack stack = Stack(); return stack; }

    Weight GetWeight() const override { ++m_reads_weight; return m_weight; }
    Weight GetBaseWeight() const override { ++m_reads_base_weight; return m_base_weight; }
    void SetCurrentWeight(Weight) override {}
    Weight GetCurrentWeight() const override { return NO_WEIGHT; }

    Timeout GetHrtPeriodicity() const override { ++m_reads_period; return m_period; }
    Timeout GetHrtDeadline() const override { ++m_reads_deadline; return m_deadline; }
    Timeout GetHrtRelativeDeadline() const override { ++m_reads_rel_deadline; return m_rel_deadline; }

    bool IsSleeping() const override { return true; }
    void Wake() override {}

    void ResetReads()
    {
        m_reads_weight = m_reads_base_weight = m_reads_rel_deadline = m_reads_period = m_reads_deadline = 0;
    }

    uint32_t TotalReads() const
    {
        return m_reads_weight + m_reads_base_weight + m_reads_rel_deadline + m_reads_period + m_reads_deadline;
    }

    Weight  m_weight;        //!< effective weight (what GetWeight() returns)
    Weight  m_base_weight;   //!< base weight (what GetBaseWeight() returns)
    Timeout m_rel_deadline;  //!< remaining deadline (dynamic, EDF)
    Timeout m_period;        //!< period (static, Rate-Monotonic rank)
    Timeout m_deadline;      //!< configured deadline (static, Deadline-Monotonic rank)

    mutable uint32_t m_reads_weight;
    mutable uint32_t m_reads_base_weight;
    mutable uint32_t m_reads_rel_deadline;
    mutable uint32_t m_reads_period;
    mutable uint32_t m_reads_deadline;
};

// Wait object bound to a SelKernelTask. Wake() only counts the call: the waiter stays in the wait list.
class SelWaitObject final : public IWaitObject
{
public:
    explicit SelWaitObject() : m_tid(TID_NONE), m_task(NULL), m_wake_count(0)
    {}

    TId GetTid() const override { return m_tid; }
    IKernelTask *GetKernelTask() const override { return m_task; }
    void Wake(bool) override { ++m_wake_count; }
    void WakeDeleted() { ++m_wake_count; }
    EState GetState() const override { return STATE_WAIT; }
    bool Tick(Timeout) override { return false; }

    TId          m_tid;
    IKernelTask *m_task;
    uint32_t     m_wake_count;
};

static const size_t SEL_NONE = static_cast<size_t>(-1); // "no waiter was selected"

// A set of waiters in arrival order (index = arrival order, 0 is the first to arrive). The waiters are linked either to
// an own wait list or, if \a sobj is given, to the wait list of that synchronization object.
// Selection is done by the kernel which is the current IKernelService::GetInstance().
class SelWaiters
{
public:
    enum { CAPACITY = 8 };

    explicit SelWaiters(ISyncObject *sobj = NULL) : m_sobj(sobj), m_count(0)
    {}

    ~SelWaiters()
    {
        Clear();
    }

    //! Waiter id derived from its arrival index.
    static TId TidOf(size_t index) { return static_cast<TId>(0x100 * (index + 1)); }

    //! Append a waiter. Returns its index.
    size_t Add(Weight weight, Timeout rel_deadline = 0, Timeout period = 0, Timeout deadline = 0)
    {
        STK_ASSERT(m_count < CAPACITY);

        SelKernelTask &task = m_task[m_count];
        task.m_weight       = weight;
        task.m_base_weight  = weight;
        task.m_rel_deadline = rel_deadline;
        task.m_period       = period;
        task.m_deadline     = deadline;

        SelWaitObject &waiter = m_waiter[m_count];
        waiter.m_tid  = TidOf(m_count);
        waiter.m_task = &task;

        if (m_sobj != NULL)
            m_sobj->AddWaitObject(&waiter);
        else
            m_list.LinkBack(&waiter);

        return m_count++;
    }

    //! Unlink a waiter (it timed out, was cancelled or was served).
    void Remove(size_t index)
    {
        STK_ASSERT(index < m_count);
        STK_ASSERT(m_waiter[index].GetHead() != NULL);

        if (m_sobj != NULL)
            m_sobj->RemoveWaitObject(&m_waiter[index]);
        else
            m_list.Unlink(&m_waiter[index]);
    }

    void Clear()
    {
        for (size_t i = 0; i < m_count; ++i)
        {
            if (m_waiter[i].GetHead() != NULL)
                Remove(i);
        }

        m_count = 0;
    }

    size_t Count() const { return m_count; }

    SelWaitObject &Waiter(size_t index) { return m_waiter[index]; }
    SelKernelTask &Task(size_t index) { return m_task[index]; }

    IWaitObject::ListHeadType &List() { return m_list; }
    const IWaitObject::ListHeadType &List() const { return m_list; }

    //! Arrival index of the waiter, SEL_NONE if \a waiter is NULL.
    size_t IndexOf(const IWaitObject *waiter) const
    {
        for (size_t i = 0; i < m_count; ++i)
        {
            if (waiter == &m_waiter[i])
                return i;
        }

        return SEL_NONE;
    }

    //! Select via the kernel service (the current IKernelService instance).
    IWaitObject *Select(EWaitOrder order)
    {
        return IKernelService::GetInstance()->SelectWaiter(m_list, order);
    }

    void ResetReads()
    {
        for (size_t i = 0; i < m_count; ++i)
            m_task[i].ResetReads();
    }

    uint32_t TotalReads() const
    {
        uint32_t reads = 0;

        for (size_t i = 0; i < m_count; ++i)
            reads += m_task[i].TotalReads();

        return reads;
    }

    uint32_t ReadsWeight() const        { uint32_t n = 0; for (size_t i = 0; i < m_count; ++i) n += m_task[i].m_reads_weight;        return n; }
    uint32_t ReadsBaseWeight() const    { uint32_t n = 0; for (size_t i = 0; i < m_count; ++i) n += m_task[i].m_reads_base_weight;   return n; }
    uint32_t ReadsRelDeadline() const   { uint32_t n = 0; for (size_t i = 0; i < m_count; ++i) n += m_task[i].m_reads_rel_deadline; return n; }
    uint32_t ReadsPeriod() const        { uint32_t n = 0; for (size_t i = 0; i < m_count; ++i) n += m_task[i].m_reads_period;        return n; }
    uint32_t ReadsDeadline() const      { uint32_t n = 0; for (size_t i = 0; i < m_count; ++i) n += m_task[i].m_reads_deadline;      return n; }

private:
    SelKernelTask             m_task[CAPACITY];
    SelWaitObject             m_waiter[CAPACITY];
    IWaitObject::ListHeadType m_list;
    ISyncObject              *m_sobj;
    size_t                    m_count;
};

// Weight packed as (priority << 16 | threshold), the encoding of SwitchStrategyPreemptionThreshold
static Weight Pack(uint32_t priority, uint32_t threshold)
{
    return static_cast<Weight>((priority << 16) | threshold);
}

// Mock IKernelService for testing the layer above the kernel (ISyncObject::FindNextWaiter): records every
// SelectWaiter() request and returns a chosen waiter. It is the instance returned by IKernelService::GetInstance()
// while it exists. Do not keep it alive together with a SelKernelScope.
class SelKernelService final : public KernelServiceMock
{
public:
    explicit SelKernelService() : m_calls(0), m_last_order(WAIT_ORDER_FIFO), m_result(NULL), m_prev(g_KernelService)
    {
        g_KernelService = this;
    }

    ~SelKernelService()
    {
        g_KernelService = m_prev;
    }

    IWaitObject *SelectWaiter(IWaitObject::ListHeadType &, EWaitOrder order) override
    {
        ++m_calls;
        m_last_order = order;
        return m_result;
    }

    uint32_t     m_calls;
    EWaitOrder   m_last_order;
    IWaitObject *m_result;

private:
    IKernelService *m_prev;
};

// SyncObjectBase with a chosen wait order, exposing what the tests need
class SelSyncObject final : public SyncObjectBase
{
public:
    explicit SelSyncObject(EWaitOrder order) : SyncObjectBase(order)
    {}

    IWaitObject *Next() { return ISyncObject::FindNextWaiter(m_wait_list, GetWaitOrder()); }

    // wake the waiter selected by the order of this object (what WakeOne() does for the built-in primitives)
    void WakeNext() { ISyncObject::WakeOne(m_wait_list, GetWaitOrder()); }
};

// ---------------------------------------------------------------------------- //
// Order cases
// ---------------------------------------------------------------------------- //

enum ESelKey
{
    SEL_KEY_NONE,         // FIFO: no key
    SEL_KEY_WEIGHT,       // effective weight
    SEL_KEY_REL_DEADLINE, // remaining deadline
    SEL_KEY_PERIOD,       // period
    SEL_KEY_DEADLINE      // configured deadline
};

// Every order which does not depend on the kernel default, with the waiter which AddDistinctWinners() makes it select and
// the key it ranks by (read once per waiter, nothing else).
struct SelOrderCase
{
    EWaitOrder order;
    size_t     winner;
    ESelKey    key;
};

static const size_t SEL_DISTINCT_COUNT = 6;

static const SelOrderCase SEL_ORDER_CASES[] =
{
    { WAIT_ORDER_FIFO,            0, SEL_KEY_NONE         },
    { WAIT_ORDER_PRIORITY,        1, SEL_KEY_WEIGHT       },
    { WAIT_ORDER_DEADLINE,        2, SEL_KEY_REL_DEADLINE },
    { WAIT_ORDER_PRIORITY_PACKED, 3, SEL_KEY_WEIGHT       },
    { WAIT_ORDER_PERIOD,          4, SEL_KEY_PERIOD       },
    { WAIT_ORDER_DEADLINE_FIXED,  5, SEL_KEY_DEADLINE     }
};

// Orders which the kernel does not compile in degenerate to FIFO
static bool IsSupported(const SelOrderCase &c, const SelCaps &caps)
{
    switch (c.key)
    {
    case SEL_KEY_NONE:   return true;
    case SEL_KEY_WEIGHT: return caps.weights;
    default:             return caps.hrt;
    }
}

static size_t ExpectedWinner(const SelOrderCase &c, const SelCaps &caps)
{
    return (IsSupported(c, caps) ? c.winner : 0);
}

static uint32_t ExpectedReads(const SelOrderCase &c, const SelCaps &caps, ESelKey key)
{
    return (((c.key == key) && IsSupported(c, caps)) ? static_cast<uint32_t>(SEL_DISTINCT_COUNT) : 0U);
}

// Six waiters, each is the best one for exactly one order, so that every order gives a different answer:
static void AddDistinctWinners(SelWaiters &w)
{
    //     weight             remaining  period  deadline
    w.Add(Pack(1, 1),         10,        100,    50); // 0: arrived first                     -> FIFO
    w.Add(Pack(5, 5),         10,        100,    50); // 1: highest raw weight                -> PRIORITY
    w.Add(Pack(1, 1),         1,         100,    50); // 2: least remaining deadline          -> DEADLINE
    w.Add(Pack(0x8000, 0),    10,        100,    50); // 3: highest priority (the raw weight is negative!) -> PRIORITY_PACKED
    w.Add(Pack(1, 1),         10,        5,      50); // 4: shortest period                   -> PERIOD
    w.Add(Pack(1, 1),         10,        100,    7);  // 5: shortest configured deadline      -> DEADLINE_FIXED

    CHECK_EQUAL(SEL_DISTINCT_COUNT, w.Count());
}

#define CHECK_SELECTED(expected_index, waiters, order) \
    CHECK_EQUAL(static_cast<size_t>(expected_index), (waiters).IndexOf((waiters).Select(order)))

// ---------------------------------------------------------------------------- //
// Templated checks (run for kernels of different kinds)
// ---------------------------------------------------------------------------- //

template <class TKernel>
static void CheckEveryOrderSelectsItsWinner(const SelCaps &caps)
{
    SelKernelScope<TKernel> kernel;

    for (size_t i = 0; i < STK_STATIC_ARRAY_SIZE(SEL_ORDER_CASES); ++i)
    {
        const SelOrderCase &c = SEL_ORDER_CASES[i];

        SelWaiters w;
        AddDistinctWinners(w);

        CHECK_SELECTED(ExpectedWinner(c, caps), w, c.order);
    }
}

template <class TKernel>
static void CheckEveryOrderReadsOnlyItsOwnKeyOncePerWaiter(const SelCaps &caps)
{
    SelKernelScope<TKernel> kernel;

    for (size_t i = 0; i < STK_STATIC_ARRAY_SIZE(SEL_ORDER_CASES); ++i)
    {
        const SelOrderCase &c = SEL_ORDER_CASES[i];

        SelWaiters w;
        AddDistinctWinners(w);
        w.ResetReads();

        CHECK_SELECTED(ExpectedWinner(c, caps), w, c.order);

        // an order which is compiled out reads nothing at all
        CHECK_EQUAL(ExpectedReads(c, caps, SEL_KEY_WEIGHT),       w.ReadsWeight());
        CHECK_EQUAL(ExpectedReads(c, caps, SEL_KEY_REL_DEADLINE), w.ReadsRelDeadline());
        CHECK_EQUAL(ExpectedReads(c, caps, SEL_KEY_PERIOD),       w.ReadsPeriod());
        CHECK_EQUAL(ExpectedReads(c, caps, SEL_KEY_DEADLINE),     w.ReadsDeadline());
        CHECK_EQUAL(0, w.ReadsBaseWeight()); // the effective weight is what orders waiters, never the base one
    }
}

template <class TKernel>
static void CheckFindNextWaiterMatchesSelectWaiter(const SelCaps &caps)
{
    SelKernelScope<TKernel> kernel;

    for (size_t i = 0; i < STK_STATIC_ARRAY_SIZE(SEL_ORDER_CASES); ++i)
    {
        const SelOrderCase &c = SEL_ORDER_CASES[i];

        SelWaiters w;
        AddDistinctWinners(w);

        const SelWaiters &cw = w;

        CHECK_EQUAL(ExpectedWinner(c, caps), w.IndexOf(ISyncObject::FindNextWaiter(w.List(), c.order)));
        CHECK_EQUAL(ExpectedWinner(c, caps), cw.IndexOf(ISyncObject::FindNextWaiter(cw.List(), c.order)));
    }

    // empty list
    SelWaiters empty;
    const SelWaiters &cempty = empty;
    POINTERS_EQUAL(NULL, ISyncObject::FindNextWaiter(empty.List(), WAIT_ORDER_FIFO));
    POINTERS_EQUAL(NULL, ISyncObject::FindNextWaiter(cempty.List(), WAIT_ORDER_AUTO));
}

// WAIT_ORDER_AUTO is resolved by the kernel to the order declared by its strategy (WAIT_ORDER_HINT)
template <class TKernel>
static void CheckAutoIsResolvedToStrategyHint(EWaitOrder hint, size_t winner)
{
    SelKernelScope<TKernel> kernel;

    CHECK_EQUAL(hint, kernel.Service()->GetDefaultWaitOrder());

    SelWaiters w;
    AddDistinctWinners(w);

    CHECK_SELECTED(winner, w, WAIT_ORDER_AUTO);

    // the same through a synchronization object which is configured with AUTO
    SelSyncObject sobj(WAIT_ORDER_AUTO);
    SelWaiters sw(&sobj);
    AddDistinctWinners(sw);

    CHECK_EQUAL(WAIT_ORDER_AUTO, sobj.GetWaitOrder()); // reported as configured, resolved only when selecting
    CHECK_EQUAL(SelWaiters::TidOf(winner), sobj.GetNextWaiter());
}

template <class TKernel>
static void CheckSyncObjectFollowsTheWaitOrder(const SelCaps &caps)
{
    SelKernelScope<TKernel> kernel;

    for (size_t i = 0; i < STK_STATIC_ARRAY_SIZE(SEL_ORDER_CASES); ++i)
    {
        const SelOrderCase &c = SEL_ORDER_CASES[i];

        SelSyncObject sobj(c.order);
        SelWaiters w(&sobj);
        AddDistinctWinners(w);

        CHECK_EQUAL(c.order, sobj.GetWaitOrder());
        CHECK_EQUAL(SelWaiters::TidOf(ExpectedWinner(c, caps)), sobj.GetNextWaiter());
        CHECK_EQUAL(ExpectedWinner(c, caps), w.IndexOf(sobj.Next()));
    }
}

template <class TKernel>
static void CheckSetWaitOrderTakesEffectWithWaitersQueued(const SelCaps &caps)
{
    SelKernelScope<TKernel> kernel;

    SelSyncObject sobj(WAIT_ORDER_FIFO);
    SelWaiters w(&sobj);
    AddDistinctWinners(w);

    for (size_t i = 0; i < STK_STATIC_ARRAY_SIZE(SEL_ORDER_CASES); ++i)
    {
        const SelOrderCase &c = SEL_ORDER_CASES[i];

        sobj.SetWaitOrder(c.order);

        CHECK_EQUAL(c.order, sobj.GetWaitOrder());
        CHECK_EQUAL(SelWaiters::TidOf(ExpectedWinner(c, caps)), sobj.GetNextWaiter());
    }
}

template <class TKernel>
static void CheckWakeOneWakesTheSelectedWaiter(const SelCaps &caps)
{
    SelKernelScope<TKernel> kernel;

    for (size_t i = 0; i < STK_STATIC_ARRAY_SIZE(SEL_ORDER_CASES); ++i)
    {
        const SelOrderCase &c = SEL_ORDER_CASES[i];

        SelSyncObject sobj(c.order);
        SelWaiters w(&sobj);
        AddDistinctWinners(w);

        sobj.WakeNext();

        for (size_t k = 0; k < SEL_DISTINCT_COUNT; ++k)
            CHECK_EQUAL((k == ExpectedWinner(c, caps)) ? 1 : 0, w.Waiter(k).m_wake_count);
    }
}

// ---------------------------------------------------------------------------- //
// Tests: selection
// ---------------------------------------------------------------------------- //

TEST_GROUP(SyncWaitOrder)
{
    void setup() {}
    void teardown() {}
};

TEST(SyncWaitOrder, EmptyListSelectsNobody)
{
    SelKernelScope<SelKernelPT32> kernel;
    SelWaiters w;

    for (size_t i = 0; i < STK_STATIC_ARRAY_SIZE(SEL_ORDER_CASES); ++i)
        CHECK_SELECTED(SEL_NONE, w, SEL_ORDER_CASES[i].order);

    CHECK_SELECTED(SEL_NONE, w, WAIT_ORDER_AUTO);
}

TEST(SyncWaitOrder, SingleWaiterIsSelectedWithoutLookingAtKeys)
{
    SelKernelScope<SelKernelPT32> kernel;
    SelWaiters w;

    w.Add(Pack(3, 3), 5, 20, 10);

    for (size_t i = 0; i < STK_STATIC_ARRAY_SIZE(SEL_ORDER_CASES); ++i)
        CHECK_SELECTED(0, w, SEL_ORDER_CASES[i].order);

    CHECK_SELECTED(0, w, WAIT_ORDER_AUTO);

    CHECK_EQUAL(0, w.TotalReads()); // nothing to choose from: no key is read
}

TEST(SyncWaitOrder, FifoSelectsFirstArrivedAndReadsNoKey)
{
    SelKernelScope<SelKernelPT32> kernel;
    SelWaiters w;

    w.Add(1, 9, 90, 90);
    w.Add(9, 1, 10, 10);
    w.Add(5, 5, 50, 50);

    CHECK_SELECTED(0, w, WAIT_ORDER_FIFO);
    CHECK_EQUAL(0, w.TotalReads());
}

TEST(SyncWaitOrder, EveryOrderSelectsItsOwnWinner_Weighted)
{
    CheckEveryOrderSelectsItsWinner<SelKernelPT32>(SEL_CAPS_WEIGHTED);
    CheckEveryOrderSelectsItsWinner<SelKernelFP32>(SEL_CAPS_WEIGHTED);
}

TEST(SyncWaitOrder, EveryOrderSelectsItsOwnWinner_Hrt)
{
    CheckEveryOrderSelectsItsWinner<SelKernelEDF>(SEL_CAPS_HRT);
    CheckEveryOrderSelectsItsWinner<SelKernelRM>(SEL_CAPS_HRT);
    CheckEveryOrderSelectsItsWinner<SelKernelDM>(SEL_CAPS_HRT);
}

TEST(SyncWaitOrder, EveryOrderIsFifo_NoKeyKernel)
{
    // round-robin has neither weights nor HRT keys: every order is compiled out and gives the first arrived waiter
    CheckEveryOrderSelectsItsWinner<SelKernelRR>(SEL_CAPS_PLAIN);
}

TEST(SyncWaitOrder, EveryOrderReadsOnlyItsOwnKeyOncePerWaiter)
{
    CheckEveryOrderReadsOnlyItsOwnKeyOncePerWaiter<SelKernelPT32>(SEL_CAPS_WEIGHTED);
    CheckEveryOrderReadsOnlyItsOwnKeyOncePerWaiter<SelKernelEDF>(SEL_CAPS_HRT);
    CheckEveryOrderReadsOnlyItsOwnKeyOncePerWaiter<SelKernelRR>(SEL_CAPS_PLAIN);
}

TEST(SyncWaitOrder, UnsupportedOrdersReadNoKeyAtAll)
{
    {
        // weighted kernel: HRT orders are FIFO
        SelKernelScope<SelKernelPT32> kernel;
        SelWaiters w;
        AddDistinctWinners(w);
        w.ResetReads();

        CHECK_SELECTED(0, w, WAIT_ORDER_DEADLINE);
        CHECK_SELECTED(0, w, WAIT_ORDER_PERIOD);
        CHECK_SELECTED(0, w, WAIT_ORDER_DEADLINE_FIXED);
        CHECK_EQUAL(0, w.TotalReads());
    }
    {
        // HRT kernel: weight orders are FIFO
        SelKernelScope<SelKernelEDF> kernel;
        SelWaiters w;
        AddDistinctWinners(w);
        w.ResetReads();

        CHECK_SELECTED(0, w, WAIT_ORDER_PRIORITY);
        CHECK_SELECTED(0, w, WAIT_ORDER_PRIORITY_PACKED);
        CHECK_EQUAL(0, w.TotalReads());
    }
}

TEST(SyncWaitOrder, FindNextWaiterMatchesSelectWaiter)
{
    CheckFindNextWaiterMatchesSelectWaiter<SelKernelPT32>(SEL_CAPS_WEIGHTED);
    CheckFindNextWaiterMatchesSelectWaiter<SelKernelEDF>(SEL_CAPS_HRT);
    CheckFindNextWaiterMatchesSelectWaiter<SelKernelRR>(SEL_CAPS_PLAIN);
}

// ---------------------------------------------------------------------------- //
// Tests: ISyncObject::FindNextWaiter delegation to the kernel service (mock)
// ---------------------------------------------------------------------------- //

TEST(SyncWaitOrder, FindNextWaiter_NoWaiterOrOneWaiterDoesNotAskTheKernel)
{
    SelKernelService svc;
    SelWaiters w;

    POINTERS_EQUAL(NULL, ISyncObject::FindNextWaiter(w.List(), WAIT_ORDER_PRIORITY));

    w.Add(1);
    CHECK_EQUAL(0, w.IndexOf(ISyncObject::FindNextWaiter(w.List(), WAIT_ORDER_PRIORITY)));
    CHECK_EQUAL(0, w.IndexOf(ISyncObject::FindNextWaiter(w.List(), WAIT_ORDER_AUTO)));

    CHECK_EQUAL(0, svc.m_calls); // nothing to choose from
}

TEST(SyncWaitOrder, FindNextWaiter_TwoOrMoreWaitersAreDelegatedToTheKernel)
{
    SelKernelService svc;
    SelWaiters w;

    w.Add(1);
    w.Add(2);
    w.Add(3);

    svc.m_result = &w.Waiter(2);

    // the answer of the kernel is returned as is
    CHECK_EQUAL(2, w.IndexOf(ISyncObject::FindNextWaiter(w.List(), WAIT_ORDER_PRIORITY)));
    CHECK_EQUAL(1, svc.m_calls);

    svc.m_result = &w.Waiter(1);
    CHECK_EQUAL(1, w.IndexOf(ISyncObject::FindNextWaiter(w.List(), WAIT_ORDER_FIFO)));
    CHECK_EQUAL(2, svc.m_calls);

    // const overload
    const SelWaiters &cw = w;
    CHECK_EQUAL(1, cw.IndexOf(ISyncObject::FindNextWaiter(cw.List(), WAIT_ORDER_FIFO)));
    CHECK_EQUAL(3, svc.m_calls);

    // the keys are not read by the helper itself
    CHECK_EQUAL(0, w.TotalReads());
}

TEST(SyncWaitOrder, FindNextWaiter_PassesTheOrderThroughUnresolved)
{
    SelKernelService svc;
    SelWaiters w;

    w.Add(1);
    w.Add(2);
    svc.m_result = &w.Waiter(0);

    for (size_t i = 0; i < STK_STATIC_ARRAY_SIZE(SEL_ORDER_CASES); ++i)
    {
        ISyncObject::FindNextWaiter(w.List(), SEL_ORDER_CASES[i].order);
        CHECK_EQUAL(SEL_ORDER_CASES[i].order, svc.m_last_order);
    }

    // AUTO is resolved by the kernel, not by the caller
    ISyncObject::FindNextWaiter(w.List(), WAIT_ORDER_AUTO);
    CHECK_EQUAL(WAIT_ORDER_AUTO, svc.m_last_order);
}

TEST(SyncWaitOrder, SyncObject_GetNextWaiterAsksTheKernelOnlyForSeveralWaiters)
{
    SelKernelService svc;

    SelSyncObject sobj(WAIT_ORDER_DEADLINE);
    SelWaiters w(&sobj);

    CHECK_EQUAL(TID_NONE, sobj.GetNextWaiter());
    CHECK_EQUAL(0, svc.m_calls);

    w.Add(1);
    CHECK_EQUAL(SelWaiters::TidOf(0), sobj.GetNextWaiter());
    CHECK_EQUAL(0, svc.m_calls);

    w.Add(2);
    svc.m_result = &w.Waiter(1);
    CHECK_EQUAL(SelWaiters::TidOf(1), sobj.GetNextWaiter());
    CHECK_EQUAL(1, svc.m_calls);
    CHECK_EQUAL(WAIT_ORDER_DEADLINE, svc.m_last_order);
}

// ---------------------------------------------------------------------------- //
// Tests: WAIT_ORDER_PRIORITY (weighted kernel)
// ---------------------------------------------------------------------------- //

TEST(SyncWaitOrder, Priority_HighestWeightWinsAnyPosition)
{
    SelKernelScope<SelKernelFP32> kernel;

    {
        SelWaiters w; // at the head
        w.Add(9); w.Add(1); w.Add(2);
        CHECK_SELECTED(0, w, WAIT_ORDER_PRIORITY);
    }
    {
        SelWaiters w; // in the middle
        w.Add(3); w.Add(7); w.Add(5);
        CHECK_SELECTED(1, w, WAIT_ORDER_PRIORITY);
    }
    {
        SelWaiters w; // at the tail
        w.Add(1); w.Add(2); w.Add(9);
        CHECK_SELECTED(2, w, WAIT_ORDER_PRIORITY);
    }
}

TEST(SyncWaitOrder, Priority_EqualWeightsKeepArrivalOrder)
{
    SelKernelScope<SelKernelFP32> kernel;

    {
        SelWaiters w;
        w.Add(5); w.Add(5); w.Add(5);
        CHECK_SELECTED(0, w, WAIT_ORDER_PRIORITY);
    }
    {
        SelWaiters w; // the earliest of the highest ones wins
        w.Add(1); w.Add(5); w.Add(5); w.Add(2);
        CHECK_SELECTED(1, w, WAIT_ORDER_PRIORITY);
    }
    {
        SelWaiters w;
        w.Add(5); w.Add(3); w.Add(5);
        CHECK_SELECTED(0, w, WAIT_ORDER_PRIORITY);
    }
}

TEST(SyncWaitOrder, Priority_HonorsWeightChangedWhileWaiting)
{
    SelKernelScope<SelKernelFP32> kernel;
    SelWaiters w;
    w.Add(5); w.Add(1); w.Add(3);

    CHECK_SELECTED(0, w, WAIT_ORDER_PRIORITY);

    // the weight of a blocked task is raised (SetWeight() or priority inheritance): the order is not stale
    w.Task(1).m_weight = 9;
    CHECK_SELECTED(1, w, WAIT_ORDER_PRIORITY);

    // and lowered
    w.Task(1).m_weight = 0;
    w.Task(0).m_weight = 2;
    CHECK_SELECTED(2, w, WAIT_ORDER_PRIORITY);
}

TEST(SyncWaitOrder, Priority_UsesEffectiveWeightNotBaseWeight)
{
    SelKernelScope<SelKernelFP32> kernel;
    SelWaiters w;
    w.Add(5);
    w.Add(1);

    // the second task has inherited a higher weight: it is what the scheduler runs it with
    w.Task(1).m_base_weight = 1;
    w.Task(1).m_weight      = 8;
    w.ResetReads();

    CHECK_SELECTED(1, w, WAIT_ORDER_PRIORITY);
    CHECK_EQUAL(0, w.ReadsBaseWeight());
}

TEST(SyncWaitOrder, Priority_ComparesRawWeightIncludingLowBits)
{
    SelKernelScope<SelKernelPT32> kernel;

    // documents the difference from WAIT_ORDER_PRIORITY_PACKED: the whole weight is compared, so with the same
    // priority the larger threshold (the low bits) wins
    SelWaiters w;
    w.Add(Pack(5, 5));
    w.Add(Pack(5, 9));

    CHECK_SELECTED(1, w, WAIT_ORDER_PRIORITY);
    CHECK_SELECTED(0, w, WAIT_ORDER_PRIORITY_PACKED);
}

// ---------------------------------------------------------------------------- //
// Tests: WAIT_ORDER_PRIORITY_PACKED (weighted kernel)
// ---------------------------------------------------------------------------- //

TEST(SyncWaitOrder, PriorityPacked_ThresholdDoesNotRank)
{
    SelKernelScope<SelKernelPT32> kernel;
    SelWaiters w; // the same priority, the second one has the larger threshold
    w.Add(Pack(5, 5));
    w.Add(Pack(5, 9));
    w.Add(Pack(5, 7));

    CHECK_SELECTED(0, w, WAIT_ORDER_PRIORITY_PACKED); // arrival order
}

TEST(SyncWaitOrder, PriorityPacked_PriorityBeatsThreshold)
{
    SelKernelScope<SelKernelPT32> kernel;
    SelWaiters w;
    w.Add(Pack(3, 31)); // lower priority, large threshold
    w.Add(Pack(4, 4));  // higher priority

    CHECK_SELECTED(1, w, WAIT_ORDER_PRIORITY_PACKED);
}

TEST(SyncWaitOrder, PriorityPacked_TopBitPriorityIsNotNegative)
{
    SelKernelScope<SelKernelPT32> kernel;

    // Pack(0x8000, 0) has the sign bit set: compared as a signed weight it would be the lowest, as a priority it is the highest
    SelWaiters w;
    w.Add(Pack(0x7FFF, 0));
    w.Add(Pack(0x8000, 0));

    CHECK_SELECTED(1, w, WAIT_ORDER_PRIORITY_PACKED);
    CHECK_SELECTED(0, w, WAIT_ORDER_PRIORITY); // the raw (signed) comparison picks the other one
}

TEST(SyncWaitOrder, PriorityPacked_HonorsWeightChangedWhileWaiting)
{
    SelKernelScope<SelKernelPT32> kernel;
    SelWaiters w;
    w.Add(Pack(2, 2));
    w.Add(Pack(1, 1));

    CHECK_SELECTED(0, w, WAIT_ORDER_PRIORITY_PACKED);

    w.Task(1).m_weight = Pack(6, 1); // inherited priority
    CHECK_SELECTED(1, w, WAIT_ORDER_PRIORITY_PACKED);
}

// ---------------------------------------------------------------------------- //
// Tests: WAIT_ORDER_DEADLINE (remaining deadline, HRT kernel)
// ---------------------------------------------------------------------------- //

TEST(SyncWaitOrder, Deadline_LeastRemainingWinsAnyPosition)
{
    SelKernelScope<SelKernelEDF> kernel;

    {
        SelWaiters w;
        w.Add(0, 2); w.Add(0, 9); w.Add(0, 7);
        CHECK_SELECTED(0, w, WAIT_ORDER_DEADLINE);
    }
    {
        SelWaiters w;
        w.Add(0, 10); w.Add(0, 3); w.Add(0, 7);
        CHECK_SELECTED(1, w, WAIT_ORDER_DEADLINE);
    }
    {
        SelWaiters w;
        w.Add(0, 10); w.Add(0, 9); w.Add(0, 1);
        CHECK_SELECTED(2, w, WAIT_ORDER_DEADLINE);
    }
}

TEST(SyncWaitOrder, Deadline_MissedDeadlineIsTheMostUrgent)
{
    SelKernelScope<SelKernelEDF> kernel;
    SelWaiters w;
    w.Add(0, 5);
    w.Add(0, -2); // the deadline is already missed
    w.Add(0, 0);

    CHECK_SELECTED(1, w, WAIT_ORDER_DEADLINE);
}

TEST(SyncWaitOrder, Deadline_EqualDeadlinesKeepArrivalOrder)
{
    SelKernelScope<SelKernelEDF> kernel;

    {
        SelWaiters w;
        w.Add(0, 4); w.Add(0, 2); w.Add(0, 2); w.Add(0, 9);
        CHECK_SELECTED(1, w, WAIT_ORDER_DEADLINE);
    }
    {
        SelWaiters w;
        w.Add(0, 3); w.Add(0, 3); w.Add(0, 3);
        CHECK_SELECTED(0, w, WAIT_ORDER_DEADLINE);
    }
}

TEST(SyncWaitOrder, Deadline_HonorsRemainingDeadlineChangedWhileWaiting)
{
    SelKernelScope<SelKernelEDF> kernel;
    SelWaiters w;
    w.Add(0, 4); w.Add(0, 8);

    CHECK_SELECTED(0, w, WAIT_ORDER_DEADLINE);

    w.Task(1).m_rel_deadline = 1; // the dynamic key is sampled when a waiter is selected, not when it starts waiting
    CHECK_SELECTED(1, w, WAIT_ORDER_DEADLINE);
}

// ---------------------------------------------------------------------------- //
// Tests: WAIT_ORDER_PERIOD and WAIT_ORDER_DEADLINE_FIXED (static ranks of RM and DM, HRT kernel)
// ---------------------------------------------------------------------------- //

TEST(SyncWaitOrder, Period_ShortestPeriodWins)
{
    SelKernelScope<SelKernelRM> kernel;
    SelWaiters w;
    w.Add(0, 0, 100); w.Add(0, 0, 20); w.Add(0, 0, 50);

    CHECK_SELECTED(1, w, WAIT_ORDER_PERIOD);
}

TEST(SyncWaitOrder, Period_EqualPeriodsKeepArrivalOrder)
{
    SelKernelScope<SelKernelRM> kernel;
    SelWaiters w;
    w.Add(0, 0, 30); w.Add(0, 0, 10); w.Add(0, 0, 10); w.Add(0, 0, 40);

    CHECK_SELECTED(1, w, WAIT_ORDER_PERIOD);
}

TEST(SyncWaitOrder, Period_IgnoresTheOtherKeys)
{
    SelKernelScope<SelKernelRM> kernel;
    SelWaiters w; // weight, remaining and configured deadline all point to the first waiter
    w.Add(9, 1, 100, 1);
    w.Add(1, 9, 10, 9);

    CHECK_SELECTED(1, w, WAIT_ORDER_PERIOD);
}

TEST(SyncWaitOrder, DeadlineFixed_ShortestConfiguredDeadlineWins)
{
    SelKernelScope<SelKernelDM> kernel;
    SelWaiters w;
    w.Add(0, 0, 0, 10); w.Add(0, 0, 0, 5); w.Add(0, 0, 0, 8);

    CHECK_SELECTED(1, w, WAIT_ORDER_DEADLINE_FIXED);
}

TEST(SyncWaitOrder, DeadlineFixed_EqualDeadlinesKeepArrivalOrder)
{
    SelKernelScope<SelKernelDM> kernel;
    SelWaiters w;
    w.Add(0, 0, 0, 6); w.Add(0, 0, 0, 3); w.Add(0, 0, 0, 3);

    CHECK_SELECTED(1, w, WAIT_ORDER_DEADLINE_FIXED);
}

TEST(SyncWaitOrder, DeadlineFixed_IsNotTheRemainingDeadline)
{
    SelKernelScope<SelKernelDM> kernel;
    SelWaiters w; // the second waiter has the shorter configured deadline but a lot of time left
    w.Add(0, 1, 0, 10);
    w.Add(0, 100, 0, 5);

    CHECK_SELECTED(0, w, WAIT_ORDER_DEADLINE);       // dynamic: least time left
    CHECK_SELECTED(1, w, WAIT_ORDER_DEADLINE_FIXED); // static: the shortest configured deadline
}

// ---------------------------------------------------------------------------- //
// Tests: removal and draining
// ---------------------------------------------------------------------------- //

TEST(SyncWaitOrder, Priority_DrainOrderIsStableForTies)
{
    SelKernelScope<SelKernelFP32> kernel;
    SelWaiters w;
    w.Add(2); w.Add(5); w.Add(5); w.Add(1); w.Add(5); w.Add(3);

    static const size_t expected[] = { 1, 2, 4, 5, 0, 3 };

    for (size_t i = 0; i < STK_STATIC_ARRAY_SIZE(expected); ++i)
    {
        const size_t selected = w.IndexOf(w.Select(WAIT_ORDER_PRIORITY));
        CHECK_EQUAL(expected[i], selected);
        w.Remove(selected);
    }

    CHECK_SELECTED(SEL_NONE, w, WAIT_ORDER_PRIORITY);
}

TEST(SyncWaitOrder, Deadline_DrainOrderIsStableForTies)
{
    SelKernelScope<SelKernelEDF> kernel;
    SelWaiters w;
    w.Add(0, 4); w.Add(0, 1); w.Add(0, 1); w.Add(0, 3); w.Add(0, 0); w.Add(0, 1);

    static const size_t expected[] = { 4, 1, 2, 5, 3, 0 };

    for (size_t i = 0; i < STK_STATIC_ARRAY_SIZE(expected); ++i)
    {
        const size_t selected = w.IndexOf(w.Select(WAIT_ORDER_DEADLINE));
        CHECK_EQUAL(expected[i], selected);
        w.Remove(selected);
    }

    CHECK_SELECTED(SEL_NONE, w, WAIT_ORDER_DEADLINE);
}

TEST(SyncWaitOrder, SelectionAfterRemovalOfTheHeadMiddleAndTail)
{
    SelKernelScope<SelKernelFP32> kernel;
    SelWaiters w;
    w.Add(1); w.Add(4); w.Add(2); w.Add(3);

    CHECK_SELECTED(1, w, WAIT_ORDER_PRIORITY);

    w.Remove(1); // the best one leaves (timeout / cancel): the next best takes over
    CHECK_SELECTED(3, w, WAIT_ORDER_PRIORITY);

    w.Remove(0); // the head leaves
    CHECK_SELECTED(3, w, WAIT_ORDER_PRIORITY);

    w.Remove(3); // the tail leaves
    CHECK_SELECTED(2, w, WAIT_ORDER_PRIORITY);

    w.Remove(2);
    CHECK_SELECTED(SEL_NONE, w, WAIT_ORDER_PRIORITY);
}

// ---------------------------------------------------------------------------- //
// Tests: WAIT_ORDER_AUTO (resolved by the kernel from the strategy hint)
// ---------------------------------------------------------------------------- //

TEST(SyncWaitOrder, Auto_IsResolvedToTheStrategyHint)
{
    // winners of AddDistinctWinners(): FIFO 0, PRIORITY 1, DEADLINE 2, PRIORITY_PACKED 3, PERIOD 4, DEADLINE_FIXED 5
    CheckAutoIsResolvedToStrategyHint<SelKernelRR>(WAIT_ORDER_FIFO, 0);
    CheckAutoIsResolvedToStrategyHint<SelKernelFP32>(WAIT_ORDER_PRIORITY, 1);
    CheckAutoIsResolvedToStrategyHint<SelKernelPT32>(WAIT_ORDER_PRIORITY_PACKED, 3);
    CheckAutoIsResolvedToStrategyHint<SelKernelEDF>(WAIT_ORDER_DEADLINE, 2);
    CheckAutoIsResolvedToStrategyHint<SelKernelRM>(WAIT_ORDER_PERIOD, 4);
    CheckAutoIsResolvedToStrategyHint<SelKernelDM>(WAIT_ORDER_DEADLINE_FIXED, 5);
}

TEST(SyncWaitOrder, Auto_IsNotResolvedWhenThereIsNothingToChooseFrom)
{
    SelKernelScope<SelKernelFP32> kernel;
    SelWaiters w;

    CHECK_SELECTED(SEL_NONE, w, WAIT_ORDER_AUTO);

    w.Add(1);
    w.ResetReads();
    CHECK_SELECTED(0, w, WAIT_ORDER_AUTO);
    CHECK_EQUAL(0, w.TotalReads());

    w.Add(5);
    CHECK_SELECTED(1, w, WAIT_ORDER_AUTO);
}

TEST(SyncWaitOrder, ExplicitOrderIsNotAffectedByTheKernelDefault)
{
    // the kernel default (FP: priority) is used for AUTO only
    SelKernelScope<SelKernelFP32> kernel;
    SelWaiters w;
    AddDistinctWinners(w);

    CHECK_SELECTED(1, w, WAIT_ORDER_AUTO);
    CHECK_SELECTED(0, w, WAIT_ORDER_FIFO);
    CHECK_SELECTED(3, w, WAIT_ORDER_PRIORITY_PACKED);
}

// ---------------------------------------------------------------------------- //
// Tests: SyncObjectBase
// ---------------------------------------------------------------------------- //

TEST(SyncWaitOrder, SyncObject_WaitListStaysInArrivalOrder)
{
    SelKernelScope<SelKernelPT32> kernel;
    SelSyncObject sobj(WAIT_ORDER_PRIORITY);
    SelWaiters w(&sobj);
    AddDistinctWinners(w);

    // the policy is applied when a waiter is selected, a waiter is always appended
    const IWaitObject *waiter = detail::DListCast::ListEntryToParent<const IWaitObject>(sobj.GetWaitList().GetFirst());

    for (size_t i = 0; i < SEL_DISTINCT_COUNT; ++i)
    {
        CHECK_TRUE(waiter != NULL);
        CHECK_EQUAL(i, w.IndexOf(waiter));

        waiter = detail::DListCast::ListEntryToParent<const IWaitObject>(waiter->GetNext());
    }

    POINTERS_EQUAL(NULL, waiter);
}

TEST(SyncWaitOrder, SyncObject_GetNextWaiterFollowsTheWaitOrder)
{
    CheckSyncObjectFollowsTheWaitOrder<SelKernelPT32>(SEL_CAPS_WEIGHTED);
    CheckSyncObjectFollowsTheWaitOrder<SelKernelEDF>(SEL_CAPS_HRT);
    CheckSyncObjectFollowsTheWaitOrder<SelKernelRR>(SEL_CAPS_PLAIN);
}

TEST(SyncWaitOrder, SyncObject_NoWaitersNoNextWaiter)
{
    SelKernelScope<SelKernelFP32> kernel;
    SelSyncObject sobj(WAIT_ORDER_PRIORITY);

    CHECK_EQUAL(TID_NONE, sobj.GetNextWaiter());
    POINTERS_EQUAL(NULL, sobj.Next());
}

TEST(SyncWaitOrder, SyncObject_SetWaitOrderTakesEffectWithWaitersQueued)
{
    CheckSetWaitOrderTakesEffectWithWaitersQueued<SelKernelPT32>(SEL_CAPS_WEIGHTED);
    CheckSetWaitOrderTakesEffectWithWaitersQueued<SelKernelEDF>(SEL_CAPS_HRT);
}

TEST(SyncWaitOrder, SyncObject_WakeOneWakesTheSelectedWaiter)
{
    CheckWakeOneWakesTheSelectedWaiter<SelKernelPT32>(SEL_CAPS_WEIGHTED);
    CheckWakeOneWakesTheSelectedWaiter<SelKernelEDF>(SEL_CAPS_HRT);
    CheckWakeOneWakesTheSelectedWaiter<SelKernelRR>(SEL_CAPS_PLAIN);
}

TEST(SyncWaitOrder, SyncObject_WakeOneWithNobodyWaitingIsHarmless)
{
    SelKernelScope<SelKernelFP32> kernel;
    SelSyncObject sobj(WAIT_ORDER_PRIORITY);

    sobj.WakeNext();

    CHECK_EQUAL(0, sobj.GetWaitList().GetSize());
}

TEST(SyncWaitOrder, SyncObject_WakeOneKeepsTheDefaultFifoForExistingCallers)
{
    // ISyncObject::WakeOne(wlist) without an order (e.g. ARM TrustZone wrappers) stays FIFO
    SelKernelScope<SelKernelFP32> kernel;
    SelWaiters w;
    AddDistinctWinners(w);

    ISyncObject::WakeOne(w.List());

    for (size_t k = 0; k < SEL_DISTINCT_COUNT; ++k)
        CHECK_EQUAL((k == 0) ? 1 : 0, w.Waiter(k).m_wake_count);
}

// ---------------------------------------------------------------------------- //
// Tests: wait order declared by the scheduling strategies (EConfig::WAIT_ORDER_HINT)
// ---------------------------------------------------------------------------- //

namespace {

// strategy which predates the hint (e.g. user-defined)
struct SelStrategyWithoutHint
{
    enum EConfig { WEIGHT_API = 0 };
};

} // namespace

TEST(SyncWaitOrder, StrategyHint_RoundRobinIsFifo)
{
    CHECK_EQUAL(WAIT_ORDER_FIFO, detail::StrategyWaitOrderHint<sched::SwitchStrategyRR>::Get());
    CHECK_EQUAL(WAIT_ORDER_FIFO, detail::StrategyWaitOrderHint<sched::SwitchStrategyRR_NSY>::Get());
}

TEST(SyncWaitOrder, StrategyHint_SmoothWeightedRoundRobinIsFifo)
{
    // a weight is a CPU share there, not a priority
    CHECK_EQUAL(WAIT_ORDER_FIFO, detail::StrategyWaitOrderHint<sched::SwitchStrategySWRR>::Get());
    CHECK_EQUAL(WAIT_ORDER_FIFO, detail::StrategyWaitOrderHint<sched::SwitchStrategySWRR_NSY>::Get());
}

TEST(SyncWaitOrder, StrategyHint_FixedPriorityIsPriority)
{
    CHECK_EQUAL(WAIT_ORDER_PRIORITY, detail::StrategyWaitOrderHint<sched::SwitchStrategyFP32>::Get());
    CHECK_EQUAL(WAIT_ORDER_PRIORITY, detail::StrategyWaitOrderHint<sched::SwitchStrategyFP64_NSY>::Get());
}

TEST(SyncWaitOrder, StrategyHint_PreemptionThresholdIsPackedPriority)
{
    CHECK_EQUAL(WAIT_ORDER_PRIORITY_PACKED, detail::StrategyWaitOrderHint<sched::SwitchStrategyPT32>::Get());
    CHECK_EQUAL(WAIT_ORDER_PRIORITY_PACKED, detail::StrategyWaitOrderHint<sched::SwitchStrategyPT64_TS>::Get());
}

TEST(SyncWaitOrder, StrategyHint_EdfIsDeadline)
{
    CHECK_EQUAL(WAIT_ORDER_DEADLINE, detail::StrategyWaitOrderHint<sched::SwitchStrategyEDF>::Get());
}

TEST(SyncWaitOrder, StrategyHint_MonotonicFollowsItsRank)
{
    CHECK_EQUAL(WAIT_ORDER_PERIOD, detail::StrategyWaitOrderHint<sched::SwitchStrategyRM>::Get());
    CHECK_EQUAL(WAIT_ORDER_DEADLINE_FIXED, detail::StrategyWaitOrderHint<sched::SwitchStrategyDM>::Get());
}

TEST(SyncWaitOrder, StrategyHint_AbsentHintGivesAuto)
{
    CHECK_EQUAL(WAIT_ORDER_AUTO, detail::StrategyWaitOrderHint<SelStrategyWithoutHint>::Get());
}

#if CPPUTEST_USE_NEW_MACROS
#include "CppUTest/MemoryLeakDetectorNewMacros.h"
#endif

} // namespace stk
} // namespace test
