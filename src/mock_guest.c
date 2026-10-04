#include "mock_process.h"
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
static volatile sig_atomic_t stopping;
static void stop_signal(int sig) { (void)sig; stopping=1; }
static int handshake(decoder *rx, int fd) {
    uint64_t end=monotonic_ms()+10000;
    while(!stopping && monotonic_ms()<end) {
        struct pollfd p={fd,POLLIN,0}; int n=poll(&p,1,100);
        if(n<0) { if(errno==EINTR) continue; return -1; }
        if(n) { int result=wire_read(rx,fd); if(result==1) return rx->type==WADDLE_MSG_SPAWN_REQ ? 0 : -1; if(result<0) return -1; }
    }
    errno=ETIMEDOUT; return -1;
}
static int flush_response(int fd, queue *tx) {
    uint64_t end=monotonic_ms()+10000;
    while(tx->len && !stopping && monotonic_ms()<end) {
        struct pollfd p={fd,POLLOUT,0}; int n=poll(&p,1,100);
        if(n<0) { if(errno==EINTR) continue; return -1; }
        if(n && queue_flush(tx,fd)) return -1;
    }
    return tx->len ? -1 : 0;
}
static int relay(int fd) {
    decoder rx={0}; queue tx={0}, input={0}; uint32_t seq=1;
    mock_process child={.input=-1,.output={-1,-1}};
    int result=1, input_eof=0, eof[2]={0}, exit_sent=0;
    if(queue_init(&tx) || queue_init(&input) || handshake(&rx,fd)) goto done;
    if(mock_spawn(&child,rx.body,rx.length)) {
        int error=errno; const char *message=strerror(error); size_t n=strlen(message);
        uint8_t b[256]={0}; waddle_put32(b,(uint32_t)error); waddle_put32(b+8,(uint32_t)n); memcpy(b+12,message,n);
        if(!wire_send(&tx,&seq,WADDLE_MSG_SPAWN_RESP,b,n+12)) result=flush_response(fd,&tx);
        goto done;
    }
    uint8_t response[12]={0}; waddle_put32(response+4,(uint32_t)child.pid);
    if(wire_send(&tx,&seq,WADDLE_MSG_SPAWN_RESP,response,12)) goto done;
    wire_consume(&rx);
    if(child.interactive) { if(wire_eof(&tx,&seq,2)) goto done; eof[1]=1; }
    while(!stopping) {
        if(mock_reap(&child)<0) goto done;
        if(child.reaped && eof[0] && eof[1] && !exit_sent && queue_space(&tx)>=48) {
            uint8_t b[16]={0};
            uint32_t code=WIFEXITED(child.status) ? (uint32_t)WEXITSTATUS(child.status) : (uint32_t)(128+WTERMSIG(child.status));
            waddle_put32(b,code); waddle_put32(b+4,WIFSIGNALED(child.status) ? 1 : 0); waddle_put64(b+8,monotonic_ms()-child.started);
            if(wire_send(&tx,&seq,WADDLE_MSG_PROCESS_EXIT,b,16)) goto done;
            exit_sent=1;
        }
        if(exit_sent && !tx.len) { result=0; goto done; }
        if(input_eof && !input.len && child.input>=0) { close(child.input); child.input=-1; }
        struct pollfd p[4]={
            {fd,(short)((!exit_sent && queue_space(&input)>=WADDLE_CHUNK_SIZE ? POLLIN : 0) | (tx.len ? POLLOUT : 0)),0},
            {child.input,(short)(input.len ? POLLOUT : 0),0},
            {child.output[0],(short)(queue_space(&tx)>=WADDLE_CHUNK_SIZE+40 ? POLLIN : 0),0},
            {child.output[1],(short)(queue_space(&tx)>=WADDLE_CHUNK_SIZE+40 ? POLLIN : 0),0}
        };
        for(int i=0;i<4;i++) if(!p[i].events) p[i].fd=-1;
        int n=poll(p,4,100); if(n<0) { if(errno==EINTR) continue; goto done; }
        if(p[0].revents&POLLOUT) if(queue_flush(&tx,fd)) goto done;
        if(p[1].revents) {
            if(queue_flush(&input,child.input)) {
                if(errno!=EPIPE && !(child.interactive && errno==EIO)) goto done;
                close(child.input); child.input=-1; input.len=0;
            }
        }
        for(int i=0;i<2;i++) if(p[2+i].revents && queue_space(&tx)>=WADDLE_CHUNK_SIZE+40) {
            uint8_t b[WADDLE_CHUNK_SIZE]; ssize_t got=read(child.output[i],b,sizeof(b));
            if(got>0) { if(wire_stream(&tx,&seq,(unsigned)i+1,b,(size_t)got)) goto done; }
            else if(got==0 || (got<0 && child.interactive && errno==EIO)) {
                close(child.output[i]); child.output[i]=-1; eof[i]=1;
                if(wire_eof(&tx,&seq,(unsigned)i+1)) goto done;
            } else if(errno!=EINTR && errno!=EAGAIN) goto done;
        }
        if(p[0].revents&(POLLIN|POLLHUP|POLLERR)) {
            int got=wire_read(&rx,fd); if(got==0) continue; if(got<0) goto done;
            uint8_t *b=rx.body; size_t len=rx.length;
            if(rx.type==WADDLE_MSG_STREAM_DATA) {
                if(input_eof || len<8 || b[0] || b[1] || b[2] || b[3] || waddle_get32(b+4)!=len-8 || len-8>WADDLE_CHUNK_SIZE) goto protocol;
                if(child.input>=0 && queue_append(&input,b+8,len-8)) goto done;
            } else if(rx.type==WADDLE_MSG_STREAM_EOF) {
                if(input_eof || len!=4 || waddle_get32(b)!=0) goto protocol;
                input_eof=1;
                if(child.interactive && child.input>=0 && queue_append(&input,"\004",1)) goto done;
            } else if(rx.type==WADDLE_MSG_SIGNAL_EVENT) {
                if(len!=4) goto protocol;
                uint32_t sig=waddle_get32(b);
                if(sig!=SIGINT && sig!=SIGQUIT && sig!=SIGTERM && sig!=SIGKILL) goto protocol;
                if(!child.reaped && kill(-child.pid,(int)sig) && errno!=ESRCH) goto done;
            } else if(rx.type==WADDLE_MSG_TERMINAL_RESIZE) {
                if(len!=8 || !child.interactive || !waddle_get16(b) || !waddle_get16(b+2)) goto protocol;
                struct winsize ws={.ws_row=waddle_get16(b),.ws_col=waddle_get16(b+2),.ws_xpixel=waddle_get16(b+4),.ws_ypixel=waddle_get16(b+6)};
                if(!child.reaped && child.output[0]>=0 && ioctl(child.output[0],TIOCSWINSZ,&ws)) goto done;
            } else goto protocol;
            wire_consume(&rx);
        }
    }
    goto done;
protocol:
    errno=EPROTO;
done:
    mock_stop(&child); wire_destroy(&rx); queue_free(&tx); queue_free(&input); return result;
}
int main(int argc, char **argv) {
    if(argc!=3 || strcmp(argv[1],"--socket-path")) { fprintf(stderr,"Usage: waddle-mock-guest --socket-path PATH\nLinux development peer; serves one session and exits.\n"); return 2; }
    const char *path=argv[2]; struct sockaddr_un addr={0}; addr.sun_family=AF_UNIX;
    if(strlen(path)>=sizeof(addr.sun_path)) { fprintf(stderr,"mock: socket path too long\n"); return 2; }
    strcpy(addr.sun_path,path);
    struct sigaction a={0}; sigemptyset(&a.sa_mask); a.sa_handler=stop_signal;
    if(sigaction(SIGTERM,&a,NULL) || sigaction(SIGINT,&a,NULL)) { perror("mock: sigaction"); return 1; }
    a.sa_handler=SIG_IGN; if(sigaction(SIGPIPE,&a,NULL)) return 1;
    umask(077);
    int listener=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0); if(listener<0) { perror("mock: socket"); return 1; }
    if(bind(listener,(struct sockaddr *)&addr,sizeof(addr))) { perror("mock: bind (existing paths are never removed)"); close(listener); return 1; }
    int result=1, client=-1;
    if(listen(listener,1)) { perror("mock: listen"); goto done; }
    while(!stopping) {
        struct pollfd p={listener,POLLIN,0}; int n=poll(&p,1,100);
        if(n<0) { if(errno==EINTR) continue; break; }
        if(!n) continue;
        client=accept4(listener,NULL,NULL,SOCK_CLOEXEC|SOCK_NONBLOCK);
        if(client<0) { if(errno==EAGAIN || errno==EINTR) continue; break; }
        close(listener); listener=-1; result=relay(client); break;
    }
done:
    if(client>=0) close(client);
    if(listener>=0) close(listener);
    (void)unlink(path); return result;
}
