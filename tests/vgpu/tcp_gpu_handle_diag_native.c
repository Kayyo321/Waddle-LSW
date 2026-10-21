/** @file tcp_gpu_handle_diag_native.c @brief Native Windows proof of the handle-retirement
 * diagnostics' ntdll layout assumptions.
 * The REAL tcp_gpu_handle_diag_windows.inc is included below, unchanged, and exercised against
 * the real ntdll of the Windows build it runs on: the ProcessHandleInformation (class 51) entry
 * layout, the ObjectTypeInformation (class 2) name layout, the object type names, the granted
 * access field, and the passivity of both queries. It cannot run on the Linux host; the
 * counterpart tcp_gpu_handle_diag_host.c covers the same code over a scripted kernel.
 *
 * Build: make vgpu-handle-diag-native-build (cross-compile only). Run build/vgpu_handle_diag_native.exe
 * in the Windows guest as a medium-integrity console process, without arguments.
 *
 * Output on stdout, one line per case and one final line:
 *   HANDLE_DIAG_NATIVE case=<name> PASS
 *   HANDLE_DIAG_NATIVE case=<name> FAIL reason=<one-line assertion text>
 *   HANDLE_DIAG_NATIVE PASS cases=<n>        every case passed and the handle count balanced
 *   HANDLE_DIAG_NATIVE FAIL ...              otherwise; the exit status is then 1
 * Informational lines on stderr start with HANDLE_DIAG_NATIVE_INFO and never decide the verdict.
 *
 * Assumptions to adjust here if the first native run disagrees (each failure prints the actual value):
 *  - NT object type names: Event, Semaphore, Mutant, Timer, File, Section, Job, Thread.
 *  - CreateEventW, CreateSemaphoreW, CreateMutexW and CreateWaitableTimerW return handles whose
 *    granted access is the documented *_ALL_ACCESS mask (0x1F0003, 0x1F0003, 0x1F0001, 0x1F0003);
 *    this is what proves the granted_access field offset. Other types only need a nonzero mask.
 *  - Standard handles: only valid GetStdHandle values are checked. Each is also duplicated with
 *    DuplicateHandle(DUPLICATE_SAME_ACCESS); the duplicate must report the same type index and access.
 *    A failing DuplicateHandle skips that comparison with an INFO line.
 *  - A silent warm-up pass (not a case) runs every API once first so first-use initialization of the
 *    OS is not attributed to a case; the balance checks start from the count after it.
 *  - Handle count balance is awaited like the fixture does (up to 100 samples, 10 ms apart); a
 *    count that never returns fails.
 *  - added_types repeats the whole case up to three times, with an INFO line, only when the sole
 *    discrepancy is unrelated handles added by the OS in the window; any missing, misnamed or
 *    mis-listed handle of its own fails at once.
 *  - The included diagnostics' stderr, stdout and printf are redirected to temp-file streams defined
 *    here, so the process's own stdout, stderr and standard handles are never touched. The temp
 *    directory must be writable: added_types, removed and failure_report_shape need it, while
 *    layout_sanity and timing skip their text sub-checks with an INFO line. Temp files are removed
 *    on every path.
 *  - TerminateThread on the never-started thread leaves its stack reserved (address space only; no handle).
 */
#include <windows.h>
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief The process stdout; the verdict lines are written to it and it is never redirected.
 * @return Borrowed real stream; never owned by the caller.
 */
static FILE *native_real_stdout(void) { return stdout; }
/** @brief The process stderr, reachable after the stderr macro below redirects the name for the .inc.
 * @return Borrowed real stream; never owned by the caller.
 */
static FILE *native_real_stderr(void) { return stderr; }
/** @brief Stream that stands in for stderr inside the included diagnostics; NULL outside a capture. */
static FILE *native_capture_stream;
/** @brief Stream that stands in for stdout and printf inside the included diagnostics; NULL outside a capture. */
static FILE *native_capture_output;
#undef stderr
/** @brief The included diagnostics write their stderr text into the capture stream. */
#define stderr (native_capture_stream)
#undef stdout
/** @brief The included diagnostics write their stdout text into the output capture stream. */
#define stdout (native_capture_output)
#undef printf
/** @brief printf of the included diagnostics goes to the output capture stream. */
#define printf(...) fprintf((native_capture_output),__VA_ARGS__)

/** @brief Resolve an export of a module; same body as symbol() in tcp_gpu_windows.c.
 * @param[in] module Module from GetModuleHandleA, borrowed.
 * @param[in] name Nonnull export name, borrowed.
 * @param[out] output Receives the function pointer bytes on success.
 * @param[in] bytes Size of output; must equal the size of a function pointer.
 * @return Zero on success; one, leaving output untouched, when the export is absent or bytes differs.
 * @note The make rule for this test compares this body with the fixture's byte for byte.
 */
static int symbol(HMODULE module,const char *name,void *output,size_t bytes)
{
    FARPROC procedure=GetProcAddress(module,name);if(!procedure || bytes!=sizeof procedure)return 1;
    memcpy(output,&procedure,bytes);return 0;
}
#include "tcp_gpu_handle_diag_windows.inc"

/** @brief Fixed capacities and counts of this test. */
enum {
    NativeObjectKinds = 8,       /**< Objects the added_types case creates. */
    NativeMaxDeltaLines = 512,   /**< Delta lines kept from one capture; more fails the case. */
    NativeCaptureBytes = 262144, /**< Largest capture text read back; more fails the case. */
    NativeLineBytes = 512,       /**< Longest output line the parsers accept. */
    NativeSettleAttempts = 100,  /**< Handle count samples, 10 ms apart, while waiting for balance. */
    NativeStressRounds = 50,     /**< Captures and name lookups of the passive case. */
    NativeStableRuns = 5,        /**< Back-to-back comparisons of layout_sanity. */
    NativeAddedAttempts = 3      /**< Whole-case attempts of added_types when only OS noise appears. */
};
/** @brief Documented all-access masks of the synchronization objects (EVENT_ALL_ACCESS and kin). */
enum { NativeEventAllAccess = 0x1F0003, NativeSemaphoreAllAccess = 0x1F0003,
       NativeMutantAllAccess = 0x1F0001, NativeTimerAllAccess = 0x1F0003 };

/** @brief One-line text of the first assertion that failed in the running case. */
static char failure_reason[512];
/** @brief Nonzero when the failure recorded in failure_reason is only unrelated OS handles. */
static int failure_is_noise;

/** @brief Record the first failure of the running case as one line; later failures are ignored.
 * @param[in] line Source line of the assertion. @param[in] format printf format of the message.
 */
__attribute__((format(printf,2,3)))
static void record_failure(int line,const char *format,...)
{
    if(failure_reason[0])return;
    const int used=snprintf(failure_reason,sizeof failure_reason,"line %d: ",line);
    va_list arguments;va_start(arguments,format);
    if(used>0 && (size_t)used<sizeof failure_reason)vsnprintf(failure_reason+used,sizeof failure_reason-(size_t)used,format,arguments);
    va_end(arguments);
    for(char *cursor=failure_reason;*cursor;cursor++)if(*cursor=='\n' || *cursor=='\r')*cursor=' ';
}
/** @brief Assert inside a case: on failure record the message and leave through cleanup.
 * @details The case defines int result and a cleanup label.
 */
#define require(condition,...) do{if(!(condition)){record_failure(__LINE__,__VA_ARGS__);result=1;goto cleanup;}}while(0)

/** @brief Print an informational line on the real stderr.
 * @param[in] format printf format; the line is prefixed and newline terminated.
 */
__attribute__((format(printf,1,2)))
static void native_info(const char *format,...)
{
    FILE *stream=native_real_stderr();
    if(!format)return;
    fputs("HANDLE_DIAG_NATIVE_INFO ",stream);
    va_list arguments;va_start(arguments,format);
    vfprintf(stream,format,arguments);
    va_end(arguments);
    fputc('\n',stream);fflush(stream);
}

/** @brief Read the process kernel handle count with the fixture's own API.
 * @return The nonzero count, or zero if GetProcessHandleCount fails.
 */
static DWORD process_handles(void)
{
    DWORD count=0;
    if(!GetProcessHandleCount(GetCurrentProcess(),&count))return 0;
    return count;
}
/** @brief Wait until the process handle count equals expected, as the fixture does.
 * @param[in] expected Count to reach. @param[out] last Nonnull; receives the final observed count.
 * @return One when the count matched within 100 samples 10 ms apart, else zero.
 * @details Asynchronous OS closures can lag a CloseHandle; a leaked handle never settles.
 */
static int settle_handles(DWORD expected,DWORD *last)
{
    for(unsigned attempt=0;attempt<NativeSettleAttempts;attempt++){
        *last=process_handles();
        if(*last==expected)return 1;
        Sleep(10);
    }
    return 0;
}
/** @brief Require the handle count to be back at start; records the failure.
 * @param[in] start Count measured before the case acquired anything. @return Zero when balanced.
 */
static int check_balance(DWORD start)
{
    DWORD last=0;
    if(settle_handles(start,&last))return 0;
    record_failure(__LINE__,"handle count did not return to its start: started at %lu, ended at %lu",(unsigned long)start,(unsigned long)last);
    return 1;
}

/** @brief Temp-file stream standing in for the included diagnostics' stderr or stdout during a capture. */
typedef struct capture_t {
    FILE *stream;       /**< Read/write binary stream, NULL when closed. */
    FILE **route;       /**< Variable the diagnostics write through while the capture is open. */
    char path[MAX_PATH];/**< Temp file path, empty when no file exists. */
} capture_t;
/** @brief Open a temp-file capture and route one of the included diagnostics' streams into it.
 * @param[out] capture Nonnull; owned by the caller and released by capture_close on every path.
 * @param[in] route &native_capture_stream for stderr text or &native_capture_output for stdout text.
 * @return Zero on success; one if the temp file cannot be created (nothing is left behind).
 */
static int capture_open(capture_t *capture,FILE **route)
{
    char directory[MAX_PATH];
    capture->stream=NULL;capture->route=NULL;capture->path[0]=0;
    const DWORD length=GetTempPathA((DWORD)sizeof directory,directory);
    if(!length || length>=sizeof directory)return 1;
    if(!GetTempFileNameA(directory,"hdn",0,capture->path)){capture->path[0]=0;return 1;}
    capture->stream=fopen(capture->path,"w+b");
    if(!capture->stream){DeleteFileA(capture->path);capture->path[0]=0;return 1;}
    capture->route=route;*route=capture->stream;
    return 0;
}
/** @brief Read everything the diagnostics wrote into the capture.
 * @param[in] capture Open capture. @param[out] text Receives the NUL-terminated bytes.
 * @param[in] capacity Size of text. @return Zero on success; one on I/O error or if the capture is larger.
 */
static int capture_read(capture_t *capture,char *text,size_t capacity)
{
    if(!capture->stream || capacity<2)return 1;
    if(fflush(capture->stream)!=0 || fseek(capture->stream,0,SEEK_SET)!=0)return 1;
    const size_t length=fread(text,1,capacity-1,capture->stream);
    if(ferror(capture->stream) || fgetc(capture->stream)!=EOF)return 1;
    text[length]=0;
    return 0;
}
/** @brief Close the capture stream and delete its temp file; idempotent.
 * @param[in,out] capture Capture to release.
 */
static void capture_close(capture_t *capture)
{
    if(capture->route && *capture->route==capture->stream)*capture->route=NULL;
    capture->route=NULL;
    if(capture->stream){fclose(capture->stream);capture->stream=NULL;}
    if(capture->path[0]){DeleteFileA(capture->path);capture->path[0]=0;}
}

/** @brief Find a snapshot entry by handle value.
 * @param[in] set Valid snapshot, borrowed. @param[in] value Handle value to look for.
 * @return Borrowed entry, valid until the set is recaptured or freed, or NULL.
 */
static const handle_diag_entry_t *find_entry(const handle_diag_set_t *set,uintptr_t value)
{
    for(size_t index=0;index<set->count;index++)if(set->entries[index].handle_value==value)return &set->entries[index];
    return NULL;
}
/** @brief Report whether any snapshot entry has the object type index. */
static int type_present(const handle_diag_set_t *set,ULONG type_index)
{
    for(size_t index=0;index<set->count;index++)if(set->entries[index].type_index==type_index)return 1;
    return 0;
}

/** @brief Parsed `added` or `removed` line of the diagnostics' output. */
typedef struct delta_line_t {
    int added;                       /**< Nonzero for an added line, zero for removed. */
    unsigned long long value;        /**< Handle value. */
    unsigned long long type_index;   /**< OS object type index. */
    char type[HandleDiagNameChars];  /**< Type name as printed. */
    unsigned long long access;       /**< Granted access mask. */
} delta_line_t;
/** @brief All delta lines of one capture, in output order. */
typedef struct delta_list_t {
    size_t count;                    /**< Lines parsed. */
    size_t added;                    /**< Of which added. */
    size_t removed;                  /**< Of which removed. */
    delta_line_t lines[NativeMaxDeltaLines]; /**< The lines. */
} delta_list_t;
/** @brief Parsed header and summary of a full failure report. */
typedef struct report_t {
    unsigned long long lifetime, iteration_index, baseline, samples, minimum, maximum, last; /**< Header fields. */
    unsigned long long check_start_ms, elapsed_ms;                                           /**< Header timing. */
    unsigned long long baseline_handles, current_handles, added, removed;                    /**< Summary fields. */
} report_t;
/** @brief The delta lines parsed by the latest parse_deltas or parse_report. */
static delta_list_t deltas;
/** @brief Capture text buffer shared by the cases that read a capture back. */
static char capture_text[NativeCaptureBytes];

/** @brief Consume a literal prefix. @return Cursor after text, or NULL if cursor does not start with it. */
static const char *take_text(const char *cursor,const char *text)
{
    const size_t length=strlen(text);
    return strncmp(cursor,text,length)==0 ? cursor+length : NULL;
}
/** @brief Consume one or more decimal digits. @return Cursor after them, or NULL on none or overflow. */
static const char *take_decimal(const char *cursor,unsigned long long *value)
{
    if(*cursor<'0' || *cursor>'9')return NULL;
    unsigned long long result=0;
    while(*cursor>='0' && *cursor<='9'){
        const unsigned digit=(unsigned)(*cursor-'0');
        if(result>(ULLONG_MAX-digit)/10)return NULL;
        result=result*10+digit;cursor++;
    }
    *value=result;
    return cursor;
}
/** @brief Consume one or more lowercase hexadecimal digits.
 * @param[out] digits Receives the digit count. @return Cursor after them, or NULL on none or overflow.
 */
static const char *take_hex(const char *cursor,unsigned long long *value,unsigned *digits)
{
    unsigned long long result=0;unsigned count=0;
    for(;;cursor++){
        unsigned digit;
        if(*cursor>='0' && *cursor<='9')digit=(unsigned)(*cursor-'0');
        else if(*cursor>='a' && *cursor<='f')digit=(unsigned)(*cursor-'a'+10);
        else break;
        if(count>=16)return NULL;
        result=result*16+digit;count++;
    }
    if(!count)return NULL;
    *value=result;*digits=count;
    return cursor;
}
/** @brief Parse `handle_diag: <added|removed> value=0x<hex> type_index=<n> type=<name> access=0x<8 hex>`.
 * @param[in] line NUL-terminated line without its newline. @param[out] out Receives the fields.
 * @return One when the line has exactly that shape, else zero.
 */
static int parse_delta_line(const char *line,delta_line_t *out)
{
    const char *cursor=take_text(line,"handle_diag: ");
    if(!cursor)return 0;
    const char *after=take_text(cursor,"added value=0x");
    if(after)out->added=1;
    else{after=take_text(cursor,"removed value=0x");if(!after)return 0;out->added=0;}
    unsigned digits=0;
    cursor=take_hex(after,&out->value,&digits);if(!cursor)return 0;
    cursor=take_text(cursor," type_index=");if(!cursor)return 0;
    cursor=take_decimal(cursor,&out->type_index);if(!cursor)return 0;
    cursor=take_text(cursor," type=");if(!cursor)return 0;
    const char *access_at=NULL;
    for(const char *probe=cursor;(probe=strstr(probe," access=0x"))!=NULL;probe++)access_at=probe;
    if(!access_at || access_at==cursor)return 0;
    const size_t name_length=(size_t)(access_at-cursor);
    if(name_length>=sizeof out->type)return 0;
    memcpy(out->type,cursor,name_length);out->type[name_length]=0;
    cursor=take_hex(access_at+strlen(" access=0x"),&out->access,&digits);
    return cursor && digits==8 && *cursor==0;
}
/** @brief Copy the next newline-terminated line out of text.
 * @param[in,out] cursor Position in text; advanced past the line. @param[out] line Receives it without the newline.
 * @return One for a line, zero at the end of text, minus one for an unterminated or oversized line.
 */
static int next_line(const char **cursor,char *line,size_t capacity)
{
    const char *start=*cursor;
    if(!*start)return 0;
    const char *end=strchr(start,'\n');
    if(!end)return -1;
    const size_t length=(size_t)(end-start);
    if(length>=capacity)return -1;
    memcpy(line,start,length);line[length]=0;
    *cursor=end+1;
    return 1;
}
/** @brief Append a parsed delta line to the global list. @return Zero on success, one if the list is full. */
static int add_delta(const delta_line_t *line)
{
    if(deltas.count>=NativeMaxDeltaLines)return 1;
    deltas.lines[deltas.count++]=*line;
    if(line->added)deltas.added++;else deltas.removed++;
    return 0;
}
/** @brief Parse a capture that must consist of delta lines only, added lines before removed lines.
 * @param[in] text Capture text. @return Zero on success; one after recording the offending line.
 */
static int parse_deltas(const char *text)
{
    memset(&deltas,0,sizeof deltas);
    const char *cursor=text;char line[NativeLineBytes];int more;int seen_removed=0;
    while((more=next_line(&cursor,line,sizeof line))>0){
        delta_line_t parsed;
        if(!parse_delta_line(line,&parsed)){record_failure(__LINE__,"output line is not a delta line: [%s]",line);return 1;}
        if(parsed.added && seen_removed){record_failure(__LINE__,"an added line follows a removed line: [%s]",line);return 1;}
        if(!parsed.added)seen_removed=1;
        if(add_delta(&parsed)){record_failure(__LINE__,"more than %d delta lines",(int)NativeMaxDeltaLines);return 1;}
    }
    if(more<0){record_failure(__LINE__,"output ends in an unterminated or oversized line");return 1;}
    return 0;
}
/** @brief Parse a full failure report: header, delta lines (added first), summary.
 * @param[in] text Capture text. @param[out] report Receives the header and summary numbers.
 * @return Zero on success; one after recording the offending line.
 * @details The header and the summary must have exactly the documented shape; numbers are matched by pattern.
 */
static int parse_report(const char *text,report_t *report)
{
    memset(&deltas,0,sizeof deltas);memset(report,0,sizeof *report);
    const char *cursor=text;char line[NativeLineBytes];int more;int state=0;int seen_removed=0;
    while((more=next_line(&cursor,line,sizeof line))>0){
        if(state==2){record_failure(__LINE__,"output continues after the summary line: [%s]",line);return 1;}
        if(state==0){
            const char *at=take_text(line,"handle_diag: retirement failed lifetime=");
            if(at)at=take_decimal(at,&report->lifetime);
            if(at)at=take_text(at," iteration_index=");
            if(at)at=take_decimal(at,&report->iteration_index);
            if(at)at=take_text(at," baseline=");
            if(at)at=take_decimal(at,&report->baseline);
            if(at)at=take_text(at," samples=");
            if(at)at=take_decimal(at,&report->samples);
            if(at)at=take_text(at," min=");
            if(at)at=take_decimal(at,&report->minimum);
            if(at)at=take_text(at," max=");
            if(at)at=take_decimal(at,&report->maximum);
            if(at)at=take_text(at," last=");
            if(at)at=take_decimal(at,&report->last);
            if(at)at=take_text(at," check_start_ms=");
            if(at)at=take_decimal(at,&report->check_start_ms);
            if(at)at=take_text(at," elapsed_ms=");
            if(at)at=take_decimal(at,&report->elapsed_ms);
            if(!at || *at){record_failure(__LINE__,"first line is not the retirement header: [%s]",line);return 1;}
            state=1;continue;
        }
        const char *summary=take_text(line,"handle_diag: snapshot baseline_handles=");
        if(summary){
            summary=take_decimal(summary,&report->baseline_handles);
            if(summary)summary=take_text(summary," current_handles=");
            if(summary)summary=take_decimal(summary,&report->current_handles);
            if(summary)summary=take_text(summary," added=");
            if(summary)summary=take_decimal(summary,&report->added);
            if(summary)summary=take_text(summary," removed=");
            if(summary)summary=take_decimal(summary,&report->removed);
            if(!summary || *summary){record_failure(__LINE__,"summary line has the wrong shape: [%s]",line);return 1;}
            state=2;continue;
        }
        delta_line_t parsed;
        if(!parse_delta_line(line,&parsed)){record_failure(__LINE__,"middle line is not a delta line: [%s]",line);return 1;}
        if(parsed.added && seen_removed){record_failure(__LINE__,"an added line follows a removed line: [%s]",line);return 1;}
        if(!parsed.added)seen_removed=1;
        if(add_delta(&parsed)){record_failure(__LINE__,"more than %d delta lines",(int)NativeMaxDeltaLines);return 1;}
    }
    if(more<0){record_failure(__LINE__,"output ends in an unterminated or oversized line");return 1;}
    if(state!=2){record_failure(__LINE__,"output has no summary line (state %d)",state);return 1;}
    return 0;
}
/** @brief Find the delta line with the given handle value and label.
 * @param[in] value Handle value. @param[in] added Nonzero for added lines, zero for removed.
 * @return Borrowed line of the global list, or NULL.
 */
static const delta_line_t *find_delta(unsigned long long value,int added)
{
    for(size_t index=0;index<deltas.count;index++)
        if(deltas.lines[index].value==value && (deltas.lines[index].added!=0)==(added!=0))return &deltas.lines[index];
    return NULL;
}

/** @brief Start routine of the suspended thread; it never runs because the thread is terminated first. */
static DWORD WINAPI native_thread_entry(LPVOID parameter)
{
    (void)parameter;
    return 0;
}
/** @brief Expected facts about one object the added_types case creates. */
typedef struct object_spec_t {
    const char *label;      /**< API that creates the object, for messages. */
    const char *type_name;  /**< NT object type name the snapshot must report, case-sensitive. */
    ULONG exact_access;     /**< Documented all-access mask, or zero when only nonzero is asserted. */
} object_spec_t;
/** @brief The eight objects, in creation order. */
static const object_spec_t ObjectSpecs[NativeObjectKinds]={
    {"CreateEventW","Event",NativeEventAllAccess},
    {"CreateSemaphoreW","Semaphore",NativeSemaphoreAllAccess},
    {"CreateMutexW","Mutant",NativeMutantAllAccess},
    {"CreateWaitableTimerW","Timer",NativeTimerAllAccess},
    {"CreateFileW(NUL)","File",0},
    {"CreateFileMappingW(4096)","Section",0},
    {"CreateJobObjectW","Job",0},
    {"CreateThread(suspended)+TerminateThread","Thread",0}
};
/** @brief Create the object of one kind, unnamed and owned by the caller.
 * @param[in] kind Index into ObjectSpecs. @return New handle, or NULL with GetLastError set.
 */
static HANDLE create_object(unsigned kind)
{
    switch(kind){
    case 0:return CreateEventW(NULL,TRUE,FALSE,NULL);
    case 1:return CreateSemaphoreW(NULL,0,1,NULL);
    case 2:return CreateMutexW(NULL,FALSE,NULL);
    case 3:return CreateWaitableTimerW(NULL,TRUE,NULL);
    case 4:{
        HANDLE file=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
        return file==INVALID_HANDLE_VALUE ? NULL : file;
    }
    case 5:return CreateFileMappingW(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,0,4096,NULL);
    case 6:return CreateJobObjectW(NULL,NULL);
    case 7:{
        HANDLE thread=CreateThread(NULL,0,native_thread_entry,NULL,CREATE_SUSPENDED,NULL);
        if(thread && !TerminateThread(thread,0)){
            const DWORD error=GetLastError();
            CloseHandle(thread);SetLastError(error);
            return NULL;
        }
        return thread;
    }
    default:return NULL;
    }
}
/** @brief Close one owned handle and NULL it; a failed close is recorded.
 * @param[in,out] handle Owned handle or NULL. @param[in] label Name for the message.
 * @return Zero when there was nothing to close or the close succeeded.
 */
static int close_owned(HANDLE *handle,const char *label)
{
    if(!*handle)return 0;
    const BOOL closed=CloseHandle(*handle);
    const DWORD error=GetLastError();
    *handle=NULL;
    if(closed)return 0;
    record_failure(__LINE__,"CloseHandle(%s) failed, error %lu",label,(unsigned long)error);
    return 1;
}
/** @brief Close the eight objects of added_types and NULL them.
 * @param[in,out] handles Array of NativeObjectKinds handles. @return Zero when every close succeeded.
 */
static int close_objects(HANDLE *handles)
{
    int failures=0;
    for(unsigned kind=0;kind<NativeObjectKinds;kind++)failures+=close_owned(&handles[kind],ObjectSpecs[kind].label);
    return failures!=0;
}

/** @brief Case layout_sanity: the class 51 snapshot matches GetProcessHandleCount and has a sane entry layout.
 * @details The kernel handle table can change between the two calls (OS worker threads, ALPC or RPC
 * activity), so the two numbers are compared five times back to back: every difference must be at
 * most 3 and at least one comparison must be exact. Every entry must have a nonzero 4-aligned value
 * and a plausible type index, some entry a nonzero access, and the valid standard handles, with their
 * duplicates, must be present with matching type and access.
 */
static int case_layout_sanity(void)
{
    int result=0;
    handle_diag_t diag;memset(&diag,0,sizeof diag);
    handle_diag_set_t set;memset(&set,0,sizeof set);
    HANDLE duplicates[3]={NULL,NULL,NULL};
    capture_t output;memset(&output,0,sizeof output);
    const DWORD start=process_handles();
    require(start!=0,"GetProcessHandleCount failed, error %lu",(unsigned long)GetLastError());
    handle_diag_begin(&diag);
    require(diag.query_process!=NULL && diag.query_object!=NULL,"ntdll exports unresolved: NtQueryInformationProcess %s, NtQueryObject %s",
        diag.query_process ? "found" : "missing",diag.query_object ? "found" : "missing");
    require(diag.baseline.unavailable[0]==0,"baseline snapshot unavailable: %s",diag.baseline.unavailable);
    require(diag.baseline.count>0,"baseline snapshot is empty");
    require(diag.frequency>0,"QueryPerformanceFrequency was unavailable");
    unsigned exact=0;unsigned long long worst=0;
    for(unsigned run=0;run<NativeStableRuns;run++){      /* OS threads may change handles between the two calls */
        const DWORD counted=process_handles();
        require(counted!=0,"GetProcessHandleCount failed, error %lu",(unsigned long)GetLastError());
        require(handle_diag_capture(&diag,&set)==0,"capture %u unavailable: %s",run,set.unavailable);
        require(set.count>0,"capture %u returned an empty snapshot",run);
        const unsigned long long snapshot=set.count;
        const unsigned long long difference=snapshot>counted ? snapshot-counted : counted-snapshot;
        if(difference==0)exact++;
        if(difference>worst)worst=difference;
        require(difference<=3,"run %u: snapshot has %llu handles but GetProcessHandleCount says %lu",run,snapshot,(unsigned long)counted);
    }
    require(exact>=1,"no run matched GetProcessHandleCount exactly; worst difference %llu",worst);
    native_info("layout_sanity: %u of %d runs matched exactly, worst difference %llu, %llu entries",exact,(int)NativeStableRuns,worst,(unsigned long long)set.count);
    unsigned granted=0;
    for(size_t index=0;index<set.count;index++){
        const handle_diag_entry_t *entry=&set.entries[index];
        require(entry->handle_value!=0 && (entry->handle_value&3u)==0,"entry %llu has handle value 0x%llx, expected nonzero and 4-aligned",
            (unsigned long long)index,(unsigned long long)entry->handle_value);
        require(entry->type_index<256,"entry %llu (handle 0x%llx) has type index %lu",(unsigned long long)index,
            (unsigned long long)entry->handle_value,(unsigned long)entry->type_index);
        if(entry->granted_access)granted++;
    }
    require(granted>0,"no snapshot entry has a nonzero granted access");
    static const struct standard_handle_t { DWORD id; const char *name; } standards[3]={
        {STD_INPUT_HANDLE,"stdin"},{STD_OUTPUT_HANDLE,"stdout"},{STD_ERROR_HANDLE,"stderr"}};
    unsigned checked=0;
    for(unsigned which=0;which<3;which++){
        HANDLE original=GetStdHandle(standards[which].id);
        if(original==NULL || original==INVALID_HANDLE_VALUE)continue;
        SetLastError(NO_ERROR);
        if(GetFileType(original)==FILE_TYPE_UNKNOWN && GetLastError()!=NO_ERROR)continue;   /* not a live handle */
        checked++;
        const handle_diag_entry_t *entry=find_entry(&set,(uintptr_t)original);
        require(entry!=NULL,"%s handle 0x%llx is missing from the snapshot of %llu handles",standards[which].name,
            (unsigned long long)(uintptr_t)original,(unsigned long long)set.count);
        const ULONG type_index=entry->type_index,access=entry->granted_access;
        if(!DuplicateHandle(GetCurrentProcess(),original,GetCurrentProcess(),&duplicates[which],0,FALSE,DUPLICATE_SAME_ACCESS)){
            native_info("layout_sanity: DuplicateHandle(%s) failed, error %lu; duplicate comparison skipped",standards[which].name,(unsigned long)GetLastError());
            duplicates[which]=NULL;
            continue;
        }
        require(handle_diag_capture(&diag,&set)==0,"capture after duplicating %s unavailable: %s",standards[which].name,set.unavailable);
        const handle_diag_entry_t *copy=find_entry(&set,(uintptr_t)duplicates[which]);
        require(copy!=NULL,"duplicate 0x%llx of %s is missing from the snapshot",(unsigned long long)(uintptr_t)duplicates[which],standards[which].name);
        require(copy->type_index==type_index && copy->granted_access==access,
            "duplicate of %s reports type %lu access 0x%08lx, original type %lu access 0x%08lx",standards[which].name,
            (unsigned long)copy->type_index,(unsigned long)copy->granted_access,(unsigned long)type_index,(unsigned long)access);
        require(find_entry(&set,(uintptr_t)original)!=NULL,"%s handle disappeared while duplicated",standards[which].name);
        require(close_owned(&duplicates[which],"a duplicated standard handle")==0,"a duplicated standard handle could not be closed");
    }
    if(!checked)native_info("layout_sanity: no valid standard handle; the stdin/stdout/stderr check had nothing to test");
    {                                                     /* the baseline line the fixture prints, over the real counts */
        const DWORD reported=process_handles();
        char expected[NativeLineBytes];
        require(reported!=0,"GetProcessHandleCount failed, error %lu",(unsigned long)GetLastError());
        if(capture_open(&output,&native_capture_output)!=0)native_info("layout_sanity: no temp file (error %lu); the describe_baseline text check was skipped",(unsigned long)GetLastError());
        else{
            handle_diag_describe_baseline(&diag,reported);
            require(capture_read(&output,capture_text,sizeof capture_text)==0,"cannot read the capture back");
            snprintf(expected,sizeof expected,"handle_diag: baseline snapshot handles=%llu process_handle_count=%lu\n",
                (unsigned long long)diag.baseline.count,(unsigned long)reported);
            require(strcmp(capture_text,expected)==0,"describe_baseline printed [%s], expected [%s]",capture_text,expected);
        }
    }
cleanup:
    capture_close(&output);
    for(unsigned which=0;which<3;which++)if(close_owned(&duplicates[which],"a duplicated standard handle"))result=1;
    handle_diag_set_free(&set);
    handle_diag_free(&diag);
    if(!result)result=check_balance(start);
    return result;
}

/** @brief Case passive: captures and type-name lookups open no handle. */
static int case_passive(void)
{
    int result=0;
    handle_diag_t diag;memset(&diag,0,sizeof diag);
    handle_diag_set_t set;memset(&set,0,sizeof set);
    ULONG distinct[64];size_t distinct_count=0;
    handle_diag_begin(&diag);
    require(diag.baseline.unavailable[0]==0,"baseline snapshot unavailable: %s",diag.baseline.unavailable);
    require(handle_diag_capture(&diag,&set)==0,"capture unavailable: %s",set.unavailable);
    for(size_t index=0;index<set.count && distinct_count<sizeof distinct/sizeof distinct[0];index++){
        size_t seen=0;
        while(seen<distinct_count && distinct[seen]!=set.entries[index].type_index)seen++;
        if(seen==distinct_count)distinct[distinct_count++]=set.entries[index].type_index;
    }
    require(distinct_count>0,"the snapshot has no entries to look up");
    const DWORD before=process_handles();
    require(before!=0,"GetProcessHandleCount failed, error %lu",(unsigned long)GetLastError());
    for(unsigned round=0;round<NativeStressRounds;round++)
        require(handle_diag_capture(&diag,&set)==0,"capture %u unavailable: %s",round,set.unavailable);
    unsigned resolved=0;
    for(unsigned round=0;round<NativeStressRounds;round++){
        char name[HandleDiagNameChars];
        handle_diag_type_name(&diag,&set,distinct[round%distinct_count],name,sizeof name);
        if(strcmp(name,"unknown")!=0)resolved++;
    }
    DWORD after=0;
    require(settle_handles(before,&after),"%d captures and %d type-name lookups changed the handle count from %lu to %lu",
        (int)NativeStressRounds,(int)NativeStressRounds,(unsigned long)before,(unsigned long)after);
    require(resolved*10>=NativeStressRounds*9,"only %u of %d type-name lookups resolved a name; the loop proved little",resolved,(int)NativeStressRounds);
    native_info("passive: %u of %d lookups resolved over %llu distinct types",resolved,(int)NativeStressRounds,(unsigned long long)distinct_count);
cleanup:
    handle_diag_set_free(&set);
    handle_diag_free(&diag);
    return result;
}

/** @brief One attempt of added_types: eight new handles of eight types are listed exactly as added.
 * @return Zero on success; one after recording the failure, with failure_is_noise set when the only
 * discrepancy is added handles that are not ours.
 */
static int added_types_attempt(void)
{
    int result=0;
    capture_t capture;memset(&capture,0,sizeof capture);
    handle_diag_t diag;memset(&diag,0,sizeof diag);
    handle_diag_set_t second;memset(&second,0,sizeof second);
    HANDLE handles[NativeObjectKinds];memset(handles,0,sizeof handles);
    failure_is_noise=0;
    const DWORD start=process_handles();
    require(start!=0,"GetProcessHandleCount failed, error %lu",(unsigned long)GetLastError());
    require(capture_open(&capture,&native_capture_stream)==0,"cannot create the temp capture file, error %lu",(unsigned long)GetLastError());
    handle_diag_begin(&diag);
    require(diag.baseline.unavailable[0]==0,"baseline snapshot unavailable: %s",diag.baseline.unavailable);
    for(unsigned kind=0;kind<NativeObjectKinds;kind++){
        handles[kind]=create_object(kind);
        require(handles[kind]!=NULL,"%s failed, error %lu",ObjectSpecs[kind].label,(unsigned long)GetLastError());
    }
    require(handle_diag_capture(&diag,&second)==0,"second snapshot unavailable: %s",second.unavailable);
    const size_t added_printed=handle_diag_print_delta(&diag,"added",&second,&diag.baseline,&second);
    const size_t removed_printed=handle_diag_print_delta(&diag,"removed",&diag.baseline,&second,&second);
    require(capture_read(&capture,capture_text,sizeof capture_text)==0,"cannot read the capture back");
    require(parse_deltas(capture_text)==0,"the delta output is malformed");
    require(deltas.added==added_printed && deltas.removed==removed_printed,"print_delta returned %llu added and %llu removed but %llu and %llu lines were parsed",
        (unsigned long long)added_printed,(unsigned long long)removed_printed,(unsigned long long)deltas.added,(unsigned long long)deltas.removed);
    for(unsigned kind=0;kind<NativeObjectKinds;kind++){
        const unsigned long long value=(unsigned long long)(uintptr_t)handles[kind];
        const delta_line_t *line=find_delta(value,1);
        require(line!=NULL,"%s handle 0x%llx is not listed as added",ObjectSpecs[kind].label,value);
        require(strcmp(line->type,ObjectSpecs[kind].type_name)==0,"%s handle 0x%llx has type name '%s', expected '%s' (type_index %llu)",
            ObjectSpecs[kind].label,value,line->type,ObjectSpecs[kind].type_name,line->type_index);
        require(line->access!=0,"%s handle 0x%llx has granted access 0",ObjectSpecs[kind].label,value);
        require(!ObjectSpecs[kind].exact_access || line->access==ObjectSpecs[kind].exact_access,
            "%s handle 0x%llx has granted access 0x%08llx, expected 0x%08lx",ObjectSpecs[kind].label,value,line->access,(unsigned long)ObjectSpecs[kind].exact_access);
        require(find_delta(value,0)==NULL,"%s handle 0x%llx is also listed as removed",ObjectSpecs[kind].label,value);
        require(find_entry(&diag.baseline,(uintptr_t)handles[kind])==NULL && find_entry(&second,(uintptr_t)handles[kind])!=NULL,
            "%s handle 0x%llx is not new in the snapshots",ObjectSpecs[kind].label,value);
    }
    require(second.count+deltas.removed==diag.baseline.count+deltas.added,"snapshots hold %llu and %llu handles but %llu were added and %llu removed",
        (unsigned long long)diag.baseline.count,(unsigned long long)second.count,(unsigned long long)deltas.added,(unsigned long long)deltas.removed);
    for(size_t index=0;index<deltas.count;index++){      /* last: anything else added is not ours */
        const delta_line_t *line=&deltas.lines[index];
        if(!line->added)continue;
        int ours=0;
        for(unsigned kind=0;kind<NativeObjectKinds;kind++)if(line->value==(unsigned long long)(uintptr_t)handles[kind])ours=1;
        if(!ours){
            failure_is_noise=1;
            record_failure(__LINE__,"unexpected added handle 0x%llx type '%s' access 0x%08llx besides the eight created",line->value,line->type,line->access);
            result=1;goto cleanup;
        }
    }
cleanup:
    if(close_objects(handles))result=1;
    handle_diag_set_free(&second);
    handle_diag_free(&diag);
    capture_close(&capture);
    if(!result)result=check_balance(start);
    return result;
}
/** @brief Case added_types: see added_types_attempt; repeated only when the sole problem is OS noise. */
static int case_added_types(void)
{
    for(unsigned attempt=1;attempt<=NativeAddedAttempts;attempt++){
        failure_reason[0]=0;
        const int result=added_types_attempt();
        if(!result)return 0;
        if(!failure_is_noise || attempt==NativeAddedAttempts)return 1;
        native_info("added_types attempt %u saw unrelated added handles (%s); repeating",attempt,failure_reason);
    }
    return 1;
}

/** @brief Case removed: a handle closed between two snapshots is listed as removed with its type. */
static int case_removed(void)
{
    int result=0;
    capture_t capture;memset(&capture,0,sizeof capture);
    handle_diag_t diag;memset(&diag,0,sizeof diag);
    handle_diag_set_t second;memset(&second,0,sizeof second);
    HANDLE witness=NULL,target=NULL;
    const DWORD start=process_handles();
    require(start!=0,"GetProcessHandleCount failed, error %lu",(unsigned long)GetLastError());
    require(capture_open(&capture,&native_capture_stream)==0,"cannot create the temp capture file, error %lu",(unsigned long)GetLastError());
    witness=CreateEventW(NULL,TRUE,FALSE,NULL);     /* stays open: a removed handle is named through a live one of its type */
    require(witness!=NULL,"CreateEventW(witness) failed, error %lu",(unsigned long)GetLastError());
    target=CreateEventW(NULL,TRUE,FALSE,NULL);
    require(target!=NULL,"CreateEventW(target) failed, error %lu",(unsigned long)GetLastError());
    handle_diag_begin(&diag);
    require(diag.baseline.unavailable[0]==0,"baseline snapshot unavailable: %s",diag.baseline.unavailable);
    const unsigned long long value=(unsigned long long)(uintptr_t)target;
    require(find_entry(&diag.baseline,(uintptr_t)target)!=NULL,"the event 0x%llx is missing from the baseline snapshot",value);
    {
        HANDLE closing=target;target=NULL;   /* one CloseHandle attempt: cleanup must never close it again */
        require(CloseHandle(closing),"CloseHandle(event) failed, error %lu",(unsigned long)GetLastError());
    }
    require(handle_diag_capture(&diag,&second)==0,"second snapshot unavailable: %s",second.unavailable);
    handle_diag_print_delta(&diag,"added",&second,&diag.baseline,&second);
    handle_diag_print_delta(&diag,"removed",&diag.baseline,&second,&second);
    require(capture_read(&capture,capture_text,sizeof capture_text)==0,"cannot read the capture back");
    require(parse_deltas(capture_text)==0,"the delta output is malformed");
    const delta_line_t *line=find_delta(value,0);
    require(line!=NULL,"closed event 0x%llx is not listed as removed (%llu removed lines)",value,(unsigned long long)deltas.removed);
    require(strcmp(line->type,"Event")==0,"closed event 0x%llx has type name '%s', expected 'Event' (type_index %llu)",value,line->type,line->type_index);
    require(line->access!=0,"closed event 0x%llx has granted access 0",value);
    require(line->access==NativeEventAllAccess,"closed event 0x%llx has granted access 0x%08llx, expected 0x%08lx",value,line->access,(unsigned long)NativeEventAllAccess);
    require(find_delta(value,1)==NULL,"closed event 0x%llx is also listed as added",value);
    require(find_entry(&second,(uintptr_t)value)==NULL,"closed event 0x%llx is still in the second snapshot",value);
cleanup:
    if(target){CloseHandle(target);target=NULL;}
    if(witness){CloseHandle(witness);witness=NULL;}
    handle_diag_set_free(&second);
    handle_diag_free(&diag);
    capture_close(&capture);
    if(!result)result=check_balance(start);
    return result;
}

/** @brief Case failure_report_shape: handle_diag_report prints the documented header, delta lines and summary. */
static int case_failure_report_shape(void)
{
    int result=0;
    capture_t capture;memset(&capture,0,sizeof capture);
    handle_diag_t diag;memset(&diag,0,sizeof diag);
    HANDLE event=NULL;
    report_t report;memset(&report,0,sizeof report);
    handle_check_t check;memset(&check,0,sizeof check);
    const DWORD start=process_handles();
    require(start!=0,"GetProcessHandleCount failed, error %lu",(unsigned long)GetLastError());
    require(capture_open(&capture,&native_capture_stream)==0,"cannot create the temp capture file, error %lu",(unsigned long)GetLastError());
    handle_check_record(&check,200);handle_check_record(&check,199);handle_check_record(&check,200);   /* three samples, none equal to 199 at the end */
    require(check.samples==3 && check.minimum==199 && check.maximum==200 && check.last==200,"handle_check_record produced samples=%u min=%lu max=%lu last=%lu, expected 3 199 200 200",
        check.samples,(unsigned long)check.minimum,(unsigned long)check.maximum,(unsigned long)check.last);
    handle_diag_begin(&diag);
    require(diag.baseline.unavailable[0]==0,"baseline snapshot unavailable: %s",diag.baseline.unavailable);
    event=CreateEventW(NULL,TRUE,FALSE,NULL);        /* guarantees one real added line between the two snapshots */
    require(event!=NULL,"CreateEventW failed, error %lu",(unsigned long)GetLastError());
    handle_diag_report(&diag,199,&check,4,123);
    require(capture_read(&capture,capture_text,sizeof capture_text)==0,"cannot read the capture back");
    require(parse_report(capture_text,&report)==0,"the report is malformed");
    require(report.lifetime==5 && report.iteration_index==4,"header says lifetime=%llu iteration_index=%llu, expected 5 and 4",report.lifetime,report.iteration_index);
    require(report.baseline==199 && report.samples==3 && report.minimum==199 && report.maximum==200 && report.last==200,
        "header says baseline=%llu samples=%llu min=%llu max=%llu last=%llu, expected 199 3 199 200 200",
        report.baseline,report.samples,report.minimum,report.maximum,report.last);
    require(report.check_start_ms==123,"header says check_start_ms=%llu, expected 123",report.check_start_ms);
    require(report.elapsed_ms<60000,"header says elapsed_ms=%llu for a report taken immediately",report.elapsed_ms);
    require(report.baseline_handles==diag.baseline.count,"summary says baseline_handles=%llu, the baseline snapshot has %llu",
        report.baseline_handles,(unsigned long long)diag.baseline.count);
    require(report.added==deltas.added && report.removed==deltas.removed,"summary says added=%llu removed=%llu but %llu and %llu delta lines were printed",
        report.added,report.removed,(unsigned long long)deltas.added,(unsigned long long)deltas.removed);
    require(report.current_handles+report.removed==report.baseline_handles+report.added,
        "summary is inconsistent: baseline_handles=%llu current_handles=%llu added=%llu removed=%llu",
        report.baseline_handles,report.current_handles,report.added,report.removed);
    {
        const unsigned long long value=(unsigned long long)(uintptr_t)event;
        const delta_line_t *line=find_delta(value,1);
        require(line!=NULL,"new event 0x%llx is not listed as added",value);
        require(strcmp(line->type,"Event")==0,"new event 0x%llx has type name '%s', expected 'Event'",value,line->type);
    }
cleanup:
    if(event){CloseHandle(event);event=NULL;}
    handle_diag_free(&diag);
    capture_close(&capture);
    if(!result)result=check_balance(start);
    return result;
}

/** @brief Case type_name_unknown: an absent type index names `unknown`, an Event's index names `Event`. */
static int case_type_name_unknown(void)
{
    int result=0;
    handle_diag_t diag;memset(&diag,0,sizeof diag);
    handle_diag_set_t set;memset(&set,0,sizeof set);
    HANDLE event=NULL;
    char name[HandleDiagNameChars];
    const DWORD start=process_handles();
    require(start!=0,"GetProcessHandleCount failed, error %lu",(unsigned long)GetLastError());
    event=CreateEventW(NULL,TRUE,FALSE,NULL);
    require(event!=NULL,"CreateEventW failed, error %lu",(unsigned long)GetLastError());
    handle_diag_begin(&diag);
    require(diag.baseline.unavailable[0]==0,"baseline snapshot unavailable: %s",diag.baseline.unavailable);
    require(handle_diag_capture(&diag,&set)==0,"capture unavailable: %s",set.unavailable);
    ULONG absent=256;
    for(ULONG candidate=255;candidate>0;candidate--)if(!type_present(&set,candidate)){absent=candidate;break;}
    require(absent<256,"every type index 1..255 is present in the snapshot");
    handle_diag_type_name(&diag,&set,absent,name,sizeof name);
    require(strcmp(name,"unknown")==0,"absent type index %lu resolved to '%s', expected 'unknown'",(unsigned long)absent,name);
    const handle_diag_entry_t *entry=find_entry(&set,(uintptr_t)event);
    require(entry!=NULL,"the event 0x%llx is missing from the snapshot",(unsigned long long)(uintptr_t)event);
    handle_diag_type_name(&diag,&set,entry->type_index,name,sizeof name);
    require(strcmp(name,"Event")==0,"type index %lu of the event resolved to '%s', expected 'Event'",(unsigned long)entry->type_index,name);
cleanup:
    if(event){CloseHandle(event);event=NULL;}
    handle_diag_set_free(&set);
    handle_diag_free(&diag);
    if(!result)result=check_balance(start);
    return result;
}

/** @brief Case timing: handle_diag_now_ms is monotonic and measures a Sleep(50) plausibly. */
static int case_timing(void)
{
    int result=0;
    handle_diag_t diag;memset(&diag,0,sizeof diag);
    capture_t output;memset(&output,0,sizeof output);
    handle_check_t check;memset(&check,0,sizeof check);
    char expected[NativeLineBytes];
    const DWORD start=process_handles();
    require(start!=0,"GetProcessHandleCount failed, error %lu",(unsigned long)GetLastError());
    handle_diag_begin(&diag);
    require(diag.frequency>0,"QueryPerformanceFrequency was unavailable");
    unsigned long long previous=handle_diag_now_ms(&diag);
    for(unsigned sample=0;sample<1000;sample++){
        const unsigned long long now=handle_diag_now_ms(&diag);
        require(now>=previous,"sample %u went backwards: %llu after %llu",sample,now,previous);
        previous=now;
    }
    const unsigned long long before=handle_diag_now_ms(&diag);
    Sleep(50);
    const unsigned long long after=handle_diag_now_ms(&diag);
    require(after>=before,"clock went backwards over Sleep(50): %llu after %llu",after,before);
    require(after-before>=40 && after-before<5000,"Sleep(50) measured %llu ms, expected at least 40 and below 5000",after-before);
    handle_check_record(&check,200);handle_check_record(&check,200);handle_check_record(&check,199);
    if(capture_open(&output,&native_capture_output)!=0)native_info("timing: no temp file (error %lu); the iteration text check was skipped",(unsigned long)GetLastError());
    else{
        handle_diag_iteration(7,&check,previous,before,after);   /* the per-lifetime stdout line, over measured times */
        require(capture_read(&output,capture_text,sizeof capture_text)==0,"cannot read the capture back");
        snprintf(expected,sizeof expected,"handle_diag: lifetime=8 iteration_index=7 samples=3 count=199 start_ms=%llu check_start_ms=%llu check_done_ms=%llu\n",previous,before,after);
        require(strcmp(capture_text,expected)==0,"handle_diag_iteration printed [%s], expected [%s]",capture_text,expected);
    }
cleanup:
    capture_close(&output);
    handle_diag_free(&diag);
    if(!result)result=check_balance(start);
    return result;
}

/** @brief One named case of the table in main. */
typedef struct case_entry_t {
    const char *name;           /**< Case name printed on its result line. */
    int (*body)(void);          /**< Case body: zero on pass, one on failure with failure_reason set. */
} case_entry_t;

/** @brief Run each API and the diagnostics once so first-use OS initialization is not blamed on a case.
 * @details Not a case: failures only print INFO lines, and nothing is asserted.
 */
static void warm_up(void)
{
    capture_t capture;memset(&capture,0,sizeof capture);
    handle_diag_t diag;memset(&diag,0,sizeof diag);
    handle_diag_set_t set;memset(&set,0,sizeof set);
    HANDLE handles[NativeObjectKinds];memset(handles,0,sizeof handles);
    char name[HandleDiagNameChars];
    if(capture_open(&capture,&native_capture_stream)!=0)native_info("warm-up: cannot create the temp capture file, error %lu",(unsigned long)GetLastError());
    handle_diag_begin(&diag);
    for(unsigned kind=0;kind<NativeObjectKinds;kind++)handles[kind]=create_object(kind);
    if(handle_diag_capture(&diag,&set)==0 && set.count){
        handle_diag_type_name(&diag,&set,set.entries[0].type_index,name,sizeof name);
        if(capture.stream){
            handle_diag_print_delta(&diag,"added",&set,&diag.baseline,&set);
            handle_diag_report(&diag,1,&(handle_check_t){1,1,1,1},0,0);
        }
    }
    (void)handle_diag_now_ms(&diag);
    for(unsigned kind=0;kind<NativeObjectKinds;kind++)if(handles[kind])CloseHandle(handles[kind]);
    handle_diag_set_free(&set);
    handle_diag_free(&diag);
    capture_close(&capture);
    Sleep(100);
}

/** @brief Run every case and report.
 * @return Zero only when every case passed and the handle count ended where the cases started.
 */
int main(void)
{
    static const case_entry_t cases[]={
        {"layout_sanity",case_layout_sanity},
        {"passive",case_passive},
        {"added_types",case_added_types},
        {"removed",case_removed},
        {"failure_report_shape",case_failure_report_shape},
        {"type_name_unknown",case_type_name_unknown},
        {"timing",case_timing},
    };
    const unsigned total=(unsigned)(sizeof cases/sizeof cases[0]);
    FILE *out=native_real_stdout();
    warm_up();
    const DWORD start=process_handles();
    if(!start){fprintf(out,"HANDLE_DIAG_NATIVE FAIL reason=GetProcessHandleCount failed, error %lu\n",(unsigned long)GetLastError());fflush(out);return 1;}
    unsigned failed=0;
    for(unsigned index=0;index<total;index++){
        failure_reason[0]=0;failure_is_noise=0;
        const int result=cases[index].body();
        if(!result)fprintf(out,"HANDLE_DIAG_NATIVE case=%s PASS\n",cases[index].name);
        else{
            fprintf(out,"HANDLE_DIAG_NATIVE case=%s FAIL reason=%s\n",cases[index].name,failure_reason[0] ? failure_reason : "unrecorded failure");
            failed++;
        }
        fflush(out);
    }
    DWORD last=0;
    const int balanced=settle_handles(start,&last);
    if(!balanced)fprintf(out,"HANDLE_DIAG_NATIVE FAIL reason=final handle count %lu differs from the starting count %lu\n",(unsigned long)last,(unsigned long)start);
    else if(failed)fprintf(out,"HANDLE_DIAG_NATIVE FAIL cases=%u failed=%u\n",total,failed);
    else fprintf(out,"HANDLE_DIAG_NATIVE PASS cases=%u\n",total);
    fflush(out);
    return (balanced && !failed) ? 0 : 1;
}
