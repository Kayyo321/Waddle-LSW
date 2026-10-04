#include "common.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#define MIB (1024u*1024u)
static char root[4096], directory[128], socket_path[128], self_path[4096];
static unsigned tests;
static void fail(const char *s) { perror(s); exit(1); }
static void full_write(int fd, const void *buffer, size_t length) {
    const uint8_t *p=buffer;
    while(length) { ssize_t n=write(fd,p,length); if(n<0 && errno==EINTR) continue; if(n<=0) fail("write"); p+=n; length-=(size_t)n; }
}
static int wait_status(pid_t pid) {
    int status; while(waitpid(pid,&status,0)<0) if(errno!=EINTR) fail("waitpid");
    assert(WIFEXITED(status)); return WEXITSTATUS(status);
}
static void fake_peer(int mode) {
    umask(077);
    int listener=socket(AF_UNIX,SOCK_STREAM,0); assert(listener>=0);
    struct sockaddr_un a={.sun_family=AF_UNIX}; strcpy(a.sun_path,socket_path);
    assert(!bind(listener,(struct sockaddr *)&a,sizeof(a))); assert(!listen(listener,1));
    int fd=accept(listener,NULL,NULL); assert(fd>=0); decoder d={0}; assert(wire_read(&d,fd)==1); wire_destroy(&d);
    if(mode!=1) {
        queue q; assert(!queue_init(&q)); uint32_t seq=1;
        uint8_t response[12]={0}; waddle_put32(response+4,123);
        if(mode==2) { assert(!wire_send(&q,&seq,WADDLE_MSG_SPAWN_RESP,response,12)); q.data[28]^=1; }
        if(mode==3) { assert(!wire_send(&q,&seq,WADDLE_MSG_SPAWN_RESP,response,12)); uint8_t exit_body[16]={0}; assert(!wire_send(&q,&seq,WADDLE_MSG_PROCESS_EXIT,exit_body,16)); }
        if(mode==4) { assert(!wire_send(&q,&seq,WADDLE_MSG_SPAWN_RESP,response,12)); waddle_put32(q.data+16,WADDLE_MAX_PAYLOAD_SIZE+1); q.len=32; }
        if(mode==5) { assert(!wire_send(&q,&seq,WADDLE_MSG_SPAWN_RESP,response,12)); q.len=17; }
        full_write(fd,q.data,q.len); queue_free(&q);
    }
    close(fd); close(listener); unlink(socket_path); _exit(0);
}
static pid_t peer(int fake) {
    pid_t pid=fork(); assert(pid>=0);
    if(!pid) { if(fake) fake_peer(fake); execl("./build/waddle-mock-guest","waddle-mock-guest","--socket-path",socket_path,(char *)NULL); _exit(99); }
    for(unsigned i=0;i<500;i++) {
        struct stat st; if(!lstat(socket_path,&st)) { assert(S_ISSOCK(st.st_mode)); assert(!(st.st_mode&077)); return pid; }
        usleep(10000);
    }
    kill(pid,SIGKILL); fail("peer startup deadline"); return -1;
}
typedef struct {
    const char *name;
    char **guest;
    int expected, fake, signal_number, large, timeout;
    const char *out, *err, *cwd, *environment;
} test_case;
static void run_case(test_case *t) {
    pid_t mock=peer(t->fake); int input[2], output[2], errors[2];
    assert(!pipe2(input,O_CLOEXEC)); assert(!pipe2(output,O_CLOEXEC)); assert(!pipe2(errors,O_CLOEXEC));
    pid_t host=fork(); assert(host>=0);
    if(!host) {
        assert(dup2(input[0],0)>=0 && dup2(output[1],1)>=0 && dup2(errors[1],2)>=0);
        close(input[0]); close(input[1]); close(output[0]); close(output[1]); close(errors[0]); close(errors[1]);
        char *args[64]={"./build/waddle","exec","--pipe","--socket-path",socket_path}; int count=5;
        if(t->timeout) { args[count++]="--timeout"; args[count++]="1"; }
        if(t->cwd) { args[count++]="--cwd"; args[count++]=(char *)t->cwd; }
        if(t->environment) { args[count++]="--env"; args[count++]=(char *)t->environment; }
        args[count++]="--"; for(unsigned i=0;t->guest[i];i++) args[count++]=t->guest[i]; args[count]=NULL;
        execv(args[0],args); _exit(99);
    }
    close(input[0]); close(output[1]); close(errors[1]);
    assert(!nonblock(input[1]) && !nonblock(output[0]) && !nonblock(errors[0]));
    size_t written=0, received[2]={0}, target=t->large ? 5*MIB : 0; int signal_sent=0;
    if(!target) { close(input[1]); input[1]=-1; }
    uint64_t deadline=monotonic_ms()+15000, slow_until=monotonic_ms()+100;
    for(;;) {
        if(monotonic_ms()>deadline) { kill(host,SIGKILL); kill(mock,SIGKILL); fprintf(stderr,"deadline: %s\n",t->name); exit(1); }
        int slow=t->large && monotonic_ms()<slow_until;
        struct pollfd p[3]={{input[1],POLLOUT,0},{slow ? -1 : output[0],POLLIN,0},{slow ? -1 : errors[0],POLLIN,0}};
        int n=poll(p,3,10); if(n<0 && errno==EINTR) continue; assert(n>=0);
        if(p[0].revents && input[1]>=0) {
            uint8_t b[16384]; size_t len=target-written; if(len>sizeof(b)) len=sizeof(b);
            for(size_t i=0;i<len;i++) b[i]=(uint8_t)((written+i)%251);
            ssize_t sent=write(input[1],b,len);
            if(sent>0) written+=(size_t)sent; else assert(errno==EAGAIN || errno==EINTR);
            if(written==target) { close(input[1]); input[1]=-1; }
        }
        for(int i=0;i<2;i++) if(p[i+1].revents) {
            int *fd=i ? &errors[0] : &output[0]; uint8_t b[8192]; ssize_t got=read(*fd,b,sizeof(b));
            if(got>0) {
                for(ssize_t j=0;j<got;j++) {
                    size_t offset=received[i]+(size_t)j;
                    if(t->large) { uint8_t want=i ? 'E' : offset<3*MIB ? 'O' : (uint8_t)((offset-3*MIB)%251); assert(b[j]==want); }
                    else if((i ? t->err : t->out)) { const char *want=i ? t->err : t->out; assert(offset<strlen(want) && b[j]==(uint8_t)want[offset]); }
                }
                received[i]+=(size_t)got;
                if(!i && t->signal_number && !signal_sent) { assert(!kill(host,t->signal_number)); signal_sent=1; }
            } else if(got==0) { close(*fd); *fd=-1; }
            else assert(errno==EAGAIN || errno==EINTR);
        }
        if(output[0]<0 && errors[0]<0) break;
    }
    if(input[1]>=0) close(input[1]);
    int host_status=wait_status(host), peer_status=wait_status(mock);
    if(host_status!=t->expected) { fprintf(stderr,"%s: host returned %d, expected %d\n",t->name,host_status,t->expected); exit(1); }
    if(!t->fake && !t->timeout) assert(peer_status==0);
    if(t->large) { assert(written==target && received[0]==8*MIB && received[1]==3*MIB); }
    else {
        if(t->out) assert(received[0]==strlen(t->out));
        if(t->err) assert(received[1]==strlen(t->err));
    }
    if(t->signal_number) assert(signal_sent);
    assert(access(socket_path,F_OK)<0); tests++; printf("integration: %s passed\n",t->name);
}
static void tty_case(int mode) {
    int disconnect=mode==1;
    pid_t mock=peer(0); int master, slave, errors[2]; struct winsize size={.ws_row=mode==2 ? 0 : 24,.ws_col=mode==2 ? 0 : 80};
    assert(!openpty(&master,&slave,NULL,NULL,&size)); assert(!pipe2(errors,O_CLOEXEC));
    assert(!fcntl(master,F_SETFD,FD_CLOEXEC)); assert(!fcntl(slave,F_SETFD,FD_CLOEXEC));
    struct termios before; assert(!tcgetattr(slave,&before));
    pid_t host=fork(); assert(host>=0);
    if(!host) {
        assert(dup2(slave,0)>=0 && dup2(slave,1)>=0 && dup2(errors[1],2)>=0);
        close(slave); close(master); close(errors[0]); close(errors[1]);
        execl("./build/waddle","waddle","exec","-i","--socket-path",socket_path,"--",self_path,"--child",disconnect ? "sleep" : "tty",(char *)NULL); _exit(99);
    }
    close(errors[1]); assert(!nonblock(master) && !nonblock(errors[0]));
    int changed=0, done=0, status=0, ready=0, resized=0; char captured[4096]={0}; size_t used=0;
    uint64_t end=monotonic_ms()+10000;
    while(!done && monotonic_ms()<end) {
        struct pollfd p[2]={{master,POLLIN,0},{errors[0],POLLIN,0}}; assert(poll(p,2,20)>=0);
        if(p[0].revents) {
            ssize_t got=read(master,captured+used,sizeof(captured)-used-1);
            if(got>0) { used+=(size_t)got; captured[used]=0; }
        }
        if(p[1].revents) { char b[256]; (void)read(errors[0],b,sizeof(b)); }
        struct termios during; assert(!tcgetattr(slave,&during)); if(!(during.c_lflag&ICANON)) changed=1;
        if(!ready && strstr(captured,"READY")) {
            ready=1;
            if(disconnect) assert(!kill(mock,SIGTERM));
            else { size.ws_row=39; size.ws_col=101; assert(!ioctl(slave,TIOCSWINSZ,&size)); assert(!kill(host,SIGWINCH)); full_write(master,"size\n",5); }
        }
        if(!disconnect && ready && !resized && strstr(captured,"39 101")) { resized=1; full_write(master,"\003",1); }
        pid_t found=waitpid(host,&status,WNOHANG); assert(found>=0); if(found==host) done=1;
    }
    if(!done) { fprintf(stderr,"TTY deadline, captured: %s\n",captured); kill(host,SIGKILL); kill(mock,SIGKILL); exit(1); }
    assert(changed && ready && WIFEXITED(status));
    assert(WEXITSTATUS(status)==(disconnect ? 125 : 130));
    if(!disconnect) assert(resized);
    struct termios after; assert(!tcgetattr(slave,&after));
    assert(before.c_iflag==after.c_iflag && before.c_oflag==after.c_oflag && before.c_cflag==after.c_cflag && before.c_lflag==after.c_lflag);
    assert(!memcmp(before.c_cc,after.c_cc,sizeof(before.c_cc)));
    close(master); close(slave); close(errors[0]); int peer_status=wait_status(mock); if(!disconnect) assert(peer_status==0);
    tests++; printf("integration: PTY %s and restoration passed\n",disconnect ? "disconnect" : mode==2 ? "unknown dimensions/resize/Ctrl-C" : "resize/Ctrl-C");
}
static int child(int argc, char **argv) {
    assert(argc>=3); const char *mode=argv[2];
    if(!strcmp(mode,"streams")) { full_write(1,"stdout\n",7); full_write(2,"stderr\n",7); return 42; }
    if(!strcmp(mode,"args")) { assert(argc==8 && !strcmp(argv[3],"") && !strcmp(argv[4],"a b") && !strcmp(argv[5],"foo\"bar") && !strcmp(argv[6],"C:\\Program Files\\") && !strcmp(argv[7],"日本語")); full_write(1,"args ok\n",8); return 0; }
    if(!strcmp(mode,"context")) { char *cwd=getcwd(NULL,0); assert(cwd && !strcmp(cwd,argv[3])); assert(getenv("WADDLE_TEST") && !strcmp(getenv("WADDLE_TEST"),"some value")); free(cwd); full_write(1,"context ok\n",11); return 0; }
    if(!strcmp(mode,"sleep")) { full_write(1,"READY\n",6); for(;;) pause(); }
    if(!strcmp(mode,"large")) {
        uint8_t b[16384]; memset(b,'O',sizeof(b)); for(unsigned i=0;i<3*MIB/sizeof(b);i++) full_write(1,b,sizeof(b));
        memset(b,'E',sizeof(b)); for(unsigned i=0;i<3*MIB/sizeof(b);i++) full_write(2,b,sizeof(b));
        for(;;) { ssize_t n=read(0,b,sizeof(b)); if(n<0 && errno==EINTR) continue; assert(n>=0); if(!n) break; full_write(1,b,(size_t)n); } return 0;
    }
    if(!strcmp(mode,"tty")) {
        assert(isatty(0) && isatty(1));
        struct winsize initial; assert(!ioctl(0,TIOCGWINSZ,&initial)); assert(initial.ws_row==24 && initial.ws_col==80);
        full_write(1,"READY\n",6);
        char b[64]; ssize_t n=read(0,b,sizeof(b)); assert(n>0);
        /* Resize and input frames are ordered, so this read sees the new size. */
        struct winsize ws; assert(!ioctl(0,TIOCGWINSZ,&ws));
        char text[64]; int len=snprintf(text,sizeof(text),"%u %u\n",ws.ws_row,ws.ws_col); full_write(1,text,(size_t)len);
        for(;;) pause();
    }
    return 99;
}
int main(int argc, char **argv) {
    if(argc>1 && !strcmp(argv[1],"--child")) return child(argc,argv);
    alarm(90); signal(SIGPIPE,SIG_IGN); setvbuf(stdout,NULL,_IOLBF,0);
    assert(getcwd(root,sizeof(root))); assert(strlen(root)+24<sizeof(self_path));
    strcpy(self_path,root); strcat(self_path,"/build/integration");
    strcpy(directory,"/tmp/waddle-test-XXXXXX"); assert(mkdtemp(directory)); assert(strlen(directory)+12<sizeof(socket_path)); strcpy(socket_path,directory); strcat(socket_path,"/guest.sock");
    char *streams[]={self_path,"--child","streams",NULL};
    test_case t={.name="separate outputs and exit 42",.guest=streams,.expected=42,.out="stdout\n",.err="stderr\n"}; run_case(&t);
    char *args[]={self_path,"--child","args","","a b","foo\"bar","C:\\Program Files\\","日本語",NULL};
    t=(test_case){.name="empty/space/quote/backslash/UTF-8 arguments",.guest=args,.out="args ok\n",.err=""}; run_case(&t);
    char *context[]={self_path,"--child","context",directory,NULL};
    t=(test_case){.name="working directory and explicit environment",.guest=context,.cwd=directory,.environment="WADDLE_TEST=some value",.out="context ok\n",.err=""}; run_case(&t);
    char *missing[]={"/no/such/waddle-program",NULL}; t=(test_case){.name="missing command returns 127",.guest=missing,.expected=127,.out=""}; run_case(&t);
    char *large[]={self_path,"--child","large",NULL}; t=(test_case){.name="16 MiB binary duplex with slow output readers",.guest=large,.large=1}; run_case(&t);
    char *sleep_args[]={self_path,"--child","sleep",NULL};
    t=(test_case){.name="SIGINT forwarding",.guest=sleep_args,.signal_number=SIGINT,.expected=130,.out="READY\n",.err=""}; run_case(&t);
    t=(test_case){.name="SIGTERM forwarding",.guest=sleep_args,.signal_number=SIGTERM,.expected=143,.out="READY\n",.err=""}; run_case(&t);
    t=(test_case){.name="total session deadline returns 124",.guest=sleep_args,.timeout=1,.expected=124,.out="READY\n"}; run_case(&t);
    for(int mode=1;mode<=5;mode++) { t=(test_case){.name="disconnect/corrupt CRC/early exit/oversize/truncated peer",.guest=streams,.fake=mode,.expected=125,.out=""}; run_case(&t); }
    tty_case(0); tty_case(1); tty_case(2);
    assert(!rmdir(directory)); printf("integration: all %u scenarios passed\n",tests); return 0;
}
