/* OutRun Web: bounded HTML reader, not a JavaScript/CSS browser.
 * All browser logic lives here. Mbed TLS is an unmodified maintained dependency.
 * Limits and security model are documented in WEB-INTEGRATION.md. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "bmp.h"
/* Parser loops intentionally keep their cursor update beside each guard. */
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif
#define WEB_URL 512
#define WEB_BODY 131072
#define WEB_ITEMS 2048
#define WEB_TABS 4
#define WEB_HIST 16
#define WEB_MARKS 8
struct web_url { char host[254], path[WEB_URL]; unsigned port; int tls; };
static int web_copy(char *d, size_t cap, const char *s) {
    size_t n = strlen(s); if (n >= cap) return 0;
    memmove(d, s, n + 1); return 1;
}
static int web_add(char *d, size_t cap, const char *s) {
    size_t n = strlen(d); return n < cap && web_copy(d+n, cap-n, s);
}
static int web_url_parse(const char *s, struct web_url *u) {
    memset(u, 0, sizeof *u);
    if (strlen(s) >= WEB_URL) return 0;
    for (const unsigned char *p=(const unsigned char *)s; *p; ++p)
        if (*p <= 32 || *p >= 127 || *p == '\\') return 0;
    if (!strncmp(s,"https://",8)) { u->tls=1; u->port=443; s+=8; }
    else if (!strncmp(s,"http://",7)) { u->port=80; s+=7; }
    else return 0;
    unsigned n=0;
    while (*s && *s!='/' && *s!=':' && *s!='?' && *s!='#') {
        char c=*s++;
        if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='.') || n>=253) return 0;
        u->host[n++]=(c>='A'&&c<='Z') ? c+32:c;
    }
    if (!n || u->host[0]=='.' || u->host[n-1]=='.') return 0;
    if (*s==':') {
        ++s; unsigned port=0, digits=0;
        while (*s>='0'&&*s<='9') { port=port*10+(*s++-'0'); if (++digits>5 || port>65535) return 0; }
        if (!port || (*s && *s!='/' && *s!='?' && *s!='#')) return 0;
        u->port=port;
    }
    n=0; if (*s!='/') u->path[n++]='/';
    while (*s && *s!='#') { if (n>=WEB_URL-1) return 0; u->path[n++]=*s++; }
    u->path[n]=0; return 1;
}
static void web_num(char *out, unsigned v) {
    char b[16]; int n=0,k=0; do { b[n++]=(char)('0'+v%10); v/=10; } while(v);
    while(n) out[k++]=b[--n]; out[k]=0;
}
static int web_resolve(const char *base, const char *ref, char *out) {
    struct web_url u, check; char origin[WEB_URL], path[WEB_URL], norm[WEB_URL];
    if (!web_url_parse(base,&u)) return 0;
    if (!strncmp(ref,"http://",7)||!strncmp(ref,"https://",8))
        return web_url_parse(ref,&check)&&web_copy(out,WEB_URL,ref);
    if (strchr(ref,':')) return 0;
    web_copy(origin,sizeof origin,u.tls?"https://":"http://");
    if (!strncmp(ref,"//",2)) {
        return web_add(origin,sizeof origin,ref+2)&&web_url_parse(origin,&check)&&web_copy(out,WEB_URL,origin);
    }
    web_add(origin,sizeof origin,u.host);
    if(u.port!=(u.tls?443u:80u)) { char port[16]; web_num(port,u.port); web_add(origin,sizeof origin,":"); web_add(origin,sizeof origin,port); }
    if(*ref=='/') { if(!web_copy(path,sizeof path,ref)) return 0; }
    else {
        web_copy(path,sizeof path,u.path); char *q=strchr(path,'?'); if(q)*q=0;
        if(*ref!='?'&&*ref!='#') { char *p=strrchr(path,'/'); if(p)p[1]=0; }
        if(!web_add(path,sizeof path,ref)) return 0;
    }
    /* Remove dot segments without interpreting percent-encoded delimiters. */
    size_t i=0,n=0; norm[n++]='/';
    while(path[i]=='/') ++i;
    while(path[i] && path[i]!='?' && path[i]!='#') {
        size_t b=i; while(path[i]&&path[i]!='/'&&path[i]!='?'&&path[i]!='#') ++i;
        size_t z=i-b;
        if(z==2&&path[b]=='.'&&path[b+1]=='.') { if(n>1) { --n; while(n>1&&norm[n-1]!='/') --n; } }
        else if(!(z==1&&path[b]=='.') && z) {
            if(n+z+1>=sizeof norm) return 0;
            memcpy(norm+n,path+b,z); n+=z; if(path[i]=='/') norm[n++]='/';
        }
        if(path[i]=='/') ++i;
    }
    norm[n]=0;
    if(path[i]&&!web_add(norm,sizeof norm,path+i)) return 0;
    if(!web_add(origin,sizeof origin,norm)||!web_url_parse(origin,&check)) return 0;
    return web_copy(out,WEB_URL,origin);
}
/* Text size classes. A bounded set rather than free pixel sizes: the glyph
 * renderer draws one 8x16 face, so a "size" scales line height and is a hint
 * the layout can honour, not an arbitrary font metric it would have to lie
 * about. Clamped at both ends -- a page asking for 9999px must not produce a
 * line height that overflows the layout arithmetic. */
enum { WEB_SIZE_MIN = 0, WEB_SIZE_SMALL = 0, WEB_SIZE_NORMAL = 1,
       WEB_SIZE_LARGE = 2, WEB_SIZE_XL = 3, WEB_SIZE_MAX = 3 };
#define WEB_BG_NONE 0xFFFFFFFFu     /* sentinel: no background-color set */
/* Item kinds. A rule is not an empty text run: the renderer has to be able to
 * tell "draw a horizontal line here" from "a word that happens to be blank",
 * and a flat display list of text-only items cannot express <hr> at all. */
enum { WEB_ITEM_TEXT = 0, WEB_ITEM_RULE = 1 };
struct web_item {
    short x,y,link,image;
    unsigned color, bg;
    unsigned char kind, heading, size;
    char text[72];
};
/* ---- Images: metadata in the document, pixels in a shared pool -----------
 * struct web_image used to embed `unsigned pixels[96*64]` -- 24 KB apiece.
 * With 8 per document that was 201 KB of a 414 KB web_doc, and web_doc is
 * instantiated per tab AND inside every snapshot, so ~983 KB of BSS was
 * reserved for image pixels whether or not any page had an image. The 8-image
 * cap was therefore not a policy about pages, it was a memory limit wearing a
 * feature's clothing.
 *
 * Pixels now live in ONE pool. The document holds metadata plus a slot index,
 * so the per-document image cap and the number of simultaneously DECODED
 * images become independent numbers: a page may reference many images while
 * only the ones actually fetched consume pixel storage.
 *
 * Pool lifetime is PER DOCUMENT and slots are owned by exactly one image.
 * web_html releases the previous document's slots, so navigation cannot leak
 * the pool one page at a time. A global cache would have to answer eviction,
 * cross-origin reuse and stale-after-navigation questions that nothing here
 * needs yet. */
#define WEB_IMAGES     48          /* image REFERENCES a document may hold   */
#define WEB_POOL_SLOTS 16          /* images DECODED into pixels at once     */
#define WEB_LINKS      256         /* link REFERENCES a document may hold    */
#define WEB_LINKBUF    16384       /* shared characters for all those hrefs  */
#define WEB_IMG_W 96
#define WEB_IMG_H 64
struct web_image {
    char src[WEB_URL], alt[72];
    int loaded;                    /* 0 pending, 1 decoded, -1 failed/refused */
    int slot;                      /* pool index, or -1 when nothing is held  */
};
struct web_doc {
    struct web_item items[WEB_ITEMS];
    /* Link hrefs pack into a shared arena. links[WEB_LINKS][WEB_URL] would be
     * 128 KB at 256 links, almost all of it padding: a real href is tens of
     * bytes, not 512. Offsets into a 16 KB arena decouple the link CAP from
     * the bytes links cost. */
    unsigned short linkat[WEB_LINKS];
    char linkbuf[WEB_LINKBUF];
    int linkused;
    struct web_image images[WEB_IMAGES];
    int count,nlinks,nimages,height,truncated;
    /* Which pool slots THIS document owns. Kept here rather than in the pool
     * so a document can release exactly its own slots without scanning. */
    unsigned char owns[WEB_POOL_SLOTS];
};
/* Always returns a readable C string. An out-of-range index yields "" rather
 * than indexing the arena, so a stale item->link can never produce a pointer
 * into unrelated bytes. */
static const char *web_link(const struct web_doc *d,int i) {
    static const char empty[] = "";
    if (i < 0 || i >= d->nlinks || d->linkat[i] >= WEB_LINKBUF) return empty;
    return d->linkbuf + d->linkat[i];
}
/* The pool itself is process-global and never appears in a snapshot: a
 * snapshot stores slot indices, and a restore re-points at live slots or
 * marks the image undecoded. Copying 1.5 MB of pixels into a snapshot is
 * exactly the cost this change exists to remove. */
static unsigned web_pool[WEB_POOL_SLOTS][WEB_IMG_W*WEB_IMG_H];
static unsigned char web_pool_taken[WEB_POOL_SLOTS];
static const unsigned *web_pool_pixels(const struct web_doc *d,int slot) {
    (void)d;
    if (slot < 0 || slot >= WEB_POOL_SLOTS || !web_pool_taken[slot]) return 0;
    return web_pool[slot];
}
static int web_pool_used(const struct web_doc *d) {
    int n=0;
    for (int i=0;i<WEB_POOL_SLOTS;i++) if (d->owns[i]) n++;
    return n;
}
static int web_pool_claim(struct web_doc *d) {
    for (int i=0;i<WEB_POOL_SLOTS;i++)
        if (!web_pool_taken[i]) { web_pool_taken[i]=1; d->owns[i]=1; return i; }
    return -1;                     /* exhausted: caller must fail closed */
}
static void web_pool_release(struct web_doc *d) {
    for (int i=0;i<WEB_POOL_SLOTS;i++)
        if (d->owns[i]) { web_pool_taken[i]=0; d->owns[i]=0; }
}
static int web_lower(int c) { return c>='A'&&c<='Z'?c+32:c; }
static int web_equal(const char *a,const char *b) {
    while(*a&&*b) if(web_lower(*a++)!=web_lower(*b++)) return 0;
    return !*a&&!*b;
}
static int web_space(int c) { return c==' '||c=='\t'||c=='\r'||c=='\n'||c=='\f'; }
static void web_attr(const char *tag,const char *key,char *out,size_t cap) {
    *out=0; const char *p=tag;
    while(*p&&!web_space(*p)) ++p;
    while(*p) {
        while(web_space(*p)||*p=='/') ++p;
        char name[32]; unsigned n=0;
        while(*p&&!web_space(*p)&&*p!='=') { if(n<31)name[n++]=(char)web_lower(*p); ++p; }
        name[n]=0; while(web_space(*p))++p;
        if(*p!='=') { if(!*p)break; continue; }
        ++p; while(web_space(*p))++p;
        char quote=(*p=='\''||*p=='"')?*p++:0; n=0;
        int match=!strcmp(name,key),overflow=0;
        while(*p&&(quote?*p!=quote:!web_space(*p))) {
            if(match) { if(n+1<cap) out[n++]=*p; else overflow=1; } ++p;
        }
        if(quote&&*p)++p;
        if(match) { out[overflow?0:n]=0; return; }
    }
}
/* ---- Element table + layout state ---------------------------------------
 * The old parser had one rule: a tag in a hardcoded list emitted a line break,
 * everything else did nothing. That cannot express <span> (inline, must NOT
 * break) or <hr> (block, and not text), and it silently treated every unknown
 * wrapper -- <section>, <main>, <nav>, which real pages are full of -- as a
 * no-op while treating <em> the same way. This table replaces that guess with
 * a declared display type per element, and the default for an UNKNOWN tag is
 * inline: a wrapper we do not recognise must be transparent to layout rather
 * than injecting a spurious break.
 *
 * There is no retained node tree. A tree would buy nothing here -- nothing
 * re-lays-out, mutates or queries the document after parse -- and would cost a
 * second bounded arena on top of the display list. What the parser keeps is
 * the ONE thing a flat loop could not: an explicit open-element STACK, so
 * nesting is tracked rather than flattened. That is what makes <a> inside
 * <span> inside <p> resolve, and what makes an inner block's close return to
 * the enclosing block instead of to the document. */
enum { WEB_DISP_INLINE = 0, WEB_DISP_BLOCK = 1, WEB_DISP_RULE = 2, WEB_DISP_SKIP = 3 };
struct web_element { const char *name; unsigned char display, heading; };
static const struct web_element web_elements[] = {
    { "script", WEB_DISP_SKIP,  0 }, { "style", WEB_DISP_SKIP,  0 },
    { "head",   WEB_DISP_SKIP,  0 }, { "title", WEB_DISP_SKIP,  0 },
    { "p",      WEB_DISP_BLOCK, 0 }, { "div",   WEB_DISP_BLOCK, 0 },
    { "br",     WEB_DISP_BLOCK, 0 }, { "li",    WEB_DISP_BLOCK, 0 },
    { "tr",     WEB_DISP_BLOCK, 0 }, { "ul",    WEB_DISP_BLOCK, 0 },
    { "ol",     WEB_DISP_BLOCK, 0 }, { "table", WEB_DISP_BLOCK, 0 },
    { "body",   WEB_DISP_BLOCK, 0 }, { "html",  WEB_DISP_BLOCK, 0 },
    { "section",WEB_DISP_BLOCK, 0 }, { "article",WEB_DISP_BLOCK,0 },
    { "header", WEB_DISP_BLOCK, 0 }, { "footer",WEB_DISP_BLOCK, 0 },
    { "nav",    WEB_DISP_BLOCK, 0 }, { "main",  WEB_DISP_BLOCK, 0 },
    { "blockquote", WEB_DISP_BLOCK, 0 }, { "pre", WEB_DISP_BLOCK, 0 },
    { "hr",     WEB_DISP_RULE,  0 },
    { "h1", WEB_DISP_BLOCK, 1 }, { "h2", WEB_DISP_BLOCK, 2 },
    { "h3", WEB_DISP_BLOCK, 3 }, { "h4", WEB_DISP_BLOCK, 4 },
    { "h5", WEB_DISP_BLOCK, 5 }, { "h6", WEB_DISP_BLOCK, 6 },
    /* Explicitly inline. Listed rather than left to the default so the intent
     * is visible: these are the ones whose breaking would be a visible bug. */
    { "span", WEB_DISP_INLINE, 0 }, { "a",  WEB_DISP_INLINE, 0 },
    { "em",   WEB_DISP_INLINE, 0 }, { "b",  WEB_DISP_INLINE, 0 },
    { "i",    WEB_DISP_INLINE, 0 }, { "strong", WEB_DISP_INLINE, 0 },
    { "code", WEB_DISP_INLINE, 0 }, { "small",  WEB_DISP_INLINE, 0 },
    { "label",WEB_DISP_INLINE, 0 }, { "img",    WEB_DISP_INLINE, 0 },
};
static const struct web_element *web_element_find(const char *name) {
    for (unsigned i=0;i<sizeof web_elements/sizeof *web_elements;i++)
        if (!strcmp(web_elements[i].name,name)) return &web_elements[i];
    return 0;   /* unknown: caller treats as inline, deliberately */
}
/* ---- Inline CSS: the three properties the brief asks for ------------------
 * A style ATTRIBUTE parser, not a stylesheet engine: no selectors, no
 * cascade, no specificity. Declaring that boundary matters, because "supports
 * CSS" would be a false claim -- what is supported is color, background-color
 * and font-size on a style="" attribute, inherited down the open-element
 * stack and restored on close, exactly as link and heading context already
 * are. Anything else in the attribute is ignored, not misread. */
static int web_hex1(int c) {
    c = web_lower(c);
    if (c>='0'&&c<='9') return c-'0';
    if (c>='a'&&c<='f') return c-'a'+10;
    return -1;
}
struct web_named_color { const char *name; unsigned rgb; };
static const struct web_named_color web_named_colors[] = {
    {"black",0x000000},{"white",0xffffff},{"red",0xff0000},{"lime",0x00ff00},
    {"green",0x008000},{"blue",0x0000ff},{"yellow",0xffff00},{"cyan",0x00ffff},
    {"aqua",0x00ffff},{"magenta",0xff00ff},{"fuchsia",0xff00ff},{"gray",0x808080},
    {"grey",0x808080},{"silver",0xc0c0c0},{"maroon",0x800000},{"olive",0x808000},
    {"navy",0x000080},{"teal",0x008080},{"purple",0x800080},{"orange",0xffa500},
};
/* Returns 1 and sets *out on success. An unrecognised or malformed value
 * leaves *out untouched so the caller keeps its inherited colour -- a page
 * asking for a colour we cannot parse must not get black-on-black. */
static int web_css_color(const char *v,unsigned *out) {
    while (web_space(*v)) ++v;
    if (*v=='#') {
        ++v; int n=0; while (web_hex1(v[n])>=0) ++n;
        if (n==3) {
            int r=web_hex1(v[0]),g=web_hex1(v[1]),b=web_hex1(v[2]);
            *out=(unsigned)((r*17)<<16 | (g*17)<<8 | (b*17)); return 1;
        }
        if (n>=6) {
            unsigned c=0;
            for (int i=0;i<6;i++) c=(c<<4)|(unsigned)web_hex1(v[i]);
            *out=c; return 1;
        }
        return 0;
    }
    if (!strncmp(v,"rgb(",4)) {
        v+=4; unsigned part[3]={0,0,0};
        for (int i=0;i<3;i++) {
            while (web_space(*v)) ++v;
            if (*v<'0'||*v>'9') return 0;
            unsigned n=0,digits=0;
            while (*v>='0'&&*v<='9') { n=n*10+(unsigned)(*v++-'0'); if(++digits>3) return 0; }
            if (n>255) n=255;
            part[i]=n;
            while (web_space(*v)) ++v;
            if (i<2) { if (*v!=',') return 0; ++v; }
        }
        while (web_space(*v)) ++v;
        if (*v!=')') return 0;
        *out=(part[0]<<16)|(part[1]<<8)|part[2]; return 1;
    }
    char name[24]; unsigned n=0;
    while (v[n] && !web_space(v[n]) && n<sizeof name-1) { name[n]=(char)web_lower(v[n]); ++n; }
    name[n]=0;
    for (unsigned i=0;i<sizeof web_named_colors/sizeof *web_named_colors;i++)
        if (!strcmp(web_named_colors[i].name,name)) { *out=web_named_colors[i].rgb; return 1; }
    return 0;
}
/* Map a CSS length onto one of the bounded size classes. The renderer has a
 * single 8x16 face, so a size is a line-height hint, not a font metric we
 * could honour precisely -- clamping is the honest behaviour rather than
 * pretending 9999px is representable. */
static int web_css_size(const char *v,unsigned char *out) {
    while (web_space(*v)) ++v;
    if (*v<'0'||*v>'9') return 0;
    unsigned px=0,digits=0;
    while (*v>='0'&&*v<='9') { px=px*10+(unsigned)(*v++-'0'); if(++digits>5) break; }
    unsigned char s = px<12 ? WEB_SIZE_SMALL : px<17 ? WEB_SIZE_NORMAL
                    : px<25 ? WEB_SIZE_LARGE : WEB_SIZE_XL;
    /* The ternary already yields a value in [WEB_SIZE_MIN, WEB_SIZE_MAX], so
     * there is no clamp here: an `s < WEB_SIZE_MIN` test on an unsigned char
     * against 0 is always false, which -Wtype-limits correctly rejects as a
     * check that cannot fire. The bound is enforced by construction. */
    *out=s; return 1;
}
/* Walk "prop: value; prop: value" and apply the three we honour. */
static void web_css_apply(const char *style,unsigned *color,unsigned *bg,unsigned char *size) {
    const char *p=style;
    while (*p) {
        while (web_space(*p)||*p==';') ++p;
        if (!*p) break;
        char prop[24]; unsigned n=0;
        while (*p && *p!=':' && *p!=';' && n<sizeof prop-1) {
            if (!web_space(*p)) prop[n++]=(char)web_lower(*p);
            ++p;
        }
        prop[n]=0;
        if (*p!=':') { while (*p && *p!=';') ++p; continue; }
        ++p;
        char val[64]; unsigned m=0;
        while (*p && *p!=';' && m<sizeof val-1) val[m++]=*p++;
        val[m]=0;
        if (!strcmp(prop,"color")) web_css_color(val,color);
        else if (!strcmp(prop,"background-color")) web_css_color(val,bg);
        else if (!strcmp(prop,"font-size")) web_css_size(val,size);
    }
}
/* Open-element stack. Bounded and non-recursive: a page with 4000 unclosed
 * divs must degrade, not smash the stack. Overflow keeps parsing at the
 * deepest tracked level rather than dropping content. */
#define WEB_DEPTH 32
struct web_layout {
    struct web_doc *d;
    int x, y;                 /* pen position                                */
    int line_h;               /* tallest thing on the current line           */
    int link;                 /* enclosing <a>, or -1                        */
    int heading;              /* enclosing <h1..6> level, or 0               */
    int skip;                 /* inside <script>/<style>/<head>              */
    unsigned color, bg;       /* inherited inline style                      */
    unsigned char size;
    int styled;               /* page set a colour explicitly on this run    */
    struct { char name[32]; unsigned char display, heading, size, styled; int link;
             unsigned color, bg; } open[WEB_DEPTH];
    int depth;
};
static void web_line_break(struct web_layout *L) {
    /* Only advance if the line has content: consecutive block tags such as
     * </div><div> must not stack up blank lines. <br> asks explicitly. */
    if (L->x > 8) { L->y += L->line_h; L->x = 8; L->line_h = 18; }
}
static void web_word(struct web_layout *L,const char *s) {
    int len=(int)strlen(s); if(!len) return;
    struct web_doc *d=L->d;
    int h = L->heading ? 22 : 18;
    /* Larger inline sizes get more line height. The face is fixed at 8x16, so
     * this is leading, not glyph scaling -- the honest effect of a size class
     * on a single-face renderer. */
    if (L->size == WEB_SIZE_LARGE && h < 22) h = 22;
    if (L->size == WEB_SIZE_XL) h = 26;
    if (L->x + len*8 > 552) { L->y += L->line_h; L->x = 8; L->line_h = h; }
    if (h > L->line_h) L->line_h = h;
    if (d->count >= WEB_ITEMS) { d->truncated = 1; return; }
    struct web_item *it=&d->items[d->count++];
    it->x=(short)L->x; it->y=(short)L->y; it->link=(short)L->link; it->image=-1;
    it->kind=WEB_ITEM_TEXT; it->heading=(unsigned char)L->heading;
    it->size=L->size; it->bg=L->bg;
    /* An explicit page colour wins; otherwise a link is link-coloured, a
     * heading heading-coloured, and body text takes the default. */
    it->color = L->styled ? L->color
              : L->link>=0 ? 0x7dd3fcu : L->heading ? 0x67e8f9u : 0xdce6efu;
    web_copy(it->text,sizeof it->text,s);
    L->x += len*8 + 8;
}
static void web_rule(struct web_layout *L) {
    struct web_doc *d=L->d;
    web_line_break(L);
    if (d->count >= WEB_ITEMS) { d->truncated = 1; return; }
    struct web_item *it=&d->items[d->count++];
    it->x=8; it->y=(short)L->y; it->link=-1; it->image=-1;
    it->kind=WEB_ITEM_RULE; it->heading=0; it->color=0x35506e;
    it->size=WEB_SIZE_NORMAL; it->bg=WEB_BG_NONE; it->text[0]=0;
    L->y += 12; L->x = 8; L->line_h = 18;
}
static void web_html(struct web_doc *d,const char *html) {
    /* Release the OUTGOING document's pool slots before zeroing it: memset
     * would erase the ownership map and strand those slots taken forever,
     * exhausting the pool after a handful of navigations. */
    web_pool_release(d);
    memset(d,0,sizeof *d);
    for (int i=0;i<WEB_IMAGES;i++) d->images[i].slot = -1;
    struct web_layout L;
    memset(&L,0,sizeof L);
    L.d=d; L.x=8; L.y=0; L.line_h=18; L.link=-1;
    L.color=0xdce6efu; L.bg=WEB_BG_NONE; L.size=WEB_SIZE_NORMAL; L.styled=0;
    const char *p=html; char word[72]; int n=0;
    while(*p) {
        if(*p=='<'||web_space(*p)) {
            word[n]=0; if(!L.skip) web_word(&L,word); n=0;
            if(web_space(*p)) { ++p; continue; }
            if(!strncmp(p,"<!--",4)) { const char *e=strstr(p+4,"-->"); if(!e)break; p=e+3; continue; }
            ++p; char tag[1024]; int t=0; char q=0;
            while(*p&&(*p!='>'||q)) {
                if(*p=='\''||*p=='"') { if(!q)q=*p; else if(q==*p)q=0; }
                if(t<1023)tag[t++]=*p; ++p;
            }
            if(*p)++p; tag[t]=0;
            char name[32]; int k=0; const char *a=tag; int close=*a=='/'; if(close)++a;
            while(*a&&!web_space(*a)&&*a!='/'&&k<31)name[k++]=(char)web_lower(*a++); name[k]=0;
            if(!name[0]) continue;
            /* Self-closing form <br/> -- the trailing slash is on the tag, not
             * the name, so it must be read off the raw text. */
            int selfclose = t>0 && tag[t-1]=='/';
            const struct web_element *el = web_element_find(name);
            unsigned char disp = el ? el->display : WEB_DISP_INLINE;

            if (disp == WEB_DISP_SKIP) { L.skip = close ? 0 : 1; continue; }
            if (L.skip) continue;

            if (disp == WEB_DISP_RULE) { if(!close) web_rule(&L); continue; }

            if (close) {
                /* Unwind to the MATCHING open element, not just one level:
                 * unbalanced markup is the norm, and popping blindly would
                 * leave the stack describing elements that already closed. */
                int at=-1;
                for (int i=L.depth-1;i>=0;i--) if (!strcmp(L.open[i].name,name)) { at=i; break; }
                if (at>=0) {
                    int was_block = 0;
                    for (int i=L.depth-1;i>=at;i--) if (L.open[i].display==WEB_DISP_BLOCK) was_block=1;
                    L.depth = at;
                    /* Restore the enclosing context from what remains open. */
                    L.link = -1; L.heading = 0;
                    L.color = 0xdce6efu; L.bg = WEB_BG_NONE;
                    L.size = WEB_SIZE_NORMAL; L.styled = 0;
                    for (int i=0;i<L.depth;i++) {
                        if (L.open[i].link >= 0) L.link = L.open[i].link;
                        if (L.open[i].heading)   L.heading = L.open[i].heading;
                        if (L.open[i].styled) { L.color = L.open[i].color; L.styled = 1; }
                        if (L.open[i].bg != WEB_BG_NONE) L.bg = L.open[i].bg;
                        if (L.open[i].size != WEB_SIZE_NORMAL) L.size = L.open[i].size;
                    }
                    if (was_block) web_line_break(&L);
                } else if (disp == WEB_DISP_BLOCK) web_line_break(&L);
                continue;
            }

            if (disp == WEB_DISP_BLOCK) {
                /* <br> is an explicit break even on an empty line. */
                if (!strcmp(name,"br")) { L.y += L.line_h; L.x = 8; L.line_h = 18; }
                else web_line_break(&L);
            }

            int link_here = -1;
            if (!strcmp(name,"a") && d->nlinks < WEB_LINKS) {
                /* Parse the href into a scratch buffer, then pack it into the
                 * arena. A link that does not fit is DROPPED -- the anchor
                 * degrades to plain text rather than pointing at a truncated
                 * or arbitrary URL, and the document keeps parsing. */
                char href[WEB_URL];
                web_attr(tag,"href",href,sizeof href);
                size_t hn = strlen(href);
                if (hn && d->linkused + (int)hn + 1 <= WEB_LINKBUF) {
                    d->linkat[d->nlinks] = (unsigned short)d->linkused;
                    memcpy(d->linkbuf + d->linkused, href, hn + 1);
                    d->linkused += (int)hn + 1;
                    link_here = d->nlinks++; L.link = link_here;
                }
            }
            if (!strcmp(name,"img") && d->nimages < WEB_IMAGES && d->count < WEB_ITEMS) {
                int z=d->nimages; struct web_image *im=&d->images[z];
                web_attr(tag,"src",im->src,sizeof im->src); web_attr(tag,"alt",im->alt,sizeof im->alt);
                if (im->src[0]) {
                    if (L.x != 8) web_line_break(&L);
                    struct web_item *it=&d->items[d->count++];
                    it->x=8; it->y=(short)L.y; it->link=(short)L.link; it->image=(short)z;
                    it->kind=WEB_ITEM_TEXT; it->heading=0; it->color=0xdce6efu;
                    it->text[0]=0;
                    ++d->nimages; L.y += 84; L.x = 8; L.line_h = 18;
                }
            }
            if (el && el->heading) L.heading = el->heading;
            /* style="" on this element, inherited by everything inside it. */
            {
                char style[256];
                web_attr(tag,"style",style,sizeof style);
                if (style[0]) {
                    unsigned c=L.color, b=L.bg; unsigned char z=L.size;
                    int had=L.styled;
                    web_css_apply(style,&c,&b,&z);
                    if (c!=L.color) { L.color=c; L.styled=1; }
                    else L.styled=had;
                    L.bg=b; L.size=z;
                }
            }

            /* Void and self-closed elements never go on the stack: pushing
             * <br> or <img> would leave them open forever and every later
             * close would unwind through them. */
            if (!selfclose && strcmp(name,"br") && strcmp(name,"img") && L.depth < WEB_DEPTH) {
                int i=L.depth++;
                web_copy(L.open[i].name,sizeof L.open[i].name,name);
                L.open[i].display=disp;
                L.open[i].heading=el?el->heading:0;
                L.open[i].link=link_here;
                L.open[i].color=L.color; L.open[i].bg=L.bg;
                L.open[i].size=L.size; L.open[i].styled=(unsigned char)L.styled;
            }
            continue;
        }
        char c=*p++;
        if(c=='&') {
            static const char *entity[]={"amp;","lt;","gt;","quot;","apos;","nbsp;"};
            static const char value[]="&<>\"' ";
            for(unsigned j=0;j<6;j++) if(!strncmp(p,entity[j],strlen(entity[j]))) { c=value[j]; p+=strlen(entity[j]); break; }
        }
        if((unsigned char)c<32||(unsigned char)c>=127)c='?';
        if(!L.skip) { word[n++]=c; if(n==68) { word[n]=0; web_word(&L,word); n=0; } }
    }
    word[n]=0; if(!L.skip) web_word(&L,word);
    d->height = L.y + L.line_h + 2;
}
struct web_response { size_t body,length; int status; char location[WEB_URL],type[80]; };
/* Returns incomplete=0, complete=1, malformed/unsupported=-1. Only compacts
 * chunked bodies after the ENTIRE message validates, so partial reads are safe. */
static int web_response(char *b,size_t n,int eof,struct web_response *r) {
    memset(r,0,sizeof *r); size_t end=0;
    for(size_t i=0;i+3<n;i++) if(!memcmp(b+i,"\r\n\r\n",4)) { end=i+4; break; }
    if(!end)return eof||n>8192?-1:0;
    if(end>8192||n<12||strncmp(b,"HTTP/1.",7)||(b[7]!='0'&&b[7]!='1')||b[8]!=' ')return -1;
    for(int i=9;i<12;i++) { if(b[i]<'0'||b[i]>'9')return -1; r->status=r->status*10+b[i]-'0'; }
    size_t pos=0,len=0; int haslen=0,chunk=0;
    while(pos+1<end&&memcmp(b+pos,"\r\n",2))++pos; pos+=2;
    while(pos+2<end) {
        size_t stop=pos; while(stop+1<end&&memcmp(b+stop,"\r\n",2))++stop;
        size_t colon=pos; while(colon<stop&&b[colon]!=':')++colon;
        if(colon==stop)return -1;
        char key[64],v[WEB_URL]; size_t kn=colon-pos,start=colon+1;
        while(start<stop&&web_space(b[start]))++start;
        size_t vend=stop; while(vend>start&&web_space(b[vend-1]))--vend;
        if(kn>=sizeof key||vend-start>=sizeof v)return -1;
        memcpy(key,b+pos,kn); key[kn]=0; memcpy(v,b+start,vend-start); v[vend-start]=0;
        if(web_equal(key,"Content-Length")) {
            if(haslen||!*v)return -1; haslen=1;
            for(size_t j=0;v[j];j++) { if(v[j]<'0'||v[j]>'9')return -1; len=len*10+v[j]-'0'; if(len>=WEB_BODY)return -1; }
        } else if(web_equal(key,"Transfer-Encoding")) { if(chunk||!web_equal(v,"chunked"))return -1; chunk=1; }
        else if(web_equal(key,"Content-Encoding")&&!web_equal(v,"identity"))return -1;
        else if(web_equal(key,"Location"))web_copy(r->location,sizeof r->location,v);
        else if(web_equal(key,"Content-Type")) { char *semi=strchr(v,';'); if(semi)*semi=0; if(!web_copy(r->type,sizeof r->type,v))return -1; }
        pos=stop+2;
    }
    r->body=end;
    if(chunk&&haslen)return -1;
    if(chunk) {
        for(int pass=0;pass<2;pass++) {
            pos=end; size_t total=0;
            for(;;) {
                size_t z=0,digits=0;
                while(pos<n&&b[pos]!='\r'&&b[pos]!=';') {
                    int c=web_lower(b[pos++]),v=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;
                    if(v<0||++digits>6)return -1; z=z*16+(unsigned)v; if(z>=WEB_BODY)return -1;
                }
                if(!digits)return eof?-1:0;
                if(pos<n&&b[pos]==';') { while(pos<n&&b[pos]!='\r')++pos; }
                if(pos+2>n)return eof?-1:0;
                if(memcmp(b+pos,"\r\n",2))return -1; pos+=2;
                if(!z) {
                    /* Trailers are bounded, ignored, and never trusted as headers. */
                    size_t trailer=pos;
                    for(;;) {
                        if(pos+2>n)return eof?-1:0;
                        if(!memcmp(b+pos,"\r\n",2)) { pos+=2; break; }
                        while(pos+1<n&&memcmp(b+pos,"\r\n",2))++pos;
                        if(pos+2>n)return eof?-1:0;
                        pos+=2; if(pos-trailer>8192)return -1;
                    }
                    r->length=total; break;
                }
                if(z>n-pos||n-pos-z<2)return eof?-1:0;
                if(memcmp(b+pos+z,"\r\n",2)||total+z>=WEB_BODY)return -1;
                if(pass)memmove(b+end+total,b+pos,z);
                total+=z; pos+=z+2;
            }
        }
        return 1;
    }
    if(haslen) { if(n-end<len)return eof?-1:0; r->length=len; return 1; }
    if(r->status==204||r->status==304) { r->length=0; return 1; }
    if(!eof)return 0; r->length=n-end; return 1;
}
static unsigned web_be16(const unsigned char *p) { return ((unsigned)p[0]<<8)|p[1]; }
static int web_dns_query(const char *host,unsigned id,unsigned char *p) {
    memset(p,0,512); p[0]=(unsigned char)(id>>8); p[1]=(unsigned char)id; p[2]=1; p[5]=1;
    int n=12; const char *s=host;
    while(*s) { const char *e=s; while(*e&&*e!='.')++e; int z=(int)(e-s);
        if(z<1||z>63||n+z+6>512)return -1;
        p[n++]=(unsigned char)z; memcpy(p+n,s,(size_t)z); n+=z; s=*e?e+1:e;
    }
    p[n++]=0; p[n++]=0; p[n++]=1; p[n++]=0; p[n++]=1; return n;
}
/* Compression pointers have a hop bound and expanded-name bound. */
static int web_dns_name(const unsigned char *p,size_t n,size_t *at,char *out) {
    size_t pos=*at,w=0,ret=0; unsigned hops=0;
    while(pos<n&&++hops<=128) {
        unsigned z=p[pos++];
        if(!z) { out[w]=0; *at=ret?ret:pos; return 1; }
        if((z&192)==192) { if(pos>=n)return 0; size_t target=((z&63)<<8)|p[pos++]; if(!ret)ret=pos; if(target>=n)return 0; pos=target; continue; }
        if(z>63||pos+z>n||w+z+1>=254)return 0;
        if(w)out[w++]='.';
        while(z--)out[w++]=(char)web_lower(p[pos++]);
    }
    return 0;
}
static int web_dns_answer(const unsigned char *p,size_t n,unsigned id,const char *host,unsigned *ip) {
    if(n<12)return -1;
    if(web_be16(p)!=id || !(p[2]&128))return 0;
    if((p[2]&0x7a)||(p[3]&15)||web_be16(p+4)!=1)return -1;
    size_t at=12; char name[254],target[254];
    if(!web_dns_name(p,n,&at,name)||at+4>n)return -1;
    if(!web_equal(name,host)||web_be16(p+at)!=1||web_be16(p+at+2)!=1)return 0;
    at+=4; web_copy(target,sizeof target,host); unsigned count=web_be16(p+6);
    if(count>64)return -1;
    /* Follow an ordered CNAME chain only; unrelated A records are not answers. */
    for(unsigned j=0;j<count;j++) {
        if(!web_dns_name(p,n,&at,name)||at+10>n)return -1;
        unsigned type=web_be16(p+at),cls=web_be16(p+at+2),len=web_be16(p+at+8); at+=10;
        if(at+len>n)return -1;
        if(cls==1&&web_equal(name,target)) {
            if(type==1&&len==4) { *ip=((unsigned)p[at]<<24)|((unsigned)p[at+1]<<16)|((unsigned)p[at+2]<<8)|p[at+3]; return 1; }
            if(type==5) { size_t q=at; if(!web_dns_name(p,n,&q,target)||q!=at+len)return -1; }
        }
        at+=len;
    }
    return -1;
}

/* Every image is decoded from received bytes. Unsupported formats remain alt
 * text, never a decorative stand-in claimed as a downloaded image.
 *
 * The pool slot is claimed HERE, on a successful parse, not at <img> parse
 * time: a page may reference WEB_IMAGES images while only the ones actually
 * fetched hold pixels. Exhaustion marks the image failed (-1) rather than
 * pending (0), because a pending image is one the fetch loop will keep
 * retrying forever. */
static int web_decode_image(struct web_doc *d,struct web_image *im,const unsigned char *b,size_t n) {
    int w,h; unsigned off;
    if(bmp_parse(b,n,&w,&h,&off)<0) { im->loaded=-1; return 0; }
    if(im->slot < 0) {
        im->slot = web_pool_claim(d);
        if(im->slot < 0) { im->loaded=-1; return 0; }   /* pool full: fail closed */
    }
    unsigned *px = web_pool[im->slot];
    for(int y=0;y<WEB_IMG_H;y++) for(int x=0;x<WEB_IMG_W;x++)
        px[y*WEB_IMG_W+x]=bmp_pixel(b,off,w,h,x*w/WEB_IMG_W,y*h/WEB_IMG_H);
    im->loaded=1; return 1;
}
struct web_tab {
    char url[WEB_URL],history[WEB_HIST][WEB_URL],status[96];
    struct web_doc doc;
    int used,hcount,hpos,scroll,pending,image_next,secure;
};
static int web_history_push(struct web_tab *t,const char *url) {
    struct web_url check; if(!web_url_parse(url,&check))return 0;
    if(t->hcount && !strcmp(t->history[t->hpos],url))return 1;
    t->hcount=t->hcount?t->hpos+1:0;
    if(t->hcount==WEB_HIST) {
        memmove(t->history,t->history+1,sizeof t->history[0]*(WEB_HIST-1)); --t->hcount;
    }
    t->hpos=t->hcount++; web_copy(t->history[t->hpos],WEB_URL,url); return 1;
}

#ifndef WEB_CORE_TEST
#include <time.h>
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/memory_buffer_alloc.h"
#include "mbedtls/platform.h"
#include "mbedtls/platform_util.h"
#ifdef WEB_HOST_NETWORK
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/random.h>
#include <arpa/inet.h>
/* The UI layer hit-tests with app_hit() and measures with app_text_width(), so
 * the host UI test needs the widget declarations and the event ABI even though
 * it has no framebuffer. gui.h honours APP_HOST_TEST by leaving sysc() extern,
 * which the test supplies. The CA roots stay out: main() is compiled away under
 * WEB_UI_TEST, so an included-but-unused 200 KB array would trip
 * -Wunused-const-variable under -Werror. */
#ifdef WEB_UI_TEST
#include "gui.h"
#endif
#else
#include "gui.h"
#include "web_ca_roots.h"
#endif

/* One live transfer bounds TLS memory and sockets. Tabs queue independently;
 * every transition returns to the event loop, including DNS and handshakes. */
enum { WEB_IDLE,WEB_DNS_SEND,WEB_DNS_WAIT,WEB_CONNECT,WEB_TLS,
       WEB_SEND,WEB_READ,WEB_DONE,WEB_ERROR };
struct web_job {
    int fd,state,tls_live,verified,redirects;
    unsigned ip,dns_id; uint64_t deadline;
    struct web_url url;
    char address[WEB_URL],request[1400],buffer[WEB_BODY+8192+1],error[96];
    size_t sent,received; unsigned char dns[512]; int dns_len;
    struct web_response response;
    mbedtls_ssl_context ssl;
};
static mbedtls_x509_crt web_roots;
static mbedtls_ssl_config web_tls_config;
static mbedtls_ctr_drbg_context web_rng;
/* 8-byte aligned deliberately. mbedtls_memory_buffer_alloc_init carves every
 * TLS allocation out of this array, so the array's own alignment becomes the
 * alignment of every struct inside it -- and an unsigned char[] only promises
 * 1. UBSan caught real misaligned mbedtls_x509_crt accesses through this heap
 * during certificate chain parsing. x86-64 tolerates unaligned scalar loads, so
 * the guest never faulted and the defect was invisible until the host test ran
 * with -fsanitize=undefined; on a stricter target it is a crash, and here it
 * silently costs split loads on every certificate field. */
static unsigned char web_tls_heap[3*1024*1024] __attribute__((aligned(8)));
static int web_tls_ready,web_have_roots;
static uint64_t web_clock(int realtime) {
#ifdef WEB_HOST_NETWORK
    struct timespec t; if(clock_gettime(realtime?CLOCK_REALTIME:CLOCK_MONOTONIC,&t))return 0;
    return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;
#else
    uint64_t t[2]; if((int64_t)sysc(SYS_CLOCK_GETTIME,realtime?0:1,(u64)t,0)<0)return 0;
    return t[0]*1000+t[1]/1000000;
#endif
}
static mbedtls_time_t web_time(mbedtls_time_t *out) {
    mbedtls_time_t t=(mbedtls_time_t)(web_clock(1)/1000); if(out)*out=t; return t;
}
/* Gregorian UTC conversion, independent of host timezone and libc. */
struct tm *mbedtls_platform_gmtime_r(const mbedtls_time_t *timer,struct tm *out) {
    if(!timer||!out||*timer<0)return 0;
    uint64_t seconds=(uint64_t)*timer,days=seconds/86400;
    memset(out,0,sizeof *out);
    out->tm_sec=(int)(seconds%60); out->tm_min=(int)(seconds/60%60); out->tm_hour=(int)(seconds/3600%24);
    out->tm_wday=(int)((days+4)%7);
    int year=1970; while(year<10000) {
        int leap=year%4==0&&(year%100!=0||year%400==0),span=365+leap;
        if(days<(unsigned)span)break; days-=(unsigned)span; ++year;
    }
    if(year==10000)return 0;
    out->tm_year=year-1900; out->tm_yday=(int)days;
    static const int md[]={31,28,31,30,31,30,31,31,30,31,30,31};
    int mon=0; while(mon<11) {
        int span=md[mon]+(mon==1&&year%4==0&&(year%100!=0||year%400==0));
        if(days<(unsigned)span)break; days-=(unsigned)span; ++mon;
    }
    out->tm_mon=mon; out->tm_mday=(int)days+1; return out;
}
static int web_entropy(void *ctx,unsigned char *out,size_t n) {
    (void)ctx;
#ifdef WEB_HOST_NETWORK
    while(n) { ssize_t r=getrandom(out,n,0); if(r<0&&errno==EINTR)continue; if(r<=0)return -1; out+=r; n-=(size_t)r; }
#else
    unsigned a=1,b,c,d;
    __asm__ volatile("cpuid":"+a"(a),"=b"(b),"=c"(c),"=d"(d));
    if(!(c&(1u<<30)))return -1;
    while(n) {
        unsigned long long word=0; unsigned char ok=0;
        for(int attempt=0;attempt<10&&!ok;attempt++)
            __asm__ volatile("rdrand %0; setc %1":"=r"(word),"=qm"(ok)::"cc");
        if(!ok)return -1;
        size_t z=n<8?n:8; memcpy(out,&word,z); out+=z; n-=z;
    }
#endif
    return 0;
}
static int web_socket(int udp) {
#ifdef WEB_HOST_NETWORK
    int r=socket(AF_INET,(udp?SOCK_DGRAM:SOCK_STREAM)|SOCK_NONBLOCK,0); return r<0?-errno:r;
#else
    return (int)(int64_t)sysc(SYS_SOCKET,2,(udp?2:1)|0x800,0);
#endif
}
static void web_fd_close(int fd) {
    if(fd<0)return;
#ifdef WEB_HOST_NETWORK
    close(fd);
#else
    sysc(SYS_CLOSE,(u64)fd,0,0);
#endif
}
static int web_connect(int fd,unsigned ip,unsigned port) {
#ifdef WEB_HOST_NETWORK
    struct sockaddr_in a={0}; a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(ip); a.sin_port=htons((uint16_t)port);
    int r=connect(fd,(struct sockaddr *)&a,sizeof a); return r<0?-errno:r;
#else
    return (int)(int64_t)sysc(SYS_CONNECT,(u64)fd,ip,port);
#endif
}
static int web_send(int fd,const unsigned char *p,size_t n) {
    if(n>1400)n=1400; /* kernel stage and TCP MSS; short writes are retained */
#ifdef WEB_HOST_NETWORK
    int r=(int)send(fd,p,n,MSG_NOSIGNAL); return r<0?-errno:r;
#else
    return (int)(int64_t)sysc(SYS_SEND,(u64)fd,(u64)p,n);
#endif
}
static int web_recv(int fd,unsigned char *p,size_t n) {
    if(n>1400)n=1400;
#ifdef WEB_HOST_NETWORK
    int r=(int)recv(fd,p,n,0); return r<0?-errno:r;
#else
    return (int)(int64_t)sysc(SYS_RECV,(u64)fd,(u64)p,n);
#endif
}
static int web_tls_send(void *ctx,const unsigned char *p,size_t n) {
    struct web_job *j=ctx; int r=web_send(j->fd,p,n);
    return r==-11||r==-107||r==-4?MBEDTLS_ERR_SSL_WANT_WRITE:r<0?MBEDTLS_ERR_SSL_INTERNAL_ERROR:r;
}
static int web_tls_recv(void *ctx,unsigned char *p,size_t n) {
    struct web_job *j=ctx; int r=web_recv(j->fd,p,n);
    return r==-11||r==-4?MBEDTLS_ERR_SSL_WANT_READ:r<0?MBEDTLS_ERR_SSL_INTERNAL_ERROR:r;
}
static int web_runtime_init(const unsigned char *roots,size_t len) {
    mbedtls_memory_buffer_alloc_init(web_tls_heap,sizeof web_tls_heap);
    mbedtls_x509_crt_init(&web_roots); mbedtls_ssl_config_init(&web_tls_config); mbedtls_ctr_drbg_init(&web_rng);
    mbedtls_platform_set_time(web_time);
    /* mbedtls_x509_crt_parse returns 0 when EVERY certificate parsed, a NEGATIVE
     * error when it could not use the input at all, and a POSITIVE count of how
     * many individual certificates it skipped while parsing the rest. Demanding
     * ==0 therefore rejects a whole trust store because of one certificate this
     * build's trimmed algorithm set cannot represent -- which is exactly what
     * the real 189 KB cacert.pem does, leaving web_have_roots at 0 and failing
     * every HTTPS request closed as if the network were down.
     *
     * Accept a partial parse, but only if it actually yielded a chain: rc<0 or
     * an empty list still fails, so an empty or garbage store can never be read
     * as success. That distinction is the whole point -- verifying against zero
     * roots would accept anything. */
    int rc=mbedtls_x509_crt_parse(&web_roots,roots,len);
    web_have_roots = rc>=0 && web_roots.version!=0;
    return web_have_roots;
}
static int web_tls_init(void) {
    if(!web_have_roots||web_clock(1)<1704067200000ull)return 0;
    if(web_tls_ready)return 1;
    const unsigned char personal[]="OutRun Web TLS";
    if(mbedtls_ctr_drbg_seed(&web_rng,web_entropy,0,personal,sizeof personal))return 0;
    if(mbedtls_ssl_config_defaults(&web_tls_config,MBEDTLS_SSL_IS_CLIENT,MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT))return 0;
    mbedtls_ssl_conf_authmode(&web_tls_config,MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&web_tls_config,&web_roots,0);
    mbedtls_ssl_conf_rng(&web_tls_config,mbedtls_ctr_drbg_random,&web_rng);
    web_tls_ready=1; return 1;
}
static void web_close(struct web_job *j) {
    web_fd_close(j->fd); j->fd=-1;
    if(j->tls_live) { mbedtls_ssl_free(&j->ssl); j->tls_live=0; }
}
static void web_fail(struct web_job *j,const char *error) {
    web_copy(j->error,sizeof j->error,error); web_close(j); j->state=WEB_ERROR;
}
static int web_ipv4(const char *s,unsigned *ip) {
    unsigned value=0;
    for(int part=0;part<4;part++) {
        unsigned v=0,k=0; while(*s>='0'&&*s<='9') { v=v*10+*s++-'0'; if(++k>3||v>255)return 0; }
        if(!k)return 0; value=(value<<8)|v;
        if(part<3) { if(*s++!='.')return 0; } else if(*s)return 0;
    }
    *ip=value; return 1;
}
static int web_tcp_start(struct web_job *j) {
    web_fd_close(j->fd); j->fd=web_socket(0);
    if(j->fd<0) { web_fail(j,"TCP socket unavailable (network capability?)"); return 0; }
    int r=web_connect(j->fd,j->ip,j->url.port);
    if(r && r!=-115 && r!=-11) { web_fail(j,"TCP connection refused"); return 0; }
    j->state=WEB_CONNECT; return 1;
}
static int web_start(struct web_job *j,const char *address) {
    /* address may alias j->address during redirect. */
    char saved[WEB_URL]; if(!web_copy(saved,sizeof saved,address))return 0;
    web_close(j); memset(j,0,sizeof *j); j->fd=-1;
    if(!web_url_parse(saved,&j->url)) { web_fail(j,"Only valid http:// and https:// URLs are supported"); return 0; }
    web_copy(j->address,sizeof j->address,saved);
    j->deadline=web_clock(0)+30000;
    web_copy(j->request,sizeof j->request,"GET "); web_add(j->request,sizeof j->request,j->url.path);
    web_add(j->request,sizeof j->request," HTTP/1.1\r\nHost: "); web_add(j->request,sizeof j->request,j->url.host);
    if(j->url.port!=(j->url.tls?443u:80u)) { char port[16]; web_num(port,j->url.port); web_add(j->request,sizeof j->request,":"); web_add(j->request,sizeof j->request,port); }
    web_add(j->request,sizeof j->request,"\r\nUser-Agent: OutRun-Web/1\r\nAccept: text/html,text/plain,image/bmp\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n");
    if(j->url.tls&&!web_tls_init()) { web_fail(j,"HTTPS unavailable: CA roots, trusted time or entropy missing"); return 0; }
    if(web_ipv4(j->url.host,&j->ip))return web_tcp_start(j);
    unsigned char nonce[2];
    if(web_entropy(0,nonce,sizeof nonce)) { web_fail(j,"DNS unavailable: secure entropy missing"); return 0; }
    j->dns_id=((unsigned)nonce[0]<<8)|nonce[1]; j->dns_len=web_dns_query(j->url.host,j->dns_id,j->dns);
    if(j->dns_len<0) { web_fail(j,"Invalid DNS name"); return 0; }
    j->fd=web_socket(1);
    /* v1.2: ask the kernel what resolver this host was actually given. The old
     * hardcoded 0x0a000203 is QEMU SLIRP's built-in forwarder — it exists only
     * under `-netdev user` and answers nothing on a real bridge, which is why
     * every lookup on a bridged VM sat in WEB_DNS_WAIT until its deadline. It
     * survives ONLY as the last-resort fallback, so a SLIRP boot behaves as it
     * always did. */
    unsigned resolver=0x0a000203u;
#ifndef WEB_HOST_NETWORK
    {
        static struct outrun_net_info ni;
        if((i64)sysc(SYS_HW_INFO,HW_NET,(u64)&ni,sizeof ni)>=0 && ni.cfg_dns)
            resolver=ni.cfg_dns;
    }
#endif
#ifdef WEB_HOST_NETWORK
    /* Tests supply a genuine UDP resolver, not an in-memory DNS answer. */
    const char *dns=getenv("WEB_DNS_IP"); if(dns&&!web_ipv4(dns,&resolver)) { web_fail(j,"Invalid test resolver"); return 0; }
#endif
    unsigned port=53;
#ifdef WEB_HOST_NETWORK
    const char *dp=getenv("WEB_DNS_PORT"); if(dp)port=(unsigned)strtoul(dp,0,10);
#endif
    if(j->fd<0||web_connect(j->fd,resolver,port)) { web_fail(j,"DNS socket unavailable"); return 0; }
    j->state=WEB_DNS_SEND; return 1;
}
static int web_want(int r) { return r==MBEDTLS_ERR_SSL_WANT_READ||r==MBEDTLS_ERR_SSL_WANT_WRITE||r==-11||r==-4; }
static void web_step(struct web_job *j) {
    if(j->state==WEB_IDLE||j->state==WEB_DONE||j->state==WEB_ERROR)return;
    if(web_clock(0)>=j->deadline) { web_fail(j,"Network deadline exceeded (30 seconds)"); return; }
    int r;
    if(j->state==WEB_DNS_SEND) {
        r=web_send(j->fd,j->dns,(size_t)j->dns_len); if(r==-11||r==-4)return;
        if(r!=j->dns_len) { web_fail(j,"DNS send failed"); return; }
        j->state=WEB_DNS_WAIT; return;
    }
    if(j->state==WEB_DNS_WAIT) {
        r=web_recv(j->fd,j->dns,sizeof j->dns); if(r==-11||r==-4)return;
        if(r<=0) { web_fail(j,"DNS receive failed"); return; }
        r=web_dns_answer(j->dns,(size_t)r,j->dns_id,j->url.host,&j->ip);
        if(!r)return; if(r<0) { web_fail(j,"DNS response invalid or no IPv4 address"); return; }
        web_tcp_start(j); return;
    }
    if(j->state==WEB_CONNECT) {
        if(j->url.tls) {
            mbedtls_ssl_init(&j->ssl); j->tls_live=1;
            if(mbedtls_ssl_setup(&j->ssl,&web_tls_config)||mbedtls_ssl_set_hostname(&j->ssl,j->url.host)) { web_fail(j,"TLS setup failed"); return; }
            mbedtls_ssl_set_bio(&j->ssl,j,web_tls_send,web_tls_recv,0); j->state=WEB_TLS;
        } else j->state=WEB_SEND;
        return;
    }
    if(j->state==WEB_TLS) {
        r=mbedtls_ssl_handshake_step(&j->ssl);
        if(web_want(r))return;
        if(r) { web_fail(j,"TLS rejected: certificate, hostname, validity or protocol"); return; }
        if(mbedtls_ssl_is_handshake_over(&j->ssl)) {
            if(mbedtls_ssl_get_verify_result(&j->ssl)) { web_fail(j,"TLS certificate verification failed"); return; }
            j->verified=1; j->state=WEB_SEND;
        }
        return;
    }
    if(j->state==WEB_SEND) {
        size_t remain=strlen(j->request)-j->sent;
        r=j->url.tls?mbedtls_ssl_write(&j->ssl,(const unsigned char *)j->request+j->sent,remain):web_send(j->fd,(const unsigned char *)j->request+j->sent,remain);
        if(web_want(r)||(!j->url.tls&&r==-107&&!j->sent))return;
        if(r<=0) { web_fail(j,"HTTP request send failed"); return; }
        j->sent+=(size_t)r; if(j->sent==strlen(j->request))j->state=WEB_READ; return;
    }
    if(j->state==WEB_READ) {
        size_t cap=sizeof j->buffer-1-j->received;
        if(!cap) { web_fail(j,"Response exceeds 128 KiB body / 8 KiB headers"); return; }
        r=j->url.tls?mbedtls_ssl_read(&j->ssl,(unsigned char *)j->buffer+j->received,cap):web_recv(j->fd,(unsigned char *)j->buffer+j->received,cap);
        if(web_want(r))return;
        if(r==MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)r=0;
        if(r<0) { web_fail(j,"Response connection failed"); return; }
        j->received+=(size_t)r; j->buffer[j->received]=0;
        int complete=web_response(j->buffer,j->received,!r,&j->response);
        if(!complete)return;
        if(complete<0) { web_fail(j,"Malformed, truncated or unsupported HTTP response"); return; }
        j->buffer[j->response.body+j->response.length]=0;
        int status=j->response.status;
        if(status==301||status==302||status==303||status==307||status==308) {
            char dest[WEB_URL]; int hops=j->redirects+1; uint64_t deadline=j->deadline;
            if(hops>5||!j->response.location[0]||!web_resolve(j->address,j->response.location,dest)) { web_fail(j,"Invalid redirect or redirect limit exceeded"); return; }
            struct web_url next; web_url_parse(dest,&next);
            if(j->url.tls&&!next.tls) { web_fail(j,"Blocked insecure HTTPS-to-HTTP redirect"); return; }
            if(web_start(j,dest)) { j->redirects=hops; j->deadline=deadline; } return;
        }
        web_close(j); j->state=WEB_DONE;
    }
}

#if !defined(WEB_HOST_NETWORK) || defined(WEB_UI_TEST)
/* ===========================================================================
 * BROWSER UI — toolbar, tab strip, address bar, viewport, status bar
 * ===========================================================================
 * The engine above is transport and parsing only; everything below is the
 * chrome that drives it. Two rules hold this together, and both exist because
 * the window manager learned them the hard way (see the titlebar control
 * geometry in kernel64.c):
 *
 *   1. ONE geometry function per control. Painting and hit-testing call the
 *      same web_slot() / web_tab_slot(), so a button cannot drift away from the
 *      region that activates it. A second copy of a hit box is how a control
 *      ends up painted where nothing responds.
 *   2. Layout constants are derived, never repeated. WEB_VIEW_H is computed
 *      from the chrome heights, so moving a bar cannot silently leave the
 *      scroll clamp measuring a viewport that no longer exists.
 *
 * Scrolling, history and tab lifetime are testable without a framebuffer:
 * web_scroll/web_back/web_new_tab/web_close_tab touch state only, which is what
 * lets test_web_ui.c drive them on the host in milliseconds rather than in a
 * 600-second emulated boot. */
/* Window size is dictated by the WINDOW ABI, not by taste. The kernel clamps
 * SYS_WIN_CREATE to WIN_MAX_W x WIN_MAX_H (600x440) because WIN_SURF_MAXW/H fix
 * the per-window virtual-address stride: every window id gets exactly
 * WIN_SURF_MAXB bytes, so a larger request cannot be honoured without changing
 * the surface layout for every window in the system.
 *
 * This was found the hard way. The browser first asked for 800x600, got 600x440
 * silently, and then the desktop UI test clicked where an 800-wide window's
 * close box WOULD have been -- 200 pixels past the real one, onto bare desktop.
 * The window never closed, and the eight checks that followed failed in a
 * cascade that looked like a compositor fault rather than a wrong constant.
 * Asking for exactly what the ABI grants keeps the chrome geometry honest. */
#define WEB_W        600
#define WEB_H        440
#define WEB_TABBAR_H  24
#define WEB_TOOLBAR_H 28
#define WEB_STATUS_H  18
#define WEB_CHROME_H  (WEB_TABBAR_H + WEB_TOOLBAR_H)
#define WEB_VIEW_H    (WEB_H - WEB_CHROME_H - WEB_STATUS_H)
#define WEB_LINE      14

static struct web_tab web_tabs[WEB_TABS];
/* One scratch snapshot slot, saved with S and restored with R. version==0
 * means "nothing saved yet" -- web_snapshot_save stamps it. */
static struct web_snapshot web_saved_page;
static struct web_job web_current;
static char web_address[WEB_URL];
static char web_marks[WEB_MARKS][WEB_URL];
static int  web_active, web_editing, web_select_all, web_nmarks, web_menu;
static int  web_job_tab = -1;
static uint64_t web_started;

/* Toolbar buttons, left to right. One table drives label, width and action. */
enum { WEB_BTN_BACK, WEB_BTN_FWD, WEB_BTN_RELOAD, WEB_BTN_HOME, WEB_BTN_MARK, WEB_BTN_COUNT };
static const struct { short x, w; const char *label; } web_btn[WEB_BTN_COUNT] = {
    { 6, 30, "<" }, { 38, 30, ">" }, { 70, 30, "R" }, { 102, 34, "Hm" }, { 140, 30, "*" },
};
#define WEB_ADDR_X 176
#define WEB_ADDR_W (WEB_W - WEB_ADDR_X - 8)

static int web_slot(int index, int *x, int *y, int *w, int *h) {
    if (index < 0 || index >= WEB_BTN_COUNT) return 0;
    *x = web_btn[index].x; *y = WEB_TABBAR_H + 3;
    *w = web_btn[index].w; *h = WEB_TOOLBAR_H - 6;
    return 1;
}
/* Tab strip: equal slots, each with a close box at its right edge. */
static int web_tab_slot(int index, int *x, int *w) {
    if (index < 0 || index >= WEB_TABS) return 0;
    int span = (WEB_W - 30) / WEB_TABS;
    *x = 2 + index * span; *w = span - 2;
    return 1;
}
static int web_tab_close_hit(int index, int px, int py) {
    int x, w;
    if (py < 2 || py >= WEB_TABBAR_H - 2 || !web_tab_slot(index, &x, &w)) return 0;
    return px >= x + w - 16 && px < x + w - 2;
}

static void web_status(struct web_tab *t, const char *s) { web_copy(t->status, sizeof t->status, s); }

static void web_ui_init(void) {
    memset(web_tabs, 0, sizeof web_tabs);
    memset(&web_current, 0, sizeof web_current);
    web_current.fd = -1;
    web_tabs[0].used = 1; web_active = 0; web_job_tab = -1;
    web_editing = web_select_all = web_menu = web_nmarks = 0;
    web_address[0] = 0;
    web_status(&web_tabs[0], "Ready");
}

/* Scroll is clamped against the DOCUMENT, not the window: a short page cannot
 * scroll at all, and a long one stops with its last line visible rather than
 * running off into blank space. */
static void web_scroll(int to) {
    struct web_tab *t = &web_tabs[web_active];
    int max = t->doc.height - WEB_VIEW_H;
    if (max < 0) max = 0;
    t->scroll = to < 0 ? 0 : (to > max ? max : to);
}

static int web_navigate(int tab, const char *url, int push) {
    if (tab < 0 || tab >= WEB_TABS || !web_tabs[tab].used) return 0;
    struct web_tab *t = &web_tabs[tab];
    struct web_url check;
    if (!web_url_parse(url, &check)) { web_status(t, "Not a valid http:// or https:// URL"); return 0; }
    web_copy(t->url, sizeof t->url, url);
    if (push) web_history_push(t, url);
    t->scroll = 0; t->pending = 1; t->image_next = 0; t->secure = 0;
    web_status(t, "Connecting");
    return 1;
}

/* dir < 0 goes back, dir > 0 forward. Moving through history must NOT push a
 * new entry -- doing so would make Back and Forward append to the stack they
 * are walking, so the user could never leave the two most recent pages. */
static int web_back(int dir) {
    struct web_tab *t = &web_tabs[web_active];
    int to = t->hpos + (dir < 0 ? -1 : 1);
    if (!t->hcount || to < 0 || to >= t->hcount) return 0;
    t->hpos = to;
    return web_navigate(web_active, t->history[to], 0);
}

static int web_new_tab(void) {
    for (int i = 0; i < WEB_TABS; i++) {
        if (web_tabs[i].used) continue;
        memset(&web_tabs[i], 0, sizeof web_tabs[i]);
        web_tabs[i].used = 1; web_active = i;
        web_address[0] = 0; web_editing = 0;
        web_status(&web_tabs[i], "New tab");
        return 1;
    }
    return 0;
}

/* Closing the last remaining tab re-arms tab 0 empty rather than leaving the
 * browser with no surface to draw: an app whose window survives its own state
 * is how a compositor ends up reading a freed document. */
static void web_close_tab(void) {
    struct web_tab *t = &web_tabs[web_active];
    if (web_job_tab == web_active) { web_close(&web_current); web_current.state = WEB_IDLE; web_job_tab = -1; }
    /* Return this tab's pixel slots before the memset erases the ownership
     * map -- otherwise closing tabs leaks the pool a document at a time. */
    web_pool_release(&t->doc);
    memset(t, 0, sizeof *t);
    for (int i = 0; i < WEB_TABS; i++)
        if (web_tabs[i].used) { web_active = i; web_copy(web_address, sizeof web_address, web_tabs[i].url); return; }
    web_ui_init();
}

static void web_activate(int index) {
    if (index < 0 || index >= WEB_TABS || !web_tabs[index].used) return;
    web_active = index; web_editing = 0;
    web_copy(web_address, sizeof web_address, web_tabs[index].url);
}

static void web_bookmark(const char *url) {
    struct web_url check;
    if (!url[0] || !web_url_parse(url, &check)) return;
    for (int i = 0; i < web_nmarks; i++) if (!strcmp(web_marks[i], url)) return;
    if (web_nmarks == WEB_MARKS) {
        memmove(web_marks, web_marks + 1, sizeof web_marks[0] * (WEB_MARKS - 1));
        --web_nmarks;
    }
    web_copy(web_marks[web_nmarks++], WEB_URL, url);
}

/* Application-assisted snapshots, NOT process checkpoints. No DOM is retained:
 * the document is a bounded display list with decoded thumbnail pixels. Never
 * copy web_job, TLS contexts, fd values, pending requests or authentication.
 * Version/size are checked even for these process-local, non-persistent cards.
 * No private-tab isolation claim: all cards share this browser's address space. */
#define WEB_SNAPSHOT_VERSION 1u
struct web_snapshot {
    uint32_t version, bytes;
    char url[WEB_URL], history[WEB_HIST][WEB_URL], marks[WEB_MARKS][WEB_URL];
    struct web_doc doc;
    int hcount, hpos, nmarks, scroll;
};
static int web_snapshot_valid(const struct web_snapshot *s) {
    if (s->version != WEB_SNAPSHOT_VERSION || s->bytes != sizeof *s ||
        s->hcount < 0 || s->hcount > WEB_HIST || s->hpos < 0 ||
        (s->hcount ? s->hpos >= s->hcount : s->hpos != 0) ||
        s->nmarks < 0 || s->nmarks > WEB_MARKS || s->scroll < 0 ||
        s->doc.count < 0 || s->doc.count > WEB_ITEMS ||
        s->doc.nlinks < 0 || s->doc.nlinks > 64 ||
        s->doc.nimages < 0 || s->doc.nimages > WEB_IMAGES ||
        s->doc.height < 0 || s->doc.height > 65536 ||
        !memchr(s->url, 0, WEB_URL)) return 0;
    for (int i=0;i<s->hcount;i++) if (!memchr(s->history[i],0,WEB_URL)) return 0;
    for (int i=0;i<s->nmarks;i++) if (!memchr(s->marks[i],0,WEB_URL)) return 0;
    if (s->doc.linkused < 0 || s->doc.linkused > WEB_LINKBUF) return 0;
    for (int i=0;i<s->doc.nlinks;i++) {
        /* Every offset must land inside the used part of the arena AND its
         * string must terminate before the arena ends, or a restored snapshot
         * hands the navigator a pointer that runs off the buffer. */
        if (s->doc.linkat[i] >= s->doc.linkused) return 0;
        if (!memchr(s->doc.linkbuf + s->doc.linkat[i], 0,
                    (size_t)(WEB_LINKBUF - s->doc.linkat[i]))) return 0;
    }
    for (int i=0;i<s->doc.nimages;i++)
        if (!memchr(s->doc.images[i].src,0,WEB_URL) || !memchr(s->doc.images[i].alt,0,72)) return 0;
    for (int i=0;i<s->doc.count;i++) {
        const struct web_item *it=&s->doc.items[i];
        if (!memchr(it->text,0,sizeof it->text) || it->link < -1 ||
            it->link >= s->doc.nlinks || it->image < -1 || it->image >= s->doc.nimages) return 0;
        /* New in slice A: both are switched on by the renderer, so a restored
         * snapshot carrying an out-of-range value would select a branch that
         * does not exist. Validate them like every other restored field. */
        if (it->kind > WEB_ITEM_RULE || it->heading > 6) return 0;
    }
    return 1;
}
static int web_snapshot_save(struct web_snapshot *s) {
    const struct web_tab *t=&web_tabs[web_active];
    memset(s,0,sizeof *s); s->version=WEB_SNAPSHOT_VERSION; s->bytes=sizeof *s;
    memcpy(s->url,t->url,sizeof s->url); memcpy(s->history,t->history,sizeof s->history);
    memcpy(s->marks,web_marks,sizeof s->marks); s->doc=t->doc;
    /* A snapshot owns NO pool slots. Copying owns[] would make two documents
     * claim the same pixels, and whichever released first would free them out
     * from under the other. The snapshot keeps image METADATA and records the
     * images as undecoded; a restored tab shows placeholders until the user
     * reloads, which is the same honesty as its offline status line. */
    memset(s->doc.owns,0,sizeof s->doc.owns);
    for (int i=0;i<s->doc.nimages;i++) { s->doc.images[i].slot=-1; s->doc.images[i].loaded=0; }
    s->hcount=t->hcount; s->hpos=t->hpos; s->nmarks=web_nmarks; s->scroll=t->scroll;
    return web_snapshot_valid(s);
}
static int web_snapshot_restore(const struct web_snapshot *s) {
    if (!web_snapshot_valid(s) || !web_new_tab()) return 0;
    struct web_tab *t=&web_tabs[web_active];
    memcpy(t->url,s->url,sizeof t->url); memcpy(t->history,s->history,sizeof t->history);
    memcpy(web_marks,s->marks,sizeof web_marks); web_nmarks=s->nmarks;
    t->doc=s->doc;
    /* Belt and braces: the saved copy already holds no slots, but a restore
     * must never be the thing that grants ownership. */
    memset(t->doc.owns,0,sizeof t->doc.owns);
    for (int i=0;i<t->doc.nimages;i++) { t->doc.images[i].slot=-1; t->doc.images[i].loaded=0; }
    t->hcount=s->hcount; t->hpos=s->hpos; web_scroll(s->scroll);
    web_copy(web_address,sizeof web_address,t->url);
    /* New tab was zeroed: no automatic document OR missing-image request.
     * User reload/navigation starts fresh transport; secure badge stays off. */
    web_status(t,"Snapshot restored offline - R reloads; navigation reconnects");
    return 1;
}

/* Use the exact rendered item geometry for hit-testing laid-out links and
 * images. */
static int web_item_box(const struct web_item *it,int scroll,int *x,int *y,int *w,int *h) {
    *x=it->x+4; *y=WEB_CHROME_H+it->y-scroll;
    *w=it->image>=0?96:app_text_width(it->text); *h=it->image>=0?64:WEB_LINE;
    if (*y<WEB_CHROME_H) { *h-=WEB_CHROME_H-*y; *y=WEB_CHROME_H; }
    if (*y+*h>WEB_H-WEB_STATUS_H) *h=WEB_H-WEB_STATUS_H-*y;
    if (*x<0) { *w+=*x; *x=0; }
    if (*x+*w>WEB_W) *w=WEB_W-*x;
    return *w>0 && *h>0;
}

static void web_press(int button) {
    struct web_tab *t = &web_tabs[web_active];
    if (button == WEB_BTN_BACK)   { web_back(-1); return; }
    if (button == WEB_BTN_FWD)    { web_back(1); return; }
    if (button == WEB_BTN_HOME)   { web_navigate(web_active, "http://outrun.local/", 1); return; }
    if (button == WEB_BTN_MARK)   { web_menu = !web_menu; web_bookmark(t->url); return; }
    if (button == WEB_BTN_RELOAD) {
        /* One button, two jobs: Stop while a transfer is live, Refresh when it
         * is not -- which is why it tests `pending` rather than assuming. */
        if (t->pending) {
            if (web_job_tab == web_active) { web_close(&web_current); web_current.state = WEB_IDLE; web_job_tab = -1; }
            t->pending = 0; web_status(t, "Stopped");
        } else if (t->url[0]) web_navigate(web_active, t->url, 0);
    }
}

/* Address-bar editing. `web_select_all` models the state after a click into the
 * field: the first printable key REPLACES the text, every later key appends. */
static void web_type(int code) {
    size_t n = strlen(web_address);
    if (code == 27) { web_editing = web_select_all = 0; return; }
    if (code == 13 || code == 10) {
        web_editing = web_select_all = 0;
        if (web_address[0]) web_navigate(web_active, web_address, 1);
        return;
    }
    if (code == 8 || code == 127) {
        if (web_select_all) { web_address[0] = 0; web_select_all = 0; return; }
        if (n) web_address[n - 1] = 0;
        return;
    }
    if (code < 32 || code > 126) return;
    if (web_select_all) { web_address[0] = 0; n = 0; web_select_all = 0; }
    if (n + 1 < sizeof web_address) { web_address[n] = (char)code; web_address[n + 1] = 0; }
}

static void web_click(int x, int y) {
    if (y < WEB_TABBAR_H) {
        for (int i = 0; i < WEB_TABS; i++) {
            int tx, tw;
            if (!web_tab_slot(i, &tx, &tw) || x < tx || x >= tx + tw) continue;
            if (!web_tabs[i].used) { web_new_tab(); return; }
            if (web_tab_close_hit(i, x, y)) { web_activate(i); web_close_tab(); return; }
            web_activate(i); return;
        }
        if (x >= WEB_W - 26) web_new_tab();
        return;
    }
    if (y < WEB_CHROME_H) {
        for (int i = 0; i < WEB_BTN_COUNT; i++) {
            int bx, by, bw, bh;
            if (web_slot(i, &bx, &by, &bw, &bh) && app_hit(x, y, bx, by, bw, bh)) { web_press(i); return; }
        }
        if (app_hit(x, y, WEB_ADDR_X, WEB_TABBAR_H + 3, WEB_ADDR_W, WEB_TOOLBAR_H - 6)) {
            web_editing = web_select_all = 1;
            web_copy(web_address, sizeof web_address, web_tabs[web_active].url);
        }
        return;
    }
    if (y >= WEB_H - WEB_STATUS_H) return;
    /* Viewport: resolve the click to a laid-out item and follow its link. */
    struct web_tab *t = &web_tabs[web_active];
    for (int i = 0; i < t->doc.count; i++) {
        struct web_item *it = &t->doc.items[i];
        if (it->link < 0 || it->link >= t->doc.nlinks) continue;
        int bx,by,bw,bh;
        if (!web_item_box(it,t->scroll,&bx,&by,&bw,&bh) || !app_hit(x,y,bx,by,bw,bh)) continue;
        char dest[WEB_URL];
        if (web_resolve(t->url, web_link(&t->doc, it->link), dest)) web_navigate(web_active, dest, 1);
        return;
    }
}

static void web_event(struct outrun_event *e) {
    if (e->type == EVENT_MOUSE_DOWN) { web_editing = 0; web_click(e->x, e->y); return; }
    if (e->type != EVENT_KEY_PRESS) return;
    if (web_editing) { web_type(e->code); return; }
    struct web_tab *t = &web_tabs[web_active];
    (void)t;
    switch (e->code) {
        case 'l': web_editing = web_select_all = 1;
                  web_copy(web_address, sizeof web_address, web_tabs[web_active].url); break;
        case 't': web_new_tab(); break;
        case 'w': web_close_tab(); break;
        case 'r': web_press(WEB_BTN_RELOAD); break;
        case '[': web_back(-1); break;
        case ']': web_back(1); break;
        case 'j': web_scroll(web_tabs[web_active].scroll + WEB_LINE); break;
        case 'k': web_scroll(web_tabs[web_active].scroll - WEB_LINE); break;
        case ' ': web_scroll(web_tabs[web_active].scroll + WEB_VIEW_H - WEB_LINE); break;
        case 'g': web_scroll(0); break;
        /* Offline snapshot of the active tab, and restore into a new tab.
         * ONE slot, deliberately: this is a scratch "hold this page while I go
         * look at something" facility, not a session manager. The snapshot is
         * a typed, versioned, bounded copy of document + scroll + history --
         * it holds no socket and no TLS session, so a restored tab is offline
         * until the user navigates or reloads, and its secure badge stays off
         * because nothing about the restored bytes was re-verified. */
        case 'S':
            web_status(&web_tabs[web_active],
                       web_snapshot_save(&web_saved_page)
                           ? "Page snapshotted - press R to restore into a new tab"
                           : "Snapshot failed: document too large or no free tab");
            break;
        case 'R':
            if (!web_saved_page.version)
                web_status(&web_tabs[web_active], "No snapshot saved - press S first");
            else if (!web_snapshot_restore(&web_saved_page))
                web_status(&web_tabs[web_active], "Restore failed: no free tab or snapshot rejected");
            break;
        default: break;
    }
}

/* Queue the next undecoded inline image for this tab, if any. Images are
 * fetched one at a time AFTER the document, on the same single job slot: the
 * page is readable while they arrive, and a page full of <img> cannot open
 * eight sockets or eight TLS sessions at once. Returns 0 when none remain. */
static int web_fetch_next_image(struct web_tab *t) {
    for (int i = 0; i < t->doc.nimages; i++) {
        struct web_image *im = &t->doc.images[i];
        if (im->loaded || !im->src[0]) continue;
        char dest[WEB_URL];
        if (!web_resolve(t->url, im->src, dest)) { im->loaded = -1; continue; }
        if (!web_start(&web_current, dest)) { im->loaded = -1; continue; }
        t->image_next = i + 1;          /* 1-based: 0 means "document, not image" */
        web_job_tab = (int)(t - web_tabs);
        web_started = web_clock(0);
        return 1;
    }
    t->image_next = 0;
    return 0;
}

/* Pump the single live transfer. One job at a time bounds TLS memory; tabs
 * queue, and the FIRST pending tab wins so a background tab cannot starve the
 * one the user is looking at forever. */
static void web_pump(void) {
    if (web_job_tab < 0) {
        for (int i = 0; i < WEB_TABS; i++) {
            if (!web_tabs[i].used || !web_tabs[i].pending) continue;
            if (!web_start(&web_current, web_tabs[i].url)) {
                web_tabs[i].pending = 0; web_status(&web_tabs[i], web_current.error);
                return;
            }
            web_job_tab = i; web_started = web_clock(0);
            web_status(&web_tabs[i], "Loading");
            return;
        }
        return;
    }
    struct web_tab *t = &web_tabs[web_job_tab];
    web_step(&web_current);
    if (web_current.state == WEB_ERROR) {
        t->pending = 0; web_status(t, web_current.error);
        web_job_tab = -1; return;
    }
    if (web_current.state != WEB_DONE) return;
    t->pending = 0; t->secure = web_current.verified;
    /* An image fetch finishing must NOT be parsed as a document: it would
     * replace the page that referenced it with an empty parse of binary data.
     * image_next is 1-based while images load, so a non-zero value here means
     * this completion belongs to image number image_next-1. */
    if (t->image_next) {
        int slot = t->image_next - 1;
        if (slot >= 0 && slot < t->doc.nimages)
            web_decode_image(&t->doc, &t->doc.images[slot],
                             (const unsigned char *)web_current.buffer + web_current.response.body,
                             web_current.response.length);
        web_job_tab = -1;
        web_fetch_next_image(t);
        return;
    }
    web_copy(t->url, sizeof t->url, web_current.address);
    web_html(&t->doc, web_current.buffer + web_current.response.body);
    t->scroll = 0;
    char ms[16]; web_num(ms, (unsigned)(web_clock(0) - web_started));
    char line[96]; web_copy(line, sizeof line, web_current.verified ? "Secure - loaded in " : "Loaded in ");
    web_add(line, sizeof line, ms); web_add(line, sizeof line, " ms");
    web_status(t, line);
    if (web_active == web_job_tab) web_copy(web_address, sizeof web_address, t->url);
    web_job_tab = -1;
    web_fetch_next_image(t);
}

static void web_draw_document(struct app_win *w, const struct web_doc *doc, int scroll) {
    /* Document. Only the lines inside the viewport are drawn. */
    for (int i = 0; i < doc->count; i++) {
        const struct web_item *it = &doc->items[i];
        int y = WEB_CHROME_H + it->y - scroll;
        if (y < WEB_CHROME_H - WEB_LINE || y >= WEB_H - WEB_STATUS_H) continue;
        if (it->image >= 0 && it->image < doc->nimages) {
            const struct web_image *im = &doc->images[it->image];
            const unsigned *px = web_pool_pixels(doc, im->slot);
            if (im->loaded > 0 && px) {
                /* Blit the decoded thumbnail from the pool, clipped to the
                 * viewport. px is NULL for any slot the document does not
                 * hold, so a stale index draws the placeholder instead of
                 * another document's pixels. */
                for (int r = 0; r < WEB_IMG_H; r++) {
                    int py = y + r;
                    if (py < WEB_CHROME_H || py >= WEB_H - WEB_STATUS_H) continue;
                    for (int c = 0; c < WEB_IMG_W; c++)
                        app_rect(w, it->x + 4 + c, py, 1, 1, px[r * WEB_IMG_W + c]);
                }
            } else {
                app_rect(w, it->x + 4, y, WEB_IMG_W, WEB_IMG_H, 0x1d2534);
                app_text(w, it->x + 8, y + 26, im->alt[0] ? im->alt : "[image]", 0x8fa3bf);
            }
            continue;
        }
        /* A rule is a drawn line, not text: without this branch <hr> parses
         * correctly and renders as nothing at all. */
        if (it->kind == WEB_ITEM_RULE) {
            app_rect(w, it->x + 4, y + WEB_LINE / 2, WEB_W - 40, 1, it->color);
            continue;
        }
        /* Background fill first: a page that sets background-color expects it
         * behind the text, not instead of it. WEB_BG_NONE means transparent,
         * which is the overwhelmingly common case. */
        if (it->bg != WEB_BG_NONE)
            app_rect(w, it->x + 4, y, app_text_width(it->text), WEB_LINE, it->bg);
        app_text(w, it->x + 4, y, it->text, it->link >= 0 ? 0x77aaff : it->color);
    }
}

static void web_draw(struct app_win *w) {
    struct web_tab *t = &web_tabs[web_active];
    app_fill(w, 0x0a0d14);
    /* Tab strip. The active tab is raised and lit; the rest are dimmed, which
     * is the same active/inactive language the window titlebar uses. */
    app_rect(w, 0, 0, WEB_W, WEB_TABBAR_H, 0x151b28);
    for (int i = 0; i < WEB_TABS; i++) {
        int x, tw;
        if (!web_tab_slot(i, &x, &tw)) continue;
        if (!web_tabs[i].used) continue;
        int on = i == web_active;
        app_rect(w, x, 2, tw, WEB_TABBAR_H - 2, on ? 0x22e4ff : 0x1d2534);
        app_rect(w, x + 1, 3, tw - 2, WEB_TABBAR_H - 4, on ? 0x11304a : 0x161d2a);
        const char *label = web_tabs[i].url[0] ? web_tabs[i].url : "New tab";
        app_text(w, x + 6, 5, label, on ? 0xeaf2f7 : 0x8fa3bf);
        app_text(w, x + tw - 14, 5, "x", on ? 0xff6b81 : 0x6f8099);
    }
    app_text(w, WEB_W - 22, 5, "+", 0x3df5c4);
    /* Toolbar. */
    app_rect(w, 0, WEB_TABBAR_H, WEB_W, WEB_TOOLBAR_H, 0x121826);
    for (int i = 0; i < WEB_BTN_COUNT; i++) {
        int x, y, bw, bh;
        if (!web_slot(i, &x, &y, &bw, &bh)) continue;
        int live = 1;
        if (i == WEB_BTN_BACK) live = t->hpos > 0;
        if (i == WEB_BTN_FWD)  live = t->hcount && t->hpos + 1 < t->hcount;
        const char *label = i == WEB_BTN_RELOAD && t->pending ? "S" : web_btn[i].label;
        app_button(w, x, y, bw, bh, label, live);
    }
    app_rect(w, WEB_ADDR_X, WEB_TABBAR_H + 3, WEB_ADDR_W, WEB_TOOLBAR_H - 6,
             web_editing ? 0x22e4ff : 0x2a3446);
    app_rect(w, WEB_ADDR_X + 1, WEB_TABBAR_H + 4, WEB_ADDR_W - 2, WEB_TOOLBAR_H - 8, 0x0a0d14);
    if (t->secure) app_text(w, WEB_ADDR_X + 4, WEB_TABBAR_H + 7, "#", 0x3df5c4);
    app_text(w, WEB_ADDR_X + 16, WEB_TABBAR_H + 7,
             web_editing ? web_address : (t->url[0] ? t->url : "Type a URL, or press L"),
             web_editing ? 0xeaf2f7 : 0xc4d2e2);
    web_draw_document(w, &t->doc, t->scroll);
    if (web_menu && web_nmarks) {
        int h = web_nmarks * WEB_LINE + 6;
        app_rect(w, 140, WEB_CHROME_H, 420, h, 0x22e4ff);
        app_rect(w, 141, WEB_CHROME_H + 1, 418, h - 2, 0x121826);
        for (int i = 0; i < web_nmarks; i++)
            app_text(w, 146, WEB_CHROME_H + 4 + i * WEB_LINE, web_marks[i], 0xc4d2e2);
    }
    /* Status bar. */
    app_rect(w, 0, WEB_H - WEB_STATUS_H, WEB_W, WEB_STATUS_H, 0x151b28);
    app_text(w, 6, WEB_H - WEB_STATUS_H + 3, t->status, 0x9deaff);
    /* Pool pressure, shown only once the shared pixel pool is more than half
     * spoken for. Images that could not claim a slot render as placeholders,
     * and without this the user has no way to tell that apart from an image
     * that simply failed to download. */
    {
        int used = web_pool_used(&t->doc), total = 0;
        for (int i = 0; i < WEB_POOL_SLOTS; i++) if (web_pool_taken[i]) total++;
        if (total * 2 > WEB_POOL_SLOTS) {
            char note[40]; web_copy(note, sizeof note, "img ");
            char n[12]; web_num(n, (unsigned)used); web_add(note, sizeof note, n);
            web_add(note, sizeof note, "/");
            web_num(n, (unsigned)WEB_POOL_SLOTS); web_add(note, sizeof note, n);
            app_text(w, WEB_W - 260, WEB_H - WEB_STATUS_H + 3, note,
                     total >= WEB_POOL_SLOTS ? 0xffcc66 : 0x8fa3bf);
        }
    }
    if (t->doc.truncated)
        app_text(w, WEB_W - 150, WEB_H - WEB_STATUS_H + 3, "document truncated", 0xffcc66);
}

/* Ring-3 entry. Applications here are _start, not main: user.ld names _start as
 * ENTRY and there is no C runtime to call main for us. Linking with main()
 * produced a working ELF whose entry point the linker had to guess, which boots
 * into whatever happens to sit at the default address.
 *
 * Compiled out for WEB_UI_TEST: that build deliberately omits the 200 KB CA
 * roots header (an included-but-unused array trips -Wunused-const-variable),
 * so the one reference to web_ca_roots below cannot resolve there. The host UI
 * test supplies its own main(). */
#ifndef WEB_UI_TEST
void _start(void) {
    struct app_win w;
    /* app_create returns NEGATIVE on failure, not zero -- see vault_pad. */
    if (app_create(&w, WEB_W, WEB_H, 0x22e4ff) < 0) app_exit(1);
    app_title(&w, "OutRun Web");
    web_ui_init();
    /* sizeof, NOT sizeof-1: mbedtls PEM parsing requires the length to include
     * the terminating NUL, and getting it wrong parses zero certificates --
     * after which every HTTPS request fails closed and looks like a network
     * fault rather than a build mistake. */
    web_runtime_init(web_ca_roots, sizeof web_ca_roots);
    for (;;) {
        struct outrun_event e;
        int r = app_poll(&w, &e);
        if (r < 0) app_exit(0);
        if (r) { web_event(&e); continue; }
        web_pump();
        web_draw(&w);
        app_present(&w);
        app_idle();
    }
}
#endif /* !WEB_UI_TEST: host UI test supplies its own main() */
#endif /* UI section */
#endif /* !WEB_CORE_TEST */