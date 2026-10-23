/** @file wddm_dll.c @brief Load the actual Windows userland DLL and verify its ABI. */
#include "waddle/venus_wddm.h"
#include <assert.h>
#include <windows.h>
/** @brief Transient native DLL initialization signature; no retained ownership. */
typedef venus_ring_status_t (*adapter_init_t)(venus_wddm_t *, uint64_t);
/** @brief Transient native DLL immutable capability query signature. */
typedef venus_ring_status_t (*adapter_query_t)(const venus_wddm_t *, venus_wddm_capabilities_t *);
/** @brief Transient native DLL adapter teardown signature. */
typedef venus_ring_status_t (*adapter_free_t)(venus_wddm_t *);
int main(void) {
    HMODULE library = LoadLibraryW(L"build/waddle_userland_adapter.dll");
    assert(library);
    const char *const Symbols[] = {"venus_wddm_init",
                                   "venus_wddm_capabilities",
                                   "venus_wddm_context_create",
                                   "venus_wddm_context_destroy",
                                   "venus_wddm_context_discard",
                                   "venus_wddm_allocation_create",
                                   "venus_wddm_allocation_destroy",
                                   "venus_wddm_free"};
    for (unsigned index = 0; index < sizeof(Symbols) / sizeof(Symbols[0]); index++)
        assert(GetProcAddress(library, Symbols[index]));
    assert(!GetProcAddress(library, "DriverEntry"));
    assert(!GetProcAddress(library, "venus_guest_exchange"));
    adapter_init_t initialize = (adapter_init_t)GetProcAddress(library, "venus_wddm_init");
    adapter_query_t query = (adapter_query_t)GetProcAddress(library, "venus_wddm_capabilities");
    adapter_free_t release = (adapter_free_t)GetProcAddress(library, "venus_wddm_free");
    venus_wddm_t adapter = {0};
    venus_wddm_capabilities_t capabilities;
    assert(initialize(&adapter, 123) == RingOk);
    assert(query(&adapter, &capabilities) == RingOk && capabilities.adapter_luid == 123);
    assert(capabilities.flags == (AdapterRender | AdapterCompute));
    assert(release(&adapter) == RingOk && !adapter.adapter_luid);
    assert(FreeLibrary(library));
    return 0;
}
