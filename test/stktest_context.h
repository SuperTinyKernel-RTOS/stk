/*
 * SuperTinyKernel(TM) (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STKTEST_CONTEX_H_
#define STKTEST_CONTEX_H_

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <exception>

namespace stk {
namespace test {

/*! \class TestContext
    \brief Common context for a test suite.
*/
class TestContext
{
public:
    enum EConsts
    {
        SUCCESS_EXIT_CODE         = 0,//!< exit code for exit() to denote the success of the test
        DEFAULT_FAILURE_EXIT_CODE = 1 //!< default exit code for exit() to denote failure of the test
    };

    explicit TestContext() : m_expect_assert(false), m_expect_panic(false), m_rethrow_assert(true)
    {}

    /*! \brief     Start expecting assertion for the test case.
        \param[in] expect: True to expect otherwise False.
    */
    void ExpectAssert(bool expect) { m_expect_assert = expect; }

    /*! \brief     Check if test case is expecting assertion.
    */
    bool IsExpectingAssert() const { return m_expect_assert; }

    /*! \brief     Start expecting kernel panic for the test case.
        \param[in] expect: True to expect otherwise False.
    */
    void ExpectPanic(bool expect) { m_expect_panic = expect; }

    /*! \brief     Check if test case is expecting kernel panic.
    */
    bool IsExpectingPanic() const { return m_expect_panic; }

    /*! \brief     Re-throw assert's exception for the test case.
        \param[in] expect: True to rethrow otherwise False.
    */
    void RethrowAssertException(bool rethrow) { m_rethrow_assert = rethrow; }

    /*! \brief     Check if exception must be thrown if assertion is hit.
    */
    bool IsRethrowingAssertException() const { return m_rethrow_assert; }

    /*! \brief     Show text string as prologue before tests start.
    */
    static inline void ShowTestSuitePrologue()
    {
        // required to show log in Eclipse IDE Console for 64-bit binary
    #if defined(_WIN32) || defined(WIN32)
        setbuf(stdout, NULL);
    #endif

        printf("STKTEST-START\n");
    }

    /*! \brief     Show text string as epilogue after tests end.
        \param[in] result: 0 if no tests are failed otherwise number of failed tests.
    */
    static inline void ShowTestSuiteEpilogue(int32_t result)
    {
        printf("STKTEST-RESULT: %d\n", (int)result);
    }

    /*! \brief     Exit test suite process forcibly.
        \param[in] result: 0 if no tests are failed otherwise number of failed tests.
    */
    static inline void ForceExitTestSuite(int32_t result)
    {
        ShowTestSuiteEpilogue(result);
        exit(result);
    }

private:
    static TestContext m_instance; //!< global instance of the TestContext
    bool m_expect_assert;          //!< assert expectation flag
    bool m_expect_panic;           //!< panic expectation flag
    bool m_rethrow_assert;         //!< rethrow assert's exception
};

/*! \var   g_TestContext
    \brief Global instance of the TestContext.
*/
extern TestContext g_TestContext;

/*! \def   STK_TEST_CHECK_EQUAL
    \brief Compare values for equality.
*/
#define STK_TEST_CHECK_EQUAL(expected, actual)\
    if ((expected) != (actual)) {\
        printf("STK_TEST_CHECK_EQUAL failed!\n file: %s\n line: %d\n  expected: %d\n  actual: %d\n", __FILE__, __LINE__, (int)expected, (int)actual);\
        __stk_debug_break();\
        exit(1);\
    }

/*! \def   STK_TEST_DECL_ASSERT
    \brief Declare assertion redirector in the source file.
*/
#define STK_TEST_DECL_ASSERT\
    extern void STK_ASSERT_HANDLER(const char *message, const char *file, int32_t line)\
    {\
        printf("_STK_ASSERT failed!\n file: %s\n line: %d\n message: %s\n", file, (int)line, message);\
        __stk_debug_break();\
        abort();\
    }

} // namespace test
} // namespace stk

#endif /* STKTEST_CONTEX_H_ */
