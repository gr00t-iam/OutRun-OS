#ifndef OUTRUN_BMP_H
#define OUTRUN_BMP_H
/* A 24-bit uncompressed BMP codec, shared by the screenshot tool that writes
 * these files and the media player that reads them back.
 *
 * ONE CODEC, TWO PROGRAMS. A second implementation compiled into the reader
 * would be a second thing to keep correct, and the tests would only ever have
 * covered one of them — the same argument the kernel build already makes for
 * compiling the scrypt core from a single source.
 *
 * BMP rather than a private format, because a screenshot that can only be
 * opened by the machine that took it is not much of a screenshot: this writes
 * a file that a host tool will open unmodified once the volume is extracted.
 *
 * The layout is the classic one: a 14-byte file header, a 40-byte BITMAPINFO
 * header, then pixel rows BOTTOM-UP, each row padded to a 4-byte boundary and
 * each pixel three bytes in B,G,R order. Bottom-up is the default orientation
 * and is what the widest range of readers accepts; the row reversal lives here
 * so neither program has to remember it. */

#define BMP_HEADER_BYTES 54

static inline unsigned bmp_stride(int w) {
    return w <= 0 ? 0u : (unsigned)(((unsigned)w * 3u + 3u) & ~3u);
}
static inline unsigned long long bmp_size(int w, int h) {
    if (w <= 0 || h <= 0) return 0ull;
    return (unsigned long long)BMP_HEADER_BYTES + (unsigned long long)bmp_stride(w) * (unsigned long long)h;
}
static inline void bmp_put16(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}
static inline void bmp_put32(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}
static inline unsigned bmp_get16(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}
static inline unsigned bmp_get32(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}
/* Writes exactly BMP_HEADER_BYTES bytes. Returns 0, or -1 for a rectangle
 * that has no pixels — a zero-sized image is refused rather than written as a
 * valid header describing nothing. */
static inline int bmp_header(unsigned char *out, int w, int h) {
    if (w <= 0 || h <= 0) return -1;
    unsigned long long total = bmp_size(w, h);
    if (total > 0xFFFFFFFFull) return -1;
    for (int i = 0; i < BMP_HEADER_BYTES; i++) out[i] = 0;
    out[0] = 'B'; out[1] = 'M';
    bmp_put32(out + 2, (unsigned)total);
    bmp_put32(out + 10, BMP_HEADER_BYTES);       /* pixel data offset          */
    bmp_put32(out + 14, 40);                     /* BITMAPINFOHEADER size      */
    bmp_put32(out + 18, (unsigned)w);
    bmp_put32(out + 22, (unsigned)h);            /* positive: rows bottom-up   */
    bmp_put16(out + 26, 1);                      /* planes                     */
    bmp_put16(out + 28, 24);                     /* bits per pixel             */
    bmp_put32(out + 30, 0);                      /* BI_RGB, no compression     */
    bmp_put32(out + 34, (unsigned)(bmp_stride(w) * (unsigned)h));
    bmp_put32(out + 38, 2835);                   /* ~72 dpi, x                 */
    bmp_put32(out + 42, 2835);                   /* ~72 dpi, y                 */
    return 0;
}
/* One row: `dst` must have bmp_stride(w) bytes. The padding bytes are written
 * as zero rather than left alone — a row buffer reused across a whole image
 * would otherwise carry the previous row's tail into the file. */
static inline void bmp_row(unsigned char *dst, const unsigned int *px, int w) {
    unsigned stride = bmp_stride(w);
    unsigned at = 0;
    for (int x = 0; x < w; x++) {
        unsigned int p = px[x];
        dst[at++] = (unsigned char)(p & 0xFF);          /* B */
        dst[at++] = (unsigned char)((p >> 8) & 0xFF);   /* G */
        dst[at++] = (unsigned char)((p >> 16) & 0xFF);  /* R */
    }
    while (at < stride) dst[at++] = 0;
}
/* Validates a file and reports its geometry. Returns 0, or -1.
 *
 * EVERY FIELD THAT COULD MAKE A READER WALK OFF THE BUFFER IS CHECKED: the
 * magic, the bit depth, the compression, the data offset, and — the one most
 * often missed — that the declared rows actually FIT in the bytes present. A
 * decoder that trusted the width and height in a header would read past the
 * end of any truncated file, and a screenshot that was cut short by a full
 * volume is exactly the file this will be handed. */
static inline int bmp_parse(const unsigned char *b, unsigned long long len,
                            int *out_w, int *out_h, unsigned *out_off) {
    if (!b || len < BMP_HEADER_BYTES) return -1;
    if (b[0] != 'B' || b[1] != 'M') return -1;
    unsigned off = bmp_get32(b + 10);
    unsigned hdr = bmp_get32(b + 14);
    unsigned w = bmp_get32(b + 18);
    unsigned h = bmp_get32(b + 22);
    unsigned bpp = bmp_get16(b + 28);
    unsigned comp = bmp_get32(b + 30);
    if (hdr < 40 || bpp != 24 || comp != 0) return -1;
    if (!w || !h || w > 0x7FFFu || h > 0x7FFFu) return -1;
    if (off < BMP_HEADER_BYTES || off > len) return -1;
    unsigned long long need = (unsigned long long)bmp_stride((int)w) * (unsigned long long)h;
    if (len - off < need) return -1;
    if (out_w) *out_w = (int)w;
    if (out_h) *out_h = (int)h;
    if (out_off) *out_off = off;
    return 0;
}
/* One pixel as 0x00RRGGBB, addressed TOP-DOWN so callers work in screen
 * coordinates and the file's bottom-up storage stays an implementation
 * detail of this header. Out-of-range coordinates return black rather than
 * reading outside the buffer. */
static inline unsigned int bmp_pixel(const unsigned char *b, unsigned off, int w, int h,
                                     int x, int y) {
    if (x < 0 || y < 0 || x >= w || y >= h) return 0u;
    const unsigned char *row = b + off + (unsigned long long)bmp_stride(w) * (unsigned)(h - 1 - y);
    const unsigned char *p = row + (unsigned)x * 3u;
    return ((unsigned int)p[2] << 16) | ((unsigned int)p[1] << 8) | (unsigned int)p[0];
}
#endif
