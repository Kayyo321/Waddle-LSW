/**
 * @file clvk_runtime_windows.cpp
 * @brief Private unload/reload probe for the MinGW C++ runtime.
 *
 * The DLL owns all C++ state and passes no pointers to or from its caller.
 * Loading constructs the namespace-scope shared mutex and iostream initializer;
 * unloading destroys them. Each invocation exercises call_once, exclusive
 * shared-mutex locking, a joined std::thread, and std::cout.
 */
#include <cstdint>
#include <iostream>
#include <mutex>
#include <shared_mutex>
#include <thread>

namespace {

constexpr std::uint32_t ApiVersion = 1;

std::shared_mutex pass_registry_mutex;
std::once_flag registry_once;
std::uint32_t once_runs = 0;
std::uint32_t registration_count = 0;
std::uint32_t joined_thread_runs = 0;
std::ios_base::Init stream_initializer;

}  // namespace

/**
 * @brief Exercise static C++ runtime state for one DLL load lifetime.
 * @param[in] api_version Must equal ApiVersion.
 * @return Zero only when call_once, lock, joined-thread, and stream checks pass.
 * @ownership The DLL owns all static objects; no pointers or allocations cross
 *            the C ABI boundary. The caller owns no returned resource.
 * @thread_safety Safe for sequential invocation; this probe is not a
 *                multithreaded stress test.
 */
extern "C" __declspec(dllexport) std::uint32_t
runtime_lifetime_probe(std::uint32_t api_version) noexcept
{
   if (api_version != ApiVersion)
      return 1;

   try {
      std::call_once(registry_once, []() { once_runs++; });

      {
         std::unique_lock<std::shared_mutex> write_lock(pass_registry_mutex);
         registration_count++;
      }

      std::uint32_t worker_observed_registrations = 0;
      std::thread worker([&worker_observed_registrations]() {
         std::unique_lock<std::shared_mutex> write_lock(pass_registry_mutex);
         joined_thread_runs++;
         worker_observed_registrations = registration_count;
      });
      worker.join();

      std::cout.put('L');
      std::cout.flush();
      if (!std::cout)
         return 2;
      if (once_runs != 1 || joined_thread_runs != 1 ||
          worker_observed_registrations != 1 || registration_count != 1)
         return 3;

      return 0;
   } catch (...) {
      return 4;
   }
}
