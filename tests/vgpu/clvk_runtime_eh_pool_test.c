/** @file clvk_runtime_eh_pool_test.c
 * Actual final-callback implementation against a bounded CRT registration double.
 * No Windows CRT ordering or real exception arena retirement is inferred here.
 */
#ifdef NDEBUG
#error "Lifecycle assertions must never be disabled"
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define WaddleRuntimeEhPoolTest
#include "../../src/vgpu/clvk_runtime_eh_pool.c"

/** Test-owned bounded registration stack; never allocates. */
static void (*callbacks[8])(void);
static size_t callback_count;
static unsigned sequence;
static unsigned frees;
static int registration_error;

/** Borrow callback until fake CRT retirement; nonzero leaves it unregistered. */
int
waddle_test_runtime_atexit(void (*callback)(void))
{
   if (registration_error)
      return registration_error;
   assert(callback && callback_count < 8);
   callbacks[callback_count++] = callback;
   return 0;
}

/** Require every registered C++ destructor to have completed before release. */
void
waddle_test_runtime_freeres(void)
{
   assert(sequence == 3 && frees == 0);
   assert(runtime_eh_state == RuntimeEhReleased);
   frees++;
}

/** Exact fatal status, distinct from assertion, signal or ordinary failure.
 * exit flushes host gcov/LSan; fake registrations never enter host atexit.
 * Production uses __fastfail and does not execute host CRT teardown. */
void __attribute__((noreturn))
waddle_test_runtime_fastfail(void)
{
   exit(77);
}

static void last_constructed_destructor(void) { assert(sequence == 0); sequence = 1; }
static void middle_destructor(void) { assert(sequence == 1); sequence = 2; }
static void first_constructed_destructor(void) { assert(sequence == 2); sequence = 3; }

static void ordered_retirement(void)
{
   assert(waddle_clvk_eh_pool_initializer == waddle_clvk_eh_pool_register);
   waddle_clvk_eh_pool_initializer();
   assert(callback_count == 1 && callbacks[0] == waddle_clvk_eh_pool_release);
   assert(runtime_eh_state == RuntimeEhRegistered && frees == 0);
   assert(waddle_test_runtime_atexit(first_constructed_destructor) == 0);
   assert(waddle_test_runtime_atexit(middle_destructor) == 0);
   assert(waddle_test_runtime_atexit(last_constructed_destructor) == 0);
   while (callback_count)
      callbacks[--callback_count]();
   assert(sequence == 3 && frees == 1 && runtime_eh_state == RuntimeEhReleased);
}

static void registration_failure(void)
{
   registration_error = 1;
   waddle_clvk_eh_pool_register();
}

static void repeated_registration(void)
{
   waddle_clvk_eh_pool_register();
   waddle_clvk_eh_pool_register();
}

static void release_without_registration(void) { waddle_clvk_eh_pool_release(); }

static void repeated_release(void)
{
   waddle_clvk_eh_pool_register();
   sequence = 3;
   waddle_clvk_eh_pool_release();
   waddle_clvk_eh_pool_release();
}

static void register_after_release(void)
{
   waddle_clvk_eh_pool_register();
   sequence = 3;
   waddle_clvk_eh_pool_release();
   waddle_clvk_eh_pool_register();
}

/** Every control inherits clean state; fatal paths must exit with status 77. */
static void run_case(void (*routine)(void), int expected)
{
   pid_t child = fork();
   int status;
   assert(child >= 0);
   if (child == 0) {
      routine();
      exit(0);
   }
   assert(waitpid(child, &status, 0) == child);
   assert(WIFEXITED(status) && WEXITSTATUS(status) == expected);
}

int main(void)
{
   run_case(ordered_retirement, 0);
   run_case(registration_failure, 77);
   run_case(repeated_registration, 77);
   run_case(release_without_registration, 77);
   run_case(repeated_release, 77);
   run_case(register_after_release, 77);
   puts("CLVK_RUNTIME_EH_POOL_TEST PASS cases=6");
   return 0;
}
