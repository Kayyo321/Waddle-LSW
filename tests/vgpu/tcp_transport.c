/** @file tcp_transport.c @brief Real loopback TCP ownership/deadline/error tests. */
#include "waddle/venus_tcp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#endif
#ifdef TcpFaultTests
/* Inject one real native failure before publication; all successful native
 * resources still use the OS and production ownership cleanup paths. */
typedef enum fault_t { FaultNone, FaultSocket, FaultTcpOption, FaultFcntlGet, FaultFcntlSet,
    FaultFdGet, FaultFdSet, FaultBind, FaultListen, FaultName, FaultConnect,
    FaultSocketError, FaultAccept, FaultPoll, FaultSend, FaultReceive,
    FaultRandom, FaultClock, FaultConnectProgress, FaultPollInterrupted,
    FaultReceiveInterrupted, FaultSendAgain, FaultShutdown } fault_t;
static fault_t fault;
static int should_fail(fault_t wanted)
{
    if (fault!=wanted) return 0;
    fault=FaultNone; errno=EIO; return 1;
}
int __real_socket(int domain, int type, int protocol);
int __wrap_socket(int domain, int type, int protocol)
{ return should_fail(FaultSocket) ? -1 : __real_socket(domain,type,protocol); }
int __real_setsockopt(int fd,int level,int option,const void *value,socklen_t extent);
/** @brief Inject native socket-option failure with real publication cleanup.
 * @param[in] fd/level/option/value/extent Borrowed native socket option arguments.
 * @return Minus1/EIO once for FaultTcpOption; otherwise real OS result.
 * @note Sole fault-test thread; pointer never retained, no allocation/ownership.
 */
int __wrap_setsockopt(int fd,int level,int option,const void *value,socklen_t extent)
{ return should_fail(FaultTcpOption) ? -1 : __real_setsockopt(fd,level,option,value,extent); }
int __real_fcntl(int fd, int command, ...);
int __wrap_fcntl(int fd, int command, ...)
{
    int value=0;
    if (command==F_SETFL || command==F_SETFD) {
        __builtin_va_list arguments; __builtin_va_start(arguments,command);
        value=__builtin_va_arg(arguments,int); __builtin_va_end(arguments);
    }
    fault_t wanted=command==F_GETFL ? FaultFcntlGet : command==F_SETFL ? FaultFcntlSet : command==F_GETFD ? FaultFdGet : FaultFdSet;
    return should_fail(wanted) ? -1 : __real_fcntl(fd,command,value);
}
int __real_bind(int fd,const struct sockaddr *address,socklen_t extent);
int __wrap_bind(int fd,const struct sockaddr *address,socklen_t extent)
{ return should_fail(FaultBind) ? -1 : __real_bind(fd,address,extent); }
int __real_listen(int fd,int backlog);
int __wrap_listen(int fd,int backlog)
{ return should_fail(FaultListen) ? -1 : __real_listen(fd,backlog); }
int __real_getsockname(int fd,struct sockaddr *address,socklen_t *extent);
int __wrap_getsockname(int fd,struct sockaddr *address,socklen_t *extent)
{ return should_fail(FaultName) ? -1 : __real_getsockname(fd,address,extent); }
int __real_connect(int fd,const struct sockaddr *address,socklen_t extent);
int __wrap_connect(int fd,const struct sockaddr *address,socklen_t extent)
{
    if (should_fail(FaultConnect)) return -1;
    if (should_fail(FaultConnectProgress)) { errno=EINPROGRESS; return -1; }
    return __real_connect(fd,address,extent);
}
int __real_getsockopt(int fd,int level,int option,void *value,socklen_t *extent);
int __wrap_getsockopt(int fd,int level,int option,void *value,socklen_t *extent)
{ return should_fail(FaultSocketError) ? -1 : __real_getsockopt(fd,level,option,value,extent); }
int __real_accept(int fd,struct sockaddr *address,socklen_t *extent);
int __wrap_accept(int fd,struct sockaddr *address,socklen_t *extent)
{ return should_fail(FaultAccept) ? -1 : __real_accept(fd,address,extent); }
int __real_poll(struct pollfd *descriptors,nfds_t count,int timeout);
int __wrap_poll(struct pollfd *descriptors,nfds_t count,int timeout)
{
    if (should_fail(FaultPoll)) return -1;
    if (should_fail(FaultPollInterrupted)) { errno=EINTR; return -1; }
    return __real_poll(descriptors,count,timeout);
}
ssize_t __real_send(int fd,const void *bytes,size_t length,int flags);
ssize_t __wrap_send(int fd,const void *bytes,size_t length,int flags)
{
    if (should_fail(FaultSend)) return -1;
    if (should_fail(FaultSendAgain)) { errno=EAGAIN; return -1; }
    return __real_send(fd,bytes,length,flags);
}
ssize_t __real_recv(int fd,void *bytes,size_t length,int flags);
ssize_t __wrap_recv(int fd,void *bytes,size_t length,int flags)
{
    if (should_fail(FaultReceive)) return -1;
    if (should_fail(FaultReceiveInterrupted)) { errno=EINTR; return -1; }
    return __real_recv(fd,bytes,length,flags);
}
ssize_t __real_getrandom(void *bytes,size_t length,unsigned flags);
ssize_t __wrap_getrandom(void *bytes,size_t length,unsigned flags)
{ return should_fail(FaultRandom) ? -1 : __real_getrandom(bytes,length,flags); }
int __real_shutdown(int fd,int direction);
int __wrap_shutdown(int fd,int direction)
{ return should_fail(FaultShutdown) ? -1 : __real_shutdown(fd,direction); }
int __real_clock_gettime(clockid_t clock_id,struct timespec *now);
int __wrap_clock_gettime(clockid_t clock_id,struct timespec *now)
{ return should_fail(FaultClock) ? -1 : __real_clock_gettime(clock_id,now); }
#endif
static unsigned resource_count(void)
{
#ifdef _WIN32
    DWORD count=0;
    assert(GetProcessHandleCount(GetCurrentProcess(), &count));
    return (unsigned)count;
#else
    DIR *directory=opendir("/proc/self/fd"); assert(directory);
    unsigned count=0;
    struct dirent *entry;
    while ((entry=readdir(directory))) if (strcmp(entry->d_name,".") && strcmp(entry->d_name,"..")) ++count;
    assert(!closedir(directory)); return count;
#endif
}
/** @brief Verify immediate request/response socket behavior on a live borrowed owner.
 * @param[in] socket Nonnull connected borrowed native owner.
 * @note Sole test thread; no handle reference or option storage retained.
 */
static void no_delay_enabled(const venus_tcp_socket_t *socket)
{
    int enabled=0;
#ifdef _WIN32
    int extent=sizeof enabled;
    assert(!getsockopt((SOCKET)socket->handle,IPPROTO_TCP,TCP_NODELAY,(char *)&enabled,&extent));
#else
    socklen_t extent=sizeof enabled;
    assert(!getsockopt((int)socket->handle,IPPROTO_TCP,TCP_NODELAY,&enabled,&extent));
#endif
    assert(enabled==1);
}
static void pair(venus_tcp_socket_t *client, venus_tcp_socket_t *server)
{
    venus_tcp_socket_t listener={0}; uint32_t port=0;
    assert(venus_tcp_socket_listen(&listener,0,&port)==RingOk && port);
    assert(venus_tcp_socket_connect(client,"127.0.0.1",port,venus_tcp_now_ms()+1000,NULL)==RingOk);
    assert(venus_tcp_socket_accept(&listener,server,venus_tcp_now_ms()+1000,NULL)==RingOk);
    no_delay_enabled(client);no_delay_enabled(server);
    venus_tcp_socket_close(&listener);
}
static void normal_cycle(void)
{
    venus_tcp_socket_t client={0},server={0}; pair(&client,&server);
    uint8_t input[VenusTcpMaxCommandBytes],output[VenusTcpMaxCommandBytes];
    for (size_t index=0;index<sizeof input;++index) input[index]=(uint8_t)(index*17+3);
    size_t received=99;
    assert(venus_tcp_socket_send(&client,input,sizeof input,venus_tcp_now_ms()+1000,NULL)==RingOk);
    assert(venus_tcp_socket_receive(&server,output,sizeof output,&received,venus_tcp_now_ms()+1000,NULL)==RingOk);
    assert(received==sizeof output && !memcmp(input,output,sizeof input));
    assert(venus_tcp_socket_send(&server,input,32,venus_tcp_now_ms()+1000,NULL)==RingOk);
    assert(venus_tcp_socket_receive(&client,output,32,&received,venus_tcp_now_ms()+1000,NULL)==RingOk && received==32);
    assert(!memcmp(input,output,32));
    assert(venus_tcp_socket_send(&client,NULL,0,0,NULL)==RingOk);
    assert(venus_tcp_socket_receive(&server,NULL,0,&received,0,NULL)==RingOk && !received);
    assert(venus_tcp_socket_shutdown_write(&client)==RingOk);
    assert(venus_tcp_socket_receive(&server,output,1,&received,venus_tcp_now_ms()+1000,NULL)==RingClosed && !received && server.received_eof);
    /* A write-half-close retains reads for the real receiver retirement Ack. */
    assert(venus_tcp_socket_send(&server,input,32,venus_tcp_now_ms()+1000,NULL)==RingOk);
    assert(venus_tcp_socket_receive(&client,output,32,&received,venus_tcp_now_ms()+1000,NULL)==RingOk && !memcmp(input,output,32));
    venus_tcp_socket_close(&server); venus_tcp_socket_close(&client);
    venus_tcp_socket_close(&server); venus_tcp_socket_close(&client);
    assert(!server.initialized && !client.initialized);
}
static void terminal_tests(void)
{
    venus_tcp_socket_t client={0},server={0}; pair(&client,&server);
    uint8_t bytes[8]={1,2,3,4,5,6,7,8}; size_t received=0;
    _Atomic uint32_t cancelled=0;
    assert(venus_tcp_socket_receive(&server,bytes,8,&received,venus_tcp_now_ms()+20,&cancelled)==RingTimeout && !received && !server.received_eof);
    assert(venus_tcp_socket_send(&client,bytes,3,venus_tcp_now_ms()+1000,NULL)==RingOk);
    assert(venus_tcp_socket_receive(&server,bytes,8,&received,venus_tcp_now_ms()+20,NULL)==RingTimeout && received==3);
    assert(venus_tcp_socket_send(&client,bytes,3,venus_tcp_now_ms()+1000,NULL)==RingOk);
    assert(venus_tcp_socket_shutdown_write(&client)==RingOk);
    assert(venus_tcp_socket_receive(&server,bytes,8,&received,venus_tcp_now_ms()+1000,NULL)==RingClosed && received==3 && server.received_eof);
    atomic_store_explicit(&cancelled,1,memory_order_release);
    assert(venus_tcp_socket_receive(&server,bytes,8,&received,venus_tcp_now_ms()+1000,&cancelled)==RingCancelled && !received && !server.received_eof);
    assert(venus_tcp_socket_send(&server,bytes,8,venus_tcp_now_ms()+1000,&cancelled)==RingCancelled);
    venus_tcp_socket_close(&server); venus_tcp_socket_close(&client);
    venus_tcp_socket_t listener={0}; uint32_t port;
    assert(venus_tcp_socket_listen(&listener,0,&port)==RingOk);
    assert(venus_tcp_socket_accept(&listener,&server,venus_tcp_now_ms()+20,NULL)==RingTimeout);
    assert(venus_tcp_socket_accept(&listener,&server,venus_tcp_now_ms()+1000,&cancelled)==RingCancelled);
    assert(venus_tcp_socket_connect(&client,"127.0.0.1",port,venus_tcp_now_ms()+1000,&cancelled)==RingCancelled);
    assert(venus_tcp_socket_connect(&client,"127.0.0.1",port,0,NULL)==RingTimeout);
    venus_tcp_socket_close(&listener);
    /* Closed ephemeral listener reliably refuses a new connection. */
    uint64_t started=venus_tcp_now_ms();
    venus_ring_status_t refused=venus_tcp_socket_connect(&client,"127.0.0.1",port,started+5000,NULL);
    if (refused!=RingClosed) { printf("Refused connection status:%d elapsed:%llu ms\n", (int)refused, (unsigned long long)(venus_tcp_now_ms()-started)); fflush(stdout); }
    assert(refused==RingClosed);
    assert(!client.initialized);
}
static void invalid_tests(void)
{
    venus_tcp_socket_t empty={0},client={0},server={0}; uint32_t port=99; uint8_t bytes[32]; size_t received=99;
    assert(venus_tcp_socket_listen(NULL,0,&port)==RingInvalid && !port);
    assert(venus_tcp_socket_listen(&empty,65536,&port)==RingInvalid && !port);
    assert(venus_tcp_socket_listen(&empty,0,NULL)==RingInvalid);
    assert(venus_tcp_socket_connect(NULL,"127.0.0.1",1,0,NULL)==RingInvalid);
    assert(venus_tcp_socket_connect(&empty,NULL,1,0,NULL)==RingInvalid);
    assert(venus_tcp_socket_connect(&empty,"example.com",1,0,NULL)==RingInvalid);
    assert(venus_tcp_socket_connect(&empty,"127.0.0.1",0,0,NULL)==RingInvalid);
    assert(venus_tcp_socket_connect(&empty,"127.0.0.1",65536,0,NULL)==RingInvalid);
    assert(venus_tcp_socket_accept(NULL,&empty,0,NULL)==RingInvalid);
    assert(venus_tcp_socket_accept(&empty,&client,0,NULL)==RingInvalid);
    assert(venus_tcp_socket_receive(NULL,bytes,1,&received,0,NULL)==RingInvalid && !received);
    assert(venus_tcp_socket_send(&empty,bytes,1,0,NULL)==RingInvalid);
    assert(venus_tcp_socket_send(NULL,bytes,1,0,NULL)==RingInvalid);
    assert(venus_tcp_socket_receive(&empty,bytes,1,&received,0,NULL)==RingInvalid && !received);
    assert(venus_tcp_socket_shutdown_write(NULL)==RingInvalid);
    assert(venus_tcp_socket_shutdown_write(&empty)==RingInvalid);
    pair(&client,&server);
    assert(venus_tcp_socket_connect(&client,"127.0.0.1",1,0,NULL)==RingInvalid);
    assert(venus_tcp_socket_listen(&client,0,&port)==RingInvalid);
    assert(venus_tcp_socket_accept(&client,&client,0,NULL)==RingInvalid);
    assert(venus_tcp_socket_accept(&client,NULL,0,NULL)==RingInvalid);
    assert(venus_tcp_socket_accept(&client,&server,0,NULL)==RingInvalid);
    assert(venus_tcp_socket_send(&client,NULL,1,0,NULL)==RingInvalid);
    assert(venus_tcp_socket_send(&client,bytes,VenusTcpMaxCommandBytes+1,0,NULL)==RingInvalid);
    assert(venus_tcp_socket_receive(&client,NULL,1,&received,0,NULL)==RingInvalid);
    assert(venus_tcp_socket_receive(&client,bytes,1,NULL,0,NULL)==RingInvalid);
    assert(venus_tcp_socket_receive(&client,bytes,VenusTcpMaxCommandBytes+1,&received,0,NULL)==RingInvalid);
    venus_tcp_socket_close(&client); venus_tcp_socket_close(&server); venus_tcp_socket_close(NULL);
    assert(venus_tcp_random(NULL,1)==RingInvalid);
    assert(venus_tcp_random(bytes,0)==RingInvalid);
    assert(venus_tcp_random(bytes,33)==RingInvalid);
    assert(venus_tcp_random(bytes,32)==RingOk);
    venus_tcp_scrub(bytes,sizeof bytes);
    for (size_t index=0;index<sizeof bytes;++index) assert(!bytes[index]);
    venus_tcp_scrub(NULL,0);
}
#ifndef _WIN32
static void *cancel_later(void *argument)
{
    struct timespec delay={.tv_nsec=25000000}; nanosleep(&delay,NULL);
    atomic_store_explicit((_Atomic uint32_t *)argument,1,memory_order_release);
    return NULL;
}
static void asynchronous_cancel(void)
{
    venus_tcp_socket_t client={0},server={0}; pair(&client,&server);
    _Atomic uint32_t cancelled=0; uint8_t bytes[8]; size_t received=0;
    pthread_t thread; assert(!pthread_create(&thread,NULL,cancel_later,&cancelled));
    uint64_t started=venus_tcp_now_ms();
    assert(venus_tcp_socket_receive(&server,bytes,sizeof bytes,&received,started+1000,&cancelled)==RingCancelled && !received);
    assert(venus_tcp_now_ms()-started<500);
    assert(!pthread_join(thread,NULL));
    venus_tcp_socket_close(&client); venus_tcp_socket_close(&server);
}
#endif
#ifdef TcpFaultTests
static void fault_tests(void)
{
    venus_tcp_socket_t listener={0},client={0},server={0}; uint32_t port=0;
    for (fault_t mode=FaultSocket;mode<=FaultName;++mode) {
        unsigned baseline=resource_count(); fault=mode;
        assert(venus_tcp_socket_listen(&listener,0,&port)==RingClosed && !listener.initialized && !port);
        assert(fault==FaultNone && resource_count()==baseline);
    }
    assert(venus_tcp_socket_listen(&listener,0,&port)==RingOk);
    for (fault_t mode=FaultConnect;mode<=FaultSocketError;++mode) {
        unsigned baseline=resource_count(); fault=mode;
        assert(venus_tcp_socket_connect(&client,"127.0.0.1",port,venus_tcp_now_ms()+1000,NULL)==RingClosed && !client.initialized);
        assert(fault==FaultNone && resource_count()==baseline);
    }
    /* Drain connection queued by SO_ERROR failure before accept-failure checks. */
    assert(venus_tcp_socket_accept(&listener,&server,venus_tcp_now_ms()+1000,NULL)==RingOk);
    venus_tcp_socket_close(&server);
    assert(venus_tcp_socket_connect(&client,"127.0.0.1",port,venus_tcp_now_ms()+1000,NULL)==RingOk);
    unsigned accept_baseline=resource_count();fault=FaultTcpOption;
    assert(venus_tcp_socket_accept(&listener,&server,venus_tcp_now_ms()+1000,NULL)==RingClosed && !server.initialized);
    assert(fault==FaultNone && resource_count()==accept_baseline);venus_tcp_socket_close(&client);
    assert(venus_tcp_socket_connect(&client,"127.0.0.1",port,venus_tcp_now_ms()+1000,NULL)==RingOk);
    fault=FaultFcntlGet;
    assert(venus_tcp_socket_accept(&listener,&server,venus_tcp_now_ms()+1000,NULL)==RingClosed && !server.initialized);
    venus_tcp_socket_close(&client);
    fault=FaultAccept;
    assert(venus_tcp_socket_accept(&listener,&server,venus_tcp_now_ms()+1000,NULL)==RingClosed);
    fault=FaultPoll;
    assert(venus_tcp_socket_accept(&listener,&server,venus_tcp_now_ms()+1000,NULL)==RingClosed);
    fault=FaultPollInterrupted;
    assert(venus_tcp_socket_accept(&listener,&server,venus_tcp_now_ms()+20,NULL)==RingTimeout && fault==FaultNone);
    fault=FaultSocket;
    assert(venus_tcp_socket_connect(&client,"127.0.0.1",port,venus_tcp_now_ms()+1000,NULL)==RingClosed && !client.initialized);
    venus_tcp_socket_close(&listener);
    pair(&client,&server);
    uint8_t bytes[8]={0}; size_t received;
    fault=FaultReceiveInterrupted;
    assert(venus_tcp_socket_receive(&server,bytes,sizeof bytes,&received,venus_tcp_now_ms()+20,NULL)==RingTimeout && fault==FaultNone);
    fault=FaultSendAgain;
    assert(venus_tcp_socket_send(&client,bytes,sizeof bytes,venus_tcp_now_ms()+1000,NULL)==RingOk && fault==FaultNone);
    assert(venus_tcp_socket_receive(&server,bytes,sizeof bytes,&received,venus_tcp_now_ms()+1000,NULL)==RingOk);
    fault=FaultShutdown;
    assert(venus_tcp_socket_shutdown_write(&client)==RingClosed && fault==FaultNone);
    fault=FaultSend;
    assert(venus_tcp_socket_send(&client,bytes,sizeof bytes,venus_tcp_now_ms()+1000,NULL)==RingClosed);
    server.received_eof=1; /* Verify native failure clears stale EOF evidence. */
    fault=FaultReceive;
    assert(venus_tcp_socket_receive(&server,bytes,sizeof bytes,&received,venus_tcp_now_ms()+1000,NULL)==RingClosed && !received && !server.received_eof);
    uint64_t deadline=venus_tcp_now_ms()+1000;
    server.received_eof=1;
    fault=FaultClock;
    assert(venus_tcp_socket_receive(&server,bytes,sizeof bytes,&received,deadline,NULL)==RingClosed && !received && !server.received_eof);
    fault=FaultRandom;
    assert(venus_tcp_random(bytes,sizeof bytes)==RingClosed);
    assert(fault==FaultNone);
    venus_tcp_socket_close(&client); venus_tcp_socket_close(&server);
}
#endif
/** @brief Run real socket ownership regression, return zero after all resources retire. */
int main(void)
{
    /* Warm up platform lazy networking/runtime initialization before baseline. */
    puts("Starting real TCP owner regression"); fflush(stdout);
    normal_cycle(); terminal_tests(); invalid_tests();
#ifdef _WIN32
    assert(socket(AF_INET,SOCK_STREAM,IPPROTO_TCP)==INVALID_SOCKET);
    assert(WSAGetLastError()==WSANOTINITIALISED);
    Sleep(1000); /* Documented one-time native OS Event retirement before baseline. */
#endif
    unsigned baseline=resource_count();
    for (unsigned iteration=0;iteration<32;++iteration) {
        normal_cycle(); terminal_tests(); invalid_tests();
#ifdef _WIN32
        assert(socket(AF_INET,SOCK_STREAM,IPPROTO_TCP)==INVALID_SOCKET);
        assert(WSAGetLastError()==WSANOTINITIALISED);
        uint64_t retired_deadline=venus_tcp_now_ms()+1000;
        unsigned current=resource_count();
        while (current!=baseline && venus_tcp_now_ms()<retired_deadline) {
            Sleep(1);
            current=resource_count();
        }
#else
        unsigned current=resource_count();
#endif
        if (current!=baseline) { printf("Owner count iteration:%u baseline:%u current:%u\n",iteration,baseline,current); fflush(stdout); }
        assert(current==baseline);
    }
#ifndef _WIN32
    asynchronous_cancel();
#endif
#ifdef TcpFaultTests
    fault_tests();
#endif
    assert(resource_count()==baseline);
    puts("TCP real loopback deadlines, cancellation and balanced owners: PASS (32 cycles)");
    return 0;
}
