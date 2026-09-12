/* Exercise the exact production state machine with POSIX nonblocking sockets.
 * No synthetic network responses; run-web-network.py owns real servers. */
#define _POSIX_C_SOURCE 200809L
#define WEB_HOST_NETWORK
#include "../outrun_web.c"
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char **argv) {
    if(argc!=4)return 2;
    FILE *f=fopen(argv[1],"rb"); if(!f)return 3;
    static char roots[300000]; size_t n=fread(roots,1,sizeof roots-1,f); fclose(f); roots[n]=0;
    if(!web_runtime_init((const unsigned char *)roots,n+1))return 4;
    static struct web_job job;
    job.fd=-1;
    if(!web_start(&job,argv[2]))return 5;
    unsigned steps=0;
    while(job.state!=WEB_DONE && job.state!=WEB_ERROR) {
        web_step(&job); ++steps;
        struct timespec pause={0,1000000}; nanosleep(&pause,0);
    }
    int expected_error=!strcmp(argv[3],"ERROR");
    printf("state=%d steps=%u verified=%d status=%d error=%s\n",job.state,steps,job.verified,job.response.status,job.error);
    int ok=expected_error?job.state==WEB_ERROR:
        job.state==WEB_DONE && strstr(job.buffer+job.response.body,argv[3])!=0;
    if(job.state==WEB_DONE && job.url.tls && !job.verified)ok=0;
    web_close(&job);
    return ok?0:1;
}
