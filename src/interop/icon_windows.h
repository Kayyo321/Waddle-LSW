/** @file icon_windows.h @brief Read-only Windows Shell icon extraction into RGBA8. */
#ifndef IconWindowsH
#define IconWindowsH

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shobjidl.h>
#else
typedef struct IShellItem IShellItem;
#endif

/** @brief Largest supported square icon dimension in pixels. */
#define IconWindowsMaxDimension 256u
/** @brief Largest Shell-returned source bitmap dimension accepted before scaling. */
#define IconWindowsMaxNativeSourceDimension 512u
/** @brief Number of dimensions in the frozen icon size set. */
#define IconWindowsSupportedDimensionCount 7u
/** @brief Maximum fallback path length in UTF-16 code units, excluding NUL. */
#define IconWindowsMaxFallbackPathUnits 32767u

/** @brief Result of one native icon extraction or cleanup operation. */
typedef enum icon_windows_status_t {
    IconWindowsStatusOk = 0, /**< Image extraction or cleanup completed. */
    IconWindowsStatusInvalidArgument, /**< A pointer, range, allocator, or output record is invalid. */
    IconWindowsStatusUnsupportedDimension, /**< Requested size is outside the frozen set. */
    IconWindowsStatusOutOfMemory, /**< Caller allocator returned NULL. */
    IconWindowsStatusNativeFailure, /**< Shell or GDI extraction failed. */
    IconWindowsStatusUnsupportedPlatform, /**< Native extraction is unavailable on this platform. */
    IconWindowsStatusCleanupRequired /**< Exact GDI handles remain in caller cleanup record. */
} icon_windows_status_t;

/** @brief Caller allocator used for the returned RGBA buffer. */
typedef struct icon_windows_allocator_t {
    void *context; /**< Borrowed context retained until image deinitialization. */
    /** @brief Allocate one writable pixel buffer.
     * @param[in] context Borrowed allocator context.
     * @param[in] size Exact requested byte count.
     * @return Writable storage or NULL; ownership transfers to the image only on success.
     */
    void *(*allocate)(void *context, size_t size);
    /** @brief Release one exact buffer previously returned by allocate.
     * @param[in] context Borrowed allocator context.
     * @param[in] memory Exact owned allocation.
     * @param[in] size Exact original allocation size.
     */
    void (*deallocate)(void *context, void *memory, size_t size);
} icon_windows_allocator_t;

/** @brief Owned top-down straight-alpha RGBA8 icon pixels. */
typedef struct icon_windows_image_t {
    uint8_t *rgba; /**< Owned pixel bytes in R,G,B,A order. */
    size_t stride; /**< Bytes per row; exactly width times four. */
    uint32_t width; /**< Requested square icon size. */
    uint32_t height; /**< Requested square icon size. */
    size_t allocation_size; /**< Exact allocator size retained for deinitialization. */
    icon_windows_allocator_t allocator; /**< Allocator captured at successful extraction. */
} icon_windows_image_t;

/** @brief Caller-owned record retaining exact GDI objects after failed cleanup. */
typedef struct icon_windows_cleanup_t {
    uintptr_t bitmap_handles[4]; /**< Exact bitmap handles awaiting DeleteObject. */
    uintptr_t icon_handles[1]; /**< Exact icon handles awaiting DestroyIcon. */
    uintptr_t device_context_handles[1]; /**< Exact memory DC handles awaiting DeleteDC. */
    uint32_t bitmap_count; /**< Number of populated bitmap handles. */
    uint32_t icon_count; /**< Number of populated icon handles. */
    uint32_t device_context_count; /**< Number of populated DC handles. */
    uint32_t operation_status; /**< Original operation status before cleanup was retained. */
    uint32_t reserved_zero; /**< Must remain zero. */
    uint32_t operation_error; /**< Copied error for the operation that preceded cleanup. */
    uint32_t last_error; /**< Copied Win32 error for the last failed native operation. */
} icon_windows_cleanup_t;

/**
 * @brief Extract a Shell icon as caller-owned top-down straight-alpha RGBA8.
 * @param[in] item Borrowed valid `IShellItem`; caller owns its COM reference and
 *        must provide a compatible initialized COM apartment. The first
 *        interface-pointer-sized range is checked for numeric overflow and
 *        overlap; the complete COM object must remain valid and readable.
 * @param[in] fallback_path Optional borrowed NUL-terminated path, at most
 *        IconWindowsMaxFallbackPathUnits UTF-16 code units, readable through
 *        its terminator; used only for the frozen 16/32/48-pixel system
 *        image-list fallback after Shell failure. The bounded pointer range
 *        range arithmetic must not overflow, and its actual NUL-terminated
 *        extent must not overlap checked inputs/outputs.
 * @param[in] dimension Requested square size from the frozen set
 *        16/24/32/48/64/128/256.
 * @param[in] allocator Complete borrowed allocator pair; it owns successful
 *        `image_out->rgba` until deinitialization. Caller keeps the context
 *        valid and disjoint from the records and input for the full call and
 *        until deinitialization; its extent is opaque and cannot be checked.
 * @param[out] image_out Naturally aligned zero-valued output record disjoint
 *        from the item, fallback path, and allocator context; unchanged on
 *        ordinary failure.
 * @param[out] cleanup_out Required naturally aligned zero-valued cleanup
 *        record disjoint from all inputs and `image_out`; populated only when
 *        native cleanup fails, retaining the exact unresolved handles.
 * @return Ok with one owned image, an explicit operational error with outputs
 *         unchanged, or CleanupRequired with handles transferred to cleanup_out.
 * @note Synchronous and serialized by the caller. Does not launch or mutate an
 *       application, initialize COM, or retain the Shell item. Every acquired
 *       bitmap/icon/DC is closed before success is published.
 */
icon_windows_status_t icon_windows_extract_rgba(IShellItem *item,
                                                const wchar_t *fallback_path,
                                                uint32_t dimension,
                                                icon_windows_allocator_t allocator,
                                                icon_windows_image_t *image_out,
                                                icon_windows_cleanup_t *cleanup_out);

/**
 * @brief Retry deletion of the exact GDI handles retained after cleanup failure.
 * @param[in,out] cleanup Caller-owned authentic record; may be empty.
 * @return Ok and zeroes the record when all GDI handles close, otherwise
 *         CleanupRequired with each unresolved handle identity preserved.
 * @note Caller serializes retries; the function never guesses or recreates
 *       handles and never calls the Shell or releases another component's COM object.
 */
icon_windows_status_t icon_windows_cleanup_retry(icon_windows_cleanup_t *cleanup);

/**
 * @brief Release one extracted RGBA image and zero its record.
 * @param[in,out] image Caller-owned record from a successful Windows extraction;
 *        an empty zero record is accepted.
 * @return No error; allocation is freed exactly once through its captured pair.
 *         An incomplete nonempty record is preserved because its ownership
 *         cannot be safely discharged.
 * @note Caller serializes access and ensures no reader retains `rgba`.
 */
void icon_windows_image_deinit(icon_windows_image_t *image);

#endif
