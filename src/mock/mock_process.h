/**
 * @file mock_process.h
 * @brief Subprocess lifecycle management for Linux mock guest agent.
 */

#ifndef WaddleMockProcessH
#define WaddleMockProcessH

#include "common.h"
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Represents a spawned mock guest child process and its associated I/O descriptors.
 */
typedef struct mock_process_t {
    /** @brief Process ID of the spawned child process. */
    pid_t pid;
    /** @brief Writable input file descriptor (STDIN pipe or PTY master). */
    int input;
    /** @brief Readable output file descriptors (0 = STDOUT / PTY, 1 = STDERR). */
    int output[2];
    /** @brief Non-zero if child was allocated inside a pseudo terminal. */
    int interactive;
    /** @brief Non-zero once child has been reaped via waitpid. */
    int reaped;
    /** @brief Exit status recorded upon process termination. */
    int status;
    /** @brief Monotonic startup timestamp in milliseconds. */
    uint64_t started;
} mock_process_t;

/* Backward compatibility typedef alias */
typedef mock_process_t mock_process;

/**
 * @brief Parses a WaddleMsgSpawnReq payload and spawns the requested child process.
 *
 * Configures process isolation, pipes, pseudo terminal (if interactive), environment,
 * and working directory.
 *
 * @param[out] p      Non-null pointer to mock_process_t to populate.
 * @param[in]  body   Raw wire payload received with WaddleMsgSpawnReq.
 * @param[in]  length Total byte length of spawn payload.
 * @return 0 on successful process spawn, or -1 on failure (errno set).
 */
int mock_spawn(mock_process_t *p, uint8_t *body, size_t length);

/**
 * @brief Non-blockingly queries and reaps the child process status via waitpid.
 *
 * @param[in,out] p Non-null pointer to mock_process_t.
 * @return 1 if child has terminated, 0 if still running, or -1 on waitpid error.
 */
int mock_reap(mock_process_t *p);

/**
 * @brief Forcibly terminates the child process group and closes all associated descriptors.
 *
 * @param[in,out] p Pointer to mock_process_t to terminate and release.
 */
void mock_stop(mock_process_t *p);

#ifdef __cplusplus
}
#endif

#endif /* WaddleMockProcessH */
