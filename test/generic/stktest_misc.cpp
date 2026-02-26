/*
 * SuperTinyKernel(TM) (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
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
// ============================= UserTask ===================================== //
// ============================================================================ //

TEST_GROUP(UserTask)
{
    void setup() {}
    void teardown() {}
};

TEST(UserTask, GetStackSize)
{
    TaskMock<ACCESS_USER> task;
    TaskMockW<1, ACCESS_USER> taskw;

    CHECK_EQUAL(TaskMock<ACCESS_USER>::STACK_SIZE, task.GetStackSize());
    CHECK_EQUAL(TaskMock<ACCESS_USER>::STACK_SIZE, taskw.GetStackSize());
}

TEST(UserTask, GetStackSizeBytes)
{
    TaskMock<ACCESS_USER> task;
    TaskMockW<1, ACCESS_USER> taskw;

    CHECK_EQUAL(TaskMock<ACCESS_USER>::STACK_SIZE * sizeof(size_t), task.GetStackSizeBytes());
    CHECK_EQUAL(TaskMock<ACCESS_USER>::STACK_SIZE * sizeof(size_t), taskw.GetStackSizeBytes());
}

TEST(UserTask, GetWeight)
{
    TaskMock<ACCESS_USER> task;
    TaskMockW<10, ACCESS_USER> taskw;

    // TaskMock inherits Task and Task does not support weights (1)
    CHECK_EQUAL(1, task.GetWeight());

    // TaskMockW supports weights as it inherits TaskW
    CHECK_EQUAL(10, taskw.GetWeight());
}

TEST(UserTask, GetIdAndName)
{
    TaskMock<ACCESS_USER> task;
    TaskMockW<1, ACCESS_USER> taskw;

    CHECK_EQUAL((size_t)&task, task.GetId());
    CHECK_EQUAL((size_t)&taskw, taskw.GetId());

    // expect NULL name by default
    CHECK_EQUAL((const char *)NULL, task.GetTraceName());
    CHECK_EQUAL((const char *)NULL, taskw.GetTraceName());
}

TEST(UserTask, TaskWUnsupportedHrt)
{
    TaskMockW<10, ACCESS_USER> taskw;

    try
    {
        g_TestContext.ExpectAssert(true);
        // on next tick kernel will attempt to remove pending task and will check its deadline
        taskw.OnDeadlineMissed(0);
        CHECK_TEXT(false, "expecting assertion - task with weights do not support HRT");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

TEST_GROUP(StackMemoryWrapper)
{
    void setup() {}
    void teardown() {}
};

TEST(StackMemoryWrapper, GetStack)
{
    StackMemoryWrapper<STACK_SIZE_MIN>::MemoryType memory;
    StackMemoryWrapper<STACK_SIZE_MIN> wrapper(&memory);

    CHECK_TRUE(NULL != wrapper.GetStack());
    CHECK_EQUAL((size_t *)&memory, wrapper.GetStack());
}

TEST(StackMemoryWrapper, GetStackSize)
{
    StackMemoryWrapper<STACK_SIZE_MIN>::MemoryType memory;
    StackMemoryWrapper<STACK_SIZE_MIN> wrapper(&memory);

    CHECK_EQUAL(STACK_SIZE_MIN, wrapper.GetStackSize());
    CHECK_EQUAL(sizeof(memory) / sizeof(size_t), wrapper.GetStackSize());
}

TEST(StackMemoryWrapper, GetStackSizeBytes)
{
    StackMemoryWrapper<STACK_SIZE_MIN>::MemoryType memory;
    StackMemoryWrapper<STACK_SIZE_MIN> wrapper(&memory);

    CHECK_EQUAL(STACK_SIZE_MIN * sizeof(size_t), wrapper.GetStackSizeBytes());
    CHECK_EQUAL(sizeof(memory), wrapper.GetStackSizeBytes());
}

TEST_GROUP(DList)
{
    void setup() {}
    void teardown() {}

    struct ListEntry : public stk::util::DListEntry<ListEntry, true>
    {
        int32_t m_id;
        ListEntry(int32_t id) : m_id(id) {}
    };

    typedef stk::util::DListHead<ListEntry, true> ListHead;
};

TEST(DList, Empty)
{
    ListHead list;

    CHECK_EQUAL_ZERO(list.GetSize());
    CHECK_TRUE(list.IsEmpty());
    CHECK_TRUE(NULL == list.GetFirst());
    CHECK_TRUE(NULL == list.GetLast());

    list.Clear();
}

TEST(DList, LinkFront)
{
    ListHead list;
    ListEntry e1(1), e2(2), e3(3);

    list.LinkFront(e1);
    CHECK_EQUAL(&e1, list.GetFirst());
    CHECK_EQUAL(&e1, list.GetLast());
    CHECK_EQUAL(&list, e1.GetHead());

    list.LinkFront(e2);
    CHECK_EQUAL(&e2, list.GetFirst());
    CHECK_EQUAL(&e1, list.GetLast());

    list.LinkFront(e3);
    CHECK_EQUAL(&e3, list.GetFirst());
    CHECK_EQUAL(&e1, list.GetLast());

    CHECK_EQUAL(3, list.GetSize());
}

TEST(DList, LinkBack)
{
    ListHead list;
    ListEntry e1(1), e2(2), e3(3);

    list.LinkBack(e1);
    CHECK_EQUAL(&e1, list.GetFirst());
    CHECK_EQUAL(&e1, list.GetLast());
    CHECK_EQUAL(&list, e1.GetHead());

    list.LinkBack(e2);
    CHECK_EQUAL(&e1, list.GetFirst());
    CHECK_EQUAL(&e2, list.GetLast());

    list.LinkBack(e3);
    CHECK_EQUAL(&e1, list.GetFirst());
    CHECK_EQUAL(&e3, list.GetLast());

    CHECK_EQUAL(3, list.GetSize());
}

TEST(DList, PopFront)
{
    ListHead list;
    ListEntry e1(1), e2(2), e3(3);

    list.LinkBack(e1);
    list.LinkBack(e2);
    list.LinkBack(e3);

    list.PopFront();

    CHECK_EQUAL(&e2, list.GetFirst());
    CHECK_EQUAL(&e3, list.GetLast());

    list.PopFront();

    CHECK_EQUAL(&e3, list.GetFirst());
    CHECK_EQUAL(&e3, list.GetLast());

    list.PopFront();

    CHECK_EQUAL_ZERO(list.GetSize());
    CHECK_TRUE(list.IsEmpty());
    CHECK_TRUE(NULL == list.GetFirst());
    CHECK_TRUE(NULL == list.GetLast());
}

TEST(DList, PopBack)
{
    ListHead list;
    ListEntry e1(1), e2(2), e3(3);

    list.LinkBack(e1);
    list.LinkBack(e2);
    list.LinkBack(e3);

    list.PopBack();

    CHECK_EQUAL(&e1, list.GetFirst());
    CHECK_EQUAL(&e2, list.GetLast());

    list.PopBack();

    CHECK_EQUAL(&e1, list.GetFirst());
    CHECK_EQUAL(&e1, list.GetLast());

    list.PopBack();

    CHECK_EQUAL_ZERO(list.GetSize());
    CHECK_TRUE(list.IsEmpty());
    CHECK_TRUE(NULL == list.GetFirst());
    CHECK_TRUE(NULL == list.GetLast());
}

TEST(DList, Clear)
{
    ListHead list;
    ListEntry e1(1), e2(2), e3(3);

    list.LinkBack(e1);
    list.LinkBack(e2);
    list.LinkBack(e3);

    list.Clear();

    CHECK_EQUAL_ZERO(list.GetSize());
    CHECK_TRUE(list.IsEmpty());
    CHECK_TRUE(NULL == list.GetFirst());
    CHECK_TRUE(NULL == list.GetLast());
}

TEST(DList, Iteration)
{
    ListHead list;
    ListEntry e1(1), e2(2), e3(3);

    list.LinkBack(e1);
    list.LinkBack(e2);
    list.LinkBack(e3);

    ListHead::DLEntryType *itr = list.GetFirst();
    CHECK_EQUAL(&e1, itr);

    itr = itr->GetNext();
    CHECK_EQUAL(&e2, itr);

    itr = itr->GetNext();
    CHECK_EQUAL(&e3, itr);

    itr = itr->GetNext();
    CHECK_EQUAL(&e1, itr);
}

TEST(DList, Relink)
{
    ListHead list;
    ListEntry e1(1), e2(2), e3(3);

    list.LinkBack(e1);
    list.LinkBack(e2);
    list.LinkBack(e3);

    ListHead list2;
    list.RelinkTo(list2);

    CHECK_EQUAL_ZERO(list.GetSize());
    CHECK_TRUE(list.IsEmpty());
    CHECK_TRUE(NULL == list.GetFirst());
    CHECK_TRUE(NULL == list.GetLast());

    CHECK_EQUAL(3, list2.GetSize());
    CHECK_EQUAL(&list2, e1.GetHead());
    CHECK_EQUAL(&list2, e2.GetHead());
    CHECK_EQUAL(&list2, e3.GetHead());
}

// ============================================================================ //
// ============================= PeriodicTimer ================================ //
// ============================================================================ //

//! Mock of GetTimeNowMsec().
namespace stk
{
    static struct KernelServiceMock : public IKernelService
    {
        int64_t ticks;
        int32_t resolution;

        static IKernelService *GetInstance() { return NULL; }
        TId GetTid() const { return 0; }
        Ticks GetTicks() const { return ticks; }
        int32_t GetTickResolution() const { return resolution; }
        void Delay(Timeout msec) { (void)msec; }
        void Sleep(Timeout msec) { (void)msec; }
        void SwitchToNext() {}
        IWaitObject *StartWaiting(ISyncObject *sobj, IMutex *mutex, Timeout timeout)
        {
        	(void)sobj;
        	(void)mutex;
        	(void)timeout;
        	return nullptr;
		}
    }
    s_KernelServiceMock;

    void SetTimeNowMsec(int64_t now)
    {
        test::g_KernelService = &s_KernelServiceMock;

        s_KernelServiceMock.resolution = 1000;
        s_KernelServiceMock.ticks = GetTicksFromMsec(now, s_KernelServiceMock.resolution);
    }
}

TEST_GROUP(PeriodicTimer)
{
    enum { PERIOD = 100 };

    void setup()
    {
        stk::SetTimeNowMsec(0);
    }
};

TEST(PeriodicTimer, DoesNotFireBeforePeriod)
{
    bool called = false;

    PeriodicTimer<PERIOD> timer;

    stk::SetTimeNowMsec(50);
    timer.Update([&](int64_t, uint32_t) {
        called = true;
    });

    CHECK_FALSE(called);
}

TEST(PeriodicTimer, FiresAtExactPeriod)
{
    bool called = false;
    int64_t cb_now = 0;
    uint32_t cb_cur = 0;

    PeriodicTimer<PERIOD> timer;

    stk::SetTimeNowMsec(100);
    timer.Update([&](int64_t now, uint32_t cur) {
        called = true;
        cb_now  = now;
        cb_cur  = cur;
    });

    CHECK_TRUE(called);
    LONGS_EQUAL(100, cb_now);
    UNSIGNED_LONGS_EQUAL(100, cb_cur);
}

TEST(PeriodicTimer, PreservesRemainderAfterFire)
{
    int call_count = 0;

    PeriodicTimer<PERIOD> timer;

    stk::SetTimeNowMsec(150);
    timer.Update([&](int64_t, uint32_t) {
        call_count++;
    });

    CHECK_EQUAL(1, call_count);

    stk::SetTimeNowMsec(190);
    timer.Update([&](int64_t, uint32_t) {
        call_count++;
    });

    CHECK_EQUAL(1, call_count); // only 60 ms accumulated
}

TEST(PeriodicTimer, AccumulatesAcrossUpdates)
{
    int call_count = 0;

    PeriodicTimer<PERIOD> timer;

    stk::SetTimeNowMsec(40);
    timer.Update([&](int64_t, uint32_t) { call_count++; });

    stk::SetTimeNowMsec(80);
    timer.Update([&](int64_t, uint32_t) { call_count++; });

    stk::SetTimeNowMsec(100);
    timer.Update([&](int64_t, uint32_t) { call_count++; });

    CHECK_EQUAL(1, call_count);
}

TEST(PeriodicTimer, ResetClearsAccumulatedTime)
{
    int call_count = 0;

    PeriodicTimer<PERIOD> timer;

    stk::SetTimeNowMsec(80);
    timer.Update([&](int64_t, uint32_t) { call_count++; });

    timer.Reset();

    stk::SetTimeNowMsec(150);
    timer.Update([&](int64_t, uint32_t) { call_count++; });

    CHECK_EQUAL(0, call_count);
}

} // namespace stk
} // namespace test
