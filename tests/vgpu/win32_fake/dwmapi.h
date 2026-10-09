/** @file dwmapi.h @brief Test-only DWM pacing declarations; not shipped SDK code. */
#ifndef WaddleTestDwmH
/** @brief Test include guard; no state. */
#define WaddleTestDwmH
#include <stdint.h>
/** @brief Interpret the native signed HRESULT success convention. */
#define SUCCEEDED(value) ((int32_t)(value) >= 0)
/** @brief Return configured fixture pacing result; single-thread, no ownership. */
int32_t DwmFlush(void);
#endif
