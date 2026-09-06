/* Run: gcc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined
 * apps/tests/test_vault_pad.c -o /tmp/test_vault_pad && /tmp/test_vault_pad
 * Host adapter uses real files; injected errors exercise actual editor loops.
 * This is not a guest-kernel/VFS or window-manager integration test. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define APP_HOST_TEST
#include "../vault_pad.c"
static void insertion(void) {
    struct vp_editor e; vp_init(&e);
    assert(vp_insert(&e,'a')); assert(vp_insert(&e,'\n')); assert(vp_insert(&e,'b'));
    e.cursor=1; assert(vp_insert(&e,'X'));
    assert(e.len==4 && e.cursor==2 && e.dirty && !memcmp(e.text,"aX\nb",4));
    while(e.len<VP_CAP) assert(vp_insert(&e,'z'));
    assert(!vp_insert(&e,'!') && e.len==VP_CAP && e.text[VP_CAP]==0);
}
static void navigation(void) {
    struct vp_editor e; vp_init(&e);
    const char *s="abcd\nx\nxyz"; for(int i=0;s[i];i++) vp_insert(&e,s[i]);
    e.cursor=3; vp_move(&e,VP_DOWN); assert(e.cursor==6);
    vp_move(&e,VP_DOWN); assert(e.cursor==8);
    vp_move(&e,VP_HOME); assert(e.cursor==7);
    vp_move(&e,VP_UP); assert(e.cursor==5);
    vp_move(&e,VP_END); assert(e.cursor==6);
    vp_delete(&e,0); assert(!strcmp(e.text,"abcd\nxxyz"));
    vp_delete(&e,1); assert(!strcmp(e.text,"abcd\nxyz"));
    e.cursor=0; e.dirty=0; vp_delete(&e,1); vp_move(&e,VP_LEFT); vp_move(&e,VP_UP);
    assert(e.cursor==0 && !e.dirty);
    e.cursor=e.len; vp_delete(&e,0); vp_move(&e,VP_RIGHT); vp_move(&e,VP_DOWN);
    assert(e.cursor==e.len && !e.dirty);
    vp_visible(&e,1,2); assert(e.top==1 && e.left==2);
    e.cursor=0; vp_visible(&e,1,2); assert(e.top==0 && e.left==0);
}
#include <unistd.h>
#include <fcntl.h>
static int io_fail, open_flags, io_chunk=17;
static long long host_io(int op,unsigned long long a,unsigned long long b,unsigned long long c) {
    if(op==5) { open_flags=(int)b; return open((const char *)a,O_RDWR|(b==1?O_CREAT:0),0600); }
    if(op==8) return close((int)a);
    if(op==22) return io_fail==22?-1:0;
    if(io_fail==op) return -1;
    if(op==6) return read((int)a,(void *)b,c>(unsigned)io_chunk?(unsigned)io_chunk:c);
    if(op==7) return write((int)a,(void *)b,c>(unsigned)io_chunk?(unsigned)io_chunk:c);
    if(op==101) return ftruncate((int)a,(off_t)b);
    return -1;
}
static void file_io(void) {
    struct vp_editor e; vp_init(&e);
    strcpy(e.filename,"/tmp/vault_pad_test.txt"); e.name_len=(int)strlen(e.filename);
    unlink(e.filename);
    assert(!vp_open(&e,host_io)); assert(access(e.filename,F_OK)!=0 && open_flags==0);
    vp_insert(&e,'a'); vp_insert(&e,'\n'); vp_insert(&e,'b');
    assert(vp_save(&e,host_io) && !e.dirty && open_flags==1);
    e.cursor=0; vp_delete(&e,0); assert(vp_save(&e,host_io));
    assert(vp_open(&e,host_io) && e.len==2 && !strcmp(e.text,"\nb") && open_flags==0);
    vp_insert(&e,'X'); assert(!vp_open(&e,host_io) && e.dirty && e.len==3);
    io_fail=7; assert(!vp_save(&e,host_io) && e.dirty); io_fail=0;
    assert(vp_save(&e,host_io));
    io_fail=101; e.dirty=1; assert(!vp_save(&e,host_io) && e.dirty);
    io_fail=22; assert(!vp_save(&e,host_io) && e.dirty);
    io_fail=0; assert(vp_save(&e,host_io) && !e.dirty);
    e.filename[0]=0; assert(!vp_save(&e,host_io) && !vp_open(&e,host_io));
    strcpy(e.filename,"/tmp/vault_pad_test.txt");
    io_fail=6; assert(!vp_open(&e,host_io) && e.len==3); io_fail=0;
    e.len=e.cursor=0; e.text[0]=0; e.dirty=1; assert(vp_save(&e,host_io));
    assert(vp_open(&e,host_io) && e.len==0);
    while(e.len<VP_CAP) vp_insert(&e,'k');
    io_chunk=65536; assert(vp_save(&e,host_io)); assert(vp_open(&e,host_io) && e.len==VP_CAP);
    int fd=open(e.filename,O_WRONLY|O_APPEND); assert(fd>=0); assert(write(fd,"x",1)==1); close(fd);
    assert(!vp_open(&e,host_io) && e.len==VP_CAP);
    unlink(e.filename);
}
static void interaction(void) {
    struct vp_editor e; vp_init(&e);
    assert(vp_click(&e,590,12,720,480)==VP_OPEN);
    assert(vp_click(&e,660,12,720,480)==VP_SAVE);
    assert(vp_click(&e,-1,-1,720,480)==VP_NONE);
    vp_click(&e,100,12,720,480); assert(e.focus);
    e.name_cursor=0; vp_key(&e,'X'); assert(e.filename[0]=='X');
    vp_key(&e,8); assert(!strcmp(e.filename,"notes.txt"));
    while(e.name_len<VP_NAME_CAP) vp_key(&e,'a');
    vp_key(&e,'b'); assert(e.name_len==VP_NAME_CAP && !e.filename[VP_NAME_CAP]);
    vp_key(&e,9); assert(!e.focus);
    vp_key(&e,'a'); vp_key(&e,10); vp_key(&e,'b');
    vp_key(&e,27); assert(e.command);
    vp_key(&e,'h'); assert(e.cursor==2);
    vp_key(&e,'x'); assert(!strcmp(e.text,"a\n"));
    assert(vp_key(&e,'s')==VP_SAVE); assert(vp_key(&e,'o')==VP_OPEN);
    vp_key(&e,'k'); assert(e.cursor==0);
    vp_key(&e,27); assert(!e.command);
    vp_click(&e,8,72,720,480); assert(e.cursor==2);
}
static int paint_chars, paint_rects, paint_star;
static void paint_probe(void *ctx,int x,int y,int w,int h,unsigned color,int c) {
    (void)ctx; (void)color;
    assert(x>=0 && y>=0 && w>0 && h>0 && x+w<=720 && y+h<=480);
    if(c) { paint_chars++; if(c=='*') paint_star++; } else paint_rects++;
}
static void rendering(void) {
    struct vp_editor e; vp_init(&e); vp_insert(&e,'a');
    vp_paint(&e,720,480,paint_probe,0);
    assert(paint_chars>30 && paint_rects>3 && paint_star==1);
    e.focus=1; e.name_cursor=e.name_len;
    vp_paint(&e,720,480,paint_probe,0);
}
/* g / G jump to the start and end of the whole document, not the line. */
static void document_jumps(void) {
    struct vp_editor e; vp_init(&e);
    const char *s="one\ntwo\nthree"; for(int i=0;s[i];i++) vp_insert(&e,s[i]);
    e.cursor=5; e.dirty=0; vp_key(&e,27);
    vp_key(&e,'g'); assert(e.cursor==0);
    vp_key(&e,'G'); assert(e.cursor==e.len);
    vp_key(&e,'g'); assert(e.cursor==0 && !e.dirty);
}
/* y yanks the cursor line into a clipboard, D cuts it, p pastes it below the
 * cursor line. The clipboard survives across edits and is bounded. */
static void line_clipboard(void) {
    struct vp_editor e; vp_init(&e);
    const char *s="alpha\nbeta\ngamma"; for(int i=0;s[i];i++) vp_insert(&e,s[i]);
    e.dirty=0; e.cursor=7; vp_key(&e,27);
    vp_key(&e,'y'); assert(!strcmp(e.clip,"beta") && !e.dirty);
    vp_key(&e,'D'); assert(!strcmp(e.text,"alpha\ngamma") && e.dirty && !strcmp(e.clip,"beta"));
    assert(e.cursor==6);
    vp_key(&e,'p'); assert(!strcmp(e.text,"alpha\ngamma\nbeta"));
    e.cursor=e.len; vp_key(&e,'D'); assert(!strcmp(e.text,"alpha\ngamma") && !strcmp(e.clip,"beta"));
    e.cursor=0; vp_key(&e,'D'); assert(!strcmp(e.text,"gamma") && !strcmp(e.clip,"alpha"));
    vp_key(&e,'D'); assert(e.len==0 && !strcmp(e.clip,"gamma"));
    vp_key(&e,'D'); assert(e.len==0 && !strcmp(e.clip,"gamma"));
    vp_key(&e,'p'); assert(!strcmp(e.text,"gamma"));
    vp_key(&e,'p'); assert(!strcmp(e.text,"gamma\ngamma"));
    /* An empty clipboard pastes nothing and does not dirty the buffer. */
    vp_init(&e); vp_key(&e,27); vp_key(&e,'p'); assert(e.len==0 && !e.dirty);
    /* A line longer than the clipboard is refused, not truncated. */
    vp_init(&e); for(int i=0;i<VP_CLIP_CAP+1;i++) vp_insert(&e,'q');
    vp_key(&e,27); vp_key(&e,'y'); assert(!e.clip[0]);
    assert(!strcmp(e.status,"LINE TOO LONG FOR CLIPBOARD"));
}
/* Saving onto a file that exists and is NOT the one this buffer was opened
 * from must ask first. The second save within the same command confirms. */
static void overwrite_confirmation(void) {
    struct vp_editor e; vp_init(&e);
    strcpy(e.filename,"/tmp/vault_pad_ow.txt"); e.name_len=(int)strlen(e.filename);
    unlink(e.filename);
    vp_insert(&e,'a');
    /* New file: no confirmation needed. */
    assert(vp_save(&e,host_io) && !e.dirty);
    /* Same file, opened from it: no confirmation needed either. */
    vp_insert(&e,'b'); assert(vp_save(&e,host_io) && !e.dirty);
    /* Now point at a DIFFERENT existing file. */
    int fd=open("/tmp/vault_pad_ow2.txt",O_WRONLY|O_CREAT|O_TRUNC,0600); assert(fd>=0);
    assert(write(fd,"keep",4)==4); close(fd);
    strcpy(e.filename,"/tmp/vault_pad_ow2.txt"); e.name_len=(int)strlen(e.filename);
    vp_insert(&e,'c');
    assert(!vp_save(&e,host_io) && e.dirty && e.confirm);
    assert(!strcmp(e.status,"FILE EXISTS: SAVE AGAIN TO OVERWRITE"));
    char buf[8]; fd=open("/tmp/vault_pad_ow2.txt",O_RDONLY); assert(read(fd,buf,8)==4); close(fd);
    /* Any other keystroke withdraws the pending confirmation. */
    vp_key(&e,'z'); assert(!e.confirm);
    assert(!vp_save(&e,host_io) && e.confirm);
    assert(vp_save(&e,host_io) && !e.dirty && !e.confirm);
    fd=open("/tmp/vault_pad_ow2.txt",O_RDONLY); assert(read(fd,buf,8)==4); close(fd);
    assert(!memcmp(buf,"abcz",4));
    /* Opening a file adopts it: later saves to it need no confirmation. */
    vp_init(&e); strcpy(e.filename,"/tmp/vault_pad_ow2.txt"); e.name_len=(int)strlen(e.filename);
    assert(vp_open(&e,host_io)); vp_insert(&e,'!');
    assert(vp_save(&e,host_io) && !e.dirty);
    unlink("/tmp/vault_pad_ow.txt"); unlink("/tmp/vault_pad_ow2.txt");
}
int main(void) {
    insertion(); navigation(); file_io(); interaction(); rendering();
    document_jumps(); line_clipboard(); overwrite_confirmation();
    puts("vault_pad: all core tests PASS");
}
