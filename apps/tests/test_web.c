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

    /* ---- Slice A: DOM, inline vs block, <span> and <hr> ------------------
     * The layout is asserted through the DOM the parser builds, not through
     * pixel coordinates alone: coordinates are a consequence and change with
     * font metrics, but "a block starts a new line and an inline does not" is
     * the actual contract. web_node_at finds the node that produced an item. */

    /* A span is INLINE: text either side of it stays on one line. A div is
     * BLOCK: it breaks. This is the single distinction the old one-pass loop
     * could not express -- it treated every tag it knew as a line break and
     * every tag it did not as nothing at all. */
    web_html(&d,"<p>alpha <span>beta</span> gamma</p>");
    {
        int y0=-1,same=1;
        for (int i=0;i<d.count;i++) {
            if (y0<0) y0=d.items[i].y;
            else if (d.items[i].y!=y0) same=0;
        }
        assert(d.count==3 && same);     /* alpha beta gamma, all one line */
    }
    web_html(&d,"<div>alpha</div><div>beta</div>");
    assert(d.count==2 && d.items[1].y > d.items[0].y);

    /* An unknown tag must be transparent, not a line break: real pages are
     * full of <section>, <main>, <nav> wrappers around inline runs. Unknown
     * INLINE-position text must keep flowing. */
    web_html(&d,"<p>alpha <em>beta</em> gamma</p>");
    {
        int y0=d.items[0].y,same=1;
        for (int i=1;i<d.count;i++) if (d.items[i].y!=y0) same=0;
        assert(d.count==3 && same);
    }

    /* <hr> is a block-level rule: it occupies its own line and is a RULE, not
     * text. It must be distinguishable from an empty text item, or the
     * renderer cannot draw it. */
    web_html(&d,"<p>above</p><hr><p>below</p>");
    {
        int rules=0,rule_y=-1;
        for (int i=0;i<d.count;i++)
            if (d.items[i].kind==WEB_ITEM_RULE) { rules++; rule_y=d.items[i].y; }
        assert(rules==1);
        assert(d.items[0].y < rule_y);          /* above, then the rule */
        int below=-1;
        for (int i=0;i<d.count;i++)
            if (d.items[i].kind==WEB_ITEM_TEXT && !strcmp(d.items[i].text,"below")) below=d.items[i].y;
        assert(below>rule_y);                   /* then below */
    }

    /* Nested blocks: an inner block still breaks, and the block that follows
     * a nested one does not collapse onto the inner block's last line. */
    web_html(&d,"<div>one<div>two</div>three</div><div>four</div>");
    {
        int y[4]={-1,-1,-1,-1};
        for (int i=0;i<d.count;i++) {
            if (!strcmp(d.items[i].text,"one"))   y[0]=d.items[i].y;
            if (!strcmp(d.items[i].text,"two"))   y[1]=d.items[i].y;
            if (!strcmp(d.items[i].text,"three")) y[2]=d.items[i].y;
            if (!strcmp(d.items[i].text,"four"))  y[3]=d.items[i].y;
        }
        assert(y[0]>=0 && y[1]>y[0] && y[2]>y[1] && y[3]>y[2]);
    }

    /* Headings are blocks AND carry their level, so the renderer can size or
     * colour them without re-parsing the tag name. */
    web_html(&d,"<h1>Big</h1><p>body</p><h3>Small</h3>");
    {
        int h1=-1,h3=-1;
        for (int i=0;i<d.count;i++) {
            if (!strcmp(d.items[i].text,"Big"))   h1=d.items[i].heading;
            if (!strcmp(d.items[i].text,"Small")) h3=d.items[i].heading;
        }
        assert(h1==1 && h3==3);
    }

    /* A link inside a span inside a paragraph still resolves: nesting must not
     * lose the href, which a flat parser drops as soon as a second tag opens. */
    web_html(&d,"<p>see <span>the <a href='/deep'>link</a></span> here</p>");
    {
        int found=-1;
        for (int i=0;i<d.count;i++) if (!strcmp(d.items[i].text,"link")) found=d.items[i].link;
        assert(found>=0 && !strcmp(d.links[found],"/deep"));
    }

    /* Line wrapping still happens inside a block, and wrapped continuation
     * lines belong to the same block rather than resetting to the document. */
    {
        char wide[512]; wide[0]=0;
        strcat(wide,"<p>");
        for (int i=0;i<40;i++) strcat(wide,"word ");
        strcat(wide,"</p>");
        web_html(&d,wide);
        int lines=1,y0=d.items[0].y;
        for (int i=1;i<d.count;i++) if (d.items[i].y!=y0) { lines++; y0=d.items[i].y; }
        assert(d.count==40 && lines>1);
    }
    puts("web: DOM inline/block, span, hr, nesting and headings PASS");

    /* ---- Slice A part 2: shared pixel pool --------------------------------
     * Before: struct web_image embedded unsigned pixels[96*64] = 24 KB, so
     * web_doc was 414 KB of which 201 KB was image pixels, replicated across
     * 4 tabs AND the snapshot -- ~983 KB of BSS reserved whether or not any
     * page had a single image. The 8-image cap was a memory limit wearing a
     * feature's clothing. Pixels now live in one pool; web_image carries a
     * slot index. */

    /* The document is dramatically smaller and the image array is metadata. */
    assert(sizeof(struct web_image) < 1024);
    assert(sizeof(struct web_doc) < 256*1024);
    /* And the cap is a real count now, not 24 KB apiece. */
    assert(WEB_IMAGES >= 32);

    /* More than the old 8 images parse and are addressable. */
    {
        static char many[16384]; many[0]=0;
        for (int i=0;i<WEB_IMAGES+4;i++) strcat(many,"<img src='/i.bmp'>");
        web_html(&d,many);
        assert(d.nimages==WEB_IMAGES);        /* capped, not overrun */
        for (int i=0;i<d.nimages;i++) assert(!strcmp(d.images[i].src,"/i.bmp"));
        /* Every item that references an image references a VALID one. */
        for (int i=0;i<d.count;i++)
            assert(d.items[i].image>=-1 && d.items[i].image<d.nimages);
    }

    /* Pool slots are claimed lazily, on DECODE, not on parse: a page that
     * mentions 40 images it never loads must not exhaust the pool. */
    web_html(&d,"<img src='/a.bmp'><img src='/b.bmp'>");
    assert(d.nimages==2);
    assert(d.images[0].slot < 0 && d.images[1].slot < 0);
    assert(web_pool_used(&d)==0);

    /* A decode claims a slot and the pixels land in the pool, reachable
     * through the accessor the renderer uses. */
    {
        unsigned char bmp[BMP_HEADER_BYTES+4]={0};
        bmp_header(bmp,1,1); bmp[54]=0x33; bmp[55]=0x22; bmp[56]=0x11;
        assert(web_decode_image(&d,&d.images[0],bmp,sizeof bmp));
        assert(d.images[0].loaded==1 && d.images[0].slot>=0);
        assert(web_pool_used(&d)==1);
        const unsigned *px=web_pool_pixels(&d,d.images[0].slot);
        assert(px && px[0]==0x112233u);
        /* The second image is untouched: one decode must not disturb another
         * image's metadata or claim its slot. */
        assert(d.images[1].slot<0 && d.images[1].loaded==0);
    }

    /* Two decoded images occupy DISTINCT slots and do not alias -- the
     * corruption a shared pool invites if the allocator hands out a slot
     * twice. Write through one, read the other. */
    {
        unsigned char bmp[BMP_HEADER_BYTES+4]={0};
        bmp_header(bmp,1,1); bmp[54]=0x66; bmp[55]=0x55; bmp[56]=0x44;
        assert(web_decode_image(&d,&d.images[1],bmp,sizeof bmp));
        assert(d.images[0].slot != d.images[1].slot);
        assert(web_pool_used(&d)==2);
        assert(web_pool_pixels(&d,d.images[0].slot)[0]==0x112233u);
        assert(web_pool_pixels(&d,d.images[1].slot)[0]==0x445566u);
    }

    /* Pool exhaustion fails CLOSED: the image reports undecodable rather than
     * scribbling outside the pool or silently reusing someone else's slot.
     * Note `d` still holds slots from the aliasing check above -- that is the
     * point. The pool is shared across documents, so this asserts against the
     * slots ACTUALLY FREE at this moment rather than assuming an empty pool,
     * which is the condition a second tab loading images really faces. */
    {
        static struct web_doc full;
        unsigned char bmp[BMP_HEADER_BYTES+4]={0};
        bmp_header(bmp,1,1); bmp[54]=0x99; bmp[55]=0x88; bmp[56]=0x77;
        int free_before=0;
        for (int i=0;i<WEB_POOL_SLOTS;i++) if (!web_pool_taken[i]) free_before++;
        assert(free_before>0 && free_before<WEB_POOL_SLOTS);   /* genuinely partial */
        static char many[16384]; many[0]=0;
        for (int i=0;i<WEB_IMAGES;i++) strcat(many,"<img src='/i.bmp'>");
        web_html(&full,many);
        int decoded=0;
        for (int i=0;i<full.nimages;i++)
            if (web_decode_image(&full,&full.images[i],bmp,sizeof bmp)) decoded++;
        assert(decoded==free_before);
        assert(web_pool_used(&full)==free_before);
        for (int i=0;i<full.nimages;i++) {
            if (full.images[i].slot>=0) assert(full.images[i].loaded==1);
            else assert(full.images[i].loaded==-1);   /* refused, not pending */
        }
        /* Every slot in the pool is now spoken for, and the two documents'
         * claims are disjoint -- no slot is owned twice. */
        for (int i=0;i<WEB_POOL_SLOTS;i++) {
            assert(web_pool_taken[i]);
            assert(!(d.owns[i] && full.owns[i]));
        }
        web_pool_release(&full);
    }

    /* Out-of-range slot indices fail closed rather than indexing the pool. */
    assert(!web_pool_pixels(&d,-1));
    assert(!web_pool_pixels(&d,WEB_POOL_SLOTS));
    assert(!web_pool_pixels(&d,9999));

    /* Re-parsing a document releases its pool slots: navigation must not leak
     * the pool one page at a time. */
    web_html(&d,"<p>plain</p>");
    assert(web_pool_used(&d)==0);
    puts("web: image pool, cap lift, aliasing and exhaustion PASS");


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
    static struct web_doc idoc;
    web_html(&idoc,"<img src='/one.bmp'>");
    assert(idoc.nimages==1);
    assert(web_decode_image(&idoc,&idoc.images[0],bmp,sizeof bmp));
    assert(idoc.images[0].loaded==1);
    assert(web_pool_pixels(&idoc,idoc.images[0].slot)[0]==0x112233);
    assert(!web_decode_image(&idoc,&idoc.images[0],bmp,54));
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
