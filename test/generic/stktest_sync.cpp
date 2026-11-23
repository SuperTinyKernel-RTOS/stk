/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <new> // placement new
#include <string.h> // memset
#include <functional> // std::function

#include "stktest.h"

namespace stk {
namespace test {

// ============================================================================ //
// ================================ Sync ====================================== //
// ============================================================================ //

TEST_GROUP(Sync)
{
    void setup() {}
    void teardown()
    {
        g_TestContext.RethrowAssertException(true);
        g_TestContext.ExpectAssert(false);
        g_TestContext.ExpectPanic(false);
        test::g_PanicValue = KERNEL_PANIC_NONE;
    }
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
        {
            sobj->WakeOne();
        }
        else
        if (wake_all && (counter >= 7))
        {
            sobj->WakeAll();
        }
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

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    g_SyncWaitWakeRelaxCpuContext.platform = platform;
    g_SyncWaitWakeRelaxCpuContext.sobj     = &sobj;
    g_RelaxCpuHandler = SyncWaitWakeRelaxCpu;

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

TEST(Sync, SyncDefaultWaitOrder)
{
    SyncObjectMock sobj;

    CHECK_EQUAL(WAIT_ORDER_AUTO, sobj.GetWaitOrder());
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

// ================================================================================== //
// ======== ISyncObject::GetWaitList().GetSize() / ISyncObject::GetNextWaiter() ===== //
// ================================================================================== //

// Wait object which is not bound to a task: allows to fill a wait list with waiters of chosen ids
// without a kernel. Wake() mimics the real wait object, which removes itself from the wait list.
static uint32_t g_WaitObjectDeleteSeq = 0; // order in which the stubs were released by WakeDeleted()

struct WaitObjectStub final : public IWaitObject
{
    explicit WaitObjectStub(TId tid) : m_tid(tid), m_sobj(NULL), m_wake_count(0), m_delete_count(0), m_delete_seq(0),
        m_state(STATE_WAIT)
    {}

    TId GetTid() const override { return m_tid; }

    IKernelTask *GetKernelTask() const { return nullptr; }

    void Wake(bool) override
    {
        ++m_wake_count;

        if (m_sobj != NULL)
        {
            m_sobj->RemoveWaitObject(this);
            m_sobj = NULL;
        }
    }

    // mimics the real wait object: the wait list is left, the result is WAIT_RESULT_DELETED
    void WakeDeleted() override
    {
        ++m_delete_count;
        m_delete_seq = ++g_WaitObjectDeleteSeq;
        m_state      = STATE_DELETED;

        if (m_sobj != NULL)
        {
            m_sobj->RemoveWaitObject(this);
            m_sobj = NULL;
        }
    }

    EState GetState() const override { return m_state; }

    bool Tick(Timeout) override { return false; }

    TId          m_tid;
    ISyncObject *m_sobj;
    uint32_t     m_wake_count;
    uint32_t     m_delete_count; // number of WakeDeleted() calls
    uint32_t     m_delete_seq;   // 1-based position in the release order (0: not released)
    EState       m_state;
};

// SyncObjectBase with helpers which maintain its wait list directly
struct WaiterListStub final : public SyncObjectBase
{
    explicit WaiterListStub() : SyncObjectBase()
    {}

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

    // release everyone as if the object is being destroyed (what ~SyncObjectBase() does)
    void DeleteEveryone()
    {
        ISyncObject::DeleteAll(m_wait_list);
    }
};

static const TId TEST_TID_1 = 0x100;
static const TId TEST_TID_2 = 0x200;
static const TId TEST_TID_3 = 0x300;

TEST(Sync, WaiterIntrospection_Empty)
{
    // need existing kernel for stk::detail::ResolveWaitOrder to succeed
    Kernel<KERNEL_STATIC | KERNEL_SYNC, 1, sched::SwitchStrategyRR, PlatformTestMock> kernel;
    kernel.Initialize();

    WaiterListStub sobj;
    const ISyncObject &iface = sobj; // read-only introspection must be usable on a const interface

    CHECK_EQUAL(0, iface.GetWaitList().GetSize());
    CHECK_EQUAL(TID_NONE, iface.GetNextWaiter());
}

TEST(Sync, WaiterIntrospection_SingleWaiter)
{
    // need existing kernel for stk::detail::ResolveWaitOrder to succeed
    Kernel<KERNEL_STATIC | KERNEL_SYNC, 1, sched::SwitchStrategyRR, PlatformTestMock> kernel;
    kernel.Initialize();

    WaiterListStub sobj;
    WaitObjectStub w1(TEST_TID_1);

    sobj.Add(w1);

    CHECK_EQUAL(1, sobj.GetWaitList().GetSize());
    CHECK_EQUAL(TEST_TID_1, sobj.GetNextWaiter());

    sobj.Remove(w1);

    CHECK_EQUAL(0, sobj.GetWaitList().GetSize());
    CHECK_EQUAL(TID_NONE, sobj.GetNextWaiter());
}

TEST(Sync, WaiterIntrospection_FifoOrder)
{
    // need existing kernel for stk::detail::ResolveWaitOrder to succeed
    Kernel<KERNEL_STATIC | KERNEL_SYNC, 1, sched::SwitchStrategyRR, PlatformTestMock> kernel;
    kernel.Initialize();

    WaiterListStub sobj;
    WaitObjectStub w1(TEST_TID_1), w2(TEST_TID_2), w3(TEST_TID_3);

    sobj.Add(w1);
    CHECK_EQUAL(1, sobj.GetWaitList().GetSize());
    CHECK_EQUAL(TEST_TID_1, sobj.GetNextWaiter());

    // waiters are appended to the back: the first one stays the first one
    sobj.Add(w2);
    CHECK_EQUAL(2, sobj.GetWaitList().GetSize());
    CHECK_EQUAL(TEST_TID_1, sobj.GetNextWaiter());

    sobj.Add(w3);
    CHECK_EQUAL(3, sobj.GetWaitList().GetSize());
    CHECK_EQUAL(TEST_TID_1, sobj.GetNextWaiter());

    sobj.Remove(w1);
    sobj.Remove(w2);
    sobj.Remove(w3);
}

TEST(Sync, WaiterIntrospection_RemoveFirst)
{
    // need existing kernel for stk::detail::ResolveWaitOrder to succeed
    Kernel<KERNEL_STATIC | KERNEL_SYNC, 1, sched::SwitchStrategyRR, PlatformTestMock> kernel;
    kernel.Initialize();

    WaiterListStub sobj;
    WaitObjectStub w1(TEST_TID_1), w2(TEST_TID_2), w3(TEST_TID_3);

    sobj.Add(w1);
    sobj.Add(w2);
    sobj.Add(w3);

    // the next waiter in the FIFO order becomes the first
    sobj.Remove(w1);
    CHECK_EQUAL(2, sobj.GetWaitList().GetSize());
    CHECK_EQUAL(TEST_TID_2, sobj.GetNextWaiter());

    sobj.Remove(w2);
    CHECK_EQUAL(1, sobj.GetWaitList().GetSize());
    CHECK_EQUAL(TEST_TID_3, sobj.GetNextWaiter());

    sobj.Remove(w3);
    CHECK_EQUAL(0, sobj.GetWaitList().GetSize());
    CHECK_EQUAL(TID_NONE, sobj.GetNextWaiter());
}

TEST(Sync, WaiterIntrospection_RemoveMiddleAndLast)
{
    // need existing kernel for stk::detail::ResolveWaitOrder to succeed
    Kernel<KERNEL_STATIC | KERNEL_SYNC, 1, sched::SwitchStrategyRR, PlatformTestMock> kernel;
    kernel.Initialize();

    WaiterListStub sobj;
    WaitObjectStub w1(TEST_TID_1), w2(TEST_TID_2), w3(TEST_TID_3);

    sobj.Add(w1);
    sobj.Add(w2);
    sobj.Add(w3);

    // removing a waiter from the middle or the tail (timeout, cancel) does not change the first one
    sobj.Remove(w2);
    CHECK_EQUAL(2, sobj.GetWaitList().GetSize());
    CHECK_EQUAL(TEST_TID_1, sobj.GetNextWaiter());

    sobj.Remove(w3);
    CHECK_EQUAL(1, sobj.GetWaitList().GetSize());
    CHECK_EQUAL(TEST_TID_1, sobj.GetNextWaiter());

    sobj.Remove(w1);
}

TEST(Sync, WaiterIntrospection_ReAddGoesToBack)
{
    // need existing kernel for stk::detail::ResolveWaitOrder to succeed
    Kernel<KERNEL_STATIC | KERNEL_SYNC, 1, sched::SwitchStrategyRR, PlatformTestMock> kernel;
    kernel.Initialize();

    WaiterListStub sobj;
    WaitObjectStub w1(TEST_TID_1), w2(TEST_TID_2);

    sobj.Add(w1);
    sobj.Add(w2);

    // w1 gives up the wait and starts waiting again: it queues behind w2
    sobj.Remove(w1);
    sobj.Add(w1);

    CHECK_EQUAL(2, sobj.GetWaitList().GetSize());
    CHECK_EQUAL(TEST_TID_2, sobj.GetNextWaiter());

    sobj.Remove(w2);
    CHECK_EQUAL(TEST_TID_1, sobj.GetNextWaiter());

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
    // need existing kernel for stk::detail::ResolveWaitOrder to succeed
    Kernel<KERNEL_STATIC | KERNEL_SYNC, 1, sched::SwitchStrategyRR, PlatformTestMock> kernel;
    kernel.Initialize();

    WaiterListStub sobj;
    WaitObjectStub w1(TEST_TID_1), w2(TEST_TID_2), w3(TEST_TID_3);

    sobj.Add(w1);
    sobj.Add(w2);
    sobj.Add(w3);

    // GetNextWaiter() predicts the waiter which WakeOne() wakes next
    CHECK_EQUAL(TEST_TID_1, sobj.GetNextWaiter());
    sobj.WakeFirst();
    CHECK_EQUAL(1, w1.m_wake_count);
    CHECK_EQUAL(0, w2.m_wake_count);
    CHECK_EQUAL(0, w3.m_wake_count);
    CHECK_EQUAL(2, sobj.GetWaitList().GetSize());

    CHECK_EQUAL(TEST_TID_2, sobj.GetNextWaiter());
    sobj.WakeFirst();
    CHECK_EQUAL(1, w2.m_wake_count);
    CHECK_EQUAL(0, w3.m_wake_count);
    CHECK_EQUAL(1, sobj.GetWaitList().GetSize());

    CHECK_EQUAL(TEST_TID_3, sobj.GetNextWaiter());
    sobj.WakeFirst();
    CHECK_EQUAL(1, w3.m_wake_count);
    CHECK_EQUAL(0, sobj.GetWaitList().GetSize());
    CHECK_EQUAL(TID_NONE, sobj.GetNextWaiter());

    // nobody is waiting: harmless
    sobj.WakeFirst();
    CHECK_EQUAL(0, sobj.GetWaitList().GetSize());
}

TEST(Sync, WaiterIntrospection_WakeAll)
{
    // need existing kernel for stk::detail::ResolveWaitOrder to succeed
    Kernel<KERNEL_STATIC | KERNEL_SYNC, 1, sched::SwitchStrategyRR, PlatformTestMock> kernel;
    kernel.Initialize();

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
    CHECK_EQUAL(TID_NONE, sobj.GetNextWaiter());
}

// ---------------------------------------------------------------------------- //
// WakeDeleted(): release of the waiters of a sync object which is being destroyed
// ---------------------------------------------------------------------------- //

TEST(Sync, WakeDeleted_DeleteAllReleasesEveryoneInArrivalOrder)
{
    // need existing kernel for stk::detail::ResolveWaitOrder to succeed
    Kernel<KERNEL_STATIC | KERNEL_SYNC, 1, sched::SwitchStrategyRR, PlatformTestMock> kernel;
    kernel.Initialize();

    WaiterListStub sobj;
    WaitObjectStub w1(TEST_TID_1), w2(TEST_TID_2), w3(TEST_TID_3);

    g_WaitObjectDeleteSeq = 0;

    sobj.Add(w1);
    sobj.Add(w2);
    sobj.Add(w3);
    CHECK_EQUAL(3, sobj.GetWaitList().GetSize());

    sobj.DeleteEveryone();

    // everyone is released with the "deleted" result, none of them is signaled
    CHECK_EQUAL(1, w1.m_delete_count);
    CHECK_EQUAL(1, w2.m_delete_count);
    CHECK_EQUAL(1, w3.m_delete_count);
    CHECK_EQUAL(0, w1.m_wake_count);
    CHECK_EQUAL(0, w2.m_wake_count);
    CHECK_EQUAL(0, w3.m_wake_count);
    CHECK_EQUAL((int)IWaitObject::STATE_DELETED, (int)w1.GetState());
    CHECK_EQUAL((int)IWaitObject::STATE_DELETED, (int)w2.GetState());
    CHECK_EQUAL((int)IWaitObject::STATE_DELETED, (int)w3.GetState());

    // released in the arrival order, each one leaves the wait list
    CHECK_EQUAL(1, w1.m_delete_seq);
    CHECK_EQUAL(2, w2.m_delete_seq);
    CHECK_EQUAL(3, w3.m_delete_seq);
    CHECK_EQUAL(0, sobj.GetWaitList().GetSize());
    CHECK_EQUAL(TID_NONE, sobj.GetNextWaiter());
}

TEST(Sync, WakeDeleted_DeleteAllOnEmptyListIsHarmless)
{
    // need existing kernel for stk::detail::ResolveWaitOrder to succeed
    Kernel<KERNEL_STATIC | KERNEL_SYNC, 1, sched::SwitchStrategyRR, PlatformTestMock> kernel;
    kernel.Initialize();

    WaiterListStub sobj;
    WaitObjectStub w1(TEST_TID_1);

    sobj.DeleteEveryone();
    CHECK_EQUAL(0, sobj.GetWaitList().GetSize());

    // already released waiter is not released twice
    sobj.Add(w1);
    sobj.DeleteEveryone();
    sobj.DeleteEveryone();

    CHECK_EQUAL(1, w1.m_delete_count);
    CHECK_EQUAL(0, sobj.GetWaitList().GetSize());
}

TEST(Sync, WakeDeleted_DeleteAllDoesNotHandleOverToWake)
{
    // need existing kernel for stk::detail::ResolveWaitOrder to succeed
    Kernel<KERNEL_STATIC | KERNEL_SYNC, 1, sched::SwitchStrategyRR, PlatformTestMock> kernel;
    kernel.Initialize();

    WaiterListStub sobj;
    WaitObjectStub w1(TEST_TID_1), w2(TEST_TID_2);

    sobj.Add(w1);
    sobj.Add(w2);

    // WakeFirst() signals only the first waiter, DeleteAll() takes the rest: no one is released twice
    sobj.WakeFirst();
    sobj.DeleteEveryone();

    CHECK_EQUAL(1, w1.m_wake_count);
    CHECK_EQUAL(0, w1.m_delete_count);
    CHECK_EQUAL(0, w2.m_wake_count);
    CHECK_EQUAL(1, w2.m_delete_count);
    CHECK_EQUAL((int)IWaitObject::STATE_DELETED, (int)w2.GetState());
}

TEST(Sync, WakeDeleted_DestructorReleasesWaiters)
{
    alignas(WaiterListStub) uint8_t storage[sizeof(WaiterListStub)];
    WaiterListStub *sobj = ::new (storage) WaiterListStub();

    WaitObjectStub w1(TEST_TID_1), w2(TEST_TID_2);

    g_WaitObjectDeleteSeq = 0;

    sobj->Add(w1);
    sobj->Add(w2);

    // no kernel is needed: the wait objects are released directly (non-TrustZone build)
    CHECK_EQUAL(0, test::g_CriticalSectionState);
    sobj->~WaiterListStub();
    CHECK_EQUAL(0, test::g_CriticalSectionState); // critical section is balanced

    CHECK_EQUAL(1, w1.m_delete_count);
    CHECK_EQUAL(1, w2.m_delete_count);
    CHECK_EQUAL(0, w1.m_wake_count);
    CHECK_EQUAL(0, w2.m_wake_count);
    CHECK_EQUAL((int)IWaitObject::STATE_DELETED, (int)w1.GetState());
    CHECK_EQUAL((int)IWaitObject::STATE_DELETED, (int)w2.GetState());
    CHECK_EQUAL(1, w1.m_delete_seq);
    CHECK_EQUAL(2, w2.m_delete_seq);

    // the wait objects left the wait list of the object before it was gone
    CHECK_TRUE(w1.m_sobj == NULL);
    CHECK_TRUE(w2.m_sobj == NULL);
}

TEST(Sync, WakeDeleted_DestructorWithoutWaitersReleasesNobody)
{
    WaitObjectStub w1(TEST_TID_1);

    CHECK_EQUAL(0, test::g_CriticalSectionState);
    {
        WaiterListStub sobj;

        // a waiter which left before the destruction (e.g. woken) must not be touched
        sobj.Add(w1);
        sobj.Remove(w1);
    }
    CHECK_EQUAL(0, test::g_CriticalSectionState);

    CHECK_EQUAL(0, w1.m_delete_count);
    CHECK_EQUAL((int)IWaitObject::STATE_WAIT, (int)w1.GetState());
}

TEST(Sync, WakeDeleted_KernelServiceMockRecordsCall)
{
    KernelServiceMock service;
    SyncObjectMock sobj;

    CHECK_EQUAL(0, service.m_wake_deleted_nr);

    service.WakeDeleted(&sobj);

    CHECK_EQUAL(1, service.m_wake_deleted_nr);
    CHECK_TRUE(service.m_wake_deleted_sobj == &sobj);
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
    TId                 tid_waiting;      // GetNextWaiter() while the task is blocked
    size_t              count_after_wake; // GetWaitList().GetSize() right after Wake(), before the next tick
    TId                 tid_after_wake;   // GetNextWaiter() right after Wake(), before the next tick

    void Process()
    {
        platform->ProcessTick();
        ++counter;

        if (counter == 2)
        {
            // the task has been blocked for 2 ticks
            count_waiting   = sobj->GetWaitList().GetSize();
            tid_waiting     = sobj->GetNextWaiter();
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
                tid_after_wake   = sobj->GetNextWaiter();
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
    CHECK_EQUAL(TID_NONE, sobj.GetNextWaiter());

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
    CHECK_EQUAL(TID_NONE, sobj.GetNextWaiter());
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

// ============================================================================ //
// ==== Real waiter: sync object destroyed / released while a task waits ======= //
// ============================================================================ //

static struct SyncDeleteContext
{
    enum EAction
    {
        ACTION_DESTROY = 0,      // destroy the object (what the destructor does for its waiters)
        ACTION_SERVICE_WAKE_DEL  // IKernelService::WakeDeleted() on an object which stays alive
    };

    SyncDeleteContext()
    {
        Reset();
    }

    void Reset()
    {
        counter          = 0;
        at_tick          = 3;
        action           = ACTION_DESTROY;
        acted            = false;
        tick_list_before = 0;
        tick_list_after  = 0;
        waiters_before   = 0;
        waiters_after    = 0;
        platform         = NULL;
        kernel           = NULL;
        sobj             = NULL;
        obj              = NULL;
        destroy          = NULL;
    }

    uint32_t            counter;
    uint32_t            at_tick;          // tick on which the object is destroyed / released
    EAction             action;
    bool                acted;
    size_t              tick_list_before; // kernel tick list size just before the action
    size_t              tick_list_after;  // kernel tick list size right after the action
    size_t              waiters_before;   // wait list size just before the action (ACTION_SERVICE_WAKE_DEL)
    size_t              waiters_after;    // wait list size right after the action (ACTION_SERVICE_WAKE_DEL)
    PlatformTestMock   *platform;
    SyncTickListKernel *kernel;
    ISyncObject        *sobj;             // ACTION_SERVICE_WAKE_DEL
    void               *obj;              // ACTION_DESTROY
    void              (*destroy)(void *); // ACTION_DESTROY

    void Process()
    {
        platform->ProcessTick();
        ++counter;

        if ((counter == at_tick) && !acted)
        {
            tick_list_before = kernel->GetTickListSize();

            if (action == ACTION_DESTROY)
            {
                destroy(obj);
            }
            else
            {
                waiters_before = sobj->GetWaitList().GetSize();
                IKernelService::GetInstance()->WakeDeleted(sobj);
                waiters_after  = sobj->GetWaitList().GetSize();
            }

            acted           = true;
            tick_list_after = kernel->GetTickListSize();
        }
    }
}
g_SyncDeleteContext;

static void SyncDeleteRelaxCpu()
{
    g_SyncDeleteContext.Process();
}

// destroys the object and poisons its storage: any access to it afterwards (by the woken waiter or the kernel)
// reads garbage instead of the valid state
template <class T>
static void SyncDeleteDestroy(void *p)
{
    static_cast<T *>(p)->~T();
    memset(p, 0xDD, sizeof(T));
}

// kernel with one task which blocks on an object that is destroyed on the 3rd tick
struct SyncDeleteHarness
{
    SyncDeleteHarness() : prev_handler(g_RelaxCpuHandler)
    {
        g_SyncDeleteContext.Reset();
    }

    ~SyncDeleteHarness()
    {
        g_RelaxCpuHandler = prev_handler;
    }

    // call after the object is constructed, before the task blocks
    void Start(void *obj, void (*destroy)(void *))
    {
        g_SyncDeleteContext.platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
        g_SyncDeleteContext.kernel   = &kernel;
        g_SyncDeleteContext.obj      = obj;
        g_SyncDeleteContext.destroy  = destroy;

        g_RelaxCpuHandler = SyncDeleteRelaxCpu;

        kernel.Initialize();
        kernel.AddTask(&task);
        kernel.Start();
    }

    void Finish()
    {
        g_RelaxCpuHandler = prev_handler;
    }

    // common post-conditions: the object was destroyed with the waiter on it and the kernel forgot it
    void CheckDestroyed()
    {
        CHECK_TRUE(g_SyncDeleteContext.acted);
        CHECK_EQUAL(1, g_SyncDeleteContext.tick_list_before); // linked when the wait started
        CHECK_EQUAL(0, g_SyncDeleteContext.tick_list_after);  // the destructor left the tick list
        CHECK_EQUAL(0, kernel.GetTickListSize());
        CHECK_EQUAL(0, test::g_CriticalSectionState);
    }

    SyncTickListKernel    kernel;
    TaskMock<ACCESS_USER> task;
    decltype(g_RelaxCpuHandler) prev_handler;
};

static void Test_DestroyWithWaiter(Timeout wait_ticks)
{
    SyncDeleteHarness h;
    MutexMock mutex;

    alignas(SyncObjectBaseStub) uint8_t storage[sizeof(SyncObjectBaseStub)];
    SyncObjectBaseStub *sobj = ::new (storage) SyncObjectBaseStub();

    h.Start(sobj, SyncDeleteDestroy<SyncObjectBaseStub>);

    MutexMock::ScopedLock guard(mutex);

    const EWaitResult wresult = IKernelService::GetInstance()->Wait(sobj, &mutex, wait_ticks);

    h.Finish();

    CHECK_EQUAL(WAIT_RESULT_DELETED, wresult);
    CHECK_TRUE(g_SyncDeleteContext.counter > g_SyncDeleteContext.at_tick); // woken by the destruction, not before
    CHECK_TRUE(mutex.m_locked); // mutex is re-locked on return, as for any other result

    h.CheckDestroyed();
}

TEST(Sync, WakeDeleted_DestroyWithWaiter_Timed)
{
    Test_DestroyWithWaiter(10);

    // woken on the 3rd tick, active on the 4th (see SyncDeleteContext::Process), no time out
    CHECK_EQUAL(4, g_SyncDeleteContext.counter);
}

TEST(Sync, WakeDeleted_DestroyWithWaiter_Infinite)
{
    // an infinite wait never expires: only the destruction can end it
    Test_DestroyWithWaiter(WAIT_INFINITE);
}

TEST(Sync, WakeDeleted_ServiceWakeDeletedReleasesWaiter)
{
    SyncDeleteHarness h;
    MutexMock mutex;
    SyncObjectMock sobj;

    g_SyncDeleteContext.action = SyncDeleteContext::ACTION_SERVICE_WAKE_DEL;
    g_SyncDeleteContext.sobj   = &sobj;
    h.Start(NULL, NULL);
    g_SyncDeleteContext.sobj   = &sobj; // Start() does not touch it, kept explicit

    MutexMock::ScopedLock guard(mutex);

    const EWaitResult wresult = IKernelService::GetInstance()->Wait(&sobj, &mutex, 10);

    h.Finish();

    CHECK_EQUAL(WAIT_RESULT_DELETED, wresult);
    CHECK_TRUE(g_SyncDeleteContext.acted);
    CHECK_EQUAL(4, g_SyncDeleteContext.counter);
    CHECK_TRUE(mutex.m_locked);

    // the waiter is released immediately, the object itself stays valid
    CHECK_EQUAL(1, g_SyncDeleteContext.waiters_before);
    CHECK_EQUAL(0, g_SyncDeleteContext.waiters_after);
    CHECK_EQUAL(0, sobj.GetWaitList().GetSize());
    CHECK_EQUAL(TID_NONE, sobj.GetNextWaiter());

    // the object is not marked as dead: it is released from the kernel's tick list by the next tick (see Wake())
    CHECK_EQUAL(0, h.kernel.GetTickListSize());

    // nobody waits now: releasing again is harmless
    IKernelService::GetInstance()->WakeDeleted(&sobj);
    CHECK_EQUAL(0, sobj.GetWaitList().GetSize());
}

// ---------------------------------------------------------------------------- //
// Built-in primitives: the waiter gets WAIT_RESULT_DELETED and reads nothing
// of the destroyed object (its storage is poisoned)
// ---------------------------------------------------------------------------- //

TEST(Sync, WakeDeleted_Event)
{
    SyncDeleteHarness h;

    alignas(sync::Event) uint8_t storage[sizeof(sync::Event)];
    sync::Event *ev = ::new (storage) sync::Event();

    h.Start(ev, SyncDeleteDestroy<sync::Event>);

    const EWaitResult wresult = ev->WaitEx(WAIT_INFINITE);

    h.Finish();

    CHECK_EQUAL(WAIT_RESULT_DELETED, wresult);
    h.CheckDestroyed();
}

TEST(Sync, WakeDeleted_Event_BoolWaitReportsFailure)
{
    SyncDeleteHarness h;

    alignas(sync::Event) uint8_t storage[sizeof(sync::Event)];
    sync::Event *ev = ::new (storage) sync::Event();

    h.Start(ev, SyncDeleteDestroy<sync::Event>);

    // the bool overload collapses everything but a signal into false
    const bool signaled = ev->Wait(WAIT_INFINITE);

    h.Finish();

    CHECK_FALSE(signaled);
    h.CheckDestroyed();
}

TEST(Sync, WakeDeleted_Semaphore)
{
    SyncDeleteHarness h;

    alignas(sync::Semaphore) uint8_t storage[sizeof(sync::Semaphore)];
    sync::Semaphore *sem = ::new (storage) sync::Semaphore(0U);

    h.Start(sem, SyncDeleteDestroy<sync::Semaphore>);

    const EWaitResult wresult = sem->WaitEx(WAIT_INFINITE);

    h.Finish();

    CHECK_EQUAL(WAIT_RESULT_DELETED, wresult);
    h.CheckDestroyed();
}

TEST(Sync, WakeDeleted_ConditionVariable)
{
    SyncDeleteHarness h;
    MutexMock mutex;

    alignas(sync::ConditionVariable) uint8_t storage[sizeof(sync::ConditionVariable)];
    sync::ConditionVariable *cv = ::new (storage) sync::ConditionVariable();

    h.Start(cv, SyncDeleteDestroy<sync::ConditionVariable>);

    MutexMock::ScopedLock guard(mutex);

    const EWaitResult wresult = cv->WaitEx(mutex, 10);

    h.Finish();

    CHECK_EQUAL(WAIT_RESULT_DELETED, wresult);
    CHECK_TRUE(mutex.m_locked); // returns with the caller's mutex re-locked, as for any other result
    h.CheckDestroyed();
}

TEST(Sync, WakeDeleted_EventFlags_WaitEx)
{
    SyncDeleteHarness h;

    alignas(sync::EventFlags) uint8_t storage[sizeof(sync::EventFlags)];
    sync::EventFlags *ef = ::new (storage) sync::EventFlags();

    h.Start(ef, SyncDeleteDestroy<sync::EventFlags>);

    uint32_t actual = 0xFFFFFFFFU;
    const EWaitResult wresult = ef->WaitEx(0x1U, sync::EventFlags::OPT_WAIT_ANY, WAIT_INFINITE, actual);

    h.Finish();

    CHECK_EQUAL(WAIT_RESULT_DELETED, wresult);

    // the flags word is gone: not read (the poison would show up here), reported as 0
    CHECK_EQUAL(0, actual);
    h.CheckDestroyed();
}

TEST(Sync, WakeDeleted_EventFlags_WaitReportsErrorDeleted)
{
    SyncDeleteHarness h;

    alignas(sync::EventFlags) uint8_t storage[sizeof(sync::EventFlags)];
    sync::EventFlags *ef = ::new (storage) sync::EventFlags();

    h.Start(ef, SyncDeleteDestroy<sync::EventFlags>);

    const uint32_t result = ef->Wait(0x1U, sync::EventFlags::OPT_WAIT_ALL, WAIT_INFINITE);

    h.Finish();

    CHECK_TRUE(sync::EventFlags::IsError(result));
    CHECK_TRUE(result == sync::EventFlags::ERROR_DELETED);

    // distinct from every other error sentinel
    CHECK_TRUE(result != sync::EventFlags::ERROR_TIMEOUT);
    CHECK_TRUE(result != sync::EventFlags::ERROR_CANCELED);
    CHECK_TRUE(result != sync::EventFlags::ERROR_FAIL);
    h.CheckDestroyed();
}

TEST(Sync, WakeDeleted_Barrier)
{
    SyncDeleteHarness h;

    alignas(sync::Barrier) uint8_t storage[sizeof(sync::Barrier)];
    sync::Barrier *bar = ::new (storage) sync::Barrier(2U);

    h.Start(bar, SyncDeleteDestroy<sync::Barrier>);

    // the only task which arrived: waits for the second one that never comes
    const sync::Barrier::EResult result = bar->WaitEx();

    h.Finish();

    CHECK_EQUAL((int)sync::Barrier::BARRIER_DELETED, (int)result);
    h.CheckDestroyed();
}

TEST(Sync, WakeDeleted_RWMutex_WriterWaitsForReader)
{
    SyncDeleteHarness h;

    alignas(sync::RWMutex) uint8_t storage[sizeof(sync::RWMutex)];
    sync::RWMutex *rw = ::new (storage) sync::RWMutex();

    h.Start(rw, SyncDeleteDestroy<sync::RWMutex>);

    // an active reader blocks the writer (a destroyed lock is, by definition, held when someone waits for it)
    rw->ReadLock();

    const EWaitResult wresult = rw->TimedLockEx(WAIT_INFINITE);

    h.Finish();

    CHECK_EQUAL(WAIT_RESULT_DELETED, wresult);
    h.CheckDestroyed();
}

TEST(Sync, WakeDeleted_RWMutex_ReaderWaitsForWriter)
{
    SyncDeleteHarness h;

    alignas(sync::RWMutex) uint8_t storage[sizeof(sync::RWMutex)];
    sync::RWMutex *rw = ::new (storage) sync::RWMutex();

    h.Start(rw, SyncDeleteDestroy<sync::RWMutex>);

    rw->Lock();

    const EWaitResult wresult = rw->TimedReadLockEx(WAIT_INFINITE);

    h.Finish();

    CHECK_EQUAL(WAIT_RESULT_DELETED, wresult);
    h.CheckDestroyed();
}

// composite primitive: the waiter blocks on an internal ConditionVariable which is destroyed with the owner
TEST(Sync, WakeDeleted_MessageQueue)
{
    typedef sync::MessageQueueT<2, sizeof(uint32_t)> Queue;

    SyncDeleteHarness h;

    alignas(Queue) uint8_t storage[sizeof(Queue)];
    Queue *queue = ::new (storage) Queue();

    h.Start(queue, SyncDeleteDestroy<Queue>);

    uint32_t msg = 0xA5A5A5A5U;
    const EWaitResult wresult = queue->GetEx(&msg, WAIT_INFINITE);

    h.Finish();

    CHECK_EQUAL(WAIT_RESULT_DELETED, wresult);
    CHECK_EQUAL(0xA5A5A5A5U, msg); // nothing was received
    h.CheckDestroyed();
}

// ============================================================================ //
// ============================ sync::Barrier ================================= //
// ============================================================================ //

// Drives a real blocked waiter on a sync::Barrier: on the 3rd tick of the wait the "second party" acts from the
// relax-CPU hook (arrives, cancels the waiter, or both), which lets the tests reach the last-arrival path and
// the cancel / failure rollback path of Barrier::WaitEx() with a single mock task
static struct BarrierContext
{
    enum EAction
    {
        ACTION_NONE = 0,      // nobody acts (the wait must be ended by something else)
        ACTION_ARRIVE,        // the other party calls WaitEx() and trips the barrier
        ACTION_CANCEL,        // CancelTaskWait() on the waiter
        ACTION_CANCEL_ARRIVE  // CancelTaskWait() on the waiter, then the barrier trips before the waiter runs again
    };

    BarrierContext()
    {
        Reset();
    }

    void Reset()
    {
        counter      = 0;
        action       = ACTION_NONE;
        acted        = false;
        other_result = sync::Barrier::BARRIER_FAILED;
        platform     = NULL;
        kernel       = NULL;
        task         = NULL;
        bar          = NULL;
    }

    uint32_t                counter;
    EAction                 action;
    bool                    acted;
    sync::Barrier::EResult  other_result; // outcome of the other party's WaitEx() (ACTION_ARRIVE, ACTION_CANCEL_ARRIVE)
    PlatformTestMock       *platform;
    IKernel                *kernel;
    ITask                  *task;
    sync::Barrier          *bar;

    void Process()
    {
        platform->ProcessTick();
        ++counter;

        if ((counter == 3) && !acted)
        {
            acted = true;

            if ((action == ACTION_CANCEL) || (action == ACTION_CANCEL_ARRIVE))
            {
                kernel->CancelTaskWait(task);
            }

            if ((action == ACTION_ARRIVE) || (action == ACTION_CANCEL_ARRIVE))
            {
                // last arrival: does not block
                other_result = bar->WaitEx();
            }
        }
    }
}
g_BarrierContext;

static void BarrierRelaxCpu()
{
    g_BarrierContext.Process();
}

struct BarrierHarness
{
    BarrierHarness() : prev_handler(g_RelaxCpuHandler)
    {
        g_BarrierContext.Reset();

        g_BarrierContext.platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
        g_BarrierContext.kernel   = &kernel;
        g_BarrierContext.task     = &task;

        kernel.Initialize();
        kernel.AddTask(&task);
        kernel.Start();

        g_RelaxCpuHandler = BarrierRelaxCpu;
    }

    ~BarrierHarness()
    {
        g_RelaxCpuHandler = prev_handler;
    }

    // blocks the calling (mock) task on the barrier, the other party acts on the 3rd tick
    sync::Barrier::EResult Round(sync::Barrier &bar, BarrierContext::EAction action)
    {
        g_BarrierContext.Reset();
        g_BarrierContext.platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
        g_BarrierContext.kernel   = &kernel;
        g_BarrierContext.task     = &task;
        g_BarrierContext.bar      = &bar;
        g_BarrierContext.action   = action;

        return bar.WaitEx();
    }

    Kernel<KERNEL_STATIC | KERNEL_SYNC, 1, sched::SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;
    decltype(g_RelaxCpuHandler) prev_handler;
};

TEST(Sync, Barrier_Threshold)
{
    sync::Barrier bar(3U);

    CHECK_EQUAL(3, bar.GetThreshold());
}

// a barrier of one party trips on every arrival without blocking: covers the last-arrival path
TEST(Sync, Barrier_SingleParty_LastArrival)
{
    BarrierHarness h;
    sync::Barrier bar(1U);

    // every round trips immediately (generation / counter are reset each time)
    for (int i = 0; i < 3; ++i)
    {
        CHECK_EQUAL((int)sync::Barrier::BARRIER_LAST_ARRIVAL, (int)bar.WaitEx());
    }

    // bool overload: true for the last arrival
    CHECK_TRUE(bar.Wait());
    CHECK_EQUAL(1, bar.GetThreshold());
    CHECK_EQUAL(0, test::g_CriticalSectionState);
}

// the last arrival trips the barrier and releases a blocked waiter, then the barrier is reusable
TEST(Sync, Barrier_LastArrivalReleasesWaiter_Cyclic)
{
    BarrierHarness h;
    sync::Barrier bar(2U);

    for (int round = 0; round < 3; ++round)
    {
        const sync::Barrier::EResult result = h.Round(bar, BarrierContext::ACTION_ARRIVE);

        CHECK_TRUE(g_BarrierContext.acted);
        CHECK_EQUAL((int)sync::Barrier::BARRIER_RELEASED, (int)result);
        CHECK_EQUAL((int)sync::Barrier::BARRIER_LAST_ARRIVAL, (int)g_BarrierContext.other_result);
    }

    CHECK_EQUAL(0, test::g_CriticalSectionState);
}

TEST(Sync, Barrier_BoolWait_ReleasedWaiterReturnsFalse)
{
    BarrierHarness h;
    sync::Barrier bar(2U);

    g_BarrierContext.Reset();
    g_BarrierContext.platform = static_cast<PlatformTestMock *>(h.kernel.GetPlatform());
    g_BarrierContext.kernel   = &h.kernel;
    g_BarrierContext.task     = &h.task;
    g_BarrierContext.bar      = &bar;
    g_BarrierContext.action   = BarrierContext::ACTION_ARRIVE;

    CHECK_FALSE(bar.Wait()); // released waiter, not the last arrival
    CHECK_EQUAL((int)sync::Barrier::BARRIER_LAST_ARRIVAL, (int)g_BarrierContext.other_result);
}

// cancelled before the barrier tripped: the arrival is rolled back
TEST(Sync, Barrier_Canceled_RollsBackArrival)
{
    BarrierHarness h;
    sync::Barrier bar(2U);

    CHECK_EQUAL((int)sync::Barrier::BARRIER_CANCELED, (int)h.Round(bar, BarrierContext::ACTION_CANCEL));
    CHECK_TRUE(g_BarrierContext.acted);

    // had the cancelled arrival stayed counted, the next arrival would trip the barrier on its own (LAST_ARRIVAL)
    // instead of blocking for a second party
    CHECK_EQUAL((int)sync::Barrier::BARRIER_RELEASED, (int)h.Round(bar, BarrierContext::ACTION_ARRIVE));
    CHECK_EQUAL((int)sync::Barrier::BARRIER_LAST_ARRIVAL, (int)g_BarrierContext.other_result);

    // and a repeated cancellation leaves the count intact as well
    CHECK_EQUAL((int)sync::Barrier::BARRIER_CANCELED, (int)h.Round(bar, BarrierContext::ACTION_CANCEL));
    CHECK_EQUAL((int)sync::Barrier::BARRIER_CANCELED, (int)h.Round(bar, BarrierContext::ACTION_CANCEL));
    CHECK_EQUAL((int)sync::Barrier::BARRIER_RELEASED, (int)h.Round(bar, BarrierContext::ACTION_ARRIVE));

    CHECK_EQUAL(0, test::g_CriticalSectionState);
}

TEST(Sync, Barrier_Canceled_BoolWaitReturnsFalse)
{
    BarrierHarness h;
    sync::Barrier bar(2U);

    g_BarrierContext.Reset();
    g_BarrierContext.platform = static_cast<PlatformTestMock *>(h.kernel.GetPlatform());
    g_BarrierContext.kernel   = &h.kernel;
    g_BarrierContext.task     = &h.task;
    g_BarrierContext.bar      = &bar;
    g_BarrierContext.action   = BarrierContext::ACTION_CANCEL;

    // the bool overload cannot tell a cancellation from a normal release
    CHECK_FALSE(bar.Wait());
}

// the barrier trips after the waiter was cancelled but before it re-acquired the critical section: the signal wins
TEST(Sync, Barrier_CancelRacesWithTrip_SignalWins)
{
    BarrierHarness h;
    sync::Barrier bar(2U);

    CHECK_EQUAL((int)sync::Barrier::BARRIER_RELEASED, (int)h.Round(bar, BarrierContext::ACTION_CANCEL_ARRIVE));
    CHECK_EQUAL((int)sync::Barrier::BARRIER_LAST_ARRIVAL, (int)g_BarrierContext.other_result);

    // the trip consumed our arrival and reset the counter: the barrier is in a clean state for the next round
    CHECK_EQUAL((int)sync::Barrier::BARRIER_RELEASED, (int)h.Round(bar, BarrierContext::ACTION_ARRIVE));
    CHECK_EQUAL((int)sync::Barrier::BARRIER_LAST_ARRIVAL, (int)g_BarrierContext.other_result);

    CHECK_EQUAL(0, test::g_CriticalSectionState);
}

// the kernel rejects the blocking wait (scheduler locked): the arrival is rolled back, no retry loop
TEST(Sync, Barrier_Failed_SchedulerLocked_RollsBackArrival)
{
    BarrierHarness h;
    sync::Barrier bar(2U);

    g_BarrierContext.Reset();

    LockScheduler();

    // waiting on locked scheduler causes an assertion by IKernelService::Wait,
    // so we test this behavior first
    try
    {
        sync::Barrier assert_bar(2U);

        g_TestContext.ExpectAssert(true);
        STK_UNUSED(assert_bar.WaitEx());
        CHECK(false); // should asssert in a DEBUG build
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    sync::Barrier::EResult result = sync::Barrier::BARRIER_RELEASED;

    // in RELEASE build assert is inactive, so we expect BARRIER_FAILED
    {
        g_TestContext.ExpectAssert(true);
        g_TestContext.RethrowAssertException(false);

        result = bar.WaitEx();

        g_TestContext.RethrowAssertException(true);
        g_TestContext.ExpectAssert(false);
    }

    UnlockScheduler();

    CHECK_EQUAL((int)sync::Barrier::BARRIER_FAILED, (int)result);

    // arrival was undone: the next arrival must block for a second party instead of tripping alone
    CHECK_EQUAL((int)sync::Barrier::BARRIER_RELEASED, (int)h.Round(bar, BarrierContext::ACTION_ARRIVE));
    CHECK_EQUAL((int)sync::Barrier::BARRIER_LAST_ARRIVAL, (int)g_BarrierContext.other_result);

    CHECK_EQUAL(0, test::g_CriticalSectionState);
}

// ============================================================================ //
// ============================ sync::EventFlags ============================== //
// ============================================================================ //

// Drives a real blocked waiter on a sync::EventFlags: from the relax-CPU hook the "other party" sets flags
// (up to two Set() calls on chosen ticks) and/or cancels the waiter
static struct EventFlagsContext
{
    EventFlagsContext()
    {
        Reset();
    }

    void Reset()
    {
        counter     = 0;
        set1_tick   = 0;
        set1_flags  = 0U;
        set2_tick   = 0;
        set2_flags  = 0U;
        cancel_tick = 0;
        platform    = NULL;
        kernel      = NULL;
        task        = NULL;
        ef          = NULL;
    }

    uint32_t           counter;
    uint32_t           set1_tick;   // tick of the 1st Set() (0 = never)
    uint32_t           set1_flags;
    uint32_t           set2_tick;   // tick of the 2nd Set() (0 = never)
    uint32_t           set2_flags;
    uint32_t           cancel_tick; // tick of CancelTaskWait() (0 = never)
    PlatformTestMock  *platform;
    IKernel           *kernel;
    ITask             *task;
    sync::EventFlags  *ef;

    void Process()
    {
        platform->ProcessTick();
        ++counter;

        if (counter == set1_tick)
        {
            ef->Set(set1_flags);
        }

        if (counter == set2_tick)
        {
            ef->Set(set2_flags);
        }

        if (counter == cancel_tick)
        {
            kernel->CancelTaskWait(task);
        }
    }
}
g_EventFlagsContext;

static void EventFlagsRelaxCpu()
{
    g_EventFlagsContext.Process();
}

struct EventFlagsHarness
{
    EventFlagsHarness() : prev_handler(g_RelaxCpuHandler)
    {
        kernel.Initialize();
        kernel.AddTask(&task);
        kernel.Start();

        Arm(NULL);
        g_RelaxCpuHandler = EventFlagsRelaxCpu;
    }

    ~EventFlagsHarness()
    {
        g_RelaxCpuHandler = prev_handler;
    }

    // resets the scenario, the caller then fills in the ticks / flags
    EventFlagsContext &Arm(sync::EventFlags *ef)
    {
        g_EventFlagsContext.Reset();
        g_EventFlagsContext.platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
        g_EventFlagsContext.kernel   = &kernel;
        g_EventFlagsContext.task     = &task;
        g_EventFlagsContext.ef       = ef;
        return g_EventFlagsContext;
    }

    Kernel<KERNEL_STATIC | KERNEL_SYNC, 1, sched::SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;
    decltype(g_RelaxCpuHandler) prev_handler;
};

TEST(Sync, EventFlags_Wait_InvalidParameters)
{
    EventFlagsHarness h;
    sync::EventFlags ef;

    // no flags requested
    CHECK_TRUE(ef.Wait(0U, sync::EventFlags::OPT_WAIT_ANY, NO_WAIT) == sync::EventFlags::ERROR_PARAMETER);

    // the error sentinel bit(s) must not be requested
    CHECK_TRUE(ef.Wait(sync::EventFlags::ERROR_MASK, sync::EventFlags::OPT_WAIT_ANY, NO_WAIT) == sync::EventFlags::ERROR_PARAMETER);
    CHECK_TRUE(sync::EventFlags::IsError(sync::EventFlags::ERROR_PARAMETER));
}

TEST(Sync, EventFlags_WaitEx_InvalidParameters)
{
    EventFlagsHarness h;
    sync::EventFlags ef;

    ef.Set(0x5U);

    // invalid argument: the wait is not performed, flags are reported as is and nothing is cleared
    uint32_t actual = 0xFFFFFFFFU;
    CHECK_EQUAL(WAIT_RESULT_FAIL, ef.WaitEx(0U, sync::EventFlags::OPT_WAIT_ANY, NO_WAIT, actual));
    CHECK_EQUAL(0x5U, actual);

    actual = 0xFFFFFFFFU;
    CHECK_EQUAL(WAIT_RESULT_FAIL, ef.WaitEx(sync::EventFlags::ERROR_MASK, sync::EventFlags::OPT_WAIT_ANY, NO_WAIT, actual));
    CHECK_EQUAL(0x5U, actual);

    CHECK_EQUAL(0x5U, ef.Get());
}

// predicate already satisfied: no blocking, matched flags are reported and cleared
TEST(Sync, EventFlags_Wait_Any_Satisfied_ClearsMatched)
{
    EventFlagsHarness h;
    sync::EventFlags ef;

    ef.Set(0x7U);

    // ANY reports only the requested flags which are set
    CHECK_EQUAL(0x3U, ef.Wait(0x3U, sync::EventFlags::OPT_WAIT_ANY, NO_WAIT));

    // matched flags are consumed, the rest stays
    CHECK_EQUAL(0x4U, ef.Get());
}

TEST(Sync, EventFlags_Wait_Any_PartialMatch)
{
    EventFlagsHarness h;
    sync::EventFlags ef;

    ef.Set(0x5U);

    // requested 0x3, only bit 0 is set: ANY is satisfied by it and reports (and clears) only that bit
    CHECK_EQUAL(0x1U, ef.Wait(0x3U, sync::EventFlags::OPT_WAIT_ANY, NO_WAIT));
    CHECK_EQUAL(0x4U, ef.Get());
}

TEST(Sync, EventFlags_Wait_All_Satisfied_ClearsRequested)
{
    EventFlagsHarness h;
    sync::EventFlags ef;

    ef.Set(0x7U);

    CHECK_EQUAL(0x3U, ef.Wait(0x3U, sync::EventFlags::OPT_WAIT_ALL, NO_WAIT));
    CHECK_EQUAL(0x4U, ef.Get());
}

TEST(Sync, EventFlags_Wait_NoClear_KeepsFlags)
{
    EventFlagsHarness h;
    sync::EventFlags ef;

    ef.Set(0x7U);

    CHECK_EQUAL(0x3U, ef.Wait(0x3U, sync::EventFlags::OPT_WAIT_ALL | sync::EventFlags::OPT_NO_CLEAR, NO_WAIT));
    CHECK_EQUAL(0x7U, ef.Get());

    CHECK_EQUAL(0x3U, ef.Wait(0x3U, sync::EventFlags::OPT_WAIT_ANY | sync::EventFlags::OPT_NO_CLEAR, NO_WAIT));
    CHECK_EQUAL(0x7U, ef.Get());
}

TEST(Sync, EventFlags_WaitEx_Satisfied_ReportsFullSnapshotBeforeClear)
{
    EventFlagsHarness h;
    sync::EventFlags ef;

    ef.Set(0x7U);

    uint32_t actual = 0U;
    CHECK_EQUAL(WAIT_RESULT_SIGNAL, ef.WaitEx(0x3U, sync::EventFlags::OPT_WAIT_ANY, NO_WAIT, actual));

    // the whole group as it was at the moment of the match, taken before the auto-clear
    CHECK_EQUAL(0x7U, actual);
    CHECK_EQUAL(0x4U, ef.Get());
}

// not satisfied and NO_WAIT: reports a timeout, flags untouched
TEST(Sync, EventFlags_Wait_NoWait_NotSatisfied)
{
    EventFlagsHarness h;
    sync::EventFlags ef;

    ef.Set(0x4U);

    CHECK_TRUE(ef.Wait(0x3U, sync::EventFlags::OPT_WAIT_ANY, NO_WAIT) == sync::EventFlags::ERROR_TIMEOUT);

    uint32_t actual = 0U;
    CHECK_EQUAL(WAIT_RESULT_TIMEOUT, ef.WaitEx(0x3U, sync::EventFlags::OPT_WAIT_ANY, NO_WAIT, actual));
    CHECK_EQUAL(0x4U, actual);
    CHECK_EQUAL(0x4U, ef.Get());
}

// blocked waiter is woken by Set(): signal path of the wait loop, matched flags are cleared afterwards
TEST(Sync, EventFlags_Wait_Blocked_WokenBySet)
{
    EventFlagsHarness h;
    sync::EventFlags ef;

    EventFlagsContext &ctx = h.Arm(&ef);
    ctx.set1_tick  = 3;
    ctx.set1_flags = 0x6U;

    CHECK_EQUAL(0x6U, ef.Wait(0x2U | 0x4U, sync::EventFlags::OPT_WAIT_ALL, WAIT_INFINITE));
    CHECK_EQUAL(0U, ef.Get());
}

TEST(Sync, EventFlags_WaitEx_Blocked_WokenBySet_NoClear)
{
    EventFlagsHarness h;
    sync::EventFlags ef;

    EventFlagsContext &ctx = h.Arm(&ef);
    ctx.set1_tick  = 3;
    ctx.set1_flags = 0x9U;

    uint32_t actual = 0U;
    CHECK_EQUAL(WAIT_RESULT_SIGNAL,
        ef.WaitEx(0x1U, sync::EventFlags::OPT_WAIT_ANY | sync::EventFlags::OPT_NO_CLEAR, WAIT_INFINITE, actual));

    CHECK_EQUAL(0x9U, actual);
    CHECK_EQUAL(0x9U, ef.Get());
}

// timed wait (absolute deadline is captured) which expires
TEST(Sync, EventFlags_Wait_Timed_Timeout)
{
    EventFlagsHarness h;
    sync::EventFlags ef;

    h.Arm(&ef);

    CHECK_TRUE(ef.Wait(0x1U, sync::EventFlags::OPT_WAIT_ANY, 4) == sync::EventFlags::ERROR_TIMEOUT);
    CHECK_EQUAL(0U, ef.Get());
}

// timed wait which is satisfied before the deadline
TEST(Sync, EventFlags_Wait_Timed_WokenBySet)
{
    EventFlagsHarness h;
    sync::EventFlags ef;

    EventFlagsContext &ctx = h.Arm(&ef);
    ctx.set1_tick  = 2;
    ctx.set1_flags = 0x1U;

    CHECK_EQUAL(0x1U, ef.Wait(0x1U, sync::EventFlags::OPT_WAIT_ANY, 10));
    CHECK_EQUAL(0U, ef.Get());
}

// partial Set() under OPT_WAIT_ALL wakes the waiter without satisfying it: the loop re-waits with the remaining
// time derived from the original deadline (not restarted), and completes when the rest arrives
TEST(Sync, EventFlags_Wait_Timed_All_PartialSetThenComplete)
{
    EventFlagsHarness h;
    sync::EventFlags ef;

    EventFlagsContext &ctx = h.Arm(&ef);
    ctx.set1_tick  = 2;
    ctx.set1_flags = 0x1U;
    ctx.set2_tick  = 4;
    ctx.set2_flags = 0x2U;

    CHECK_EQUAL(0x3U, ef.Wait(0x3U, sync::EventFlags::OPT_WAIT_ALL, 10));
    CHECK_EQUAL(0U, ef.Get());
}

// the partial Set() must not restart the timeout: the 2nd part never comes and the wait still ends by the deadline
TEST(Sync, EventFlags_WaitEx_Timed_All_PartialSetThenTimeout)
{
    EventFlagsHarness h;
    sync::EventFlags ef;

    EventFlagsContext &ctx = h.Arm(&ef);
    ctx.set1_tick  = 2;
    ctx.set1_flags = 0x1U;

    uint32_t actual = 0U;
    CHECK_EQUAL(WAIT_RESULT_TIMEOUT, ef.WaitEx(0x3U, sync::EventFlags::OPT_WAIT_ALL, 5, actual));

    // snapshot is reported on timeout, nothing is cleared
    CHECK_EQUAL(0x1U, actual);
    CHECK_EQUAL(0x1U, ef.Get());

    // the whole wait took about the original timeout (not 2 + 5 ticks)
    CHECK_TRUE(g_EventFlagsContext.counter <= 7);
}

TEST(Sync, EventFlags_Wait_Canceled)
{
    EventFlagsHarness h;
    sync::EventFlags ef;

    EventFlagsContext &ctx = h.Arm(&ef);
    ctx.cancel_tick = 3;

    const uint32_t result = ef.Wait(0x1U, sync::EventFlags::OPT_WAIT_ANY, WAIT_INFINITE);

    CHECK_TRUE(sync::EventFlags::IsError(result));
    CHECK_TRUE(result == sync::EventFlags::ERROR_CANCELED);
    CHECK_TRUE(result != sync::EventFlags::ERROR_TIMEOUT);
    CHECK_EQUAL(0U, ef.Get());
}

TEST(Sync, EventFlags_WaitEx_Canceled_ReportsSnapshot)
{
    EventFlagsHarness h;
    sync::EventFlags ef;

    EventFlagsContext &ctx = h.Arm(&ef);
    ctx.set1_tick   = 2;
    ctx.set1_flags  = 0x4U; // does not satisfy the wait for 0x1, but wakes the waiter for re-evaluation

    // cancel only after the waiter has woken (tick 2) and blocked again: CancelTaskWait() is a no-op for a task
    // whose wait is already signaled, and the hook would not fire again, leaving the waiter blocked forever
    ctx.cancel_tick = 5;

    uint32_t actual = 0U;
    CHECK_EQUAL(WAIT_RESULT_CANCELED, ef.WaitEx(0x1U, sync::EventFlags::OPT_WAIT_ANY, WAIT_INFINITE, actual));

    CHECK_EQUAL(0x4U, actual);
    CHECK_EQUAL(0x4U, ef.Get()); // nothing cleared
}

// only NO_WAIT is permitted inside an ISR: any blocking timeout is rejected without waiting
TEST(Sync, EventFlags_Wait_InsideISR_BlockingRejected)
{
    EventFlagsHarness h;
    sync::EventFlags ef;

    ef.Set(0x1U); // even if the predicate is satisfied the call is rejected

    ScopedInsideISR isr;

    const uint32_t r_inf   = ef.Wait(0x1U, sync::EventFlags::OPT_WAIT_ANY, WAIT_INFINITE);
    const uint32_t r_timed = ef.Wait(0x1U, sync::EventFlags::OPT_WAIT_ANY, 5);

    CHECK_TRUE(sync::EventFlags::IsError(r_inf));
    CHECK_TRUE(r_inf == sync::EventFlags::ERROR_ISR);
    CHECK_TRUE(r_timed == sync::EventFlags::ERROR_ISR);

    // distinct from every other error sentinel
    CHECK_TRUE(r_inf != sync::EventFlags::ERROR_TIMEOUT);
    CHECK_TRUE(r_inf != sync::EventFlags::ERROR_PARAMETER);
    CHECK_TRUE(r_inf != sync::EventFlags::ERROR_CANCELED);
    CHECK_TRUE(r_inf != sync::EventFlags::ERROR_FAIL);
    CHECK_TRUE(r_inf != sync::EventFlags::ERROR_DELETED);

    // nothing was consumed
    CHECK_EQUAL(0x1U, ef.Get());
}

// NO_WAIT is the only mode allowed inside an ISR (polling)
TEST(Sync, EventFlags_Wait_InsideISR_NoWaitAllowed)
{
    EventFlagsHarness h;
    sync::EventFlags ef;

    ScopedInsideISR isr;

    // nothing set: polling reports a timeout, not ERROR_ISR
    CHECK_TRUE(ef.Wait(0x1U, sync::EventFlags::OPT_WAIT_ANY, NO_WAIT) == sync::EventFlags::ERROR_TIMEOUT);

    ef.Set(0x3U);

    // satisfied: matched flags are returned and cleared
    CHECK_EQUAL(0x1U, ef.Wait(0x1U, sync::EventFlags::OPT_WAIT_ANY, NO_WAIT));
    CHECK_EQUAL(0x2U, ef.Get());
}

// the invalid-parameter check takes precedence over the ISR check
TEST(Sync, EventFlags_Wait_InsideISR_ParameterErrorFirst)
{
    EventFlagsHarness h;
    sync::EventFlags ef;

    ScopedInsideISR isr;

    CHECK_TRUE(ef.Wait(0U, sync::EventFlags::OPT_WAIT_ANY, WAIT_INFINITE) == sync::EventFlags::ERROR_PARAMETER);
}

TEST(Sync, EventFlags_WaitEx_InsideISR_BlockingRejected)
{
    EventFlagsHarness h;
    sync::EventFlags ef;

    ef.Set(0x5U);

    ScopedInsideISR isr;

    // blocking wait inside an ISR: not performed, flags are reported as is and nothing is cleared
    uint32_t actual = 0xFFFFFFFFU;
    CHECK_EQUAL(WAIT_RESULT_FAIL, ef.WaitEx(0x1U, sync::EventFlags::OPT_WAIT_ANY, WAIT_INFINITE, actual));
    CHECK_EQUAL(0x5U, actual);

    actual = 0xFFFFFFFFU;
    CHECK_EQUAL(WAIT_RESULT_FAIL, ef.WaitEx(0x1U, sync::EventFlags::OPT_WAIT_ANY, 5, actual));
    CHECK_EQUAL(0x5U, actual);

    CHECK_EQUAL(0x5U, ef.Get());
}

TEST(Sync, EventFlags_WaitEx_InsideISR_NoWaitAllowed)
{
    EventFlagsHarness h;
    sync::EventFlags ef;

    ScopedInsideISR isr;

    uint32_t actual = 0xFFFFFFFFU;
    CHECK_EQUAL(WAIT_RESULT_TIMEOUT, ef.WaitEx(0x1U, sync::EventFlags::OPT_WAIT_ANY, NO_WAIT, actual));
    CHECK_EQUAL(0U, actual);

    ef.Set(0x3U);

    CHECK_EQUAL(WAIT_RESULT_SIGNAL, ef.WaitEx(0x1U, sync::EventFlags::OPT_WAIT_ANY, NO_WAIT, actual));
    CHECK_EQUAL(0x3U, actual);
    CHECK_EQUAL(0x2U, ef.Get());
}

// the kernel rejects the blocking wait (scheduler locked)
TEST(Sync, EventFlags_Wait_Failed_SchedulerLocked)
{
    EventFlagsHarness h;
    sync::EventFlags ef;

    h.Arm(&ef);

    LockScheduler();

    // waiting on locked scheduler causes an assertion by IKernelService::Wait,
    // so we test this behavior first
    try
    {
        sync::EventFlags assert_ef;

        g_TestContext.ExpectAssert(true);
        STK_UNUSED(assert_ef.Wait(0x1U, sync::EventFlags::OPT_WAIT_ANY, WAIT_INFINITE));
        CHECK(false); // should asssert in a DEBUG build
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    uint32_t result = 0U;
    uint32_t actual = 0xFFFFFFFFU;
    EWaitResult wresult = WAIT_RESULT_SIGNAL;

    // in RELEASE build assert is inactive, so we expect a failure result
    {
        g_TestContext.ExpectAssert(true);
        g_TestContext.RethrowAssertException(false);

        result  = ef.Wait(0x1U, sync::EventFlags::OPT_WAIT_ANY, WAIT_INFINITE);
        wresult = ef.WaitEx(0x1U, sync::EventFlags::OPT_WAIT_ANY, WAIT_INFINITE, actual);

        g_TestContext.RethrowAssertException(true);
        g_TestContext.ExpectAssert(false);
    }

    UnlockScheduler();

    CHECK_TRUE(sync::EventFlags::IsError(result));
    CHECK_TRUE(result == sync::EventFlags::ERROR_FAIL);
    CHECK_EQUAL(WAIT_RESULT_FAIL, wresult);
    CHECK_EQUAL(0U, actual);
}

// ============================================================================ //
// ============================ sync::MessageQueue ============================ //
// ============================================================================ //

typedef sync::MessageQueueT<2, sizeof(uint32_t)> TestQueue;

static const uint32_t MQ_UNTOUCHED = 0xA5A5A5A5U; // marker for "nothing was copied to the caller's buffer"

// Drives a real blocked waiter on a sync::MessageQueue: from the relax-CPU hook the "other party" runs a script of
// non-blocking queue operations (put / put-front / get / reset) and/or cancels the waiter at chosen ticks
static struct MessageQueueContext
{
    enum EType
    {
        EV_PUT,       // TryPut(value)
        EV_PUT_FRONT, // TryPutFront(value)
        EV_GET,       // TryGet() and discard (e.g. steals a just-produced message from a woken consumer)
        EV_RESET,     // Reset()
        EV_CANCEL     // CancelTaskWait() on the waiter
    };

    struct Event
    {
        uint32_t tick;
        EType    type;
        uint32_t value;
    };

    static const uint32_t EVENTS_MAX = 5;

    MessageQueueContext()
    {
        Reset();
    }

    void Reset()
    {
        counter  = 0;
        count    = 0;
        platform = NULL;
        kernel   = NULL;
        task     = NULL;
        queue    = NULL;
    }

    void Add(uint32_t tick, EType type, uint32_t value = 0U)
    {
        STK_ASSERT(count < EVENTS_MAX);

        events[count].tick  = tick;
        events[count].type  = type;
        events[count].value = value;
        ++count;
    }

    uint32_t           counter;
    uint32_t           count;
    Event              events[EVENTS_MAX];
    PlatformTestMock  *platform;
    IKernel           *kernel;
    ITask             *task;
    sync::MessageQueue *queue;

    void Process()
    {
        platform->ProcessTick();
        ++counter;

        for (uint32_t i = 0; i < count; ++i)
        {
            if (events[i].tick != counter)
                continue;

            uint32_t tmp = 0U;

            switch (events[i].type)
            {
            case EV_PUT:       queue->TryPut(&events[i].value);      break;
            case EV_PUT_FRONT: queue->TryPutFront(&events[i].value); break;
            case EV_GET:       queue->TryGet(&tmp);                  break;
            case EV_RESET:     queue->Reset();                       break;
            case EV_CANCEL:    kernel->CancelTaskWait(task);         break;
            }
        }
    }
}
g_MessageQueueContext;

static void MessageQueueRelaxCpu()
{
    g_MessageQueueContext.Process();
}

struct MessageQueueHarness
{
    MessageQueueHarness() : prev_handler(g_RelaxCpuHandler)
    {
        kernel.Initialize();
        kernel.AddTask(&task);
        kernel.Start();

        Arm(NULL);
        g_RelaxCpuHandler = MessageQueueRelaxCpu;
    }

    ~MessageQueueHarness()
    {
        g_RelaxCpuHandler = prev_handler;
    }

    // resets the script, the caller then adds events
    MessageQueueContext &Arm(sync::MessageQueue *queue)
    {
        g_MessageQueueContext.Reset();
        g_MessageQueueContext.platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
        g_MessageQueueContext.kernel   = &kernel;
        g_MessageQueueContext.task     = &task;
        g_MessageQueueContext.queue    = queue;
        return g_MessageQueueContext;
    }

    Kernel<KERNEL_STATIC | KERNEL_SYNC, 1, sched::SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;
    decltype(g_RelaxCpuHandler) prev_handler;
};

static uint32_t MqPop(sync::MessageQueue &q)
{
    uint32_t v = MQ_UNTOUCHED;
    CHECK_TRUE(q.TryGet(&v));
    return v;
}

static void MqPush(sync::MessageQueue &q, uint32_t v)
{
    CHECK_TRUE(q.TryPut(&v));
}

TEST(Sync, MessageQueue_Introspection)
{
    MessageQueueHarness h;
    TestQueue q;

    CHECK_EQUAL(2U, q.GetCapacity());
    CHECK_EQUAL(sizeof(uint32_t), q.GetMsgSize());
    CHECK_TRUE(q.IsStorageValid());
    CHECK_TRUE(q.GetBuffer() != NULL);
    CHECK_TRUE(q.IsEmpty());
    CHECK_FALSE(q.IsFull());
    CHECK_EQUAL(0U, q.GetCount());
    CHECK_EQUAL(2U, q.GetSpace());

    MqPush(q, 1U);
    CHECK_FALSE(q.IsEmpty());
    CHECK_FALSE(q.IsFull());
    CHECK_EQUAL(1U, q.GetCount());
    CHECK_EQUAL(1U, q.GetSpace());

    MqPush(q, 2U);
    CHECK_TRUE(q.IsFull());
    CHECK_EQUAL(0U, q.GetSpace());
}

TEST(Sync, MessageQueue_Fifo_WrapAround_NonBlocking)
{
    MessageQueueHarness h;
    TestQueue q;

    uint32_t v = 3U;

    MqPush(q, 1U);
    MqPush(q, 2U);
    CHECK_FALSE(q.TryPut(&v)); // full

    CHECK_EQUAL(1U, MqPop(q));
    MqPush(q, 3U);             // wraps around the ring
    CHECK_EQUAL(2U, MqPop(q));
    CHECK_EQUAL(3U, MqPop(q));

    v = MQ_UNTOUCHED;
    CHECK_FALSE(q.TryGet(&v)); // empty
    CHECK_EQUAL(MQ_UNTOUCHED, v);
    CHECK_TRUE(q.IsEmpty());
}

TEST(Sync, MessageQueue_PutFront_Peek_Reset)
{
    MessageQueueHarness h;
    TestQueue q;

    uint32_t v = MQ_UNTOUCHED;
    CHECK_FALSE(q.TryPeek(&v));
    CHECK_FALSE(q.TryPeekFront(&v));
    CHECK_EQUAL(MQ_UNTOUCHED, v);

    MqPush(q, 7U);

    // peek does not remove
    CHECK_TRUE(q.TryPeek(&v));
    CHECK_EQUAL(7U, v);
    v = 0U;
    CHECK_TRUE(q.TryPeekFront(&v));
    CHECK_EQUAL(7U, v);
    CHECK_EQUAL(1U, q.GetCount());

    // front insertion becomes the next message
    v = 8U;
    CHECK_TRUE(q.TryPutFront(&v));
    CHECK_FALSE(q.TryPutFront(&v)); // full

    v = 0U;
    CHECK_TRUE(q.TryPeekFront(&v));
    CHECK_EQUAL(8U, v);
    v = 0U;
    CHECK_TRUE(q.TryPeek(&v));
    CHECK_EQUAL(8U, v);

    CHECK_EQUAL(8U, MqPop(q));
    CHECK_EQUAL(7U, MqPop(q));

    MqPush(q, 1U);
    q.Reset();
    CHECK_TRUE(q.IsEmpty());
    CHECK_FALSE(q.TryGet(&v));
}

// ---- consumers: WaitNotEmpty() via GetEx() / PeekEx() / PeekFrontEx() ----

TEST(Sync, MessageQueue_GetEx_Blocked_WokenByPut)
{
    MessageQueueHarness h;
    TestQueue q;

    h.Arm(&q).Add(3, MessageQueueContext::EV_PUT, 0x11U);

    uint32_t msg = MQ_UNTOUCHED;
    CHECK_EQUAL(WAIT_RESULT_SIGNAL, q.GetEx(&msg, WAIT_INFINITE));
    CHECK_EQUAL(0x11U, msg);
    CHECK_TRUE(q.IsEmpty());
}

TEST(Sync, MessageQueue_GetEx_Timed_WokenByPut)
{
    MessageQueueHarness h;
    TestQueue q;

    h.Arm(&q).Add(3, MessageQueueContext::EV_PUT, 0x22U);

    uint32_t msg = MQ_UNTOUCHED;
    CHECK_EQUAL(WAIT_RESULT_SIGNAL, q.GetEx(&msg, 10));
    CHECK_EQUAL(0x22U, msg);
    CHECK_TRUE(q.IsEmpty());
}

// timed wait (absolute deadline is captured) which expires
TEST(Sync, MessageQueue_GetEx_Timed_Timeout)
{
    MessageQueueHarness h;
    TestQueue q;

    h.Arm(&q);

    uint32_t msg = MQ_UNTOUCHED;
    CHECK_EQUAL(WAIT_RESULT_TIMEOUT, q.GetEx(&msg, 4));
    CHECK_EQUAL(MQ_UNTOUCHED, msg); // nothing is copied
    CHECK_TRUE(q.IsEmpty());

    CHECK_FALSE(q.Get(&msg, 4));
}

// the message is stolen by another consumer between the wake-up and the re-acquisition of the critical section
// (Mesa-style wake-up): the waiter loops with the remaining time derived from the original deadline
TEST(Sync, MessageQueue_GetEx_Timed_MessageStolen_ThenTimeout)
{
    MessageQueueHarness h;
    TestQueue q;

    MessageQueueContext &ctx = h.Arm(&q);
    ctx.Add(2, MessageQueueContext::EV_PUT, 1U);
    ctx.Add(2, MessageQueueContext::EV_GET);

    uint32_t msg = MQ_UNTOUCHED;
    CHECK_EQUAL(WAIT_RESULT_TIMEOUT, q.GetEx(&msg, 6));
    CHECK_EQUAL(MQ_UNTOUCHED, msg);
    CHECK_TRUE(q.IsEmpty());
}

TEST(Sync, MessageQueue_GetEx_Timed_MessageStolen_ThenReceived)
{
    MessageQueueHarness h;
    TestQueue q;

    MessageQueueContext &ctx = h.Arm(&q);
    ctx.Add(2, MessageQueueContext::EV_PUT, 1U);
    ctx.Add(2, MessageQueueContext::EV_GET);
    ctx.Add(5, MessageQueueContext::EV_PUT, 2U);

    uint32_t msg = MQ_UNTOUCHED;
    CHECK_EQUAL(WAIT_RESULT_SIGNAL, q.GetEx(&msg, 10));
    CHECK_EQUAL(2U, msg);
    CHECK_TRUE(q.IsEmpty());
}

TEST(Sync, MessageQueue_GetEx_Infinite_MessageStolen_ThenReceived)
{
    MessageQueueHarness h;
    TestQueue q;

    MessageQueueContext &ctx = h.Arm(&q);
    ctx.Add(2, MessageQueueContext::EV_PUT, 1U);
    ctx.Add(2, MessageQueueContext::EV_GET);
    ctx.Add(5, MessageQueueContext::EV_PUT, 2U);

    uint32_t msg = MQ_UNTOUCHED;
    CHECK_EQUAL(WAIT_RESULT_SIGNAL, q.GetEx(&msg, WAIT_INFINITE));
    CHECK_EQUAL(2U, msg);
    CHECK_TRUE(q.IsEmpty());
}

TEST(Sync, MessageQueue_GetEx_Canceled)
{
    MessageQueueHarness h;
    TestQueue q;

    uint32_t msg = MQ_UNTOUCHED;

    h.Arm(&q).Add(3, MessageQueueContext::EV_CANCEL);
    CHECK_EQUAL(WAIT_RESULT_CANCELED, q.GetEx(&msg, WAIT_INFINITE));
    CHECK_EQUAL(MQ_UNTOUCHED, msg);

    // the bool overload cannot tell a cancellation from a timeout
    h.Arm(&q).Add(3, MessageQueueContext::EV_CANCEL);
    CHECK_FALSE(q.Get(&msg, WAIT_INFINITE));
    CHECK_EQUAL(MQ_UNTOUCHED, msg);

    CHECK_TRUE(q.IsEmpty());
}

// woken without a message and then cancelled while waiting again
TEST(Sync, MessageQueue_GetEx_MessageStolen_ThenCanceled)
{
    MessageQueueHarness h;
    TestQueue q;

    MessageQueueContext &ctx = h.Arm(&q);
    ctx.Add(2, MessageQueueContext::EV_PUT, 1U);
    ctx.Add(2, MessageQueueContext::EV_GET);
    ctx.Add(5, MessageQueueContext::EV_CANCEL); // after the waiter blocked again

    uint32_t msg = MQ_UNTOUCHED;
    CHECK_EQUAL(WAIT_RESULT_CANCELED, q.GetEx(&msg, WAIT_INFINITE));
    CHECK_EQUAL(MQ_UNTOUCHED, msg);
}

// the kernel rejects the blocking wait (scheduler locked)
TEST(Sync, MessageQueue_GetEx_Failed_SchedulerLocked)
{
    MessageQueueHarness h;
    TestQueue q;

    h.Arm(&q);

    IKernelService::GetInstance()->LockScheduler();

    // waiting on locked scheduler causes an assertion by IKernelService::Wait,
    // so we test this behavior first
    try
    {
        TestQueue assert_q;
        uint32_t tmp = 0U;

        g_TestContext.ExpectAssert(true);
        STK_UNUSED(assert_q.GetEx(&tmp, WAIT_INFINITE));
        CHECK(false); // should asssert in a DEBUG build
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    uint32_t msg = MQ_UNTOUCHED;
    EWaitResult wresult = WAIT_RESULT_SIGNAL;

    // in RELEASE build assert is inactive, so we expect a failure result
    {
        g_TestContext.ExpectAssert(true);
        g_TestContext.RethrowAssertException(false);

        wresult = q.GetEx(&msg, WAIT_INFINITE);

        g_TestContext.RethrowAssertException(true);
        g_TestContext.ExpectAssert(false);
    }

    IKernelService::GetInstance()->UnlockScheduler();

    CHECK_EQUAL(WAIT_RESULT_FAIL, wresult);
    CHECK_EQUAL(MQ_UNTOUCHED, msg);
    CHECK_TRUE(q.IsEmpty());
}

TEST(Sync, MessageQueue_PeekEx_Blocked_WokenByPut_KeepsMessage)
{
    MessageQueueHarness h;
    TestQueue q;

    h.Arm(&q).Add(3, MessageQueueContext::EV_PUT, 0x33U);

    uint32_t msg = MQ_UNTOUCHED;
    CHECK_EQUAL(WAIT_RESULT_SIGNAL, q.PeekEx(&msg, WAIT_INFINITE));
    CHECK_EQUAL(0x33U, msg);

    // the message stays in the queue
    CHECK_EQUAL(1U, q.GetCount());
    CHECK_EQUAL(0x33U, MqPop(q));
}

TEST(Sync, MessageQueue_PeekFrontEx_Blocked_WokenByPutFront_KeepsMessage)
{
    MessageQueueHarness h;
    TestQueue q;

    h.Arm(&q).Add(3, MessageQueueContext::EV_PUT_FRONT, 0x44U);

    uint32_t msg = MQ_UNTOUCHED;
    CHECK_EQUAL(WAIT_RESULT_SIGNAL, q.PeekFrontEx(&msg, 10));
    CHECK_EQUAL(0x44U, msg);

    CHECK_EQUAL(1U, q.GetCount());
    CHECK_EQUAL(0x44U, MqPop(q));
}

TEST(Sync, MessageQueue_Peek_Timeout_Canceled)
{
    MessageQueueHarness h;
    TestQueue q;

    uint32_t msg = MQ_UNTOUCHED;

    h.Arm(&q);
    CHECK_EQUAL(WAIT_RESULT_TIMEOUT, q.PeekEx(&msg, 4));
    CHECK_EQUAL(WAIT_RESULT_TIMEOUT, q.PeekFrontEx(&msg, 4));
    CHECK_FALSE(q.Peek(&msg, 4));
    CHECK_FALSE(q.PeekFront(&msg, 4));

    h.Arm(&q).Add(3, MessageQueueContext::EV_CANCEL);
    CHECK_EQUAL(WAIT_RESULT_CANCELED, q.PeekEx(&msg, WAIT_INFINITE));

    h.Arm(&q).Add(3, MessageQueueContext::EV_CANCEL);
    CHECK_EQUAL(WAIT_RESULT_CANCELED, q.PeekFrontEx(&msg, WAIT_INFINITE));

    CHECK_EQUAL(MQ_UNTOUCHED, msg);
    CHECK_TRUE(q.IsEmpty());
}

// ---- producers: WaitNotFull() via PutEx() / PutFrontEx() ----

TEST(Sync, MessageQueue_PutEx_Blocked_WokenByGet)
{
    MessageQueueHarness h;
    TestQueue q;

    MqPush(q, 1U);
    MqPush(q, 2U);

    h.Arm(&q).Add(3, MessageQueueContext::EV_GET); // frees the slot of message 1

    uint32_t v = 3U;
    CHECK_EQUAL(WAIT_RESULT_SIGNAL, q.PutEx(&v, WAIT_INFINITE));

    CHECK_TRUE(q.IsFull());
    CHECK_EQUAL(2U, MqPop(q));
    CHECK_EQUAL(3U, MqPop(q));
}

TEST(Sync, MessageQueue_PutFrontEx_Blocked_WokenByGet)
{
    MessageQueueHarness h;
    TestQueue q;

    MqPush(q, 1U);
    MqPush(q, 2U);

    h.Arm(&q).Add(3, MessageQueueContext::EV_GET);

    uint32_t v = 9U;
    CHECK_EQUAL(WAIT_RESULT_SIGNAL, q.PutFrontEx(&v, 10));

    CHECK_TRUE(q.IsFull());
    CHECK_EQUAL(9U, MqPop(q)); // front-inserted
    CHECK_EQUAL(2U, MqPop(q));
}

TEST(Sync, MessageQueue_Put_Timeout_Canceled)
{
    MessageQueueHarness h;
    TestQueue q;

    MqPush(q, 1U);
    MqPush(q, 2U);

    uint32_t v = 3U;

    h.Arm(&q);
    CHECK_EQUAL(WAIT_RESULT_TIMEOUT, q.PutEx(&v, 4));
    CHECK_EQUAL(WAIT_RESULT_TIMEOUT, q.PutFrontEx(&v, 4));
    CHECK_FALSE(q.Put(&v, 4));
    CHECK_FALSE(q.PutFront(&v, 4));

    h.Arm(&q).Add(3, MessageQueueContext::EV_CANCEL);
    CHECK_EQUAL(WAIT_RESULT_CANCELED, q.PutEx(&v, WAIT_INFINITE));

    h.Arm(&q).Add(3, MessageQueueContext::EV_CANCEL);
    CHECK_EQUAL(WAIT_RESULT_CANCELED, q.PutFrontEx(&v, WAIT_INFINITE));

    // nothing was enqueued
    CHECK_EQUAL(2U, q.GetCount());
    CHECK_EQUAL(1U, MqPop(q));
    CHECK_EQUAL(2U, MqPop(q));
}

// the freed slot is taken by another producer before the waiter runs: it loops with the remaining time
TEST(Sync, MessageQueue_PutEx_Timed_SlotStolen_ThenEnqueued)
{
    MessageQueueHarness h;
    TestQueue q;

    MqPush(q, 1U);
    MqPush(q, 2U);

    MessageQueueContext &ctx = h.Arm(&q);
    ctx.Add(2, MessageQueueContext::EV_GET);      // frees a slot, wakes the producer
    ctx.Add(2, MessageQueueContext::EV_PUT, 5U);  // ... which is taken by someone else
    ctx.Add(5, MessageQueueContext::EV_GET);      // frees a slot again

    uint32_t v = 6U;
    CHECK_EQUAL(WAIT_RESULT_SIGNAL, q.PutEx(&v, 10));

    CHECK_EQUAL(5U, MqPop(q));
    CHECK_EQUAL(6U, MqPop(q));
    CHECK_TRUE(q.IsEmpty());
}

TEST(Sync, MessageQueue_PutEx_Timed_SlotStolen_ThenTimeout)
{
    MessageQueueHarness h;
    TestQueue q;

    MqPush(q, 1U);
    MqPush(q, 2U);

    MessageQueueContext &ctx = h.Arm(&q);
    ctx.Add(2, MessageQueueContext::EV_GET);
    ctx.Add(2, MessageQueueContext::EV_PUT, 5U);

    uint32_t v = 6U;
    CHECK_EQUAL(WAIT_RESULT_TIMEOUT, q.PutEx(&v, 6));

    CHECK_EQUAL(2U, MqPop(q));
    CHECK_EQUAL(5U, MqPop(q));
    CHECK_TRUE(q.IsEmpty());
}

// Reset() wakes blocked producers: the queue is empty and the waiting message is enqueued
TEST(Sync, MessageQueue_Reset_WakesBlockedProducer)
{
    MessageQueueHarness h;
    TestQueue q;

    MqPush(q, 1U);
    MqPush(q, 2U);

    h.Arm(&q).Add(3, MessageQueueContext::EV_RESET);

    uint32_t v = 7U;
    CHECK_EQUAL(WAIT_RESULT_SIGNAL, q.PutEx(&v, WAIT_INFINITE));

    CHECK_EQUAL(1U, q.GetCount());
    CHECK_EQUAL(7U, MqPop(q));
}

// ============================================================================ //
// =============================== sync::Event ================================ //
// ============================================================================ //

// Drives a real blocked waiter on a sync::Event: from the relax-CPU hook the "other party" runs a script of
// operations (Set / Reset / Pulse / cancel) at chosen ticks
static struct SyncEventContext
{
    enum EType
    {
        EV_SET,
        EV_RESET,
        EV_PULSE,
        EV_CANCEL // CancelTaskWait() on the waiter
    };

    struct Event
    {
        uint32_t tick;
        EType    type;
    };

    static const uint32_t EVENTS_MAX = 4;

    SyncEventContext()
    {
        Reset();
    }

    void Reset()
    {
        counter  = 0;
        count    = 0;
        platform = NULL;
        kernel   = NULL;
        task     = NULL;
        evt      = NULL;
    }

    void Add(uint32_t tick, EType type)
    {
        STK_ASSERT(count < EVENTS_MAX);

        events[count].tick = tick;
        events[count].type = type;
        ++count;
    }

    uint32_t           counter;
    uint32_t           count;
    Event              events[EVENTS_MAX];
    PlatformTestMock  *platform;
    IKernel           *kernel;
    ITask             *task;
    sync::Event       *evt;

    void Process()
    {
        platform->ProcessTick();
        ++counter;

        for (uint32_t i = 0; i < count; ++i)
        {
            if (events[i].tick != counter)
                continue;

            switch (events[i].type)
            {
            case EV_SET:    evt->Set();                    break;
            case EV_RESET:  evt->Reset();                  break;
            case EV_PULSE:  evt->Pulse();                  break;
            case EV_CANCEL: kernel->CancelTaskWait(task);  break;
            }
        }
    }
}
g_SyncEventContext;

static void SyncEventRelaxCpu()
{
    g_SyncEventContext.Process();
}

struct SyncEventHarness
{
    SyncEventHarness() : prev_handler(g_RelaxCpuHandler)
    {
        kernel.Initialize();
        kernel.AddTask(&task);
        kernel.Start();

        Arm(NULL);
        g_RelaxCpuHandler = SyncEventRelaxCpu;
    }

    ~SyncEventHarness()
    {
        g_RelaxCpuHandler = prev_handler;
    }

    // resets the script, the caller then adds events
    SyncEventContext &Arm(sync::Event *evt)
    {
        g_SyncEventContext.Reset();
        g_SyncEventContext.platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
        g_SyncEventContext.kernel   = &kernel;
        g_SyncEventContext.task     = &task;
        g_SyncEventContext.evt      = evt;
        return g_SyncEventContext;
    }

    Kernel<KERNEL_STATIC | KERNEL_SYNC, 1, sched::SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;
    decltype(g_RelaxCpuHandler) prev_handler;
};

// ---- non-blocking behavior ----

TEST(Sync, Event_Set_Reset_ReturnValues)
{
    SyncEventHarness h;
    sync::Event evt;

    CHECK_FALSE(evt.TryWait());

    CHECK_FALSE(evt.Reset()); // already non-signaled
    CHECK_TRUE(evt.Set());    // state changed
    CHECK_FALSE(evt.Set());   // already signaled
    CHECK_TRUE(evt.Reset());  // state changed
    CHECK_FALSE(evt.Reset());
}

TEST(Sync, Event_InitialState)
{
    SyncEventHarness h;

    sync::Event not_signaled(false, false);
    CHECK_FALSE(not_signaled.TryWait());

    sync::Event signaled(false, true);
    CHECK_FALSE(signaled.Set()); // already signaled
    CHECK_TRUE(signaled.TryWait());
}

TEST(Sync, Event_AutoReset_Wait_ConsumesSignal)
{
    SyncEventHarness h;
    sync::Event evt(false, true);

    // fast path: signaled, consumed by the wait
    CHECK_EQUAL(WAIT_RESULT_SIGNAL, evt.WaitEx(NO_WAIT));
    CHECK_EQUAL(WAIT_RESULT_TIMEOUT, evt.WaitEx(NO_WAIT));
    CHECK_FALSE(evt.TryWait());

    evt.Set();
    CHECK_TRUE(evt.Wait(NO_WAIT));
    CHECK_FALSE(evt.Wait(NO_WAIT));
}

TEST(Sync, Event_AutoReset_TryWait_ConsumesSignal)
{
    SyncEventHarness h;
    sync::Event evt;

    evt.Set();

    CHECK_TRUE(evt.TryWait());
    CHECK_FALSE(evt.TryWait());
}

TEST(Sync, Event_ManualReset_Wait_KeepsSignal)
{
    SyncEventHarness h;
    sync::Event evt(true);

    evt.Set();

    CHECK_EQUAL(WAIT_RESULT_SIGNAL, evt.WaitEx(NO_WAIT));
    CHECK_EQUAL(WAIT_RESULT_SIGNAL, evt.WaitEx(NO_WAIT));
    CHECK_TRUE(evt.TryWait());
    CHECK_TRUE(evt.TryWait());

    CHECK_TRUE(evt.Reset());
    CHECK_EQUAL(WAIT_RESULT_TIMEOUT, evt.WaitEx(NO_WAIT));
    CHECK_FALSE(evt.TryWait());
}

TEST(Sync, Event_Pulse_NoWaiters_LeavesNonSignaled)
{
    SyncEventHarness h;

    sync::Event auto_evt;
    auto_evt.Pulse();
    CHECK_FALSE(auto_evt.TryWait());

    sync::Event manual_evt(true);
    manual_evt.Pulse();
    CHECK_FALSE(manual_evt.TryWait());

    // pulse also clears an already signaled event
    auto_evt.Set();
    auto_evt.Pulse();
    CHECK_FALSE(auto_evt.TryWait());

    manual_evt.Set();
    manual_evt.Pulse();
    CHECK_FALSE(manual_evt.TryWait());
}

// ---- blocking behavior ----

TEST(Sync, Event_AutoReset_Blocked_WokenBySet_ConsumesSignal)
{
    SyncEventHarness h;
    sync::Event evt;

    h.Arm(&evt).Add(3, SyncEventContext::EV_SET);

    CHECK_EQUAL(WAIT_RESULT_SIGNAL, evt.WaitEx(WAIT_INFINITE));

    // auto-reset is applied when the waiter is removed from the wait list: the signal was consumed by the waiter
    CHECK_FALSE(evt.TryWait());
    CHECK_TRUE(evt.Set()); // non-signaled again
}

TEST(Sync, Event_AutoReset_Timed_WokenBySet)
{
    SyncEventHarness h;
    sync::Event evt;

    h.Arm(&evt).Add(3, SyncEventContext::EV_SET);

    CHECK_TRUE(evt.Wait(10));
    CHECK_FALSE(evt.TryWait());
}

TEST(Sync, Event_ManualReset_Blocked_WokenBySet_StaysSignaled)
{
    SyncEventHarness h;
    sync::Event evt(true);

    h.Arm(&evt).Add(3, SyncEventContext::EV_SET);

    CHECK_EQUAL(WAIT_RESULT_SIGNAL, evt.WaitEx(WAIT_INFINITE));

    // manual-reset: the state remains signaled
    CHECK_TRUE(evt.TryWait());
    CHECK_FALSE(evt.Set());
}

TEST(Sync, Event_AutoReset_Blocked_WokenByPulse)
{
    SyncEventHarness h;
    sync::Event evt;

    h.Arm(&evt).Add(3, SyncEventContext::EV_PULSE);

    CHECK_EQUAL(WAIT_RESULT_SIGNAL, evt.WaitEx(WAIT_INFINITE));

    // pulse releases the waiter and leaves the event non-signaled
    CHECK_FALSE(evt.TryWait());
    CHECK_TRUE(evt.Set());
}

TEST(Sync, Event_ManualReset_Blocked_WokenByPulse)
{
    SyncEventHarness h;
    sync::Event evt(true);

    h.Arm(&evt).Add(3, SyncEventContext::EV_PULSE);

    CHECK_EQUAL(WAIT_RESULT_SIGNAL, evt.WaitEx(WAIT_INFINITE));

    CHECK_FALSE(evt.TryWait());
    CHECK_TRUE(evt.Set());
}

// timeout: nothing consumed, no signal
TEST(Sync, Event_Wait_Timeout)
{
    SyncEventHarness h;

    sync::Event auto_evt;
    h.Arm(&auto_evt);
    CHECK_EQUAL(WAIT_RESULT_TIMEOUT, auto_evt.WaitEx(4));
    CHECK_FALSE(auto_evt.Wait(4));
    CHECK_FALSE(auto_evt.TryWait());

    sync::Event manual_evt(true);
    h.Arm(&manual_evt);
    CHECK_EQUAL(WAIT_RESULT_TIMEOUT, manual_evt.WaitEx(4));
    CHECK_FALSE(manual_evt.TryWait());

    // a timed-out wait leaves the event usable
    CHECK_TRUE(auto_evt.Set());
    CHECK_TRUE(auto_evt.TryWait());
}

TEST(Sync, Event_Wait_Canceled)
{
    SyncEventHarness h;
    sync::Event evt;

    h.Arm(&evt).Add(3, SyncEventContext::EV_CANCEL);
    CHECK_EQUAL(WAIT_RESULT_CANCELED, evt.WaitEx(WAIT_INFINITE));

    // the bool overload cannot tell a cancellation from a timeout
    h.Arm(&evt).Add(3, SyncEventContext::EV_CANCEL);
    CHECK_FALSE(evt.Wait(WAIT_INFINITE));

    // the event state is not consumed / changed
    CHECK_FALSE(evt.TryWait());
    CHECK_TRUE(evt.Set());
    CHECK_TRUE(evt.TryWait());
}

// the kernel rejects the blocking wait (scheduler locked)
TEST(Sync, Event_Wait_Failed_SchedulerLocked)
{
    SyncEventHarness h;
    sync::Event evt;

    h.Arm(&evt);

    IKernelService::GetInstance()->LockScheduler();

    // waiting on locked scheduler causes an assertion by IKernelService::Wait,
    // so we test this behavior first
    try
    {
        sync::Event assert_evt;

        g_TestContext.ExpectAssert(true);
        STK_UNUSED(assert_evt.WaitEx(WAIT_INFINITE));
        CHECK(false); // should asssert in a DEBUG build
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    EWaitResult wresult = WAIT_RESULT_SIGNAL;

    // in RELEASE build assert is inactive, so we expect a failure result
    {
        g_TestContext.ExpectAssert(true);
        g_TestContext.RethrowAssertException(false);

        wresult = evt.WaitEx(WAIT_INFINITE);

        g_TestContext.RethrowAssertException(true);
        g_TestContext.ExpectAssert(false);
    }

    IKernelService::GetInstance()->UnlockScheduler();

    CHECK_EQUAL(WAIT_RESULT_FAIL, wresult);
    CHECK_FALSE(evt.TryWait());
}

// ---- ISR ----

// ISR-safe operations: Set/Reset/Pulse/TryWait and WaitEx(NO_WAIT)
TEST(Sync, Event_InsideISR_NonBlockingOperations)
{
    SyncEventHarness h;
    sync::Event evt;

    g_InsideISR = true;

    const bool set1   = evt.Set();
    const bool set2   = evt.Set();
    const EWaitResult w1 = evt.WaitEx(NO_WAIT); // consumes
    const EWaitResult w2 = evt.WaitEx(NO_WAIT); // non-signaled
    evt.Set();
    const bool try1   = evt.TryWait();
    const bool try2   = evt.TryWait();
    evt.Set();
    const bool reset1 = evt.Reset();
    evt.Pulse();
    const bool try3   = evt.TryWait();

    g_InsideISR = false;

    CHECK_TRUE(set1);
    CHECK_FALSE(set2);
    CHECK_EQUAL(WAIT_RESULT_SIGNAL, w1);
    CHECK_EQUAL(WAIT_RESULT_TIMEOUT, w2);
    CHECK_TRUE(try1);
    CHECK_FALSE(try2);
    CHECK_TRUE(reset1);
    CHECK_FALSE(try3);
}

// a blocking wait inside an ISR violates the API contract
TEST(Sync, Event_InsideISR_BlockingWait_Asserts)
{
    SyncEventHarness h;
    sync::Event evt;

    // a signaled event does not block, so no assertion
    evt.Set();
    g_InsideISR = true;
    const EWaitResult fast = evt.WaitEx(WAIT_INFINITE);
    g_InsideISR = false;
    CHECK_EQUAL(WAIT_RESULT_SIGNAL, fast);

    try
    {
        g_InsideISR = true;
        g_TestContext.ExpectAssert(true);
        STK_UNUSED(evt.WaitEx(2)); // finite, so that a build without assertions cannot block forever
        g_InsideISR = false;
        CHECK(false); // should asssert in a DEBUG build
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_InsideISR = false;
        g_TestContext.ExpectAssert(false);
    }
}

// ============================================================================ //
// ============================== sync::Semaphore ============================= //
// ============================================================================ //

// Drives a real blocked waiter on a sync::Semaphore: from the relax-CPU hook the "other party" runs a script of
// signal operations and/or cancels the waiter at chosen ticks
static struct SemaphoreContext
{
    enum EType
    {
        EV_SIGNAL,       // Signal()
        EV_TRY_SIGNAL,   // TrySignal()
        EV_TRY_SIGNAL_C, // TrySignal(value)
        EV_CANCEL        // CancelTaskWait() on the waiter
    };

    struct Event
    {
        uint32_t tick;
        EType    type;
        uint32_t value;
    };

    static const uint32_t EVENTS_MAX = 4;

    SemaphoreContext()
    {
        Reset();
    }

    void Reset()
    {
        counter    = 0;
        count      = 0;
        platform   = NULL;
        kernel     = NULL;
        task       = NULL;
        sem        = NULL;
        try_result = false;
    }

    void Add(uint32_t tick, EType type, uint32_t value = 0U)
    {
        STK_ASSERT(count < EVENTS_MAX);

        events[count].tick  = tick;
        events[count].type  = type;
        events[count].value = value;
        ++count;
    }

    uint32_t           counter;
    uint32_t           count;
    Event              events[EVENTS_MAX];
    PlatformTestMock  *platform;
    IKernel           *kernel;
    ITask             *task;
    sync::Semaphore   *sem;
    bool               try_result; // result of the last TrySignal() executed by the script

    void Process()
    {
        platform->ProcessTick();
        ++counter;

        for (uint32_t i = 0; i < count; ++i)
        {
            if (events[i].tick != counter)
                continue;

            switch (events[i].type)
            {
            case EV_SIGNAL:       sem->Signal();                                                   break;
            case EV_TRY_SIGNAL:   try_result = sem->TrySignal();                                   break;
            case EV_TRY_SIGNAL_C: try_result = sem->TrySignal(static_cast<uint16_t>(events[i].value)); break;
            case EV_CANCEL:       kernel->CancelTaskWait(task);                                    break;
            }
        }
    }
}
g_SemaphoreContext;

static void SemaphoreRelaxCpu()
{
    g_SemaphoreContext.Process();
}

struct SemaphoreHarness
{
    SemaphoreHarness() : prev_handler(g_RelaxCpuHandler)
    {
        kernel.Initialize();
        kernel.AddTask(&task);
        kernel.Start();

        Arm(NULL);
        g_RelaxCpuHandler = SemaphoreRelaxCpu;
    }

    ~SemaphoreHarness()
    {
        g_RelaxCpuHandler = prev_handler;
    }

    // resets the script, the caller then adds events
    SemaphoreContext &Arm(sync::Semaphore *sem)
    {
        g_SemaphoreContext.Reset();
        g_SemaphoreContext.platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
        g_SemaphoreContext.kernel   = &kernel;
        g_SemaphoreContext.task     = &task;
        g_SemaphoreContext.sem      = sem;
        return g_SemaphoreContext;
    }

    Kernel<KERNEL_STATIC | KERNEL_SYNC, 1, sched::SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;
    decltype(g_RelaxCpuHandler) prev_handler;
};

// ---- construction / non-blocking behavior ----

TEST(Sync, Semaphore_Construction)
{
    SemaphoreHarness h;

    sync::Semaphore def;
    CHECK_EQUAL(0U, def.GetCount());
    CHECK_FALSE(def.TryWait());

    sync::Semaphore init(3U);
    CHECK_EQUAL(3U, init.GetCount());

    sync::Semaphore limited(1U, 2U);
    CHECK_EQUAL(1U, limited.GetCount());

    CHECK_EQUAL(0xFFFFU, sync::Semaphore::COUNT_MAX);
    CHECK_EQUAL(0xFFU, sync::SemaphoreT<uint8_t>::COUNT_MAX);
    CHECK_EQUAL(0xFFFFFFFFU, sync::SemaphoreT<uint32_t>::COUNT_MAX);
}

TEST(Sync, Semaphore_Construction_InitialAboveMax_Asserts)
{
    SemaphoreHarness h;

    try
    {
        g_TestContext.ExpectAssert(true);
        sync::Semaphore sem(3U, 2U);
        CHECK(false); // should asssert in a DEBUG build
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

TEST(Sync, Semaphore_Wait_NoWait_ConsumesTokens)
{
    SemaphoreHarness h;
    sync::Semaphore sem(2U);

    CHECK_EQUAL(WAIT_RESULT_SIGNAL, sem.WaitEx(NO_WAIT));
    CHECK_EQUAL(1U, sem.GetCount());

    CHECK_TRUE(sem.TryWait());
    CHECK_EQUAL(0U, sem.GetCount());

    // no token: immediately-expired timeout, nothing consumed
    CHECK_EQUAL(WAIT_RESULT_TIMEOUT, sem.WaitEx(NO_WAIT));
    CHECK_FALSE(sem.Wait(NO_WAIT));
    CHECK_FALSE(sem.TryWait());
    CHECK_EQUAL(0U, sem.GetCount());
}

TEST(Sync, Semaphore_Signal_IncrementsUpToMax)
{
    SemaphoreHarness h;
    sync::Semaphore sem(0U, 2U);

    sem.Signal();
    CHECK_EQUAL(1U, sem.GetCount());

    CHECK_TRUE(sem.TrySignal());
    CHECK_EQUAL(2U, sem.GetCount());

    // saturated, nobody waiting: TrySignal() fails and the counter is unchanged
    CHECK_FALSE(sem.TrySignal());
    CHECK_EQUAL(2U, sem.GetCount());

    // the tokens are consumable again afterwards
    CHECK_TRUE(sem.TryWait());
    CHECK_TRUE(sem.TryWait());
    CHECK_FALSE(sem.TryWait());
}

// Signal() above max_count is an API contract violation
TEST(Sync, Semaphore_Signal_AboveMax_Asserts)
{
    SemaphoreHarness h;
    sync::Semaphore sem(1U, 1U);

    try
    {
        g_TestContext.ExpectAssert(true);
        sem.Signal();
        CHECK(false); // should asssert in a DEBUG build
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    // the counter was not changed
    CHECK_EQUAL(1U, sem.GetCount());
}

TEST(Sync, Semaphore_TrySignal_Ceiling)
{
    SemaphoreHarness h;
    sync::Semaphore sem(0U, 10U);

    // per-call ceiling lower than the object maximum limits the counter
    CHECK_TRUE(sem.TrySignal(static_cast<uint16_t>(2U)));
    CHECK_TRUE(sem.TrySignal(static_cast<uint16_t>(2U)));
    CHECK_EQUAL(2U, sem.GetCount());
    CHECK_FALSE(sem.TrySignal(static_cast<uint16_t>(2U)));
    CHECK_EQUAL(2U, sem.GetCount());

    // ceiling below the current count: rejected
    CHECK_FALSE(sem.TrySignal(static_cast<uint16_t>(1U)));
    CHECK_FALSE(sem.TrySignal(static_cast<uint16_t>(0U)));
    CHECK_EQUAL(2U, sem.GetCount());

    // ceiling above the object maximum: the maximum wins
    CHECK_TRUE(sem.TrySignal(static_cast<uint16_t>(0xFFFFU)));
    CHECK_EQUAL(3U, sem.GetCount());

    sync::Semaphore small(0U, 1U);
    CHECK_TRUE(small.TrySignal(static_cast<uint16_t>(100U)));
    CHECK_FALSE(small.TrySignal(static_cast<uint16_t>(100U)));
    CHECK_EQUAL(1U, small.GetCount());
}

TEST(Sync, Semaphore_CounterTypes)
{
    SemaphoreHarness h;

    // 8-bit counter: saturates at its type maximum
    sync::SemaphoreT<uint8_t> s8(254U);
    CHECK_TRUE(s8.TrySignal());
    CHECK_EQUAL(255U, s8.GetCount());
    CHECK_FALSE(s8.TrySignal());
    CHECK_TRUE(s8.TryWait());
    CHECK_EQUAL(254U, s8.GetCount());

    // 32-bit counter
    sync::SemaphoreT<uint32_t> s32(0xFFFFFFFEU);
    CHECK_TRUE(s32.TrySignal());
    CHECK_EQUAL(0xFFFFFFFFU, s32.GetCount());
    CHECK_FALSE(s32.TrySignal());
    CHECK_FALSE(s32.TrySignal(0x10U));

    // 64-bit counter (GetCount() reads under a critical section where wider than a pointer)
    sync::SemaphoreT<uint64_t> s64(0x1FFFFFFFFULL);
    CHECK_TRUE(s64.TrySignal());
    CHECK_TRUE(s64.GetCount() == 0x200000000ULL);
    CHECK_TRUE(s64.TryWait());
    CHECK_TRUE(s64.GetCount() == 0x1FFFFFFFFULL);
    CHECK_FALSE(s64.TrySignal(static_cast<uint64_t>(0x1FFFFFFFFULL))); // ceiling equal to the count: rejected
    CHECK_TRUE(s64.TrySignal(static_cast<uint64_t>(0x200000000ULL)));  // ceiling above the count
}

// ---- blocking behavior ----

// direct handover: the waiter receives the token, the counter is not incremented
TEST(Sync, Semaphore_Blocked_WokenBySignal_DirectHandover)
{
    SemaphoreHarness h;
    sync::Semaphore sem;

    h.Arm(&sem).Add(3, SemaphoreContext::EV_SIGNAL);

    CHECK_EQUAL(WAIT_RESULT_SIGNAL, sem.WaitEx(WAIT_INFINITE));
    CHECK_EQUAL(0U, sem.GetCount());
}

TEST(Sync, Semaphore_Timed_WokenByTrySignal_DirectHandover)
{
    SemaphoreHarness h;
    sync::Semaphore sem;

    h.Arm(&sem).Add(3, SemaphoreContext::EV_TRY_SIGNAL);

    CHECK_TRUE(sem.Wait(10));
    CHECK_TRUE(g_SemaphoreContext.try_result);
    CHECK_EQUAL(0U, sem.GetCount());
}

// hand-off to a waiter does not depend on the counter limits
TEST(Sync, Semaphore_Blocked_WokenByTrySignal_CeilingIgnoredForWaiter)
{
    SemaphoreHarness h;
    sync::Semaphore sem(0U, 1U);

    h.Arm(&sem).Add(3, SemaphoreContext::EV_TRY_SIGNAL_C, 0U);

    CHECK_EQUAL(WAIT_RESULT_SIGNAL, sem.WaitEx(WAIT_INFINITE));
    CHECK_TRUE(g_SemaphoreContext.try_result);
    CHECK_EQUAL(0U, sem.GetCount());

    // with nobody waiting the same ceiling is enforced
    CHECK_FALSE(sem.TrySignal(static_cast<uint16_t>(0U)));
    CHECK_EQUAL(0U, sem.GetCount());
}

// a waiter exists only while the counter is zero, so a hand-off never overflows max_count (even for max_count == 1)
TEST(Sync, Semaphore_Blocked_WokenBySignal_MaxOne)
{
    SemaphoreHarness h;
    sync::Semaphore sem(0U, 1U);

    h.Arm(&sem).Add(3, SemaphoreContext::EV_SIGNAL);

    CHECK_TRUE(sem.Wait(WAIT_INFINITE));
    CHECK_EQUAL(0U, sem.GetCount());

    sem.Signal();
    CHECK_EQUAL(1U, sem.GetCount());
}

TEST(Sync, Semaphore_Wait_Timeout)
{
    SemaphoreHarness h;
    sync::Semaphore sem;

    h.Arm(&sem);
    CHECK_EQUAL(WAIT_RESULT_TIMEOUT, sem.WaitEx(4));
    CHECK_FALSE(sem.Wait(4));
    CHECK_EQUAL(0U, sem.GetCount());

    // a timed-out wait does not leave a stale waiter: the next signal is stored
    sem.Signal();
    CHECK_EQUAL(1U, sem.GetCount());
}

TEST(Sync, Semaphore_Wait_Canceled)
{
    SemaphoreHarness h;
    sync::Semaphore sem;

    h.Arm(&sem).Add(3, SemaphoreContext::EV_CANCEL);
    CHECK_EQUAL(WAIT_RESULT_CANCELED, sem.WaitEx(WAIT_INFINITE));

    // the bool overload cannot tell a cancellation from a timeout
    h.Arm(&sem).Add(3, SemaphoreContext::EV_CANCEL);
    CHECK_FALSE(sem.Wait(WAIT_INFINITE));

    // no token was consumed and no stale waiter is left behind
    CHECK_EQUAL(0U, sem.GetCount());
    CHECK_TRUE(sem.TrySignal());
    CHECK_EQUAL(1U, sem.GetCount());
}

// the kernel rejects the blocking wait (scheduler locked)
TEST(Sync, Semaphore_Wait_Failed_SchedulerLocked)
{
    SemaphoreHarness h;
    sync::Semaphore sem;

    h.Arm(&sem);

    IKernelService::GetInstance()->LockScheduler();

    // waiting on locked scheduler causes an assertion by IKernelService::Wait,
    // so we test this behavior first
    try
    {
        sync::Semaphore assert_sem;

        g_TestContext.ExpectAssert(true);
        STK_UNUSED(assert_sem.WaitEx(WAIT_INFINITE));
        CHECK(false); // should asssert in a DEBUG build
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    EWaitResult wresult = WAIT_RESULT_SIGNAL;

    // in RELEASE build assert is inactive, so we expect a failure result
    {
        g_TestContext.ExpectAssert(true);
        g_TestContext.RethrowAssertException(false);

        wresult = sem.WaitEx(WAIT_INFINITE);

        g_TestContext.RethrowAssertException(true);
        g_TestContext.ExpectAssert(false);
    }

    IKernelService::GetInstance()->UnlockScheduler();

    CHECK_EQUAL(WAIT_RESULT_FAIL, wresult);
    CHECK_EQUAL(0U, sem.GetCount());
}

// ---- ISR ----

// ISR-safe operations: TryWait / Wait(NO_WAIT) / Signal / TrySignal
TEST(Sync, Semaphore_InsideISR_NonBlockingOperations)
{
    SemaphoreHarness h;
    sync::Semaphore sem(0U, 2U);

    g_InsideISR = true;

    sem.Signal();
    const bool try1 = sem.TrySignal();
    const bool try2 = sem.TrySignal(); // saturated
    const bool w1   = sem.TryWait();
    const EWaitResult w2 = sem.WaitEx(NO_WAIT);
    const EWaitResult w3 = sem.WaitEx(NO_WAIT); // empty

    g_InsideISR = false;

    CHECK_TRUE(try1);
    CHECK_FALSE(try2);
    CHECK_TRUE(w1);
    CHECK_EQUAL(WAIT_RESULT_SIGNAL, w2);
    CHECK_EQUAL(WAIT_RESULT_TIMEOUT, w3);
    CHECK_EQUAL(0U, sem.GetCount());
}

// a blocking wait inside an ISR violates the API contract
TEST(Sync, Semaphore_InsideISR_BlockingWait_Asserts)
{
    SemaphoreHarness h;
    sync::Semaphore sem;

    // a token is available, so there is nothing to block on and no assertion
    sem.Signal();
    g_InsideISR = true;
    const EWaitResult fast = sem.WaitEx(WAIT_INFINITE);
    g_InsideISR = false;
    CHECK_EQUAL(WAIT_RESULT_SIGNAL, fast);

    try
    {
        g_InsideISR = true;
        g_TestContext.ExpectAssert(true);
        STK_UNUSED(sem.WaitEx(2)); // finite, so that a build without assertions cannot block forever
        g_InsideISR = false;
        CHECK(false); // should asssert in a DEBUG build
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_InsideISR = false;
        g_TestContext.ExpectAssert(false);
    }
}

// ============================================================================ //
// ====================== sync::Mutex / sync::FastMutex ======================= //
// ============================================================================ //

// A mutex needs a second task as the owner, which a single mock task cannot be. Both mutexes talk to the kernel only
// through IKernelService, so a fake service is installed instead of a kernel: it provides the id of the "current task"
// (settable), simulates blocked waiters (wait list entries) and scripts the outcome of Wait(). Together with the
// recording of the priority-inheritance calls this drives every path of the mutex logic.

static const TId MTX_OWNER  = 0x10; // another task, owns the mutex
static const TId MTX_CALLER = 0x20; // the "current task" by default

struct MtxServiceFake : public KernelServiceMock
{
    static const uint32_t SCRIPT_MAX = 4;
    static const uint32_t STUBS_MAX  = 8;

    MtxServiceFake()
    {
        wait_calls = 0; last_wait_timeout = 0; last_wait_sobj = NULL;
        script_n = 0; script_idx = 0; handover = true; owner_tid = MTX_OWNER; unlocker = NULL;
        inherit_calls = 0; last_inherit_tid = TID_NONE; last_inherit_donor = TID_NONE;
        inherit_sobj_calls = 0; last_inherit_sobj_tid = TID_NONE; last_inherit_sobj = NULL;
        restore_calls = 0; last_restore_tid = TID_NONE; last_restore_sobj = NULL;
        stub_n = 0;
        m_tid = MTX_CALLER;
    }

    ~MtxServiceFake()
    {
        for (uint32_t i = 0; i < stub_n; ++i)
        {
            if (stubs[i]->m_sobj != NULL)
                stubs[i]->m_sobj->RemoveWaitObject(stubs[i]);

            delete stubs[i];
        }
    }

    // outcomes of the consecutive Wait() calls (the default is a timeout)
    void Script(EWaitResult r1, EWaitResult r2 = WAIT_RESULT_TIMEOUT, EWaitResult r3 = WAIT_RESULT_TIMEOUT)
    {
        script[0] = r1; script[1] = r2; script[2] = r3;
        script_n = 3; script_idx = 0;
    }

    // adds a blocked task to the wait list of an object
    WaitObjectStub *AddWaiter(ISyncObject *sobj, TId tid)
    {
        STK_ASSERT(stub_n < STUBS_MAX);

        WaitObjectStub *const w = new WaitObjectStub(tid);
        stubs[stub_n++] = w;

        sobj->AddWaitObject(w);
        w->m_sobj = sobj;
        return w;
    }

    EWaitResult Wait(ISyncObject *sobj, IMutex *, Timeout timeout) override
    {
        ++wait_calls;
        last_wait_timeout = timeout;
        last_wait_sobj    = sobj;

        EWaitResult r = WAIT_RESULT_TIMEOUT;
        if (script_idx < script_n)
            r = script[script_idx++];

        // the caller is a blocked waiter while the wait lasts
        WaitObjectStub *const w = AddWaiter(sobj, static_cast<TId>(m_tid));

        // actions of the other tasks which happen while the caller is blocked
        if (on_wait)
            on_wait(wait_calls);

        if ((r == WAIT_RESULT_SIGNAL) && handover && (unlocker != NULL))
        {
            // the owner releases the mutex: ownership is handed over to the waiter (us)
            const size_t saved = m_tid;
            m_tid = owner_tid;
            unlocker->Unlock();
            m_tid = saved;
        }
        else if (w->m_sobj != NULL)
        {
            // the wait ended without a hand-over: the kernel removes the waiter from the list
            w->m_sobj->RemoveWaitObject(w);
            w->m_sobj = NULL;
        }

        return r;
    }

    // wakes the first waiter of the object, the kernel selection is reduced to FIFO
    TId Wake(ISyncObject *sobj, bool all) override
    {
        TId first = TID_NONE;

        for (uint32_t i = 0; i < stub_n; ++i)
        {
            if (stubs[i]->m_sobj == sobj)
            {
                if (first == TID_NONE)
                    first = stubs[i]->m_tid;

                stubs[i]->Wake(false);

                if (!all)
                    break;
            }
        }

        return first;
    }

    void InheritWeight(TId tid, TId donor_tid) override
    {
        ++inherit_calls;
        last_inherit_tid   = tid;
        last_inherit_donor = donor_tid;
    }

    void InheritWeight(TId tid, const ISyncObject *sobj) override
    {
        ++inherit_sobj_calls;
        last_inherit_sobj_tid = tid;
        last_inherit_sobj     = sobj;
    }

    void RestoreWeight(TId tid, ISyncObject *sobj) override
    {
        ++restore_calls;
        last_restore_tid  = tid;
        last_restore_sobj = sobj;
    }

    std::function<void(uint32_t)> on_wait; // called inside Wait() with the 1-based number of the call
    uint32_t            wait_calls;
    Timeout             last_wait_timeout;
    ISyncObject        *last_wait_sobj;
    EWaitResult         script[SCRIPT_MAX];
    uint32_t            script_n;
    uint32_t            script_idx;
    bool                handover;   // Wait(SIGNAL) is accompanied by a real Unlock() of the owner
    TId                 owner_tid;  // the task which releases the mutex in Wait(SIGNAL)
    IMutex             *unlocker;   // the mutex under test
    uint32_t            inherit_calls;
    TId                 last_inherit_tid;
    TId                 last_inherit_donor;
    uint32_t            inherit_sobj_calls;
    TId                 last_inherit_sobj_tid;
    const ISyncObject  *last_inherit_sobj;
    uint32_t            restore_calls;
    TId                 last_restore_tid;
    ISyncObject        *last_restore_sobj;
    WaitObjectStub     *stubs[STUBS_MAX];
    uint32_t            stub_n;
};

// installs the fake as the kernel service for the scope of a test (declare it before the mutex under test)
struct MtxFixture
{
    MtxFixture() : prev(g_KernelService)
    {
        g_KernelService = &fake;
    }

    ~MtxFixture()
    {
        g_KernelService = prev;
    }

    MtxServiceFake  fake;
    IKernelService *prev;
};

// takes the mutex on behalf of another task (without waiting)
template <class M>
static void MtxLockAs(MtxFixture &f, M &m, TId tid)
{
    f.fake.m_tid = tid;
    CHECK_TRUE(m.TryLock());
    f.fake.m_tid = MTX_CALLER;
}

// runs a body for both mutexes: they share the IMutex logic (ownership, hand-over, wait outcomes)
#define MTX_TEST(name) \
    template <class M> static void Test_Mtx_##name(); \
    TEST(Sync, Mutex_##name)     { Test_Mtx_##name<sync::Mutex>(); } \
    TEST(Sync, FastMutex_##name) { Test_Mtx_##name<sync::FastMutex>(); } \
    template <class M> static void Test_Mtx_##name()

#define MTX_SOBJ(m) static_cast<ISyncObject *>(&(m))

MTX_TEST(FreeLock_Unlock)
{
    MtxFixture f;
    M m;

    CHECK_FALSE(m.IsLocked());
    CHECK_EQUAL(TID_NONE, m.GetOwner());

    CHECK_TRUE(m.TryLock());
    CHECK_TRUE(m.IsLocked());
    CHECK_EQUAL(MTX_CALLER, m.GetOwner());

    // an uncontended lock never involves the kernel wait
    CHECK_EQUAL(0, f.fake.wait_calls);
    CHECK_EQUAL(0, f.fake.inherit_calls);

    m.Unlock();
    CHECK_FALSE(m.IsLocked());
    CHECK_EQUAL(TID_NONE, m.GetOwner());

    m.Lock();
    CHECK_EQUAL(MTX_CALLER, m.GetOwner());
    m.Unlock();

    CHECK_EQUAL(WAIT_RESULT_SIGNAL, m.TimedLockEx(5));
    CHECK_EQUAL(0, f.fake.wait_calls);
    m.Unlock();

    CHECK_EQUAL(0, test::g_CriticalSectionState);
}

// NO_WAIT on a mutex owned by another task: immediately-expired timeout, no wait, no priority boost
MTX_TEST(Contested_NoWait)
{
    MtxFixture f;
    M m;

    MtxLockAs(f, m, MTX_OWNER);

    CHECK_FALSE(m.TryLock());
    CHECK_FALSE(m.TimedLock(NO_WAIT));
    CHECK_EQUAL(WAIT_RESULT_TIMEOUT, m.TimedLockEx(NO_WAIT));

    CHECK_EQUAL(0, f.fake.wait_calls);
    CHECK_EQUAL(0, f.fake.inherit_calls);
    CHECK_EQUAL(MTX_OWNER, m.GetOwner());
}

// the owner releases the mutex while the caller is blocked: ownership is handed over directly
MTX_TEST(Contested_Blocked_OwnershipHandedOver)
{
    MtxFixture f;
    M m;

    MtxLockAs(f, m, MTX_OWNER);

    f.fake.unlocker = &m;
    f.fake.Script(WAIT_RESULT_SIGNAL);

    CHECK_EQUAL(WAIT_RESULT_SIGNAL, m.TimedLockEx(10));

    CHECK_EQUAL(1, f.fake.wait_calls);
    CHECK_EQUAL(10, f.fake.last_wait_timeout);
    CHECK_TRUE(f.fake.last_wait_sobj == MTX_SOBJ(m));

    // the owner was boosted by the weight of the caller before it blocked
    CHECK_EQUAL(1, f.fake.inherit_calls);
    CHECK_EQUAL(MTX_OWNER, f.fake.last_inherit_tid);
    CHECK_EQUAL(MTX_CALLER, f.fake.last_inherit_donor);

    CHECK_EQUAL(MTX_CALLER, m.GetOwner());
    CHECK_TRUE(m.IsLocked());

    m.Unlock();
    CHECK_FALSE(m.IsLocked());
    CHECK_EQUAL(0, test::g_CriticalSectionState);
}

// a wait which did not end with a signal: not the owner, the boost of the owner is undone
template <class M>
static void Test_Mtx_WaitFailure(EWaitResult outcome)
{
    MtxFixture f;
    M m;

    MtxLockAs(f, m, MTX_OWNER);

    f.fake.Script(outcome, outcome);

    CHECK_EQUAL(outcome, m.TimedLockEx(10));

    CHECK_EQUAL(1, f.fake.wait_calls);
    CHECK_EQUAL(1, f.fake.inherit_calls);

    // the owner did not change: the priority boost is rolled back through the mutex
    CHECK_EQUAL(1, f.fake.restore_calls);
    CHECK_EQUAL(MTX_OWNER, f.fake.last_restore_tid);
    CHECK_TRUE(f.fake.last_restore_sobj == MTX_SOBJ(m));

    CHECK_EQUAL(MTX_OWNER, m.GetOwner());

    // the bool overload cannot tell the outcomes apart
    CHECK_FALSE(m.TimedLock(10));
    CHECK_EQUAL(MTX_OWNER, m.GetOwner());
    CHECK_EQUAL(0, test::g_CriticalSectionState);
}

MTX_TEST(Wait_Timeout)  { Test_Mtx_WaitFailure<M>(WAIT_RESULT_TIMEOUT);  }
MTX_TEST(Wait_Canceled) { Test_Mtx_WaitFailure<M>(WAIT_RESULT_CANCELED); }
MTX_TEST(Wait_Failed)   { Test_Mtx_WaitFailure<M>(WAIT_RESULT_FAIL);     }

// the mutex was destroyed while waiting: no member is touched, so nothing is restored
MTX_TEST(Wait_Deleted)
{
    MtxFixture f;
    M m;

    MtxLockAs(f, m, MTX_OWNER);

    f.fake.Script(WAIT_RESULT_DELETED);

    CHECK_EQUAL(WAIT_RESULT_DELETED, m.TimedLockEx(10));

    CHECK_EQUAL(1, f.fake.inherit_calls);
    CHECK_EQUAL(0, f.fake.restore_calls);
}

// void Lock() cannot be canceled: the cancellation is absorbed and the wait is retried
MTX_TEST(Lock_RetriesOnCancel)
{
    MtxFixture f;
    M m;

    MtxLockAs(f, m, MTX_OWNER);

    f.fake.unlocker = &m;
    f.fake.Script(WAIT_RESULT_CANCELED, WAIT_RESULT_CANCELED, WAIT_RESULT_SIGNAL);

    m.Lock();

    CHECK_EQUAL(3, f.fake.wait_calls);
    CHECK_EQUAL(WAIT_INFINITE, f.fake.last_wait_timeout);
    CHECK_EQUAL(MTX_CALLER, m.GetOwner());

    m.Unlock();
}

// void Lock() panics on any other failure
MTX_TEST(Lock_PanicsOnFailure)
{
    MtxFixture f;
    M m;

    MtxLockAs(f, m, MTX_OWNER);

    f.fake.Script(WAIT_RESULT_FAIL);

    g_TestContext.ExpectPanic(true);
    try
    {
        m.Lock();
    }
    catch (std::exception &)
    {}
    g_TestContext.ExpectPanic(false);

    CHECK_EQUAL((int)KERNEL_PANIC_ASSERT, (int)test::g_PanicValue);
}

// a signal without the ownership transfer is an internal defect
MTX_TEST(Wait_SignalWithoutHandover_Panics)
{
    MtxFixture f;
    M m;

    MtxLockAs(f, m, MTX_OWNER);

    f.fake.unlocker = &m;
    f.fake.handover = false;
    f.fake.Script(WAIT_RESULT_SIGNAL);

    g_TestContext.ExpectPanic(true);
    try
    {
        STK_UNUSED(m.TimedLockEx(10));
    }
    catch (std::exception &)
    {}
    g_TestContext.ExpectPanic(false);

    CHECK_EQUAL((int)KERNEL_PANIC_ASSERT, (int)test::g_PanicValue);
}

// Unlock() with waiters: ownership goes to the first waiter, the owner's boost is restored and the new owner is
// boosted by the waiters which are still blocked
MTX_TEST(Unlock_HandsOverToWaiters)
{
    MtxFixture f;
    M m;

    const TId W1 = 0x31, W2 = 0x32;

    CHECK_TRUE(m.TryLock());

    WaitObjectStub *w1 = f.fake.AddWaiter(MTX_SOBJ(m), W1);
    WaitObjectStub *w2 = f.fake.AddWaiter(MTX_SOBJ(m), W2);

    m.Unlock(); // by MTX_CALLER

    CHECK_EQUAL(W1, m.GetOwner());
    CHECK_TRUE(m.IsLocked());
    CHECK_EQUAL(1, w1->m_wake_count);
    CHECK_EQUAL(0, w2->m_wake_count);

    CHECK_EQUAL(1, f.fake.restore_calls);
    CHECK_EQUAL(MTX_CALLER, f.fake.last_restore_tid);

    // W2 is still waiting: the new owner inherits its weight
    CHECK_EQUAL(1, f.fake.inherit_sobj_calls);
    CHECK_EQUAL(W1, f.fake.last_inherit_sobj_tid);
    CHECK_TRUE(f.fake.last_inherit_sobj == MTX_SOBJ(m));

    f.fake.m_tid = W1;
    m.Unlock();

    CHECK_EQUAL(W2, m.GetOwner());
    CHECK_EQUAL(1, w2->m_wake_count);
    CHECK_EQUAL(1, f.fake.inherit_sobj_calls); // nobody is left waiting: no boost

    f.fake.m_tid = W2;
    m.Unlock();

    CHECK_FALSE(m.IsLocked());
    CHECK_EQUAL(TID_NONE, m.GetOwner());
    CHECK_EQUAL(0, test::g_CriticalSectionState);
}

// Unlock() by a task which does not own the mutex (or of a free mutex) violates the API contract
MTX_TEST(Unlock_NotOwner_Asserts)
{
    MtxFixture f;
    M m;

    // free mutex
    try
    {
        g_TestContext.ExpectAssert(true);
        m.Unlock();
        CHECK(false); // should asssert in a DEBUG build
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    // mutex owned by another task
    MtxLockAs(f, m, MTX_OWNER);

    try
    {
        g_TestContext.ExpectAssert(true);
        m.Unlock();
        CHECK(false); // should asssert in a DEBUG build
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    CHECK_EQUAL(MTX_OWNER, m.GetOwner());
    CHECK_EQUAL(0, test::g_CriticalSectionState);
}

// must be called from a task
MTX_TEST(Lock_OutsideTask_Asserts)
{
    MtxFixture f;
    M m;

    f.fake.m_tid = TID_NONE;

    try
    {
        g_TestContext.ExpectAssert(true);
        STK_UNUSED(m.TryLock());
        CHECK(false); // should asssert in a DEBUG build
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    CHECK_EQUAL(0, test::g_CriticalSectionState);
}

// ISR-safe: NO_WAIT paths and Unlock()
MTX_TEST(InsideISR_NonBlocking)
{
    MtxFixture f;
    M m;

    {
        ScopedInsideISR isr;

        CHECK_TRUE(m.TryLock());
        CHECK_EQUAL(MTX_CALLER, m.GetOwner());
        m.Unlock();
        CHECK_FALSE(m.IsLocked());
    }

    MtxLockAs(f, m, MTX_OWNER);

    {
        ScopedInsideISR isr;
        CHECK_FALSE(m.TryLock());
    }
}

// a blocking lock inside an ISR violates the API contract
MTX_TEST(InsideISR_BlockingLock_Asserts)
{
    MtxFixture f;
    M m;

    MtxLockAs(f, m, MTX_OWNER);

    try
    {
        ScopedInsideISR isr;
        g_TestContext.ExpectAssert(true);
        STK_UNUSED(m.TimedLockEx(2)); // finite, so that a build without assertions cannot block
        CHECK(false); // should asssert in a DEBUG build
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    CHECK_EQUAL(0, test::g_CriticalSectionState);
}

// ---- sync::Mutex: recursion ----

TEST(Sync, Mutex_Recursive_NestedLocks)
{
    MtxFixture f;
    sync::Mutex m;

    CHECK_EQUAL(0, m.GetRecursionCount());

    m.Lock();
    CHECK_EQUAL(1, m.GetRecursionCount());

    m.Lock();
    CHECK_EQUAL(2, m.GetRecursionCount());

    CHECK_TRUE(m.TryLock());
    CHECK_EQUAL(3, m.GetRecursionCount());

    CHECK_EQUAL(WAIT_RESULT_SIGNAL, m.TimedLockEx(5));
    CHECK_EQUAL(4, m.GetRecursionCount());

    // recursive acquisition never blocks
    CHECK_EQUAL(0, f.fake.wait_calls);

    // released only by the matching number of Unlock() calls
    m.Unlock(); m.Unlock(); m.Unlock();
    CHECK_TRUE(m.IsLocked());
    CHECK_EQUAL(MTX_CALLER, m.GetOwner());
    CHECK_EQUAL(1, m.GetRecursionCount());
    CHECK_EQUAL(0, f.fake.restore_calls);

    m.Unlock();
    CHECK_FALSE(m.IsLocked());
    CHECK_EQUAL(0, m.GetRecursionCount());
    CHECK_EQUAL(1, f.fake.restore_calls); // the owner's priority is restored on the final release
}

// nesting by the owner does not let other tasks in
TEST(Sync, Mutex_Recursive_OtherTaskStillExcluded)
{
    MtxFixture f;
    sync::Mutex m;

    f.fake.m_tid = MTX_OWNER;
    m.Lock();
    m.Lock();
    f.fake.m_tid = MTX_CALLER;

    CHECK_EQUAL(2, m.GetRecursionCount());
    CHECK_EQUAL(WAIT_RESULT_TIMEOUT, m.TimedLockEx(NO_WAIT));
    CHECK_EQUAL(2, m.GetRecursionCount()); // unchanged
}

TEST(Sync, Mutex_Recursive_DepthLimit_Asserts)
{
    MtxFixture f;
    sync::Mutex m;

    const uint16_t max_depth = sync::Mutex::RECURSION_MAX;

    for (uint16_t i = 0; i < max_depth; ++i)
        m.Lock();

    CHECK_EQUAL(max_depth, m.GetRecursionCount());

    try
    {
        g_TestContext.ExpectAssert(true);
        STK_UNUSED(m.TryLock());
        CHECK(false); // should asssert in a DEBUG build
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    CHECK_EQUAL(max_depth, m.GetRecursionCount());

    for (uint16_t i = 0; i < max_depth; ++i)
        m.Unlock();

    CHECK_FALSE(m.IsLocked());
    CHECK_EQUAL(0, test::g_CriticalSectionState);
}

// the new owner starts with a fresh recursion count
TEST(Sync, Mutex_Recursive_HandOverResetsDepth)
{
    MtxFixture f;
    sync::Mutex m;

    m.Lock();
    m.Lock();

    f.fake.AddWaiter(MTX_SOBJ(m), 0x31);

    m.Unlock();
    CHECK_EQUAL(1, m.GetRecursionCount());
    CHECK_EQUAL(MTX_CALLER, m.GetOwner()); // still nested

    m.Unlock();
    CHECK_EQUAL(0x31, m.GetOwner());
    CHECK_EQUAL(1, m.GetRecursionCount());

    f.fake.m_tid = 0x31;
    m.Unlock();
    CHECK_EQUAL(0, m.GetRecursionCount());
}

// ---- sync::FastMutex: non-recursive ----

// locking a mutex already owned by the caller is an unrecoverable contract violation
template <class Fn>
static void FastMutexExpectDeadlock(Fn fn)
{
    test::g_PanicValue = KERNEL_PANIC_NONE;

    g_TestContext.ExpectPanic(true);
    try
    {
        fn();
    }
    catch (std::exception &)
    {}
    g_TestContext.ExpectPanic(false);

    CHECK_TRUE(((int)KERNEL_PANIC_SYNC_DEADLOCK == (int)test::g_PanicValue) ||
               ((int)KERNEL_PANIC_ASSERT == (int)test::g_PanicValue)); // when test allows to skip panic handler with ExpectPanic the logic will further assert
    CHECK_EQUAL(0, test::g_CriticalSectionState);
}

TEST(Sync, FastMutex_SelfLock_Panics)
{
    MtxFixture f;
    sync::FastMutex m;

    m.Lock();

    FastMutexExpectDeadlock([&]() { m.Lock(); });
    FastMutexExpectDeadlock([&]() { STK_UNUSED(m.TryLock()); });
    FastMutexExpectDeadlock([&]() { STK_UNUSED(m.TimedLock(NO_WAIT)); });
    FastMutexExpectDeadlock([&]() { STK_UNUSED(m.TimedLockEx(10)); });

    // no blocking attempt was made and the ownership is untouched
    CHECK_EQUAL(0, f.fake.wait_calls);
    CHECK_EQUAL(MTX_CALLER, m.GetOwner());

    m.Unlock();
    CHECK_FALSE(m.IsLocked());
}

// ============================================================================ //
// =============================== sync::RWMutex ============================== //
// ============================================================================ //

// RWMutex has no owner (it is not bound to a task id), so the fake kernel service of the Mutex tests drives it as is:
// its internal ConditionVariables block through IKernelService::Wait(), whose outcome is scripted, while the actions
// of the "other tasks" (unlock, steal, tick advance) are executed from MtxServiceFake::on_wait during that wait.

// expects KERNEL_PANIC_ASSERT from a void Lock() / ReadLock() which cannot report a failure
template <class Fn>
static void RwExpectAssertPanic(Fn fn)
{
    test::g_PanicValue = KERNEL_PANIC_NONE;

    g_TestContext.ExpectPanic(true);
    try
    {
        fn();
    }
    catch (std::exception &)
    {}
    g_TestContext.ExpectPanic(false);

    CHECK_EQUAL((int)KERNEL_PANIC_ASSERT, (int)test::g_PanicValue);
    CHECK_EQUAL(0, test::g_CriticalSectionState);
}

TEST(Sync, RWMutex_Uncontended_ReadersAndWriter)
{
    MtxFixture f;
    sync::RWMutex rw;

    // shared access: any number of readers
    CHECK_TRUE(rw.TryReadLock());
    CHECK_TRUE(rw.TryReadLock());
    CHECK_TRUE(rw.TimedReadLock(NO_WAIT));
    CHECK_EQUAL(WAIT_RESULT_SIGNAL, rw.TimedReadLockEx(NO_WAIT));

    // exclusive access is refused while readers are active ...
    CHECK_FALSE(rw.TryLock());
    CHECK_EQUAL(WAIT_RESULT_TIMEOUT, rw.TimedLockEx(NO_WAIT));

    // ... and the refused attempt must not leave a waiting writer behind (it would block new readers)
    CHECK_TRUE(rw.TryReadLock());

    for (int i = 0; i < 5; ++i)
        rw.ReadUnlock();

    // exclusive access
    CHECK_TRUE(rw.TryLock());
    CHECK_FALSE(rw.TryLock());
    CHECK_FALSE(rw.TryReadLock());
    CHECK_EQUAL(WAIT_RESULT_TIMEOUT, rw.TimedReadLockEx(NO_WAIT));
    rw.Unlock();

    // free again
    CHECK_TRUE(rw.TryReadLock());
    rw.ReadUnlock();
    CHECK_TRUE(rw.TimedLock(NO_WAIT));
    rw.Unlock();

    CHECK_EQUAL(0, test::g_CriticalSectionState);
}

// a reader blocked by the writer is released by Unlock()
TEST(Sync, RWMutex_Reader_WokenByWriterUnlock)
{
    MtxFixture f;
    sync::RWMutex rw;

    CHECK_TRUE(rw.TryLock());

    f.fake.Script(WAIT_RESULT_SIGNAL);
    f.fake.on_wait = [&](uint32_t) { rw.Unlock(); };

    CHECK_EQUAL(WAIT_RESULT_SIGNAL, rw.TimedReadLockEx(10));
    CHECK_EQUAL(1, f.fake.wait_calls);
    CHECK_EQUAL(10, f.fake.last_wait_timeout);

    // the reader holds the shared lock now
    CHECK_FALSE(rw.TryLock());
    rw.ReadUnlock();
    CHECK_TRUE(rw.TryLock());
    rw.Unlock();
}

// a writer blocked by readers is released by the last ReadUnlock() only
TEST(Sync, RWMutex_Writer_WokenByLastReader)
{
    MtxFixture f;
    sync::RWMutex rw;

    CHECK_TRUE(rw.TryReadLock());
    CHECK_TRUE(rw.TryReadLock());

    f.fake.Script(WAIT_RESULT_SIGNAL);
    f.fake.on_wait = [&](uint32_t)
    {
        rw.ReadUnlock(); // one reader is still active
        rw.ReadUnlock(); // the last one: wakes the writer
    };

    CHECK_EQUAL(WAIT_RESULT_SIGNAL, rw.TimedLockEx(WAIT_INFINITE));

    CHECK_FALSE(rw.TryReadLock());
    rw.Unlock();
    CHECK_TRUE(rw.TryReadLock());
    rw.ReadUnlock();
}

// writer preference: new readers are blocked while a writer waits, even though only readers hold the lock
TEST(Sync, RWMutex_WriterPreference_BlocksNewReaders)
{
    MtxFixture f;
    sync::RWMutex rw;

    CHECK_TRUE(rw.TryReadLock());

    bool reader_admitted = true;

    f.fake.Script(WAIT_RESULT_SIGNAL);
    f.fake.on_wait = [&](uint32_t call)
    {
        if (call == 1)
        {
            // another reader arrives while the writer is waiting: the lock is held by a reader only, still refused
            reader_admitted = rw.TryReadLock();

            rw.ReadUnlock(); // the writer's turn
        }
    };

    CHECK_EQUAL(WAIT_RESULT_SIGNAL, rw.TimedLockEx(10));
    CHECK_FALSE(reader_admitted);

    rw.Unlock();
    CHECK_TRUE(rw.TryReadLock());
    rw.ReadUnlock();
}

// a writer blocked by another writer is released by Unlock() (the next writer is preferred to the readers)
TEST(Sync, RWMutex_Writer_WokenByWriterUnlock)
{
    MtxFixture f;
    sync::RWMutex rw;

    CHECK_TRUE(rw.TryLock());

    f.fake.Script(WAIT_RESULT_SIGNAL);
    f.fake.on_wait = [&](uint32_t) { rw.Unlock(); };

    CHECK_EQUAL(WAIT_RESULT_SIGNAL, rw.TimedLockEx(10));

    CHECK_FALSE(rw.TryReadLock());
    rw.Unlock();
    CHECK_TRUE(rw.TryLock());
    rw.Unlock();
}

// ---- wait outcomes which are not a signal ----

static void RwReaderWaitFailure(EWaitResult outcome)
{
    MtxFixture f;
    sync::RWMutex rw;

    CHECK_TRUE(rw.TryLock());

    f.fake.Script(outcome);

    CHECK_EQUAL(outcome, rw.TimedReadLockEx(10));
    CHECK_EQUAL(1, f.fake.wait_calls);

    f.fake.Script(outcome);
    CHECK_FALSE(rw.TimedReadLock(10));

    // no reader was registered: the writer can be followed by a new writer
    rw.Unlock();
    CHECK_TRUE(rw.TryLock());
    rw.Unlock();
    CHECK_EQUAL(0, test::g_CriticalSectionState);
}

TEST(Sync, RWMutex_Reader_Timeout)  { RwReaderWaitFailure(WAIT_RESULT_TIMEOUT);  }
TEST(Sync, RWMutex_Reader_Canceled) { RwReaderWaitFailure(WAIT_RESULT_CANCELED); }
TEST(Sync, RWMutex_Reader_Failed)   { RwReaderWaitFailure(WAIT_RESULT_FAIL);     }

static void RwWriterWaitFailure(EWaitResult outcome)
{
    MtxFixture f;
    sync::RWMutex rw;

    CHECK_TRUE(rw.TryReadLock());

    f.fake.Script(outcome);

    CHECK_EQUAL(outcome, rw.TimedLockEx(10));
    CHECK_EQUAL(1, f.fake.wait_calls);

    f.fake.Script(outcome);
    CHECK_FALSE(rw.TimedLock(10));

    // the withdrawn writer does not keep new readers out
    CHECK_TRUE(rw.TryReadLock());

    rw.ReadUnlock();
    rw.ReadUnlock();

    CHECK_TRUE(rw.TryLock());
    rw.Unlock();
    CHECK_EQUAL(0, test::g_CriticalSectionState);
}

TEST(Sync, RWMutex_Writer_Timeout)  { RwWriterWaitFailure(WAIT_RESULT_TIMEOUT);  }
TEST(Sync, RWMutex_Writer_Canceled) { RwWriterWaitFailure(WAIT_RESULT_CANCELED); }
TEST(Sync, RWMutex_Writer_Failed)   { RwWriterWaitFailure(WAIT_RESULT_FAIL);     }

// a writer which waits for another writer and gives up: readers stay blocked by the active writer
TEST(Sync, RWMutex_Writer_Timeout_WhileWriterActive)
{
    MtxFixture f;
    sync::RWMutex rw;

    CHECK_TRUE(rw.TryLock());

    f.fake.Script(WAIT_RESULT_TIMEOUT);
    CHECK_EQUAL(WAIT_RESULT_TIMEOUT, rw.TimedLockEx(10));

    CHECK_FALSE(rw.TryReadLock());
    rw.Unlock();

    CHECK_TRUE(rw.TryReadLock());
    rw.ReadUnlock();
}

// the object was destroyed while waiting: nothing is touched, the result is reported as is
TEST(Sync, RWMutex_Reader_Deleted)
{
    MtxFixture f;
    sync::RWMutex rw;

    CHECK_TRUE(rw.TryLock());

    f.fake.Script(WAIT_RESULT_DELETED);
    CHECK_EQUAL(WAIT_RESULT_DELETED, rw.TimedReadLockEx(10));

    rw.Unlock();
}

TEST(Sync, RWMutex_Writer_Deleted)
{
    MtxFixture f;
    sync::RWMutex rw;

    CHECK_TRUE(rw.TryReadLock());

    f.fake.Script(WAIT_RESULT_DELETED);
    CHECK_EQUAL(WAIT_RESULT_DELETED, rw.TimedLockEx(10));

    // 'this' is considered gone: the waiting writer is not withdrawn and the lock is left as is
    // (the destructor accepts it: tasks were waiting)
}

// ---- void ReadLock() / Lock() ----

TEST(Sync, RWMutex_ReadLock_RetriesOnCancel)
{
    MtxFixture f;
    sync::RWMutex rw;

    CHECK_TRUE(rw.TryLock());

    f.fake.Script(WAIT_RESULT_CANCELED, WAIT_RESULT_CANCELED, WAIT_RESULT_SIGNAL);
    f.fake.on_wait = [&](uint32_t call)
    {
        if (call == 3)
            rw.Unlock();
    };

    rw.ReadLock();

    CHECK_EQUAL(3, f.fake.wait_calls);
    CHECK_EQUAL(WAIT_INFINITE, f.fake.last_wait_timeout);

    CHECK_FALSE(rw.TryLock()); // reader holds the lock
    rw.ReadUnlock();
    CHECK_TRUE(rw.TryLock());
    rw.Unlock();
}

TEST(Sync, RWMutex_Lock_RetriesOnCancel)
{
    MtxFixture f;
    sync::RWMutex rw;

    CHECK_TRUE(rw.TryReadLock());

    f.fake.Script(WAIT_RESULT_CANCELED, WAIT_RESULT_CANCELED, WAIT_RESULT_SIGNAL);
    f.fake.on_wait = [&](uint32_t call)
    {
        if (call == 3)
            rw.ReadUnlock();
    };

    rw.Lock();

    CHECK_EQUAL(3, f.fake.wait_calls);

    CHECK_FALSE(rw.TryReadLock()); // writer holds the lock
    rw.Unlock();
    CHECK_TRUE(rw.TryReadLock());
    rw.ReadUnlock();
}

TEST(Sync, RWMutex_ReadLock_PanicsOnFailure)
{
    MtxFixture f;
    sync::RWMutex rw;

    CHECK_TRUE(rw.TryLock());

    f.fake.Script(WAIT_RESULT_FAIL);
    RwExpectAssertPanic([&]() { rw.ReadLock(); });

    rw.Unlock();
}

TEST(Sync, RWMutex_Lock_PanicsOnFailure)
{
    MtxFixture f;
    sync::RWMutex rw;

    CHECK_TRUE(rw.TryReadLock());

    f.fake.Script(WAIT_RESULT_FAIL);
    RwExpectAssertPanic([&]() { rw.Lock(); });

    rw.ReadUnlock();
}

// ---- timed waits: the deadline is captured once, spurious wake-ups do not restart it ----

// the lock is stolen again between the wake-up and the re-check: the reader waits for the remaining time only
TEST(Sync, RWMutex_Reader_Timed_SpuriousWake_RemainingTime)
{
    MtxFixture f;
    sync::RWMutex rw;

    f.fake.m_ticks = 100;

    CHECK_TRUE(rw.TryLock());

    f.fake.Script(WAIT_RESULT_SIGNAL, WAIT_RESULT_SIGNAL);
    f.fake.on_wait = [&](uint32_t call)
    {
        if (call == 1)
        {
            f.fake.m_ticks = 102;

            rw.Unlock();
            CHECK_TRUE(rw.TryLock()); // another writer takes the lock first
        }
        else
        {
            rw.Unlock();
        }
    };

    CHECK_EQUAL(WAIT_RESULT_SIGNAL, rw.TimedReadLockEx(10));

    CHECK_EQUAL(2, f.fake.wait_calls);
    CHECK_EQUAL(8, f.fake.last_wait_timeout); // 10 ticks from tick 100, 2 are gone

    rw.ReadUnlock();
}

// the deadline is passed while the reader was woken in vain: the next wait is a non-blocking one
TEST(Sync, RWMutex_Reader_Timed_DeadlinePassed)
{
    MtxFixture f;
    sync::RWMutex rw;

    f.fake.m_ticks = 100;

    CHECK_TRUE(rw.TryLock());

    f.fake.Script(WAIT_RESULT_SIGNAL, WAIT_RESULT_TIMEOUT);
    f.fake.on_wait = [&](uint32_t call)
    {
        if (call == 1)
            f.fake.m_ticks = 200; // far beyond the deadline, the lock is still held
    };

    CHECK_EQUAL(WAIT_RESULT_TIMEOUT, rw.TimedReadLockEx(10));

    CHECK_EQUAL(1, f.fake.wait_calls);
    CHECK_EQUAL(10, f.fake.last_wait_timeout); // an expired wait is a NO_WAIT one: it is completed without the kernel, the last kernel wait is the first one

    rw.Unlock();
}

TEST(Sync, RWMutex_Writer_Timed_SpuriousWake_RemainingTime)
{
    MtxFixture f;
    sync::RWMutex rw;

    f.fake.m_ticks = 100;

    CHECK_TRUE(rw.TryReadLock());

    f.fake.Script(WAIT_RESULT_SIGNAL, WAIT_RESULT_SIGNAL);
    f.fake.on_wait = [&](uint32_t call)
    {
        if (call == 1)
        {
            f.fake.m_ticks = 103;

            rw.ReadUnlock();
            CHECK_TRUE(rw.TryLock()); // another writer takes the free lock first
        }
        else
        {
            rw.Unlock(); // wakes the waiting writer
        }
    };

    CHECK_EQUAL(WAIT_RESULT_SIGNAL, rw.TimedLockEx(10));

    CHECK_EQUAL(2, f.fake.wait_calls);
    CHECK_EQUAL(7, f.fake.last_wait_timeout);

    rw.Unlock();
}

TEST(Sync, RWMutex_Writer_Timed_DeadlinePassed)
{
    MtxFixture f;
    sync::RWMutex rw;

    f.fake.m_ticks = 100;

    CHECK_TRUE(rw.TryReadLock());

    f.fake.Script(WAIT_RESULT_SIGNAL, WAIT_RESULT_TIMEOUT);
    f.fake.on_wait = [&](uint32_t call)
    {
        if (call == 1)
            f.fake.m_ticks = 200;
    };

    const EWaitResult result = rw.TimedLockEx(10);

    // withdrawn: the reader count is unchanged and new readers are admitted
    const bool reader_admitted = rw.TryReadLock();
    if (reader_admitted)
        rw.ReadUnlock();
    rw.ReadUnlock(); // release before the checks: a failed check must not leave the lock held (the destructor asserts)

    CHECK_EQUAL(WAIT_RESULT_TIMEOUT, result);
    CHECK_EQUAL(1, f.fake.wait_calls);
    CHECK_EQUAL(10, f.fake.last_wait_timeout); // an expired wait is a NO_WAIT one: it is completed without the kernel, the last kernel wait is the first one
    CHECK_TRUE(reader_admitted);
}

// ---- RAII wrappers ----

TEST(Sync, RWMutex_ScopedLocks)
{
    MtxFixture f;
    sync::RWMutex rw;

    {
        sync::RWMutex::ScopedTimedReadMutex r1(rw, NO_WAIT);
        sync::RWMutex::ScopedTimedReadMutex r2(rw);

        CHECK_TRUE(r1.IsLocked());
        CHECK_TRUE(r2.IsLocked());
        CHECK_EQUAL(WAIT_RESULT_SIGNAL, r1.GetResult());

        // a writer is refused while the readers are in the scope
        sync::RWMutex::ScopedTimedLock w(rw, NO_WAIT);
        CHECK_FALSE(w.IsLocked());
        CHECK_EQUAL(WAIT_RESULT_TIMEOUT, w.GetResult());
    }

    {
        sync::RWMutex::ScopedTimedLock w(rw);
        CHECK_TRUE(w.IsLocked());
        CHECK_EQUAL(WAIT_RESULT_SIGNAL, w.GetResult());

        sync::RWMutex::ScopedTimedReadMutex r(rw, NO_WAIT);
        CHECK_FALSE(r.IsLocked());
        CHECK_EQUAL(WAIT_RESULT_TIMEOUT, r.GetResult());
    }

    // everything is released at the end of the scopes
    CHECK_TRUE(rw.TryLock());
    rw.Unlock();
}

// ---- API contract ----

TEST(Sync, RWMutex_Unlock_WithoutLock_Asserts)
{
    MtxFixture f;
    sync::RWMutex rw;

    try
    {
        g_TestContext.ExpectAssert(true);
        rw.Unlock();
        CHECK(false); // should asssert in a DEBUG build
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    try
    {
        g_TestContext.ExpectAssert(true);
        rw.ReadUnlock();
        CHECK(false); // should asssert in a DEBUG build
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    CHECK_EQUAL(0, test::g_CriticalSectionState);
}

TEST(Sync, RWMutex_ReaderLimit_Asserts)
{
    MtxFixture f;
    sync::RWMutex rw;

    const uint32_t max_readers = 0xFFFEU;

    for (uint32_t i = 0; i < max_readers; ++i)
        CHECK_TRUE(rw.TryReadLock());

    try
    {
        g_TestContext.ExpectAssert(true);
        STK_UNUSED(rw.TryReadLock());
        CHECK(false); // should asssert in a DEBUG build
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    for (uint32_t i = 0; i < max_readers; ++i)
        rw.ReadUnlock();

    CHECK_TRUE(rw.TryLock());
    rw.Unlock();
    CHECK_EQUAL(0, test::g_CriticalSectionState);
}

// ISR-safe operations: non-blocking attempts and unlocks
TEST(Sync, RWMutex_InsideISR_NonBlocking)
{
    MtxFixture f;
    sync::RWMutex rw;

    ScopedInsideISR isr;

    CHECK_TRUE(rw.TryReadLock());
    CHECK_TRUE(rw.TryReadLock());
    rw.ReadUnlock();
    rw.ReadUnlock();

    CHECK_TRUE(rw.TryLock());
    rw.Unlock();

    CHECK_EQUAL(0, f.fake.wait_calls);
}

// ============================================================================ //
// ========================= sync::Semaphore: waiters ========================= //
// ============================================================================ //

// Several waiters can not be created with a single mock task, so the fake kernel service of the Mutex tests provides
// them (blocked tasks in the wait list). It covers the hand-over to the selected waiter and the destruction with waiters.

// Signal() gives the token to the first waiter in arrival order, the counter is not touched
TEST(Sync, Semaphore_Signal_HandsTokenToFirstWaiter_Fifo)
{
    MtxFixture f;
    sync::Semaphore sem;

    WaitObjectStub *w1 = f.fake.AddWaiter(MTX_SOBJ(sem), 0x31);
    WaitObjectStub *w2 = f.fake.AddWaiter(MTX_SOBJ(sem), 0x32);
    WaitObjectStub *w3 = f.fake.AddWaiter(MTX_SOBJ(sem), 0x33);

    sem.Signal();
    CHECK_EQUAL(1, w1->m_wake_count);
    CHECK_EQUAL(0, w2->m_wake_count);
    CHECK_EQUAL(0, w3->m_wake_count);
    CHECK_EQUAL(0U, sem.GetCount());

    CHECK_TRUE(sem.TrySignal());
    CHECK_EQUAL(1, w2->m_wake_count);
    CHECK_EQUAL(0, w3->m_wake_count);
    CHECK_EQUAL(0U, sem.GetCount());

    CHECK_TRUE(sem.TrySignal(static_cast<uint16_t>(5U)));
    CHECK_EQUAL(1, w3->m_wake_count);
    CHECK_EQUAL(0U, sem.GetCount());

    // nobody waits any more: the signal is remembered
    sem.Signal();
    CHECK_EQUAL(1U, sem.GetCount());

    // each waiter was woken exactly once
    CHECK_EQUAL(1, w1->m_wake_count);
    CHECK_EQUAL(1, w2->m_wake_count);
    CHECK_EQUAL(1, w3->m_wake_count);
}

// a waiter exists only when no token is available, the hand-over does not depend on the counter limits
TEST(Sync, Semaphore_TrySignal_WithWaiter_IgnoresLimits)
{
    MtxFixture f;
    sync::Semaphore sem(0U, 1U);

    WaitObjectStub *w = f.fake.AddWaiter(MTX_SOBJ(sem), 0x31);

    CHECK_TRUE(sem.TrySignal(static_cast<uint16_t>(0U)));
    CHECK_EQUAL(1, w->m_wake_count);
    CHECK_EQUAL(0U, sem.GetCount());

    // without waiters the same ceiling is enforced
    CHECK_FALSE(sem.TrySignal(static_cast<uint16_t>(0U)));
    CHECK_EQUAL(0U, sem.GetCount());
}

// destruction with waiters: all of them are released with WAIT_RESULT_DELETED, no token is handed over
TEST(Sync, Semaphore_Destroyed_WithWaiters_ReleasedAsDeleted)
{
    MtxFixture f;

    WaitObjectStub *w1 = NULL, *w2 = NULL;
    ISyncObject *sobj = NULL;

    {
        sync::Semaphore sem;
        sobj = MTX_SOBJ(sem);

        w1 = f.fake.AddWaiter(sobj, 0x31);
        w2 = f.fake.AddWaiter(sobj, 0x32);
    }

    CHECK_EQUAL(1, w1->m_delete_count);
    CHECK_EQUAL(1, w2->m_delete_count);
    CHECK_EQUAL(0, w1->m_wake_count);
    CHECK_EQUAL(0, w2->m_wake_count);
    CHECK_TRUE(w1->GetState() == IWaitObject::STATE_DELETED);
    CHECK_TRUE(w2->GetState() == IWaitObject::STATE_DELETED);
}

// destruction of an idle semaphore does not involve the waiters' machinery
TEST(Sync, Semaphore_Destroyed_NoWaiters)
{
    MtxFixture f;

    {
        sync::Semaphore sem(2U);
        CHECK_TRUE(sem.TryWait());
    }

    CHECK_EQUAL(0, test::g_CriticalSectionState);
}

// ============================================================================ //
// =========================== sync::Pipe / PipeT ============================= //
// ============================================================================ //

// Pipe (runtime-sized, byte-oriented) and PipeT (compile-time-sized, typed) share the logic, so the test bodies are
// templates instantiated for both. The blocking is driven by the fake kernel service of the Mutex tests (scripted
// Wait() outcomes + MtxServiceFake::on_wait actions of "the other task"): unlike the single-task kernel harness it
// controls every wait of a bulk transfer individually (partial progress, deadlines) and needs no tick timing.

static const size_t PIPE_CAP = 8; // capacity (and the threshold at which PipeT switches to its memcpy bulk path)

// typed view of sync::Pipe (elements are uint32_t) with the call signatures of PipeT<uint32_t, PIPE_CAP>
struct PipeA
{
    PipeA() : p(buf, PIPE_CAP, sizeof(uint32_t)) {}

    EWaitResult WriteEx(uint32_t v, Timeout t)            { return p.WriteEx(&v, t); }
    bool        Write(uint32_t v, Timeout t)              { return p.Write(&v, t); }
    bool        TryWrite(uint32_t v)                      { return p.TryWrite(&v); }
    EWaitResult ReadEx(uint32_t &v, Timeout t)            { return p.ReadEx(&v, t); }
    bool        Read(uint32_t &v, Timeout t)              { return p.Read(&v, t); }
    bool        TryRead(uint32_t &v)                      { return p.TryRead(&v); }

    EWaitResult WriteBulkEx(const uint32_t *s, size_t n, size_t &w, Timeout t) { return p.WriteBulkEx(s, n, w, t); }
    size_t      WriteBulk(const uint32_t *s, size_t n, Timeout t)              { return p.WriteBulk(s, n, t); }
    size_t      TryWriteBulk(const uint32_t *s, size_t n)                      { return p.TryWriteBulk(s, n); }
    EWaitResult ReadBulkEx(uint32_t *d, size_t n, size_t &r, Timeout t)        { return p.ReadBulkEx(d, n, r, t); }
    size_t      ReadBulk(uint32_t *d, size_t n, Timeout t)                     { return p.ReadBulk(d, n, t); }
    size_t      TryReadBulk(uint32_t *d, size_t n)                             { return p.TryReadBulk(d, n); }

    void   Reset()            { p.Reset(); }
    size_t GetCapacity() const { return p.GetCapacity(); }
    size_t GetCount() const    { return p.GetCount(); }
    size_t GetSpace() const    { return p.GetSpace(); }
    bool   IsEmpty() const     { return p.IsEmpty(); }
    bool   IsFull() const      { return p.IsFull(); }

    uint8_t    buf[PIPE_CAP * sizeof(uint32_t)];
    sync::Pipe p;
};

typedef sync::PipeT<uint32_t, PIPE_CAP> PipeTA;

#define PIPE_TEST(name) \
    template <class P> static void Test_Pipe_##name(); \
    TEST(Sync, Pipe_##name)  { Test_Pipe_##name<PipeA>(); } \
    TEST(Sync, PipeT_##name) { Test_Pipe_##name<PipeTA>(); } \
    template <class P> static void Test_Pipe_##name()

template <class P>
static void PipeFill(P &p, uint32_t first, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i)
        CHECK_TRUE(p.TryWrite(first + i));
}

template <class P>
static uint32_t PipePop(P &p)
{
    uint32_t v = MQ_UNTOUCHED;
    CHECK_TRUE(p.TryRead(v));
    return v;
}

static void PipeSeq(uint32_t *a, uint32_t first, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i)
        a[i] = first + i;
}

// ---- non-blocking behavior ----

PIPE_TEST(Basics)
{
    MtxFixture f;
    P p;

    CHECK_EQUAL(PIPE_CAP, p.GetCapacity());
    CHECK_EQUAL(0U, p.GetCount());
    CHECK_EQUAL(PIPE_CAP, p.GetSpace());
    CHECK_TRUE(p.IsEmpty());
    CHECK_FALSE(p.IsFull());

    PipeFill(p, 1U, PIPE_CAP);

    CHECK_TRUE(p.IsFull());
    CHECK_FALSE(p.IsEmpty());
    CHECK_EQUAL(0U, p.GetSpace());
    CHECK_FALSE(p.TryWrite(99U)); // full

    for (uint32_t i = 1; i <= PIPE_CAP; ++i)
        CHECK_EQUAL(i, PipePop(p));

    CHECK_TRUE(p.IsEmpty());

    uint32_t v = MQ_UNTOUCHED;
    CHECK_FALSE(p.TryRead(v)); // empty
    CHECK_EQUAL(MQ_UNTOUCHED, v);

    CHECK_EQUAL(0, test::g_CriticalSectionState);
}

PIPE_TEST(Fifo_WrapAround)
{
    MtxFixture f;
    P p;

    PipeFill(p, 1U, PIPE_CAP);

    CHECK_EQUAL(1U, PipePop(p));
    CHECK_EQUAL(2U, PipePop(p));
    CHECK_EQUAL(3U, PipePop(p));

    PipeFill(p, 9U, 3U); // wraps around the ring

    for (uint32_t expected = 4; expected <= 11; ++expected)
        CHECK_EQUAL(expected, PipePop(p));

    CHECK_TRUE(p.IsEmpty());
}

PIPE_TEST(Bulk_NonBlocking)
{
    MtxFixture f;
    P p;

    uint32_t src[PIPE_CAP], dst[PIPE_CAP];
    PipeSeq(src, 1U, PIPE_CAP);

    CHECK_EQUAL(5U, p.TryWriteBulk(src, 5));
    CHECK_EQUAL(3U, p.TryWriteBulk(src + 5, 5)); // only 3 slots are free, the rest is discarded
    CHECK_TRUE(p.IsFull());
    CHECK_EQUAL(0U, p.TryWriteBulk(src, 1));

    memset(dst, 0, sizeof(dst));
    CHECK_EQUAL(6U, p.TryReadBulk(dst, 6));
    for (uint32_t i = 0; i < 6; ++i)
        CHECK_EQUAL(i + 1U, dst[i]);

    CHECK_EQUAL(2U, p.TryReadBulk(dst, 6)); // only 2 are left
    CHECK_EQUAL(7U, dst[0]);
    CHECK_EQUAL(8U, dst[1]);

    CHECK_EQUAL(0U, p.TryReadBulk(dst, 1));
    CHECK_TRUE(p.IsEmpty());
}

// both the write and the read index cross the end of the ring in the middle of a transfer
PIPE_TEST(Bulk_WrapAround)
{
    MtxFixture f;
    P p;

    uint32_t src[PIPE_CAP], dst[PIPE_CAP];

    PipeSeq(src, 10U, 6U);
    CHECK_EQUAL(6U, p.TryWriteBulk(src, 6));
    CHECK_EQUAL(6U, p.TryReadBulk(dst, 6));

    PipeSeq(src, 20U, 5U);
    CHECK_EQUAL(5U, p.TryWriteBulk(src, 5)); // head: 6 -> 3 (wrapped)
    CHECK_EQUAL(5U, p.GetCount());

    memset(dst, 0, sizeof(dst));
    CHECK_EQUAL(5U, p.TryReadBulk(dst, PIPE_CAP)); // tail: 6 -> 3 (wrapped)
    for (uint32_t i = 0; i < 5; ++i)
        CHECK_EQUAL(20U + i, dst[i]);

    CHECK_TRUE(p.IsEmpty());

    // the indexes are consistent afterwards
    PipeFill(p, 50U, 3U);
    CHECK_EQUAL(50U, PipePop(p));
}

PIPE_TEST(Bulk_Arguments)
{
    MtxFixture f;
    P p;

    uint32_t buf[2] = { 1U, 2U };
    size_t n = 99;

    // null pointers are rejected
    CHECK_EQUAL(WAIT_RESULT_FAIL, p.WriteBulkEx(NULL, 2, n, 10));
    CHECK_EQUAL(0U, n);

    n = 99;
    CHECK_EQUAL(WAIT_RESULT_FAIL, p.ReadBulkEx(NULL, 2, n, 10));
    CHECK_EQUAL(0U, n);

    // an empty request is trivially satisfied
    n = 99;
    CHECK_EQUAL(WAIT_RESULT_SIGNAL, p.WriteBulkEx(buf, 0, n, 10));
    CHECK_EQUAL(0U, n);

    n = 99;
    CHECK_EQUAL(WAIT_RESULT_SIGNAL, p.ReadBulkEx(buf, 0, n, 10));
    CHECK_EQUAL(0U, n);

    CHECK_EQUAL(0, f.fake.wait_calls);
    CHECK_TRUE(p.IsEmpty());
}

PIPE_TEST(Bulk_Ex_Complete)
{
    MtxFixture f;
    P p;

    uint32_t src[3] = { 7U, 8U, 9U }, dst[3] = { 0U, 0U, 0U };
    size_t n = 0;

    CHECK_EQUAL(WAIT_RESULT_SIGNAL, p.WriteBulkEx(src, 3, n, 10));
    CHECK_EQUAL(3U, n);

    n = 0;
    CHECK_EQUAL(WAIT_RESULT_SIGNAL, p.ReadBulkEx(dst, 3, n, 10));
    CHECK_EQUAL(3U, n);
    CHECK_EQUAL(7U, dst[0]);
    CHECK_EQUAL(9U, dst[2]);

    // plain variants
    CHECK_EQUAL(3U, p.WriteBulk(src, 3, 10));
    dst[0] = 0U;
    CHECK_EQUAL(3U, p.ReadBulk(dst, 3, 10));
    CHECK_EQUAL(7U, dst[0]);

    CHECK_EQUAL(0, f.fake.wait_calls); // never blocked
}

// ---- single element: blocking ----

// a writer blocked by a full pipe is released by a read
PIPE_TEST(Write_Blocked_WokenByRead)
{
    MtxFixture f;
    P p;

    PipeFill(p, 1U, PIPE_CAP);

    f.fake.Script(WAIT_RESULT_SIGNAL);
    f.fake.on_wait = [&](uint32_t) { (void)PipePop(p); };

    CHECK_EQUAL(WAIT_RESULT_SIGNAL, p.WriteEx(99U, WAIT_INFINITE));
    CHECK_EQUAL(1, f.fake.wait_calls);
    CHECK_EQUAL(WAIT_INFINITE, f.fake.last_wait_timeout);

    CHECK_TRUE(p.IsFull());
    for (uint32_t v = 2; v <= PIPE_CAP; ++v)
        CHECK_EQUAL(v, PipePop(p));
    CHECK_EQUAL(99U, PipePop(p));
}

// a reader blocked by an empty pipe is released by a write
PIPE_TEST(Read_Blocked_WokenByWrite)
{
    MtxFixture f;
    P p;

    f.fake.Script(WAIT_RESULT_SIGNAL);
    f.fake.on_wait = [&](uint32_t) { CHECK_TRUE(p.TryWrite(7U)); };

    uint32_t v = MQ_UNTOUCHED;
    CHECK_EQUAL(WAIT_RESULT_SIGNAL, p.ReadEx(v, 10));
    CHECK_EQUAL(10, f.fake.last_wait_timeout);
    CHECK_EQUAL(7U, v);
    CHECK_TRUE(p.IsEmpty());
}

// a wait without a signal: nothing is written / read
template <class P>
static void PipeOutcomes(EWaitResult outcome)
{
    MtxFixture f;
    P p;

    // writer: full pipe
    PipeFill(p, 1U, PIPE_CAP);

    f.fake.Script(outcome);
    CHECK_EQUAL(outcome, p.WriteEx(99U, 10));
    CHECK_EQUAL(1, f.fake.wait_calls);

    f.fake.Script(outcome);
    CHECK_FALSE(p.Write(99U, 10)); // the bool overload cannot tell the outcomes apart

    CHECK_EQUAL(PIPE_CAP, p.GetCount());
    CHECK_EQUAL(1U, PipePop(p));

    // reader: empty pipe
    uint32_t dst[PIPE_CAP];

    CHECK_EQUAL(PIPE_CAP - 1U, p.TryReadBulk(dst, PIPE_CAP));

    uint32_t v = MQ_UNTOUCHED;

    f.fake.Script(outcome);
    CHECK_EQUAL(outcome, p.ReadEx(v, 10));
    CHECK_EQUAL(MQ_UNTOUCHED, v);

    f.fake.Script(outcome);
    CHECK_FALSE(p.Read(v, 10));
    CHECK_EQUAL(MQ_UNTOUCHED, v);

    CHECK_TRUE(p.IsEmpty());
    CHECK_EQUAL(0, test::g_CriticalSectionState);
}

PIPE_TEST(Wait_Timeout)  { PipeOutcomes<P>(WAIT_RESULT_TIMEOUT);  }
PIPE_TEST(Wait_Canceled) { PipeOutcomes<P>(WAIT_RESULT_CANCELED); }
PIPE_TEST(Wait_Failed)   { PipeOutcomes<P>(WAIT_RESULT_FAIL);     }

// NO_WAIT: the same as a timeout
PIPE_TEST(NoWait_FullAndEmpty)
{
    MtxFixture f;
    P p;

    uint32_t v = MQ_UNTOUCHED;

    CHECK_EQUAL(WAIT_RESULT_TIMEOUT, p.ReadEx(v, NO_WAIT));
    CHECK_FALSE(p.TryRead(v));

    PipeFill(p, 1U, PIPE_CAP);

    CHECK_EQUAL(WAIT_RESULT_TIMEOUT, p.WriteEx(99U, NO_WAIT));
    CHECK_FALSE(p.TryWrite(99U));
    CHECK_EQUAL(MQ_UNTOUCHED, v);
}

// the object was destroyed while waiting: reported as is, no member is touched
PIPE_TEST(Wait_Deleted)
{
    MtxFixture f;
    P p;

    uint32_t v = MQ_UNTOUCHED;
    uint32_t buf[2] = { 1U, 2U };
    size_t n = 99;

    f.fake.Script(WAIT_RESULT_DELETED);
    CHECK_EQUAL(WAIT_RESULT_DELETED, p.ReadEx(v, 10));
    CHECK_EQUAL(MQ_UNTOUCHED, v);

    f.fake.Script(WAIT_RESULT_DELETED);
    CHECK_EQUAL(WAIT_RESULT_DELETED, p.ReadBulkEx(buf, 2, n, 10));
    CHECK_EQUAL(0U, n);

    PipeFill(p, 1U, PIPE_CAP);

    f.fake.Script(WAIT_RESULT_DELETED);
    CHECK_EQUAL(WAIT_RESULT_DELETED, p.WriteEx(99U, 10));

    f.fake.Script(WAIT_RESULT_DELETED);
    CHECK_EQUAL(WAIT_RESULT_DELETED, p.WriteBulkEx(buf, 2, n, 10));
    CHECK_EQUAL(0U, n);
}

// Reset() empties the pipe and releases a blocked writer
PIPE_TEST(Reset_WakesBlockedWriter)
{
    MtxFixture f;
    P p;

    PipeFill(p, 1U, PIPE_CAP);

    f.fake.Script(WAIT_RESULT_SIGNAL);
    f.fake.on_wait = [&](uint32_t) { p.Reset(); };

    CHECK_EQUAL(WAIT_RESULT_SIGNAL, p.WriteEx(77U, 10));

    CHECK_EQUAL(1U, p.GetCount());
    CHECK_EQUAL(77U, PipePop(p));

    // the indexes were reset: the ring works from the start
    PipeFill(p, 1U, PIPE_CAP);
    p.Reset();
    CHECK_TRUE(p.IsEmpty());
    CHECK_EQUAL(PIPE_CAP, p.GetSpace());

    uint32_t v = MQ_UNTOUCHED;
    CHECK_FALSE(p.TryRead(v));
}

// ---- single element: timed waits ----

// woken without a free slot / data (stolen by another task): the wait continues for the remaining time only
PIPE_TEST(Write_Timed_SpuriousWake_RemainingTime)
{
    MtxFixture f;
    P p;

    f.fake.m_ticks = 100;
    PipeFill(p, 1U, PIPE_CAP);

    f.fake.Script(WAIT_RESULT_SIGNAL, WAIT_RESULT_SIGNAL);
    f.fake.on_wait = [&](uint32_t call)
    {
        if (call == 1)
            f.fake.m_ticks = 102; // woken, the slot is gone
        else
            (void)PipePop(p);
    };

    CHECK_EQUAL(WAIT_RESULT_SIGNAL, p.WriteEx(77U, 10));
    CHECK_EQUAL(2, f.fake.wait_calls);
    CHECK_EQUAL(8, f.fake.last_wait_timeout); // 10 ticks from tick 100, 2 are gone
}

PIPE_TEST(Read_Timed_SpuriousWake_RemainingTime)
{
    MtxFixture f;
    P p;

    f.fake.m_ticks = 100;

    f.fake.Script(WAIT_RESULT_SIGNAL, WAIT_RESULT_SIGNAL);
    f.fake.on_wait = [&](uint32_t call)
    {
        if (call == 1)
            f.fake.m_ticks = 103;
        else
            CHECK_TRUE(p.TryWrite(5U));
    };

    uint32_t v = MQ_UNTOUCHED;
    CHECK_EQUAL(WAIT_RESULT_SIGNAL, p.ReadEx(v, 10));
    CHECK_EQUAL(2, f.fake.wait_calls);
    CHECK_EQUAL(7, f.fake.last_wait_timeout);
    CHECK_EQUAL(5U, v);
}

// the deadline is passed while the task was woken in vain: the next wait is a non-blocking one
PIPE_TEST(Write_Timed_DeadlinePassed)
{
    MtxFixture f;
    P p;

    f.fake.m_ticks = 100;
    PipeFill(p, 1U, PIPE_CAP);

    f.fake.Script(WAIT_RESULT_SIGNAL, WAIT_RESULT_TIMEOUT);
    f.fake.on_wait = [&](uint32_t call)
    {
        if (call == 1)
            f.fake.m_ticks = 200;
    };

    CHECK_EQUAL(WAIT_RESULT_TIMEOUT, p.WriteEx(77U, 10));
    CHECK_EQUAL(1, f.fake.wait_calls);
    CHECK_EQUAL(10, f.fake.last_wait_timeout); // an expired wait is a NO_WAIT one: it is completed without the kernel, the last kernel wait is the first one
}

PIPE_TEST(Read_Timed_DeadlinePassed)
{
    MtxFixture f;
    P p;

    f.fake.m_ticks = 100;

    f.fake.Script(WAIT_RESULT_SIGNAL, WAIT_RESULT_TIMEOUT);
    f.fake.on_wait = [&](uint32_t call)
    {
        if (call == 1)
            f.fake.m_ticks = 200;
    };

    uint32_t v = MQ_UNTOUCHED;
    CHECK_EQUAL(WAIT_RESULT_TIMEOUT, p.ReadEx(v, 10));
    CHECK_EQUAL(10, f.fake.last_wait_timeout); // an expired wait is a NO_WAIT one: it is completed without the kernel, the last kernel wait is the first one
    CHECK_EQUAL(MQ_UNTOUCHED, v);
}

// ---- bulk: blocking, partial progress ----

// the block is larger than the free space: the first part is written, the rest after a reader made room
PIPE_TEST(WriteBulk_Blocked_WokenByRead)
{
    MtxFixture f;
    P p;

    PipeFill(p, 1U, 6U); // 2 free

    uint32_t src[5];
    PipeSeq(src, 100U, 5U);

    f.fake.Script(WAIT_RESULT_SIGNAL);
    f.fake.on_wait = [&](uint32_t)
    {
        uint32_t tmp[3];
        CHECK_EQUAL(3U, p.TryReadBulk(tmp, 3)); // frees 3 slots
        CHECK_EQUAL(1U, tmp[0]);
    };

    size_t written = 0;
    CHECK_EQUAL(WAIT_RESULT_SIGNAL, p.WriteBulkEx(src, 5, written, 10));
    CHECK_EQUAL(5U, written);
    CHECK_EQUAL(1, f.fake.wait_calls); // one wait only: the first 2 elements did not need one

    CHECK_TRUE(p.IsFull());

    uint32_t dst[PIPE_CAP];
    CHECK_EQUAL(PIPE_CAP, p.TryReadBulk(dst, PIPE_CAP));

    const uint32_t expected[PIPE_CAP] = { 4U, 5U, 6U, 100U, 101U, 102U, 103U, 104U };
    for (size_t i = 0; i < PIPE_CAP; ++i)
        CHECK_EQUAL(expected[i], dst[i]);
}

PIPE_TEST(ReadBulk_Blocked_WokenByWrite)
{
    MtxFixture f;
    P p;

    PipeFill(p, 1U, 2U);

    f.fake.Script(WAIT_RESULT_SIGNAL);
    f.fake.on_wait = [&](uint32_t)
    {
        uint32_t src[3];
        PipeSeq(src, 50U, 3U);
        CHECK_EQUAL(3U, p.TryWriteBulk(src, 3));
    };

    uint32_t dst[5] = { 0U, 0U, 0U, 0U, 0U };
    size_t n = 0;
    CHECK_EQUAL(WAIT_RESULT_SIGNAL, p.ReadBulkEx(dst, 5, n, 10));
    CHECK_EQUAL(5U, n);
    CHECK_EQUAL(1, f.fake.wait_calls);

    const uint32_t expected[5] = { 1U, 2U, 50U, 51U, 52U };
    for (size_t i = 0; i < 5; ++i)
        CHECK_EQUAL(expected[i], dst[i]);

    CHECK_TRUE(p.IsEmpty());
}

// a failed wait of a bulk transfer keeps what was already moved
template <class P>
static void PipeBulkOutcomes(EWaitResult outcome)
{
    MtxFixture f;
    P p;

    uint32_t src[5], dst[5] = { MQ_UNTOUCHED, MQ_UNTOUCHED, MQ_UNTOUCHED, MQ_UNTOUCHED, MQ_UNTOUCHED };
    PipeSeq(src, 100U, 5U);

    // writer: 2 slots free, 5 elements requested
    PipeFill(p, 1U, 6U);

    size_t n = 99;
    f.fake.Script(outcome);
    CHECK_EQUAL(outcome, p.WriteBulkEx(src, 5, n, 10));
    CHECK_EQUAL(2U, n);          // partial count
    CHECK_TRUE(p.IsFull());      // the written elements stay in the pipe

    f.fake.Script(outcome);
    CHECK_EQUAL(0U, p.WriteBulk(src, 5, 10)); // full: nothing fits

    // reader: the pipe holds {1..6, 100, 101}, a request which is covered by it does not wait at all
    n = 99;
    f.fake.Script(outcome);
    CHECK_EQUAL(WAIT_RESULT_SIGNAL, p.ReadBulkEx(dst, 5, n, 10));
    CHECK_EQUAL(5U, n);
    CHECK_EQUAL(5U, dst[4]);

    // 3 elements are left, 8 are requested: the rest is waited for in vain
    uint32_t big[PIPE_CAP];
    n = 99;
    f.fake.Script(outcome);
    CHECK_EQUAL(outcome, p.ReadBulkEx(big, PIPE_CAP, n, 10));
    CHECK_EQUAL(3U, n);
    CHECK_EQUAL(6U, big[0]);
    CHECK_EQUAL(100U, big[1]);
    CHECK_EQUAL(101U, big[2]);

    f.fake.Script(outcome);
    CHECK_EQUAL(0U, p.ReadBulk(big, PIPE_CAP, 10)); // empty

    CHECK_TRUE(p.IsEmpty());
    CHECK_EQUAL(0, test::g_CriticalSectionState);
}

PIPE_TEST(Bulk_Wait_Timeout)  { PipeBulkOutcomes<P>(WAIT_RESULT_TIMEOUT);  }
PIPE_TEST(Bulk_Wait_Canceled) { PipeBulkOutcomes<P>(WAIT_RESULT_CANCELED); }
PIPE_TEST(Bulk_Wait_Failed)   { PipeBulkOutcomes<P>(WAIT_RESULT_FAIL);     }

// bulk timed waits: the deadline is captured once for the whole transfer
PIPE_TEST(WriteBulk_Timed_SpuriousWake_RemainingTime)
{
    MtxFixture f;
    P p;

    f.fake.m_ticks = 100;
    PipeFill(p, 1U, 7U); // 1 free

    uint32_t src[3];
    PipeSeq(src, 100U, 3U);

    f.fake.Script(WAIT_RESULT_SIGNAL, WAIT_RESULT_SIGNAL);
    f.fake.on_wait = [&](uint32_t call)
    {
        if (call == 1)
        {
            f.fake.m_ticks = 103;
        }
        else
        {
            uint32_t tmp[2];
            CHECK_EQUAL(2U, p.TryReadBulk(tmp, 2));
        }
    };

    size_t n = 0;
    CHECK_EQUAL(WAIT_RESULT_SIGNAL, p.WriteBulkEx(src, 3, n, 10));
    CHECK_EQUAL(3U, n);
    CHECK_EQUAL(2, f.fake.wait_calls);
    CHECK_EQUAL(7, f.fake.last_wait_timeout);
}

PIPE_TEST(ReadBulk_Timed_SpuriousWake_RemainingTime)
{
    MtxFixture f;
    P p;

    f.fake.m_ticks = 100;
    PipeFill(p, 1U, 1U);

    f.fake.Script(WAIT_RESULT_SIGNAL, WAIT_RESULT_SIGNAL);
    f.fake.on_wait = [&](uint32_t call)
    {
        if (call == 1)
        {
            f.fake.m_ticks = 103;
        }
        else
        {
            uint32_t src[2] = { 8U, 9U };
            CHECK_EQUAL(2U, p.TryWriteBulk(src, 2));
        }
    };

    uint32_t dst[3];
    size_t n = 0;
    CHECK_EQUAL(WAIT_RESULT_SIGNAL, p.ReadBulkEx(dst, 3, n, 10));
    CHECK_EQUAL(3U, n);
    CHECK_EQUAL(2, f.fake.wait_calls);
    CHECK_EQUAL(7, f.fake.last_wait_timeout);
    CHECK_EQUAL(9U, dst[2]);
}

PIPE_TEST(Bulk_Timed_DeadlinePassed)
{
    MtxFixture f;
    P p;

    uint32_t buf[3] = { 1U, 2U, 3U };
    size_t n = 0;

    // reader of an empty pipe
    f.fake.m_ticks = 100;
    f.fake.Script(WAIT_RESULT_SIGNAL, WAIT_RESULT_TIMEOUT);
    f.fake.on_wait = [&](uint32_t call)
    {
        if (call == 1)
            f.fake.m_ticks = 200;
    };

    CHECK_EQUAL(WAIT_RESULT_TIMEOUT, p.ReadBulkEx(buf, 3, n, 10));
    CHECK_EQUAL(10, f.fake.last_wait_timeout); // an expired wait is a NO_WAIT one: it is completed without the kernel, the last kernel wait is the first one
    CHECK_EQUAL(0U, n);

    // writer of a full pipe
    PipeFill(p, 1U, PIPE_CAP);

    f.fake.m_ticks = 100;
    f.fake.wait_calls = 0;
    f.fake.Script(WAIT_RESULT_SIGNAL, WAIT_RESULT_TIMEOUT);

    CHECK_EQUAL(WAIT_RESULT_TIMEOUT, p.WriteBulkEx(buf, 3, n, 10));
    CHECK_EQUAL(1, f.fake.wait_calls);
    CHECK_EQUAL(10, f.fake.last_wait_timeout); // an expired wait is a NO_WAIT one: it is completed without the kernel, the last kernel wait is the first one
    CHECK_EQUAL(0U, n);
}

// ISR-safe: non-blocking operations
PIPE_TEST(InsideISR_NonBlocking)
{
    MtxFixture f;
    P p;

    uint32_t src[3] = { 1U, 2U, 3U }, dst[3];
    uint32_t v = MQ_UNTOUCHED;

    ScopedInsideISR isr;

    CHECK_TRUE(p.TryWrite(5U));
    CHECK_TRUE(p.TryRead(v));
    CHECK_EQUAL(5U, v);
    CHECK_FALSE(p.TryRead(v));

    CHECK_EQUAL(3U, p.TryWriteBulk(src, 3));
    CHECK_EQUAL(3U, p.TryReadBulk(dst, 3));
    CHECK_EQUAL(3U, dst[2]);

    p.Reset();
    CHECK_TRUE(p.IsEmpty());
}

// ---- sync::Pipe: runtime-sized byte elements ----

TEST(Sync, Pipe_Introspection)
{
    MtxFixture f;
    PipeA a;

    CHECK_EQUAL(sizeof(uint32_t), a.p.GetElementSize());
    CHECK_TRUE(a.p.IsStorageValid());
    CHECK_TRUE(a.p.GetBuffer() == a.buf);

    const size_t capacity_max = sync::Pipe::CAPACITY_MAX;
    CHECK_EQUAL(0xFFFEU, capacity_max);
}

// the element size is a number of bytes: elements of an odd size keep their layout across the ring
TEST(Sync, Pipe_ByteElements_OddSize)
{
    MtxFixture f;

    uint8_t storage[4 * 3];
    sync::Pipe p(storage, 4, 3);

    CHECK_EQUAL(3U, p.GetElementSize());

    const uint8_t src[5 * 3] = { 1, 2, 3,  4, 5, 6,  7, 8, 9,  10, 11, 12,  13, 14, 15 };
    uint8_t dst[5 * 3];

    CHECK_EQUAL(3U, p.TryWriteBulk(src, 3));
    CHECK_EQUAL(2U, p.TryReadBulk(dst, 2));
    CHECK_EQUAL(4U, dst[3]);

    // wraps the ring: 2 more elements, {10, 11, 12} and {13, 14, 15}
    CHECK_EQUAL(2U, p.TryWriteBulk(src + 9, 2));
    CHECK_EQUAL(3U, p.GetCount());

    memset(dst, 0, sizeof(dst));
    CHECK_EQUAL(3U, p.TryReadBulk(dst, 5));
    CHECK_EQUAL(7U, dst[0]);
    CHECK_EQUAL(9U, dst[2]);
    CHECK_EQUAL(10U, dst[3]);
    CHECK_EQUAL(12U, dst[5]);
    CHECK_EQUAL(15U, dst[8]);

    // single element
    const uint8_t one[3] = { 21, 22, 23 };
    uint8_t out[3] = { 0, 0, 0 };
    CHECK_TRUE(p.TryWrite(one));
    CHECK_TRUE(p.TryRead(out));
    CHECK_EQUAL(21U, out[0]);
    CHECK_EQUAL(23U, out[2]);
}

TEST(Sync, Pipe_Construction_InvalidArguments_Assert)
{
    MtxFixture f;

    uint8_t storage[8];

    for (int i = 0; i < 4; ++i)
    {
        try
        {
            g_TestContext.ExpectAssert(true);

            switch (i)
            {
            case 0: { sync::Pipe p(NULL, 2, 4);                                    break; } // no storage
            case 1: { sync::Pipe p(storage, 0, 4);                                 break; } // no capacity
            case 2: { const size_t too_large = (size_t)0xFFFFU; sync::Pipe p(storage, too_large, 1); break; } // too large
            case 3: { sync::Pipe p(storage, 2, 0);                                 break; } // no element size
            }

            CHECK(false); // should asssert in a DEBUG build
        }
        catch (TestAssertPassed &pass)
        {
            CHECK(true);
            g_TestContext.ExpectAssert(false);
        }
    }
}

TEST(Sync, Pipe_NullData_Asserts)
{
    MtxFixture f;
    PipeA a;

    try
    {
        g_TestContext.ExpectAssert(true);
        STK_UNUSED(a.p.WriteEx(NULL, NO_WAIT));
        CHECK(false); // should asssert in a DEBUG build
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    try
    {
        g_TestContext.ExpectAssert(true);
        STK_UNUSED(a.p.ReadEx(NULL, NO_WAIT));
        CHECK(false); // should asssert in a DEBUG build
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    CHECK_EQUAL(0, test::g_CriticalSectionState);
}

// ---- sync::Pipe: ReadBulkTriggered ----

TEST(Sync, Pipe_ReadBulkTriggered_ThresholdReached)
{
    MtxFixture f;
    PipeA a;

    uint32_t src[PIPE_CAP], dst[PIPE_CAP];
    PipeSeq(src, 1U, PIPE_CAP);
    CHECK_EQUAL(5U, a.TryWriteBulk(src, 5));

    size_t n = 0;

    // everything available is returned (up to max_count), not only the trigger count
    CHECK_EQUAL(WAIT_RESULT_SIGNAL, a.p.ReadBulkTriggeredEx(dst, 3, 8, n, NO_WAIT));
    CHECK_EQUAL(5U, n);
    CHECK_EQUAL(1U, dst[0]);
    CHECK_EQUAL(5U, dst[4]);
    CHECK_TRUE(a.IsEmpty());
    CHECK_EQUAL(0, f.fake.wait_calls);

    // max_count limits the result
    CHECK_EQUAL(6U, a.TryWriteBulk(src, 6));
    CHECK_EQUAL(4U, a.p.ReadBulkTriggered(dst, 2, 4, NO_WAIT));
    CHECK_EQUAL(2U, a.GetCount());
}

TEST(Sync, Pipe_ReadBulkTriggered_TriggerIsClamped)
{
    MtxFixture f;
    PipeA a;

    uint32_t src[PIPE_CAP], dst[PIPE_CAP];
    PipeSeq(src, 1U, PIPE_CAP);

    size_t n = 99;

    // trigger 0 means 1
    CHECK_EQUAL(2U, a.TryWriteBulk(src, 2));
    CHECK_EQUAL(WAIT_RESULT_SIGNAL, a.p.ReadBulkTriggeredEx(dst, 0, 4, n, NO_WAIT));
    CHECK_EQUAL(2U, n);

    // trigger above max_count means max_count
    CHECK_EQUAL(6U, a.TryWriteBulk(src, 6));
    CHECK_EQUAL(WAIT_RESULT_SIGNAL, a.p.ReadBulkTriggeredEx(dst, 100, 4, n, NO_WAIT));
    CHECK_EQUAL(4U, n);
    CHECK_EQUAL(2U, a.GetCount());
}

TEST(Sync, Pipe_ReadBulkTriggered_Arguments)
{
    MtxFixture f;
    PipeA a;

    uint32_t dst[2];
    size_t n = 99;

    CHECK_EQUAL(WAIT_RESULT_FAIL, a.p.ReadBulkTriggeredEx(NULL, 1, 2, n, 10));
    CHECK_EQUAL(0U, n);

    n = 99;
    CHECK_EQUAL(WAIT_RESULT_SIGNAL, a.p.ReadBulkTriggeredEx(dst, 1, 0, n, 10)); // nothing requested
    CHECK_EQUAL(0U, n);

    CHECK_EQUAL(0, f.fake.wait_calls);
}

// the threshold is not reached: whatever is available is drained, the result is the wait outcome
TEST(Sync, Pipe_ReadBulkTriggered_ThresholdNotReached_DrainsAvailable)
{
    MtxFixture f;
    PipeA a;

    uint32_t src[2] = { 1U, 2U }, dst[PIPE_CAP];
    size_t n = 99;

    // NO_WAIT
    CHECK_EQUAL(2U, a.TryWriteBulk(src, 2));
    CHECK_EQUAL(WAIT_RESULT_TIMEOUT, a.p.ReadBulkTriggeredEx(dst, 3, 8, n, NO_WAIT));
    CHECK_EQUAL(2U, n);
    CHECK_EQUAL(2U, dst[1]);
    CHECK_TRUE(a.IsEmpty());

    // nothing at all
    n = 99;
    CHECK_EQUAL(WAIT_RESULT_TIMEOUT, a.p.ReadBulkTriggeredEx(dst, 3, 8, n, NO_WAIT));
    CHECK_EQUAL(0U, n);

    CHECK_EQUAL(0U, a.p.ReadBulkTriggered(dst, 3, 8, NO_WAIT));
    CHECK_EQUAL(0U, a.p.TryReadBulkTriggered(dst, 8));
}

TEST(Sync, Pipe_TryReadBulkTriggered)
{
    MtxFixture f;
    PipeA a;

    uint32_t src[3] = { 1U, 2U, 3U }, dst[3];

    CHECK_EQUAL(3U, a.TryWriteBulk(src, 3));

    CHECK_EQUAL(2U, a.p.TryReadBulkTriggered(dst, 2));
    CHECK_EQUAL(1U, a.p.TryReadBulkTriggered(dst, 2));
    CHECK_EQUAL(3U, dst[0]);
    CHECK_EQUAL(0U, a.p.TryReadBulkTriggered(dst, 2));
}

// the data is drained across the end of the ring
TEST(Sync, Pipe_ReadBulkTriggered_WrapAround)
{
    MtxFixture f;
    PipeA a;

    uint32_t src[PIPE_CAP], dst[PIPE_CAP];

    PipeSeq(src, 1U, PIPE_CAP);
    CHECK_EQUAL(PIPE_CAP, a.TryWriteBulk(src, PIPE_CAP));
    CHECK_EQUAL(6U, a.TryReadBulk(dst, 6)); // tail: 6

    PipeSeq(src, 11U, 5U);
    CHECK_EQUAL(5U, a.TryWriteBulk(src, 5)); // head wraps, 7 elements: {7, 8, 11..15}

    size_t n = 0;
    CHECK_EQUAL(WAIT_RESULT_SIGNAL, a.p.ReadBulkTriggeredEx(dst, 1, 7, n, NO_WAIT));
    CHECK_EQUAL(7U, n);

    const uint32_t expected[7] = { 7U, 8U, 11U, 12U, 13U, 14U, 15U };
    for (size_t i = 0; i < 7; ++i)
        CHECK_EQUAL(expected[i], dst[i]);
}

TEST(Sync, Pipe_ReadBulkTriggered_Blocked_WokenByWrite)
{
    MtxFixture f;
    PipeA a;

    CHECK_TRUE(a.TryWrite(1U));

    f.fake.Script(WAIT_RESULT_SIGNAL);
    f.fake.on_wait = [&](uint32_t)
    {
        uint32_t src[2] = { 2U, 3U };
        CHECK_EQUAL(2U, a.TryWriteBulk(src, 2));
    };

    uint32_t dst[PIPE_CAP];
    size_t n = 0;
    CHECK_EQUAL(WAIT_RESULT_SIGNAL, a.p.ReadBulkTriggeredEx(dst, 3, 8, n, 10));
    CHECK_EQUAL(3U, n);
    CHECK_EQUAL(1, f.fake.wait_calls);
    CHECK_EQUAL(3U, dst[2]);
    CHECK_TRUE(a.IsEmpty());
}

// woken before the threshold was reached: the wait continues for the remaining time only
TEST(Sync, Pipe_ReadBulkTriggered_Timed_SpuriousWake_RemainingTime)
{
    MtxFixture f;
    PipeA a;

    f.fake.m_ticks = 100;

    f.fake.Script(WAIT_RESULT_SIGNAL, WAIT_RESULT_SIGNAL);
    f.fake.on_wait = [&](uint32_t call)
    {
        f.fake.m_ticks = 100 + 2 * call;

        uint32_t v = 10U + call;
        CHECK_TRUE(a.TryWrite(v)); // one element per wake-up: the threshold of 2 is reached by the 2nd one
    };

    uint32_t dst[PIPE_CAP];
    size_t n = 0;
    CHECK_EQUAL(WAIT_RESULT_SIGNAL, a.p.ReadBulkTriggeredEx(dst, 2, 8, n, 10));
    CHECK_EQUAL(2U, n);
    CHECK_EQUAL(2, f.fake.wait_calls);
    CHECK_EQUAL(8, f.fake.last_wait_timeout); // 10 ticks from tick 100, 2 are gone
    CHECK_EQUAL(11U, dst[0]);
    CHECK_EQUAL(12U, dst[1]);
}

TEST(Sync, Pipe_ReadBulkTriggered_Timed_DeadlinePassed)
{
    MtxFixture f;
    PipeA a;

    f.fake.m_ticks = 100;

    f.fake.Script(WAIT_RESULT_SIGNAL, WAIT_RESULT_TIMEOUT);
    f.fake.on_wait = [&](uint32_t call)
    {
        if (call == 1)
            f.fake.m_ticks = 200;
    };

    uint32_t dst[PIPE_CAP];
    size_t n = 99;
    CHECK_EQUAL(WAIT_RESULT_TIMEOUT, a.p.ReadBulkTriggeredEx(dst, 2, 8, n, 10));
    CHECK_EQUAL(0U, n);
    CHECK_EQUAL(10, f.fake.last_wait_timeout); // an expired wait is a NO_WAIT one: it is completed without the kernel, the last kernel wait is the first one
}

template <class Dummy>
static void PipeTriggeredOutcome(EWaitResult outcome)
{
    MtxFixture f;
    PipeA a;

    CHECK_TRUE(a.TryWrite(7U));

    f.fake.Script(outcome);

    uint32_t dst[PIPE_CAP];
    size_t n = 99;
    CHECK_EQUAL(outcome, a.p.ReadBulkTriggeredEx(dst, 3, 8, n, 10));

    // the element which was available is drained
    CHECK_EQUAL(1U, n);
    CHECK_EQUAL(7U, dst[0]);
    CHECK_TRUE(a.IsEmpty());
}

TEST(Sync, Pipe_ReadBulkTriggered_Wait_Timeout)  { PipeTriggeredOutcome<int>(WAIT_RESULT_TIMEOUT);  }
TEST(Sync, Pipe_ReadBulkTriggered_Wait_Canceled) { PipeTriggeredOutcome<int>(WAIT_RESULT_CANCELED); }
TEST(Sync, Pipe_ReadBulkTriggered_Wait_Failed)   { PipeTriggeredOutcome<int>(WAIT_RESULT_FAIL);     }

TEST(Sync, Pipe_ReadBulkTriggered_Wait_Deleted)
{
    MtxFixture f;
    PipeA a;

    f.fake.Script(WAIT_RESULT_DELETED);

    uint32_t dst[PIPE_CAP];
    size_t n = 99;
    CHECK_EQUAL(WAIT_RESULT_DELETED, a.p.ReadBulkTriggeredEx(dst, 3, 8, n, 10));
}

// ---- sync::PipeT: element copy paths ----

struct PipeItem
{
    uint32_t a;
    uint32_t b;
};

// non-scalar elements and small pipes (N < 8) are copied element by element
TEST(Sync, PipeT_ElementWiseCopy_NonScalar)
{
    MtxFixture f;
    sync::PipeT<PipeItem, 4> p;

    CHECK_EQUAL(4U, p.GetCapacity());

    PipeItem src[4] = { { 1U, 10U }, { 2U, 20U }, { 3U, 30U }, { 4U, 40U } };
    PipeItem dst[4] = {};

    CHECK_EQUAL(3U, p.TryWriteBulk(src, 3));
    CHECK_EQUAL(2U, p.TryReadBulk(dst, 2));
    CHECK_EQUAL(2U, dst[1].a);
    CHECK_EQUAL(20U, dst[1].b);

    // wraps the ring
    CHECK_EQUAL(3U, p.TryWriteBulk(src + 1, 4)); // 3 free slots
    CHECK_EQUAL(4U, p.GetCount());

    memset(dst, 0, sizeof(dst));
    CHECK_EQUAL(4U, p.TryReadBulk(dst, 4));
    CHECK_EQUAL(3U, dst[0].a);
    CHECK_EQUAL(2U, dst[1].a);
    CHECK_EQUAL(3U, dst[2].a);
    CHECK_EQUAL(4U, dst[3].a);
    CHECK_EQUAL(40U, dst[3].b);

    // single element
    PipeItem item = { 5U, 50U }, out = {};
    CHECK_TRUE(p.TryWrite(item));
    CHECK_TRUE(p.TryRead(out));
    CHECK_EQUAL(5U, out.a);
    CHECK_EQUAL(50U, out.b);
}

TEST(Sync, PipeT_ElementWiseCopy_SmallScalar)
{
    MtxFixture f;
    sync::PipeT<uint32_t, 4> p;

    uint32_t src[4] = { 1U, 2U, 3U, 4U }, dst[4] = {};

    CHECK_EQUAL(3U, p.TryWriteBulk(src, 3));
    CHECK_EQUAL(3U, p.TryReadBulk(dst, 3));
    CHECK_EQUAL(3U, dst[2]);

    CHECK_EQUAL(4U, p.TryWriteBulk(src, 4)); // wraps
    CHECK_TRUE(p.IsFull());

    memset(dst, 0, sizeof(dst));
    CHECK_EQUAL(4U, p.TryReadBulk(dst, 4));
    CHECK_EQUAL(1U, dst[0]);
    CHECK_EQUAL(4U, dst[3]);
    CHECK_TRUE(p.IsEmpty());
}

// ============================================================================ //
// =============================== sync::SpinLock ============================= //
// ============================================================================ //

// The lock is recursive and bound to the id of the calling task, so the fake kernel service of the Mutex tests
// provides the "current task" (settable). The low-level hw::SpinLock of the test build does not spin: it only
// records the state, so a contended Lock() is not run to completion (it would never return on a target), the
// contention is observed through TryLock() and through the consistency check of the owner bookkeeping.

// takes the lock on behalf of a task, then switches back to the default task
static void SpinLockAs(MtxFixture &f, sync::SpinLock &lock, TId tid)
{
    f.fake.m_tid = tid;
    lock.Lock();
    f.fake.m_tid = MTX_CALLER;
}

static void SpinUnlockAs(MtxFixture &f, sync::SpinLock &lock, TId tid)
{
    f.fake.m_tid = tid;
    lock.Unlock();
    f.fake.m_tid = MTX_CALLER;
}

// is the lock available to the other task: probes with TryLock() and releases it again
static bool SpinLockFreeFor(MtxFixture &f, sync::SpinLock &lock, TId tid)
{
    f.fake.m_tid = tid;
    const bool free = lock.TryLock();
    if (free)
        lock.Unlock();
    f.fake.m_tid = MTX_CALLER;
    return free;
}

TEST(Sync, SpinLock_LockUnlock)
{
    MtxFixture f;
    sync::SpinLock lock;

    CHECK_TRUE(SpinLockFreeFor(f, lock, MTX_OWNER));

    lock.Lock();

    // held: the other task cannot take it
    CHECK_FALSE(SpinLockFreeFor(f, lock, MTX_OWNER));

    lock.Unlock();

    CHECK_TRUE(SpinLockFreeFor(f, lock, MTX_OWNER));
    CHECK_EQUAL(0, test::g_CriticalSectionState);
}

TEST(Sync, SpinLock_TryLock)
{
    MtxFixture f;
    sync::SpinLock lock;

    CHECK_TRUE(lock.TryLock());

    // owned by this task: recursive acquisition
    CHECK_TRUE(lock.TryLock());

    // owned by another task: refused, the ownership is unchanged
    f.fake.m_tid = MTX_OWNER;
    CHECK_FALSE(lock.TryLock());
    CHECK_FALSE(lock.TryLock());
    f.fake.m_tid = MTX_CALLER;

    lock.Unlock();
    CHECK_FALSE(SpinLockFreeFor(f, lock, MTX_OWNER)); // still held once

    lock.Unlock();
    CHECK_TRUE(SpinLockFreeFor(f, lock, MTX_OWNER));
}

// the owner can take the lock any number of times, it is released by the matching number of Unlock() calls
TEST(Sync, SpinLock_Recursive)
{
    MtxFixture f;
    sync::SpinLock lock;

    lock.Lock();
    lock.Lock();
    CHECK_TRUE(lock.TryLock());
    lock.Lock();

    for (int i = 0; i < 3; ++i)
    {
        lock.Unlock();
        CHECK_FALSE(SpinLockFreeFor(f, lock, MTX_OWNER)); // still held
    }

    lock.Unlock();
    CHECK_TRUE(SpinLockFreeFor(f, lock, MTX_OWNER));
}

// the lock goes from one task to another
TEST(Sync, SpinLock_HandOverBetweenTasks)
{
    MtxFixture f;
    sync::SpinLock lock;

    SpinLockAs(f, lock, MTX_OWNER);
    CHECK_FALSE(lock.TryLock());

    SpinUnlockAs(f, lock, MTX_OWNER);
    CHECK_TRUE(lock.TryLock());

    // and the other task is excluded now
    CHECK_FALSE(SpinLockFreeFor(f, lock, MTX_OWNER));

    lock.Unlock();
    CHECK_TRUE(SpinLockFreeFor(f, lock, MTX_OWNER));
}

// a lock owned by another task: the low-level lock is not honored (it does not spin in the test build), which
// breaks the owner bookkeeping: an internal defect is reported
TEST(Sync, SpinLock_Lock_OwnerBookkeepingOutOfSync_Panics)
{
    MtxFixture f;
    sync::SpinLock lock;

    SpinLockAs(f, lock, MTX_OWNER);

    test::g_PanicValue = KERNEL_PANIC_NONE;

    g_TestContext.ExpectPanic(true);
    try
    {
        lock.Lock(); // by MTX_CALLER
    }
    catch (std::exception &)
    {}
    g_TestContext.ExpectPanic(false);

    CHECK_EQUAL((int)KERNEL_PANIC_ASSERT, (int)test::g_PanicValue);

    // the panic handler of the test returns: the bookkeeping now names the caller as the owner
    lock.Unlock();
    CHECK_EQUAL(0, test::g_CriticalSectionState);
}

// ---- API contract ----

TEST(Sync, SpinLock_Unlock_NotOwner_Asserts)
{
    MtxFixture f;
    sync::SpinLock lock;

    // free lock
    try
    {
        g_TestContext.ExpectAssert(true);
        lock.Unlock();
        CHECK(false); // should asssert in a DEBUG build
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    // lock owned by another task
    SpinLockAs(f, lock, MTX_OWNER);

    try
    {
        g_TestContext.ExpectAssert(true);
        lock.Unlock();
        CHECK(false); // should asssert in a DEBUG build
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    // the ownership is intact
    CHECK_FALSE(lock.TryLock());
    SpinUnlockAs(f, lock, MTX_OWNER);
    CHECK_TRUE(SpinLockFreeFor(f, lock, MTX_CALLER));
}

TEST(Sync, SpinLock_Unlock_InsideISR_Asserts)
{
    MtxFixture f;
    sync::SpinLock lock;

    lock.Lock();

    try
    {
        ScopedInsideISR isr;
        g_TestContext.ExpectAssert(true);
        lock.Unlock();
        CHECK(false); // should asssert in a DEBUG build
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    lock.Unlock();
    CHECK_TRUE(SpinLockFreeFor(f, lock, MTX_OWNER));
}

TEST(Sync, SpinLock_RecursionDepthLimit_Asserts)
{
    MtxFixture f;
    sync::SpinLock lock;

    const uint32_t max_depth = 0xFFFEU;

    for (uint32_t i = 0; i < max_depth; ++i)
        lock.Lock();

    try
    {
        g_TestContext.ExpectAssert(true);
        lock.Lock();
        CHECK(false); // should asssert in a DEBUG build
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    for (uint32_t i = 0; i < max_depth; ++i)
        lock.Unlock();

    CHECK_TRUE(SpinLockFreeFor(f, lock, MTX_OWNER));
}

// ============================================================================ //
// ================= KernelTask::IsSyncWaiting (via Kernel) =================== //
// ============================================================================ //

// exposes KernelTask::IsSyncWaiting() of the task bound to a given user task
template <uint8_t TMode>
struct IsSyncWaitingKernel : public Kernel<TMode, 1, sched::SwitchStrategyRR, PlatformTestMock>
{
    typedef Kernel<TMode, 1, sched::SwitchStrategyRR, PlatformTestMock> BaseType;

    // false if the user task is not bound to any slot
    bool CallIsSyncWaiting(const ITask *user_task)
    {
        const typename BaseType::KernelTask *const task = BaseType::FindTaskByUserTask(user_task);

        return (task != nullptr) && task->IsSyncWaiting();
    }

    bool IsTaskBound(const ITask *user_task)
    {
        return (BaseType::FindTaskByUserTask(user_task) != nullptr);
    }
};

typedef IsSyncWaitingKernel<KERNEL_STATIC | KERNEL_SYNC> IsSyncWaitingSyncKernel;

static struct IsSyncWaitingContext
{
    IsSyncWaitingContext()
    {
        Reset();
    }

    void Reset()
    {
        counter           = 0;
        wake              = false;
        waiting_on_entry  = false;
        waiting_before_wake = false;
        waiting_after_wake  = true;
        platform          = NULL;
        kernel            = NULL;
        sobj              = NULL;
        task              = NULL;
    }

    uint32_t               counter;
    bool                   wake;                // wake the waiter on 5th tick, otherwise let the wait time out
    bool                   waiting_on_entry;    // IsSyncWaiting() on the first spin iteration of the blocked task
    bool                   waiting_before_wake; // IsSyncWaiting() right before WakeOne()
    bool                   waiting_after_wake;  // IsSyncWaiting() right after WakeOne()
    PlatformTestMock      *platform;
    IsSyncWaitingSyncKernel *kernel;
    SyncObjectMock        *sobj;
    ITask                 *task;

    void Process()
    {
        if (counter == 0)
        {
            waiting_on_entry = kernel->CallIsSyncWaiting(task);
        }

        platform->ProcessTick();
        ++counter;

        if (wake && (counter == 5))
        {
            waiting_before_wake = kernel->CallIsSyncWaiting(task);

            sobj->WakeOne();

            waiting_after_wake = kernel->CallIsSyncWaiting(task);
        }
    }
}
g_IsSyncWaitingContext;

static void IsSyncWaitingRelaxCpu()
{
    g_IsSyncWaitingContext.Process();
}

static void Test_IsSyncWaiting(bool wake, Timeout wait_ticks, EWaitResult expected_result)
{
    IsSyncWaitingSyncKernel kernel;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    TaskMock<ACCESS_USER> task;
    MutexMock mutex;
    SyncObjectMock sobj;

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    // not waiting before any wait is started
    CHECK_TRUE(kernel.IsTaskBound(&task));
    CHECK_FALSE(kernel.CallIsSyncWaiting(&task));

    g_IsSyncWaitingContext.Reset();
    g_IsSyncWaitingContext.platform = platform;
    g_IsSyncWaitingContext.kernel   = &kernel;
    g_IsSyncWaitingContext.sobj     = &sobj;
    g_IsSyncWaitingContext.task     = &task;
    g_IsSyncWaitingContext.wake     = wake;

    decltype(g_RelaxCpuHandler) prev_handler = g_RelaxCpuHandler;
    g_RelaxCpuHandler = IsSyncWaitingRelaxCpu;

    MutexMock::ScopedLock guard(mutex);

    const EWaitResult wresult = IKernelService::GetInstance()->Wait(&sobj, &mutex, wait_ticks);

    g_RelaxCpuHandler = prev_handler;

    CHECK_EQUAL(expected_result, wresult);

    // the task was registered in the sync object's wait list while it was blocked
    CHECK_TRUE(g_IsSyncWaitingContext.waiting_on_entry);

    if (wake)
    {
        CHECK_TRUE(g_IsSyncWaitingContext.waiting_before_wake);

        // Wake() removes the wait object from the wait list immediately
        CHECK_FALSE(g_IsSyncWaitingContext.waiting_after_wake);
    }

    // however the wait ended, the task is not waiting anymore
    CHECK_FALSE(kernel.CallIsSyncWaiting(&task));
}

TEST(Sync, KernelTask_IsSyncWaiting_WokenBySignal)
{
    Test_IsSyncWaiting(true, 10, WAIT_RESULT_SIGNAL);
}

TEST(Sync, KernelTask_IsSyncWaiting_EndedByTimeout)
{
    Test_IsSyncWaiting(false, 4, WAIT_RESULT_TIMEOUT);
}

TEST(Sync, KernelTask_IsSyncWaiting_NotSyncMode)
{
    // without KERNEL_SYNC there is no wait object: always false
    IsSyncWaitingKernel<KERNEL_STATIC> kernel;
    TaskMock<ACCESS_USER> task;

    kernel.Initialize();
    kernel.AddTask(&task);

    CHECK_TRUE(kernel.IsTaskBound(&task));
    CHECK_FALSE(kernel.CallIsSyncWaiting(&task));

    kernel.Start();

    CHECK_FALSE(kernel.CallIsSyncWaiting(&task));
}

#if CPPUTEST_USE_NEW_MACROS
#include "CppUTest/MemoryLeakDetectorNewMacros.h"
#endif

} // namespace stk
} // namespace test
