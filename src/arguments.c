#include "waddle/cli_protocol.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
char *waddle_quote(char *const argv[]) {
    size_t size=1;
    for(size_t i=0;argv[i];i++) {
        size_t n=strlen(argv[i]);
        if(size>WADDLE_MAX_PAYLOAD_SIZE-3 || n>(WADDLE_MAX_PAYLOAD_SIZE-size-3)/2) { errno=E2BIG; return NULL; }
        size+=2*n+3;
    }
    char *out=malloc(size), *p=out; if(!out) return NULL;
    for(size_t i=0;argv[i];i++) {
        if(i) *p++=' ';
        *p++='"'; const char *s=argv[i];
        while(*s) {
            size_t slashes=0; while(*s=='\\') { slashes++; s++; }
            size_t emit=(*s=='"' || !*s) ? 2*slashes : slashes;
            while(emit--) *p++='\\';
            if(*s=='"') *p++='\\';
            if(*s) *p++=*s++;
        }
        *p++='"';
    }
    *p=0; return out;
}
void waddle_free_argv(char **argv) { if(argv) { for(size_t i=0;argv[i];i++) free(argv[i]); free(argv); } }
char **waddle_unquote(const char *command) {
    size_t len=strlen(command), count=0;
    char **argv=calloc(len/2+2,sizeof(*argv)); if(!argv) return NULL;
    const char *s=command;
    while(*s) {
        if(*s!='"') goto invalid;
        s++; char *a=malloc(strlen(s)+1); if(!a) goto invalid;
        argv[count++]=a; char *p=a;
        for(;;) {
            size_t n=0; while(*s=='\\') { n++; s++; }
            if(*s=='"') {
                for(size_t i=0;i<n/2;i++) *p++='\\';
                s++; if(n%2) { *p++='"'; continue; }
                break;
            }
            while(n--) *p++='\\';
            if(!*s) goto invalid;
            *p++=*s++;
        }
        *p=0;
        if(*s==' ') s++; else if(*s) goto invalid;
    }
    if(!count) goto invalid;
    return argv;
invalid:
    waddle_free_argv(argv); errno=EINVAL; return NULL;
}
char *waddle_translate_path(const char *path) {
    if(path[0]!='/') return strdup(path);
    if(strchr(path,'\\')) { errno=EINVAL; return NULL; }
    for(const char *p=path;*p;) {
        while(*p=='/') p++;
        const char *start=p; while(*p && *p!='/') p++;
        if(p-start==2 && start[0]=='.' && start[1]=='.') { errno=EINVAL; return NULL; }
    }
    size_t n=strlen(path); if(n>WADDLE_MAX_PAYLOAD_SIZE-3) { errno=E2BIG; return NULL; }
    char *out=malloc(n+3); if(!out) return NULL;
    out[0]='Z'; out[1]=':';
    for(size_t i=0;i<=n;i++) out[i+2]=path[i]=='/' ? '\\' : path[i];
    return out;
}
