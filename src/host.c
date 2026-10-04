#include "session.h"
#include "terminal.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/vm_sockets.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

static int number(const char *s, uint32_t *out) {
    if(!*s) return -1;
    uint64_t n=0;
    for(;*s;s++) { if(*s<'0' || *s>'9') return -1; n=n*10+(unsigned)(*s-'0'); if(n>UINT32_MAX) return -1; }
    *out=(uint32_t)n; return 0;
}
static void usage(FILE *f) {
    fprintf(f,"Usage: waddle exec|run [options] -- program [arguments...]\n"
              "  --socket-path PATH       UNIX mock transport (default: VSOCK)\n"
              "  --vsock-cid N --vsock-port N   default 3:5242\n"
              "  --pipe|-P  --interactive|-i|--tty|-t\n"
              "  --cwd PATH  --env|-e KEY=VALUE  --translate-path\n"
              "  --timeout SECONDS        total deadline; 0 disables\n");
}
static int connect_peer(const char *path, uint32_t cid, uint32_t port, uint64_t deadline) {
    int fd=socket(path ? AF_UNIX : AF_VSOCK,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
    if(fd<0) return -1;
    int result;
    if(path) {
        struct sockaddr_un a={0}; a.sun_family=AF_UNIX;
        if(strlen(path)>=sizeof(a.sun_path)) { errno=ENAMETOOLONG; close(fd); return -1; }
        strcpy(a.sun_path,path); result=connect(fd,(struct sockaddr *)&a,sizeof(a));
    } else {
        struct sockaddr_vm a={0}; a.svm_family=AF_VSOCK; a.svm_cid=cid; a.svm_port=port;
        result=connect(fd,(struct sockaddr *)&a,sizeof(a));
    }
    if(result==0) return fd;
    if(errno!=EINPROGRESS) { int e=errno; close(fd); errno=e; return -1; }
    for(;;) {
        int timeout=-1;
        if(deadline) { uint64_t now=monotonic_ms(); if(now>=deadline) { errno=ETIMEDOUT; break; } uint64_t left=deadline-now; timeout=left>INT_MAX ? INT_MAX : (int)left; }
        struct pollfd p={fd,POLLOUT,0}; result=poll(&p,1,timeout);
        if(result<0 && errno==EINTR) continue;
        if(result<=0) { if(!result) errno=ETIMEDOUT; break; }
        int e=0; socklen_t len=sizeof(e); if(getsockopt(fd,SOL_SOCKET,SO_ERROR,&e,&len)) break;
        if(e) { errno=e; break; } return fd;
    }
    int e=errno; close(fd); errno=e; return -1;
}
int main(int argc, char **argv) {
    const char *socket_path=NULL, *cwd_arg=NULL; uint32_t cid=3, port=WADDLE_DEFAULT_VSOCK_PORT, timeout=0;
    int interactive=-1, translate=0, start=0, result=2, fd=-1;
    char *cwd=NULL, *command=NULL, **args=NULL; queue env={0}, tx={0}; uint8_t *spawn=NULL;
    if(argc==2 && (!strcmp(argv[1],"--help") || !strcmp(argv[1],"-h"))) { usage(stdout); return 0; }
    if(argc<3 || (strcmp(argv[1],"exec") && strcmp(argv[1],"run"))) { usage(stderr); return 2; }
    if(queue_init(&env) || queue_init(&tx)) { result=125; goto done; }
    for(int i=2;i<argc;i++) {
        const char *opt=argv[i];
        if(!strcmp(opt,"--")) { start=i+1; break; }
        if(!strcmp(opt,"--pipe") || !strcmp(opt,"-P")) { interactive=0; continue; }
        if(!strcmp(opt,"--interactive") || !strcmp(opt,"-i") || !strcmp(opt,"--tty") || !strcmp(opt,"-t")) { interactive=1; continue; }
        if(!strcmp(opt,"--translate-path")) { translate=1; continue; }
        if(!strcmp(opt,"--help") || !strcmp(opt,"-h")) { usage(stdout); result=0; goto done; }
        if(i+1>=argc) goto usage_error;
        const char *val=argv[++i];
        if(!strcmp(opt,"--socket-path")) socket_path=val;
        else if(!strcmp(opt,"--cwd")) cwd_arg=val;
        else if(!strcmp(opt,"--vsock-cid")) { if(number(val,&cid)) goto usage_error; }
        else if(!strcmp(opt,"--vsock-port")) { if(number(val,&port) || !port) goto usage_error; }
        else if(!strcmp(opt,"--timeout")) { if(number(val,&timeout)) goto usage_error; }
        else if(!strcmp(opt,"--env") || !strcmp(opt,"-e")) {
            const char *equal=strchr(val,'=');
            if(!equal || equal==val || strlen(val)+1>WADDLE_MAX_PAYLOAD_SIZE-env.len || queue_append(&env,val,strlen(val)+1)) goto usage_error;
        } else goto usage_error;
    }
    if(!start || start>=argc || !*argv[start]) goto usage_error;
    if(interactive<0) interactive=isatty(0) && isatty(1);
    if(interactive && (!isatty(0) || !isatty(1))) { fprintf(stderr,"waddle: interactive mode requires terminal stdin and stdout\n"); goto done; }
    cwd=cwd_arg ? strdup(cwd_arg) : getcwd(NULL,0); if(!cwd) { result=125; goto done; }
    args=calloc((size_t)(argc-start)+1,sizeof(*args)); if(!args) { result=125; goto done; }
    for(int i=start;i<argc;i++) { args[i-start]=translate ? waddle_translate_path(argv[i]) : strdup(argv[i]); if(!args[i-start]) { result=125; goto done; } }
    if(translate) { char *mapped=waddle_translate_path(cwd); if(!mapped) { result=125; goto done; } free(cwd); cwd=mapped; }
    command=waddle_quote(args); if(!command) { result=125; goto done; }
    size_t cwd_len=strlen(cwd), cmd_len=strlen(command);
    if(cwd_len>WADDLE_MAX_PAYLOAD_SIZE || cmd_len>WADDLE_MAX_PAYLOAD_SIZE || cwd_len+cmd_len+env.len+26>WADDLE_MAX_PAYLOAD_SIZE) { errno=E2BIG; result=125; goto local_error; }
    size_t total=cwd_len+cmd_len+env.len+26; spawn=calloc(total,1); if(!spawn) { result=125; goto done; }
    waddle_put32(spawn,interactive ? WADDLE_SPAWN_FLAG_INTERACTIVE : WADDLE_SPAWN_FLAG_RAW_PIPES);
    if(translate) waddle_put32(spawn,waddle_get32(spawn)|WADDLE_SPAWN_FLAG_TRANSLATE_PATH);
    struct winsize ws={.ws_row=24,.ws_col=80}; if(interactive && ioctl(0,TIOCGWINSZ,&ws)) { result=125; goto local_error; }
    waddle_put16(spawn+4,ws.ws_row); waddle_put16(spawn+6,ws.ws_col); waddle_put16(spawn+8,ws.ws_xpixel); waddle_put16(spawn+10,ws.ws_ypixel);
    waddle_put32(spawn+12,(uint32_t)cwd_len); waddle_put32(spawn+16,(uint32_t)cmd_len); waddle_put32(spawn+20,(uint32_t)env.len);
    memcpy(spawn+24,cwd,cwd_len+1); memcpy(spawn+25+cwd_len,command,cmd_len+1); memcpy(spawn+26+cwd_len+cmd_len,env.data,env.len);
    if(waddle_terminal_init(interactive)) { result=125; goto local_error; }
    uint64_t deadline=timeout ? monotonic_ms()+(uint64_t)timeout*1000 : 0;
    fd=connect_peer(socket_path,cid,port,deadline);
    if(fd<0) { result=errno==ETIMEDOUT ? 124 : 125; goto local_error; }
    if(waddle_standard_nonblock()) { result=125; goto local_error; }
    uint32_t seq=1;
    if(wire_send(&tx,&seq,WADDLE_MSG_SPAWN_REQ,spawn,total)) { result=125; goto local_error; }
    result=waddle_session(fd,&tx,seq,deadline,interactive); goto done;
usage_error:
    usage(stderr); goto done;
local_error:
    fprintf(stderr,"waddle: %s\n",strerror(errno));
done:
    waddle_terminal_close(); if(fd>=0) close(fd);
    free(spawn); free(command); free(cwd); waddle_free_argv(args); queue_free(&env); queue_free(&tx); return result;
}
