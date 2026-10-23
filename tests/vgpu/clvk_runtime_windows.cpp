/**
 * @file clvk_runtime_windows.cpp
 * @brief Isolated MinGW runtime lifetime probe loaded by a native C harness.
 *
 * All C++ objects, thread IDs and allocations belong to this DLL. The exports
 * use scalar C ABI values only. No nontrivial thread_local object is defined,
 * so the probe does not add __cxa_thread_atexit state to the runtime under test.
 */
#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <exception>
#include <iostream>
#include <mutex>
#include <shared_mutex>
#include <thread>

namespace {

enum : std::uint32_t {
   ApiVersion = 2,
   ForeignThreadSlotCount = 4,
   ForeignWaitTimeoutMs = 5000,
   StatusOk = 0,
   StatusVersion = 1,
   StatusOnce = 2,
   StatusRegistration = 3,
   StatusJoinedThread = 4,
   StatusThreadLocal = 5,
   StatusThreadIdentity = 6,
   StatusException = 7,
   StatusDetachedThread = 8,
   StatusForeignArguments = 20,
   StatusForeignThreadLocal = 21,
   StatusForeignIdentity = 22,
   StatusForeignReady = 23,
   StatusForeignRelease = 24,
   StatusForeignException = 25,
};

constexpr std::uint32_t ThreadLocalInitialValue = 7;
constexpr std::uint32_t ThreadLocalUpdatedValue = 8;
constexpr std::uint32_t WorkerThreadLocalValue = 99;
constexpr std::uint32_t DetachedThreadLocalValue = 23;
constexpr std::uint32_t OnceWorkerCount = 4;

struct once_worker_context_t {
   std::atomic<std::uintptr_t> thread_handle{ 0 };
   std::atomic<bool> handle_published{ false };
};

std::shared_mutex pass_registry_mutex;
std::mutex foreign_registry_mutex;
std::once_flag registry_once;
std::uint32_t once_runs = 0;
std::uint32_t registration_count = 0;
std::uint32_t joined_thread_runs = 0;
std::atomic<std::uint32_t> detached_thread_runs{ 0 };
std::uint32_t foreign_thread_seen_count = 0;
std::atomic<std::uint32_t> once_error{ 0 };
std::atomic<std::uint32_t> detached_thread_status{ StatusDetachedThread };
std::atomic<std::uintptr_t> detached_thread_handle{ 0 };
std::atomic<bool> detached_handle_published{ false };
std::array<std::thread::id, ForeignThreadSlotCount> foreign_thread_ids{};
std::array<bool, ForeignThreadSlotCount> foreign_thread_seen{};
std::ios_base::Init stream_initializer;
thread_local std::uint32_t thread_counter = ThreadLocalInitialValue;

/**
 * @brief Write a bounded diagnostic and terminate when DLL code may remain live.
 * @param[in] message Static null-terminated diagnostic owned by the image.
 * @param[in] exit_code Nonzero process exit status.
 * @return Never returns.
 * @ownership Uses only the borrowed standard error handle and current-process
 *            pseudo-handle; the OS owns process teardown.
 * @thread_safety Safe for a fatal path; no C++ synchronization is used.
 */
[[noreturn]] void fail_closed_process(const char *message,
                                     std::uint32_t exit_code) noexcept
{
   DWORD written = 0;
   HANDLE error_handle = GetStdHandle(STD_ERROR_HANDLE);
   if (error_handle && error_handle != INVALID_HANDLE_VALUE && message) {
      std::uint32_t length = 0;
      while (message[length] != '\0')
         length++;
      (void)WriteFile(error_handle, message, length, &written, nullptr);
      (void)FlushFileBuffers(error_handle);
   }
   (void)TerminateProcess(GetCurrentProcess(), exit_code);
   for (;;)
      Sleep(1000);
}

/**
 * @brief Invoke the one-time registration callback from a concurrent caller.
 * @return StatusOk after std::call_once completes.
 * @ownership Reads/writes only DLL-owned registry_once and once_runs.
 * @thread_safety Safe for concurrent callers; std::call_once serializes writes.
 */
std::uint32_t run_once_caller() noexcept
{
   try {
      std::call_once(registry_once, []() { once_runs++; });
      return StatusOk;
   } catch (...) {
      return StatusOnce;
   }
}

/**
 * @brief Wait for a duplicated real thread handle and close the DLL-owned copy.
 * @param[in] thread_handle Valid duplicated handle owned by this call.
 * @return TRUE when the thread terminated within the fixed bound and the
 *         duplicated handle was closed.
 * @ownership Closes only thread_handle, duplicated from GetCurrentThread().
 * @thread_safety No shared mutable state.
 */
BOOL wait_and_close_thread(HANDLE thread_handle) noexcept
{
   const DWORD wait_status = WaitForSingleObject(thread_handle,
                                                  ForeignWaitTimeoutMs);
   if (wait_status != WAIT_OBJECT_0)
      return FALSE;
   return CloseHandle(thread_handle);
}

} // namespace

namespace llvm {
/**
 * @brief Probe stand-in for LLVM's managed-static teardown (no LLVM is linked here).
 * @return No value; the shared EH-pool callback calls it once after every static
 *         destructor of this probe image, exactly as OpenCL.dll calls the real one.
 * @ownership Owns nothing; the probe has no LLVM managed statics.
 * @thread_safety Loader/CRT serialized final callback only.
 */
void llvm_shutdown() noexcept {}
} // namespace llvm

/**
 * @brief Exercise C++ runtime state and thread retirement for one DLL load.
 * @param[in] api_version Must equal ApiVersion; scalar and non-null by value.
 * @return StatusOk or a documented Status* failure code; never throws across C ABI.
 * @ownership The DLL owns every C++ object and duplicated native handle. No
 *            allocation, C++ object, or owned handle crosses the ABI.
 * @thread_safety Call once from the harness thread per DLL load; its internal
 *                concurrent first use is tested.
 */
extern "C" __declspec(dllexport) std::uint32_t
runtime_lifetime_probe(std::uint32_t api_version) noexcept
{
   if (api_version != ApiVersion)
      return StatusVersion;

   try {
      HANDLE once_ready_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      HANDLE once_start_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      if (!once_ready_event || !once_start_event) {
         if (once_ready_event)
            (void)CloseHandle(once_ready_event);
         if (once_start_event)
            (void)CloseHandle(once_start_event);
         return StatusOnce;
      }

      std::array<std::thread, OnceWorkerCount> once_workers;
      std::array<once_worker_context_t, OnceWorkerCount> once_contexts;
      std::atomic<std::uint32_t> ready_count{ 0 };
      std::uint32_t started_workers = 0;
      BOOL setup_ok = TRUE;
      try {
         for (; started_workers < OnceWorkerCount; started_workers++) {
            const std::uint32_t slot = started_workers;
            once_workers[started_workers] = std::thread(
               [&ready_count, &once_contexts, slot, once_ready_event,
                once_start_event]() {
                  if (ready_count.fetch_add(1, std::memory_order_acq_rel) + 1 ==
                      OnceWorkerCount && !SetEvent(once_ready_event)) {
                     once_error.fetch_or(StatusOnce,
                                         std::memory_order_relaxed);
                  }
                  const DWORD start_status = WaitForSingleObject(
                     once_start_event, ForeignWaitTimeoutMs);
                  if (start_status != WAIT_OBJECT_0) {
                     once_error.fetch_or(StatusOnce,
                                         std::memory_order_relaxed);
                  } else {
                     const std::uint32_t status = run_once_caller();
                     if (status != StatusOk)
                        once_error.fetch_or(status,
                                            std::memory_order_relaxed);
                  }
                  HANDLE duplicated_self = NULL;
                  if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                                       GetCurrentProcess(), &duplicated_self,
                                       SYNCHRONIZE, FALSE, 0)) {
                     once_error.fetch_or(StatusOnce,
                                         std::memory_order_relaxed);
                  } else {
                     once_contexts[slot].thread_handle.store(
                        reinterpret_cast<std::uintptr_t>(duplicated_self),
                        std::memory_order_relaxed);
                  }
                  once_contexts[slot].handle_published.store(
                     true, std::memory_order_release);
               });
         }
      } catch (...) {
         setup_ok = FALSE;
      }

      if (setup_ok &&
          WaitForSingleObject(once_ready_event, ForeignWaitTimeoutMs) !=
             WAIT_OBJECT_0)
         setup_ok = FALSE;
      if (!SetEvent(once_start_event))
         setup_ok = FALSE;
      for (std::uint32_t index = 0; index < started_workers; index++) {
         bool handle_published = false;
         for (std::uint32_t poll = 0; poll < ForeignWaitTimeoutMs; poll++) {
            if (once_contexts[index].handle_published.load(
                   std::memory_order_acquire)) {
               handle_published = true;
               break;
            }
            Sleep(1);
         }
         if (!handle_published)
            fail_closed_process("RUNTIME_FATAL stage=once_handle_publish\n",
                                0xe7);
         const std::uintptr_t handle_value =
            once_contexts[index].thread_handle.load(std::memory_order_relaxed);
         if (handle_value == 0)
            fail_closed_process("RUNTIME_FATAL stage=once_handle_missing\n",
                                0xe8);
         const HANDLE worker_handle = reinterpret_cast<HANDLE>(handle_value);
         const DWORD worker_status = WaitForSingleObject(
            worker_handle, ForeignWaitTimeoutMs);
         if (worker_status != WAIT_OBJECT_0)
            fail_closed_process("RUNTIME_FATAL stage=once_worker_timeout\n",
                                0xe9);
         if (!CloseHandle(worker_handle))
            fail_closed_process("RUNTIME_FATAL stage=once_handle_close\n",
                                0xea);
         try {
            once_workers[index].join();
         } catch (...) {
            fail_closed_process("RUNTIME_FATAL stage=once_thread_join\n",
                                0xeb);
         }
      }
      const BOOL ready_closed = CloseHandle(once_ready_event);
      const BOOL start_closed = CloseHandle(once_start_event);
      if (!setup_ok || !ready_closed || !start_closed || once_runs != 1 ||
          once_error.load(std::memory_order_relaxed) != 0)
         return StatusOnce;

      {
         std::unique_lock<std::shared_mutex> write_lock(pass_registry_mutex);
         registration_count++;
      }

      if (thread_counter != ThreadLocalInitialValue)
         return StatusThreadLocal;
      thread_counter++;
      std::uint32_t worker_initial_counter = 0;
      BOOL worker_exception_ok = FALSE;
      std::thread::id worker_seen_id;
      const std::thread::id caller_id = std::this_thread::get_id();
      std::atomic<std::uintptr_t> joined_thread_handle{ 0 };
      std::atomic<bool> joined_handle_published{ false };
      std::thread joined_worker([&]() {
         std::unique_lock<std::shared_mutex> write_lock(pass_registry_mutex);
         joined_thread_runs++;
         worker_initial_counter = thread_counter;
         thread_counter = WorkerThreadLocalValue;
         worker_seen_id = std::this_thread::get_id();
         try {
            throw 29;
         } catch (int value) {
            worker_exception_ok = value == 29 &&
                                  std::uncaught_exceptions() == 0;
         } catch (...) {
            worker_exception_ok = FALSE;
         }
         HANDLE duplicated_self = NULL;
         if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                              GetCurrentProcess(), &duplicated_self,
                              SYNCHRONIZE, FALSE, 0)) {
            joined_handle_published.store(true, std::memory_order_release);
            return;
         }
         joined_thread_handle.store(
            reinterpret_cast<std::uintptr_t>(duplicated_self),
            std::memory_order_relaxed);
         joined_handle_published.store(true, std::memory_order_release);
      });
      const std::thread::id joined_worker_id = joined_worker.get_id();
      bool joined_handle_ready = false;
      for (std::uint32_t poll = 0; poll < ForeignWaitTimeoutMs; poll++) {
         if (joined_handle_published.load(std::memory_order_acquire)) {
            joined_handle_ready = true;
            break;
         }
         Sleep(1);
      }
      if (!joined_handle_ready)
         fail_closed_process("RUNTIME_FATAL stage=joined_handle_publish\n",
                             0xec);
      const std::uintptr_t joined_handle_value =
         joined_thread_handle.load(std::memory_order_relaxed);
      if (joined_handle_value == 0)
         fail_closed_process("RUNTIME_FATAL stage=joined_handle_missing\n",
                             0xed);
      const HANDLE joined_native_handle =
         reinterpret_cast<HANDLE>(joined_handle_value);
      if (WaitForSingleObject(joined_native_handle, ForeignWaitTimeoutMs) !=
          WAIT_OBJECT_0)
         fail_closed_process("RUNTIME_FATAL stage=joined_worker_timeout\n",
                             0xee);
      if (!CloseHandle(joined_native_handle))
         fail_closed_process("RUNTIME_FATAL stage=joined_handle_close\n",
                             0xef);
      try {
         joined_worker.join();
      } catch (...) {
         fail_closed_process("RUNTIME_FATAL stage=joined_thread_join\n",
                             0xf0);
      }
      if (joined_worker.joinable())
         return StatusJoinedThread;
      if (worker_initial_counter != ThreadLocalInitialValue ||
          thread_counter != ThreadLocalUpdatedValue || !worker_exception_ok)
         return StatusThreadLocal;
      if (worker_seen_id != joined_worker_id || caller_id == joined_worker_id ||
          caller_id != std::this_thread::get_id())
         return StatusThreadIdentity;

      bool caught_exception = false;
      try {
         throw 17;
      } catch (int value) {
         caught_exception = value == 17 && std::uncaught_exceptions() == 0;
      } catch (...) {
         return StatusException;
      }
      if (!caught_exception)
         return StatusException;

      detached_thread_runs.store(0, std::memory_order_relaxed);
      detached_thread_status.store(StatusDetachedThread,
                                   std::memory_order_relaxed);
      detached_thread_handle.store(0, std::memory_order_relaxed);
      detached_handle_published.store(false, std::memory_order_relaxed);
      std::thread detached_worker([]() {
         HANDLE duplicated_self = NULL;
         if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                              GetCurrentProcess(), &duplicated_self,
                              SYNCHRONIZE, FALSE, 0)) {
            detached_thread_status.store(StatusDetachedThread,
                                         std::memory_order_relaxed);
            detached_handle_published.store(true, std::memory_order_release);
            return;
         }
         detached_thread_handle.store(
            reinterpret_cast<std::uintptr_t>(duplicated_self),
            std::memory_order_relaxed);
         if (thread_counter != ThreadLocalInitialValue) {
            detached_thread_status.store(StatusThreadLocal,
                                         std::memory_order_relaxed);
         } else {
            thread_counter = DetachedThreadLocalValue;
            try {
               throw 31;
            } catch (int value) {
               if (value == 31 && std::uncaught_exceptions() == 0) {
                  detached_thread_runs.fetch_add(1,
                                                 std::memory_order_relaxed);
                  detached_thread_status.store(StatusOk,
                                               std::memory_order_relaxed);
               } else {
                  detached_thread_status.store(StatusException,
                                               std::memory_order_relaxed);
               }
            } catch (...) {
               detached_thread_status.store(StatusException,
                                            std::memory_order_relaxed);
            }
         }
         detached_handle_published.store(true, std::memory_order_release);
      });

      bool handle_published = false;
      for (std::uint32_t poll = 0; poll < ForeignWaitTimeoutMs; poll++) {
         if (detached_handle_published.load(std::memory_order_acquire)) {
            handle_published = true;
            break;
         }
         Sleep(1);
      }
      if (!handle_published)
         fail_closed_process("RUNTIME_FATAL stage=detached_handle_publish\n",
                             0xe3);

      const std::uintptr_t detached_handle_value =
         detached_thread_handle.load(std::memory_order_relaxed);
      if (detached_handle_value == 0) {
         fail_closed_process("RUNTIME_FATAL stage=detached_handle_missing\n",
                             0xe4);
      }

      try {
         detached_worker.detach();
      } catch (...) {
         const HANDLE failed_detached_handle = reinterpret_cast<HANDLE>(
            detached_handle_value);
         if (!wait_and_close_thread(failed_detached_handle))
            fail_closed_process("RUNTIME_FATAL stage=detach_failure_wait\n",
                                0xf1);
         if (detached_worker.joinable()) {
            try {
               detached_worker.join();
            } catch (...) {
               fail_closed_process("RUNTIME_FATAL stage=detached_fallback_join\n",
                                   0xf2);
            }
         }
         return StatusDetachedThread;
      }

      const HANDLE actual_detached_handle = reinterpret_cast<HANDLE>(
         detached_handle_value);
      if (!wait_and_close_thread(actual_detached_handle))
         fail_closed_process("RUNTIME_FATAL stage=detached_thread_wait\n",
                             0xe6);
      if (detached_thread_runs.load(std::memory_order_relaxed) != 1 ||
          detached_thread_status.load(std::memory_order_relaxed) != StatusOk)
         return StatusDetachedThread;

      std::cout.put('L');
      std::cout.flush();
      if (!std::cout)
         return StatusRegistration;

      if (once_runs != 1 || once_error.load(std::memory_order_relaxed) != 0 ||
          registration_count != 1 || joined_thread_runs != 1)
         return StatusRegistration;
      return StatusOk;
   } catch (...) {
      return StatusRegistration;
   }
}

/**
 * @brief Exercise the DLL from one harness-owned raw Win32 thread.
 * @param[in] api_version Must equal ApiVersion.
 * @param[in] slot Unique slot in 0..ForeignThreadSlotCount-1 for this DLL load.
 * @param[in] ready_event Borrowed event handle signaled after all IDs publish.
 * @param[in] release_event Borrowed event handle released by the harness.
 * @return StatusOk or a documented StatusForeign* failure code.
 * @ownership Both event handles remain owned by the caller and must stay valid
 *            until this call returns. The DLL closes neither handle.
 * @thread_safety Safe for concurrent slots; the registry mutex protects IDs.
 */
extern "C" __declspec(dllexport) std::uint32_t
runtime_foreign_thread_probe(std::uint32_t api_version, std::uint32_t slot,
                             std::uintptr_t ready_event,
                             std::uintptr_t release_event) noexcept
{
   if (api_version != ApiVersion || slot >= ForeignThreadSlotCount ||
       ready_event == 0 || release_event == 0)
      return StatusForeignArguments;

   try {
      if (thread_counter != ThreadLocalInitialValue)
         return StatusForeignThreadLocal;
      thread_counter = WorkerThreadLocalValue;
      const std::thread::id current_id = std::this_thread::get_id();
      bool signal_ready = false;
      {
         std::lock_guard<std::mutex> registry_lock(foreign_registry_mutex);
         if (foreign_thread_seen[slot])
            return StatusForeignIdentity;
         for (std::uint32_t index = 0; index < ForeignThreadSlotCount; index++) {
            if (foreign_thread_seen[index] &&
                foreign_thread_ids[index] == current_id)
               return StatusForeignIdentity;
         }
         foreign_thread_ids[slot] = current_id;
         foreign_thread_seen[slot] = true;
         foreign_thread_seen_count++;
         signal_ready = foreign_thread_seen_count == ForeignThreadSlotCount;
      }

      if (signal_ready && !SetEvent(reinterpret_cast<HANDLE>(ready_event)))
         return StatusForeignReady;
      const DWORD wait_status = WaitForSingleObject(
         reinterpret_cast<HANDLE>(release_event), ForeignWaitTimeoutMs);
      if (wait_status != WAIT_OBJECT_0)
         return StatusForeignRelease;
      if (thread_counter != WorkerThreadLocalValue)
         return StatusForeignThreadLocal;
      bool caught_exception = false;
      try {
         throw slot;
      } catch (std::uint32_t value) {
         caught_exception = value == slot && std::uncaught_exceptions() == 0;
      } catch (...) {
         return StatusForeignException;
      }
      return caught_exception ? StatusOk : StatusForeignException;
   } catch (...) {
      return StatusForeignException;
   }
}
