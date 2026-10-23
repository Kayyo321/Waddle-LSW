/** @file opencl_transport_gate.c @brief Qualification-only authenticated cancel gate. */
#include "opencl_transport_gate.h"

#if defined(WaddleOpenclTransportGate)
#if defined(_WIN32)
#error WaddleOpenclTransportGate requires the POSIX qualification harness.
#endif

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define GatePathCapacity 4096u
#define GateWitnessCapacity 160u

static int gate_copy_path(char path[GatePathCapacity])
{
    const char *configured=getenv("WADDLE_OPENCL_CANCEL_GATE");
    size_t length=0;
    if (!configured || configured[0]!='/') return 0;
    while (length<GatePathCapacity && configured[length]) {
        unsigned char value=(unsigned char)configured[length];
        if (value<0x20 || value==0x7f) return 0;
        ++length;
    }
    if (!length || length>=GatePathCapacity) return 0;
    memcpy(path,configured,length+1);
    return 1;
}

static venus_ring_status_t gate_publish(const char *path,uint64_t session,uint64_t sequence,
    uint32_t opcode)
{
    char witness[GateWitnessCapacity];
    int length=snprintf(witness,sizeof witness,
        "WADDLE_OPENCL_CANCEL_GATE_V1\nsession=%016" PRIx64 "\nsequence=%" PRIu64
        "\nopcode=%" PRIu32 "\npid=%ld\n",session,sequence,opcode,(long)getpid());
    if (length<0 || (size_t)length>=sizeof witness) return RingClosed;

    int descriptor=open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if (descriptor<0) return RingClosed;
    venus_ring_status_t status=RingClosed;
    struct stat info;
    if (fchmod(descriptor,0600)<0 || fstat(descriptor,&info)<0 ||
        !S_ISREG(info.st_mode) || (info.st_mode&0777)!=0600) goto cleanup;
    size_t written=0;
    while (written<(size_t)length) {
        ssize_t amount=write(descriptor,witness+written,(size_t)length-written);
        if (amount<0 && errno==EINTR) continue;
        if (amount<=0) goto cleanup;
        written+=(size_t)amount;
    }
    status=RingOk;
cleanup:
    if (close(descriptor)<0) status=RingClosed;
    if (status!=RingOk) (void)unlink(path);
    memset(witness,0,sizeof witness);
    return status;
}

venus_ring_status_t venus_opencl_transport_gate_wait(uint64_t session,uint64_t sequence,
    uint32_t opcode,uint64_t deadline_ms,const _Atomic uint32_t *cancel)
{
    char path[GatePathCapacity];
    memset(path,0,sizeof path);
    if (!session || !sequence || opcode!=18 || !deadline_ms || !cancel)
        return RingInvalid;
    if (!gate_copy_path(path)) return RingInvalid;
    venus_ring_status_t status=gate_publish(path,session,sequence,opcode);
    memset(path,0,sizeof path);
    if (status!=RingOk) return status;

    for (;;) {
        if (atomic_load_explicit(cancel,memory_order_acquire)!=0) return RingCancelled;
        uint64_t now=venus_tcp_now_ms();
        if (!now) return RingClosed;
        if (now>=deadline_ms) return RingTimeout;
        int result=poll(NULL,0,1);
        if (result<0 && errno!=EINTR) return RingClosed;
    }
}

#undef GateWitnessCapacity
#undef GatePathCapacity
#endif
