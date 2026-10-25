/** @file keymap_names.h @brief C ABI for XKB keymap name extraction (layout, variant, lock modifier indices). */
#ifndef WaddleKeymapNamesH
/** @brief Include guard; compile-time marker with no ownership. */
#define WaddleKeymapNamesH
#include <stddef.h>
#include <stdint.h>

/** @brief Maximum keymap text length in bytes accepted by waddle_keymap_names_parse. */
#define WaddleKeymapMaxBytes 262144u

/** @brief Maximum layout or variant name length in bytes. */
#define WaddleKeymapMaxNameBytes 64u

/** @brief Modifier index meaning the keymap did not resolve that lock modifier. */
#define WaddleKeymapModifierAbsent 0xFFu

/** @brief Status codes returned by waddle_keymap_names_parse. Ok is zero. */
typedef enum waddle_keymap_names_status_t {
    /** Parse succeeded; *out was fully written, zero-filled where a field is absent. */
    WaddleKeymapNamesOk = 0,
    /** out is NULL, or text is NULL while length is nonzero; nothing was written. */
    WaddleKeymapNamesNullArgument = 1,
    /** length exceeds WaddleKeymapMaxBytes; nothing was written. */
    WaddleKeymapNamesTooLarge = 2,
    /** A present layout or variant name is invalid or over-long; nothing was written. */
    WaddleKeymapNamesMalformedName = 3
} waddle_keymap_names_status_t;

/**
 * @brief Extracted keymap names. Plain data with no pointers and no padding; the
 * size is 136 bytes. Zero-filled fields mean absent. Caller owns the storage.
 * Layout invariant (documented, not compiled): sizeof(waddle_keymap_names_t) == 136.
 */
typedef struct waddle_keymap_names_t {
    /** 1 when a layout was found in the xkb_symbols include line, else 0. */
    uint8_t has_layout;
    /** Byte length of layout; 0 when absent. */
    uint8_t layout_len;
    /** Layout bytes matching [a-z][a-z0-9_]*; zero past layout_len. */
    uint8_t layout[WaddleKeymapMaxNameBytes];
    /** 1 when a non-empty variant was found for the layout, else 0. */
    uint8_t has_variant;
    /** Byte length of variant; 0 when absent. */
    uint8_t variant_len;
    /** Variant bytes matching [A-Za-z0-9_-]*; zero past variant_len. */
    uint8_t variant[WaddleKeymapMaxNameBytes];
    /** Core modifier index of Caps (Shift=0, Lock=1, Control=2, Mod1=3 .. Mod5=7), or WaddleKeymapModifierAbsent. */
    uint8_t caps;
    /** Core modifier index of Num lock, or WaddleKeymapModifierAbsent. */
    uint8_t num;
    /** Core modifier index of Scroll lock, or WaddleKeymapModifierAbsent. */
    uint8_t scroll;
    /**
     * 1 when no modifier_map statement was recognized and the fixed fallback
     * (caps=1, num=4, scroll=WaddleKeymapModifierAbsent) was written; else 0.
     */
    uint8_t fallback_used;
} waddle_keymap_names_t;

/**
 * @brief Parse XKB keymap text (format xkb_v1) and extract the first group's layout and
 * variant plus the Caps, Num and Scroll lock modifier indices.
 * @param text in, may be NULL only when length is 0. Read only during the call; caller owns it.
 * @param length in, byte count of text; must not exceed WaddleKeymapMaxBytes.
 * @param out out, non-NULL, writable for sizeof(waddle_keymap_names_t) bytes. Written only on
 * WaddleKeymapNamesOk and never read; unchanged on any other return. Caller owns it.
 * @return WaddleKeymapNamesOk, WaddleKeymapNamesNullArgument, WaddleKeymapNamesTooLarge, or
 * WaddleKeymapNamesMalformedName. Performs no heap allocation.
 * Thread safety: stateless and reentrant. Concurrent calls are safe when each call has its own out.
 */
int waddle_keymap_names_parse(const uint8_t *text, size_t length, waddle_keymap_names_t *out);

#endif /* WaddleKeymapNamesH */
