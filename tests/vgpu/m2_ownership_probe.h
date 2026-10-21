/** @file m2_ownership_probe.h @brief Qualification-only real Win32 DC observer.
 * @note The ICD lock serializes ordinary calls; standalone controls use one UI
 * thread. No retained caller pointer, allocation, thread or native resource beyond
 * the exact GetDC acquisition. Never linked into an ordinary production DLL.
 */
#ifndef WaddleM2OwnershipProbeH
/** @brief Include guard; no storage. */
#define WaddleM2OwnershipProbeH
#include "m2_owner_ledger.h"
#ifdef WaddleM2OwnershipProbeExport
/** @brief Only the read-only query is exported by the qualified Windows DLL. */
#define M2ProbeExport __declspec(dllexport)
#else
/** @brief Standalone control uses normal C linkage. */
#define M2ProbeExport
#endif
/** @brief Reserve an identity and call actual GetDC once.
 * @param[in] hwnd Borrowed live HWND bits, never destroyed by this observer.
 * @return Acquired HDC bits, or zero with no unrecorded successful acquisition.
 * @note Serialized; caller must pass each nonzero result to matching release.
 */
uintptr_t venus_m2_dc_acquire(uintptr_t hwnd);
/** @brief Release only the exact observed owned DC, retaining native failures.
 * @param[in] hwnd Borrowed original HWND. @param[in] dc Owned acquired HDC bits.
 * @return One actual retirement, zero unknown identity/native release failure.
 * @note Serialized; no call to ReleaseDC is made for an unproven identity.
 */
int venus_m2_dc_release(uintptr_t hwnd, uintptr_t dc);
/** @brief Read the fixed real-DC owner snapshot; never resets or retires state.
 * @param[in] version Exact M2OwnerVersion. @param[out] output Nonnull distinct
 * snapshot, unchanged on bad ABI. @param[in] bytes Exact snapshot size.
 * @return One snapshot copied, zero invalid pointer/version/size.
 * @note Caller serializes with presentation; returned values transfer no ownership.
 */
M2ProbeExport int venus_m2_dc_snapshot(uint32_t version, m2_owner_snapshot_t *output, size_t bytes);
#ifdef WaddleM2OwnershipFaults
/** @brief Standalone-only next-release controls; absent from qualified DLL. */
typedef enum m2_dc_fault_t {
    M2DcNormal = 0, /**< Call the actual ReleaseDC and record its result. */
    M2DcOmitRelease = 1, /**< Retain the acquisition but return sink success. */
    M2DcFailRelease = 2 /**< Retain the acquisition and inject failed release. */
} m2_dc_fault_t;
/** @brief Set one test release fault, rejecting invalid/overlapping controls.
 * @param[in] fault Exact enum. @return One installed, zero invalid or pending.
 * @note Sole control thread; consumes once, owns no native resource.
 */
int venus_m2_dc_test_fault(uint32_t fault);
/** @brief Retry actual release ONLY for exact acquired control-owned DC entries.
 * @return One if all acquired entries retire, zero any failure/pending reservation.
 * @note Sole control thread; failure entries remain, sticky errors are preserved.
 */
int venus_m2_dc_test_cleanup(void);
/** @brief Reset standalone test state only after all owner entries retire.
 * @return One empty reset, zero any retained/reserved entry; no native operation.
 * @note Sole control thread; never present in qualified DLL.
 */
int venus_m2_dc_test_reset(void);
#endif
#endif
