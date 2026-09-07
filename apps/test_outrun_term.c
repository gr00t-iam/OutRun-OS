#include <assert.h>
#include <stdio.h>
#include <string.h>
#define APP_HOST_TEST
#include "outrun_term.c"
static struct term_session s;
static int calls;
static long long failure(const char *cmd,char *out,unsigned cap) {
    (void)cmd; (void)out; (void)cap; return -13;
}
static long long command(const char *cmd, char *out, unsigned cap) {
    assert(!strcmp(cmd,"help")); assert(cap == 4096); calls++;
    memcpy(out,"\033[32mOK\033[0m\r\n",13); return 13;
}
int main(void) {
    term_init(&s);
    struct term_tab *t=&s.tabs[0];
    term_feed(t,"abc\rZ",5);
    assert(term_cell(t,0,0)->ch=='Z' && term_cell(t,0,1)->ch=='b');
    term_feed(t,"\033[2;4H\033[31mX",12);
    assert(term_cell(t,1,3)->ch=='X' && term_cell(t,1,3)->fg==1);
    term_feed(t,"\033[2J\033[H",7);
    assert(term_cell(t,1,3)->ch==' ' && t->row==0 && t->col==0);
    const char *overflow="\033[9999999999999999999999C!";
    term_feed(t,overflow,(unsigned)strlen(overflow));
    assert(t->col==TERM_COLS && term_cell(t,0,TERM_COLS-1)->ch=='!');
    term_init(&s); t=&s.tabs[0];
    for(int i=0;i<1100;i++) term_feed(t,"x\r\n",3);
    assert(t->history==1000 && t->row==TERM_ROWS-1);
    term_scroll(t,2000); assert(t->view==1000);
    term_scroll(t,-2000); assert(t->view==0);
    term_feed(t,"hello",5);
    term_select(t,0,TERM_ROWS-1); term_select(t,5,TERM_ROWS-1);
    char copy[12]; assert(term_copy(t,copy,sizeof copy)==5 && !strcmp(copy,"hello"));
    assert(term_copy(t,copy,3)==2 && !strcmp(copy,"he"));
    assert(term_tab_switch(&s,1)==1 && s.active==1);
    assert(s.tabs[1].history==0 && s.tabs[0].history==1000);
    assert(!term_tab_switch(&s,TERM_TABS));
    for(const char *p="help";*p;p++) term_key(&s,*p,command);
    term_key(&s,13,command);
    assert(calls==1 && s.tabs[1].cmdlen==0);
    assert(term_cell(&s.tabs[1],1,0)->ch=='O');
    term_key(&s,13,failure);
    assert(term_cell(&s.tabs[1],3,0)->ch=='c');
    term_key(&s,27,command); term_key(&s,'1',command); assert(s.active==0);
    /* Malformed and fragmented escape sequences must be bounded and recover. */
    term_init(&s); t=&s.tabs[0];
    term_feed(t,"\033[",2); term_feed(t,"3",1); term_feed(t,"4mB",3);
    assert(term_cell(t,0,0)->ch=='B' && term_cell(t,0,0)->fg==4);
    const char *erase="\033[2K\033[2;1Habc\033[1K";
    term_feed(t,erase,(unsigned)strlen(erase));
    assert(term_cell(t,0,0)->ch==' ' && term_cell(t,1,0)->ch==' ');
    const char *save="\033[4;5H\0337\033[1;1H\0338Z";
    term_feed(t,save,(unsigned)strlen(save));
    assert(term_cell(t,3,4)->ch=='Z');
    for(int i=0;i<120;i++) term_key(&s,'a',failure);
    assert(t->cmdlen==95 && t->cmd[95]==0);
    unsigned seed=42;
    for(int i=0;i<100000;i++) {
        seed=seed*1664525u+1013904223u; char c=(char)(seed>>24);
        term_feed(t,&c,1);
        assert(t->row>=0 && t->row<TERM_ROWS && t->col>=0 && t->col<=TERM_COLS);
    }
    printf("outrun_term: ANSI, 1000-line ring, selection, tabs, command routing, malformed streams PASS; session=%zu bytes\n",sizeof s);
}
