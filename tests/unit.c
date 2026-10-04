#include "common.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
static void quoting(void) {
    char *cases[]={"", "plain", "hello world", "foo\"bar", "C:\\Program Files\\", "a\\\\\"b", "tab\tvalue", "\\", "日本語", NULL};
    char *q=waddle_quote(cases); assert(q);
    char **a=waddle_unquote(q); assert(a);
    for(size_t i=0;cases[i];i++) assert(a[i] && !strcmp(a[i],cases[i]));
    assert(!a[9]); waddle_free_argv(a); free(q);
    char *one[]={"C:\\Program Files\\",NULL}; q=waddle_quote(one);
    assert(!strcmp(q,"\"C:\\Program Files\\\\\"")); free(q);
    assert(!waddle_unquote("unquoted")); assert(!waddle_unquote("\"unterminated"));
    /* Exhaust all short strings of quote-sensitive characters. */
    const char alphabet[]="a \\\"\t";
    for(unsigned n=0;n<7776;n++) {
        unsigned v=n; char s[6];
        for(unsigned i=0;i<5;i++) { s[i]=alphabet[v%6]; v/=6; } s[5]=0;
        char *args[]={s,NULL}; q=waddle_quote(args); assert(q); a=waddle_unquote(q);
        assert(a && !strcmp(a[0],s) && !a[1]); free(q); waddle_free_argv(a);
    }
}
static void frames(void) {
    queue q; assert(!queue_init(&q)); uint32_t seq=1;
    assert(!wire_send(&q,&seq,WADDLE_MSG_SIGNAL_EVENT,"abcd",4));
    int fd[2]; assert(!socketpair(AF_UNIX,SOCK_STREAM,0,fd)); assert(!nonblock(fd[1]));
    decoder d={0};
    for(size_t i=0;i<q.len;i++) {
        assert(write(fd[0],q.data+i,1)==1);
        assert(wire_read(&d,fd[1])==(i==q.len-1 ? 1 : 0));
    }
    assert(d.type==WADDLE_MSG_SIGNAL_EVENT && d.length==4 && !memcmp(d.body,"abcd",4)); wire_consume(&d);
    q.data[24]=2; q.data[28]^=1;
    assert(write(fd[0],q.data,q.len)==(ssize_t)q.len); assert(wire_read(&d,fd[1])==-1); wire_destroy(&d);
    waddle_put32(q.data+16,WADDLE_MAX_PAYLOAD_SIZE+1);
    assert(write(fd[0],q.data,32)==32); assert(wire_read(&d,fd[1])==-1); wire_destroy(&d);
    close(fd[0]); close(fd[1]); queue_free(&q);
    assert(!socketpair(AF_UNIX,SOCK_STREAM,0,fd)); assert(write(fd[0],"bad",3)==3); close(fd[0]);
    assert(wire_read(&d,fd[1])==-1); wire_destroy(&d); close(fd[1]);
}
int main(void) {
    assert(waddle_crc32("123456789",9)==UINT32_C(0xcbf43926)); assert(waddle_crc32(NULL,0)==0);
    uint8_t b[8]; waddle_put64(b,UINT64_C(0x0123456789abcdef)); assert(b[0]==0xef && b[7]==1);
    assert(waddle_get64(b)==UINT64_C(0x0123456789abcdef));
    quoting(); frames();
    char *p=waddle_translate_path("/home/dev/a b"); assert(p && !strcmp(p,"Z:\\home\\dev\\a b")); free(p);
    p=waddle_translate_path("relative/file"); assert(p && !strcmp(p,"relative/file")); free(p);
    assert(!waddle_translate_path("/a/../b")); assert(!waddle_translate_path("/a\\b"));
    puts("unit: layouts, CRC, 7776 quoting cases, paths, fragmented/corrupt/truncated frames passed");
    return 0;
}
