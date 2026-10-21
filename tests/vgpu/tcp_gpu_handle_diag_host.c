/** @file tcp_gpu_handle_diag_host.c @brief Linux host unit test of the native fixture's
 * handle-retirement failure diagnostics.
 * The REAL tcp_gpu_handle_diag_windows.inc is included below over the fake Win32/NT surface in
 * win32_fake/handle_diag_fake.h and a scripted NT kernel defined here. Nothing is copied or
 * sliced out of tcp_gpu_windows.c. Build with -std=c11 -Wall -Wextra -Wpedantic -Werror, plain
 * or with -fsanitize=address,leak,undefined. This does not replace the native Windows run.
 *
 * Usage: tcp_gpu_handle_diag_host [path-to-tcp_gpu_windows.c]
 *  - With the path, the unchanged acceptance rule of the fixture (handle_baseline, native_handles,
 *    the lifetime count and the call order around the baseline sample) is also pinned by exact
 *    source text, because that rule lives in the fixture and cannot be compiled here.
 *  - WADDLE_TEST_LEAK_CONTROL=1 runs only a deliberate leak of the real baseline snapshot. A
 *    LeakSanitizer build must then exit with LSAN_OPTIONS exitcode (see make
 *    vgpu-handle-diag-sanitizers); any other build exits zero.
 *
 * Failures print the failing expression or both texts to the real stderr and exit with status 1.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "win32_fake/handle_diag_fake.h"

/** @brief Test double of symbol() in tcp_gpu_windows.c: same body, over the fake GetProcAddress.
 * @param[in] module Token from the fake GetModuleHandleA. @param[in] name Export name, borrowed.
 * @param[out] output Receives the function pointer bytes on success. @param[in] bytes Size of output.
 * @return Zero on success; one, leaving output untouched, when the export is absent or bytes
 * differs from the size of a function pointer.
 * @note The acceptance-source pin below asserts the real helper still has this exact body.
 */
static int symbol(HMODULE module,const char *name,void *output,size_t bytes)
{
    FARPROC procedure=GetProcAddress(module,name);if(!procedure || bytes!=sizeof procedure)return 1;
    memcpy(output,&procedure,bytes);return 0;
}
#include "tcp_gpu_handle_diag_windows.inc"

/* ---- Check helpers: failures go to the real stderr and end the process with status 1. ---- */

/** @brief Number of checks that passed so far. */
static unsigned checks_passed;
/** @brief Name of the running case, for failure messages. */
static const char *current_case = "";

/** @brief Report a failed check and exit.
 * @param[in] file,line Source location. @param[in] what Static description or expression text.
 * @param[in] expected,actual Optional borrowed texts to print, NULL when not applicable.
 * @details _exit skips LeakSanitizer so the first failure is not buried under leak reports.
 */
static void fail_check(const char *file,int line,const char *what,const char *expected,const char *actual)
{
    FILE *stream=handle_diag_fake_real_stderr();
    fprintf(stream,"tcp_gpu_handle_diag_host: FAIL in case %s at %s:%d: %s\n",current_case,file,line,what);
    if(expected)fprintf(stream,"--- expected ---\n%s--- actual ---\n%s--- end ---\n",expected,actual ? actual : "(null)");
    fflush(stream);_exit(1);
}
/** @brief Require a condition. @param[in] condition Nonzero when it holds. @param[in] text,file,line Location. */
static void check_condition(int condition,const char *text,const char *file,int line)
{
    if(!condition)fail_check(file,line,text,NULL,NULL);
    checks_passed++;
}
/** @brief Require two unsigned values to be equal.
 * @param[in] actual,expected Values. @param[in] text,file,line Location and expression text.
 */
static void check_unsigned(unsigned long long actual,unsigned long long expected,const char *text,const char *file,int line)
{
    if(actual!=expected){
        char message[256];
        snprintf(message,sizeof message,"%s is %llu (0x%llx), expected %llu (0x%llx)",text,actual,actual,expected,expected);
        fail_check(file,line,message,NULL,NULL);
    }
    checks_passed++;
}
/** @brief Require two texts to be byte-identical.
 * @param[in] actual,expected Nonnull NUL-terminated texts. @param[in] text,file,line Location.
 */
static void check_text(const char *actual,const char *expected,const char *text,const char *file,int line)
{
    if(strcmp(actual,expected)!=0)fail_check(file,line,text,expected,actual);
    checks_passed++;
}
/** @brief Require a condition. */
#define expect(condition) check_condition((condition)!=0,#condition,__FILE__,__LINE__)
/** @brief Require two integers to be equal as unsigned 64-bit values. */
#define expect_u(actual,expected) check_unsigned((unsigned long long)(actual),(unsigned long long)(expected),#actual,__FILE__,__LINE__)
/** @brief Require two texts to be byte-identical. */
#define expect_text(actual,expected) check_text((actual),(expected),#actual,__FILE__,__LINE__)

/* ---- Scripted NT kernel. The wire layouts below are the documented Windows ones and are
 * deliberately declared again here, independently of the .inc, so a drifting layout fails. ---- */

/** @brief Documented PROCESS_HANDLE_SNAPSHOT_INFORMATION header, x86-64. */
typedef struct nt_header_t { ULONG_PTR number_of_handles; ULONG_PTR reserved; } nt_header_t;
/** @brief Documented PROCESS_HANDLE_TABLE_ENTRY_INFO, x86-64. */
typedef struct nt_entry_t {
    HANDLE handle_value; ULONG_PTR handle_count; ULONG_PTR pointer_count;
    ULONG granted_access; ULONG object_type_index; ULONG handle_attributes; ULONG reserved;
} nt_entry_t;
/** @brief Documented leading UNICODE_STRING of OBJECT_TYPE_INFORMATION, x86-64. */
typedef struct nt_unicode_t { USHORT length; USHORT maximum_length; PWSTR buffer; } nt_unicode_t;
_Static_assert(sizeof(void *)==8 && sizeof(nt_header_t)==16 && sizeof(nt_entry_t)==40 && sizeof(nt_unicode_t)==16,
    "the scripted kernel reproduces the 64-bit Windows layouts");

/** @brief Capacities of the scripted kernel tables. */
enum { FakeTableCapacity = 2048, FakeTypeCount = 8, FakeQueriedSlots = 16 };
/** @brief Expected ABI constants, written out independently of the .inc. */
enum { ExpectedProcessHandleInformation = 51, ExpectedObjectTypeInformation = 2, ExpectedTypeBytes = 1024 };
/** @brief STATUS_INFO_LENGTH_MISMATCH. */
static const ULONG StatusInfoLengthMismatch = 0xC0000004UL;
/** @brief STATUS_INVALID_HANDLE. */
static const ULONG StatusInvalidHandle = 0xC0000008UL;
/** @brief STATUS_ACCESS_DENIED. */
static const ULONG StatusAccessDenied = 0xC0000022UL;

/** @brief One handle of the scripted process. */
typedef struct fake_handle_t {
    uintptr_t value;    /**< Handle value. */
    ULONG type;         /**< Object type index; names come from kernel.type_names. */
    ULONG access;       /**< Granted access mask. */
    int live;           /**< Zero: NtQueryObject reports STATUS_INVALID_HANDLE (closed since the snapshot). */
} fake_handle_t;
/** @brief How the scripted NtQueryObject lays out the type information. */
typedef enum object_mode_t {
    ObjectModeNormal,             /**< Valid: name at +64. */
    ObjectModeOddLength,          /**< Length is an odd byte count. */
    ObjectModeBufferInsideHeader, /**< Characters pointer lies inside the leading structure. */
    ObjectModeBufferBeforeHeader, /**< Characters pointer lies below the buffer. */
    ObjectModeBufferPastEnd,      /**< Characters pointer lies beyond the buffer. */
    ObjectModeLengthPastEnd,      /**< Characters start inside but run past the buffer end. */
    ObjectModeOversizedLength,    /**< Length 0xFFFE. */
    ObjectModeZeroLength,         /**< Empty name. */
    ObjectModeNullBuffer,         /**< NULL characters pointer with a nonzero length. */
    ObjectModeTextAtEnd,          /**< Valid: characters end exactly at the buffer end. */
    ObjectModeTextJustAfterHeader,/**< Valid: characters start exactly after the structure. */
    ObjectModeNonPrintable        /**< Valid length; characters outside 0x20..0x7e. */
} object_mode_t;
/** @brief What the scripted NtQueryInformationProcess reports as the required length on a mismatch. */
typedef enum hint_mode_t {
    HintExact,  /**< The true required size. */
    HintZero,   /**< Zero, i.e. no hint. */
    HintFixed   /**< kernel.hint_value, regardless of the truth. */
} hint_mode_t;
/** @brief All scripted NT state; reset by begin_case. */
typedef struct fake_kernel_t {
    fake_handle_t table[FakeTableCapacity]; /**< Process handle table. */
    size_t length;                          /**< Used entries of table. */
    unsigned process_calls;                 /**< NtQueryInformationProcess calls. */
    ULONG last_length;                      /**< Buffer length of the latest such call. */
    LONG forced_status;                     /**< Negative: returned at once; positive: returned after a normal reply. */
    unsigned mismatch_remaining;            /**< Leading calls answered with STATUS_INFO_LENGTH_MISMATCH. */
    int mismatch_forever;                   /**< Nonzero: every call answers STATUS_INFO_LENGTH_MISMATCH. */
    hint_mode_t hint_mode;                  /**< Required-length report on a mismatch. */
    ULONG hint_value;                       /**< Value for HintFixed. */
    int claim_active;                       /**< Nonzero: the header advertises claimed handles. */
    size_t claimed;                         /**< Advertised count; only entries that fit are written. */
    unsigned object_calls;                  /**< NtQueryObject calls. */
    object_mode_t object_mode;              /**< Layout of the type information. */
    LONG object_forced_status;              /**< Nonzero: NtQueryObject returns this at once. */
    uintptr_t queried[FakeQueriedSlots];    /**< Handle values passed to the first NtQueryObject calls. */
    const char *type_names[FakeTypeCount];  /**< Type name per type index. */
    const char *name_override;              /**< When set, used instead of type_names. */
} fake_kernel_t;
/** @brief The one scripted kernel. */
static fake_kernel_t kernel;

/** @brief Scripted NtQueryInformationProcess, class 51 only.
 * @details Verifies its arguments and never writes outside the caller's buffer.
 */
static LONG NTAPI fake_query_process(HANDLE process,ULONG information_class,PVOID buffer,ULONG length,PULONG returned)
{
    handle_diag_fake_log("nt_qip",NULL);
    kernel.process_calls++;kernel.last_length=length;
    expect(process==(HANDLE)(intptr_t)-1);
    expect(information_class==ExpectedProcessHandleInformation);
    expect(buffer!=NULL && returned!=NULL && length>=sizeof(nt_header_t));
    const size_t needed=sizeof(nt_header_t)+kernel.length*sizeof(nt_entry_t);
    ULONG hint=(ULONG)needed;
    if(kernel.hint_mode==HintZero)hint=0;
    if(kernel.hint_mode==HintFixed)hint=kernel.hint_value;
    if(kernel.forced_status<0)return kernel.forced_status;
    if(kernel.mismatch_forever || kernel.mismatch_remaining || (!kernel.claim_active && length<needed)){
        if(kernel.mismatch_remaining)kernel.mismatch_remaining--;
        *returned=hint;return (LONG)StatusInfoLengthMismatch;
    }
    const size_t capacity=(length-sizeof(nt_header_t))/sizeof(nt_entry_t);
    const size_t advertised=kernel.claim_active ? kernel.claimed : kernel.length;
    const size_t writable=advertised<capacity ? advertised : capacity;
    const nt_header_t header={advertised,0x5555};
    memcpy(buffer,&header,sizeof header);
    for(size_t index=0;index<writable;index++){
        nt_entry_t entry={0};
        if(index<kernel.length){
            entry.handle_value=(HANDLE)kernel.table[index].value;entry.granted_access=kernel.table[index].access;
            entry.object_type_index=kernel.table[index].type;
        }
        entry.handle_count=0x1111;entry.pointer_count=0x2222;entry.handle_attributes=0x33;entry.reserved=0x44;
        memcpy((unsigned char *)buffer+sizeof header+index*sizeof entry,&entry,sizeof entry);
    }
    *returned=(ULONG)(sizeof header+writable*sizeof(nt_entry_t));
    return kernel.forced_status>0 ? kernel.forced_status : 0;
}

/** @brief Characters of ObjectModeNonPrintable: three printable, four replaced, one more printable. */
static const uint16_t NonPrintableUnits[]={'A',0x263A,0x0001,0x007f,' ','~',0x00e9,'z'};

/** @brief Scripted NtQueryObject, ObjectTypeInformation only.
 * @details Verifies its arguments and never writes outside the caller's buffer.
 */
static LONG NTAPI fake_query_object(HANDLE handle,ULONG information_class,PVOID buffer,ULONG length,PULONG returned)
{
    handle_diag_fake_log("nt_qo",NULL);
    if(kernel.object_calls<FakeQueriedSlots)kernel.queried[kernel.object_calls]=(uintptr_t)handle;
    kernel.object_calls++;
    expect(information_class==ExpectedObjectTypeInformation);
    expect(returned==NULL && length==ExpectedTypeBytes && ((uintptr_t)buffer&15u)==0);
    if(kernel.object_forced_status)return kernel.object_forced_status;
    const fake_handle_t *found=NULL;
    for(size_t index=0;index<kernel.length;index++)if(kernel.table[index].value==(uintptr_t)handle)found=&kernel.table[index];
    if(!found || !found->live)return (LONG)StatusInvalidHandle;
    unsigned char *base=buffer;
    const char *name=kernel.name_override;
    if(!name)name=(found->type<FakeTypeCount && kernel.type_names[found->type]) ? kernel.type_names[found->type] : "Unnamed";
    const size_t chars=strlen(name);
    size_t offset=64;                    /* byte offset of the characters inside the buffer */
    nt_unicode_t info={(USHORT)(chars*2),(USHORT)(chars*2+2),NULL};
    size_t written=chars;                /* characters written at offset */
    switch(kernel.object_mode){
    case ObjectModeNormal:break;
    case ObjectModeOddLength:info.length=(USHORT)(chars*2+1);break;
    case ObjectModeBufferInsideHeader:offset=8;break;
    case ObjectModeBufferBeforeHeader:offset=0;written=0;break;   /* pointer set below to base-2 */
    case ObjectModeBufferPastEnd:offset=length;written=0;break;   /* nothing written; pointer set below */
    case ObjectModeLengthPastEnd:offset=length-4;written=2;info.length=8;break;
    case ObjectModeOversizedLength:info.length=0xFFFE;break;
    case ObjectModeZeroLength:info.length=0;break;
    case ObjectModeNullBuffer:offset=0;written=0;break;
    case ObjectModeTextAtEnd:offset=length-chars*2;break;
    case ObjectModeTextJustAfterHeader:offset=sizeof info;break;
    case ObjectModeNonPrintable:
        written=0;info.length=(USHORT)sizeof NonPrintableUnits;
        memcpy(base+offset,NonPrintableUnits,sizeof NonPrintableUnits);break;
    }
    if(kernel.object_mode!=ObjectModeNonPrintable)
        for(size_t index=0;index<written;index++){
            const uint16_t unit=(uint16_t)(unsigned char)name[index];
            memcpy(base+offset+index*2,&unit,sizeof unit);
        }
    info.buffer=(PWSTR)(base+offset);
    if(kernel.object_mode==ObjectModeBufferBeforeHeader)info.buffer=(PWSTR)((uintptr_t)base-2);
    if(kernel.object_mode==ObjectModeBufferPastEnd)info.buffer=(PWSTR)((uintptr_t)base+length+2);
    if(kernel.object_mode==ObjectModeNullBuffer)info.buffer=NULL;
    memcpy(base,&info,sizeof info);
    return 0;
}

/** @brief Add one handle to the scripted process table. */
static void kernel_add(uintptr_t value,ULONG type,ULONG access)
{
    expect(kernel.length<FakeTableCapacity);
    kernel.table[kernel.length++]=(fake_handle_t){value,type,access,1};
}
/** @brief Fill the table with count deterministic handles of types 1..7. */
static void kernel_fill(size_t count)
{
    for(size_t index=0;index<count;index++)kernel_add(0x100+4*index,(ULONG)(1+index%7),(ULONG)(0x100000+index));
}
/** @brief Copy the scripted table into a caller-owned stack snapshot.
 * @param[out] storage At least kernel.length records. @param[out] set Receives a view over storage;
 * it owns nothing and must not be passed to handle_diag_set_free.
 */
static void kernel_view(handle_diag_entry_t *storage,handle_diag_set_t *set)
{
    for(size_t index=0;index<kernel.length;index++)
        storage[index]=(handle_diag_entry_t){kernel.table[index].value,kernel.table[index].type,kernel.table[index].access};
    *set=(handle_diag_set_t){storage,kernel.length,{0}};
}

/** @brief Reset every fake and open fresh capture streams. */
static void begin_case(const char *name)
{
    current_case=name;
    memset(&handle_diag_fake,0,sizeof handle_diag_fake);
    memset(&kernel,0,sizeof kernel);
    handle_diag_fake.frequency=1000;handle_diag_fake.ntdll_present=1;
    handle_diag_fake.query_process_export=(FARPROC)fake_query_process;
    handle_diag_fake.query_object_export=(FARPROC)fake_query_object;
    kernel.type_names[1]="File";kernel.type_names[2]="Key";kernel.type_names[3]="ALPC Port";kernel.type_names[4]="Event";
    kernel.type_names[5]="IRTimer";kernel.type_names[6]="Thread";kernel.type_names[7]="Section";
    handle_diag_fake_capture_open();
}
/** @brief Close the capture streams and require exact allocation balance. */
static void end_case(void)
{
    handle_diag_fake_capture_close();
    expect_u(handle_diag_fake.outstanding,0);
    expect(handle_diag_fake.lowest_outstanding>=0);
    expect_u(handle_diag_fake.zero_size_reallocs,0);
}
/** @brief End the running case and start another inside the same test function.
 * @param[in] name Static name of the next case.
 */
static void next_case(const char *name)
{
    end_case();
    begin_case(name);
}
/** @brief Capture the baseline snapshot from the scripted kernel into diag. */
static void begin_diag(handle_diag_t *diag)
{
    *diag=(handle_diag_t){0};
    handle_diag_begin(diag);
}

/* ---- Cases ---- */

/** @brief handle_check_record: first, third and hundredth sample, sample failure, extremes. */
static void test_check_statistics(void)
{
    handle_check_t check={0};
    handle_check_record(&check,199);                   /* equal on the first sample */
    expect_u(check.samples,1);expect_u(check.minimum,199);expect_u(check.maximum,199);expect_u(check.last,199);
    check=(handle_check_t){0};
    const DWORD third[]={200,200,199};                  /* equal on the third sample */
    for(size_t index=0;index<3;index++)handle_check_record(&check,third[index]);
    expect_u(check.samples,3);expect_u(check.minimum,199);expect_u(check.maximum,200);expect_u(check.last,199);
    check=(handle_check_t){0};
    for(unsigned index=0;index<100;index++)handle_check_record(&check,(index&1u) ? 200u : 201u); /* none equal */
    expect_u(check.samples,100);expect_u(check.minimum,200);expect_u(check.maximum,201);expect_u(check.last,200);
    check=(handle_check_t){0};
    handle_check_record(&check,200);                    /* a later unreadable sample records nothing */
    expect_u(check.samples,1);expect_u(check.minimum,200);expect_u(check.maximum,200);expect_u(check.last,200);
    check=(handle_check_t){0};
    handle_check_record(&check,198);handle_check_record(&check,201);handle_check_record(&check,199);
    expect_u(check.samples,3);expect_u(check.minimum,198);expect_u(check.maximum,201);expect_u(check.last,199);
    check=(handle_check_t){0};
    handle_check_record(&check,0xFFFFFFFFu);handle_check_record(&check,1);
    expect_u(check.samples,2);expect_u(check.minimum,1);expect_u(check.maximum,0xFFFFFFFFu);expect_u(check.last,1);
}

/** @brief handle_diag_begin and a plain capture: call order, fields, wire decoding, balance. */
static void test_begin_and_capture(void)
{
    kernel_fill(200);
    handle_diag_fake.frequency=10000000;handle_diag_fake.counter=424242;
    handle_diag_t diag;begin_diag(&diag);
    expect_text(handle_diag_fake.calls,"qpf qpc gmh:ntdll.dll gpa:NtQueryInformationProcess gpa:NtQueryObject nt_qip");
    expect_u(diag.frequency,10000000);expect_u(diag.start,424242);
    expect(diag.query_process==fake_query_process && diag.query_object==fake_query_object);
    expect(diag.baseline.unavailable[0]==0);
    expect_u(diag.baseline.count,200);
    for(size_t index=0;index<200;index++){
        expect_u(diag.baseline.entries[index].handle_value,kernel.table[index].value);
        expect_u(diag.baseline.entries[index].type_index,kernel.table[index].type);
        expect_u(diag.baseline.entries[index].granted_access,kernel.table[index].access);
    }
    expect_u(kernel.process_calls,1);expect_u(kernel.last_length,32768);
    expect_u(handle_diag_fake.realloc_calls,1);expect_u(handle_diag_fake.realloc_sizes[0],32768);
    expect_u(handle_diag_fake.malloc_calls,1);expect_u(handle_diag_fake.malloc_sizes[0],200*sizeof(handle_diag_entry_t));
    expect_u(handle_diag_fake.outstanding,1);          /* only the entries; the raw buffer is already freed */
    expect_u(kernel.object_calls,0);                   /* nothing is named at baseline time */
    handle_diag_free(&diag);
    expect(diag.baseline.entries==NULL && diag.baseline.count==0);
    expect_u(handle_diag_fake.outstanding,0);
}

/** @brief handle_diag_begin when pieces are unavailable: order, short-circuits, reasons. */
static void test_begin_variants(void)
{
    handle_diag_t diag;
    kernel_fill(3);
    handle_diag_fake.ntdll_present=0;                   /* ntdll cannot be found */
    handle_diag_fake.counter=77;begin_diag(&diag);
    expect_text(handle_diag_fake.calls,"qpf qpc gmh:ntdll.dll");
    expect(diag.query_process==NULL && diag.query_object==NULL);
    expect_text(diag.baseline.unavailable,"NtQueryInformationProcess export missing");
    expect_u(diag.baseline.count,0);expect_u(diag.frequency,1000);expect_u(diag.start,77);
    expect_u(kernel.process_calls,0);
    handle_diag_free(&diag);expect(diag.baseline.unavailable[0]==0);

    handle_diag_fake.calls[0]=0;handle_diag_fake.ntdll_present=1;
    handle_diag_fake.query_process_export=NULL;         /* only the process export is absent */
    begin_diag(&diag);
    expect_text(handle_diag_fake.calls,"qpf qpc gmh:ntdll.dll gpa:NtQueryInformationProcess gpa:NtQueryObject");
    expect(diag.query_process==NULL && diag.query_object==fake_query_object);
    expect_text(diag.baseline.unavailable,"NtQueryInformationProcess export missing");
    handle_diag_free(&diag);

    handle_diag_fake.calls[0]=0;handle_diag_fake.query_process_export=(FARPROC)fake_query_process;
    handle_diag_fake.query_object_export=NULL;          /* only the object export is absent */
    begin_diag(&diag);
    expect(diag.query_process==fake_query_process && diag.query_object==NULL);
    expect(diag.baseline.unavailable[0]==0);expect_u(diag.baseline.count,3);
    handle_diag_free(&diag);

    handle_diag_fake.query_object_export=(FARPROC)fake_query_object;
    handle_diag_fake.calls[0]=0;handle_diag_fake.frequency_fails=1; /* frequency call fails: counter never read */
    begin_diag(&diag);
    expect_text(handle_diag_fake.calls,"qpf gmh:ntdll.dll gpa:NtQueryInformationProcess gpa:NtQueryObject nt_qip");
    expect_u(diag.frequency,0);expect_u(diag.start,0);expect(diag.baseline.unavailable[0]==0);
    handle_diag_free(&diag);

    handle_diag_fake.calls[0]=0;handle_diag_fake.frequency_fails=0;handle_diag_fake.frequency=0; /* zero frequency */
    begin_diag(&diag);
    expect_text(handle_diag_fake.calls,"qpf gmh:ntdll.dll gpa:NtQueryInformationProcess gpa:NtQueryObject nt_qip");
    expect_u(diag.frequency,0);expect_u(diag.start,0);
    handle_diag_free(&diag);

    handle_diag_fake.calls[0]=0;handle_diag_fake.frequency=-5;  /* negative frequency */
    begin_diag(&diag);
    expect_u(diag.frequency,0);expect_u(diag.start,0);
    handle_diag_free(&diag);

    handle_diag_fake.calls[0]=0;handle_diag_fake.frequency=1000;handle_diag_fake.counter_fails=1;
    begin_diag(&diag);                                  /* counter call fails: nothing is recorded */
    expect_text(handle_diag_fake.calls,"qpf qpc gmh:ntdll.dll gpa:NtQueryInformationProcess gpa:NtQueryObject nt_qip");
    expect_u(diag.frequency,0);expect_u(diag.start,0);
    expect_u(handle_diag_now_ms(&diag),0);
    handle_diag_free(&diag);
}

/** @brief handle_diag_now_ms arithmetic: flooring, large values, unavailable and regressing counters. */
static void test_now_ms(void)
{
    handle_diag_t diag;
    handle_diag_fake.frequency=10000000;handle_diag_fake.counter=1000;
    begin_diag(&diag);
    handle_diag_fake.counter=1000;expect_u(handle_diag_now_ms(&diag),0);
    handle_diag_fake.counter=1000+9999;expect_u(handle_diag_now_ms(&diag),0);        /* floors below one millisecond */
    handle_diag_fake.counter=1000+10000;expect_u(handle_diag_now_ms(&diag),1);
    handle_diag_fake.counter=1000+12345678;expect_u(handle_diag_now_ms(&diag),1234);
    handle_diag_fake.counter=1000+9999999;expect_u(handle_diag_now_ms(&diag),999);
    handle_diag_fake.counter=1000+10000000;expect_u(handle_diag_now_ms(&diag),1000);
    handle_diag_fake.counter=999;expect_u(handle_diag_now_ms(&diag),0);               /* counter behind the origin */
    handle_diag_fake.counter=INT64_MAX;diag.start=0;                                   /* no overflow at the extreme */
    expect_u(handle_diag_now_ms(&diag),922337203685477ull);
    diag.frequency=3000000000ull;diag.start=5;handle_diag_fake.counter=5+(int64_t)(3000000000ull*86400ull);
    expect_u(handle_diag_now_ms(&diag),86400000);                                      /* one day at 3 GHz */
    handle_diag_fake.counter=5+(int64_t)(3000000000ull+1500000000ull);expect_u(handle_diag_now_ms(&diag),1500);
    diag.frequency=1;diag.start=0;handle_diag_fake.counter=5;expect_u(handle_diag_now_ms(&diag),5000);
    handle_diag_fake.counter_fails=1;expect_u(handle_diag_now_ms(&diag),0);            /* counter read fails */
    handle_diag_fake.counter_fails=0;diag.frequency=0;handle_diag_fake.counter=99;
    expect_u(handle_diag_now_ms(&diag),0);                                             /* counter unavailable at start */
    handle_diag_t zero={0};expect_u(handle_diag_now_ms(&zero),0);
    handle_diag_free(&diag);
}

/** @brief Buffer growth: reported and unreported lengths, never shrinking, exact reallocation sizes. */
static void test_capture_growth(void)
{
    handle_diag_t diag;
    kernel_fill(1000);                                  /* needs 16 + 1000*40 = 40016 bytes */
    begin_diag(&diag);
    expect_u(kernel.process_calls,2);expect_u(kernel.last_length,80032);
    expect_u(handle_diag_fake.realloc_calls,2);
    expect_u(handle_diag_fake.realloc_sizes[0],32768);expect_u(handle_diag_fake.realloc_sizes[1],80032);
    expect_u(diag.baseline.count,1000);expect(diag.baseline.unavailable[0]==0);
    expect_u(diag.baseline.entries[999].handle_value,kernel.table[999].value);
    handle_diag_free(&diag);

    next_case("capture_growth_unreported");             /* no hint: the size doubles */
    kernel_fill(1000);kernel.hint_mode=HintZero;
    begin_diag(&diag);
    expect_u(kernel.process_calls,2);expect_u(handle_diag_fake.realloc_sizes[0],32768);expect_u(handle_diag_fake.realloc_sizes[1],65536);
    expect_u(diag.baseline.count,1000);
    handle_diag_free(&diag);

    next_case("capture_growth_small_hint");  /* a hint below the current size never shrinks the buffer */
    kernel_fill(1000);kernel.hint_mode=HintFixed;kernel.hint_value=100;
    begin_diag(&diag);
    expect_u(handle_diag_fake.realloc_sizes[1],65536);expect_u(diag.baseline.count,1000);
    handle_diag_free(&diag);
}

/** @brief Attempt cap: six queries at most, with exact growth sequences, and the boundary around it. */
static void test_capture_attempt_cap(void)
{
    handle_diag_t diag;
    kernel_fill(5);kernel.mismatch_forever=1;kernel.hint_mode=HintZero;
    begin_diag(&diag);
    expect_u(kernel.process_calls,6);expect_u(handle_diag_fake.realloc_calls,6);
    const size_t doubling[]={32768,65536,131072,262144,524288,1048576};
    for(size_t index=0;index<6;index++)expect_u(handle_diag_fake.realloc_sizes[index],doubling[index]);
    expect_text(diag.baseline.unavailable,"NtQueryInformationProcess status=0xc0000004");
    expect(diag.baseline.entries==NULL && diag.baseline.count==0);
    expect_u(handle_diag_fake.malloc_calls,0);expect_u(handle_diag_fake.outstanding,0);
    handle_diag_free(&diag);

    next_case("capture_attempt_cap_hinted");
    kernel_fill(5);kernel.mismatch_forever=1;kernel.hint_mode=HintFixed;kernel.hint_value=100000;
    begin_diag(&diag);
    const size_t hinted[]={32768,200000,400000,800000,1600000,3200000};
    expect_u(kernel.process_calls,6);
    for(size_t index=0;index<6;index++)expect_u(handle_diag_fake.realloc_sizes[index],hinted[index]);
    expect_text(diag.baseline.unavailable,"NtQueryInformationProcess status=0xc0000004");
    handle_diag_free(&diag);

    next_case("capture_attempt_cap_boundary");
    kernel_fill(5);kernel.mismatch_remaining=5;kernel.hint_mode=HintZero; /* the sixth query still succeeds */
    begin_diag(&diag);
    expect_u(kernel.process_calls,6);expect(diag.baseline.unavailable[0]==0);expect_u(diag.baseline.count,5);
    expect_u(handle_diag_fake.realloc_sizes[5],1048576);
    handle_diag_free(&diag);

    next_case("capture_attempt_cap_boundary_fail");
    kernel_fill(5);kernel.mismatch_remaining=6;kernel.hint_mode=HintZero; /* a sixth mismatch is final */
    begin_diag(&diag);
    expect_u(kernel.process_calls,6);
    expect_text(diag.baseline.unavailable,"NtQueryInformationProcess status=0xc0000004");
    handle_diag_free(&diag);
}

/** @brief Size cap: a required size above 8 MiB is refused, exactly 8 MiB doubles to the 16 MiB limit. */
static void test_capture_size_cap(void)
{
    handle_diag_t diag;
    kernel_fill(5);kernel.mismatch_remaining=1;kernel.hint_mode=HintFixed;kernel.hint_value=8388608;
    begin_diag(&diag);                                  /* exactly at the limit: accepted */
    expect_u(kernel.process_calls,2);expect_u(handle_diag_fake.realloc_sizes[1],16777216);
    expect_u(kernel.last_length,16777216);expect(diag.baseline.unavailable[0]==0);expect_u(diag.baseline.count,5);
    handle_diag_free(&diag);

    next_case("capture_size_cap_exceeded");
    kernel_fill(5);kernel.mismatch_remaining=1;kernel.hint_mode=HintFixed;kernel.hint_value=8388609;
    begin_diag(&diag);                                  /* one byte over: refused before any larger allocation */
    expect_u(kernel.process_calls,1);expect_u(handle_diag_fake.realloc_calls,1);
    expect_text(diag.baseline.unavailable,"snapshot exceeds 16777216 bytes");
    handle_diag_free(&diag);

    next_case("capture_size_cap_maximum_hint");
    kernel_fill(5);kernel.mismatch_remaining=1;kernel.hint_mode=HintFixed;kernel.hint_value=0xFFFFFFFFu;
    begin_diag(&diag);
    expect_text(diag.baseline.unavailable,"snapshot exceeds 16777216 bytes");expect_u(handle_diag_fake.realloc_calls,1);
    handle_diag_free(&diag);
}

/** @brief Error statuses: negative statuses fail with the exact code, non-negative statuses succeed. */
static void test_capture_statuses(void)
{
    handle_diag_t diag;
    kernel_fill(3);kernel.forced_status=(LONG)StatusAccessDenied;
    begin_diag(&diag);
    expect_text(diag.baseline.unavailable,"NtQueryInformationProcess status=0xc0000022");
    expect_u(kernel.process_calls,1);expect_u(handle_diag_fake.realloc_calls,1);expect(diag.baseline.entries==NULL);
    handle_diag_free(&diag);

    next_case("capture_status_invalid_class");
    kernel_fill(3);kernel.forced_status=(LONG)0xC0000003UL;
    begin_diag(&diag);
    expect_text(diag.baseline.unavailable,"NtQueryInformationProcess status=0xc0000003");
    handle_diag_free(&diag);

    next_case("capture_status_warning");     /* a negative status other than length mismatch never retries */
    kernel_fill(3);kernel.forced_status=(LONG)0x80000005UL;
    begin_diag(&diag);
    expect_text(diag.baseline.unavailable,"NtQueryInformationProcess status=0x80000005");expect_u(kernel.process_calls,1);
    handle_diag_free(&diag);

    next_case("capture_status_positive");   /* non-negative statuses are success */
    kernel_fill(3);kernel.forced_status=0x103;
    begin_diag(&diag);
    expect(diag.baseline.unavailable[0]==0);expect_u(diag.baseline.count,3);
    handle_diag_free(&diag);
}

/** @brief Hostile handle counts: counts beyond the buffer are refused, counts that fit are decoded. */
static void test_capture_hostile_counts(void)
{
    handle_diag_t diag;
    kernel_fill(3);kernel.claim_active=1;kernel.claimed=819;  /* (32768-16)/40 = 818 entries fit */
    begin_diag(&diag);
    expect_text(diag.baseline.unavailable,"invalid snapshot layout handles=819 bytes=32768");
    expect(diag.baseline.entries==NULL && diag.baseline.count==0);expect_u(handle_diag_fake.malloc_calls,0);
    handle_diag_free(&diag);

    next_case("capture_hostile_counts_boundary");
    kernel_fill(3);kernel.claim_active=1;kernel.claimed=818;  /* exactly what fits */
    begin_diag(&diag);
    expect(diag.baseline.unavailable[0]==0);expect_u(diag.baseline.count,818);
    expect_u(diag.baseline.entries[2].handle_value,kernel.table[2].value);
    expect_u(diag.baseline.entries[3].handle_value,0);expect_u(diag.baseline.entries[817].granted_access,0);
    expect_u(handle_diag_fake.malloc_sizes[0],818*sizeof(handle_diag_entry_t));
    handle_diag_free(&diag);

    next_case("capture_hostile_counts_huge");
    kernel_fill(3);kernel.claim_active=1;kernel.claimed=SIZE_MAX;
    begin_diag(&diag);
    expect_text(diag.baseline.unavailable,"invalid snapshot layout handles=18446744073709551615 bytes=32768");
    handle_diag_free(&diag);

    next_case("capture_hostile_counts_after_growth");
    kernel_fill(3);kernel.mismatch_remaining=1;kernel.hint_mode=HintZero;kernel.claim_active=1;kernel.claimed=2001;
    begin_diag(&diag);                                  /* after one doubling: (65536-16)/40 = 1638 */
    expect_text(diag.baseline.unavailable,"invalid snapshot layout handles=2001 bytes=65536");
    handle_diag_free(&diag);

    next_case("capture_zero_handles");
    kernel.claim_active=1;kernel.claimed=0;
    begin_diag(&diag);
    expect(diag.baseline.unavailable[0]==0 && diag.baseline.entries==NULL && diag.baseline.count==0);
    expect_u(handle_diag_fake.malloc_calls,0);
    handle_diag_free(&diag);
}

/** @brief Allocation failures: exact reasons, no leak, nothing half-owned. */
static void test_capture_allocation_failures(void)
{
    handle_diag_t diag;
    kernel_fill(5);handle_diag_fake.fail_realloc_at=1;
    begin_diag(&diag);
    expect_text(diag.baseline.unavailable,"allocation of 32768 bytes failed");
    expect_u(kernel.process_calls,0);expect(diag.baseline.entries==NULL);
    handle_diag_free(&diag);

    next_case("capture_allocation_growth");
    kernel_fill(1000);handle_diag_fake.fail_realloc_at=2;  /* the first buffer must still be released */
    begin_diag(&diag);
    expect_text(diag.baseline.unavailable,"allocation of 80032 bytes failed");expect_u(kernel.process_calls,1);
    expect_u(handle_diag_fake.outstanding,0);
    handle_diag_free(&diag);

    next_case("capture_allocation_entries");
    kernel_fill(5);handle_diag_fake.fail_malloc_at=1;
    begin_diag(&diag);
    expect_text(diag.baseline.unavailable,"allocation of 5 entries failed");
    expect_u(handle_diag_fake.outstanding,0);expect(diag.baseline.entries==NULL && diag.baseline.count==0);
    handle_diag_free(&diag);

    next_case("capture_allocation_replaces_previous");
    kernel_fill(5);                                     /* recapture into a populated set frees it first */
    begin_diag(&diag);
    expect_u(handle_diag_fake.outstanding,1);
    handle_diag_fake.fail_malloc_at=handle_diag_fake.malloc_calls+1;
    expect_u(handle_diag_capture(&diag,&diag.baseline),1);
    expect_text(diag.baseline.unavailable,"allocation of 5 entries failed");
    expect_u(handle_diag_fake.outstanding,0);expect(diag.baseline.entries==NULL);
    expect_u(handle_diag_capture(&diag,&diag.baseline),0);  /* and a later success clears the reason */
    expect(diag.baseline.unavailable[0]==0);expect_u(diag.baseline.count,5);expect_u(handle_diag_fake.outstanding,1);
    expect_u(handle_diag_capture(&diag,&diag.baseline),0);  /* replacing a valid set does not accumulate blocks */
    expect_u(handle_diag_fake.outstanding,1);
    handle_diag_free(&diag);

    next_case("capture_missing_export_direct");
    kernel_fill(2);
    handle_diag_set_t set={0};diag=(handle_diag_t){0};
    expect_u(handle_diag_capture(&diag,&set),1);
    expect_text(set.unavailable,"NtQueryInformationProcess export missing");expect_u(kernel.process_calls,0);
    handle_diag_set_free(&set);expect(set.unavailable[0]==0);
}

/** @brief Hide a constant size from the optimizer.
 * @param[in] value Size to pass through. @return value, unknown to the compiler.
 * @details The deliberate-truncation calls below must not trip the compiler's own format-truncation
 * diagnostic, which would otherwise reject the code under test for behaving as documented.
 */
static size_t opaque_size(size_t value)
{
    volatile size_t held=value;
    return held;
}
/** @brief handle_diag_type_name: hostile layouts, boundaries, truncation, fallbacks, exact queries. */
static void test_type_names(void)
{
    handle_diag_t diag={0};diag.query_object=fake_query_object;
    handle_diag_entry_t storage[8];handle_diag_set_t live;
    char name[HandleDiagNameChars];
    kernel_add(0x40,3,1);kernel_add(0x44,4,2);
    kernel_view(storage,&live);

    handle_diag_type_name(&diag,&live,3,name,sizeof name);expect_text(name,"ALPC Port");
    expect_u(kernel.object_calls,1);expect_u(kernel.queried[0],0x40);

    const struct { object_mode_t mode; const char *expected; } modes[]={
        {ObjectModeNormal,"ALPC Port"},{ObjectModeOddLength,"unknown"},{ObjectModeBufferInsideHeader,"unknown"},
        {ObjectModeBufferBeforeHeader,"unknown"},{ObjectModeBufferPastEnd,"unknown"},{ObjectModeLengthPastEnd,"unknown"},
        {ObjectModeOversizedLength,"unknown"},{ObjectModeZeroLength,"unknown"},{ObjectModeNullBuffer,"unknown"},
        {ObjectModeTextAtEnd,"ALPC Port"},{ObjectModeTextJustAfterHeader,"ALPC Port"},{ObjectModeNonPrintable,"A??? ~?z"},
    };
    for(size_t index=0;index<sizeof modes/sizeof modes[0];index++){
        kernel.object_mode=modes[index].mode;kernel.object_calls=0;
        handle_diag_type_name(&diag,&live,3,name,sizeof name);
        expect_text(name,modes[index].expected);
        expect_u(kernel.object_calls,1);                /* a rejected reply is not retried on the same handle */
    }
    kernel.object_mode=ObjectModeNormal;

    kernel.name_override="0123456789012345678901234567890123456789012345678901234567890123456789";
    handle_diag_type_name(&diag,&live,3,name,sizeof name);   /* 70 characters into 48 bytes */
    expect_text(name,"01234567890123456789012345678901234567890123456");
    kernel.name_override=NULL;

    char scratch[16];                                   /* sentinel '#' proves nothing is written past size */
    memset(scratch,'#',sizeof scratch);handle_diag_type_name(&diag,&live,3,scratch,opaque_size(4));
    expect_text(scratch,"ALP");expect(scratch[4]=='#');
    memset(scratch,'#',sizeof scratch);handle_diag_type_name(&diag,&live,3,scratch,opaque_size(9));
    expect_text(scratch,"ALPC Por");expect(scratch[9]=='#');                          /* one short of fitting */
    memset(scratch,'#',sizeof scratch);handle_diag_type_name(&diag,&live,3,scratch,opaque_size(10));
    expect_text(scratch,"ALPC Port");expect(scratch[10]=='#');                        /* exact fit with the NUL */
    memset(scratch,'#',sizeof scratch);handle_diag_type_name(&diag,&live,3,scratch,opaque_size(1));
    expect_text(scratch,"");expect(scratch[1]=='#');
    memset(scratch,'#',sizeof scratch);handle_diag_type_name(&diag,&live,3,scratch,opaque_size(0));
    expect(scratch[0]=='#');                                                           /* size zero writes nothing */
    memset(scratch,'#',sizeof scratch);handle_diag_type_name(&diag,&live,99,scratch,opaque_size(4));
    expect_text(scratch,"unk");expect(scratch[4]=='#');                                /* truncated fallback text */
    memset(scratch,'#',sizeof scratch);handle_diag_type_name(&diag,&live,99,scratch,opaque_size(0));
    expect(scratch[0]=='#');
    handle_diag_type_name(&diag,&live,99,name,sizeof name);expect_text(name,"unknown");   /* no live handle of the type */

    kernel.object_calls=0;kernel.object_forced_status=(LONG)StatusAccessDenied;           /* every query fails */
    handle_diag_type_name(&diag,&live,3,name,sizeof name);expect_text(name,"unknown");expect_u(kernel.object_calls,1);
    kernel.object_forced_status=0;

    diag.query_object=NULL;kernel.object_calls=0;                                           /* export missing */
    handle_diag_type_name(&diag,&live,3,name,sizeof name);expect_text(name,"unknown");expect_u(kernel.object_calls,0);
    diag.query_object=fake_query_object;

    next_case("type_names_fallback_to_next_live_handle");
    diag=(handle_diag_t){0};diag.query_object=fake_query_object;
    kernel_add(0x50,3,1);kernel_add(0x54,3,1);kernel_add(0x58,3,1);
    kernel.table[0].live=0;kernel.table[1].live=0;          /* closed since the snapshot */
    kernel_view(storage,&live);
    handle_diag_type_name(&diag,&live,3,name,sizeof name);
    expect_text(name,"ALPC Port");expect_u(kernel.object_calls,3);
    expect_u(kernel.queried[0],0x50);expect_u(kernel.queried[1],0x54);expect_u(kernel.queried[2],0x58);
    kernel.table[2].live=0;kernel.object_calls=0;           /* all closed: tried once each, then unknown */
    handle_diag_type_name(&diag,&live,3,name,sizeof name);expect_text(name,"unknown");expect_u(kernel.object_calls,3);
}

/** @brief handle_diag_contains and handle_diag_print_delta used directly, with no NtQueryObject. */
static void test_contains_and_print_delta(void)
{
    handle_diag_t diag={0};
    handle_diag_set_t empty={0};
    const handle_diag_entry_t probe={0x10,3,7};
    expect_u(handle_diag_contains(&empty,&probe),0);       /* NULL entries with zero count is valid */
    handle_diag_entry_t first[]={{0x10,3,7},{0x14,3,7}};
    handle_diag_entry_t second[]={{0x10,3,7},{0x14,3,8},{0x18,4,7}};
    const handle_diag_set_t from={first,2,{0}},to={second,3,{0}};
    expect_u(handle_diag_contains(&from,&probe),1);
    expect_u(handle_diag_contains(&from,&(handle_diag_entry_t){0x10,4,7}),0);   /* type differs */
    expect_u(handle_diag_contains(&from,&(handle_diag_entry_t){0x10,3,8}),0);   /* access differs */
    expect_u(handle_diag_contains(&from,&(handle_diag_entry_t){0x11,3,7}),0);   /* value differs */
    expect_u(handle_diag_print_delta(&diag,"added",&to,&from,&to),2);
    expect_text(handle_diag_fake_err_text(),
        "handle_diag: added value=0x14 type_index=3 type=unknown access=0x00000008\n"
        "handle_diag: added value=0x18 type_index=4 type=unknown access=0x00000007\n");
    handle_diag_fake_capture_reset();
    expect_u(handle_diag_print_delta(&diag,"removed",&from,&to,&to),1);
    expect_text(handle_diag_fake_err_text(),"handle_diag: removed value=0x14 type_index=3 type=unknown access=0x00000007\n");
    handle_diag_fake_capture_reset();
    expect_u(handle_diag_print_delta(&diag,"added",&empty,&to,&to),0);
    expect_text(handle_diag_fake_err_text(),"");
}

/** @brief Add the handles of a scripted failure-time table: used by the report cases. */
static void script_report_tables(void)
{
    kernel_add(0x04,1,0x00120089);kernel_add(0x08,3,0x001f0001);kernel_add(0x0c,5,0x001f0003);
    kernel_add(0x10,3,0x001f0001);kernel_add(0x14,6,0x00100000);
}
/** @brief Replace the scripted table with the failure-time one for the report cases. */
static void script_failure_table(void)
{
    memset(kernel.table,0,sizeof kernel.table);kernel.length=0;
    kernel_add(0x04,1,0x00120089);kernel_add(0x10,3,0x001f0001);kernel_add(0x18,3,0x001f0001);
    kernel_add(0x1c,4,0x001f0003);kernel_add(0x14,6,0x00100001);
}

/** @brief Full failure report: exact text, ordering, identity by value+type+access, type names, balance. */
static void test_report_delta(void)
{
    handle_diag_t diag;
    script_report_tables();
    handle_diag_fake.frequency=1000;handle_diag_fake.counter=100;
    begin_diag(&diag);
    script_failure_table();
    handle_diag_fake.counter=100+6100;
    kernel.object_calls=0;
    const handle_check_t check={100,200,200,200};
    handle_diag_report(&diag,199,&check,2,5000);
    expect_text(handle_diag_fake_err_text(),
        "handle_diag: retirement failed lifetime=3 iteration_index=2 baseline=199 samples=100 min=200 max=200 last=200 check_start_ms=5000 elapsed_ms=6100\n"
        "handle_diag: added value=0x18 type_index=3 type=ALPC Port access=0x001f0001\n"
        "handle_diag: added value=0x1c type_index=4 type=Event access=0x001f0003\n"
        "handle_diag: added value=0x14 type_index=6 type=Thread access=0x00100001\n"
        "handle_diag: removed value=0x8 type_index=3 type=ALPC Port access=0x001f0001\n"
        "handle_diag: removed value=0xc type_index=5 type=unknown access=0x001f0003\n"
        "handle_diag: removed value=0x14 type_index=6 type=Thread access=0x00100000\n"
        "handle_diag: snapshot baseline_handles=5 current_handles=5 added=3 removed=3\n");
    expect_u(kernel.process_calls,2);                   /* baseline snapshot plus one failure-time snapshot */
    expect_u(handle_diag_fake.outstanding,1);           /* the failure-time snapshot is private and freed */
    expect_u(diag.baseline.count,5);                    /* the baseline snapshot is untouched */
    expect_u(diag.baseline.entries[1].handle_value,0x08);
    handle_diag_free(&diag);

    next_case("report_identical");
    script_report_tables();
    begin_diag(&diag);
    handle_diag_fake.counter=250;
    handle_diag_report(&diag,199,&(handle_check_t){1,199,199,199},0,0);
    expect_text(handle_diag_fake_err_text(),
        "handle_diag: retirement failed lifetime=1 iteration_index=0 baseline=199 samples=1 min=199 max=199 last=199 check_start_ms=0 elapsed_ms=250\n"
        "handle_diag: snapshot baseline_handles=5 current_handles=5 added=0 removed=0\n");
    handle_diag_free(&diag);

    next_case("report_same_value_other_type");
    kernel_add(0x20,1,5);
    begin_diag(&diag);
    kernel.table[0].type=2;
    handle_diag_report(&diag,10,&(handle_check_t){2,11,12,12},4,1);
    expect_text(handle_diag_fake_err_text(),
        "handle_diag: retirement failed lifetime=5 iteration_index=4 baseline=10 samples=2 min=11 max=12 last=12 check_start_ms=1 elapsed_ms=0\n"
        "handle_diag: added value=0x20 type_index=2 type=Key access=0x00000005\n"
        "handle_diag: removed value=0x20 type_index=1 type=unknown access=0x00000005\n"
        "handle_diag: snapshot baseline_handles=1 current_handles=1 added=1 removed=1\n");
    handle_diag_free(&diag);
}

/** @brief Report headers for the sample variants, and the two unavailable branches. */
static void test_report_unavailable(void)
{
    handle_diag_t diag;
    kernel_fill(3);handle_diag_fake.ntdll_present=0;handle_diag_fake.counter=100;
    begin_diag(&diag);
    handle_diag_fake.counter=100+7345;
    handle_diag_report(&diag,199,&(handle_check_t){100,200,201,200},6,12345);
    expect_text(handle_diag_fake_err_text(),
        "handle_diag: retirement failed lifetime=7 iteration_index=6 baseline=199 samples=100 min=200 max=201 last=200 check_start_ms=12345 elapsed_ms=7345\n"
        "handle_diag: snapshot unavailable (baseline: NtQueryInformationProcess export missing)\n");
    expect_u(kernel.process_calls,0);                   /* no second snapshot when the baseline is unusable */
    handle_diag_fake_capture_reset();
    handle_diag_report(&diag,199,&(handle_check_t){0},0,0);   /* no sample was ever read */
    expect_text(handle_diag_fake_err_text(),
        "handle_diag: retirement failed lifetime=1 iteration_index=0 baseline=199 samples=0 min=none max=none last=none check_start_ms=0 elapsed_ms=7345\n"
        "handle_diag: snapshot unavailable (baseline: NtQueryInformationProcess export missing)\n");
    handle_diag_free(&diag);

    next_case("report_failure_snapshot_unavailable");
    kernel_fill(3);
    begin_diag(&diag);
    kernel.forced_status=(LONG)StatusAccessDenied;
    handle_diag_report(&diag,199,&(handle_check_t){2,200,200,200},1,9);
    expect_text(handle_diag_fake_err_text(),
        "handle_diag: retirement failed lifetime=2 iteration_index=1 baseline=199 samples=2 min=200 max=200 last=200 check_start_ms=9 elapsed_ms=0\n"
        "handle_diag: snapshot unavailable (failure: NtQueryInformationProcess status=0xc0000022)\n");
    handle_diag_free(&diag);

    next_case("report_failure_snapshot_allocation");
    kernel_fill(3);
    begin_diag(&diag);
    handle_diag_fake.fail_malloc_at=handle_diag_fake.malloc_calls+1;
    handle_diag_report(&diag,199,&(handle_check_t){1,200,200,200},0,0);
    expect_text(handle_diag_fake_err_text(),
        "handle_diag: retirement failed lifetime=1 iteration_index=0 baseline=199 samples=1 min=200 max=200 last=200 check_start_ms=0 elapsed_ms=0\n"
        "handle_diag: snapshot unavailable (failure: allocation of 3 entries failed)\n");
    expect_u(handle_diag_fake.outstanding,1);
    handle_diag_free(&diag);
}

/** @brief Large deltas are printed completely, in table order, for both directions. */
static void test_report_large_delta(void)
{
    handle_diag_t diag;
    kernel_fill(5);
    begin_diag(&diag);                                  /* baseline: the five handles 0x100..0x110 */
    for(unsigned index=0;index<300;index++)kernel_add(0x4000+4*index,(ULONG)(1+index%7),0x1000+index);
    handle_diag_report(&diag,199,&(handle_check_t){1,305,305,305},0,0);
    const char *text=handle_diag_fake_err_text();
    unsigned lines=0;for(const char *cursor=text;*cursor;cursor++)if(*cursor=='\n')lines++;
    expect_u(lines,302);                                /* header + 300 added + summary */
    expect(strstr(text,"handle_diag: added value=0x4000 type_index=1 type=File access=0x00001000\n")!=NULL);
    expect(strstr(text,"handle_diag: added value=0x44ac type_index=6 type=Thread access=0x0000112b\n")!=NULL);
    expect(strstr(text,"added=300 removed=0\n")!=NULL && strstr(text,"baseline_handles=5 current_handles=305")!=NULL);
    expect(strstr(text,"removed value")==NULL);
    handle_diag_fake_capture_reset();
    memset(kernel.table,0,sizeof kernel.table);kernel.length=0;
    kernel_fill(2);                                     /* three of the five baseline handles disappear */
    handle_diag_report(&diag,199,&(handle_check_t){1,2,2,2},0,0);
    text=handle_diag_fake_err_text();
    lines=0;for(const char *cursor=text;*cursor;cursor++)if(*cursor=='\n')lines++;
    expect_u(lines,5);                                  /* header + 3 removed + summary */
    expect(strstr(text,"added=0 removed=3\n")!=NULL);
    expect(strstr(text,"handle_diag: removed value=0x108 type_index=3 type=unknown access=0x00100002\n")!=NULL);
    handle_diag_free(&diag);
}

/** @brief Stdout lines for the baseline outcome and the per-lifetime timing; exact text and flushing. */
static void test_stdout_lines(void)
{
    handle_diag_t diag;
    kernel_fill(199);
    begin_diag(&diag);
    handle_diag_describe_baseline(&diag,199);
    const char *baseline_line="handle_diag: baseline snapshot handles=199 process_handle_count=199\n";
    expect_u(handle_diag_fake_out_visible_size(),strlen(baseline_line));   /* flushed by the function itself */
    expect_text(handle_diag_fake_out_text(),baseline_line);
    expect_text(handle_diag_fake_err_text(),"");
    handle_diag_free(&diag);

    handle_diag_fake_capture_reset();
    handle_diag_fake.ntdll_present=0;
    begin_diag(&diag);
    handle_diag_describe_baseline(&diag,198);
    const char *unavailable_line="handle_diag: snapshot unavailable (baseline: NtQueryInformationProcess export missing)\n";
    expect_u(handle_diag_fake_out_visible_size(),strlen(unavailable_line));
    expect_text(handle_diag_fake_out_text(),unavailable_line);
    handle_diag_free(&diag);

    handle_diag_fake_capture_reset();
    handle_diag_iteration(7,&(handle_check_t){3,199,200,199},1500,61000,61075);
    const char *timing_line="handle_diag: lifetime=8 iteration_index=7 samples=3 count=199 start_ms=1500 check_start_ms=61000 check_done_ms=61075\n";
    expect_u(handle_diag_fake_out_visible_size(),strlen(timing_line));
    expect_text(handle_diag_fake_out_text(),timing_line);
    expect_text(handle_diag_fake_err_text(),"");
}

/** @brief The failure report reaches the stream without relying on a later flush. */
static void test_report_is_flushed(void)
{
    handle_diag_t diag;
    kernel_fill(2);
    begin_diag(&diag);
    handle_diag_report(&diag,199,&(handle_check_t){1,200,200,200},0,0);
    const size_t visible=handle_diag_fake_err_visible_size();
    expect(visible>0);
    expect_u(visible,strlen(handle_diag_fake_err_text()));
    expect_text(handle_diag_fake_out_text(),"");
    handle_diag_free(&diag);
}

/** @brief Release semantics: zero-initialized, repeated and failed contexts. */
static void test_free_semantics(void)
{
    handle_diag_t zero={0};
    handle_diag_free(&zero);handle_diag_free(&zero);       /* nothing allocated, nothing to undo */
    expect_u(handle_diag_fake.outstanding,0);expect(zero.baseline.entries==NULL);
    handle_diag_set_t set={0};handle_diag_set_free(&set);handle_diag_set_free(&set);

    handle_diag_t diag;
    kernel_fill(4);
    begin_diag(&diag);
    expect_u(handle_diag_fake.outstanding,1);
    handle_diag_free(&diag);
    expect(diag.baseline.entries==NULL && diag.baseline.count==0 && diag.baseline.unavailable[0]==0);
    expect_u(handle_diag_fake.outstanding,0);
    handle_diag_free(&diag);                               /* second release is a no-op */
    expect_u(handle_diag_fake.outstanding,0);

    kernel.forced_status=(LONG)StatusAccessDenied;
    begin_diag(&diag);                                     /* a failed capture owns no memory but keeps its reason */
    expect(diag.baseline.unavailable[0]!=0);expect_u(handle_diag_fake.outstanding,0);
    handle_diag_free(&diag);
    expect(diag.baseline.unavailable[0]==0);
}

/** @brief Find an exact snippet in text exactly once. @return Offset of the snippet, or SIZE_MAX on failure. */
static size_t find_once(const char *text,const char *snippet,const char *label)
{
    const char *first=strstr(text,snippet);
    if(!first)fail_check(__FILE__,__LINE__,label,snippet,"(not found)\n");
    if(strstr(first+1,snippet))fail_check(__FILE__,__LINE__,label,snippet,"(found more than once)\n");
    checks_passed++;
    return (size_t)(first-text);
}
/** @brief Pin the unchanged acceptance rule of tcp_gpu_windows.c by exact source text.
 * @param[in] path Fixture source, read with the real allocator and released before return.
 * @details handle_baseline cannot be compiled here, so any edit to the 100 samples, the 10 ms
 * interval, the equality test, the legacy failure line, the baseline-sample ordering or the
 * lifetime count fails this case until the pins are updated together with the acceptance criteria.
 */
static void test_acceptance_source_pin(const char *path)
{
    FILE *file=fopen(path,"rb");
    if(!file)fail_check(__FILE__,__LINE__,"cannot open the fixture source",path,"(open failed)\n");
    fseek(file,0,SEEK_END);const long length=ftell(file);fseek(file,0,SEEK_SET);
    expect(length>0);
    char *source=(malloc)((size_t)length+1);
    expect(source!=NULL);
    expect_u(fread(source,1,(size_t)length,file),(size_t)length);
    source[length]=0;fclose(file);
    const char symbol_text[]=
        "static int symbol(HMODULE module,const char *name,void *output,size_t bytes)\n"
        "{\n"
        "    FARPROC procedure=GetProcAddress(module,name);if(!procedure || bytes!=sizeof procedure)return 1;\n"
        "    memcpy(output,&procedure,bytes);return 0;\n"
        "}\n";
    const char native_handles_text[]=
        "static DWORD native_handles(void)\n"
        "{\n"
        "    DWORD count=0;\n"
        "    if(!GetProcessHandleCount(GetCurrentProcess(),&count)){DWORD error=GetLastError();fprintf(stderr,\"handle_diag: GetProcessHandleCount failed error=%lu\\n\",(unsigned long)error);return 0;}\n"
        "    return count;\n"
        "}\n";
    const char baseline_text[]=
        "static int handle_baseline(DWORD baseline,handle_check_t *check)\n"
        "{\n"
        "    *check=(handle_check_t){0};\n"
        "    for(unsigned attempt=0;attempt<100;attempt++){DWORD count=native_handles();if(!count)return 1;handle_check_record(check,count);if(count==baseline)return 0;Sleep(10);}\n"
        "    fprintf(stderr,\"Native GPU handle baseline=%lu actual=%lu\\n\",(unsigned long)baseline,(unsigned long)native_handles());return 1;\n"
        "}\n";
    const size_t symbol_at=find_once(source,symbol_text,"symbol() differs from the host test double");
    const size_t native_at=find_once(source,native_handles_text,"native_handles() differs from the pinned text");
    const size_t include_at=find_once(source,"#include \"tcp_gpu_handle_diag_windows.inc\"\n","diagnostics include");
    const size_t baseline_at=find_once(source,baseline_text,"handle_baseline() differs from the pinned acceptance rule");
    expect(symbol_at<native_at && native_at<include_at && include_at<baseline_at);
    find_once(source,
        "    DWORD baseline=native_handles();if(!baseline)goto cleanup;\n"
        "    handle_diag_begin(&diag);\n"
        "    printf(\"Native repeated application handle baseline=%lu\\n\",(unsigned long)baseline);fflush(stdout);\n",
        "baseline sample, then diagnostics, then the baseline line");
    find_once(source,"    for(unsigned iteration=0;iteration<(presentation_smoke ? 2u : 8u);iteration++) {\n","lifetime count");
    find_once(source,
        "        stage=\"exact native application handle retirement\";\n"
        "        const unsigned long long check_start_ms=handle_diag_now_ms(&diag);handle_check_t check={0};\n"
        "        if(handle_baseline(baseline,&check)){handle_diag_report(&diag,baseline,&check,iteration,check_start_ms);goto cleanup;}\n",
        "retirement check and failure path");
    find_once(source,"handle_baseline(baseline,&check)","the only handle_baseline call");
    (free)(source);
}

/** @brief One named case of the table in main. */
typedef struct case_entry_t {
    const char *name;           /**< Static case name. */
    void (*body)(void);         /**< Case body; may start further cases with next_case. */
} case_entry_t;
/** @brief Run one case: fresh fakes, the body, then the allocation-balance check. */
static void run_case(const char *name,void (*body)(void))
{
    begin_case(name);
    body();
    end_case();
}

/** @brief Deliberately leak the real baseline snapshot; only LeakSanitizer can notice.
 * @details handle_diag_begin allocates the snapshot through the real code path and the context
 * is dropped without handle_diag_free. Kept out of line and followed by scrub_stack so no stale
 * copy of the pointer survives in a dead frame that the exit-time scan could still read.
 */
__attribute__((noinline)) static void leak_control(void)
{
    begin_case("leak_control");
    kernel_fill(5);
    handle_diag_t context;
    begin_diag(&context);
    handle_diag_fake_capture_close();
}
/** @brief Overwrite the stack region used by leak_control with zeros. */
__attribute__((noinline)) static void scrub_stack(void)
{
    volatile unsigned char pad[65536];
    for(size_t index=0;index<sizeof pad;index++)pad[index]=0;
}

/** @brief Run every case, then optionally pin the acceptance rule.
 * @param[in] argc,argv Optional single argument: path of tcp_gpu_windows.c.
 * @return Zero when every check passed; failures exit with status 1 from fail_check.
 */
int main(int argc,char **argv)
{
    const char *control=getenv("WADDLE_TEST_LEAK_CONTROL");
    if(control && control[0]=='1'){leak_control();scrub_stack();return 0;}
    if(argc>2){fprintf(handle_diag_fake_real_stderr(),"usage: %s [path-to-tcp_gpu_windows.c]\n",argv[0]);return 2;}
    static const case_entry_t cases[]={
        {"check_statistics",test_check_statistics},
        {"begin_and_capture",test_begin_and_capture},
        {"begin_variants",test_begin_variants},
        {"now_ms",test_now_ms},
        {"capture_growth",test_capture_growth},
        {"capture_attempt_cap",test_capture_attempt_cap},
        {"capture_size_cap",test_capture_size_cap},
        {"capture_statuses",test_capture_statuses},
        {"capture_hostile_counts",test_capture_hostile_counts},
        {"capture_allocation_failures",test_capture_allocation_failures},
        {"type_names",test_type_names},
        {"contains_and_print_delta",test_contains_and_print_delta},
        {"report_delta",test_report_delta},
        {"report_unavailable",test_report_unavailable},
        {"report_large_delta",test_report_large_delta},
        {"stdout_lines",test_stdout_lines},
        {"report_is_flushed",test_report_is_flushed},
        {"free_semantics",test_free_semantics},
    };
    for(size_t index=0;index<sizeof cases/sizeof cases[0];index++)run_case(cases[index].name,cases[index].body);
    if(argc==2){current_case="acceptance_source_pin";test_acceptance_source_pin(argv[1]);}
    FILE *stream=handle_diag_fake_real_stdout();
    fprintf(stream,"tcp_gpu_handle_diag_host: %zu cases, %u checks PASS%s\n",sizeof cases/sizeof cases[0],checks_passed,
        argc==2 ? ", acceptance source pinned" : ", acceptance source not pinned (no path given)");
    fflush(stream);
    return 0;
}
