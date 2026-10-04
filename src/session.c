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

int waddle_session(int fd, queue *tx, uint32_t seq, uint64_t deadline, int interactive) {
    queue output[2]={{0}}; decoder rx={0}; int result=125, spawned=0, stdin_eof=0, eof[2]={0}, exited=0;
    uint32_t exit_code=0;
    if(queue_init(&output[0]) || queue_init(&output[1])) goto done;
    for(;;) {
        if(deadline && monotonic_ms()>=deadline) { fprintf(stderr,"waddle: session timed out\n"); result=124; goto done; }
        if(spawned && !exited && waddle_send_pending(tx,&seq,interactive)) goto error;
        if(exited && !output[0].len && !output[1].len) { result=(int)(exit_code&255); goto done; }
        struct pollfd p[5]={
            {fd,(short)((!exited && queue_space(&output[0])>=WADDLE_CHUNK_SIZE && queue_space(&output[1])>=WADDLE_CHUNK_SIZE ? POLLIN : 0) | (tx->len && !exited ? POLLOUT : 0)),0},
            {0,(short)(spawned && !exited && !stdin_eof && queue_space(tx)>=WADDLE_CHUNK_SIZE+40 ? POLLIN : 0),0},
            {1,(short)(output[0].len ? POLLOUT : 0),0},
            {2,(short)(output[1].len ? POLLOUT : 0),0},
            {waddle_signal_fd(),POLLIN,0}
        };
        /* fd=-1 avoids perpetual POLLHUP on inactive stdin or completed sockets. */
        if(!p[0].events) p[0].fd=-1;
        if(!p[1].events) p[1].fd=-1;
        int timeout=-1;
        if(deadline) { uint64_t now=monotonic_ms(); uint64_t left=deadline>now ? deadline-now : 0; timeout=left>INT_MAX ? INT_MAX : (int)left; }
        int n=poll(p,5,timeout); if(n<0) { if(errno==EINTR) continue; goto error; }
        if(p[4].revents) { if(spawned) { if(waddle_send_pending(tx,&seq,interactive)) goto error; } else { waddle_drain_signals(); } }
        for(int i=0;i<2;i++) if(p[2+i].revents && queue_flush(&output[i],1+i)) goto error;
        if((p[0].revents&POLLOUT) && queue_flush(tx,fd)) goto error;
        if(p[1].revents&(POLLIN|POLLHUP|POLLERR)) {
            uint8_t b[WADDLE_CHUNK_SIZE]; ssize_t got=read(0,b,sizeof(b));
            if(got>0) { if(wire_stream(tx,&seq,0,b,(size_t)got)) goto error; }
            else if(got==0) { if(wire_eof(tx,&seq,0)) goto error; stdin_eof=1; }
            else if(errno!=EAGAIN && errno!=EINTR) goto error;
        }
        if(p[0].revents&(POLLIN|POLLHUP|POLLERR)) {
            int got=wire_read(&rx,fd); if(got==0) continue;
            if(got<0) { if(got==-2) errno=ECONNRESET; goto error; }
            uint8_t *b=rx.body; size_t len=rx.length;
            if(!spawned) {
                if(rx.type!=WADDLE_MSG_SPAWN_RESP || len<12 || waddle_get32(b+8)!=len-12) goto protocol;
                uint32_t status=waddle_get32(b);
                if(status) {
                    fprintf(stderr,"waddle: spawn failed (%u): ",status); (void)fwrite(b+12,1,len-12,stderr); fputc('\n',stderr);
                    result=status==ENOENT ? 127 : 126; goto done;
                }
                if(len!=12 || !waddle_get32(b+4)) goto protocol;
                spawned=1;
            } else if(rx.type==WADDLE_MSG_STREAM_DATA) {
                if(len<8 || b[0]<1 || b[0]>2 || b[1] || b[2] || b[3] || waddle_get32(b+4)!=len-8 || len-8>WADDLE_CHUNK_SIZE || eof[b[0]-1]) goto protocol;
                if(queue_append(&output[b[0]-1],b+8,len-8)) goto error;
            } else if(rx.type==WADDLE_MSG_STREAM_EOF) {
                if(len!=4 || waddle_get32(b)<1 || waddle_get32(b)>2) goto protocol;
                unsigned stream=waddle_get32(b)-1; if(eof[stream]) goto protocol; eof[stream]=1;
            } else if(rx.type==WADDLE_MSG_PROCESS_EXIT) {
                if(len!=16 || !eof[0] || !eof[1] || waddle_get32(b+4)>2) goto protocol;
                exit_code=waddle_get32(b); exited=1; tx->len=0;
            } else goto protocol;
            wire_consume(&rx);
        }
    }
protocol:
    errno=EPROTO;
error:
    fprintf(stderr,"waddle: session failed: %s\n",strerror(errno));
done:
    wire_destroy(&rx); queue_free(&output[0]); queue_free(&output[1]); return result;
}
