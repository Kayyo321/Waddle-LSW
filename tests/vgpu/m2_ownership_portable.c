/** @file m2_ownership_portable.c @brief Actual observer/sink with scripted Win32 APIs. */
#include "m2_ownership_probe.h"
#include "waddle/venus_win32_present.h"
#include <windows.h>
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>
static unsigned checks, gets, releases, live, fail_get, fail_release, duplicate;
static uintptr_t serial=100;
static void check(int value,unsigned line) {
    ++checks;
    if(!value){fprintf(stderr,"M2 observer check failed line=%u\n",line);exit(1);}
}
/** @brief Non-elidable test assertion. */
#define Check(value) check(!!(value),__LINE__)
int IsWindow(HWND window){return window==(HWND)(uintptr_t)11;}
int GetClientRect(HWND window,RECT *bounds){if(!IsWindow(window))return 0;*bounds=(RECT){0,0,2,2};return 1;}
HDC GetDC(HWND window){Check(IsWindow(window));++gets;if(fail_get)return NULL;++live;return (HDC)(duplicate ? (uintptr_t)101 : ++serial);}
int ReleaseDC(HWND window,HDC dc){Check(IsWindow(window) && dc && live);++releases;if(fail_release)return 0;--live;return 1;}
int StretchDIBits(HDC dc,int x,int y,int width,int height,int source_x,int source_y,int source_width,int source_height,const void *pixels,const BITMAPINFO *info,unsigned usage,unsigned operation){
    (void)x;(void)y;(void)width;(void)height;(void)source_x;(void)source_y;(void)source_width;(void)source_height;(void)pixels;(void)info;(void)usage;(void)operation;
    Check(dc && live);return 2;
}
int32_t DwmFlush(void){return 0;}
static m2_owner_snapshot_t snapshot(void){m2_owner_snapshot_t view={0};Check(venus_m2_dc_snapshot(M2OwnerVersion,&view,sizeof view));return view;}
static int present(void){const uint32_t pixels[4]={1,2,3,4};return venus_win32_present_pixels_exact(11,2,2,pixels,sizeof pixels,0);}
static void reset(void){Check(!live);Check(venus_m2_dc_test_reset());fail_get=fail_release=duplicate=0;serial=100;gets=releases=0;}
static void ordinary(void){
    reset();m2_owner_snapshot_t view=snapshot();Check(m2_owner_clean(&view));
    for(unsigned i=0;i<128;++i)Check(present()==1);
    view=snapshot();Check(m2_owner_clean(&view) && view.acquired==128 && view.retired==128 && gets==128 && releases==128 && !live);
    fail_get=1;Check(!present());view=snapshot();Check(m2_owner_clean(&view) && view.acquired==128 && view.next_id==130);
    fail_get=0;Check(present()==1);Check(venus_m2_dc_test_cleanup());
    Check(!venus_m2_dc_snapshot(999,&view,sizeof view));Check(!venus_m2_dc_snapshot(M2OwnerVersion,NULL,sizeof view));Check(!venus_m2_dc_snapshot(M2OwnerVersion,&view,1));
    puts("M2_OWNERSHIP_PORTABLE case=normal-and-failed-acquisition PASS");
}
static void negative(void){
    for(unsigned fault=M2DcOmitRelease;fault<=M2DcFailRelease;++fault){
        reset();Check(!venus_m2_dc_test_fault(999));Check(venus_m2_dc_test_fault(fault));Check(!venus_m2_dc_test_fault(fault));
        Check(present()==(fault==M2DcOmitRelease ? 1 : 0));
        m2_owner_snapshot_t view=snapshot();Check(view.live==1 && view.acquired==1 && !view.retired && view.first_live_value==101 && !m2_owner_clean(&view));
        Check(view.failures==(fault==M2DcFailRelease ? M2OwnerReleaseFailed : 0));Check(!venus_m2_dc_test_reset());
        const unsigned prior=gets;
        if(fault==M2DcFailRelease){Check(!present());Check(gets==prior);}
        fail_release=1;Check(!venus_m2_dc_test_cleanup());view=snapshot();Check(view.live==1 && live==1);
        fail_release=0;Check(venus_m2_dc_test_cleanup());view=snapshot();Check(!view.live && view.acquired==view.retired);
    }
    reset();fail_release=1;Check(!present());
    m2_owner_snapshot_t failed=snapshot();Check(failed.live==1 && failed.failures==M2OwnerReleaseFailed && releases==1);
    fail_release=0;Check(venus_m2_dc_test_cleanup());Check(!live);
    reset();const uintptr_t dc=venus_m2_dc_acquire(11);Check(dc);
    const unsigned prior=releases;Check(!venus_m2_dc_release(12,dc));Check(releases==prior && live==1);
    Check(!venus_m2_dc_acquire(11));Check(venus_m2_dc_release(11,dc));Check(venus_m2_dc_test_cleanup());
    reset();Check(!venus_m2_dc_acquire(0));Check(!gets);
    puts("M2_OWNERSHIP_PORTABLE case=leak-failed-cleanup-and-foreign-release PASS");
}
static void capacity(void){
    reset();uintptr_t owned[M2OwnerCapacity];
    for(unsigned i=0;i<M2OwnerCapacity;++i){owned[i]=venus_m2_dc_acquire(11);Check(owned[i]);}
    Check(!venus_m2_dc_acquire(11));Check(gets==M2OwnerCapacity && live==M2OwnerCapacity);
    for(unsigned i=0;i<M2OwnerCapacity;++i)Check(venus_m2_dc_release(11,owned[i]));
    m2_owner_snapshot_t view=snapshot();Check(!view.live && view.failures==M2OwnerLimit);
    for(unsigned failed=0;failed<2;++failed){
        reset();duplicate=1;Check(venus_m2_dc_acquire(11)==101);fail_release=failed;
        Check(!venus_m2_dc_acquire(11));view=snapshot();Check(view.live==(failed ? 2u : 1u) && (view.failures&M2OwnerDuplicate));
        fail_release=0;Check(venus_m2_dc_test_cleanup());Check(!live);
    }
    reset();Check(venus_m2_dc_test_fault(M2DcNormal));
    puts("M2_OWNERSHIP_PORTABLE case=pre-acquisition-capacity-and-duplicate-result PASS");
}
/** @brief Run actual C observer/sink over API doubles; no Windows execution claim.
 * @return Zero all ownership controls pass; one on failure. Sole UI test thread.
 */
int main(void){ordinary();negative();capacity();printf("M2_OWNERSHIP_PORTABLE 3 cases %u checks PASS\n",checks);return 0;}
