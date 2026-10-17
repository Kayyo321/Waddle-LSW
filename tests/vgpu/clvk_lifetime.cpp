/** @file clvk_lifetime.cpp
 * @brief Actual patched vendor refcount owners under sanitizer/thread stress.
 * This C++ boundary is required to exercise clvk's own C++ lifetime types.
 * Logging is a no-allocation double; Vulkan/compiler/GPU execution is separate.
 */
#include "objects.hpp"
#include <cassert>
#include <thread>
#include <vector>

/** @brief Vendor logging ABI double; arguments borrowed, no owner or effect.
 * Thread safe; all arguments may be null where the vendor permits them.
 */
void cvk_log(uint64_t, loglevel, const char*, ...) {}

/** @brief One real refcounted allocation; base destructor retires the ledger. */
struct counted_owner_t : refcounted {};

int main() {
    assert(waddle_lifetime.can_shutdown(0, true));
    assert(!waddle_lifetime.can_shutdown(0, false));
    auto owner = new counted_owner_t;
    assert(owner->refcount() == 1);
    assert(waddle_lifetime.live_objects.load() == 1);
    assert(!waddle_lifetime.can_shutdown(0, true));
    owner->retain();
    assert(owner->refcount() == 2);
    assert(waddle_lifetime.live_objects.load() == 1);
    owner->release();
    assert(waddle_lifetime.live_objects.load() == 1);
    {
        refcounted_holder<counted_owner_t> held(owner);
        refcounted_holder<counted_owner_t> copy(held);
        refcounted_holder<counted_owner_t> moved(std::move(copy));
        assert(owner->refcount() == 3);
        held.reset(nullptr);
        assert(owner->refcount() == 2);
    }
    assert(owner->refcount() == 1);
    owner->release();
    owner = nullptr;
    assert(waddle_lifetime.can_shutdown(0, true));
    std::vector<std::thread> threads;
    for (size_t i = 0; i < 8; ++i) {
        threads.emplace_back([] {
            for (size_t cycle = 0; cycle < 2000; ++cycle) {
                auto item = new counted_owner_t;
                item->retain();
                item->release();
                item->release();
            }
        });
    }
    for (auto& thread : threads) thread.join();
    assert(waddle_lifetime.live_objects.load() == 0);
    assert(waddle_lifetime.can_shutdown(0, true));
}
