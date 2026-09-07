#ifndef OUTRUN_BUILD_TASK_H
#define OUTRUN_BUILD_TASK_H
#include "../include/outrun_abi.h"
#define OUTRUN_BUILD_OPCODE 0x4255494c44ll
static inline int build_path(const unsigned char *s,unsigned n) {
    if(n<2 || n>64 || s[n-1] || s[0]=='-') return -1;
    for(unsigned i=0;i<n-1;i++) {
        unsigned c=s[i];
        if(!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='/' || c=='.' || c=='_' || c=='-')) return -1;
    }
    return 0;
}
static inline int build_request(struct outrun_ipc_msg *m,unsigned long long pid,const char *path) {
    unsigned n=0; while(n<64 && path[n]) n++;
    if(!pid || n==64 || build_path((const unsigned char *)path,n+1)<0) return -1;
    unsigned char *p=(unsigned char *)m; for(unsigned i=0;i<sizeof *m;i++) p[i]=0;
    m->recipient_pid=pid; m->xfer_handle=OUTRUN_BUILD_OPCODE; m->payload_len=n+1;
    for(unsigned i=0;i<=n;i++) m->inline_data[i]=(unsigned char)path[i];
    return 0;
}
static inline int build_command(const struct outrun_ipc_msg *m,char out[96]) {
    if(m->msg_type || m->xfer_handle!=OUTRUN_BUILD_OPCODE || build_path(m->inline_data,m->payload_len)<0) return -1;
    int at=0; const char *prefix="cc ",*suffix=" /tmp/outrun-build.elf";
    while(*prefix) out[at++]=*prefix++;
    for(unsigned i=0;i<m->payload_len-1;i++) out[at++]=(char)m->inline_data[i];
    while(*suffix) out[at++]=*suffix++;
    out[at]=0; return 0;
}
#endif
