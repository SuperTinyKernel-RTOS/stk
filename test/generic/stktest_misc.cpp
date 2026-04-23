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
// =============================== Basic ====================================== //
// ============================================================================ //

TEST_GROUP(Basic)
{
    void setup() {}
    void teardown() {}
};

TEST(Basic, MinMax)
{
    static_assert(stk::Min(10, 20) == 10, "Min failed at compile time");
    static_assert(stk::Max(10, 20) == 20, "Max failed at compile time");

    // basic & symmetry
    CHECK_EQUAL(1, stk::Min(1, 2));
    CHECK_EQUAL(1, stk::Min(2, 1));

    // negatives
    CHECK_EQUAL(-5, stk::Min(-5, -2));
    CHECK_EQUAL(-2, stk::Max(-5, -2));

    // equality
    CHECK_EQUAL(42, stk::Max(42, 42));

    // boundaries (assuming 32-bit ints)
    CHECK_EQUAL(2147483647, stk::Max(0, 2147483647));

    // support const vars
    {
        const int32_t a = 10;
        const int32_t b = 20;
        CHECK_EQUAL(10, stk::Min(a, b));
    }

    // no duplicate operation
    {
        int32_t x = 1;
        int32_t y = 5;

        // in a macro-based MIN(x++, y), x would become 3, with STK's template-based stk::Min, x should become 2
        int32_t result = stk::Min(x++, y);

        CHECK_EQUAL(1, result); // the value before increment
        CHECK_EQUAL(2, x);      // x should only have been incremented once
        CHECK_EQUAL(5, y);      // y remains untouched
    }
}

TEST(Basic, ISwitchStrategyStub)
{
    SwitchStrategyRR ss;

    // these are just stubs for interface noop functions to achieve 100% coverage, providing nullptr
    // for ITasks should be noop too
    ss.OnTaskDeadlineMissed(nullptr);
    ss.OnTaskWeightChange(nullptr, NO_WEIGHT);
}

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

    CHECK_EQUAL(TaskMock<ACCESS_USER>::STACK_SIZE * sizeof(Word), task.GetStackSizeBytes());
    CHECK_EQUAL(TaskMock<ACCESS_USER>::STACK_SIZE * sizeof(Word), taskw.GetStackSizeBytes());
}

TEST(UserTask, GetStackSpace)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;

    // not initialized with STK_STACK_MEMORY_FILLER
    CHECK_EQUAL(0, task.GetStackSpace());

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    size_t space = task.GetStackSpace();

    // initialized with STK_STACK_MEMORY_FILLER, all free
    CHECK_EQUAL(TaskMock<ACCESS_USER>::STACK_SIZE, space);

    // write something to bottom
    task.GetStack()[task.GetStackSize() - 1] = 0x12345678;
    task.GetStack()[task.GetStackSize() - 2] = 0x12345678;

    space = task.GetStackSpace();

    // consumed 1 Word
    CHECK_EQUAL(TaskMock<ACCESS_USER>::STACK_SIZE - 2, space);
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

    CHECK_EQUAL((TId)&task, task.GetId());
    CHECK_EQUAL((TId)&taskw, taskw.GetId());

    CHECK_EQUAL((TId)&task, stk::GetTidFromUserTask(&task));
    CHECK_EQUAL((TId)&taskw, stk::GetTidFromUserTask(&taskw));

    CHECK_EQUAL(&task, stk::GetUserTaskFromTid(stk::GetTidFromUserTask(&task)));
    CHECK_EQUAL(&taskw, stk::GetUserTaskFromTid(stk::GetTidFromUserTask(&taskw)));

    // expect NULL name by default
    CHECK_EQUAL((const char *)NULL, task.GetTraceName());
    CHECK_EQUAL((const char *)NULL, taskw.GetTraceName());
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
    CHECK_EQUAL((Word *)&memory, wrapper.GetStack());
}

TEST(StackMemoryWrapper, GetStackSize)
{
    StackMemoryWrapper<STACK_SIZE_MIN>::MemoryType memory;
    StackMemoryWrapper<STACK_SIZE_MIN> wrapper(&memory);

    CHECK_EQUAL(STACK_SIZE_MIN, wrapper.GetStackSize());
    CHECK_EQUAL(sizeof(memory) / sizeof(Word), wrapper.GetStackSize());
}

TEST(StackMemoryWrapper, GetStackSizeBytes)
{
    StackMemoryWrapper<STACK_SIZE_MIN>::MemoryType memory;
    StackMemoryWrapper<STACK_SIZE_MIN> wrapper(&memory);

    CHECK_EQUAL(STACK_SIZE_MIN * sizeof(Word), wrapper.GetStackSizeBytes());
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

} // namespace stk
} // namespace test
