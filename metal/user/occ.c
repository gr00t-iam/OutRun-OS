/* ============================================================================
 * occ — the Outrun C Compiler.  Runs at RING 3, inside OutRun OS.
 * ============================================================================
 * Reads a .c file out of the VFS, compiles it, and writes a runnable x86-64 ELF
 * back to the VFS. No host toolchain is involved at any point: this file is
 * compiled INTO the ring-3 image once, and thereafter the compiler that runs is
 * the one executing on OutRun's own scheduler, using OutRun's own heap and
 * filesystem syscalls.
 *
 * It is a classic SINGLE-PASS compiler: the parser emits machine code as it
 * parses, with no AST and no separate assembler or linker stage. That is a
 * deliberate trade — it is why the whole thing fits in one auditable file, and
 * it is why the "assembling" and "linking" steps of a conventional toolchain
 * are collapsed into the code emitter rather than existing as separate passes.
 * Forward calls are the one thing a single pass cannot resolve immediately, so
 * those go through a small fixup table patched when the function is defined.
 *
 * THE LANGUAGE SUBSET (documented honestly — this is not C89):
 *   types      int, char, and pointers to them. `int` is 64-BIT here: every
 *              value lives in a full register and every stack slot is 8 bytes.
 *              That is a real deviation from C, and it is the single
 *              simplification that keeps the code generator this small.
 *   decls      global `int`/`char *` variables; functions with <= 6 parameters
 *   stmts      compound blocks, local declarations, if/else, while, for,
 *              return, expression statements
 *   exprs      = || && | ^ & == != < > <= >= << >> + - * / %
 *              unary - ! * &  calls, integer literals, character literals,
 *              string literals, identifiers, parentheses, indexing a[i]
 *              — but note a[i] means *(a + i*8): a WORD, never a byte.
 *   builtins   __syscall(n, a0, a1, a2) — the whole OS ABI in one intrinsic,
 *                  so a compiled program can do real I/O with no libc linked in
 *              __ldb(p, i)     — the unsigned BYTE at p[i]
 *              __stb(p, i, v)  — store v's low byte at p[i]; returns v
 *                  These two exist because a[i] is word-scaled, so without them
 *                  occ could not walk a C string and /usr/lib/libc.oc's
 *                  strlen/strcpy/atoi could not be written at all.
 *   NOT here   structs, unions, enums, typedefs, floats, switch, goto, the
 *              preprocessor, varargs, multi-file linking, and LOCAL ARRAYS
 *              (a local declaration is one 8-byte slot; scratch buffers have
 *              to be globals, which is why libc.oc's are)
 *
 * MEMORY MODEL. Three fixed, page-aligned bases mean absolute addressing works
 * and no relocation machinery is needed: text is emitted before data sizes are
 * known, but the data BASE is chosen up front, so a global's address is simply
 * DATA_BASE + offset baked into a mov as an immediate.
 * ==========================================================================*/

#define OCC_TEXT_BASE   0x0000500000000000ull
#define OCC_RODATA_BASE 0x0000500000200000ull
#define OCC_DATA_BASE   0x0000500000400000ull
#define OCC_STACK_TOP   0x0000500000FF3C00ull   /* mirrors the kernel's USTK_INIT */

#define OCC_MAXSRC   (192u * 1024u)  /* v0.57: prelude + headers + user code */
#define OCC_MAXTEXT  (48u * 1024u)
#define OCC_MAXDATA  (8u * 1024u)
#define OCC_MAXSYM   256
#define OCC_MAXFIX   1024
#define OCC_MAXLOC   32
#define OCC_NAMELEN  32

/* ---- diagnostics ----------------------------------------------------------
 * Everything goes to stderr through the std fd table, so a compile error shows
 * up in the Cyber-Terminal exactly like any other program's output. */
static int   occ_errors;
static int   occ_line;
/* v0.57: the file the current line came from. Maintained by the lexer from the
 * `#line N FILE` markers the preprocessor emits, which replaces v0.56's
 * "subtract the prelude's line count" arithmetic — that worked only while the
 * prelude was the single thing prepended, and #include ended that. */
#define OCC_FILELEN 64
static char  occ_file[OCC_FILELEN] = "<none>";

static void occ_err(const char *msg, const char *what) {
    occ_errors++;
    char b[288]; int n = 0;
    const char *p = "occ: ";
    while (*p) b[n++] = *p++;
    for (int k = 0; occ_file[k] && n < 80; k++) b[n++] = occ_file[k];
    b[n++] = ':';
    /* line number, decimal */
    { int v = occ_line, d[8], k = 0; if (!v) d[k++] = 0; while (v) { d[k++] = v % 10; v /= 10; }
      while (k) b[n++] = (char)('0' + d[--k]); }
    b[n++] = ':'; b[n++] = ' ';
    for (const char *q = msg; *q && n < 160; q++) b[n++] = *q;
    if (what && *what) { b[n++] = ' '; b[n++] = '\'';
        for (const char *q = what; *q && n < 185; q++) b[n++] = *q;
        b[n++] = '\''; }
    b[n++] = '\n'; b[n] = 0;
    owrite(STDERR_FILENO, b, (u64)n);
}

/* Same channel, for things that are not errors (the SDK banner). Diagnostics
 * exist to be READ, and on this system the thing reading them is the
 * Cyber-Terminal: it arms the kernel console capture via SYS_RUN_CMD, and
 * ring-3 SYS_WRITE goes through the kernel's kputc, so anything written here
 * lands in the terminal's window as well as on the serial console. */
static void occ_note(const char *msg, const char *what) {
    char b[224]; int n = 0;
    const char *p = "occ: ";
    while (*p) b[n++] = *p++;
    for (const char *q = msg; *q && n < 160; q++) b[n++] = *q;
    if (what && *what) { b[n++] = ' ';
        for (const char *q = what; *q && n < 210; q++) b[n++] = *q; }
    b[n++] = '\n'; b[n] = 0;
    owrite(STDERR_FILENO, b, (u64)n);
}

/* ===========================================================================
 * v0.57: THE PREPROCESSOR
 * ===========================================================================
 * A genuine pass, not a skip. It runs to completion BEFORE the lexer sees a
 * single character, turning a source file plus its includes into one expanded
 * translation unit in `occ_src`. The fused lexer/parser/codegen below is
 * untouched by it, which is the whole reason for doing it as a separate pass:
 * a single-pass code generator cannot also be re-entered to expand a macro.
 *
 * WHAT IT DOES
 *   #include <h> / "h"   resolved against /usr/include/ on the VFS (and, for
 *                        the quoted form, first against the including file's
 *                        own directory — that is the only difference between
 *                        the two forms here, and it is the standard one)
 *   #define NAME body    object-like substitution
 *   #define NAME(a,b) …  function-like substitution with positional arguments
 *   #undef NAME
 *   #ifdef / #ifndef / #else / #endif   nested, so header guards work
 *   #line n "file"       EMITTED by this pass, CONSUMED by the lexer, so a
 *                        diagnostic names the user's real file and line even
 *                        after the prelude and three headers have been pasted
 *                        in front of their code. This is what replaces v0.56's
 *                        "subtract the prelude's line count" arithmetic, which
 *                        could not survive #include at all.
 *
 * COMMENTS ARE STRIPPED FIRST, on load, before any directive is looked at.
 * That ordering is not cosmetic: /usr/include/outrun_abi.h contains
 *
 *     #define O_CREAT 1   / * the ONLY defined SYS_OPEN flag. Any other
 *                           * bit is rejected with -22, on purpose.   * /
 *
 * — a macro whose body is followed by a comment that runs onto the next line.
 * Processing directives first would define O_CREAT as `1 / * the ONLY …`.
 * Newlines inside comments are preserved so line numbers stay honest.
 *
 * HONEST LIMITS, all of them deliberate and none of them hidden:
 *   - A function-like macro's invocation must fit on one line.
 *   - Rescanning for nested macros is bounded (OCC_PP_RESCAN passes) rather
 *     than a proper recursive expansion; a macro that expands to itself is
 *     also blocked by a per-macro `expanding` flag, so neither can loop.
 *   - No #if with expressions, no #elif, no ##, no #, no varargs macros.
 *     #ifdef/#ifndef is what header guards need and that is what is here.
 * ==========================================================================*/
/* Shared by the preprocessor and the lexer, so they live above both. */
static int occ_isalpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int occ_isdigit(char c) { return c >= '0' && c <= '9'; }
static int occ_isalnum(char c) { return occ_isalpha(c) || occ_isdigit(c); }

#define OCC_MAXMACRO    128
#define OCC_MACRO_BODY  192
#define OCC_MACRO_PARAM 6
#define OCC_PP_MAXDEPTH 8
#define OCC_PP_RESCAN   8
#define OCC_PP_MAXLINE  1024

struct occ_macro {
    char name[OCC_NAMELEN];
    char body[OCC_MACRO_BODY];
    char params[OCC_MACRO_PARAM][OCC_NAMELEN];
    int  nparams;                    /* -1 = object-like                      */
    int  used;
    int  expanding;                  /* blue paint: no self-recursion         */
};
static struct occ_macro occ_macros[OCC_MAXMACRO];
static int  occ_nmacro;

static char *occ_ppout;              /* the expanded unit being built         */
static int   occ_ppn;                /* bytes written so far                  */
static int   occ_ppcap;
static int   occ_pp_depth;
static int   occ_pp_includes;        /* how many files were pasted in         */

static int occ_ppeq(const char *a, const char *b) { return ostrneq(a, b, OCC_NAMELEN); }

static int occ_macro_find(const char *n) {
    for (int i = 0; i < occ_nmacro; i++)
        if (occ_macros[i].used && occ_ppeq(occ_macros[i].name, n)) return i;
    return -1;
}
static void occ_macro_undef(const char *n) {
    int i = occ_macro_find(n);
    if (i >= 0) occ_macros[i].used = 0;
}
static void occ_ppemit(const char *s, int n) {
    for (int i = 0; i < n && occ_ppn < occ_ppcap - 1; i++) occ_ppout[occ_ppn++] = s[i];
}
static void occ_ppemitc(char c) { if (occ_ppn < occ_ppcap - 1) occ_ppout[occ_ppn++] = c; }
static void occ_ppemitz(const char *s) { while (*s) occ_ppemitc(*s++); }

/* `#line N FILE` — the lexer parses these to keep occ_line/occ_file truthful.
 * The filename is emitted bare (no quotes) because the lexer reads it as a
 * run of non-space characters, and no path in this system contains a space. */
static void occ_ppmark(int line, const char *file) {
    char b[32]; int n = 0, d[10], k = 0, v = line;
    occ_ppemitz("\n#line ");
    if (!v) d[k++] = 0;
    while (v) { d[k++] = v % 10; v /= 10; }
    while (k) b[n++] = (char)('0' + d[--k]);
    occ_ppemit(b, n);
    occ_ppemitc(' ');
    occ_ppemitz(file);
    occ_ppemitc('\n');
}

/* Strip both comment forms in place, keeping every newline so that line
 * numbers survive. Also collapses a backslash-newline continuation, which is
 * how a macro body longer than one line is written. */
static void occ_pp_decomment(char *s) {
    int r = 0, w = 0;
    while (s[r]) {
        if (s[r] == '\\' && s[r + 1] == '\n') { s[w++] = ' '; r += 2; continue; }
        if (s[r] == '/' && s[r + 1] == '/') {
            while (s[r] && s[r] != '\n') r++;
            continue;
        }
        if (s[r] == '/' && s[r + 1] == '*') {
            r += 2;
            while (s[r] && !(s[r] == '*' && s[r + 1] == '/')) {
                if (s[r] == '\n') s[w++] = '\n';         /* keep the line count */
                r++;
            }
            if (s[r]) r += 2;
            s[w++] = ' ';
            continue;
        }
        if (s[r] == '"' || s[r] == '\'') {               /* never look inside   */
            char q = s[r];
            s[w++] = s[r++];
            while (s[r] && s[r] != q) {
                if (s[r] == '\\' && s[r + 1]) s[w++] = s[r++];
                s[w++] = s[r++];
            }
            if (s[r]) s[w++] = s[r++];
            continue;
        }
        s[w++] = s[r++];
    }
    s[w] = 0;
}

static int occ_ppspace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

/* Copy one identifier out of `s` at `i`; returns its length. */
static int occ_ppident(const char *s, int i, char *out, int cap) {
    int n = 0;
    while (s[i] && occ_isalnum(s[i]) && n < cap - 1) out[n++] = s[i++];
    out[n] = 0;
    return n;
}

/* Substitute every macro invocation in `in`, writing to `out`. Returns 1 if
 * anything was replaced, so the caller can rescan for nested macros. */
static int occ_pp_expand_once(const char *in, char *out, int cap) {
    int i = 0, o = 0, did = 0;
    while (in[i] && o < cap - 1) {
        if (in[i] == '"' || in[i] == '\'') {             /* literals are opaque */
            char q = in[i];
            out[o++] = in[i++];
            while (in[i] && in[i] != q && o < cap - 2) {
                if (in[i] == '\\' && in[i + 1]) out[o++] = in[i++];
                out[o++] = in[i++];
            }
            if (in[i]) out[o++] = in[i++];
            continue;
        }
        if (!occ_isalpha(in[i])) { out[o++] = in[i++]; continue; }

        char id[OCC_NAMELEN];
        int  idl = occ_ppident(in, i, id, sizeof id);
        int  mi  = occ_macro_find(id);
        if (mi < 0 || occ_macros[mi].expanding) {
            for (int k = 0; k < idl && o < cap - 1; k++) out[o++] = in[i + k];
            i += idl;
            continue;
        }
        struct occ_macro *m = &occ_macros[mi];

        if (m->nparams < 0) {                            /* object-like        */
            i += idl;
            m->expanding = 1;
            for (const char *p = m->body; *p && o < cap - 1; p++) out[o++] = *p;
            m->expanding = 0;
            did = 1;
            continue;
        }

        /* function-like: it is only an invocation if a '(' follows */
        int j = i + idl;
        while (in[j] && occ_ppspace(in[j])) j++;
        if (in[j] != '(') {
            for (int k = 0; k < idl && o < cap - 1; k++) out[o++] = in[i + k];
            i += idl;
            continue;
        }
        j++;                                             /* past '('           */
        char args[OCC_MACRO_PARAM][OCC_MACRO_BODY];
        int  na = 0, depth = 1;
        for (int a = 0; a < OCC_MACRO_PARAM; a++) args[a][0] = 0;
        int al = 0;
        while (in[j] && depth > 0) {
            if (in[j] == '(') depth++;
            else if (in[j] == ')') { depth--; if (!depth) { j++; break; } }
            if (depth == 1 && in[j] == ',') {
                if (na < OCC_MACRO_PARAM - 1) { args[na][al] = 0; na++; al = 0; }
                j++;
                while (in[j] && occ_ppspace(in[j])) j++;
                continue;
            }
            if (na < OCC_MACRO_PARAM && al < OCC_MACRO_BODY - 1) args[na][al++] = in[j];
            j++;
        }
        if (na < OCC_MACRO_PARAM) { args[na][al] = 0; na++; }
        if (na != m->nparams) {
            occ_err("wrong argument count for macro", m->name);
            for (int k = 0; k < idl && o < cap - 1; k++) out[o++] = in[i + k];
            i += idl;
            continue;
        }
        /* paste the body, replacing parameter names with the actual arguments */
        m->expanding = 1;
        for (int b = 0; m->body[b] && o < cap - 1; ) {
            if (!occ_isalpha(m->body[b])) { out[o++] = m->body[b++]; continue; }
            char bid[OCC_NAMELEN];
            int bl = occ_ppident(m->body, b, bid, sizeof bid);
            int pi = -1;
            for (int p = 0; p < m->nparams; p++) if (occ_ppeq(m->params[p], bid)) { pi = p; break; }
            if (pi >= 0) {
                out[o++] = '(';                          /* keep precedence     */
                for (const char *q = args[pi]; *q && o < cap - 2; q++) out[o++] = *q;
                if (o < cap - 1) out[o++] = ')';
            } else {
                for (int k = 0; k < bl && o < cap - 1; k++) out[o++] = m->body[b + k];
            }
            b += bl;
        }
        m->expanding = 0;
        i = j;
        did = 1;
    }
    out[o] = 0;
    return did;
}

static void occ_pp_expand(const char *in, char *out, int cap) {
    static char a[OCC_PP_MAXLINE * 2], b[OCC_PP_MAXLINE * 2];
    int n = 0;
    while (in[n] && n < (int)sizeof a - 1) { a[n] = in[n]; n++; }
    a[n] = 0;
    for (int pass = 0; pass < OCC_PP_RESCAN; pass++) {
        if (!occ_pp_expand_once(a, b, (int)sizeof b)) break;
        for (n = 0; b[n] && n < (int)sizeof a - 1; n++) a[n] = b[n];
        a[n] = 0;
    }
    int o = 0;
    while (a[o] && o < cap - 1) { out[o] = a[o]; o++; }
    out[o] = 0;
}

static void occ_pp_file(const char *path);

/* Resolve an #include target. `<x>` looks only in /usr/include; `"x"` tries
 * the including file's own directory first, then falls back the same way. */
static void occ_pp_include(const char *spec, int angled, const char *from) {
    char path[OCC_MACRO_BODY];
    int n = 0;
    if (!angled) {
        int slash = -1;
        for (int i = 0; from[i]; i++) if (from[i] == '/') slash = i;
        if (slash > 0) {
            for (int i = 0; i <= slash && n < (int)sizeof path - 1; i++) path[n++] = from[i];
            for (int i = 0; spec[i] && n < (int)sizeof path - 1; i++) path[n++] = spec[i];
            path[n] = 0;
            if (oopen(path) >= 0) { occ_pp_file(path); return; }
            /* not there: fall through to the system directory */
            n = 0;
        }
    }
    const char *inc = "/usr/include/";
    n = 0;
    for (int i = 0; inc[i] && n < (int)sizeof path - 1; i++) path[n++] = inc[i];
    for (int i = 0; spec[i] && n < (int)sizeof path - 1; i++) path[n++] = spec[i];
    path[n] = 0;
    occ_pp_file(path);
}

/* Process one file into occ_ppout. Recursive, depth-limited. */
static void occ_pp_file(const char *path) {
    if (occ_pp_depth >= OCC_PP_MAXDEPTH) { occ_err("#include nested too deeply at", path); return; }

    int fd = oopen(path);
    if (fd < 0) { occ_err("cannot open include", path); return; }
    char *buf = (char *)omalloc(OCC_MAXSRC);
    if (!buf) { oclose(fd); occ_err("out of memory reading", path); return; }
    i64 n = oread(fd, buf, OCC_MAXSRC - 1);
    oclose(fd);
    if (n < 0) { ofree(buf); occ_err("cannot read", path); return; }
    while (n > 0 && buf[n - 1] == 0) n--;                /* stored NUL, if any */
    buf[n] = 0;
    occ_pp_decomment(buf);
    occ_pp_depth++;
    occ_pp_includes++;

    /* Conditional-compilation stack. `cond_active[k]` is strictly "level k's
     * OWN branch is the live one" — never "we are emitting at level k". Those
     * two are different and conflating them is the classic way nested
     * conditionals go wrong: `emitting` is the AND across all open levels and
     * is recomputed from scratch by occ_pp_emitting() whenever the stack
     * changes, so no level's state depends on the order it was pushed. */
    int  cond_active[16], cond_taken[16];
    int  ncond = 0;
    int  emitting = 1;
    int  line = 1;
    occ_ppmark(line, path);

    int i = 0;
    static char raw[OCC_PP_MAXLINE], exp[OCC_PP_MAXLINE * 2];
    while (buf[i]) {
        int l = 0;
        while (buf[i] && buf[i] != '\n' && l < OCC_PP_MAXLINE - 1) raw[l++] = buf[i++];
        raw[l] = 0;
        if (buf[i] == '\n') i++;

        int p = 0;
        while (raw[p] && occ_ppspace(raw[p])) p++;

        if (raw[p] == '#') {
            p++;
            while (raw[p] && occ_ppspace(raw[p])) p++;
            char dir[OCC_NAMELEN];
            int dl = occ_ppident(raw, p, dir, sizeof dir);
            p += dl;
            while (raw[p] && occ_ppspace(raw[p])) p++;

            if (occ_ppeq(dir, "ifdef") || occ_ppeq(dir, "ifndef")) {
                char nm[OCC_NAMELEN];
                occ_ppident(raw, p, nm, sizeof nm);
                int have = occ_macro_find(nm) >= 0;
                int want = occ_ppeq(dir, "ifdef") ? have : !have;
                if (ncond < 16) {
                    cond_active[ncond] = want;
                    cond_taken[ncond]  = want;
                    ncond++;
                    emitting = 1;
                    for (int k = 0; k < ncond; k++) if (!cond_active[k]) emitting = 0;
                } else occ_err("#ifdef nested too deeply in", path);
                line++; continue;
            }
            if (occ_ppeq(dir, "else")) {
                if (ncond > 0) {
                    cond_active[ncond - 1] = !cond_taken[ncond - 1];
                    cond_taken[ncond - 1]  = 1;
                    emitting = 1;
                    for (int k = 0; k < ncond; k++) if (!cond_active[k]) emitting = 0;
                } else occ_err("#else without #ifdef in", path);
                line++; continue;
            }
            if (occ_ppeq(dir, "endif")) {
                if (ncond > 0) {
                    ncond--;
                    emitting = 1;
                    for (int k = 0; k < ncond; k++) if (!cond_active[k]) emitting = 0;
                } else occ_err("#endif without #ifdef in", path);
                line++; continue;
            }
            if (!emitting) { line++; continue; }

            if (occ_ppeq(dir, "define")) {
                char nm[OCC_NAMELEN];
                int nl = occ_ppident(raw, p, nm, sizeof nm);
                p += nl;
                int mi = occ_macro_find(nm);
                if (mi < 0) {
                    if (occ_nmacro >= OCC_MAXMACRO) { occ_err("too many macros at", nm); line++; continue; }
                    mi = occ_nmacro++;
                }
                struct occ_macro *m = &occ_macros[mi];
                for (int k = 0; k < OCC_NAMELEN; k++) m->name[k] = nm[k];
                m->used = 1; m->expanding = 0; m->nparams = -1;
                if (raw[p] == '(') {                     /* function-like       */
                    p++;
                    m->nparams = 0;
                    for (;;) {
                        while (raw[p] && occ_ppspace(raw[p])) p++;
                        if (raw[p] == ')') { p++; break; }
                        char pn[OCC_NAMELEN];
                        int pl = occ_ppident(raw, p, pn, sizeof pn);
                        if (!pl) { p++; continue; }
                        p += pl;
                        if (m->nparams < OCC_MACRO_PARAM) {
                            for (int k = 0; k < OCC_NAMELEN; k++) m->params[m->nparams][k] = pn[k];
                            m->nparams++;
                        } else occ_err("too many macro parameters in", nm);
                        while (raw[p] && occ_ppspace(raw[p])) p++;
                        if (raw[p] == ',') { p++; continue; }
                        if (raw[p] == ')') { p++; break; }
                        if (!raw[p]) break;
                    }
                }
                while (raw[p] && occ_ppspace(raw[p])) p++;
                int b = 0;
                while (raw[p] && b < OCC_MACRO_BODY - 1) m->body[b++] = raw[p++];
                while (b > 0 && occ_ppspace(m->body[b - 1])) b--;   /* trim tail */
                m->body[b] = 0;
                line++; continue;
            }
            if (occ_ppeq(dir, "undef")) {
                char nm[OCC_NAMELEN];
                occ_ppident(raw, p, nm, sizeof nm);
                occ_macro_undef(nm);
                line++; continue;
            }
            if (occ_ppeq(dir, "include")) {
                char spec[OCC_MACRO_BODY];
                int angled = 0, s = 0;
                if (raw[p] == '<' || raw[p] == '"') {
                    char close = (raw[p] == '<') ? '>' : '"';
                    angled = (raw[p] == '<');
                    p++;
                    while (raw[p] && raw[p] != close && s < (int)sizeof spec - 1) spec[s++] = raw[p++];
                    spec[s] = 0;
                    occ_pp_include(spec, angled, path);
                    occ_ppmark(++line, path);            /* back to this file   */
                    continue;
                }
                occ_err("malformed #include in", path);
                line++; continue;
            }
            if (occ_ppeq(dir, "line")) { line++; continue; }   /* ours; ignore  */
            /* Anything else (#pragma, #if, #elif, #error) is SKIPPED rather
             * than guessed at, and says so instead of silently mis-compiling. */
            occ_err("unsupported preprocessor directive", dir);
            line++; continue;
        }

        if (emitting) {
            occ_pp_expand(raw, exp, (int)sizeof exp);
            occ_ppemitz(exp);
        }
        occ_ppemitc('\n');
        line++;
    }
    if (ncond) occ_err("unterminated #ifdef in", path);
    occ_pp_depth--;
    ofree(buf);
}

/* ---- lexer ---------------------------------------------------------------*/
enum { T_EOF = 0, T_NUM, T_IDENT, T_STR, T_PUNCT, T_KW };

static char *occ_src;
static int   occ_pos;
static int   occ_tk;                 /* current token kind      */
static i64   occ_val;                /* T_NUM value             */
static char  occ_txt[OCC_NAMELEN];   /* T_IDENT / T_KW / T_PUNCT spelling */
static char  occ_str[256];           /* T_STR contents          */
static int   occ_strlen_;


static int occ_kw(const char *s) {
    static const char *kws[] = { "int", "char", "return", "if", "else", "while",
                                 "for", "void", "__syscall", "__ldb", "__stb",
                                 "struct", "union", "typedef", 0 };
    for (int i = 0; kws[i]; i++) if (ostrneq(s, kws[i], OCC_NAMELEN)) return 1;
    return 0;
}

static void occ_next(void) {
    /* whitespace and both comment forms */
    for (;;) {
        char c = occ_src[occ_pos];
        if (c == '\n') { occ_line++; occ_pos++; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { occ_pos++; continue; }
        if (c == '/' && occ_src[occ_pos + 1] == '/') {
            while (occ_src[occ_pos] && occ_src[occ_pos] != '\n') occ_pos++;
            continue;
        }
        if (c == '/' && occ_src[occ_pos + 1] == '*') {
            occ_pos += 2;
            while (occ_src[occ_pos] && !(occ_src[occ_pos] == '*' && occ_src[occ_pos + 1] == '/')) {
                if (occ_src[occ_pos] == '\n') occ_line++;
                occ_pos++;
            }
            if (occ_src[occ_pos]) occ_pos += 2;
            continue;
        }
        /* v0.57: the ONLY directive that can still be here is `#line N FILE`,
         * which the preprocessor emitted for us. Consuming it is what makes a
         * diagnostic name the user's own file and line after the prelude and
         * several headers have been pasted in front of their code — the thing
         * v0.56's "subtract the prelude's line count" arithmetic could not do
         * once #include existed. Any other '#' means the preprocessor missed
         * something, so it is skipped and reported rather than ignored. */
        if (c == '#') {
            int q = occ_pos + 1;
            if (occ_src[q] == 'l' && occ_src[q+1] == 'i' && occ_src[q+2] == 'n' && occ_src[q+3] == 'e') {
                q += 4;
                while (occ_src[q] == ' ' || occ_src[q] == '\t') q++;
                int v = 0;
                while (occ_isdigit(occ_src[q])) { v = v * 10 + (occ_src[q] - '0'); q++; }
                while (occ_src[q] == ' ' || occ_src[q] == '\t') q++;
                int n = 0;
                while (occ_src[q] && occ_src[q] != '\n' && n < OCC_FILELEN - 1) occ_file[n++] = occ_src[q++];
                occ_file[n] = 0;
                occ_line = v;
                occ_pos  = q;
                continue;                        /* occ_line is now authoritative */
            }
            occ_err("stray preprocessor directive reached the lexer", 0);
            while (occ_src[occ_pos] && occ_src[occ_pos] != '\n') occ_pos++;
            continue;
        }
        break;
    }
    char c = occ_src[occ_pos];
    if (!c) { occ_tk = T_EOF; occ_txt[0] = 0; return; }

    if (occ_isdigit(c)) {                       /* integer literal, dec or hex */
        i64 v = 0;
        if (c == '0' && (occ_src[occ_pos + 1] == 'x' || occ_src[occ_pos + 1] == 'X')) {
            occ_pos += 2;
            for (;;) {
                char h = occ_src[occ_pos];
                int d;
                if (occ_isdigit(h)) d = h - '0';
                else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
                else if (h >= 'A' && h <= 'F') d = h - 'A' + 10;
                else break;
                v = v * 16 + d; occ_pos++;
            }
        } else {
            while (occ_isdigit(occ_src[occ_pos])) { v = v * 10 + (occ_src[occ_pos] - '0'); occ_pos++; }
        }
        occ_val = v; occ_tk = T_NUM; return;
    }
    if (occ_isalpha(c)) {
        int n = 0;
        while (occ_isalnum(occ_src[occ_pos]) && n < OCC_NAMELEN - 1) occ_txt[n++] = occ_src[occ_pos++];
        while (occ_isalnum(occ_src[occ_pos])) occ_pos++;         /* over-long: truncate */
        occ_txt[n] = 0;
        occ_tk = occ_kw(occ_txt) ? T_KW : T_IDENT;
        return;
    }
    if (c == '\'') {                            /* character literal */
        occ_pos++;
        char v = occ_src[occ_pos++];
        if (v == '\\') {
            char e = occ_src[occ_pos++];
            v = e == 'n' ? '\n' : e == 't' ? '\t' : e == '0' ? '\0' :
                e == 'r' ? '\r' : e;
        }
        if (occ_src[occ_pos] == '\'') occ_pos++;
        occ_val = (i64)v; occ_tk = T_NUM; return;
    }
    if (c == '"') {
        occ_pos++;
        int n = 0;
        while (occ_src[occ_pos] && occ_src[occ_pos] != '"' && n < (int)sizeof occ_str - 1) {
            char v = occ_src[occ_pos++];
            if (v == '\\') {
                char e = occ_src[occ_pos++];
                v = e == 'n' ? '\n' : e == 't' ? '\t' : e == '0' ? '\0' :
                    e == 'r' ? '\r' : e;
            }
            occ_str[n++] = v;
        }
        if (occ_src[occ_pos] == '"') occ_pos++;
        occ_str[n] = 0; occ_strlen_ = n;
        occ_tk = T_STR; return;
    }
    /* punctuation, longest match first so >= does not lex as > then = */
    static const char *two[] = { "==", "!=", "<=", ">=", "&&", "||", "<<", ">>", "->", 0 };
    for (int i = 0; two[i]; i++)
        if (c == two[i][0] && occ_src[occ_pos + 1] == two[i][1]) {
            occ_txt[0] = two[i][0]; occ_txt[1] = two[i][1]; occ_txt[2] = 0;
            occ_pos += 2; occ_tk = T_PUNCT; return;
        }
    occ_txt[0] = c; occ_txt[1] = 0; occ_pos++; occ_tk = T_PUNCT;
}

static int occ_is(const char *s) {
    return (occ_tk == T_PUNCT || occ_tk == T_KW) && ostrneq(occ_txt, s, OCC_NAMELEN);
}
static int occ_accept(const char *s) { if (occ_is(s)) { occ_next(); return 1; } return 0; }
static void occ_expect(const char *s) {
    if (!occ_accept(s)) { occ_err("expected", s); if (occ_tk != T_EOF) occ_next(); }
}

/* ---- output buffers ------------------------------------------------------*/
static u8  *occ_text;  static int occ_tlen;
static u8  *occ_data;  static int occ_dlen;    /* globals (RW)            */
static u8  *occ_rod;   static int occ_rlen;    /* string literals (R)     */

static void occ_emit(u8 b) { if (occ_tlen < (int)OCC_MAXTEXT) occ_text[occ_tlen++] = b; }
static void occ_emit32(u32 v) { for (int i = 0; i < 4; i++) occ_emit((u8)(v >> (8 * i))); }
static void occ_emit64(u64 v) { for (int i = 0; i < 8; i++) occ_emit((u8)(v >> (8 * i))); }

/* ---- symbols -------------------------------------------------------------*/
struct occ_sym { char name[OCC_NAMELEN]; int kind; i64 addr; int defined;
                 int sidx, ptr, size; };   /* v0.57: type of a global variable */
/* kind: 0 = function, 1 = global variable */
static struct occ_sym occ_syms[OCC_MAXSYM];
static int occ_nsym;

struct occ_fix { int at; int sym; };            /* patch a call rel32 later */
static struct occ_fix occ_fixes[OCC_MAXFIX];
static int occ_nfix;

/* ===========================================================================
 * v0.57: TYPES — struct, union, typedef, and real memory layout
 * ===========================================================================
 * Until now occ had no type system whatsoever: `int` was 64-bit, every value
 * was a register, every local was one 8-byte slot, and `occ_is_type()` was a
 * three-way string compare that threw the answer away. Structs cannot be built
 * on that, because a struct is precisely a thing whose parts live at known
 * BYTE OFFSETS in memory.
 *
 * A type here is the triple (sidx, ptr, size):
 *     sidx >= 0, ptr == 0   a struct or union BY VALUE; size is its layout size
 *     sidx >= 0, ptr  > 0   a pointer to one; size 8
 *     sidx == -1            a scalar: size 1 for `char`, 8 for `int`/pointers
 *
 * LAYOUT is the real thing, not a simplification: each member is aligned up to
 * its own alignment, and the struct's total size is rounded up to the struct's
 * alignment so arrays of it stay aligned. `struct { char a; int b; }` therefore
 * puts a at 0, b at 8, and has size 16 — which is what the compilerstrs suite
 * checks, and it could not check anything meaningful if every member were
 * simply 8 bytes wide.
 *
 * A union places every member at offset 0 and takes the size of its largest.
 *
 * HONEST LIMITS: no bitfields, no anonymous members, no struct assignment or
 * struct arguments (a struct-typed expression evaluates to its ADDRESS, the
 * same rule C uses for arrays), and no forward references to a struct that has
 * not been defined yet. Members may be scalars, pointers, or nested structs
 * by value. */
#define OCC_MAXSTRUCT  32
#define OCC_MAXMEMBER  16
#define OCC_MAXTYPEDEF 32

struct occ_member { char name[OCC_NAMELEN]; int off, size, sidx, ptr; };
struct occ_struct {
    char name[OCC_NAMELEN];
    struct occ_member m[OCC_MAXMEMBER];
    int nm, size, align, is_union, used;
};
static struct occ_struct occ_structs[OCC_MAXSTRUCT];
static int occ_nstruct;

struct occ_td { char name[OCC_NAMELEN]; int sidx, ptr, size, used; };
static struct occ_td occ_tds[OCC_MAXTYPEDEF];
static int occ_ntd;

static int occ_struct_find(const char *n) {
    for (int i = 0; i < occ_nstruct; i++)
        if (occ_structs[i].used && ostrneq(occ_structs[i].name, n, OCC_NAMELEN)) return i;
    return -1;
}
static int occ_td_find(const char *n) {
    for (int i = 0; i < occ_ntd; i++)
        if (occ_tds[i].used && ostrneq(occ_tds[i].name, n, OCC_NAMELEN)) return i;
    return -1;
}
/* Alignment of a type: a struct's own alignment, else its size (1 or 8). */
static int occ_align_of(int sidx, int ptr, int size) {
    if (sidx >= 0 && ptr == 0) return occ_structs[sidx].align;
    (void)size;
    return ptr ? 8 : (size == 1 ? 1 : 8);
}

static struct occ_loc { char name[OCC_NAMELEN]; int off; int sidx, ptr, size; } occ_locs[OCC_MAXLOC];
static int occ_nloc, occ_frame;

static int occ_sym_find(const char *n) {
    for (int i = 0; i < occ_nsym; i++) if (ostrneq(occ_syms[i].name, n, OCC_NAMELEN)) return i;
    return -1;
}
static int occ_sym_get(const char *n, int kind) {
    int i = occ_sym_find(n);
    if (i >= 0) return i;
    if (occ_nsym >= OCC_MAXSYM) { occ_err("too many symbols", n); return 0; }
    i = occ_nsym++;
    for (int k = 0; k < OCC_NAMELEN; k++) occ_syms[i].name[k] = n[k] ? n[k] : 0;
    occ_syms[i].kind = kind; occ_syms[i].addr = 0; occ_syms[i].defined = 0;
    occ_syms[i].sidx = -1; occ_syms[i].ptr = 0; occ_syms[i].size = 8;
    return i;
}
static int occ_loc_find(const char *n) {
    for (int i = occ_nloc - 1; i >= 0; i--) if (ostrneq(occ_locs[i].name, n, OCC_NAMELEN)) return i;
    return -1;
}

/* ---- instruction helpers -------------------------------------------------
 * Values live in RAX. Binary operators evaluate the left side, push it, then
 * evaluate the right side into RDI and pop the left back into RAX — which is
 * why every operator below reads "op rax, rdi" and why subtraction and division
 * come out the right way round without a swap. */
static void occ_mov_rax_imm(u64 v)  { occ_emit(0x48); occ_emit(0xB8); occ_emit64(v); }
static void occ_push_rax(void)      { occ_emit(0x50); }
static void occ_pop_rax(void)       { occ_emit(0x58); }
static void occ_pop_rdi(void)       { occ_emit(0x5F); }
static void occ_mov_rdi_rax(void)   { occ_emit(0x48); occ_emit(0x89); occ_emit(0xC7); }
static void occ_load_rax_ind(void)  { occ_emit(0x48); occ_emit(0x8B); occ_emit(0x00); }  /* rax=[rax] */
static void occ_store_rdi_rax(void) { occ_emit(0x48); occ_emit(0x89); occ_emit(0x07); }  /* [rdi]=rax */
static void occ_test_rax(void)      { occ_emit(0x48); occ_emit(0x85); occ_emit(0xC0); }
static void occ_lea_local(int off)  { /* lea rax,[rbp+off] */
    occ_emit(0x48); occ_emit(0x8D); occ_emit(0x85); occ_emit32((u32)off);
}
/* v0.57: byte/qword access and offset arithmetic, for struct members. */
static void occ_add_rax_imm32(int v) {
    if (!v) return;
    occ_emit(0x48); occ_emit(0x05); occ_emit32((u32)v);      /* add rax, imm32   */
}
static void occ_load_rax_sized(int size) {
    if (size == 1) { occ_emit(0x48); occ_emit(0x0F); occ_emit(0xB6); occ_emit(0x00); }  /* movzx rax,[rax] */
    else           { occ_emit(0x48); occ_emit(0x8B); occ_emit(0x00); }                  /* mov rax,[rax]   */
}
/* value in rax, destination address in rdi */
static void occ_store_sized(int size) {
    if (size == 1) { occ_emit(0x88); occ_emit(0x07); }                                  /* mov [rdi],al    */
    else           { occ_emit(0x48); occ_emit(0x89); occ_emit(0x07); }                  /* mov [rdi],rax   */
}

static void occ_setcc(u8 cc) {       /* setcc al ; movzx rax, al */
    occ_emit(0x0F); occ_emit(cc); occ_emit(0xC0);
    occ_emit(0x48); occ_emit(0x0F); occ_emit(0xB6); occ_emit(0xC0);
}
static int  occ_jmp_fwd(u8 op) {     /* op = 0 for jmp, else jcc opcode2 */
    if (op) { occ_emit(0x0F); occ_emit(op); } else occ_emit(0xE9);
    int at = occ_tlen; occ_emit32(0);
    return at;
}
static void occ_patch(int at) {      /* make the jump at `at` land here */
    u32 rel = (u32)(occ_tlen - (at + 4));
    for (int i = 0; i < 4; i++) occ_text[at + i] = (u8)(rel >> (8 * i));
}

static void occ_expr(void);

/* ---- expressions ---------------------------------------------------------
 * occ_lvalue() leaves an ADDRESS in RAX; occ_primary() leaves a VALUE. Keeping
 * those two jobs separate is what makes `a = b`, `*p = v` and `a[i] = v` all
 * fall out of the same assignment path. */
/* v0.57: the SIZE of the object the last occ_lvalue() addressed, so the
 * assignment path can store 1 byte into a `char` member and 8 into an `int`
 * one. Without this every store was a qword and a char member would flatten
 * the seven bytes after it. */
static int occ_lv_size;

/* v0.57: walk a `.`/`->` chain, folding member offsets into the address already
 * in RAX. Called by both the lvalue and rvalue paths so the two can never
 * disagree about layout — which is exactly the sort of divergence that makes a
 * struct read fine and written wrong.
 *
 *   `.`  the base is an ADDRESS already (a struct-typed variable evaluates to
 *        its address, the rule C uses for arrays), so only the offset is added.
 *   `->` the base is a POINTER VALUE, which is also just an address in RAX, so
 *        the emitted code is identical. The difference is entirely in what the
 *        type check permits, and reporting that difference is what turns a
 *        misuse into a diagnostic instead of a wrong offset.
 *
 * Threads (sidx, ptr, size) in and out; the final size lands in occ_lv_size. */
static void occ_member_chain(int *sidx, int *ptr, int *size) {
    for (;;) {
        int arrow = occ_is("->");
        if (!arrow && !occ_is(".")) break;
        occ_next();
        if (occ_tk != T_IDENT) { occ_err("expected a member name after . or ->", occ_txt); return; }
        char mn[OCC_NAMELEN];
        for (int i = 0; i < OCC_NAMELEN; i++) mn[i] = occ_txt[i];

        if (*sidx < 0) { occ_err("not a struct or union", mn); occ_next(); return; }
        if (arrow && *ptr == 0) { occ_err("'->' on a non-pointer; use '.'", mn); occ_next(); return; }
        if (!arrow && *ptr != 0) { occ_err("'.' on a pointer; use '->'", mn); occ_next(); return; }

        struct occ_struct *st = &occ_structs[*sidx];
        int mi = -1;
        for (int i = 0; i < st->nm; i++) if (ostrneq(st->m[i].name, mn, OCC_NAMELEN)) { mi = i; break; }
        if (mi < 0) { occ_err("no such member", mn); occ_next(); return; }

        occ_add_rax_imm32(st->m[mi].off);
        *sidx = st->m[mi].sidx;
        *ptr  = st->m[mi].ptr;
        *size = st->m[mi].size;
        occ_next();

        /* A pointer member must be LOADED before it can be stepped through
         * again: `a->b->c` needs the value of b, not its address. */
        if (*ptr > 0 && (occ_is("->") || occ_is("."))) {
            occ_load_rax_sized(8);
            /* the loaded value is the pointer itself, so one level is consumed
             * by the load and the next `->` sees ptr == 1 again */
        }
    }
    occ_lv_size = *size;
}

static int occ_lvalue(void) {     /* returns 1 if it emitted an address */
    if (occ_tk != T_IDENT) return 0;
    /* look ahead: a bare identifier not followed by '(' or '[' is a variable */
    char nm[OCC_NAMELEN];
    for (int i = 0; i < OCC_NAMELEN; i++) nm[i] = occ_txt[i];
    int save_pos = occ_pos, save_tk = occ_tk, save_line = occ_line;
    i64 save_val = occ_val;
    occ_next();
    if (occ_is("(")) {                       /* it is a call, not an lvalue */
        occ_pos = save_pos; occ_tk = save_tk; occ_val = save_val; occ_line = save_line;
        for (int i = 0; i < OCC_NAMELEN; i++) occ_txt[i] = nm[i];
        return 0;
    }
    int sidx = -1, ptr = 0, size = 8;
    int li = occ_loc_find(nm);
    if (li >= 0) {
        occ_lea_local(occ_locs[li].off);
        sidx = occ_locs[li].sidx; ptr = occ_locs[li].ptr; size = occ_locs[li].size;
    } else {
        int si = occ_sym_get(nm, 1);
        occ_mov_rax_imm(OCC_DATA_BASE + (u64)occ_syms[si].addr);
        sidx = occ_syms[si].sidx; ptr = occ_syms[si].ptr; size = occ_syms[si].size;
    }
    occ_lv_size = size;
    if (occ_accept("[")) {                   /* a[i] -> *(a + i*8) */
        occ_load_rax_ind();                  /* the array/pointer value        */
        occ_push_rax();
        occ_expr();
        occ_emit(0x48); occ_emit(0xC1); occ_emit(0xE0); occ_emit(3);   /* shl rax,3 */
        occ_mov_rdi_rax(); occ_pop_rax();
        occ_emit(0x48); occ_emit(0x01); occ_emit(0xF8);                /* add rax,rdi */
        occ_expect("]");
        occ_lv_size = 8;
        return 1;
    }
    if (occ_is(".") || occ_is("->")) {
        /* RAX holds the variable's ADDRESS. For `.` that is already what the
         * chain wants. For `->` the POINTER VALUE is wanted, so load it. */
        if (occ_is("->")) occ_load_rax_sized(8);
        occ_member_chain(&sidx, &ptr, &size);
    }
    return 1;
}

static void occ_primary(void) {
    if (occ_tk == T_NUM) { occ_mov_rax_imm((u64)occ_val); occ_next(); return; }
    if (occ_tk == T_STR) {
        u64 addr = OCC_RODATA_BASE + (u64)occ_rlen;
        for (int i = 0; i <= occ_strlen_ && occ_rlen < (int)OCC_MAXDATA; i++)
            occ_rod[occ_rlen++] = (u8)occ_str[i];
        while (occ_rlen & 7) occ_rod[occ_rlen++] = 0;
        occ_mov_rax_imm(addr); occ_next(); return;
    }
    if (occ_accept("(")) { occ_expr(); occ_expect(")"); return; }
    if (occ_accept("-")) { occ_primary();
        occ_emit(0x48); occ_emit(0xF7); occ_emit(0xD8); return; }     /* neg rax */
    if (occ_accept("!")) { occ_primary(); occ_test_rax(); occ_setcc(0x94); return; }
    if (occ_accept("*")) { occ_primary(); occ_load_rax_ind(); return; }
    if (occ_accept("&")) { if (!occ_lvalue()) occ_err("& needs an lvalue", 0); return; }
    if (occ_is("__syscall")) {                /* the OS ABI intrinsic */
        occ_next(); occ_expect("(");
        /* Arguments are evaluated left to right and parked on the stack, then
         * loaded into the ABI registers at the end — evaluating straight into
         * rdi/rsi/rdx would have a later argument clobber an earlier one. */
        int n = 0;
        for (;;) {
            occ_expr(); occ_push_rax(); n++;
            if (!occ_accept(",")) break;
        }
        occ_expect(")");
        while (n < 4) { occ_mov_rax_imm(0); occ_push_rax(); n++; }
        occ_emit(0x5A);                                  /* pop rdx  (a2) */
        occ_emit(0x5E);                                  /* pop rsi  (a1) */
        occ_emit(0x5F);                                  /* pop rdi  (a0) */
        occ_emit(0x58);                                  /* pop rax  (num) */
        occ_emit(0x0F); occ_emit(0x05);                  /* syscall */
        return;
    }
    /* v0.56 Stage F: BYTE ADDRESSING. occ's `a[i]` is defined as *(a + i*8) —
     * every value is a machine word, which is the simplification the whole
     * code generator rests on. The consequence is that occ could not touch a
     * BYTE, and therefore could not walk a C string: a strlen() written with
     * s[n] reads eight bytes at a time and stops at the first NUL-containing
     * word. A "string.h" whose strlen cannot walk a string is a prop, so these
     * two intrinsics exist instead of a type system:
     *
     *     __ldb(p, i)      -> the unsigned byte at p[i]
     *     __stb(p, i, v)   -> stores the low byte of v at p[i], returns v
     *
     * They are builtins for the same reason __syscall is: they need no types,
     * no addressing modes and no new syntax, and they make the runtime in
     * /usr/lib/libc.oc genuinely implementable in occ's own subset. */
    if (occ_is("__ldb")) {
        occ_next(); occ_expect("(");
        occ_expr(); occ_push_rax();                      /* p */
        occ_expect(",");
        occ_expr();                                      /* i -> rax */
        occ_expect(")");
        occ_mov_rdi_rax();                               /* rdi = i        */
        occ_pop_rax();                                   /* rax = p        */
        occ_emit(0x48); occ_emit(0x0F); occ_emit(0xB6);
        occ_emit(0x04); occ_emit(0x38);                  /* movzx rax,[rax+rdi] */
        return;
    }
    if (occ_is("__stb")) {
        occ_next(); occ_expect("(");
        occ_expr(); occ_push_rax();                      /* p */
        occ_expect(",");
        occ_expr(); occ_push_rax();                      /* i */
        occ_expect(",");
        occ_expr();                                      /* v -> rax */
        occ_expect(")");
        occ_emit(0x48); occ_emit(0x89); occ_emit(0xC2);  /* mov rdx,rax (v) */
        occ_emit(0x5F);                                  /* pop rdi     (i) */
        occ_emit(0x58);                                  /* pop rax     (p) */
        occ_emit(0x88); occ_emit(0x14); occ_emit(0x38);  /* mov [rax+rdi],dl */
        occ_emit(0x48); occ_emit(0x89); occ_emit(0xD0);  /* mov rax,rdx     */
        return;
    }
    if (occ_tk == T_IDENT) {
        char nm[OCC_NAMELEN];
        for (int i = 0; i < OCC_NAMELEN; i++) nm[i] = occ_txt[i];
        occ_next();
        if (occ_accept("(")) {                            /* function call */
            int n = 0;
            if (!occ_is(")")) for (;;) {
                occ_expr(); occ_push_rax(); n++;
                if (!occ_accept(",")) break;
            }
            occ_expect(")");
            if (n > 6) { occ_err("more than 6 arguments", nm); n = 6; }
            /* Arguments were pushed left to right, so popping in REVERSE order
             * lands each one in its SysV register. Popping into rax and moving
             * across keeps one mapping table instead of six pop encodings. */
            for (int i = n - 1; i >= 0; i--) {
                occ_pop_rax();
                static const u8 lo[4] = { 0xC7, 0xC6, 0xC2, 0xC1 };  /* rdi rsi rdx rcx */
                if (i < 4) { occ_emit(0x48); occ_emit(0x89); occ_emit(lo[i]); }
                else { occ_emit(0x49); occ_emit(0x89); occ_emit(i == 4 ? 0xC0 : 0xC1); } /* r8/r9 */
            }
            int si = occ_sym_get(nm, 0);
            occ_emit(0xE8);
            if (occ_nfix < OCC_MAXFIX) { occ_fixes[occ_nfix].at = occ_tlen; occ_fixes[occ_nfix].sym = si; occ_nfix++; }
            occ_emit32(0);
            return;
        }
        int sidx = -1, ptr = 0, size = 8, isloc = 0;
        int li = occ_loc_find(nm);
        int si = -1;
        if (li >= 0) {
            occ_lea_local(occ_locs[li].off);
            sidx = occ_locs[li].sidx; ptr = occ_locs[li].ptr; size = occ_locs[li].size;
            isloc = 1;
        } else {
            si = occ_sym_get(nm, 1);
            occ_mov_rax_imm(OCC_DATA_BASE + (u64)occ_syms[si].addr);
            sidx = occ_syms[si].sidx; ptr = occ_syms[si].ptr; size = occ_syms[si].size;
        }
        (void)isloc;
        /* RAX now holds the variable's ADDRESS. */
        if (occ_is(".") || occ_is("->")) {
            if (occ_is("->")) occ_load_rax_sized(8);       /* want the pointer VALUE */
            occ_member_chain(&sidx, &ptr, &size);
            /* A member that is itself a struct stays an ADDRESS — there is no
             * way to hold a struct in a register, and C says the same. */
            if (!(sidx >= 0 && ptr == 0)) occ_load_rax_sized(size);
            return;
        }
        /* v0.57: a struct-typed variable evaluates to its ADDRESS, exactly as an
         * array does in C. Loading it would put the first eight bytes of the
         * struct in RAX and call it the value, which is meaningless. */
        if (sidx >= 0 && ptr == 0) return;
        occ_load_rax_sized(size);
        return;
    }
    occ_err("unexpected token", occ_txt);
    occ_next();
    occ_mov_rax_imm(0);
}

static void occ_postfix(void) {
    /* index on a value expression: p[i] */
    occ_primary();
    while (occ_is("[")) {
        occ_next();
        occ_push_rax();
        occ_expr();
        occ_emit(0x48); occ_emit(0xC1); occ_emit(0xE0); occ_emit(3);   /* shl rax,3 */
        occ_mov_rdi_rax(); occ_pop_rax();
        occ_emit(0x48); occ_emit(0x01); occ_emit(0xF8);                /* add rax,rdi */
        occ_load_rax_ind();
        occ_expect("]");
    }
}

static void occ_binop(int lvl);

static void occ_unary(void) { occ_postfix(); }

/* precedence climbing, C's order:
 *   0 = ||   1 = &&   2 = |   3 = ^   4 = &   5 = == !=
 *   6 = relational    7 = << >>       8 = + -  9 = * / %
 *
 * v0.56 Stage F: levels 2, 3, 4 and 7 are new. A systems compiler without
 * shifts and masks cannot express a byte-packing routine or a hex formatter,
 * and libc.oc's puthex was the immediate proof — it had to be written with
 * repeated division because `>>` did not exist. The renumbering below is
 * mechanical; the two-character tokens "<<" and ">>" also had to be added to
 * the lexer, or `a >> b` lexed as two separate '>' punctuators. */
static void occ_binop(int lvl) {
    if (lvl > 9) { occ_unary(); return; }
    occ_binop(lvl + 1);
    for (;;) {
        if (lvl == 0 && occ_is("||")) {
            occ_next();
            occ_test_rax();
            int t = occ_jmp_fwd(0x85);            /* jnz -> true */
            occ_binop(lvl + 1); occ_test_rax();
            int f = occ_jmp_fwd(0x84);            /* jz  -> false */
            occ_patch(t); occ_mov_rax_imm(1);
            int e = occ_jmp_fwd(0);
            occ_patch(f); occ_mov_rax_imm(0);
            occ_patch(e);
            continue;
        }
        if (lvl == 1 && occ_is("&&")) {
            occ_next();
            occ_test_rax();
            int f = occ_jmp_fwd(0x84);
            occ_binop(lvl + 1); occ_test_rax();
            int f2 = occ_jmp_fwd(0x84);
            occ_mov_rax_imm(1);
            int e = occ_jmp_fwd(0);
            occ_patch(f); occ_patch(f2); occ_mov_rax_imm(0);
            occ_patch(e);
            continue;
        }
        int cmp = 0; u8 cc = 0;
        if (lvl == 5 && occ_is("==")) { cc = 0x94; cmp = 1; }
        else if (lvl == 5 && occ_is("!=")) { cc = 0x95; cmp = 1; }
        else if (lvl == 6 && occ_is("<"))  { cc = 0x9C; cmp = 1; }
        else if (lvl == 6 && occ_is(">"))  { cc = 0x9F; cmp = 1; }
        else if (lvl == 6 && occ_is("<=")) { cc = 0x9E; cmp = 1; }
        else if (lvl == 6 && occ_is(">=")) { cc = 0x9D; cmp = 1; }
        if (cmp) {
            occ_next(); occ_push_rax(); occ_binop(lvl + 1);
            occ_mov_rdi_rax(); occ_pop_rax();
            occ_emit(0x48); occ_emit(0x39); occ_emit(0xF8);            /* cmp rax,rdi */
            occ_setcc(cc);
            continue;
        }
        /* bitwise: one shape, three opcodes (or/xor/and on rax, rdi) */
        int bop = 0; u8 bcode = 0;
        if      (lvl == 2 && occ_is("|")) { bcode = 0x09; bop = 1; }
        else if (lvl == 3 && occ_is("^")) { bcode = 0x31; bop = 1; }
        else if (lvl == 4 && occ_is("&")) { bcode = 0x21; bop = 1; }
        if (bop) {
            occ_next(); occ_push_rax(); occ_binop(lvl + 1);
            occ_mov_rdi_rax(); occ_pop_rax();
            occ_emit(0x48); occ_emit(bcode); occ_emit(0xF8);           /* op rax,rdi */
            continue;
        }
        /* shifts: the count has to be in cl, and cl is the ONLY place x86-64
         * will take it from, so the operand order here is forced. */
        if (lvl == 7 && (occ_is("<<") || occ_is(">>"))) {
            int right = occ_is(">>");
            occ_next(); occ_push_rax(); occ_binop(lvl + 1);
            occ_emit(0x48); occ_emit(0x89); occ_emit(0xC1);            /* mov rcx,rax */
            occ_pop_rax();
            occ_emit(0x48); occ_emit(0xD3); occ_emit(right ? 0xF8 : 0xE0); /* sar/shl rax,cl */
            continue;
        }
        if (lvl == 8 && (occ_is("+") || occ_is("-"))) {
            int sub = occ_is("-");
            occ_next(); occ_push_rax(); occ_binop(lvl + 1);
            occ_mov_rdi_rax(); occ_pop_rax();
            occ_emit(0x48); occ_emit(sub ? 0x29 : 0x01); occ_emit(0xF8);
            continue;
        }
        if (lvl == 9 && (occ_is("*") || occ_is("/") || occ_is("%"))) {
            int op = occ_is("*") ? 0 : occ_is("/") ? 1 : 2;
            occ_next(); occ_push_rax(); occ_binop(lvl + 1);
            occ_mov_rdi_rax(); occ_pop_rax();
            if (op == 0) { occ_emit(0x48); occ_emit(0x0F); occ_emit(0xAF); occ_emit(0xC7); }
            else {
                occ_emit(0x48); occ_emit(0x99);                        /* cqo        */
                occ_emit(0x48); occ_emit(0xF7); occ_emit(0xFF);        /* idiv rdi   */
                if (op == 2) { occ_emit(0x48); occ_emit(0x89); occ_emit(0xD0); } /* mov rax,rdx */
            }
            continue;
        }
        return;
    }
}

static void occ_expr(void) {
    /* assignment is right-associative and needs an lvalue, so it is tried
     * first with a full token-position rollback if this turns out to be an
     * ordinary expression instead. */
    int save_pos = occ_pos, save_tk = occ_tk, save_line = occ_line;
    i64 save_val = occ_val;
    char save_txt[OCC_NAMELEN];
    for (int i = 0; i < OCC_NAMELEN; i++) save_txt[i] = occ_txt[i];

    if (occ_tk == T_IDENT || occ_is("*")) {
        int deref = occ_accept("*");
        int ok = deref ? (occ_lvalue() ? 1 : (occ_primary(), 1)) : occ_lvalue();
        if (ok && occ_is("=")) {
            /* v0.57: capture the size BEFORE the right-hand side is parsed —
             * evaluating it can walk another member chain and overwrite
             * occ_lv_size, which would silently store the wrong width. */
            int dstsz = deref ? 8 : occ_lv_size;
            occ_next();
            occ_push_rax();                     /* the destination address */
            occ_expr();                         /* the value               */
            occ_pop_rdi();
            occ_store_sized(dstsz);
            return;
        }
        occ_pos = save_pos; occ_tk = save_tk; occ_val = save_val; occ_line = save_line;
        for (int i = 0; i < OCC_NAMELEN; i++) occ_txt[i] = save_txt[i];
    }
    occ_binop(0);
}

/* ---- types: specifiers, struct/union definitions, typedef -----------------*/

/* True if the current token can BEGIN a declaration. `struct`/`union` and any
 * name introduced by typedef now count, which is what lets a local or a member
 * be declared with a user-defined type. */
static int occ_is_type(void) {
    if (occ_is("int") || occ_is("char") || occ_is("void")) return 1;
    if (occ_is("struct") || occ_is("union")) return 1;
    if (occ_tk == T_IDENT && occ_td_find(occ_txt) >= 0) return 1;
    return 0;
}

static void occ_struct_body(int si);

/* Parse a type specifier plus any '*'s. Returns 1 on success and writes the
 * triple through the out-parameters. Also DEFINES the struct if the specifier
 * carries a `{ ... }` body, so `struct P { int x; } ;` and `struct P p;` are
 * both handled by the one function. */
static int occ_parse_type(int *sidx, int *ptr, int *size) {
    *sidx = -1; *ptr = 0; *size = 8;

    if (occ_is("struct") || occ_is("union")) {
        int is_union = occ_is("union");
        occ_next();
        char nm[OCC_NAMELEN]; nm[0] = 0;
        if (occ_tk == T_IDENT) { for (int i = 0; i < OCC_NAMELEN; i++) nm[i] = occ_txt[i]; occ_next(); }
        int si = nm[0] ? occ_struct_find(nm) : -1;
        if (occ_is("{")) {                       /* a definition */
            if (si < 0) {
                if (occ_nstruct >= OCC_MAXSTRUCT) { occ_err("too many struct types", nm); return 0; }
                si = occ_nstruct++;
                for (int i = 0; i < OCC_NAMELEN; i++) occ_structs[si].name[i] = nm[i];
                occ_structs[si].used = 1;
            }
            /* v0.57: a shared header is legitimately pasted into EVERY unit,
             * because each unit gets a fresh macro table and therefore a fresh
             * include guard. So the same struct really is defined more than
             * once in one translation unit, and refusing that outright would
             * make shared headers unusable across units.
             *
             * Redefinition is accepted only if the layout comes out IDENTICAL.
             * That keeps the common case working while still catching the case
             * that actually hurts: two different definitions of one name, where
             * whichever parsed last would silently decide every member offset
             * for code compiled against the other. */
            int redef = occ_structs[si].nm > 0;
            struct occ_struct prev;
            if (redef) prev = occ_structs[si];
            occ_structs[si].is_union = is_union;
            occ_struct_body(si);
            if (redef) {
                struct occ_struct *now = &occ_structs[si];
                int same = (now->nm == prev.nm && now->size == prev.size &&
                            now->align == prev.align && now->is_union == prev.is_union);
                for (int i = 0; same && i < now->nm; i++)
                    if (now->m[i].off != prev.m[i].off || now->m[i].size != prev.m[i].size ||
                        now->m[i].ptr != prev.m[i].ptr || now->m[i].sidx != prev.m[i].sidx ||
                        !ostrneq(now->m[i].name, prev.m[i].name, OCC_NAMELEN)) same = 0;
                if (!same) occ_err("struct redefined with a DIFFERENT layout", now->name);
            }
        } else if (si < 0) {
            occ_err("unknown struct or union", nm[0] ? nm : "<anonymous>");
            return 0;
        }
        *sidx = si;
        *size = occ_structs[si].size;
    } else if (occ_is("int") || occ_is("void")) {
        occ_next(); *size = 8;
    } else if (occ_is("char")) {
        occ_next(); *size = 1;
    } else if (occ_tk == T_IDENT) {
        int ti = occ_td_find(occ_txt);
        if (ti < 0) return 0;
        *sidx = occ_tds[ti].sidx; *ptr = occ_tds[ti].ptr; *size = occ_tds[ti].size;
        occ_next();
    } else return 0;

    /* Deliberately does NOT consume '*'. A pointer level belongs to the
     * DECLARATOR, not the specifier: in `int *a, b;` only a is a pointer.
     * Consuming here also double-counted, because every declarator loop that
     * calls this already consumes its own stars — `struct P *p;` came out as
     * pointer-to-pointer. */
    return 1;
}

/* `{ member; member; ... }` — computes offsets, alignment and total size. */
static void occ_struct_body(int si) {
    occ_expect("{");
    struct occ_struct *st = &occ_structs[si];
    st->nm = 0; st->size = 0; st->align = 1;
    int off = 0;
    while (!occ_is("}") && occ_tk != T_EOF && !occ_errors) {
        int msi, mptr, msz;
        if (!occ_parse_type(&msi, &mptr, &msz)) { occ_err("expected a member type", occ_txt); occ_next(); continue; }
        for (;;) {                                /* `int x, y;` */
            int xptr = mptr, xsz = msz;
            while (occ_accept("*")) { xptr++; xsz = 8; }
            if (occ_tk != T_IDENT) { occ_err("expected a member name", occ_txt); break; }
            if (st->nm >= OCC_MAXMEMBER) { occ_err("too many members in", st->name); }
            else {
                struct occ_member *mm = &st->m[st->nm];
                for (int i = 0; i < OCC_NAMELEN; i++) mm->name[i] = occ_txt[i];
                int a = occ_align_of(msi, xptr, xsz);
                if (a < 1) a = 1;
                if (st->is_union) { mm->off = 0; }
                else { off = (off + a - 1) / a * a; mm->off = off; off += xsz; }
                /* sidx is the member's TARGET struct in both cases: for a
                 * by-value member it is the member's own type, and for a
                 * pointer member it is what `->` will dereference to. `ptr`
                 * distinguishes them. */
                mm->size = xsz; mm->sidx = msi; mm->ptr = xptr;
                if (a > st->align) st->align = a;
                if (st->is_union && xsz > st->size) st->size = xsz;
                st->nm++;
            }
            occ_next();
            if (!occ_accept(",")) break;
        }
        occ_expect(";");
    }
    occ_expect("}");
    if (!st->is_union) st->size = off;
    /* Round the total up to the struct's own alignment so an array of it, and
     * a following member of the same type, both stay aligned. */
    if (st->align > 1) st->size = (st->size + st->align - 1) / st->align * st->align;
    if (st->size == 0) st->size = 1;
}

/* ---- statements ----------------------------------------------------------*/

static void occ_stmt(void) {
    if (occ_accept("{")) { while (!occ_is("}") && occ_tk != T_EOF) occ_stmt(); occ_expect("}"); return; }

    if (occ_is_type()) {                         /* local declaration */
        int sidx, ptr, size;
        if (!occ_parse_type(&sidx, &ptr, &size)) { occ_err("expected a type", occ_txt); occ_next(); return; }
        /* `struct P { int x; };` inside a function defines the type and
         * declares nothing — allow it rather than demanding a declarator. */
        if (occ_accept(";")) return;
        for (;;) {
            int xptr = ptr, xsz = size;
            while (occ_accept("*")) { xptr++; xsz = 8; }
            if (occ_tk != T_IDENT) { occ_err("expected a declarator", occ_txt); return; }
            if (occ_nloc >= OCC_MAXLOC) occ_err("too many locals in one function", occ_txt);
            else {
                struct occ_loc *L = &occ_locs[occ_nloc];
                for (int i = 0; i < OCC_NAMELEN; i++) L->name[i] = occ_txt[i];
                /* v0.57: a struct local occupies its REAL size, rounded up to 8
                 * so the frame stays qword-aligned. Every local was one 8-byte
                 * slot before this, which is exactly why structs needed it. */
                int slotsz = (xsz + 7) & ~7;
                occ_frame += slotsz;
                L->off  = -occ_frame;
                L->sidx = (xptr ? sidx : sidx);
                L->ptr  = xptr;
                L->size = xsz;
                occ_nloc++;
            }
            int slot = occ_nloc - 1;
            occ_next();
            if (occ_accept("=")) {
                if (slot >= 0 && occ_locs[slot].sidx >= 0 && occ_locs[slot].ptr == 0)
                    occ_err("cannot initialise a struct by assignment", occ_locs[slot].name);
                occ_expr();
                occ_emit(0x48); occ_emit(0x89); occ_emit(0x85); occ_emit32((u32)occ_locs[slot].off);
            }
            if (!occ_accept(",")) break;
        }
        occ_expect(";");
        return;
    }
    if (occ_accept("return")) {
        if (!occ_is(";")) occ_expr(); else occ_mov_rax_imm(0);
        occ_expect(";");
        occ_emit(0xC9);                          /* leave */
        occ_emit(0xC3);                          /* ret   */
        return;
    }
    if (occ_accept("if")) {
        occ_expect("("); occ_expr(); occ_expect(")");
        occ_test_rax();
        int f = occ_jmp_fwd(0x84);
        occ_stmt();
        if (occ_accept("else")) {
            int e = occ_jmp_fwd(0);
            occ_patch(f);
            occ_stmt();
            occ_patch(e);
        } else occ_patch(f);
        return;
    }
    if (occ_accept("while")) {
        int top = occ_tlen;
        occ_expect("("); occ_expr(); occ_expect(")");
        occ_test_rax();
        int f = occ_jmp_fwd(0x84);
        occ_stmt();
        occ_emit(0xE9); occ_emit32((u32)(top - (occ_tlen + 4)));
        occ_patch(f);
        return;
    }
    if (occ_accept("for")) {
        occ_expect("(");
        if (!occ_is(";")) occ_expr();
        occ_expect(";");
        int top = occ_tlen;
        int f = -1;
        if (!occ_is(";")) { occ_expr(); occ_test_rax(); f = occ_jmp_fwd(0x84); }
        occ_expect(";");
        /* The increment textually precedes the body but must RUN after it, so
         * it is emitted here and jumped over, with the body jumping back to it.
         * That is the standard single-pass shape for `for` without an AST. */
        int jbody = occ_jmp_fwd(0);
        int inc = occ_tlen;
        if (!occ_is(")")) occ_expr();
        occ_emit(0xE9); occ_emit32((u32)(top - (occ_tlen + 4)));
        occ_expect(")");
        occ_patch(jbody);
        occ_stmt();
        occ_emit(0xE9); occ_emit32((u32)(inc - (occ_tlen + 4)));
        if (f >= 0) occ_patch(f);
        return;
    }
    if (occ_accept(";")) return;
    occ_expr();
    occ_expect(";");
}

/* ---- top level -----------------------------------------------------------*/
static void occ_toplevel(void) {
    while (occ_tk != T_EOF && !occ_errors) {
        /* v0.57: typedef. The alias records the full triple, so a later
         * `Pt p;` declares a struct with the right size and `Pt *q;` a
         * pointer to it. */
        if (occ_accept("typedef")) {
            int tsidx, tptr, tsize;
            if (!occ_parse_type(&tsidx, &tptr, &tsize)) {
                occ_err("expected a type after typedef", occ_txt); occ_next(); continue;
            }
            while (occ_accept("*")) { tptr++; tsize = 8; }
            if (occ_tk != T_IDENT) { occ_err("expected a typedef name", occ_txt); occ_next(); continue; }
            if (occ_ntd >= OCC_MAXTYPEDEF) occ_err("too many typedefs", occ_txt);
            else {
                struct occ_td *T = &occ_tds[occ_ntd++];
                for (int i = 0; i < OCC_NAMELEN; i++) T->name[i] = occ_txt[i];
                T->sidx = tsidx; T->ptr = tptr; T->size = tsize; T->used = 1;
            }
            occ_next();
            occ_expect(";");
            continue;
        }

        if (!occ_is_type()) { occ_err("expected a declaration", occ_txt); occ_next(); continue; }
        int bsidx, bptr, bsize;
        if (!occ_parse_type(&bsidx, &bptr, &bsize)) {
            occ_err("expected a type", occ_txt); occ_next(); continue;
        }
        /* `struct P { int x; };` — a definition with no declarator. */
        if (occ_accept(";")) continue;

        int dptr = bptr, dsize = bsize;
        while (occ_accept("*")) { dptr++; dsize = 8; }
        if (occ_tk != T_IDENT) { occ_err("expected a name", occ_txt); occ_next(); continue; }
        char nm[OCC_NAMELEN];
        for (int i = 0; i < OCC_NAMELEN; i++) nm[i] = occ_txt[i];
        occ_next();

        if (occ_accept("(")) {                                   /* function */
            int si = occ_sym_get(nm, 0);
            occ_nloc = 0; occ_frame = 0;

            /* parameters become the first locals */
            int np = 0;
            if (!occ_is(")")) for (;;) {
                /* v0.57: parameters go through the real type parser so a
                 * `struct P *p` parameter carries its target type and `p->x`
                 * inside the body knows the layout. A parameter is always
                 * passed in a register, so its slot stays one word wide even
                 * when its type names a struct — only POINTERS to structs can
                 * be parameters here, which the by-value check below enforces
                 * instead of silently truncating. */
                int psidx = -1, pptr = 0, psize = 8;
                if (occ_is_type()) {
                    if (!occ_parse_type(&psidx, &pptr, &psize)) { occ_err("bad parameter type", occ_txt); occ_next(); }
                }
                while (occ_accept("*")) { pptr++; psize = 8; }
                if (occ_tk == T_IDENT) {
                    if (psidx >= 0 && pptr == 0)
                        occ_err("a struct cannot be passed by value; pass a pointer", occ_txt);
                    if (occ_nloc < OCC_MAXLOC) {
                        struct occ_loc *L = &occ_locs[occ_nloc];
                        for (int i = 0; i < OCC_NAMELEN; i++) L->name[i] = occ_txt[i];
                        occ_frame += 8; L->off = -occ_frame;
                        L->sidx = psidx; L->ptr = pptr; L->size = psize;
                        occ_nloc++;
                    }
                    np++;
                    occ_next();
                }
                if (!occ_accept(",")) break;
            }
            occ_expect(")");

            /* v0.57: a PROTOTYPE — `int strlen(char *s);` — ends here with a
             * semicolon and emits nothing. This became mandatory the moment
             * #include started pasting real header text into the unit: every
             * declaration in /usr/include/string.h would otherwise be parsed as
             * a function DEFINITION whose body turned out to be a ';'.
             *
             * The symbol is registered but deliberately left `defined = 0`, so
             * the existing fixup pass still reports "undefined function" for
             * anything declared and never defined — a prototype is a promise,
             * not an implementation, and occ has no linker to satisfy it from
             * somewhere else. Nothing was emitted while parsing the parameter
             * list, so unwinding here is just resetting the frame bookkeeping. */
            if (occ_accept(";")) { occ_nloc = 0; occ_frame = 0; continue; }

            /* v0.57: with several inputs fused into one unit, a name defined
             * twice used to overwrite the first symbol's address in silence,
             * and every call to it went to whichever unit was parsed last. */
            if (occ_syms[si].defined) occ_err("function already defined", nm);
            occ_syms[si].addr = occ_tlen;
            occ_syms[si].defined = 1;

            occ_emit(0x55);                                      /* push rbp        */
            occ_emit(0x48); occ_emit(0x89); occ_emit(0xE5);      /* mov rbp,rsp     */
            occ_emit(0x48); occ_emit(0x81); occ_emit(0xEC);      /* sub rsp, imm32  */
            int frame_at = occ_tlen; occ_emit32(0);              /* patched below   */

            /* spill the incoming register arguments into their slots */
            static const u8 sr[6][3] = {
                { 0x48, 0x89, 0xBD }, { 0x48, 0x89, 0xB5 }, { 0x48, 0x89, 0x95 },
                { 0x48, 0x89, 0x8D }, { 0x4C, 0x89, 0x85 }, { 0x4C, 0x89, 0x8D },
            };
            for (int i = 0; i < np && i < 6; i++) {
                occ_emit(sr[i][0]); occ_emit(sr[i][1]); occ_emit(sr[i][2]);
                occ_emit32((u32)occ_locs[i].off);
            }

            occ_expect("{");
            while (!occ_is("}") && occ_tk != T_EOF) occ_stmt();
            occ_expect("}");

            occ_mov_rax_imm(0);                                  /* implicit return 0 */
            occ_emit(0xC9); occ_emit(0xC3);

            /* the frame is only known now: 16-byte aligned, as SysV requires */
            u32 fsz = (u32)((occ_frame + 15) & ~15);
            for (int i = 0; i < 4; i++) occ_text[frame_at + i] = (u8)(fsz >> (8 * i));
            continue;
        }

        /* global variable (optionally with a constant initialiser) */
        int si = occ_sym_get(nm, 1);
        /* v0.57: align the global to its type before placing it, or a struct
         * whose alignment is 8 could start at an odd offset and every member
         * offset computed from it would be misaligned. */
        { int ga = occ_align_of(bsidx, dptr, dsize);
          if (ga > 1) while (occ_dlen % ga) { if (occ_dlen < (int)OCC_MAXDATA) occ_data[occ_dlen++] = 0; else break; } }
        /* Same hazard as a duplicate function, with a worse failure mode: the
         * second definition would move the symbol's address to freshly reserved
         * storage, so writes through the old name and the new one would land in
         * different places and neither would see the other's value. */
        if (occ_syms[si].defined) occ_err("global already defined", nm);
        occ_syms[si].addr = occ_dlen;
        occ_syms[si].defined = 1;
        occ_syms[si].sidx = bsidx; occ_syms[si].ptr = dptr; occ_syms[si].size = dsize;
        i64 init = 0;
        int cells = 1;
        if (occ_accept("[")) { if (occ_tk == T_NUM) { cells = (int)occ_val; occ_next(); } occ_expect("]"); }
        if (occ_accept("=")) { if (occ_tk == T_NUM) { init = occ_val; occ_next(); }
                               else { occ_err("global initialiser must be a constant", nm); occ_next(); } }
        /* v0.57: reserve the type's REAL size per cell. An array of a 24-byte
         * struct needs 24 bytes per element, not 8 — the old code assumed every
         * global cell was one machine word, which is true only for scalars. */
        { int cellsz = (dsize < 8) ? 8 : dsize;      /* scalars keep a full word */
          if (bsidx >= 0 && dptr == 0) cellsz = occ_structs[bsidx].size;
          for (int c = 0; c < cells; c++)
              for (int i = 0; i < cellsz && occ_dlen < (int)OCC_MAXDATA; i++)
                  occ_data[occ_dlen++] = (u8)((i < 8 && !c) ? ((u64)init >> (8 * i)) : 0); }
        occ_expect(";");
    }
}

/* ---- ELF writer ----------------------------------------------------------
 * Three PT_LOADs, exactly like user.ld produces for the boot image: text R+X,
 * rodata R, data R+W. That is not decoration — the kernel's elf_load REFUSES a
 * segment that is both writable and executable, so a compiler that merged them
 * would produce binaries this OS will not run. */
struct occ_eh { u8 ident[16]; u16 type, machine; u32 version; u64 entry, phoff, shoff;
                u32 flags; u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx; };
struct occ_ph { u32 type, flags; u64 offset, vaddr, paddr, filesz, memsz, align; };

static int occ_write_elf(u8 *out, int cap, u64 entry) {
    int ehs = (int)sizeof(struct occ_eh), phs = (int)sizeof(struct occ_ph);
    int hdr = ehs + 3 * phs;
    int off_text = (hdr + 15) & ~15;
    int off_rod  = off_text + occ_tlen;
    int off_data = off_rod + occ_rlen;
    int total    = off_data + occ_dlen;
    if (total > cap) return -1;
    for (int i = 0; i < total; i++) out[i] = 0;

    struct occ_eh *e = (struct occ_eh *)out;
    e->ident[0] = 0x7F; e->ident[1] = 'E'; e->ident[2] = 'L'; e->ident[3] = 'F';
    e->ident[4] = 2;    /* 64-bit      */
    e->ident[5] = 1;    /* little end  */
    e->ident[6] = 1;    /* EI_VERSION  */
    e->type = 2;        /* ET_EXEC     */
    e->machine = 0x3E;  /* x86-64      */
    e->version = 1;
    e->entry = entry;
    e->phoff = (u64)ehs;
    e->ehsize = (u16)ehs;
    e->phentsize = (u16)phs;
    e->phnum = 3;

    struct occ_ph *p = (struct occ_ph *)(out + ehs);
    p[0].type = 1; p[0].flags = 5;                 /* R + X */
    p[0].offset = (u64)off_text; p[0].vaddr = p[0].paddr = OCC_TEXT_BASE;
    p[0].filesz = p[0].memsz = (u64)occ_tlen; p[0].align = 0x1000;

    p[1].type = 1; p[1].flags = 4;                 /* R     */
    p[1].offset = (u64)off_rod; p[1].vaddr = p[1].paddr = OCC_RODATA_BASE;
    p[1].filesz = p[1].memsz = (u64)occ_rlen; p[1].align = 0x1000;

    p[2].type = 1; p[2].flags = 6;                 /* R + W */
    p[2].offset = (u64)off_data; p[2].vaddr = p[2].paddr = OCC_DATA_BASE;
    p[2].filesz = p[2].memsz = (u64)occ_dlen; p[2].align = 0x1000;

    for (int i = 0; i < occ_tlen; i++) out[off_text + i] = occ_text[i];
    for (int i = 0; i < occ_rlen; i++) out[off_rod  + i] = occ_rod[i];
    for (int i = 0; i < occ_dlen; i++) out[off_data + i] = occ_data[i];
    return total;
}

/* ---- driver --------------------------------------------------------------
 * occ_compile(srcs, nsrc, outpath) -> 0 on success, negative on failure.
 *
 * v0.57: MULTI-UNIT. Every input file is preprocessed, in order, into ONE
 * translation unit, and then a single code-generation pass runs over the whole
 * thing. That is not a shortcut around linking — it IS occ's linkage model, the
 * same one /usr/lib/libc.oc has always used, and it is the only one available
 * to a compiler with no relocations, no symbol table in its output, and no
 * linker. Cross-unit references resolve through the existing forward-call fixup
 * table, so a function in the second file may call one in the third.
 *
 * WHAT THIS IS NOT: separate compilation. There are no object files, nothing is
 * compiled independently, and a change to any input recompiles everything. Two
 * consequences are visible to a user and are handled rather than hidden:
 *
 *   - Each input gets a FRESH MACRO TABLE. In real C a #define in a.c cannot
 *     reach b.c, and concatenating the files naively would let it. Resetting
 *     per unit costs re-pasting the headers each unit includes — harmless,
 *     since a prototype emits no code — and buys the semantics a user expects.
 *     The prelude is preprocessed once, before any of them; it defines
 *     functions, not macros, so nothing leaks from it either.
 *   - A name DEFINED in two units is a hard error now, reported with the file
 *     and line of the duplicate. Without that check the second definition
 *     silently overwrote the first symbol's address and calls went to whichever
 *     unit happened to be parsed last. */
static int occ_compile(const char **srcs, int nsrc, const char *outpath) {
    occ_errors = 0; occ_line = 1; occ_pos = 0;
    occ_tlen = occ_dlen = occ_rlen = 0;
    occ_nsym = occ_nfix = occ_nloc = 0; occ_frame = 0;

    occ_src  = (char *)omalloc(OCC_MAXSRC);
    occ_text = (u8 *)omalloc(OCC_MAXTEXT);
    occ_data = (u8 *)omalloc(OCC_MAXDATA);
    occ_rod  = (u8 *)omalloc(OCC_MAXDATA);
    if (!occ_src || !occ_text || !occ_data || !occ_rod) { occ_err("out of memory", 0); return -1; }

    /* v0.57: THE PREPROCESSOR BUILDS THE TRANSLATION UNIT.
     *
     * v0.56 concatenated /usr/lib/libc.oc and the user's file into one buffer
     * by hand and skipped every '#' line. That was the honest thing to do with
     * no preprocessor, but it could not survive #include: line numbers were
     * corrected by subtracting a fixed prelude length, which is meaningless
     * once a third and fourth file can be pasted in at arbitrary points.
     *
     * Now both go through occ_pp_file(), which emits `#line N FILE` markers the
     * lexer consumes — so the prelude, every header, and the user's own code
     * all report their own real names and line numbers. The prelude is still
     * FIRST, for the same single-pass reason as before: definitions must be
     * emitted before the code that calls them, or every call needs a fixup.
     *
     * The prelude is included as source, not linked, because occ still has no
     * linker. That has not changed and is not being implied away. */
    occ_ppout = occ_src; occ_ppn = 0; occ_ppcap = (int)OCC_MAXSRC;
    occ_pp_depth = 0; occ_pp_includes = 0;
    occ_nmacro = 0;
    for (int i = 0; i < OCC_MAXMACRO; i++) { occ_macros[i].used = 0; occ_macros[i].expanding = 0; }

    /* These are existence probes, and they must CLOSE what they open —
     * occ_pp_file opens the file itself. Leaking a descriptor per probe would
     * burn two of the sixteen global kernel descriptors on every compile. */
    { int t = oopen("/usr/lib/libc.oc");
      if (t >= 0) { oclose(t); occ_pp_file("/usr/lib/libc.oc"); }
      else occ_note("no /usr/lib/libc.oc - compiling with no runtime;",
                    "calls to strlen/puts/malloc will be undefined"); }

    for (int u = 0; u < nsrc; u++) {
        int t = oopen(srcs[u]);
        if (t < 0) { occ_err("cannot open source", srcs[u]); return -2; }
        oclose(t);
        /* Fresh macro table per unit — see the note above. */
        occ_nmacro = 0;
        for (int i = 0; i < OCC_MAXMACRO; i++) { occ_macros[i].used = 0; occ_macros[i].expanding = 0; }
        occ_pp_file(srcs[u]);
    }
    occ_src[occ_ppn] = 0;
    if (occ_errors) return -3;
    if (nsrc > 1) {
        char b[32]; int n = 0, d[8], k = 0, v = nsrc;
        while (v) { d[k++] = v % 10; v /= 10; }
        while (k) b[n++] = (char)('0' + d[--k]);
        b[n] = 0;
        occ_note(b, "input files compiled as one translation unit");
    }

    /* A tiny entry stub is emitted FIRST so the ELF entry point is offset 0:
     * it calls main and turns the return value into SYS_EXIT. This is the crt0
     * of a compiled program, generated rather than linked. */
    int main_sym = occ_sym_get("main", 0);
    occ_emit(0xE8);
    int entry_fix = occ_tlen; occ_emit32(0);
    occ_mov_rdi_rax();                                    /* exit status = main's return */
    occ_emit(0x48); occ_emit(0xC7); occ_emit(0xC0); occ_emit32(2);   /* mov rax,2 (SYS_EXIT) */
    occ_emit(0x0F); occ_emit(0x05);                       /* syscall */
    occ_emit(0xEB); occ_emit(0xFE);                       /* jmp . (unreachable) */

    occ_next();
    occ_toplevel();

    /* With several inputs, main() may be defined in any of them — so the
     * message names the OUTPUT being built rather than one arbitrary input. */
    if (!occ_syms[main_sym].defined) occ_err("no main() in any input file, building", outpath);

    /* resolve the entry stub's call and every forward call */
    if (occ_syms[main_sym].defined) {
        u32 rel = (u32)((int)occ_syms[main_sym].addr - (entry_fix + 4));
        for (int i = 0; i < 4; i++) occ_text[entry_fix + i] = (u8)(rel >> (8 * i));
    }
    for (int i = 0; i < occ_nfix; i++) {
        struct occ_sym *s = &occ_syms[occ_fixes[i].sym];
        if (!s->defined) { occ_err("undefined function", s->name); continue; }
        u32 rel = (u32)((int)s->addr - (occ_fixes[i].at + 4));
        for (int k = 0; k < 4; k++) occ_text[occ_fixes[i].at + k] = (u8)(rel >> (8 * k));
    }
    if (occ_errors) {
        ofree(occ_src); ofree(occ_text); ofree(occ_data); ofree(occ_rod);
        return -3;
    }

    u8 *img = (u8 *)omalloc(OCC_MAXTEXT + 2 * OCC_MAXDATA + 4096);
    if (!img) { occ_err("out of memory for the output image", 0); return -1; }
    int len = occ_write_elf(img, (int)(OCC_MAXTEXT + 2 * OCC_MAXDATA + 4096), OCC_TEXT_BASE);
    if (len < 0) { occ_err("output image too large", outpath); ofree(img); return -4; }

    int ofd = ocreat(outpath);   /* the compiler AUTHORS its output */
    if (ofd < 0) { occ_err("cannot open output", outpath); ofree(img); return -5; }
    i64 w = owrite(ofd, (const char *)img, (u64)len);
    oclose(ofd);
    ofree(img);
    ofree(occ_src); ofree(occ_text); ofree(occ_data); ofree(occ_rod);
    if (w != (i64)len) {
        /* "short write" alone is not actionable: whether the image is too big,
         * the filesystem refused it, or the descriptor went bad are three
         * different faults with the same symptom. Report all of it. */
        char b[160]; int bn = 0;
        const char *pre = "short write: len=";
        while (*pre) b[bn++] = *pre++;
        struct { const char *lbl; i64 v; } f[] = {
            { 0, (i64)len }, { " wrote=", w }, { " text=", occ_tlen },
            { " rodata=", occ_rlen }, { " data=", occ_dlen },
        };
        for (int k = 0; k < 5; k++) {
            if (f[k].lbl) { const char *q = f[k].lbl; while (*q) b[bn++] = *q++; }
            i64 v = f[k].v; if (v < 0) { b[bn++] = '-'; v = -v; }
            int d[20], dk = 0; if (!v) d[dk++] = 0;
            while (v) { d[dk++] = (int)(v % 10); v /= 10; }
            while (dk) b[bn++] = (char)('0' + d[--dk]);
        }
        b[bn] = 0;
        occ_err(b, outpath);
        return -6;
    }
    return 0;
}
