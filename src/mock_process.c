#include "mock_process.h"
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
static void child_error(int fd, int error) {
    ssize_t n; do { n=write(fd,&error,sizeof(error)); } while(n<0 && errno==EINTR);
    _exit(126);
}
static void close_pair(int p[2]) { for(int i=0;i<2;i++) if(p[i]>=0) close(p[i]); }
int mock_spawn(mock_process *p, uint8_t *body, size_t length) {
    memset(p,0,sizeof(*p)); p->input=p->output[0]=p->output[1]=-1;
    if(length<26) { errno=EPROTO; return -1; }
    uint32_t flags=waddle_get32(body), cwd_len=waddle_get32(body+12), cmd_len=waddle_get32(body+16), env_len=waddle_get32(body+20);
    if((uint64_t)cwd_len+cmd_len+env_len+26!=length || !cmd_len ||
       (flags & ~(WADDLE_SPAWN_FLAG_INTERACTIVE|WADDLE_SPAWN_FLAG_RAW_PIPES|WADDLE_SPAWN_FLAG_TRANSLATE_PATH)) ||
       ((flags&3)!=1 && (flags&3)!=2)) { errno=EPROTO; return -1; }
    char *cwd=(char *)body+24, *cmd=cwd+cwd_len+1, *env=cmd+cmd_len+1;
    if(cwd[cwd_len] || cmd[cmd_len] || memchr(cwd,0,cwd_len) || memchr(cmd,0,cmd_len)) { errno=EPROTO; return -1; }
    for(size_t off=0;off<env_len;) {
        char *end=memchr(env+off,0,env_len-off); if(!end) { errno=EPROTO; return -1; }
        char *equal=memchr(env+off,'=',(size_t)(end-env-off));
        if(!equal || equal==env+off) { errno=EPROTO; return -1; } off=(size_t)(end-env)+1;
    }
    char **argv=waddle_unquote(cmd); if(!argv) return -1;
    if(!*argv[0]) { waddle_free_argv(argv); errno=EPROTO; return -1; }
    p->interactive=(flags&1)!=0;
    struct winsize ws={.ws_row=waddle_get16(body+4),.ws_col=waddle_get16(body+6),.ws_xpixel=waddle_get16(body+8),.ws_ypixel=waddle_get16(body+10)};
    if(p->interactive && (!ws.ws_row || !ws.ws_col)) { waddle_free_argv(argv); errno=EPROTO; return -1; }
    int error_pipe[2]={-1,-1}, in[2]={-1,-1}, out[2]={-1,-1}, err[2]={-1,-1}, master=-1;
    if(pipe2(error_pipe,O_CLOEXEC)) goto failure;
    if(!p->interactive && (pipe2(in,O_CLOEXEC) || pipe2(out,O_CLOEXEC) || pipe2(err,O_CLOEXEC))) goto failure;
    p->started=monotonic_ms();
    p->pid=p->interactive ? forkpty(&master,NULL,NULL,&ws) : fork();
    if(p->pid<0) { p->pid=0; goto failure; }
    if(p->pid==0) {
        close(error_pipe[0]);
        signal(SIGPIPE,SIG_DFL); signal(SIGINT,SIG_DFL); signal(SIGQUIT,SIG_DFL); signal(SIGTERM,SIG_DFL);
        if(!p->interactive) {
            if(setsid()<0 || dup2(in[0],0)<0 || dup2(out[1],1)<0 || dup2(err[1],2)<0) child_error(error_pipe[1],errno);
            close_pair(in); close_pair(out); close_pair(err);
        }
        if(chdir(cwd)) child_error(error_pipe[1],errno);
        for(size_t off=0;off<env_len;) {
            char *item=env+off; size_t n=strlen(item); char *equal=strchr(item,'='); *equal=0;
            if(setenv(item,equal+1,1)) child_error(error_pipe[1],errno);
            off+=n+1;
        }
        execvp(argv[0],argv); child_error(error_pipe[1],errno);
    }
    close(error_pipe[1]); error_pipe[1]=-1;
    if(p->interactive) {
        if(fcntl(master,F_SETFD,FD_CLOEXEC)) goto failure;
        p->input=dup(master); p->output[0]=master; master=-1;
        if(p->input<0 || fcntl(p->input,F_SETFD,FD_CLOEXEC)) goto failure;
    } else {
        close(in[0]); in[0]=-1; close(out[1]); out[1]=-1; close(err[1]); err[1]=-1;
        p->input=in[1]; in[1]=-1; p->output[0]=out[0]; out[0]=-1; p->output[1]=err[0]; err[0]=-1;
    }
    int error=0; size_t have=0;
    for(;;) {
        ssize_t n=read(error_pipe[0],(uint8_t *)&error+have,sizeof(error)-have);
        if(n<0 && errno==EINTR) continue;
        if(n<0) goto failure;
        if(n==0) break;
        have+=(size_t)n; if(have==sizeof(error)) break;
    }
    close(error_pipe[0]); error_pipe[0]=-1;
    if(have) { errno=have==sizeof(error) ? error : EIO; goto failure; }
    if(nonblock(p->input) || nonblock(p->output[0]) || (p->output[1]>=0 && nonblock(p->output[1]))) goto failure;
    waddle_free_argv(argv); return 0;
failure: {
    int saved=errno; close_pair(error_pipe); close_pair(in); close_pair(out); close_pair(err);
    if(master>=0) close(master);
    mock_stop(p); waddle_free_argv(argv); errno=saved; return -1;
}}
int mock_reap(mock_process *p) {
    if(p->reaped) return 1;
    pid_t result=waitpid(p->pid,&p->status,WNOHANG);
    if(result<0) return -1;
    if(result==p->pid) { p->reaped=1; (void)kill(-p->pid,SIGKILL); return 1; }
    return 0;
}
void mock_stop(mock_process *p) {
    if(p->pid>0) {
        (void)kill(-p->pid,SIGKILL);
        if(!p->reaped) { (void)kill(p->pid,SIGKILL); while(waitpid(p->pid,&p->status,0)<0 && errno==EINTR) {} p->reaped=1; }
    }
    if(p->input>=0) { close(p->input); p->input=-1; }
    for(int i=0;i<2;i++) if(p->output[i]>=0) { close(p->output[i]); p->output[i]=-1; }
}
