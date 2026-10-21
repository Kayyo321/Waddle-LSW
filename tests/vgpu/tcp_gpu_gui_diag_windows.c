/** @file tcp_gpu_gui_diag_windows.c @brief Actual Win32 HWND diagnostic controls.
 * Cross-build is not execution. Root runs on the existing interactive Windows
 * desktop. Only this test's own HWNDs are destroyed; no GPU/DLL/VM control.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "tcp_gpu_gui_diag_windows.inc"
int main(void) {
    gui_diag_snapshot_t before,retained,retired;
    HWND top=NULL,child=NULL,message=NULL;
    int result=1;
    const DWORD gdi_before=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    const DWORD user_before=GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS);
    gui_diag_capture(&before);
    top=CreateWindowExA(0,"STATIC","GUI diagnostic owned control",WS_POPUP,0,0,8,8,NULL,NULL,GetModuleHandleA(NULL),NULL);
    if(!top)goto cleanup;
    child=CreateWindowExA(0,"STATIC","GUI diagnostic child",WS_CHILD,0,0,4,4,top,NULL,GetModuleHandleA(NULL),NULL);
    message=CreateWindowExA(0,"STATIC","GUI diagnostic message",0,0,0,0,0,HWND_MESSAGE,NULL,GetModuleHandleA(NULL),NULL);
    if(!child || !message)goto cleanup;
    gui_diag_capture(&retained);
    if(retained.flags || !gui_diag_find(&retained,(uintptr_t)top) ||
       !gui_diag_find(&retained,(uintptr_t)child) || !gui_diag_find(&retained,(uintptr_t)message))goto cleanup;
    /* Retained real HWND identities are intentional negative-control evidence;
     * the report is diagnostic only and does not require a global count delta. */
    const DWORD gdi_retained=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    const DWORD user_retained=GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS);
    gui_diag_report(&before,gdi_before,gdi_retained,user_before,user_retained);
    puts("GUI_DIAG_NATIVE case=retained_top_child_message PASS");
    if(!DestroyWindow(child))goto cleanup;
    child=NULL;
    if(!DestroyWindow(message))goto cleanup;
    message=NULL;
    HWND retired_top=top;
    if(!DestroyWindow(top))goto cleanup;
    top=NULL;
    gui_diag_capture(&retired);
    if(retired.flags || gui_diag_find(&retired,(uintptr_t)retired_top))goto cleanup;
    puts("GUI_DIAG_NATIVE case=exact_owned_cleanup PASS");result=0;
cleanup:
    if(child && !DestroyWindow(child))result=1;
    if(message && !DestroyWindow(message))result=1;
    if(top && !DestroyWindow(top))result=1;
    if(result)fputs("GUI_DIAG_NATIVE FAIL\n",stderr);
    return result;
}
