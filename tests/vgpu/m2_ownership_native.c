/** @file m2_ownership_native.c @brief Real Win32 sink ownership and leak controls.
 * @note Executes only when explicitly launched on Windows; no Vulkan/GPU/VM
 * connection. Each case owns one HWND, any unrelated DC and all probe DCs.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "m2_ownership_probe.h"
#include "waddle/venus_win32_present.h"
#include <stdio.h>
static int run_case(unsigned mode, const char *name) {
    HWND window=NULL;
    HDC unrelated=NULL;
    int result=1;
    const char *stage="empty test state";
    m2_owner_snapshot_t view={0};
    DWORD before=0,after=0;
    if(!venus_m2_dc_test_reset())goto cleanup;
    stage="real window acquisition";
    window=CreateWindowExA(0,"STATIC","M2 exact owner controls",WS_POPUP|WS_VISIBLE,
                           20,20,16,16,NULL,NULL,GetModuleHandleA(NULL),NULL);
    if(!window || !UpdateWindow(window))goto cleanup;
    const DWORD gdi=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    const DWORD user=GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS);
    if(mode==3){stage="unrelated owned DC acquisition";unrelated=GetDC(window);if(!unrelated)goto cleanup;}
    stage="process diagnostic before control";
    if(!GetProcessHandleCount(GetCurrentProcess(),&before) || !before)goto cleanup;
    if(mode && !venus_m2_dc_test_fault(mode==2 ? M2DcFailRelease : M2DcOmitRelease))goto cleanup;
    const uint32_t pixels[256]={0x00123456};
    stage="actual production sink";
    const int presented=venus_win32_present_pixels_exact((uintptr_t)window,16,16,pixels,sizeof pixels,0);
    if(presented!=(mode==2 ? 0 : 1))goto cleanup;
    if(unrelated){
        stage="unrelated exact DC release";
        if(!ReleaseDC(window,unrelated))goto cleanup;
        unrelated=NULL;
    }
    stage="read-only owner snapshot";
    if(!venus_m2_dc_snapshot(M2OwnerVersion,&view,sizeof view))goto cleanup;
    if(view.acquired!=1 || view.next_id!=2 || view.pending)goto cleanup;
    if(!mode){
        if(!m2_owner_clean(&view) || view.retired!=1)goto cleanup;
    } else {
        if(m2_owner_clean(&view) || view.live!=1 || view.retired || view.first_live_id!=1 ||
           view.first_live_parent!=(uint64_t)(uintptr_t)window || !view.first_live_value ||
           view.failures!=(mode==2 ? M2OwnerReleaseFailed : 0))goto cleanup;
        /* A deliberate leak must be rejected before cleanup despite equal total
         * kernel counts. No retry, baseline inflation or warmup is performed. */
        if(venus_m2_dc_test_reset())goto cleanup;
    }
    stage="equal process-total negative control";
    if(!GetProcessHandleCount(GetCurrentProcess(),&after) || !after || (mode && before!=after))goto cleanup;
    printf("M2_OWNERSHIP_NATIVE_INFO case=%s count_before=%lu count_after=%lu live=%u first_id=%llu failures=%u rejected=%u\n",
           name,(unsigned long)before,(unsigned long)after,view.live,
           (unsigned long long)view.first_live_id,view.failures,mode ? 1u : 0u);
    stage="actual matching owned DC cleanup";
    if(!venus_m2_dc_test_cleanup())goto cleanup;
    if(!venus_m2_dc_snapshot(M2OwnerVersion,&view,sizeof view) || view.live || view.pending || view.acquired!=view.retired)goto cleanup;
    if(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)!=gdi ||
       GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS)!=user)goto cleanup;
    result=0;
cleanup:
    /* Never close an arbitrary borrowed handle. Only this case's acquisitions
     * are eligible; a real API release failure remains a failing test. */
    if(!venus_m2_dc_test_cleanup())result=1;
    if(unrelated && !ReleaseDC(window,unrelated))result=1;
    if(window && !DestroyWindow(window))result=1;
    if(result)fprintf(stderr,"M2_OWNERSHIP_NATIVE case=%s FAIL stage=%s before=%lu after=%lu\n",
                      name,stage,(unsigned long)before,(unsigned long)after);
    else printf("M2_OWNERSHIP_NATIVE case=%s PASS\n",name);
    fflush(stdout);fflush(stderr);
    return result;
}
/** @brief Execute four real-window controls and prove all controlled owners retire.
 * @return Zero all cases pass; one failed/incomplete evidence. Sole UI thread.
 * Leak/failure injection exists only in this standalone binary, not the DLL.
 */
int main(void) {
    const char *names[]={"positive", "owned-leak-equal-total", "injected-release-failure", "unrelated-release-masks-total"};
    for(unsigned mode=0;mode<4;++mode)if(run_case(mode,names[mode]))return 1;
    puts("M2_OWNERSHIP_NATIVE 4 cases PASS");
    return 0;
}
