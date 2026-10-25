#ifndef WaddleHostPopupPositionerH
#define WaddleHostPopupPositionerH
/**
 * @file host_popup_positioner.h
 * @brief C ABI of the pure host popup positioner (`host_popup_positioner.zig`, TODO #11c).
 *
 * Three allocation-free functions. Each reads its inputs into locals and writes its output only on
 * success. None retains a pointer, touches the operating system, or holds state, so every function
 * may be called from any thread. The contract is
 * `impl/seamless-interop/claude_45_percent/host_popup_positioner_contract.md`.
 */
#include <stdint.h>

/** @brief Frozen positioner constants (IMPL_DESC section 8.44). */
enum host_popup_positioner_constants_t {
    HostPopupMaxScale = 8, /**< Largest integer host scale accepted by request and restore. */
    HostPopupMaxLogicalExtent = 32768, /**< Largest absolute logical offset or dimension a request may produce. */
    HostPopupAnchorTopLeft = 5, /**< xdg_positioner anchor top_left; the adapter sets it on every request. */
    HostPopupGravityBottomRight = 8, /**< xdg_positioner gravity bottom_right; the adapter sets it on every request. */
    HostPopupConstraintSlideFlip = 15 /**< constraint_adjustment slide_x | slide_y | flip_x | flip_y. */
};

/** @brief Result of every positioner function. */
typedef enum host_popup_status_t {
    HostPopupOk = 0, /**< Output written. */
    HostPopupInvalidArgument = 1, /**< A required pointer was NULL. Nothing was written. */
    HostPopupInvalidScale = 2, /**< Scale outside 1..8, zero denominator, or a fraction that rounds outside 1..8. Nothing was written. */
    HostPopupOutOfRange = 3, /**< A logical offset or dimension exceeds 32768 in absolute value. Nothing was written. */
    HostPopupOverflow = 4 /**< A scaled value does not fit its output type. Nothing was written. */
} host_popup_status_t;

/**
 * @brief Rectangle in the coordinate space named by the function that uses it.
 *
 * `request` input: guest physical pixels, `x`/`y` relative to the parent's top-left.
 * `request` output: logical anchor origin in `x`/`y` and logical positioner size in `width`/`height`.
 * `restore` input: compositor-adjusted logical rectangle. `restore` output: guest physical pixels.
 */
typedef struct host_popup_rect_t {
    int32_t x; /**< Signed horizontal origin. */
    int32_t y; /**< Signed vertical origin. */
    uint32_t width; /**< Unsigned width. */
    uint32_t height; /**< Unsigned height. */
} host_popup_rect_t;

/* Layout frozen with the Zig extern struct; a change in either breaks the build. */
_Static_assert(sizeof(host_popup_rect_t) == 16, "host_popup_rect_t layout");

/**
 * @brief Integer host scale for a fractional scale, rounded up.
 *
 * @param numerator In. Fraction numerator, for example 150 when the denominator is 120.
 * @param denominator In. Fraction denominator. Zero gives `HostPopupInvalidScale`.
 * @param out_scale Out. Receives `ceil(numerator / denominator)` in 1..8 on success only. NULL gives `HostPopupInvalidArgument`.
 * @return `HostPopupOk`, `HostPopupInvalidScale` or `HostPopupInvalidArgument`. Thread safety: any thread.
 */
host_popup_status_t host_popup_positioner_integer_scale(uint32_t numerator, uint32_t denominator, uint32_t *out_scale);

/**
 * @brief Logical popup request for a guest rectangle.
 *
 * Origins use floor division toward negative infinity. Each dimension uses ceiling division and is at
 * least 1. A result whose absolute offset or dimension exceeds 32768 is refused.
 *
 * @param guest In. Guest physical rectangle relative to the parent's top-left. NULL gives `HostPopupInvalidArgument`.
 * @param scale In. Integer host scale, 1..8. Other values give `HostPopupInvalidScale`.
 * @param out Out. Receives the logical request on success only. NULL gives `HostPopupInvalidArgument`.
 * @return `HostPopupOk`, `HostPopupInvalidArgument`, `HostPopupInvalidScale` or `HostPopupOutOfRange`.
 *         Thread safety: any thread.
 */
host_popup_status_t host_popup_positioner_request(const host_popup_rect_t *guest, uint32_t scale, host_popup_rect_t *out);

/**
 * @brief Guest rectangle for a compositor-adjusted logical rectangle.
 *
 * Every field is multiplied by `scale`. Each result is range-checked for its own type.
 *
 * @param logical In. Compositor-adjusted logical rectangle. NULL gives `HostPopupInvalidArgument`.
 * @param scale In. Integer host scale, 1..8. Other values give `HostPopupInvalidScale`.
 * @param out Out. Receives the guest rectangle on success only. NULL gives `HostPopupInvalidArgument`.
 * @return `HostPopupOk`, `HostPopupInvalidArgument`, `HostPopupInvalidScale` or `HostPopupOverflow`.
 *         Thread safety: any thread.
 */
host_popup_status_t host_popup_positioner_restore(const host_popup_rect_t *logical, uint32_t scale, host_popup_rect_t *out);

#endif
