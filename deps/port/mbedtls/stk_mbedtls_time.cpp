/**
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/**
 * \file stk_mbedtls_time.cpp
 *
 * \brief mbedTLS time abstraction port for
 *        SuperTinyKernel(TM) RTOS (STK) - implementation.
 *
 * Deploy this file, together with threading_alt.h, under
 * deps/port/mbedtls/ in the STK repository. Compile as C++ (it is included
 * in the mbedTLS build as an extra translation unit; only its extern "C"
 * entry points and the mbedtls_platform_*_t layout are visible to the rest
 * of mbedTLS, which stays plain C).
 */

#include "mbedtls/platform_util.h"
#include "mbedtls/platform.h"

#include "stk.h"

extern "C" mbedtls_ms_time_t mbedtls_ms_time(void)
{
    return static_cast<mbedtls_ms_time_t>(stk::GetTimeNowMs());
}
