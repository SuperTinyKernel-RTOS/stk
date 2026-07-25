/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include "stktest.h"
#include <CppUTest/CommandLineTestRunner.h>

using namespace stk;
using namespace stk::test;

TestContext test::g_TestContext;
void (* g_RelaxCpuHandler)() = NULL;
IKernelService *test::g_KernelService = NULL;
int32_t test::g_CriticalSectionState = false;
EKernelPanicId test::g_PanicValue = KERNEL_PANIC_NONE;
bool test::g_InsideISR = false;
uintptr_t g_Tls = 0;

/*! \fn    STK_ASSERT_HANDLER
    \brief Custom assertion handler which intercepts assertions from STK package.
*/
void STK_ASSERT_HANDLER(const char *message, const char *file, int32_t line)
{
	if (g_TestContext.IsExpectingAssert())
	{
    #if CPPUTEST_HAVE_EXCEPTIONS
	    if (g_TestContext.IsRethrowingAssertException())
	        throw TestAssertPassed();
	    else
	        return;
    #else
		return;
    #endif
	}

	SimpleString what = "Assertion failed!\n";
	what += "\twhat: ";
	what += message;
	what += "\n\tfile: ";
	what += file;
	what += "\n\tline: ";
	what += StringFrom(line);

	CHECK_TEXT(false, what.asCharString());
}

/*! \fn    STK_PANIC_HANDLER_DEFAULT
    \brief Custom panic handler which intercepts panic invocations from the kernel.
*/
void STK_PANIC_HANDLER_DEFAULT(EKernelPanicId id)
{
    g_PanicValue = id;

    if (g_TestContext.IsExpectingPanic())
        return;

    SimpleString what = "Kernel panic!\n";
    what += "\n\tid: ";
    what += StringFrom(id);

    CHECK_TEXT(false, what.asCharString());
}

IKernelService *IKernelService::GetInstance()
{
    return g_KernelService;
}

stk::hw::CriticalSection::Session stk::hw::CriticalSection::Enter(CriticalSection::Session is_npriv)
{
    STK_UNUSED(is_npriv);

    ++g_CriticalSectionState;
}
void stk::hw::CriticalSection::Exit(CriticalSection::Session is_npriv)
{
    STK_UNUSED(is_npriv);

    --g_CriticalSectionState;

    CHECK_TRUE(g_CriticalSectionState >= 0);
}

void stk::hw::SpinLock::Lock()
{
    m_lock = true;
}
void stk::hw::SpinLock::Unlock()
{
    m_lock = false;
}

uintptr_t stk::hw::GetTls()
{
    return g_Tls;
}
void stk::hw::SetTls(uintptr_t tp)
{
    g_Tls = tp;
}

bool stk::hw::IsInsideISR()
{
    return g_InsideISR;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    TestContext::ShowTestSuitePrologue();

    int32_t result = RUN_ALL_TESTS(argc, argv);

    TestContext::ShowTestSuiteEpilogue(result);

    return result;
}
