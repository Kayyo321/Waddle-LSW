/**
 * @file clvk_runtime_tls_test.c
 * @brief Linux unit tests for the actual src/vgpu/clvk_runtime_tls.c logic.
 *
 * The shim is included with WaddleRuntimeTlsFakeWindows, which renames its
 * libgcc ABI entry points to waddle_test_* and replaces the loader-owned
 * record with a Linux __thread record. POSIX threads emulate Win32 threads.
 * Every fail-fast scenario runs in a forked child that must exit with 77.
 * This proves layout, initialization, isolation, error and ownership logic
 * only; it is not evidence of Windows loader or native unload behavior.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../../src/vgpu/clvk_runtime_tls.c"

#include <errno.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#undef malloc
#undef free

/** Test sizes; immutable and own no resources. */
typedef enum {
   ThreadCount = 8,
   RaceObjects = 6,
   FastFailStatus = 77,
} test_constants_t;

/** Heap-owned state behind one fake thread handle. */
struct fake_thread_t {
   /** Joinable POSIX thread, valid once started. */
   pthread_t thread;
   /** Guards resumed/terminated. */
   pthread_mutex_t mutex;
   /** Signals resumption. */
   pthread_cond_t cond;
   /** Nonzero after ResumeThread or TerminateThread. */
   int resumed;
   /** Nonzero when TerminateThread cancelled the routine. */
   int terminated;
   /** Routine result stored at thread exit. */
   DWORD exit_code;
   /** Fake id unique within this process. */
   DWORD id;
   /** Routine and its parameter. */
   LPTHREAD_START_ROUTINE routine;
   LPVOID parameter;
};

static __thread DWORD fake_current_id = 1;
static __thread DWORD fake_last_error;
static DWORD fake_next_id = 2;
static long fake_open_handles;
static long fake_live_allocations;
static int fail_next_malloc;
static int fail_next_create;
static int fail_next_resume;
static int fail_next_wait;
static int fail_next_exit_code;
/** Test barriers hold first use/foreign identities concurrently alive. */
static pthread_barrier_t race_barrier;
static pthread_barrier_t foreign_barrier;

#define Check(condition)                                                     \
   do {                                                                      \
      if (!(condition)) {                                                    \
         fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__,    \
                 #condition);                                                \
         exit(1);                                                            \
      }                                                                      \
   } while (0)

void AcquireSRWLockExclusive(SRWLOCK *lock) { Check(pthread_mutex_lock(&lock->mutex) == 0); }
void ReleaseSRWLockExclusive(SRWLOCK *lock) { Check(pthread_mutex_unlock(&lock->mutex) == 0); }
DWORD GetLastError(void) { return fake_last_error; }
DWORD GetCurrentThreadId(void) { return fake_current_id; }
DWORD GetThreadId(HANDLE thread) { return thread->id; }
void __fastfail(unsigned int code) { exit(code == FAST_FAIL_FATAL_APP_EXIT ? FastFailStatus : 1); }

void *
waddle_fake_malloc(size_t size)
{
   void *pointer;
   if (fail_next_malloc) {
      fail_next_malloc = 0;
      return NULL;
   }
   pointer = malloc(size);
   if (pointer)
      __atomic_add_fetch(&fake_live_allocations, 1, __ATOMIC_SEQ_CST);
   return pointer;
}

void
waddle_fake_free(void *pointer)
{
   if (pointer)
      __atomic_sub_fetch(&fake_live_allocations, 1, __ATOMIC_SEQ_CST);
   free(pointer);
}

static void *
fake_trampoline(void *parameter)
{
   struct fake_thread_t *thread = parameter;
   int terminated;
   Check(pthread_mutex_lock(&thread->mutex) == 0);
   while (!thread->resumed)
      Check(pthread_cond_wait(&thread->cond, &thread->mutex) == 0);
   terminated = thread->terminated;
   Check(pthread_mutex_unlock(&thread->mutex) == 0);
   fake_current_id = thread->id;
   if (!terminated)
      thread->exit_code = thread->routine(thread->parameter);
   return NULL;
}

HANDLE
CreateThread(void *attributes, size_t stack, LPTHREAD_START_ROUTINE routine,
             LPVOID parameter, DWORD flags, DWORD *thread_id)
{
   struct fake_thread_t *thread;
   (void)attributes;
   (void)stack;
   (void)thread_id;
   Check(flags == CREATE_SUSPENDED);
   if (fail_next_create) {
      fail_next_create = 0;
      fake_last_error = 1455;
      return NULL;
   }
   thread = calloc(1, sizeof(*thread));
   Check(thread != NULL);
   Check(pthread_mutex_init(&thread->mutex, NULL) == 0);
   Check(pthread_cond_init(&thread->cond, NULL) == 0);
   thread->routine = routine;
   thread->parameter = parameter;
   thread->id = __atomic_fetch_add(&fake_next_id, 1, __ATOMIC_SEQ_CST);
   Check(pthread_create(&thread->thread, NULL, fake_trampoline, thread) == 0);
   __atomic_add_fetch(&fake_open_handles, 1, __ATOMIC_SEQ_CST);
   return thread;
}

static void
fake_release(HANDLE thread, int terminate)
{
   Check(pthread_mutex_lock(&thread->mutex) == 0);
   thread->resumed = 1;
   thread->terminated = terminate;
   Check(pthread_cond_signal(&thread->cond) == 0);
   Check(pthread_mutex_unlock(&thread->mutex) == 0);
}

DWORD
ResumeThread(HANDLE thread)
{
   if (fail_next_resume) {
      fail_next_resume = 0;
      fake_last_error = 5;
      return (DWORD)-1;
   }
   fake_release(thread, 0);
   return 1;
}

int
TerminateThread(HANDLE thread, DWORD exit_code)
{
   thread->exit_code = exit_code;
   fake_release(thread, 1);
   return 1;
}

DWORD
WaitForSingleObject(HANDLE thread, DWORD milliseconds)
{
   Check(milliseconds == INFINITE);
   Check(pthread_join(thread->thread, NULL) == 0);
   if (fail_next_wait) {
      fail_next_wait = 0;
      fake_last_error = 6;
      return WAIT_FAILED;
   }
   return WAIT_OBJECT_0;
}

int
GetExitCodeThread(HANDLE thread, DWORD *exit_code)
{
   if (fail_next_exit_code) {
      fail_next_exit_code = 0;
      fake_last_error = 87;
      return 0;
   }
   *exit_code = thread->exit_code;
   return 1;
}

int
CloseHandle(HANDLE thread)
{
   Check(pthread_cond_destroy(&thread->cond) == 0);
   Check(pthread_mutex_destroy(&thread->mutex) == 0);
   free(thread);
   __atomic_sub_fetch(&fake_open_handles, 1, __ATOMIC_SEQ_CST);
   return 1;
}

/** Run @p scenario in a child; require exit status 77 (fail-fast). */
static void
expect_fast_fail(void (*scenario)(void))
{
   int status = 0;
   pid_t child;
   fflush(NULL);
   child = fork();
   Check(child >= 0);
   if (child == 0) {
      scenario();
      exit(2);
   }
   Check(waitpid(child, &status, 0) == child);
   Check(WIFEXITED(status) && WEXITSTATUS(status) == FastFailStatus);
}

static const unsigned char template_bytes[16] = {
   1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16
};

static void
test_layout_and_initialization(void)
{
   emutls_object_t byte_object = { 1, 1, 0, NULL };
   emutls_object_t aligned_object = { 16, 16, 0, template_bytes };
   emutls_object_t word_object = { 4, 4, 0, template_bytes };
   emutls_object_t empty_object = { 0, 0, 0, NULL };
   unsigned char *byte_address = waddle_test___emutls_get_address(&byte_object);
   unsigned char *aligned_address = waddle_test___emutls_get_address(&aligned_object);
   unsigned char *word_address = waddle_test___emutls_get_address(&word_object);
   unsigned char *empty_address = waddle_test___emutls_get_address(&empty_object);

   Check(((uintptr_t)aligned_address % 16) == 0);
   Check(((uintptr_t)word_address % 4) == 0);
   Check(*byte_address == 0);
   Check(memcmp(aligned_address, template_bytes, 16) == 0);
   Check(memcmp(word_address, template_bytes, 4) == 0);
   Check(aligned_address >= byte_address + 1);
   Check(word_address >= aligned_address + 16);
   Check(empty_address >= word_address + 4);
   *byte_address = 0x5a;
   word_address[0] = 0x77;
   Check(waddle_test___emutls_get_address(&byte_object) == byte_address);
   Check(*byte_address == 0x5a);
   Check(waddle_test___emutls_get_address(&word_object) == word_address);
   Check(word_address[0] == 0x77);
   Check(byte_object.location != 0 && aligned_object.location != 0);
   Check(fake_live_allocations == 0);
}

static emutls_object_t race_objects[RaceObjects] = {
   { 8, 8, 0, template_bytes }, { 4, 4, 0, NULL }, { 16, 16, 0, template_bytes },
   { 2, 2, 0, NULL }, { 8, 8, 0, NULL }, { 1, 1, 0, template_bytes },
};

/** Wait on a test-owned barrier; no resource is transferred. */
static void
barrier_wait(pthread_barrier_t *barrier)
{
   int status = pthread_barrier_wait(barrier);
   Check(status == 0 || status == PTHREAD_BARRIER_SERIAL_THREAD);
}

static void *
race_worker(void *parameter)
{
   uintptr_t token = (uintptr_t)parameter;
   unsigned char *addresses[RaceObjects];
   if (token != 0x33)
      barrier_wait(&race_barrier);
   for (unsigned index = 0; index < RaceObjects; index++) {
      addresses[index] = waddle_test___emutls_get_address(&race_objects[index]);
      if (race_objects[index].templ)
         Check(memcmp(addresses[index], template_bytes, race_objects[index].size) == 0);
      else
         for (uintptr_t byte = 0; byte < race_objects[index].size; byte++)
            Check(addresses[index][byte] == 0);
      memset(addresses[index], (int)token, race_objects[index].size);
   }
   for (unsigned index = 0; index < RaceObjects; index++) {
      Check(waddle_test___emutls_get_address(&race_objects[index]) == addresses[index]);
      for (uintptr_t byte = 0; byte < race_objects[index].size; byte++)
         Check(addresses[index][byte] == (unsigned char)token);
   }
   return NULL;
}

static void
test_concurrent_first_use_and_isolation(void)
{
   pthread_t threads[ThreadCount];
   uintptr_t layout[RaceObjects];
   Check(pthread_barrier_init(&race_barrier, NULL, ThreadCount + 1) == 0);
   for (uintptr_t index = 0; index < ThreadCount; index++)
      Check(pthread_create(&threads[index], NULL, race_worker, (void *)(index + 0x10)) == 0);
   barrier_wait(&race_barrier);
   for (unsigned index = 0; index < ThreadCount; index++)
      Check(pthread_join(threads[index], NULL) == 0);
   Check(pthread_barrier_destroy(&race_barrier) == 0);
   for (unsigned index = 0; index < RaceObjects; index++) {
      layout[index] = race_objects[index].location;
      Check(layout[index] != 0);
      for (unsigned other = 0; other < index; other++) {
         size_t a = layout[index] >> RuntimeTlsSlotBits;
         size_t b = layout[other] >> RuntimeTlsSlotBits;
         Check((layout[index] & 0xff) != (layout[other] & 0xff));
         Check(a + race_objects[index].size <= b || b + race_objects[other].size <= a ||
               race_objects[index].size == 0 || race_objects[other].size == 0);
      }
   }
   race_worker((void *)(uintptr_t)0x33);
   Check(fake_live_allocations == 0);
}

static void
test_assignment_is_idempotent(void)
{
   /* Deterministically covers the locked re-check that a racing first use
    * observes after another thread published the location word. */
   emutls_object_t object = { 8, 8, 0, NULL };
   uintptr_t first = runtime_tls_assign(&object);
   Check(first != 0 && object.location == first);
   Check(runtime_tls_assign(&object) == first);
}

static void
test_register_common(void)
{
   emutls_object_t object = { 4, 4, 0, template_bytes };
   waddle_test___emutls_register_common(&object, 8, 2, NULL);
   Check(object.size == 8 && object.align == 4 && object.templ == NULL);
   waddle_test___emutls_register_common(&object, 8, 8, template_bytes);
   Check(object.size == 8 && object.align == 8 && object.templ == template_bytes);
   waddle_test___emutls_register_common(&object, 4, 1, template_bytes + 1);
   Check(object.size == 8 && object.align == 8 && object.templ == template_bytes);
}

static void scenario_alignment_too_large(void)
{
   emutls_object_t object = { 32, 32, 0, NULL };
   (void)waddle_test___emutls_get_address(&object);
}

static void scenario_alignment_not_power_of_two(void)
{
   emutls_object_t object = { 3, 3, 0, NULL };
   (void)waddle_test___emutls_get_address(&object);
}

static void scenario_object_too_large(void)
{
   emutls_object_t object = { RuntimeTlsArenaBytes + 1, 1, 0, NULL };
   (void)waddle_test___emutls_get_address(&object);
}

static void scenario_arena_overflow(void)
{
   static emutls_object_t first = { RuntimeTlsArenaBytes - 8, 8, 0, NULL };
   static emutls_object_t second = { 16, 8, 0, NULL };
   (void)waddle_test___emutls_get_address(&first);
   (void)waddle_test___emutls_get_address(&second);
}

static void scenario_slot_exhaustion(void)
{
   static emutls_object_t objects[RuntimeTlsSlotCapacity + 1];
   for (unsigned index = 0; index <= RuntimeTlsSlotCapacity; index++) {
      objects[index].size = 1;
      objects[index].align = 1;
      (void)waddle_test___emutls_get_address(&objects[index]);
   }
}

static void scenario_size_grown_after_assignment(void)
{
   static emutls_object_t object = { 4, 4, 0, NULL };
   (void)waddle_test___emutls_get_address(&object);
   waddle_test___emutls_register_common(&object, 8, 4, NULL);
   (void)waddle_test___emutls_get_address(&object);
}

static void scenario_alignment_grown_after_assignment(void)
{
   emutls_object_t object = { 4, 1, 0, NULL };
   (void)waddle_test___emutls_get_address(&object);
   waddle_test___emutls_register_common(&object, 4, 8, NULL);
   (void)waddle_test___emutls_get_address(&object);
}

static void scenario_null_control(void)
{
   (void)waddle_test___emutls_get_address(NULL);
}

static void scenario_null_common(void)
{
   waddle_test___emutls_register_common(NULL, 0, 0, NULL);
}

static void scenario_zero_tag(void)
{
   emutls_object_t object = { 1, 1, (uintptr_t)1 << RuntimeTlsSlotBits, NULL };
   (void)waddle_test___emutls_get_address(&object);
}

static void scenario_large_tag(void)
{
   emutls_object_t object = { 1, 1, RuntimeTlsSlotCapacity + 1, NULL };
   (void)waddle_test___emutls_get_address(&object);
}

static void scenario_offset_outside_arena(void)
{
   emutls_object_t object = { 1, 1, 0, NULL };
   (void)waddle_test___emutls_get_address(&object);
   object.location = ((uintptr_t)(RuntimeTlsArenaBytes + 1) << RuntimeTlsSlotBits) | 1;
   (void)waddle_test___emutls_get_address(&object);
}

static void scenario_offset_misaligned(void)
{
   emutls_object_t object = { 8, 8, 0, NULL };
   (void)waddle_test___emutls_get_address(&object);
   object.location = ((uintptr_t)1 << RuntimeTlsSlotBits) | 1;
   (void)waddle_test___emutls_get_address(&object);
}

static void scenario_offset_changed(void)
{
   emutls_object_t object = { 1, 1, 0, NULL };
   (void)waddle_test___emutls_get_address(&object);
   object.location = ((uintptr_t)16 << RuntimeTlsSlotBits) | 1;
   (void)waddle_test___emutls_get_address(&object);
}

static void scenario_foreign_slot(void)
{
   emutls_object_t owner = { 4, 4, 0, NULL };
   emutls_object_t foreign = { 4, 4, 0, NULL };
   (void)waddle_test___emutls_get_address(&owner);
   foreign.location = owner.location;
   (void)waddle_test___emutls_get_address(&foreign);
}

static void scenario_record_null(void)
{
   (void)runtime_tls_checked_record(NULL, 0, sizeof(runtime_tls_record_t));
}

static void scenario_record_misaligned(void)
{
   (void)runtime_tls_checked_record((unsigned char *)&runtime_tls_fake_record + 1,
                                    0, sizeof(runtime_tls_record_t));
}

static void scenario_record_offset_misaligned(void)
{
   (void)runtime_tls_checked_record((unsigned char *)&runtime_tls_fake_record,
                                    1, sizeof(runtime_tls_record_t) + 1);
}

static void scenario_record_offset_outside(void)
{
   (void)runtime_tls_checked_record((unsigned char *)&runtime_tls_fake_record,
                                    sizeof(runtime_tls_record_t), 0);
}

static void scenario_record_short(void)
{
   (void)runtime_tls_checked_record((unsigned char *)&runtime_tls_fake_record,
                                    0, sizeof(runtime_tls_record_t) - 1);
}

static void scenario_record_address_overflow(void)
{
   (void)runtime_tls_checked_record((unsigned char *)(UINTPTR_MAX & ~(uintptr_t)15),
                                    16, sizeof(runtime_tls_record_t) + 16);
}

/** Exact-fit positive controls precede no other assignments in this process. */
static void test_exact_boundaries(void)
{
   emutls_object_t object = { RuntimeTlsArenaBytes, 16, 0, NULL };
   unsigned char *bytes = waddle_test___emutls_get_address(&object);
   for (size_t index = 0; index < RuntimeTlsArenaBytes; index++)
      Check(bytes[index] == 0);
   bytes[RuntimeTlsArenaBytes - 1] = 0x7f;
   Check(*(unsigned char *)waddle_test___emutls_get_address(&object) == 0);
}

/** Prove slot capacity is inclusive rather than merely expecting overflow. */
static void test_exact_slot_capacity(void)
{
   emutls_object_t objects[RuntimeTlsSlotCapacity] = { 0 };
   for (unsigned index = 0; index < RuntimeTlsSlotCapacity; index++) {
      objects[index].size = 1;
      objects[index].align = 1;
      unsigned char *address = waddle_test___emutls_get_address(&objects[index]);
      *address = (unsigned char)(index + 1);
   }
   for (unsigned index = 0; index < RuntimeTlsSlotCapacity; index++)
      Check(*(unsigned char *)waddle_test___emutls_get_address(&objects[index]) == index + 1);
}

static void
test_fail_fast_paths(void)
{
   expect_fast_fail(scenario_alignment_too_large);
   expect_fast_fail(scenario_alignment_not_power_of_two);
   expect_fast_fail(scenario_object_too_large);
   expect_fast_fail(scenario_arena_overflow);
   expect_fast_fail(scenario_slot_exhaustion);
   expect_fast_fail(scenario_size_grown_after_assignment);
   expect_fast_fail(scenario_alignment_grown_after_assignment);
   expect_fast_fail(scenario_null_control);
   expect_fast_fail(scenario_null_common);
   expect_fast_fail(scenario_zero_tag);
   expect_fast_fail(scenario_large_tag);
   expect_fast_fail(scenario_offset_outside_arena);
   expect_fast_fail(scenario_offset_misaligned);
   expect_fast_fail(scenario_offset_changed);
   expect_fast_fail(scenario_foreign_slot);
   expect_fast_fail(scenario_record_null);
   expect_fast_fail(scenario_record_misaligned);
   expect_fast_fail(scenario_record_offset_misaligned);
   expect_fast_fail(scenario_record_offset_outside);
   expect_fast_fail(scenario_record_short);
   expect_fast_fail(scenario_record_address_overflow);
}

static HANDLE observed_self;

static void *
gthread_routine(void *argument)
{
   observed_self = waddle_test___gthr_win32_self();
   return (void *)((uintptr_t)argument + 1);
}

static void *
foreign_identity(void *argument)
{
   HANDLE *slot = argument;
   *slot = waddle_test___gthr_win32_self();
   Check(*slot == waddle_test___gthr_win32_self());
   barrier_wait(&foreign_barrier);
   barrier_wait(&foreign_barrier);
   return NULL;
}

static void
test_gthreads(void)
{
   HANDLE thread = NULL;
   HANDLE foreign[2];
   pthread_t workers[2];
   void *value = NULL;
   HANDLE main_identity = waddle_test___gthr_win32_self();

   Check(((uintptr_t)main_identity & 1) == 1);
   Check(main_identity == waddle_test___gthr_win32_self());
   Check(waddle_test___gthr_win32_create(&thread, gthread_routine, (void *)41) == 0);
   Check(waddle_test___gthr_win32_join(thread, &value) == 0);
   Check(value == (void *)42 && observed_self == thread);
   Check(fake_open_handles == 0 && fake_live_allocations == 0);

   Check(waddle_test___gthr_win32_create(&thread, gthread_routine, NULL) == 0);
   Check(waddle_test___gthr_win32_join(thread, NULL) == 0);

   Check(pthread_barrier_init(&foreign_barrier, NULL, 3) == 0);
   for (unsigned index = 0; index < 2; index++)
      Check(pthread_create(&workers[index], NULL, foreign_identity, &foreign[index]) == 0);
   barrier_wait(&foreign_barrier);
   Check(foreign[0] != foreign[1] && foreign[0] != main_identity);
   barrier_wait(&foreign_barrier);
   for (unsigned index = 0; index < 2; index++)
      Check(pthread_join(workers[index], NULL) == 0);
   Check(pthread_barrier_destroy(&foreign_barrier) == 0);
   Check(foreign[0] != foreign[1] && foreign[0] != main_identity);
   Check(((uintptr_t)foreign[0] & 1) == 1 && ((uintptr_t)foreign[1] & 1) == 1);

   fail_next_malloc = 1;
   Check(waddle_test___gthr_win32_create(&thread, gthread_routine, NULL) ==
         (int)ERROR_NOT_ENOUGH_MEMORY);
   fail_next_create = 1;
   Check(waddle_test___gthr_win32_create(&thread, gthread_routine, NULL) == 1455);
   fail_next_resume = 1;
   Check(waddle_test___gthr_win32_create(&thread, gthread_routine, NULL) == 5);
   Check(fake_open_handles == 0 && fake_live_allocations == 0);

   Check(waddle_test___gthr_win32_create(&thread, gthread_routine, NULL) == 0);
   fail_next_wait = 1;
   Check(waddle_test___gthr_win32_join(thread, &value) == 6);
   Check(waddle_test___gthr_win32_create(&thread, gthread_routine, NULL) == 0);
   fail_next_exit_code = 1;
   Check(waddle_test___gthr_win32_join(thread, &value) == 87);
   Check(fake_open_handles == 0 && fake_live_allocations == 0);
}

static void *
self_join_routine(void *argument)
{
   HANDLE own = waddle_test___gthr_win32_self();
   (void)argument;
   Check(waddle_test___gthr_win32_join(own, NULL) == 1);
   return NULL;
}

static void
test_self_join_keeps_handle(void)
{
   HANDLE thread = NULL;
   Check(waddle_test___gthr_win32_create(&thread, self_join_routine, NULL) == 0);
   Check(waddle_test___gthr_win32_join(thread, NULL) == 0);
   Check(fake_open_handles == 0 && fake_live_allocations == 0);
}

/** Run each group in a fresh process; no previous layout can mask a boundary. */
static void
run_clean_case(void (*body)(void))
{
   int status;
   pid_t child = fork();
   Check(child >= 0);
   if (child == 0) {
      body();
      Check(fake_open_handles == 0 && fake_live_allocations == 0);
      exit(0);
   }
   Check(waitpid(child, &status, 0) == child);
   Check(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int
main(void)
{
   run_clean_case(test_layout_and_initialization);
   run_clean_case(test_concurrent_first_use_and_isolation);
   run_clean_case(test_assignment_is_idempotent);
   run_clean_case(test_register_common);
   run_clean_case(test_gthreads);
   run_clean_case(test_self_join_keeps_handle);
   run_clean_case(test_exact_boundaries);
   run_clean_case(test_exact_slot_capacity);
   run_clean_case(test_fail_fast_paths);
   printf("CLVK_RUNTIME_TLS_TEST PASS\n");
   return 0;
}
