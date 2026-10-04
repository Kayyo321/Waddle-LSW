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

static struct termios original_term;
static int term_saved, original_flags[3]={-1,-1,-1}, signal_pipe[2]={-1,-1};
static volatile sig_atomic_t pending[NSIG];
void waddle_restore(void) {
    if(term_saved) (void)tcsetattr(STDIN_FILENO,TCSANOW,&original_term);
    for(int i=0;i<3;i++) if(original_flags[i]>=0) (void)fcntl(i,F_SETFL,original_flags[i]);
}
static void on_signal(int sig) {
    int saved=errno; pending[sig]=1;
    unsigned char wake=1; (void)write(signal_pipe[1],&wake,1); errno=saved;
}
static int setup_signals(void) {
    if(pipe2(signal_pipe,O_NONBLOCK|O_CLOEXEC)) return -1;
    struct sigaction a={0}; a.sa_handler=on_signal; sigemptyset(&a.sa_mask);
    const int signals[]={SIGINT,SIGQUIT,SIGTERM,SIGWINCH};
    for(size_t i=0;i<sizeof(signals)/sizeof(signals[0]);i++) if(sigaction(signals[i],&a,NULL)) return -1;
    a.sa_handler=SIG_IGN; return sigaction(SIGPIPE,&a,NULL);
}
int waddle_send_pending(queue *tx, uint32_t *seq, int interactive) {
    unsigned char junk[128]; while(read(signal_pipe[0],junk,sizeof(junk))>0) {}
    const int signals[]={SIGINT,SIGQUIT,SIGTERM,SIGWINCH};
    for(size_t i=0;i<sizeof(signals)/sizeof(signals[0]);i++) {
        int sig=signals[i];
        if(!pending[sig] || queue_space(tx)<40) continue;
        /* Block briefly so clearing the pending flag cannot race a handler. */
        sigset_t set, old; sigemptyset(&set); sigaddset(&set,sig);
        if(sigprocmask(SIG_BLOCK,&set,&old)) return -1;
        int present=pending[sig]; pending[sig]=0;
        if(sigprocmask(SIG_SETMASK,&old,NULL)) return -1;
        if(!present) continue;
        uint8_t b[8]={0};
        if(sig==SIGWINCH) {
            if(!interactive) continue;
            struct winsize ws={0}; if(ioctl(0,TIOCGWINSZ,&ws)) return -1;
            waddle_put16(b,ws.ws_row); waddle_put16(b+2,ws.ws_col);
            waddle_put16(b+4,ws.ws_xpixel); waddle_put16(b+6,ws.ws_ypixel);
            if(wire_send(tx,seq,WADDLE_MSG_TERMINAL_RESIZE,b,8)) return -1;
        } else {
            waddle_put32(b,(uint32_t)sig);
            if(wire_send(tx,seq,WADDLE_MSG_SIGNAL_EVENT,b,4)) return -1;
        }
    }
    return 0;
}

int waddle_signal_fd(void) { return signal_pipe[0]; }
void waddle_drain_signals(void) { unsigned char b[128]; while(read(signal_pipe[0],b,sizeof(b))>0) {} }
int waddle_terminal_init(int interactive) {
    if(atexit(waddle_restore) || setup_signals()) return -1;
    if(interactive) {
        if(tcgetattr(0,&original_term)) return -1;
        term_saved=1;
        struct termios raw=original_term;
        cfmakeraw(&raw);
        if(tcsetattr(0,TCSANOW,&raw)) return -1;
    }
    return 0;
}
int waddle_standard_nonblock(void) {
    for(int i=0;i<3;i++) {
        original_flags[i]=fcntl(i,F_GETFL);
        if(original_flags[i]<0 || nonblock(i)) return -1;
    }
    return 0;
}
void waddle_terminal_close(void) {
    waddle_restore();
    for(int i=0;i<2;i++) if(signal_pipe[i]>=0) { close(signal_pipe[i]); signal_pipe[i]=-1; }
}
