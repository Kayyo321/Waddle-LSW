/**
 * @file guest_win32_compat.h
 * @brief Win32 type, struct and constant subset used by the guest keyboard executor.
 *
 * Why this header exists: guest_input.h exposes a table of real Win32 function pointers, so
 * its parameter types must be the real Win32 types. Windows builds (mingw and MSVC) get them
 * from <windows.h> unchanged. Other hosts, which run the Linux test build, get the same names
 * with the same widths and the same field layout for INPUT, KEYBDINPUT, MOUSEINPUT and
 * HARDWAREINPUT (x86-64 Windows layout: INPUT is 40 bytes, KEYBDINPUT is 24 bytes). The
 * constants carry the values Windows defines. No function is declared here: the table holds
 * the functions, and the Linux build only ever calls test doubles.
 *
 * Identifiers from the Windows API (SendInput, KEYEVENTF_SCANCODE, ...) keep their Windows
 * spelling. They are external names and cannot follow the repository naming rules.
 *
 * @note Header only. No allocation. Thread-safe trivially (types and constants).
 */
#ifndef WaddleGuestWin32CompatH
/** @brief Include guard; compile-time marker with no ownership. */
#define WaddleGuestWin32CompatH

#ifdef _WIN32

#include <windows.h>

#else

#include <stddef.h>
#include <stdint.h>

/** @brief Win32 BOOL: 32-bit signed truth value. */
typedef int32_t BOOL;
/** @brief Win32 UINT: 32-bit unsigned. */
typedef uint32_t UINT;
/** @brief Win32 DWORD: 32-bit unsigned. */
typedef uint32_t DWORD;
/** @brief Win32 WORD: 16-bit unsigned. */
typedef uint16_t WORD;
/** @brief Win32 SHORT: 16-bit signed. */
typedef int16_t SHORT;
/** @brief Win32 LONG: 32-bit signed. */
typedef int32_t LONG;
/** @brief Win32 ULONG_PTR: pointer-sized unsigned. */
typedef uintptr_t ULONG_PTR;
/** @brief Win32 WPARAM: pointer-sized unsigned message parameter. */
typedef uintptr_t WPARAM;
/** @brief Win32 LPARAM: pointer-sized signed message parameter. */
typedef intptr_t LPARAM;
/** @brief Win32 HWND: opaque window handle, modelled as a pointer. */
typedef void *HWND;
/** @brief Win32 HKL: opaque keyboard layout handle, modelled as a pointer. */
typedef void *HKL;
/** @brief Win32 LPDWORD: non-owning pointer to a DWORD. */
typedef DWORD *LPDWORD;
/** @brief Win32 LPCWSTR: non-owning pointer to a NUL-terminated UTF-16 string. */
typedef const wchar_t *LPCWSTR;

/** @brief Win32 MOUSEINPUT. Layout only; the executor never fills a mouse record. */
typedef struct MOUSEINPUT {
    LONG dx;              /**< Horizontal delta. */
    LONG dy;              /**< Vertical delta. */
    DWORD mouseData;      /**< Wheel or X-button data. */
    DWORD dwFlags;        /**< Mouse event flags. */
    DWORD time;           /**< Timestamp, zero for system time. */
    ULONG_PTR dwExtraInfo; /**< Extra information. */
} MOUSEINPUT;

/** @brief Win32 KEYBDINPUT: a keyboard event record. */
typedef struct KEYBDINPUT {
    WORD wVk;             /**< Virtual-key code, zero for scan-code events. */
    WORD wScan;           /**< Scan code, used with KEYEVENTF_SCANCODE or for Pause. */
    DWORD dwFlags;        /**< KEYEVENTF_* flags. */
    DWORD time;           /**< Timestamp, zero for system time. */
    ULONG_PTR dwExtraInfo; /**< Extra information. */
} KEYBDINPUT;

/** @brief Win32 HARDWAREINPUT. Layout only; never filled. */
typedef struct HARDWAREINPUT {
    DWORD uMsg;    /**< Hardware message. */
    WORD wParamL;  /**< Low word of the parameter. */
    WORD wParamH;  /**< High word of the parameter. */
} HARDWAREINPUT;

/** @brief Win32 INPUT: one synthesized input record passed to SendInput. */
typedef struct INPUT {
    DWORD type; /**< INPUT_KEYBOARD for keyboard records. */
    union {
        MOUSEINPUT mi;     /**< Mouse payload. */
        KEYBDINPUT ki;     /**< Keyboard payload. */
        HARDWAREINPUT hi;  /**< Hardware payload. */
    };
} INPUT;

/** @brief Win32 pointer to INPUT. Non-owning. */
typedef INPUT *LPINPUT;

/** @brief INPUT.type value for keyboard records. */
#define INPUT_KEYBOARD 1u
/** @brief KEYBDINPUT.dwFlags: the key is extended (E0 prefix for scan codes). */
#define KEYEVENTF_EXTENDEDKEY 0x0001u
/** @brief KEYBDINPUT.dwFlags: the event is a key release. */
#define KEYEVENTF_KEYUP 0x0002u
/** @brief KEYBDINPUT.dwFlags: wScan is used instead of wVk. */
#define KEYEVENTF_SCANCODE 0x0008u
/** @brief Virtual key Caps Lock. */
#define VK_CAPITAL 0x14
/** @brief Virtual key Num Lock. */
#define VK_NUMLOCK 0x90
/** @brief Virtual key Scroll Lock. */
#define VK_SCROLL 0x91
/** @brief Virtual key Pause. */
#define VK_PAUSE 0x13
/** @brief Message posted to a window to request an input language (keyboard layout) change. */
#define WM_INPUTLANGCHANGEREQUEST 0x0050u
/** @brief LoadKeyboardLayoutW flag: activate the loaded layout. */
#define KLF_ACTIVATE 0x00000001u
/** @brief LoadKeyboardLayoutW flag: the layout applies to the calling process. */
#define KLF_SETFORPROCESS 0x00000100u

#endif

#endif
