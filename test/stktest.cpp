/*
 * SuperTinyKernel(TM) (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
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

IKernelService *IKernelService::GetInstance()
{
    return g_KernelService;
}

void stk::hw::CriticalSection::Enter()
{
    ++g_CriticalSectionState;
}
void stk::hw::CriticalSection::Exit()
{
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
