/** @file clvk_pool_lifetime.cpp
 * @brief Deterministic queue-retirement interleaving against the actual pool.
 * The build-admission test extracts the patched vendor pool verbatim. Only its
 * executor boundary is a controlled double; no GPU/OS-thread claim follows.
 */
#include <cassert>
#include <memory>
#include <mutex>
#include <unordered_map>

/** @brief Vendor executor boundary double; single test thread owns each flag. */
struct cvk_executor_thread {
    bool idle = true;
    bool is_idle() { return idle; }
    void shutdown() {}
};
#include "clvk_pool_under_test.hpp"

int main() {
    cvk_executor_thread_pool pool;
    auto first = pool.get_executor();
    first->idle = false;
    pool.return_executor(first);
    assert(!pool.waddle_quiescent());
    // A queue can return its executor before the execution loop becomes idle.
    // Selection must skip that executor without stranding it as bound.
    auto second = pool.get_executor();
    assert(second != first);
    first->idle = true;
    second->idle = false;
    pool.return_executor(second);
    auto reused = pool.get_executor();
    assert(reused == first);
    assert(!pool.waddle_quiescent());
    pool.return_executor(reused);
    assert(!pool.waddle_quiescent());
    second->idle = true;
    assert(pool.waddle_quiescent());
}
