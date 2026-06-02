/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef DRIVER_LED_H_
#define DRIVER_LED_H_

#include "defs.h"

typedef enum LedId
{
    LED_GREEN,
    LED_ORANGE,
    LED_RED,
    LED_BLUE,

    LED_MAX
}
LedId;

#ifdef __cplusplus

namespace bsp {

struct Led
{
    using Id = LedId;

    static constexpr Id GREEN  = LED_GREEN;
    static constexpr Id ORANGE = LED_ORANGE;
    static constexpr Id RED    = LED_RED;
    static constexpr Id BLUE   = LED_BLUE;

    static void Init(Id led, bool init_state);
    static inline void InitAll(bool init_state)
    {
        for (uint8_t led = 0U; led < LED_MAX; ++led)
        {
            Led::Init(static_cast<LedId>(led), init_state);
        }
    }
    static void Set(Id led, bool state);
    static inline void SwitchOnExclusive(Id led)
    {
        for (uint8_t i = 0U; i < LED_MAX; ++i)
        {
            Led::Set(static_cast<LedId>(i), (i == led));
        }
    }
};

} // namespace bsp

#endif // __cplusplus

STK_EXTERN void Led_Init(LedId led, bool init_state);
STK_EXTERN void Led_InitAll(bool init_state);
STK_EXTERN void Led_Set(LedId led, bool state);
STK_EXTERN void Led_SwitchOnExclusive(LedId led);

#endif /* DRIVER_LED_H_ */
