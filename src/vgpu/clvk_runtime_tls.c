/**
 * @file clvk_runtime_tls.c
 * @brief Unload-neutral replacements for two libgcc Win32 runtime members.
 *
 * OpenCL.dll statically links GCC's Win32-thread-model runtime. Two libgcc
 * archive members allocate a process TLS index with TlsAlloc and never call
 * TlsFree: emutls.o (`emutls_key`) and gthr-win32-thread.o
 * (`__gthr_win32_tls`). Every LoadLibrary/FreeLibrary cycle therefore loses
 * two of the process's 1088 dynamic TLS indices.
 *
 * This translation unit defines the complete external symbol set of both
 * members (`__emutls_get_address`, `__emutls_register_common`,
 * `__gthr_win32_create`, `__gthr_win32_join`, `__gthr_win32_self`). Linked as
 * an object before libgcc.a, it prevents either member from being extracted.
 * Per-thread state lives in one record inside this module's implicit
 * (loader-managed) TLS template (`.tls$WDL`, between mingw-w64's `_tls_start`
 * and `_tls_end`). The Windows loader allocates that record for every thread,
 * initializes it from the template. Native heap/TLS/FLS controls must verify
 * actual retirement for threads alive across unload; it is not inferred here.
 *
 * Consequently this file owns no TLS index, kernel handle, heap block or lock
 * that survives a thread start: there is no DllMain, TLS-callback, atexit or
 * other loader-lock cleanup code, and no state is destroyed early.
 *
 * Ownership summary:
 * - Emulated TLS storage: bytes inside the calling thread's loader-owned
 *   record; valid until that thread exits or the module unloads.
 * - Thread start block: malloc-owned by __gthr_win32_create until the new
 *   thread copies and frees it before invoking the user routine.
 * - Thread handle: created by CreateThread, returned to libstdc++ (owned by
 *   std::thread, closed by __gthr_win32_join or __gthread_detach). The new
 *   thread's record stores a borrowed copy only for __gthr_win32_self.
 *
 * The behavior mirrors GCC 14.2 libgcc (Debian gcc-mingw-w64 14.2.0-19+27+b1)
 * except: emulated TLS never allocates from the heap, and
 * __gthr_win32_self() on a thread not created through __gthr_win32_create
 * returns an opaque tagged identity, unique among simultaneously live threads,
 * instead of a newly duplicated thread handle. It is for comparison only.
 */
#ifdef WaddleRuntimeTlsFakeWindows
#include "clvk_runtime_tls_fake/windows.h"
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
_Static_assert(sizeof(void *) == 8, "x64 TEB layout is required");
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/** Default arena bound, overridable only by a source-bound qualified build. */
#ifndef WaddleRuntimeTlsArenaBytes
#define WaddleRuntimeTlsArenaBytes 192
#endif
/** Default slot bound, overridable only by a source-bound qualified build. */
#ifndef WaddleRuntimeTlsSlotCapacity
#define WaddleRuntimeTlsSlotCapacity 32
#endif

/** Fixed capacities of the per-thread record; immutable, own no resources.
 * The build driver checks the linked DLL's emulated-TLS inventory against
 * these values; exceeding them at runtime terminates through __fastfail. */
typedef enum {
   /** Current full-DLL worst-case first-touch-order bound is 157 bytes. */
   RuntimeTlsArenaBytes = WaddleRuntimeTlsArenaBytes,
   /** Current full DLL defines 11 distinct controls (20 aliased symbols). */
   RuntimeTlsSlotCapacity = WaddleRuntimeTlsSlotCapacity,
   /** Largest alignment guaranteed by the loader's per-thread heap block. */
   RuntimeTlsMaxAlign = 16,
   /** Bits of the encoded control word that hold slot index + 1. */
   RuntimeTlsSlotBits = 8,
   /** x64 TEB offset of ThreadLocalStoragePointer (NT ABI, used by MSVC). */
   RuntimeTlsTebVectorOffset = 0x58,
   /** Low tag bit marking a resource-free foreign-thread identity. */
   RuntimeTlsForeignIdentityTag = 1,
} runtime_tls_constants_t;

/** @brief Source-bound admission metadata, not an exported runtime API.
 * Fields are schema version, arena bytes, slot capacity and maximum alignment,
 * each one little-endian uint32 in the x64 PE. The link driver retains this
 * read-only symbol and compares actual image bytes to its compile inputs.
 * @ownership Module-image storage; no allocation or caller-owned pointer.
 * @thread_safety Immutable throughout the module lifetime. */
__attribute__((used, section(".rdata$WaddleRuntimeTlsLimits")))
const uint32_t waddle_clvk_runtime_tls_limits[4] = {
   1, RuntimeTlsArenaBytes, RuntimeTlsSlotCapacity, RuntimeTlsMaxAlign
};

_Static_assert(RuntimeTlsSlotCapacity < (1 << RuntimeTlsSlotBits),
               "slot index + 1 must fit in the encoded control word");
_Static_assert(RuntimeTlsSlotCapacity > 0 && RuntimeTlsSlotCapacity <= 32,
               "each slot needs one bit in the per-thread initialization mask");
_Static_assert(RuntimeTlsArenaBytes > 0 &&
               RuntimeTlsArenaBytes <= (UINTPTR_MAX >> RuntimeTlsSlotBits),
               "encoded arena offsets must fit without overflow");
_Static_assert((RuntimeTlsArenaBytes % RuntimeTlsMaxAlign) == 0,
               "arena must preserve record alignment");

/**
 * @brief ABI mirror of libgcc's `struct __emutls_object` (GCC 14.2 emutls.c).
 * GCC emits one per `__thread`/`thread_local` variable as `__emutls_v.<name>`
 * in the module's writable data. Fields are borrowed module data.
 */
typedef struct emutls_object_t {
   /** Size in bytes of one thread's instance; may grow via register_common. */
   uintptr_t size;
   /** Required power-of-two alignment in bytes. */
   uintptr_t align;
   /** Zero until assigned; then (byte offset << SlotBits) | (slot + 1).
    * Written once under runtime_tls_layout_lock with release ordering. */
   uintptr_t location;
   /** Borrowed initializer template of `size` bytes, or NULL for zero fill. */
   const void *templ;
} emutls_object_t;

_Static_assert(sizeof(emutls_object_t) == 4 * sizeof(void *),
               "emutls control object ABI mismatch");

/**
 * @brief Per-thread record copied by the loader from this module's template.
 * Lives in loader-owned memory; never freed or reset by this module.
 * @thread_safety Only the owning thread reads or writes its own record.
 */
typedef struct runtime_tls_record_t {
   /** Emulated TLS storage; slots are laid out identically in all threads. */
   _Alignas(16) unsigned char storage[RuntimeTlsArenaBytes];
   /** Nonzero once this thread initialized the slot from its template. */
   uint32_t initialized;
   /** Borrowed creation handle for threads started by __gthr_win32_create;
    * NULL for every other thread. Never closed through this field. */
   HANDLE gthread_handle;
} runtime_tls_record_t;

/**
 * @brief Start block passed from __gthr_win32_create to the new thread.
 * Heap-owned by the creator until gthread_entry copies and frees it.
 */
typedef struct gthread_start_t {
   /** User routine supplied by libstdc++; never NULL. */
   void *(*routine)(void *);
   /** Opaque routine argument owned by libstdc++. */
   void *argument;
   /** Creation handle written before ResumeThread; borrowed by the thread. */
   HANDLE handle;
} gthread_start_t;

/** @brief Fail immediately on an unsupported ABI or corrupt internal layout.
 * @return Never returns; no destructor or loader-lock callback is executed.
 * @thread_safety Reentrant; owns no resources. */
static void __attribute__((noreturn)) runtime_tls_fail(void);

/** @brief Validate one loader-record range before any record dereference.
 * @param[in] block Borrowed template-sized loader block, nonnull on success.
 * @param[in] offset Byte offset of the module record within its template.
 * @param[in] extent Complete initialized template extent in bytes.
 * @return Borrowed aligned record, valid only for this thread/module lifetime.
 * @note Fails fast on null, misalignment, overflow or incomplete range.
 * @thread_safety Reentrant; no allocation, mutation or retained input. */
static runtime_tls_record_t *
runtime_tls_checked_record(unsigned char *block, uintptr_t offset,
                           uintptr_t extent)
{
   uintptr_t address = (uintptr_t)block;
   if (!block || (address % RuntimeTlsMaxAlign) != 0 ||
       (offset % RuntimeTlsMaxAlign) != 0 || offset > extent ||
       sizeof(runtime_tls_record_t) > extent - offset ||
       offset > UINTPTR_MAX - address)
      runtime_tls_fail();
   return (runtime_tls_record_t *)(address + offset);
}

#ifdef WaddleRuntimeTlsFakeWindows
#define RuntimeTlsSymbol(name) waddle_test_##name
/** Linux test double for the loader-owned per-thread record. */
static __thread runtime_tls_record_t runtime_tls_fake_record;

static runtime_tls_record_t *
runtime_tls_current_record(void)
{
   return runtime_tls_checked_record((unsigned char *)&runtime_tls_fake_record,
                                      0, sizeof(runtime_tls_fake_record));
}
#else
#define RuntimeTlsSymbol(name) name
/** Implicit TLS index written by the loader (mingw-w64 crt/tlssup.c). */
extern ULONG _tls_index;
/** First template byte, StartAddressOfRawData of `_tls_used` (tlssup.c). */
extern char _tls_start;
/** End of initialized module TLS template (mingw-w64 crt/tlssup.c). */
extern char _tls_end;

/** Template image of runtime_tls_record_t. Never accessed directly: the
 * loader copies it into each thread's block. `.tls$WDL` sorts between
 * `.tls` (`_tls_start`) and `.tls$ZZZ` (`_tls_end`) in GNU ld's PE script. */
__attribute__((section(".tls$WDL"), used)) static runtime_tls_record_t
   runtime_tls_template;

/**
 * @brief Return the calling thread's loader-owned record.
 * @return Non-null pointer valid until the thread exits or the module unloads.
 * @ownership Borrowed loader memory; never freed by this module.
 * @thread_safety Reentrant; returns a different record for every thread.
 */
static runtime_tls_record_t *
runtime_tls_current_record(void)
{
   void **vector;
   unsigned char *block;
   uintptr_t start = (uintptr_t)&_tls_start;
   uintptr_t end = (uintptr_t)&_tls_end;
   uintptr_t record = (uintptr_t)&runtime_tls_template;

   /* Same instruction MSVC emits for __declspec(thread); written as asm
    * because GCC 14 misreports mingw-w64's __readgsqword as a NULL access. */
   __asm__("movq %%gs:%c1, %0" : "=r"(vector) : "i"(RuntimeTlsTebVectorOffset));
   if (!vector || record < start || end < record)
      runtime_tls_fail();
   block = (unsigned char *)vector[_tls_index];
   return runtime_tls_checked_record(block, record - start, end - start);
}
#endif

/** Global slot layout lock; SRWLOCK_INIT needs no creation or destruction. */
static SRWLOCK runtime_tls_layout_lock = SRWLOCK_INIT;
/** Bytes of the arena assigned so far; guarded by runtime_tls_layout_lock. */
static size_t runtime_tls_used_bytes;
/** Number of slots assigned so far; guarded by runtime_tls_layout_lock. */
static size_t runtime_tls_used_slots;
/** Byte size reserved for each slot; written before publication. */
static size_t runtime_tls_slot_bytes[RuntimeTlsSlotCapacity];
/** Alignment reserved per slot; immutable before location publication. */
static size_t runtime_tls_slot_align[RuntimeTlsSlotCapacity];
/** Exact control identity and offset reserved for each published location. */
static emutls_object_t *runtime_tls_slot_objects[RuntimeTlsSlotCapacity];
static size_t runtime_tls_slot_offsets[RuntimeTlsSlotCapacity];

/**
 * @brief Terminate on an impossible layout without running any unload code.
 * @note __fastfail ends the process immediately (no DllMain, no atexit),
 *       so a broken invariant cannot turn into a hang under the loader lock.
 */
static void __attribute__((noreturn))
runtime_tls_fail(void)
{
   __fastfail(FAST_FAIL_FATAL_APP_EXIT);
}

/**
 * @brief Assign a process-wide arena slot to a control object exactly once.
 * @param[in,out] object Non-null control object; `location` is published.
 * @return The nonzero encoded location word.
 * @ownership No allocation; the slot persists until the module unloads.
 * @thread_safety Serialized by runtime_tls_layout_lock; idempotent.
 */
static uintptr_t
runtime_tls_assign(emutls_object_t *object)
{
   uintptr_t encoded;

   AcquireSRWLockExclusive(&runtime_tls_layout_lock);
   encoded = __atomic_load_n(&object->location, __ATOMIC_RELAXED);
   if (encoded == 0) {
      uintptr_t align = object->align ? object->align : 1;
      size_t offset;

      if (align > RuntimeTlsMaxAlign || (align & (align - 1)) != 0 ||
          object->size > RuntimeTlsArenaBytes ||
          runtime_tls_used_slots >= RuntimeTlsSlotCapacity) {
         ReleaseSRWLockExclusive(&runtime_tls_layout_lock);
         runtime_tls_fail();
      }
      offset = (runtime_tls_used_bytes + align - 1) & ~(size_t)(align - 1);
      if (offset > RuntimeTlsArenaBytes - object->size) {
         ReleaseSRWLockExclusive(&runtime_tls_layout_lock);
         runtime_tls_fail();
      }
      runtime_tls_slot_bytes[runtime_tls_used_slots] = object->size;
      runtime_tls_slot_align[runtime_tls_used_slots] = align;
      runtime_tls_slot_objects[runtime_tls_used_slots] = object;
      runtime_tls_slot_offsets[runtime_tls_used_slots] = offset;
      encoded = ((uintptr_t)offset << RuntimeTlsSlotBits) |
                (uintptr_t)(runtime_tls_used_slots + 1);
      runtime_tls_used_slots++;
      runtime_tls_used_bytes = offset + object->size;
      __atomic_store_n(&object->location, encoded, __ATOMIC_RELEASE);
   }
   ReleaseSRWLockExclusive(&runtime_tls_layout_lock);
   return encoded;
}

/**
 * @brief libgcc ABI: return the calling thread's instance of a TLS variable.
 * @param[in,out] object Non-null compiler-emitted control object.
 * @return Non-null address aligned to object->align, initialized from
 *         object->templ (or zero) on this thread's first access.
 * @ownership Borrowed loader-owned storage; caller must not free it. Valid
 *            until the calling thread exits or this module unloads.
 * @thread_safety Thread-safe; each thread receives distinct storage.
 * @note Fails fast on null control, malformed published location, alignment or
 * size growth, unsupported alignment, exhausted slots or exhausted arena.
 */
void *
RuntimeTlsSymbol(__emutls_get_address)(emutls_object_t *object)
{
   runtime_tls_record_t *record = runtime_tls_current_record();
   uintptr_t encoded;
   size_t slot;
   uintptr_t tag;
   uintptr_t offset;
   uintptr_t align;
   unsigned char *address;

   if (!object)
      runtime_tls_fail();
   encoded = __atomic_load_n(&object->location, __ATOMIC_ACQUIRE);
   if (encoded == 0)
      encoded = runtime_tls_assign(object);
   tag = encoded & ((1u << RuntimeTlsSlotBits) - 1);
   offset = encoded >> RuntimeTlsSlotBits;
   if (tag == 0 || tag > RuntimeTlsSlotCapacity)
      runtime_tls_fail();
   slot = (size_t)tag - 1;
   align = object->align ? object->align : 1;
   if (align > RuntimeTlsMaxAlign || (align & (align - 1)) != 0 ||
       offset > RuntimeTlsArenaBytes || object->size > RuntimeTlsArenaBytes - offset ||
       (offset % align) != 0 || object->size > runtime_tls_slot_bytes[slot] ||
       align > runtime_tls_slot_align[slot] ||
       runtime_tls_slot_objects[slot] != object ||
       runtime_tls_slot_offsets[slot] != offset)
      runtime_tls_fail();
   address = record->storage + offset;
   if ((record->initialized & (UINT32_C(1) << slot)) == 0) {
      if (object->templ)
         memcpy(address, object->templ, object->size);
      record->initialized |= UINT32_C(1) << slot;
   }
   return address;
}

/**
 * @brief libgcc ABI: merge a common-symbol TLS definition (emutls.c rules).
 * @param[in,out] object Non-null control object to update.
 * @param[in] size Candidate instance size in bytes.
 * @param[in] align Candidate alignment in bytes.
 * @param[in] templ Optional borrowed template; may be NULL.
 * @ownership No allocation; templ remains owned by the module image.
 * @thread_safety Called from module constructors before concurrent use.
 * @note Nonnull object is required. No synchronization is provided for merging
 * metadata concurrently with access; growth after assignment fails on access.
 */
void
RuntimeTlsSymbol(__emutls_register_common)(emutls_object_t *object,
                                           uintptr_t size, uintptr_t align,
                                           const void *templ)
{
   if (!object)
      runtime_tls_fail();
   if (object->size < size) {
      object->size = size;
      object->templ = NULL;
   }
   if (object->align < align)
      object->align = align;
   if (templ && size == object->size)
      object->templ = templ;
}

/**
 * @brief Thread entry: adopt the start block, record the handle, run.
 * @param[in] parameter Heap gthread_start_t from __gthr_win32_create.
 * @return Low 32 bits of the routine result, as libgcc returns them.
 * @ownership Frees @p parameter before calling user code.
 * @thread_safety One owning child; handle publication precedes ResumeThread.
 */
static DWORD WINAPI
gthread_entry(LPVOID parameter)
{
   gthread_start_t start;

   memcpy(&start, parameter, sizeof(start));
   free(parameter);
   runtime_tls_current_record()->gthread_handle = start.handle;
   return (DWORD)(uintptr_t)start.routine(start.argument);
}

/**
 * @brief libgcc ABI: start a thread for std::thread.
 * @param[out] thread Non-null; receives the owned creation handle on success.
 * @param[in] routine Non-null routine run on the new thread.
 * @param[in] argument Opaque argument passed to @p routine.
 * @return 0 on success; a Win32 error code (or ERROR_NOT_ENOUGH_MEMORY) on
 *         failure, in which case nothing remains allocated or running.
 * @ownership The caller (std::thread) owns *thread and must join or detach.
 * @thread_safety Thread-safe.
 */
int
RuntimeTlsSymbol(__gthr_win32_create)(HANDLE *thread, void *(*routine)(void *),
                                      void *argument)
{
   gthread_start_t *start = malloc(sizeof(*start));
   HANDLE handle;
   int error;

   if (!start)
      return ERROR_NOT_ENOUGH_MEMORY;
   start->routine = routine;
   start->argument = argument;
   start->handle = NULL;
   handle = CreateThread(NULL, 0, gthread_entry, start, CREATE_SUSPENDED, NULL);
   if (!handle) {
      error = (int)GetLastError();
      free(start);
      return error;
   }
   start->handle = handle;
   if (ResumeThread(handle) == (DWORD)-1) {
      /* The suspended thread never ran user or loader-notification code. */
      error = (int)GetLastError();
      if (!TerminateThread(handle, 0) ||
          WaitForSingleObject(handle, INFINITE) != WAIT_OBJECT_0 ||
          !CloseHandle(handle))
         runtime_tls_fail();
      free(start);
      return error;
   }
   *thread = handle;
   return 0;
}

/**
 * @brief libgcc ABI: wait for a std::thread and close its handle.
 * @param[in] thread Owned creation handle; consumed unless 1 is returned.
 * @param[out] value Optional; receives the routine's 32-bit result.
 * @return 0 on success, 1 when joining the calling thread itself (handle kept,
 *         matching libgcc), otherwise a Win32 error (handle closed).
 * @thread_safety Call once per handle.
 * @pre thread is a nonnull creation handle exclusively owned by this caller.
 */
int
RuntimeTlsSymbol(__gthr_win32_join)(HANDLE thread, void **value)
{
   DWORD exit_code;
   int status = 0;

   if (GetThreadId(thread) == GetCurrentThreadId())
      return 1;
   if (WaitForSingleObject(thread, INFINITE) != WAIT_OBJECT_0) {
      status = (int)GetLastError();
   } else if (value) {
      if (GetExitCodeThread(thread, &exit_code))
         *value = (void *)(uintptr_t)exit_code;
      else
         status = (int)GetLastError();
   }
   if (!CloseHandle(thread))
      runtime_tls_fail();
   return status;
}

/**
 * @brief libgcc ABI: identity for std::this_thread::get_id().
 * @return For threads from __gthr_win32_create: their creation handle
 *         (equal to the owning std::thread's id). For any other thread: the
 *         address of its loader-owned record with the low bit set, which is
 *         unique among live threads, stable for the thread's life and owns no
 *         kernel resource. The opaque value may be reused after thread exit.
 * @ownership Borrowed; callers must never close or wait on the result.
 * @thread_safety Reentrant.
 */
HANDLE
RuntimeTlsSymbol(__gthr_win32_self)(void)
{
   runtime_tls_record_t *record = runtime_tls_current_record();

   if (record->gthread_handle)
      return record->gthread_handle;
   return (HANDLE)((uintptr_t)record | RuntimeTlsForeignIdentityTag);
}
