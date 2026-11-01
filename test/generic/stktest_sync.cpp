/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <new> // placement new

#include "stktest.h"

namespace stk {
namespace test {

// ============================================================================ //
// ================================ Sync ====================================== //
// ============================================================================ //

TEST_GROUP(Sync)
{
    void setup() {}
    void teardown() {}
};

TEST(Sync, CriticalSection)
{
    CHECK_EQUAL(0, test::g_CriticalSectionState);
    {
        sync::ScopedCriticalSection cs;
        CHECK_EQUAL(1, test::g_CriticalSectionState);
    }
    CHECK_EQUAL(0, test::g_CriticalSectionState);
}

class ScopedCriticalSectionMock : public sync::ScopedCriticalSection
{
public:
    void Unlock() { return sync::ScopedCriticalSection::Unlock(); }
    bool TryLock() { return sync::ScopedCriticalSection::TryLock(); }
};

TEST(Sync, CriticalSectionTryLock)
{
    CHECK_EQUAL(0, test::g_CriticalSectionState);
    {
        ScopedCriticalSectionMock cs;

        cs.Unlock();
        CHECK_EQUAL(0, test::g_CriticalSectionState);

        CHECK_EQUAL(true, cs.TryLock());
        CHECK_EQUAL(1, test::g_CriticalSectionState);
    }
    CHECK_EQUAL(0, test::g_CriticalSectionState);
}

TEST(Sync, IMutex_ScopedLock)
{
    MutexMock mutex;
    {
        MutexMock::ScopedLock guard(mutex);
        CHECK_TRUE(mutex.m_locked);
    }
    CHECK_FALSE(mutex.m_locked);
}

static struct SyncWaitWakeRelaxCpuContext
{
    SyncWaitWakeRelaxCpuContext()
    {
        wake_all = false;
        counter  = 0;
        platform = NULL;
        sobj     = NULL;
    }

    bool              wake_all;
    uint32_t          counter;
    PlatformTestMock *platform;
    SyncObjectMock   *sobj;

    void Process()
    {
        platform->ProcessTick();
        ++counter;

        if (!wake_all && (counter >= 5))
            sobj->WakeOne();
        else
        if (wake_all && (counter >= 7))
            sobj->WakeAll();
    }
}
g_SyncWaitWakeRelaxCpuContext;

static void SyncWaitWakeRelaxCpu()
{
    g_SyncWaitWakeRelaxCpuContext.Process();
}

TEST(Sync, SyncWait_Wake)
{
    Kernel<KERNEL_STATIC | KERNEL_SYNC, 1, sched::SwitchStrategyRR, PlatformTestMock> kernel;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    TaskMock<ACCESS_USER> task;

    MutexMock mutex;
    SyncObjectMock sobj;

    g_SyncWaitWakeRelaxCpuContext.platform = platform;
    g_SyncWaitWakeRelaxCpuContext.sobj     = &sobj;
    g_RelaxCpuHandler = SyncWaitWakeRelaxCpu;

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    MutexMock::ScopedLock guard(mutex);

    // short wait in order to get wait object for a Wake testing
    EWaitResult wresult = IKernelService::GetInstance()->Wait(&sobj, &mutex, 10);

    // woken by WakeOne() on 5th tick, became active on 6th (see SyncWaitWakeRelaxCpuContext::Process)
    CHECK_EQUAL(6, g_SyncWaitWakeRelaxCpuContext.counter);
    CHECK_EQUAL(WAIT_RESULT_SIGNAL, wresult); // expect no timeout
    CHECK_EQUAL(true, mutex.m_locked); // expect locked mutex after Wait return

    g_SyncWaitWakeRelaxCpuContext.counter  = 0;
    g_SyncWaitWakeRelaxCpuContext.wake_all = true;

    // repeat for WakeAll()
    wresult = IKernelService::GetInstance()->Wait(&sobj, &mutex, 10);

    // woken by WakeAll() on 7th tick, became active on 8th (see SyncWaitWakeRelaxCpuContext::Process)
    CHECK_EQUAL(8, g_SyncWaitWakeRelaxCpuContext.counter);
    CHECK_EQUAL(WAIT_RESULT_SIGNAL, wresult); // expect no timeout
    CHECK_EQUAL(true, mutex.m_locked); // expect locked mutex after Wait return
}

// ============================================================================ //
// ============ SyncObjectBase destruction vs. the kernel's tick list ========= //
// ============================================================================ //

#if CPPUTEST_USE_NEW_MACROS
#undef new
#endif

typedef Kernel<KERNEL_STATIC | KERNEL_SYNC, 1, sched::SwitchStrategyRR, PlatformTestMock> SyncTickListKernelBase;

// exposes the list of sync objects the kernel calls Tick() on every tick (see Kernel::UpdateSyncObjects)
struct SyncTickListKernel : public SyncTickListKernelBase
{
    size_t GetTickListSize() { return m_sync_list->GetSize(); }
};

// minimal concrete SyncObjectBase (its constructor is protected, the rest of ISyncObject is implemented by it)
struct SyncObjectBaseStub final : public SyncObjectBase
{
    explicit SyncObjectBaseStub() : SyncObjectBase()
    {}

    STK_VIRT_DTOR ~SyncObjectBaseStub() = default;
};

static struct SyncDestroyContext
{
    SyncDestroyContext()
    {
        Reset();
    }

    void Reset()
    {
        counter          = 0;
        destroy_on_wake  = false;
        destroyed        = false;
        tick_list_before = 0;
        tick_list_after  = 0;
        platform         = NULL;
        kernel           = NULL;
        sobj             = NULL;
    }

    uint32_t            counter;
    bool                destroy_on_wake;  // destroy the object right after waking its waiter, before the next tick
    bool                destroyed;
    size_t              tick_list_before; // kernel tick list size just before the waiter was woken
    size_t              tick_list_after;  // kernel tick list size right after the object was destroyed
    PlatformTestMock   *platform;
    SyncTickListKernel *kernel;
    SyncObjectBaseStub *sobj;

    void Process()
    {
        platform->ProcessTick();
        ++counter;

        if (counter == 5)
        {
            tick_list_before = kernel->GetTickListSize();

            // wake the waiter: it is removed from the wait list immediately, but the object stays in
            // the kernel's tick list until the next tick unlinks it (Tick() returns false)
            IKernelService::GetInstance()->Wake(sobj, false);

            if (destroy_on_wake)
            {
                // destroy within the same tick interval: no tick has run since Wake()
                sobj->~SyncObjectBaseStub();
                destroyed       = true;
                tick_list_after = kernel->GetTickListSize();
            }
        }
    }
}
g_SyncDestroyContext;

static void SyncDestroyRelaxCpu()
{
    g_SyncDestroyContext.Process();
}

TEST(Sync, SyncObjectBase_DestroyAfterWakeBeforeTick)
{
    SyncTickListKernel kernel;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    TaskMock<ACCESS_USER> task;
    MutexMock mutex;

    alignas(SyncObjectBaseStub) uint8_t storage[sizeof(SyncObjectBaseStub)];
    SyncObjectBaseStub *sobj = ::new (storage) SyncObjectBaseStub();

    g_SyncDestroyContext.Reset();
    g_SyncDestroyContext.platform        = platform;
    g_SyncDestroyContext.kernel          = &kernel;
    g_SyncDestroyContext.sobj            = sobj;
    g_SyncDestroyContext.destroy_on_wake = true;

    decltype(g_RelaxCpuHandler) prev_handler = g_RelaxCpuHandler;
    g_RelaxCpuHandler = SyncDestroyRelaxCpu;

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    MutexMock::ScopedLock guard(mutex);

    // short wait in order to get the object linked into the kernel's tick list
    const EWaitResult wresult = IKernelService::GetInstance()->Wait(sobj, &mutex, 10);

    g_RelaxCpuHandler = prev_handler;

    // woken on 5th tick, became active on 6th (see SyncDestroyContext::Process)
    CHECK_EQUAL(6, g_SyncDestroyContext.counter);
    CHECK_EQUAL(WAIT_RESULT_SIGNAL, wresult);
    CHECK_TRUE(g_SyncDestroyContext.destroyed);

    // the kernel linked the object into its tick list when the wait started
    CHECK_EQUAL(1, g_SyncDestroyContext.tick_list_before);

    // regression: the destructor must leave the tick list, otherwise the next tick calls Tick() on a destroyed object
    CHECK_EQUAL(0, g_SyncDestroyContext.tick_list_after);
    CHECK_EQUAL(0, kernel.GetTickListSize());
}

TEST(Sync, SyncObjectBase_DestroyAfterWaiterResumed)
{
    SyncTickListKernel kernel;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    TaskMock<ACCESS_USER> task;
    MutexMock mutex;

    alignas(SyncObjectBaseStub) uint8_t storage[sizeof(SyncObjectBaseStub)];
    SyncObjectBaseStub *sobj = ::new (storage) SyncObjectBaseStub();

    g_SyncDestroyContext.Reset();
    g_SyncDestroyContext.platform        = platform;
    g_SyncDestroyContext.kernel          = &kernel;
    g_SyncDestroyContext.sobj            = sobj;
    g_SyncDestroyContext.destroy_on_wake = false;

    decltype(g_RelaxCpuHandler) prev_handler = g_RelaxCpuHandler;
    g_RelaxCpuHandler = SyncDestroyRelaxCpu;

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    MutexMock::ScopedLock guard(mutex);

    const EWaitResult wresult = IKernelService::GetInstance()->Wait(sobj, &mutex, 10);

    g_RelaxCpuHandler = prev_handler;

    CHECK_EQUAL(6, g_SyncDestroyContext.counter);
    CHECK_EQUAL(WAIT_RESULT_SIGNAL, wresult);
    CHECK_EQUAL(1, g_SyncDestroyContext.tick_list_before);

    // the tick which resumed the waiter has already unlinked the object (its wait list was empty)
    CHECK_EQUAL(0, kernel.GetTickListSize());
    CHECK_FALSE(sobj->IsLinked());

    // destroying it now (e.g. by the waiter itself) must keep the list consistent
    sobj->~SyncObjectBaseStub();
    CHECK_EQUAL(0, kernel.GetTickListSize());
}

TEST(Sync, SyncObjectBase_DestroyNotLinked)
{
    CHECK_EQUAL(0, test::g_CriticalSectionState);
    {
        // never waited on: not linked into any list, the destructor must be a harmless no-op
        SyncObjectBaseStub sobj;
        CHECK_FALSE(sobj.IsLinked());
    }
    CHECK_EQUAL(0, test::g_CriticalSectionState); // critical section is balanced
}

// ============================================================================ //
// ======== ISyncObject::GetWaitList().GetSize() / ISyncObject::GetFirstWaiterTid() ===== //
// ============================================================================ //

// Wait object which is not bound to a task: allows to fill a wait list with waiters of chosen ids
// without a kernel. Wake() mimics the real wait object, which removes itself from the wait list.
struct WaitObjectStub final : public IWaitObject
{
    explicit WaitObjectStub(TId tid) : m_tid(tid), m_sobj(NULL), m_wake_count(0)
    {}

    TId GetTid() const override { return m_tid; }

    void Wake(bool) override
    {
        ++m_wake_count;

        if (m_sobj != NULL)
        {
            m_sobj->RemoveWaitObject(this);
            m_sobj = NULL;
        }
    }

    EState GetState() const override { return STATE_WAIT; }

    bool Tick(Timeout) override { return false; }

    TId          m_tid;
    ISyncObject *m_sobj;
    uint32_t     m_wake_count;
};

// SyncObjectBase with helpers which maintain its wait list directly
struct WaiterListStub final : public SyncObjectBase
{
    explicit WaiterListStub() : SyncObjectBase()
    {}

    STK_VIRT_DTOR ~WaiterListStub() = default;

    void Add(WaitObjectStub &w)
    {
        AddWaitObject(&w);
        w.m_sobj = this;
    }

    void Remove(WaitObjectStub &w)
    {
        RemoveWaitObject(&w);
        w.m_sobj = NULL;
    }

    // wake the first waiter only (what WakeOne() does for the built-in primitives)
    void WakeFirst()
    {
        ISyncObject::WakeOne(m_wait_list);
    }

    void WakeEveryone()
    {
        ISyncObject::WakeAll(m_wait_list);
    }
};

static const TId TEST_TID_1 = 0x100;
static const TId TEST_TID_2 = 0x200;
static const TId TEST_TID_3 = 0x300;

TEST(Sync, WaiterIntrospection_Empty)
{
    WaiterListStub sobj;
    const ISyncObject &iface = sobj; // read-only introspection must be usable on a const interface

    CHECK_EQUAL(0, iface.GetWaitList().GetSize());
    CHECK_EQUAL(TID_NONE, iface.GetFirstWaiterTid());
}

TEST(Sync, WaiterIntrospection_SingleWaiter)
{
    WaiterListStub sobj;
    WaitObjectStub w1(TEST_TID_1);

    sobj.Add(w1);

    CHECK_EQUAL(1, sobj.GetWaitList().GetSize());
    CHECK_EQUAL(TEST_TID_1, sobj.GetFirstWaiterTid());

    sobj.Remove(w1);

    CHECK_EQUAL(0, sobj.GetWaitList().GetSize());
    CHECK_EQUAL(TID_NONE, sobj.GetFirstWaiterTid());
}

TEST(Sync, WaiterIntrospection_FifoOrder)
{
    WaiterListStub sobj;
    WaitObjectStub w1(TEST_TID_1), w2(TEST_TID_2), w3(TEST_TID_3);

    sobj.Add(w1);
    CHECK_EQUAL(1, sobj.GetWaitList().GetSize());
    CHECK_EQUAL(TEST_TID_1, sobj.GetFirstWaiterTid());

    // waiters are appended to the back: the first one stays the first one
    sobj.Add(w2);
    CHECK_EQUAL(2, sobj.GetWaitList().GetSize());
    CHECK_EQUAL(TEST_TID_1, sobj.GetFirstWaiterTid());

    sobj.Add(w3);
    CHECK_EQUAL(3, sobj.GetWaitList().GetSize());
    CHECK_EQUAL(TEST_TID_1, sobj.GetFirstWaiterTid());

    sobj.Remove(w1);
    sobj.Remove(w2);
    sobj.Remove(w3);
}

TEST(Sync, WaiterIntrospection_RemoveFirst)
{
    WaiterListStub sobj;
    WaitObjectStub w1(TEST_TID_1), w2(TEST_TID_2), w3(TEST_TID_3);

    sobj.Add(w1);
    sobj.Add(w2);
    sobj.Add(w3);

    // the next waiter in the FIFO order becomes the first
    sobj.Remove(w1);
    CHECK_EQUAL(2, sobj.GetWaitList().GetSize());
    CHECK_EQUAL(TEST_TID_2, sobj.GetFirstWaiterTid());

    sobj.Remove(w2);
    CHECK_EQUAL(1, sobj.GetWaitList().GetSize());
    CHECK_EQUAL(TEST_TID_3, sobj.GetFirstWaiterTid());

    sobj.Remove(w3);
    CHECK_EQUAL(0, sobj.GetWaitList().GetSize());
    CHECK_EQUAL(TID_NONE, sobj.GetFirstWaiterTid());
}

TEST(Sync, WaiterIntrospection_RemoveMiddleAndLast)
{
    WaiterListStub sobj;
    WaitObjectStub w1(TEST_TID_1), w2(TEST_TID_2), w3(TEST_TID_3);

    sobj.Add(w1);
    sobj.Add(w2);
    sobj.Add(w3);

    // removing a waiter from the middle or the tail (timeout, cancel) does not change the first one
    sobj.Remove(w2);
    CHECK_EQUAL(2, sobj.GetWaitList().GetSize());
    CHECK_EQUAL(TEST_TID_1, sobj.GetFirstWaiterTid());

    sobj.Remove(w3);
    CHECK_EQUAL(1, sobj.GetWaitList().GetSize());
    CHECK_EQUAL(TEST_TID_1, sobj.GetFirstWaiterTid());

    sobj.Remove(w1);
}

TEST(Sync, WaiterIntrospection_ReAddGoesToBack)
{
    WaiterListStub sobj;
    WaitObjectStub w1(TEST_TID_1), w2(TEST_TID_2);

    sobj.Add(w1);
    sobj.Add(w2);

    // w1 gives up the wait and starts waiting again: it queues behind w2
    sobj.Remove(w1);
    sobj.Add(w1);

    CHECK_EQUAL(2, sobj.GetWaitList().GetSize());
    CHECK_EQUAL(TEST_TID_2, sobj.GetFirstWaiterTid());

    sobj.Remove(w2);
    CHECK_EQUAL(TEST_TID_1, sobj.GetFirstWaiterTid());

    sobj.Remove(w1);
}

TEST(Sync, WaiterIntrospection_CountTracksAddedWaiters)
{
    WaiterListStub sobj;
    WaitObjectStub w1(TEST_TID_1), w2(TEST_TID_2), w3(TEST_TID_3);
    size_t expected = 0;

    // the wait list size follows an independent count of the waiters which were added and removed
    CHECK_EQUAL(expected, sobj.GetWaitList().GetSize());

    sobj.Add(w1); ++expected;
    CHECK_EQUAL(expected, sobj.GetWaitList().GetSize());

    sobj.Add(w2); ++expected;
    sobj.Add(w3); ++expected;
    CHECK_EQUAL(3, expected);
    CHECK_EQUAL(expected, sobj.GetWaitList().GetSize());

    sobj.Remove(w2); --expected;
    CHECK_EQUAL(expected, sobj.GetWaitList().GetSize());

    sobj.Remove(w1); --expected;
    sobj.Remove(w3); --expected;
    CHECK_EQUAL(0, expected);
    CHECK_EQUAL(expected, sobj.GetWaitList().GetSize());
}

TEST(Sync, WaiterIntrospection_FirstIsTheOneWokenNext)
{
    WaiterListStub sobj;
    WaitObjectStub w1(TEST_TID_1), w2(TEST_TID_2), w3(TEST_TID_3);

    sobj.Add(w1);
    sobj.Add(w2);
    sobj.Add(w3);

    // GetFirstWaiterTid() predicts the waiter which WakeOne() wakes next
    CHECK_EQUAL(TEST_TID_1, sobj.GetFirstWaiterTid());
    sobj.WakeFirst();
    CHECK_EQUAL(1, w1.m_wake_count);
    CHECK_EQUAL(0, w2.m_wake_count);
    CHECK_EQUAL(0, w3.m_wake_count);
    CHECK_EQUAL(2, sobj.GetWaitList().GetSize());

    CHECK_EQUAL(TEST_TID_2, sobj.GetFirstWaiterTid());
    sobj.WakeFirst();
    CHECK_EQUAL(1, w2.m_wake_count);
    CHECK_EQUAL(0, w3.m_wake_count);
    CHECK_EQUAL(1, sobj.GetWaitList().GetSize());

    CHECK_EQUAL(TEST_TID_3, sobj.GetFirstWaiterTid());
    sobj.WakeFirst();
    CHECK_EQUAL(1, w3.m_wake_count);
    CHECK_EQUAL(0, sobj.GetWaitList().GetSize());
    CHECK_EQUAL(TID_NONE, sobj.GetFirstWaiterTid());

    // nobody is waiting: harmless
    sobj.WakeFirst();
    CHECK_EQUAL(0, sobj.GetWaitList().GetSize());
}

TEST(Sync, WaiterIntrospection_WakeAll)
{
    WaiterListStub sobj;
    WaitObjectStub w1(TEST_TID_1), w2(TEST_TID_2), w3(TEST_TID_3);

    sobj.Add(w1);
    sobj.Add(w2);
    sobj.Add(w3);
    CHECK_EQUAL(3, sobj.GetWaitList().GetSize());

    sobj.WakeEveryone();

    CHECK_EQUAL(1, w1.m_wake_count);
    CHECK_EQUAL(1, w2.m_wake_count);
    CHECK_EQUAL(1, w3.m_wake_count);
    CHECK_EQUAL(0, sobj.GetWaitList().GetSize());
    CHECK_EQUAL(TID_NONE, sobj.GetFirstWaiterTid());
}

// ---------------------------------------------------------------------------- //
// Real waiter: a task blocked in IKernelService::Wait()
// ---------------------------------------------------------------------------- //

static struct WaiterIntrospectContext
{
    enum EEnd
    {
        END_WAKE = 0, // the wait is ended by IKernelService::Wake()
        END_TIMEOUT,  // the wait expires
        END_CANCEL    // the wait is ended by IKernel::CancelTaskWait()
    };

    WaiterIntrospectContext()
    {
        Reset();
    }

    void Reset()
    {
        counter          = 0;
        end              = END_WAKE;
        platform         = NULL;
        kernel           = NULL;
        task             = NULL;
        sobj             = NULL;
        count_waiting    = 0;
        tid_waiting      = TID_NONE;
        sampled_waiting  = false;
        count_after_wake = 0;
        tid_after_wake   = TID_NONE;
    }

    uint32_t            counter;
    EEnd                end;
    PlatformTestMock   *platform;
    IKernel            *kernel;
    ITask              *task;
    SyncObjectBaseStub *sobj;

    bool                sampled_waiting;  // the object was sampled while the task was blocked on it
    size_t              count_waiting;    // GetWaitList().GetSize() while the task is blocked
    TId                 tid_waiting;      // GetFirstWaiterTid() while the task is blocked
    size_t              count_after_wake; // GetWaitList().GetSize() right after Wake(), before the next tick
    TId                 tid_after_wake;   // GetFirstWaiterTid() right after Wake(), before the next tick

    void Process()
    {
        platform->ProcessTick();
        ++counter;

        if (counter == 2)
        {
            // the task has been blocked for 2 ticks
            count_waiting   = sobj->GetWaitList().GetSize();
            tid_waiting     = sobj->GetFirstWaiterTid();
            sampled_waiting = true;
        }
        else
        if (counter == 3)
        {
            if (end == END_WAKE)
            {
                IKernelService::GetInstance()->Wake(sobj, false);

                // Wake() removes the waiter from the wait list immediately, no tick is needed
                count_after_wake = sobj->GetWaitList().GetSize();
                tid_after_wake   = sobj->GetFirstWaiterTid();
            }
            else
            if (end == END_CANCEL)
            {
                kernel->CancelTaskWait(task);
            }
        }
    }
}
g_WaiterIntrospectContext;

static void WaiterIntrospectRelaxCpu()
{
    g_WaiterIntrospectContext.Process();
}

static void Test_WaiterIntrospection(WaiterIntrospectContext::EEnd end, const EWaitResult expected, Timeout wait_ticks)
{
    Kernel<KERNEL_STATIC | KERNEL_SYNC, 1, sched::SwitchStrategyRR, PlatformTestMock> kernel;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    TaskMock<ACCESS_USER> task;

    MutexMock mutex;
    SyncObjectBaseStub sobj; // destroyed before the kernel: leaves the kernel's tick list

    g_WaiterIntrospectContext.Reset();
    g_WaiterIntrospectContext.end      = end;
    g_WaiterIntrospectContext.platform = platform;
    g_WaiterIntrospectContext.kernel   = &kernel;
    g_WaiterIntrospectContext.task     = &task;
    g_WaiterIntrospectContext.sobj     = &sobj;

    decltype(g_RelaxCpuHandler) prev_handler = g_RelaxCpuHandler;
    g_RelaxCpuHandler = WaiterIntrospectRelaxCpu;

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    // nobody has waited yet
    CHECK_EQUAL(0, sobj.GetWaitList().GetSize());
    CHECK_EQUAL(TID_NONE, sobj.GetFirstWaiterTid());

    MutexMock::ScopedLock guard(mutex);

    const EWaitResult wresult = IKernelService::GetInstance()->Wait(&sobj, &mutex, wait_ticks);

    g_RelaxCpuHandler = prev_handler;

    CHECK_EQUAL(expected, wresult);

    // while it was blocked, the task was the only waiter and therefore the first one
    CHECK_TRUE(g_WaiterIntrospectContext.sampled_waiting);
    CHECK_EQUAL(1, g_WaiterIntrospectContext.count_waiting);
    CHECK_EQUAL(task.GetTid(), g_WaiterIntrospectContext.tid_waiting);

    if (end == WaiterIntrospectContext::END_WAKE)
    {
        CHECK_EQUAL(0, g_WaiterIntrospectContext.count_after_wake);
        CHECK_EQUAL(TID_NONE, g_WaiterIntrospectContext.tid_after_wake);
    }

    // however the wait ended, the waiter is gone from the wait list
    CHECK_EQUAL(0, sobj.GetWaitList().GetSize());
    CHECK_EQUAL(TID_NONE, sobj.GetFirstWaiterTid());
}

TEST(Sync, WaiterIntrospection_TaskWaitEndedByWake)
{
    Test_WaiterIntrospection(WaiterIntrospectContext::END_WAKE, WAIT_RESULT_SIGNAL, 10);
}

TEST(Sync, WaiterIntrospection_TaskWaitEndedByTimeout)
{
    Test_WaiterIntrospection(WaiterIntrospectContext::END_TIMEOUT, WAIT_RESULT_TIMEOUT, 4);
}

TEST(Sync, WaiterIntrospection_TaskWaitEndedByCancel)
{
    Test_WaiterIntrospection(WaiterIntrospectContext::END_CANCEL, WAIT_RESULT_CANCELED, 10);
}

#if CPPUTEST_USE_NEW_MACROS
#include "CppUTest/MemoryLeakDetectorNewMacros.h"
#endif

} // namespace stk
} // namespace test
