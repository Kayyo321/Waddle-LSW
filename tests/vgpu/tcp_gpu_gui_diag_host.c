/** @file tcp_gpu_gui_diag_host.c @brief Actual passive collector over bounded API doubles.
 * No Windows/GPU execution. Test graph/log buffers are test-owned and freed.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "win32_fake/gui_diag_fake.h"
static FILE *gui_log;
#undef stderr
#define stderr gui_log
#include "tcp_gpu_gui_diag_windows.inc"
#undef stderr
/** @brief Synthetic window; integer identities have no native owner. */
typedef struct fake_window_t { uintptr_t parent; DWORD pid; int live; } fake_window_t;
static fake_window_t graph[GuiDiagVisitLimit+8];
static unsigned graph_count, checks, groups, query_calls;
static DWORD last_error;
static int fault, duplicate_top;
#define Check(value) do { ++checks; if (!(value)) { fprintf(stdout,"GUI diagnostic FAIL line=%d: %s\n",__LINE__,#value); exit(1); } } while(0)
static uintptr_t number(HWND window) { return (uintptr_t)window; }
static DWORD GetCurrentProcessId(void) { return 100; }
static void SetLastError(DWORD error) { last_error=error; }
static DWORD GetLastError(void) { return last_error; }
static BOOL EnumWindows(BOOL (*callback)(HWND,LPARAM), LPARAM argument) {
    if (fault==1) { last_error=5; return 0; }
    for (unsigned i=1; i<=graph_count; ++i) if (graph[i].live && !graph[i].parent) {
        if (!callback((HWND)(uintptr_t)i,argument)) return 0;
        if (duplicate_top && !callback((HWND)(uintptr_t)i,argument)) return 0;
    }
    return 1;
}
static HWND FindWindowExA(HWND parent,HWND after,const char *class_name,const char *title) {
    Check(!class_name && !title);
    if (fault==2) { last_error=1400; return NULL; }
    if (fault==3 && parent==(HWND)(uintptr_t)1) return parent;
    if (fault==4 && after) return after;
    for (uintptr_t i=number(after)+1; i<=graph_count; ++i)
        if (graph[i].live && graph[i].parent==number(parent)) return (HWND)i;
    return NULL;
}
static DWORD GetWindowThreadProcessId(HWND window,DWORD *pid) {
    ++query_calls;
    if (fault==5) { last_error=1400; *pid=0; return 0; }
    *pid=graph[number(window)].pid;
    if (fault==6 && query_calls%2==0) *pid=999;
    return 1000+(DWORD)number(window);
}
static int GetClassNameA(HWND window,char *output,int bytes) {
    Check(number(window)>0 && bytes==GuiDiagClassBytes);
    if (fault==7) { last_error=1400; return 0; }
    if (fault==8) { memset(output,'x',(size_t)bytes-1); output[bytes-1]=0; return bytes-1; }
    strcpy(output,"STATIC"); return 6;
}
static LONG_PTR GetWindowLongPtrA(HWND window,int index) {
    (void)window;
    Check(index==GWL_STYLE || index==GWL_EXSTYLE);
    if (fault==9 || (fault==10 && index==GWL_EXSTYLE)) last_error=1400;
    return index==GWL_STYLE ? 1 : 0;
}
static HWND GetAncestor(HWND window,unsigned flags) { Check(flags==GA_PARENT);return (HWND)graph[number(window)].parent; }
static HWND GetWindow(HWND window,unsigned flags) { (void)window;Check(flags==GW_OWNER);return NULL; }
static BOOL IsWindowVisible(HWND window) { return number(window)%2!=0; }
static void reset(void) { memset(graph,0,sizeof graph);graph_count=0;fault=duplicate_top=0;query_calls=last_error=0;++groups; }
static void add(unsigned id,uintptr_t parent,DWORD pid) { graph[id]=(fake_window_t){.parent=parent,.pid=pid,.live=1};if(id>graph_count)graph_count=id; }
int main(void) {
    gui_diag_snapshot_t before,after;
    reset();last_error=123;gui_diag_capture(&before);Check(!before.count && !before.flags && last_error==123);
    reset();add(1,0,100);add(2,1,100);add(3,0,999);add(4,3,100);add(5,(uintptr_t)HWND_MESSAGE,100);add(6,5,100);add(7,(uintptr_t)HWND_MESSAGE,999);
    gui_diag_capture(&before);Check(before.count==5 && before.visited==7 && !before.flags);
    Check(gui_diag_find(&before,4) && gui_diag_find(&before,6) && !gui_diag_find(&before,3));
    const gui_diag_snapshot_t saved=before;
    graph[2].live=0;add(8,0,100);
    char *log=NULL;size_t log_bytes=0;gui_log=open_memstream(&log,&log_bytes);Check(gui_log!=NULL);
    gui_diag_report(&before,7,7,4,9);Check(fclose(gui_log)==0);gui_log=NULL;
    Check(!memcmp(&saved,&before,sizeof before));
    Check(strstr(log,"failed_samples gdi=7->7 user=4->9") && strstr(log,"phase=removed hwnd=2 ") && strstr(log,"phase=added hwnd=8 "));
    Check(strstr(log,"class_hex=535441544943") && !strstr(log,"PASS"));free(log);
    reset();for(unsigned i=1;i<=GuiDiagCapacity+1;i++)add(i,0,100);gui_diag_capture(&after);Check(after.count==GuiDiagCapacity && (after.flags&GuiDiagCapacityFault));
    reset();for(unsigned i=1;i<=GuiDiagVisitLimit+1;i++)add(i,0,999);gui_diag_capture(&after);Check(after.visited==GuiDiagVisitLimit && (after.flags&GuiDiagVisitFault) && !after.count);
    reset();for(unsigned i=1;i<=GuiDiagDepthLimit+2;i++)add(i,i-1,100);gui_diag_capture(&after);Check(after.flags&GuiDiagDepthFault);
    reset();add(1,0,100);duplicate_top=1;gui_diag_capture(&after);Check(after.count==1 && (after.flags&GuiDiagDuplicate));
    const DWORD expected[]={0,GuiDiagEnumerationFault,GuiDiagEnumerationFault,GuiDiagDuplicate,GuiDiagDuplicate,GuiDiagQueryFault,GuiDiagIdentityChanged,GuiDiagQueryFault,GuiDiagClassTruncated,GuiDiagQueryFault,GuiDiagQueryFault};
    for(int test=1;test<=10;test++) {
        reset();add(1,0,100);add(2,1,100);fault=test;
        gui_diag_capture(&after);Check(after.flags&expected[test]);
        if(test==1 || test==2 || test==5 || test==7 || test==9 || test==10)Check(after.last_error!=0);
    }
    reset();add(1,0,0);gui_diag_capture(&after);Check(after.flags&GuiDiagQueryFault);
    reset();add(1,0,100);gui_diag_capture(&before);after=before;
    after.windows[0].pid=101;Check(gui_diag_find(&after,1)->pid!=before.windows[0].pid);
    /* Exercise report identity replacement and empty class bytes without OS calls. */
    before.windows[0].pid=101;before.windows[0].tid=1;before.windows[0].class_name[0]=0;
    log=NULL;log_bytes=0;gui_log=open_memstream(&log,&log_bytes);Check(gui_log!=NULL);
    gui_diag_report(&before,1,2,3,4);Check(fclose(gui_log)==0);gui_log=NULL;
    Check(strstr(log,"phase=removed") && strstr(log,"phase=added"));free(log);
    before.windows[0].pid=100;
    log=NULL;log_bytes=0;gui_log=open_memstream(&log,&log_bytes);Check(gui_log!=NULL);
    gui_diag_report(&before,1,2,3,4);Check(fclose(gui_log)==0);free(log);
    before.windows[0].tid=1001;
    log=NULL;log_bytes=0;gui_log=open_memstream(&log,&log_bytes);Check(gui_log!=NULL);
    gui_diag_report(&before,1,2,3,4);Check(fclose(gui_log)==0);free(log);
    printf("GUI_DIAG_HOST groups=%u checks=%u PASS (API doubles only)\n",groups,checks);
    return 0;
}
