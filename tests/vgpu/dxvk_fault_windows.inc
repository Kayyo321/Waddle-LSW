/** @file dxvk_fault_windows.inc @brief Actual DXVK submission transport-loss gate.
 * The optional postflush case borrows live COM resources and owns one helper
 * thread and two events until joined. It never invokes D3D on the thread that
 * holds DXVK's submission mutex. The main thread records actual work before the
 * authenticated receiver retirement receipt, then observes real device loss.
 */

/** @brief C binding of pinned DXVK IDXGIVkInteropDevice's published COM ABI.
 * UUID and slot order come from submodules/dxvk/src/dxgi/dxgi_interfaces.h.
 * Only the three queue synchronization slots are invoked. Unused Vulkan slots
 * are represented as function pointers and never called. No reference escapes.
 */
typedef struct dxvk_fault_interop_t dxvk_fault_interop_t;
/** @brief Borrowed immutable vendor COM table; outputs transfer COM references. */
typedef struct dxvk_fault_vtable_t {
    HRESULT (STDMETHODCALLTYPE *query_interface)(dxvk_fault_interop_t *,REFIID,void **);
    ULONG (STDMETHODCALLTYPE *add_ref)(dxvk_fault_interop_t *);
    ULONG (STDMETHODCALLTYPE *release)(dxvk_fault_interop_t *);
    void (STDMETHODCALLTYPE *get_vulkan_handles)(void);
    void (STDMETHODCALLTYPE *get_submission_queue)(void);
    void (STDMETHODCALLTYPE *transition_surface_layout)(void);
    void (STDMETHODCALLTYPE *flush_rendering_commands)(dxvk_fault_interop_t *);
    void (STDMETHODCALLTYPE *lock_submission_queue)(dxvk_fault_interop_t *);
    void (STDMETHODCALLTYPE *release_submission_queue)(dxvk_fault_interop_t *);
} dxvk_fault_vtable_t;
/** @brief Borrowed vendor interface; query reference released after helper joins. */
struct dxvk_fault_interop_t { const dxvk_fault_vtable_t *vtable; };
/** @brief Published pinned vendor interface identity; no owned storage. */
static const GUID DxvkFaultInteropId={0xe2ef5fa5,0xdc21,0x4af7,{0x90,0xc4,0xf6,0x7e,0xf6,0xa0,0x93,0x23}};
/** @brief Main-owned events and reference, borrowed by sole helper until join. */
typedef struct dxvk_fault_lock_t {
    dxvk_fault_interop_t *interop;
    HANDLE locked;
    HANDLE release;
    DWORD error;
} dxvk_fault_lock_t;

/** @brief Hold the actual vendor submission mutex across external retirement.
 * @param[in,out] opaque Nonnull main-owned context, alive until thread joins.
 * @return Zero after unlock; one for an event failure. Main closes all owners.
 * @note Only helper calls lock/unlock; it never invokes a D3D method.
 */
static DWORD WINAPI fault_queue_thread(void *opaque)
{
    dxvk_fault_lock_t *lock=opaque;
    lock->interop->vtable->lock_submission_queue(lock->interop);
    if(!SetEvent(lock->locked))lock->error=GetLastError();
    else if(WaitForSingleObject(lock->release,INFINITE)!=WAIT_OBJECT_0)
        lock->error=GetLastError();
    lock->interop->vtable->release_submission_queue(lock->interop);
    return lock->error ? 1 : 0;
}

/** @brief Record a real GPU clear before loss, then require actual DXGI loss.
 * @param[in] device/context/view/swapchain Nonnull borrowed live COM owners.
 * @param[in] receipt Trusted fresh absolute path; identity exact bound session.
 * @return S_OK only if device and Present report DEVICE_RESET/REMOVED within
 * the original ten-second operation bound. Other failures are never credit.
 * @note Main thread records D3D; helper holds queue. Every helper/event/interface
 * is released before return. Receipt waits retain ownership, as normal cleanup.
 */
static HRESULT verify_transport_loss(ID3D11Device *device,ID3D11DeviceContext *context,
    ID3D11RenderTargetView *view,IDXGISwapChain *swapchain,const wchar_t *receipt,uint64_t identity)
{
    dxvk_fault_lock_t lock={0};HANDLE thread=NULL;HRESULT result=E_FAIL;
    HRESULT status=ID3D11Device_QueryInterface(device,&DxvkFaultInteropId,(void **)&lock.interop);
    if(FAILED(status)||!lock.interop)return FAILED(status)?status:E_NOINTERFACE;
    /* Release context bindings while the device is healthy. The later clear
     * targets the borrowed view directly and introduces no context bindings.
     * Recording another ClearState after device removal would create fresh CS
     * work in DXVK's destructor rather than merely releasing these owners. */
    ID3D11DeviceContext_ClearState(context);
    lock.interop->vtable->flush_rendering_commands(lock.interop);
    lock.locked=CreateEventW(NULL,TRUE,FALSE,NULL);
    lock.release=CreateEventW(NULL,TRUE,FALSE,NULL);
    if(!lock.locked||!lock.release)goto cleanup;
    thread=CreateThread(NULL,0,fault_queue_thread,&lock,0,NULL);
    if(!thread)goto cleanup;
    if(WaitForSingleObject(lock.locked,10000)!=WAIT_OBJECT_0)goto cleanup;
    const FLOAT Color[]={32.0f/255.0f,64.0f/255.0f,128.0f/255.0f,1.0f};
    ID3D11DeviceContext_ClearRenderTargetView(context,view,Color);
    lock.interop->vtable->flush_rendering_commands(lock.interop);
    fprintf(stderr,"DXVK actual fault stage: GPU work recorded, submission locked, session=%016llx\n",(unsigned long long)identity);
    fflush(stderr);
    wait_retired(receipt,identity);
    if(!SetEvent(lock.release))goto cleanup;
    if(WaitForSingleObject(thread,INFINITE)!=WAIT_OBJECT_0||lock.error)goto cleanup;
    ULONGLONG started=GetTickCount64();
    do {
        status=ID3D11Device_GetDeviceRemovedReason(device);
        if(GetTickCount64()-started>=10000){
            fprintf(stderr,"DXVK transport loss verification failed: device=0x%08lx elapsed_ms=%llu\n",
                (unsigned long)status,(unsigned long long)(GetTickCount64()-started));
            goto cleanup;
        }
        if(status==DXGI_ERROR_DEVICE_RESET||status==DXGI_ERROR_DEVICE_REMOVED)break;
        if(FAILED(status))goto cleanup;
        Sleep(1);
    }while(1);
    HRESULT presented=IDXGISwapChain_Present(swapchain,0,0);
    fprintf(stderr,"DXVK actual transport loss: device=0x%08lx Present=0x%08lx elapsed_ms=%llu\n",
        (unsigned long)status,(unsigned long)presented,(unsigned long long)(GetTickCount64()-started));
    if(GetTickCount64()-started>=10000||
        (presented!=DXGI_ERROR_DEVICE_RESET&&presented!=DXGI_ERROR_DEVICE_REMOVED))goto cleanup;
    puts("DXVK actual recorded submission and Present reported device removal after authenticated receiver retirement.");
    result=S_OK;
cleanup:
    if(thread){
        while(!SetEvent(lock.release))Sleep(10);
        while(WaitForSingleObject(thread,INFINITE)!=WAIT_OBJECT_0)Sleep(10);
        if(lock.error)result=E_FAIL;
        while(!CloseHandle(thread))Sleep(10);
    }
    if(lock.release)while(!CloseHandle(lock.release))Sleep(10);
    if(lock.locked)while(!CloseHandle(lock.locked))Sleep(10);
    lock.interop->vtable->release(lock.interop);
    return result;
}
