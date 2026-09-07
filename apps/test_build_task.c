#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "build_task.h"
int main(void) {
    struct outrun_ipc_msg m; char cmd[96];
    assert(build_request(&m,123,"/src/demo.c")==0);
    assert(m.recipient_pid==123 && m.payload_len==12 && sizeof m==104);
    assert(build_command(&m,cmd)==0 && !strcmp(cmd,"cc /src/demo.c /tmp/outrun-build.elf"));
    assert(build_request(&m,123,"x;reboot")==-1);
    assert(build_request(&m,0,"x.c")==-1);
    assert(build_request(&m,123,"-bad")==-1);
    assert(build_request(&m,123,"x.c")==0);
    m.payload_len=65; assert(build_command(&m,cmd)==-1);
    m.payload_len=4; m.inline_data[3]='x'; assert(build_command(&m,cmd)==-1);
    puts("build IPC: ABI, recipient, payload validation and command construction PASS");
}
