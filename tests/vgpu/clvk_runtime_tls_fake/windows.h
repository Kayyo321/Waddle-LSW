/**
 * @file windows.h
 * @brief Linux-only Win32 test double for src/vgpu/clvk_runtime_tls.c.
 *
 * Declares exactly the Win32 surface the runtime TLS shim uses. Definitions
 * live in tests/vgpu/clvk_runtime_tls_test.c and are built on POSIX threads.
 * This double is never part of a Windows build and proves no native behavior.
 */
#ifndef WaddleRuntimeTlsFakeWindowsHeader
#define WaddleRuntimeTlsFakeWindowsHeader

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define WINAPI
/** Fake success value of WaitForSingleObject. */
#define WAIT_OBJECT_0 0u
/** Fake failure value of WaitForSingleObject. */
#define WAIT_FAILED 0xffffffffu
/** Fake infinite timeout. */
#define INFINITE 0xffffffffu
/** Fake CreateThread flag; the double always honors it. */
#define CREATE_SUSPENDED 0x4u
/** Fake Win32 error code returned for allocation failure. */
#define ERROR_NOT_ENOUGH_MEMORY 8u
/** Fake __fastfail code. */
#define FAST_FAIL_FATAL_APP_EXIT 7u

typedef uint32_t DWORD;
typedef unsigned long ULONG;
typedef void *LPVOID;
/** Opaque fake handle: a heap-owned fake thread record. */
typedef struct fake_thread_t *HANDLE;
/** Fake thread routine signature. */
typedef DWORD (*LPTHREAD_START_ROUTINE)(LPVOID);
/** Fake slim reader/writer lock backed by a statically initialized mutex. */
typedef struct {
   pthread_mutex_t mutex;
} SRWLOCK;
/** Static initializer; the fake needs no destruction, like SRWLOCK_INIT. */
#define SRWLOCK_INIT { PTHREAD_MUTEX_INITIALIZER }

/** Acquire @p lock exclusively; never fails. */
void AcquireSRWLockExclusive(SRWLOCK *lock);
/** Release @p lock held exclusively by the caller. */
void ReleaseSRWLockExclusive(SRWLOCK *lock);
/** Create a suspended fake thread; NULL and last error on injected failure. */
HANDLE CreateThread(void *attributes, size_t stack, LPTHREAD_START_ROUTINE routine,
                    LPVOID parameter, DWORD flags, DWORD *thread_id);
/** Resume a fake thread; (DWORD)-1 on injected failure. */
DWORD ResumeThread(HANDLE thread);
/** Cancel a never-resumed fake thread; returns nonzero. */
int TerminateThread(HANDLE thread, DWORD exit_code);
/** Join the fake thread; WAIT_FAILED on injected failure. */
DWORD WaitForSingleObject(HANDLE thread, DWORD milliseconds);
/** Read the exit code; zero on injected failure. */
int GetExitCodeThread(HANDLE thread, DWORD *exit_code);
/** Free the fake handle; returns nonzero. */
int CloseHandle(HANDLE thread);
/** Return the fake thread's id. */
DWORD GetThreadId(HANDLE thread);
/** Return the calling thread's fake id. */
DWORD GetCurrentThreadId(void);
/** Return the calling thread's fake last error. */
DWORD GetLastError(void);
/** Fake fail-fast: exits the (forked) test process with status 77. */
void __fastfail(unsigned int code) __attribute__((noreturn));
/** Counting allocator used by the shim under test. */
void *waddle_fake_malloc(size_t size);
/** Counting free paired with waddle_fake_malloc. */
void waddle_fake_free(void *pointer);

#define malloc(size) waddle_fake_malloc(size)
#define free(pointer) waddle_fake_free(pointer)

#endif
