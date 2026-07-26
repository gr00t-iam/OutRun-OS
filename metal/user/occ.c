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

#define OCC_MAXSRC   (48u * 1024u)
#define OCC_MAXTEXT  (48u * 1024u)
#define OCC_MAXDATA  (8u * 1024u)
#define OCC_MAXSYM   96
#define OCC_MAXFIX   256
#define OCC_MAXLOC   32
#define OCC_NAMELEN  32

/* ---- diagnostics ----------------------------------------------------------
 * Everything goes to stderr through the std fd table, so a compile error shows
 * up in the Cyber-Terminal exactly like any other program's output. */
static int   occ_errors;
static int   occ_line;
/* v0.56 Stage F: how many lines of /usr/lib/libc.oc were prepended to this
 * translation unit. Diagnostics subtract it so a user gets THEIR line number,
 * and errors that genuinely land inside the runtime say so instead of quoting
 * a line number the user cannot find in their own file. */
static int   occ_prelude_lines;

static void occ_err(const char *msg, const char *what) {
    occ_errors++;
    char b[224]; int n = 0;
    int in_prelude = (occ_line <= occ_prelude_lines);
    int shown = in_prelude ? occ_line : occ_line - occ_prelude_lines;
    const char *p = in_prelude ? "occ: /usr/lib/libc.oc line " : "occ: line ";
    while (*p) b[n++] = *p++;
    /* line number, decimal */
    { int v = shown, d[8], k = 0; if (!v) d[k++] = 0; while (v) { d[k++] = v % 10; v /= 10; }
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

/* ---- lexer ---------------------------------------------------------------*/
enum { T_EOF = 0, T_NUM, T_IDENT, T_STR, T_PUNCT, T_KW };

static char *occ_src;
static int   occ_pos;
static int   occ_tk;                 /* current token kind      */
static i64   occ_val;                /* T_NUM value             */
static char  occ_txt[OCC_NAMELEN];   /* T_IDENT / T_KW / T_PUNCT spelling */
static char  occ_str[256];           /* T_STR contents          */
static int   occ_strlen_;

static int occ_isalpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int occ_isdigit(char c) { return c >= '0' && c <= '9'; }
static int occ_isalnum(char c) { return occ_isalpha(c) || occ_isdigit(c); }

static int occ_kw(const char *s) {
    static const char *kws[] = { "int", "char", "return", "if", "else", "while",
                                 "for", "void", "__syscall", "__ldb", "__stb", 0 };
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
        /* A #include or any other directive is SKIPPED, not processed: occ has
         * no preprocessor, and silently ignoring the line is more useful than
         * failing, because the SDK headers exist to be read by humans and by a
         * future front end. Declarations still have to appear in the file. */
        if (c == '#') { while (occ_src[occ_pos] && occ_src[occ_pos] != '\n') occ_pos++; continue; }
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
    static const char *two[] = { "==", "!=", "<=", ">=", "&&", "||", "<<", ">>", 0 };
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
struct occ_sym { char name[OCC_NAMELEN]; int kind; i64 addr; int defined; };
/* kind: 0 = function, 1 = global variable */
static struct occ_sym occ_syms[OCC_MAXSYM];
static int occ_nsym;

struct occ_fix { int at; int sym; };            /* patch a call rel32 later */
static struct occ_fix occ_fixes[OCC_MAXFIX];
static int occ_nfix;

static struct occ_loc { char name[OCC_NAMELEN]; int off; } occ_locs[OCC_MAXLOC];
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
    int li = occ_loc_find(nm);
    if (li >= 0) occ_lea_local(occ_locs[li].off);
    else {
        int si = occ_sym_get(nm, 1);
        occ_mov_rax_imm(OCC_DATA_BASE + (u64)occ_syms[si].addr);
    }
    if (occ_accept("[")) {                   /* a[i] -> *(a + i*8) */
        occ_load_rax_ind();                  /* the array/pointer value        */
        occ_push_rax();
        occ_expr();
        occ_emit(0x48); occ_emit(0xC1); occ_emit(0xE0); occ_emit(3);   /* shl rax,3 */
        occ_mov_rdi_rax(); occ_pop_rax();
        occ_emit(0x48); occ_emit(0x01); occ_emit(0xF8);                /* add rax,rdi */
        occ_expect("]");
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
        int li = occ_loc_find(nm);
        if (li >= 0) { occ_lea_local(occ_locs[li].off); occ_load_rax_ind(); return; }
        int si = occ_sym_get(nm, 1);
        occ_mov_rax_imm(OCC_DATA_BASE + (u64)occ_syms[si].addr);
        occ_load_rax_ind();
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
            occ_next();
            occ_push_rax();                     /* the destination address */
            occ_expr();                         /* the value               */
            occ_pop_rdi();
            occ_store_rdi_rax();
            return;
        }
        occ_pos = save_pos; occ_tk = save_tk; occ_val = save_val; occ_line = save_line;
        for (int i = 0; i < OCC_NAMELEN; i++) occ_txt[i] = save_txt[i];
    }
    occ_binop(0);
}

/* ---- statements ----------------------------------------------------------*/
static int occ_is_type(void) { return occ_is("int") || occ_is("char") || occ_is("void"); }

static void occ_stmt(void) {
    if (occ_accept("{")) { while (!occ_is("}") && occ_tk != T_EOF) occ_stmt(); occ_expect("}"); return; }

    if (occ_is_type()) {                         /* local declaration */
        occ_next();
        while (occ_accept("*")) { }
        if (occ_tk != T_IDENT) { occ_err("expected a declarator", 0); return; }
        if (occ_nloc >= OCC_MAXLOC) occ_err("too many locals in one function", occ_txt);
        else {
            for (int i = 0; i < OCC_NAMELEN; i++) occ_locs[occ_nloc].name[i] = occ_txt[i];
            occ_frame += 8;
            occ_locs[occ_nloc].off = -occ_frame;
            occ_nloc++;
        }
        int slot = occ_nloc - 1;
        occ_next();
        if (occ_accept("=")) {
            occ_expr();
            occ_emit(0x48); occ_emit(0x89); occ_emit(0x85); occ_emit32((u32)occ_locs[slot].off);
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
        if (!occ_is_type()) { occ_err("expected a declaration", occ_txt); occ_next(); continue; }
        occ_next();
        while (occ_accept("*")) { }
        if (occ_tk != T_IDENT) { occ_err("expected a name", occ_txt); occ_next(); continue; }
        char nm[OCC_NAMELEN];
        for (int i = 0; i < OCC_NAMELEN; i++) nm[i] = occ_txt[i];
        occ_next();

        if (occ_accept("(")) {                                   /* function */
            int si = occ_sym_get(nm, 0);
            occ_syms[si].addr = occ_tlen;
            occ_syms[si].defined = 1;
            occ_nloc = 0; occ_frame = 0;

            /* parameters become the first locals */
            int np = 0;
            if (!occ_is(")")) for (;;) {
                if (occ_is_type()) occ_next();
                while (occ_accept("*")) { }
                if (occ_tk == T_IDENT) {
                    if (occ_nloc < OCC_MAXLOC) {
                        for (int i = 0; i < OCC_NAMELEN; i++) occ_locs[occ_nloc].name[i] = occ_txt[i];
                        occ_frame += 8; occ_locs[occ_nloc].off = -occ_frame; occ_nloc++;
                    }
                    np++;
                    occ_next();
                }
                if (!occ_accept(",")) break;
            }
            occ_expect(")");

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
        occ_syms[si].addr = occ_dlen;
        occ_syms[si].defined = 1;
        i64 init = 0;
        int cells = 1;
        if (occ_accept("[")) { if (occ_tk == T_NUM) { cells = (int)occ_val; occ_next(); } occ_expect("]"); }
        if (occ_accept("=")) { if (occ_tk == T_NUM) { init = occ_val; occ_next(); }
                               else { occ_err("global initialiser must be a constant", nm); occ_next(); } }
        for (int c = 0; c < cells; c++) {
            for (int i = 0; i < 8 && occ_dlen < (int)OCC_MAXDATA; i++)
                occ_data[occ_dlen++] = (u8)((u64)(c ? 0 : init) >> (8 * i));
        }
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
 * occ_compile(srcpath, outpath) -> 0 on success, negative on failure.
 * Reads the source out of the VFS, compiles, and writes the ELF back. */
static int occ_compile(const char *srcpath, const char *outpath) {
    occ_errors = 0; occ_line = 1; occ_pos = 0;
    occ_tlen = occ_dlen = occ_rlen = 0;
    occ_nsym = occ_nfix = occ_nloc = 0; occ_frame = 0;

    occ_src  = (char *)omalloc(OCC_MAXSRC);
    occ_text = (u8 *)omalloc(OCC_MAXTEXT);
    occ_data = (u8 *)omalloc(OCC_MAXDATA);
    occ_rod  = (u8 *)omalloc(OCC_MAXDATA);
    if (!occ_src || !occ_text || !occ_data || !occ_rod) { occ_err("out of memory", 0); return -1; }

    /* v0.56 Stage F: THE PRELUDE. /usr/lib/libc.oc is read out of the VFS and
     * placed AHEAD of the user's source in the same buffer, so its definitions
     * are already in scope — and already emitted — by the time the user's code
     * is parsed. That ordering matters for a single-pass compiler: a prelude
     * appended afterwards would need a forward fixup for every libc call, and
     * a prelude in a separate buffer would need a linker, which occ does not
     * have. Source inclusion IS occ's linkage model, and this is it.
     *
     * A missing /usr/lib/libc.oc is not fatal: the program simply compiles
     * without a runtime, and any call it makes to strlen() fails the ordinary
     * "undefined function" check. That is the property that makes /usr/lib
     * genuinely load-bearing rather than decorative — remove it and compiles
     * that depend on it start failing, with a diagnostic that names it. */
    occ_prelude_lines = 0;
    u64 base = 0;
    int lfd = oopen("/usr/lib/libc.oc");
    if (lfd >= 0) {
        i64 ln = oread(lfd, occ_src, OCC_MAXSRC / 2);
        oclose(lfd);
        if (ln > 0) {
            /* oread includes the stored NUL; drop it or the lexer stops here. */
            while (ln > 0 && occ_src[ln - 1] == 0) ln--;
            for (i64 i = 0; i < ln; i++) if (occ_src[i] == '\n') occ_prelude_lines++;
            occ_src[ln] = '\n'; occ_prelude_lines++;
            base = (u64)ln + 1;
        }
    } else {
        occ_note("no /usr/lib/libc.oc — compiling with no runtime;",
                 "calls to strlen/puts/malloc will be undefined");
    }

    int fd = oopen(srcpath);
    if (fd < 0) { occ_err("cannot open source", srcpath); return -2; }
    i64 n = oread(fd, occ_src + base, OCC_MAXSRC - 1 - base);
    oclose(fd);
    if (n < 0) { occ_err("cannot read source", srcpath); return -2; }
    occ_src[base + (u64)n] = 0;

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

    if (!occ_syms[main_sym].defined) occ_err("no main() in", srcpath);

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
    if (w != (i64)len) { occ_err("short write to", outpath); return -6; }
    return 0;
}
