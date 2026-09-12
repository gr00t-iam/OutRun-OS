#include <assert.h>
#include <stdio.h>
#include <string.h>
#define WEB_CORE_TEST
#include "../outrun_web.c"
int main(void) {
    struct web_url u;
    assert(web_url_parse("https://example.com:8443/a?q=1#frag", &u));
    assert(u.tls && u.port == 8443 && !strcmp(u.host,"example.com"));
    assert(!strcmp(u.path,"/a?q=1"));
    assert(!web_url_parse("https://a\r\nInjected: x/", &u));
    assert(!web_url_parse("file:///etc/passwd", &u));
    assert(!web_url_parse("https://user:pw@example.com/", &u));
    assert(!web_url_parse("http://a:65536/", &u));
    puts("web: URL validation PASS");
    char out[WEB_URL];
    assert(web_resolve("https://example.com/a/b", "../c?q=1", out));
    assert(!strcmp(out,"https://example.com/c?q=1"));
    assert(!web_resolve("https://example.com/", "javascript:alert(1)", out));
    static struct web_doc d;
    web_html(&d,"<h1>Hello &amp; world</h1><script>bad()</script><a href='/next'>Next</a><img src='/x.bmp' alt='Pic'>");
    assert(d.count && d.nlinks == 1 && d.nimages == 1);
    assert(!strcmp(d.links[0],"/next"));
    assert(!strcmp(d.images[0].src,"/x.bmp"));
    for (int i=0;i<d.count;i++) assert(!strstr(d.items[i].text,"bad"));
    puts("web: URL resolution and bounded HTML PASS");
    struct web_response r;
    char response[]="HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Type: text/html\r\n\r\nhello";
    assert(web_response(response,sizeof(response)-1,0,&r)==1);
    assert(r.length==5 && !memcmp(response+r.body,"hello",5));
    char chunks[]="HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nhe\r\n3\r\nllo\r\n0\r\n\r\n";
    assert(web_response(chunks,sizeof(chunks)-2,0,&r)==0);
    assert(web_response(chunks,sizeof(chunks)-1,0,&r)==1);
    assert(r.length==5 && !memcmp(chunks+r.body,"hello",5));
    char bad[]="HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort";
    assert(web_response(bad,sizeof(bad)-1,1,&r)==-1);
    unsigned char packet[512]; unsigned ip=0;
    int qn=web_dns_query("example.com",0x1234,packet);
    assert(qn==29);
    packet[2]=0x81; packet[3]=0x80; packet[7]=1;
    unsigned char answer[]={0xc0,0x0c,0,1,0,1,0,0,0,10,0,4,93,184,216,34};
    memcpy(packet+qn,answer,sizeof answer);
    assert(web_dns_answer(packet,qn+sizeof answer,0x1234,"example.com",&ip)==1 && ip==0x5db8d822);
    assert(web_dns_answer(packet,qn+sizeof answer,0x1235,"example.com",&ip)==0);
    assert(web_dns_answer(packet,qn+sizeof answer,0x1234,"evil.com",&ip)==0);
    assert(web_dns_answer(packet,qn+5,0x1234,"example.com",&ip)==-1);
    puts("web: HTTP framing and DNS validation PASS");
    unsigned char bmp[58] = {0};
    bmp_header(bmp,1,1); bmp[54]=0x33; bmp[55]=0x22; bmp[56]=0x11;
    struct web_image image={0};
    assert(web_decode_image(&image,bmp,sizeof bmp));
    assert(image.loaded==1 && image.pixels[0]==0x112233);
    assert(!web_decode_image(&image,bmp,54));
    static struct web_tab tab;
    assert(web_history_push(&tab,"https://example.com/"));
    assert(web_history_push(&tab,"https://example.com/next"));
    assert(tab.hcount==2 && tab.hpos==1);
    tab.hpos=0;
    assert(web_history_push(&tab,"https://example.com/replacement"));
    assert(tab.hcount==2 && !strcmp(tab.history[1],"https://example.com/replacement"));
    puts("web: image decoding and history branching PASS");
    return 0;
}
