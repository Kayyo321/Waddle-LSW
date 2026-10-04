#include "common.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
uint16_t waddle_get16(const void *p) { const uint8_t *b=p; return (uint16_t)(b[0] | (uint16_t)b[1]<<8); }
uint32_t waddle_get32(const void *p) { const uint8_t *b=p; return (uint32_t)b[0] | (uint32_t)b[1]<<8 | (uint32_t)b[2]<<16 | (uint32_t)b[3]<<24; }
uint64_t waddle_get64(const void *p) { const uint8_t *b=p; return waddle_get32(b) | (uint64_t)waddle_get32(b+4)<<32; }
void waddle_put16(void *p, uint16_t n) { uint8_t *b=p; b[0]=(uint8_t)n; b[1]=(uint8_t)(n>>8); }
void waddle_put32(void *p, uint32_t n) { uint8_t *b=p; for(unsigned i=0;i<4;i++) b[i]=(uint8_t)(n>>(8*i)); }
void waddle_put64(void *p, uint64_t n) { uint8_t *b=p; waddle_put32(b,(uint32_t)n); waddle_put32(b+4,(uint32_t)(n>>32)); }
uint32_t waddle_crc32(const void *p, size_t n) {
    const uint8_t *b=p; uint32_t crc=UINT32_MAX;
    while(n--) { crc^=*b++; for(unsigned i=0;i<8;i++) crc=(crc>>1) ^ (UINT32_C(0xedb88320) & (0u-(crc&1))); }
    return ~crc;
}
int queue_init(queue *q) { memset(q,0,sizeof(*q)); q->data=malloc(WADDLE_QUEUE_SIZE); return q->data ? 0 : -1; }
void queue_free(queue *q) { free(q->data); memset(q,0,sizeof(*q)); }
size_t queue_space(const queue *q) { return WADDLE_QUEUE_SIZE-q->len; }
int queue_append(queue *q, const void *p, size_t n) {
    if(n>queue_space(q)) { errno=ENOBUFS; return -1; }
    if(q->off+q->len+n>WADDLE_QUEUE_SIZE) { memmove(q->data,q->data+q->off,q->len); q->off=0; }
    if(n) memcpy(q->data+q->off+q->len,p,n);
    q->len+=n; return 0;
}
int queue_flush(queue *q, int fd) {
    if(!q->len) return 0;
    ssize_t n=write(fd,q->data+q->off,q->len);
    if(n<0) return errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR ? 0 : -1;
    if(n==0) { errno=EIO; return -1; }
    q->off+=(size_t)n; q->len-=(size_t)n; if(!q->len) q->off=0; return 0;
}
int wire_send(queue *q, uint32_t *seq, uint16_t type, const void *p, size_t n) {
    if(n>WADDLE_MAX_PAYLOAD_SIZE || queue_space(q)<n+32) { errno=ENOBUFS; return -1; }
    uint8_t h[32]={0}; waddle_put32(h,WADDLE_CLI_MAGIC); waddle_put16(h+4,1);
    waddle_put16(h+6,type); waddle_put64(h+8,1); waddle_put32(h+16,(uint32_t)n);
    waddle_put32(h+24,(*seq)++); waddle_put32(h+28,waddle_crc32(p,n));
    return queue_append(q,h,32) || queue_append(q,p,n) ? -1 : 0;
}
int wire_stream(queue *q, uint32_t *seq, unsigned stream, const void *p, size_t n) {
    if(n>WADDLE_CHUNK_SIZE) { errno=EINVAL; return -1; }
    uint8_t b[WADDLE_CHUNK_SIZE+8]={0}; b[0]=(uint8_t)stream; waddle_put32(b+4,(uint32_t)n);
    memcpy(b+8,p,n); return wire_send(q,seq,WADDLE_MSG_STREAM_DATA,b,n+8);
}
int wire_eof(queue *q, uint32_t *seq, unsigned stream) { uint8_t b[4]; waddle_put32(b,stream); return wire_send(q,seq,WADDLE_MSG_STREAM_EOF,b,4); }
int wire_read(decoder *d, int fd) {
    if(!d->started) { d->next=1; d->started=1; }
    for(;;) {
        size_t target=d->body ? d->length+32 : 32;
        if(d->have<target) {
            uint8_t *dest=d->body ? d->body+d->have-32 : d->header+d->have;
            ssize_t n=read(fd,dest,target-d->have);
            if(n==0) { if(d->have) { errno=EPROTO; return -1; } return -2; }
            if(n<0) { if(errno==EINTR) continue; return errno==EAGAIN || errno==EWOULDBLOCK ? 0 : -1; }
            d->have+=(size_t)n; if(d->have<target) continue;
        }
        if(!d->body) {
            const uint8_t *h=d->header;
            if(waddle_get32(h)!=WADDLE_CLI_MAGIC || waddle_get16(h+4)!=1 || waddle_get64(h+8)!=1 ||
               waddle_get32(h+20) || waddle_get32(h+24)!=d->next || waddle_get32(h+16)>WADDLE_MAX_PAYLOAD_SIZE) { errno=EPROTO; return -1; }
            d->length=waddle_get32(h+16); d->type=waddle_get16(h+6);
            d->body=malloc(d->length ? d->length : 1); if(!d->body) return -1;
            if(d->length) continue;
        }
        if(waddle_crc32(d->body,d->length)!=waddle_get32(d->header+28)) { errno=EPROTO; return -1; }
        return 1;
    }
}
void wire_consume(decoder *d) { free(d->body); d->body=NULL; d->have=0; d->length=0; d->next++; }
void wire_destroy(decoder *d) { free(d->body); memset(d,0,sizeof(*d)); }
int nonblock(int fd) { int f=fcntl(fd,F_GETFL); return f<0 ? -1 : fcntl(fd,F_SETFL,f|O_NONBLOCK); }
uint64_t monotonic_ms(void) { struct timespec ts; if(clock_gettime(CLOCK_MONOTONIC,&ts)) return 0; return (uint64_t)ts.tv_sec*1000+(uint64_t)ts.tv_nsec/1000000; }
