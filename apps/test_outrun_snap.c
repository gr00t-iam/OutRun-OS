/* Host tests for the screenshot utility: the BMP codec and the capture plan.
 *
 * The codec is exercised as a ROUND TRIP — encode, then decode the bytes back
 * and compare every pixel — because an encoder and a decoder that share a
 * mistake will agree with each other perfectly. The header bytes are therefore
 * also checked against the literal values a foreign reader will look for.
 *
 * The capture plan matters because a full desktop does not fit in a ring-3
 * program's memory: the image is captured and written in horizontal strips,
 * bottom-up, and the strips must tile the rectangle EXACTLY. A plan that
 * overlapped would duplicate rows and one that left a gap would write a file
 * shorter than its own header claims — which is precisely the truncation the
 * decoder above refuses. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define APP_HOST_TEST
#include "outrun_snap.c"

static unsigned int src[8 * 5];
static unsigned char file[BMP_HEADER_BYTES + 8 * 3 * 5 + 64];

int main(void) {
    /* ---- geometry -------------------------------------------------------- */
    assert(bmp_stride(1) == 4);        /* 3 bytes padded to 4                  */
    assert(bmp_stride(2) == 8);        /* 6 -> 8                               */
    assert(bmp_stride(4) == 12);       /* 12 is already aligned                */
    assert(bmp_stride(8) == 24);
    assert(bmp_stride(0) == 0);
    assert(bmp_size(8, 5) == BMP_HEADER_BYTES + 24 * 5);
    assert(bmp_size(0, 5) == 0 && bmp_size(8, 0) == 0);

    /* ---- header bytes, against the literals a foreign reader looks for ---- */
    unsigned char h[BMP_HEADER_BYTES];
    assert(bmp_header(h, 8, 5) == 0);
    assert(h[0] == 'B' && h[1] == 'M');
    assert(bmp_get32(h + 2) == BMP_HEADER_BYTES + 24 * 5);
    assert(bmp_get32(h + 10) == BMP_HEADER_BYTES);
    assert(bmp_get32(h + 14) == 40);
    assert(bmp_get32(h + 18) == 8);
    assert(bmp_get32(h + 22) == 5);
    assert(bmp_get16(h + 26) == 1);
    assert(bmp_get16(h + 28) == 24);
    assert(bmp_get32(h + 30) == 0);
    assert(bmp_header(h, 0, 5) == -1 && bmp_header(h, 8, -1) == -1);

    /* ---- round trip ------------------------------------------------------ */
    for (int y = 0; y < 5; y++)
        for (int x = 0; x < 8; x++)
            src[y * 8 + x] = ((unsigned)(x * 32) << 16) | ((unsigned)(y * 50) << 8) | (unsigned)(x + y);
    memset(file, 0xAB, sizeof file);            /* poison, so padding is visible */
    bmp_header(file, 8, 5);
    /* Rows are written BOTTOM-UP: the last source row goes first. */
    for (int y = 0; y < 5; y++)
        bmp_row(file + BMP_HEADER_BYTES + (unsigned)y * bmp_stride(8), &src[(4 - y) * 8], 8);

    int w = 0, hh = 0;
    unsigned off = 0;
    assert(bmp_parse(file, bmp_size(8, 5), &w, &hh, &off) == 0);
    assert(w == 8 && hh == 5 && off == BMP_HEADER_BYTES);
    for (int y = 0; y < 5; y++)
        for (int x = 0; x < 8; x++)
            assert(bmp_pixel(file, off, 8, 5, x, y) == src[y * 8 + x]);
    /* Out-of-range reads are black, not out-of-bounds. */
    assert(bmp_pixel(file, off, 8, 5, -1, 0) == 0);
    assert(bmp_pixel(file, off, 8, 5, 8, 0) == 0);
    assert(bmp_pixel(file, off, 8, 5, 0, 5) == 0);
    /* Width 8 needs no padding at all: 24 pixel bytes is already 4-aligned,
     * so the row buffer must not add any. */
    assert(bmp_stride(8) == 24);

    /* Width 5 DOES: 15 pixel bytes, one pad byte, and that byte must be
     * written as zero rather than left holding the previous row's tail. The
     * row buffer here is poisoned first so a skipped write is visible. */
    {
        unsigned char rowbuf[32];
        unsigned int five[5] = { 0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF };
        memset(rowbuf, 0xAB, sizeof rowbuf);
        assert(bmp_stride(5) == 16);
        bmp_row(rowbuf, five, 5);
        for (int i = 0; i < 15; i++) assert(rowbuf[i] == 0xFF);
        assert(rowbuf[15] == 0);
        assert(rowbuf[16] == 0xAB);            /* and nothing beyond the row  */
    }

    /* ---- malformed files are refused, one reason at a time ---------------- */
    unsigned char bad[sizeof file];
    memcpy(bad, file, sizeof file);
    assert(bmp_parse(bad, BMP_HEADER_BYTES - 1, 0, 0, 0) == -1);   /* too short */
    bad[0] = 'X';
    assert(bmp_parse(bad, bmp_size(8, 5), 0, 0, 0) == -1);         /* magic     */
    memcpy(bad, file, sizeof file);
    bmp_put16(bad + 28, 32);
    assert(bmp_parse(bad, bmp_size(8, 5), 0, 0, 0) == -1);         /* 32bpp     */
    memcpy(bad, file, sizeof file);
    bmp_put32(bad + 30, 1);
    assert(bmp_parse(bad, bmp_size(8, 5), 0, 0, 0) == -1);         /* RLE       */
    memcpy(bad, file, sizeof file);
    bmp_put32(bad + 10, 4);
    assert(bmp_parse(bad, bmp_size(8, 5), 0, 0, 0) == -1);         /* offset    */
    memcpy(bad, file, sizeof file);
    bmp_put32(bad + 18, 0);
    assert(bmp_parse(bad, bmp_size(8, 5), 0, 0, 0) == -1);         /* zero width*/
    /* THE TRUNCATION CASE: the header is perfect and the rows are not there. */
    memcpy(bad, file, sizeof file);
    assert(bmp_parse(bad, bmp_size(8, 5) - 1, 0, 0, 0) == -1);

    /* ---- capture rectangles ---------------------------------------------- */
    struct snap_rect r;
    snap_preset(&r, SNAP_FULL, 1024, 768);
    assert(r.x == 0 && r.y == 0 && r.w == 1024 && r.h == 768);
    assert(snap_valid(&r, 1024, 768));
    snap_preset(&r, SNAP_LEFT, 1024, 768);
    assert(r.x == 0 && r.w == 512 && r.h == 768);
    snap_preset(&r, SNAP_RIGHT, 1024, 768);
    assert(r.x == 512 && r.w == 512);
    snap_preset(&r, SNAP_CENTRE, 1024, 768);
    assert(r.x == 256 && r.y == 192 && r.w == 512 && r.h == 384);
    /* An odd desktop width must not produce a rectangle that runs one pixel
     * off the right edge. */
    snap_preset(&r, SNAP_RIGHT, 1025, 769);
    assert(snap_valid(&r, 1025, 769));
    assert(r.x + r.w == 1025);

    /* Out-of-range rectangles are refused, matching SYS_FB_CAPTURE, which
     * refuses rather than clamps for the same reason. */
    r.x = 0; r.y = 0; r.w = 0; r.h = 10;   assert(!snap_valid(&r, 100, 100));
    r.w = 10; r.h = 0;                     assert(!snap_valid(&r, 100, 100));
    r.w = 10; r.h = 10; r.x = 95;          assert(!snap_valid(&r, 100, 100));
    r.x = 0; r.y = 95;                     assert(!snap_valid(&r, 100, 100));
    r.x = -1; r.y = 0;                     assert(!snap_valid(&r, 100, 100));
    r.x = 90; r.y = 90;                    assert(snap_valid(&r, 100, 100));

    /* ---- the strip plan tiles the rectangle exactly ----------------------- */
    /* Strips run BOTTOM-UP, because that is the order the rows go into the
     * file; strip 0 is therefore the BOTTOM of the image. */
    assert(snap_strips(768, 64) == 12);
    assert(snap_strips(100, 64) == 2);
    assert(snap_strips(64, 64) == 1);
    assert(snap_strips(1, 64) == 1);
    assert(snap_strips(0, 64) == 0);

    assert(snap_strip_top(100, 64, 0) == 36 && snap_strip_rows(100, 64, 0) == 64);
    assert(snap_strip_top(100, 64, 1) == 0  && snap_strip_rows(100, 64, 1) == 36);
    assert(snap_strip_rows(100, 64, 2) == 0);

    for (int height = 1; height <= 200; height++) {
        for (int strip = 1; strip <= 70; strip += 13) {
            int n = snap_strips(height, strip), covered = 0, expect_top = height;
            for (int i = 0; i < n; i++) {
                int rows = snap_strip_rows(height, strip, i);
                int top = snap_strip_top(height, strip, i);
                assert(rows > 0 && rows <= strip);
                assert(top + rows == expect_top);       /* abuts the one below */
                expect_top = top;
                covered += rows;
            }
            assert(expect_top == 0);                    /* reaches the top     */
            assert(covered == height);                  /* no gap, no overlap  */
        }
    }

    /* ---- generated file names -------------------------------------------- */
    char name[24];
    snap_name(name, 0);      assert(!strcmp(name, "snap0000.bmp"));
    snap_name(name, 7);      assert(!strcmp(name, "snap0007.bmp"));
    snap_name(name, 1234);   assert(!strcmp(name, "snap1234.bmp"));
    snap_name(name, 10000);  assert(!strcmp(name, "snap0000.bmp"));   /* wraps  */

    printf("outrun_snap: BMP header literals, pixel round trip, six malformed-file "
           "refusals, capture rectangles and an exact strip plan PASS\n");
    return 0;
}
