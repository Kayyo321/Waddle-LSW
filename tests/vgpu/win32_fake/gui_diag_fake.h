/** @file gui_diag_fake.h @brief Hand-written test-only Win32 ABI subset.
 * No SDK implementation/vendor source. Functions are deterministic API doubles;
 * they borrow integer HWND identities and never acquire a native resource.
 */
#ifndef WaddleGuiDiagFakeH
#define WaddleGuiDiagFakeH
#include <stdint.h>
typedef void *HWND;
typedef uint32_t DWORD;
typedef intptr_t LONG_PTR;
typedef intptr_t LPARAM;
typedef int BOOL;
#define CALLBACK
#define HWND_MESSAGE ((HWND)(intptr_t)-3)
#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)
#define GA_PARENT 1
#define GW_OWNER 4
static DWORD GetCurrentProcessId(void);
static void SetLastError(DWORD error);
static DWORD GetLastError(void);
static BOOL EnumWindows(BOOL (*callback)(HWND,LPARAM), LPARAM argument);
static HWND FindWindowExA(HWND parent,HWND after,const char *class_name,const char *title);
static DWORD GetWindowThreadProcessId(HWND window,DWORD *pid);
static int GetClassNameA(HWND window,char *output,int bytes);
static LONG_PTR GetWindowLongPtrA(HWND window,int index);
static HWND GetAncestor(HWND window,unsigned flags);
static HWND GetWindow(HWND window,unsigned flags);
static BOOL IsWindowVisible(HWND window);
#endif
