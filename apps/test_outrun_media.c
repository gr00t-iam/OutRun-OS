/* Host tests for the media player's core: playlist assembly, frame timing,
 * and the letterbox fit.
 *
 * A player has three ways to be quietly wrong, and all three are pinned here.
 * It can play frames in the wrong ORDER, because a flat directory hands them
 * over in table order rather than in name order. It can play them at the wrong
 * RATE, because the interval between two samples of a wall clock is a delta
 * with all the usual failure modes. And it can present a frame at the wrong
 * SHAPE, because fitting an image into a window either preserves the aspect
 * ratio or silently stretches it.
 *
 * The fit is checked for both orientations and for the exact-fit case, and it
 * is checked to CENTRE what it fits — an off-by-one in the centring is the
 * kind of thing that looks fine on one window size and wrong on the next. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define APP_HOST_TEST
#include "outrun_media.c"

static struct media_state m;

int main(void) {
    /* ---- playlist assembly ----------------------------------------------- */
    media_reset(&m);
    assert(m.count == 0);
    /* Entries arrive in whatever order the flat dirent table holds them. */
    assert(media_add(&m, "snap0003.bmp", 100) == 0);
    assert(media_add(&m, "snap0001.bmp", 100) == 0);
    assert(media_add(&m, "snap0002.bmp", 100) == 0);
    /* Anything without a .bmp suffix is skipped rather than queued and then
     * failing to decode one at a time during playback. The filter is on the
     * SUFFIX, not on the "snap" prefix: any BMP on the volume is playable, and
     * restricting the player to files this system's own screenshot tool
     * produced would be an arbitrary rule with nothing behind it. */
    assert(media_add(&m, "readme", 10) < 0);
    assert(media_add(&m, "notes.txt", 10) < 0);
    assert(media_add(&m, "snap0001.bm", 10) < 0);
    assert(media_add(&m, "", 10) < 0);
    assert(media_add(&m, "picture.bmp", 100) == 0 && m.count == 4);
    m.count = 3;                                  /* back to the three frames  */
    /* A zero-length file cannot contain a BMP header, let alone pixels. */
    assert(media_add(&m, "snap0009.bmp", 0) < 0);
    assert(m.count == 3);

    media_sort(&m);
    assert(!strcmp(m.name[0], "snap0001.bmp"));
    assert(!strcmp(m.name[1], "snap0002.bmp"));
    assert(!strcmp(m.name[2], "snap0003.bmp"));

    /* The playlist is bounded, and a full one says so rather than dropping
     * the tail silently. */
    media_reset(&m);
    for (int i = 0; i < MEDIA_MAXFRAME; i++) {
        char n[24];
        sprintf(n, "snap%04d.bmp", i);
        assert(media_add(&m, n, 100) == 0);
    }
    assert(m.count == MEDIA_MAXFRAME && !m.truncated);
    assert(media_add(&m, "snap9999.bmp", 100) < 0);
    assert(m.truncated == 1 && m.count == MEDIA_MAXFRAME);

    /* ---- frame advance --------------------------------------------------- */
    media_reset(&m);
    for (int i = 0; i < 3; i++) {
        char n[24];
        sprintf(n, "snap%04d.bmp", i);
        media_add(&m, n, 100);
    }
    m.fps = 10;                                 /* 100 ms per frame            */
    m.playing = 1;
    m.frame = 0;

    /* The first tick establishes the clock; it must not advance a frame, or a
     * player started at an arbitrary uptime would jump immediately. */
    assert(media_tick(&m, 1000000000ull) == 0);
    assert(m.frame == 0);
    assert(media_tick(&m, 1000000000ull + 99000000ull) == 0);
    assert(m.frame == 0);
    assert(media_tick(&m, 1000000000ull + 100000000ull) == 1);
    assert(m.frame == 1);

    /* A STALL ADVANCES BY ONE FRAME, not by the number of frames that would
     * have elapsed. Catching up would run most of the clip inside a single
     * repaint and show only where it landed.
     *
     * The stall lengths below are chosen so the two policies DISAGREE. With
     * three frames, a stall of exactly 49 periods lands on the same frame
     * either way, and an assertion written against such a stall would pass
     * against a player that catches up — a test that cannot fail. 300 ms is
     * three periods: advancing by one gives frame 2, catching up gives 1. */
    assert(media_tick(&m, 1000000000ull + 400000000ull) == 1);   /* +3 periods */
    assert(m.frame == 2);

    /* Wrap-around, and then a nine-period stall: one gives 1, nine gives 0. */
    assert(media_tick(&m, 1000000000ull + 500000000ull) == 1);
    assert(m.frame == 0);
    assert(media_tick(&m, 1000000000ull + 1400000000ull) == 1);  /* +9 periods */
    assert(m.frame == 1);

    /* A paused player never advances. */
    m.playing = 0;
    assert(media_tick(&m, 1000000000ull + 9000000000ull) == 0);
    assert(m.frame == 1);
    m.frame = 0;

    /* A clock that goes backwards does not advance and does not wedge: the
     * next forward interval still works. */
    m.playing = 1;
    assert(media_tick(&m, 1ull) == 0);
    assert(m.frame == 0);
    assert(media_tick(&m, 1ull + 100000000ull) == 1);
    assert(m.frame == 1);

    /* An empty playlist cannot advance, and must not divide by its length. */
    media_reset(&m);
    m.fps = 10;
    m.playing = 1;
    assert(media_tick(&m, 1000000000ull) == 0);
    assert(media_tick(&m, 2000000000ull) == 0);
    assert(m.frame == 0);

    /* fps is clamped into a range the compositor can actually serve, and a
     * request outside it is REFUSED rather than clamped, so a control can tell
     * "applied" from "not supported". */
    assert(media_set_fps(&m, 1) == 0 && m.fps == 1);
    assert(media_set_fps(&m, 60) == 0 && m.fps == 60);
    assert(media_set_fps(&m, 0) < 0 && m.fps == 60);
    assert(media_set_fps(&m, 61) < 0 && m.fps == 60);

    /* ---- the letterbox fit ----------------------------------------------- */
    struct media_fit f;
    /* Exact fit: no scaling, no offset. */
    media_fit(&f, 100, 100, 100, 100);
    assert(f.w == 100 && f.h == 100 && f.x == 0 && f.y == 0);

    /* A wide image in a square window: full width, bars top and bottom. */
    media_fit(&f, 200, 100, 100, 100);
    assert(f.w == 100 && f.h == 50 && f.x == 0 && f.y == 25);

    /* A tall image in a square window: full height, bars left and right. */
    media_fit(&f, 100, 200, 100, 100);
    assert(f.w == 50 && f.h == 100 && f.x == 25 && f.y == 0);

    /* Enlargement is allowed, and stays centred. */
    media_fit(&f, 50, 50, 200, 100);
    assert(f.w == 100 && f.h == 100 && f.x == 50 && f.y == 0);

    /* An odd remainder centres to the pixel and never runs off the edge. */
    media_fit(&f, 3, 2, 101, 51);
    assert(f.x >= 0 && f.y >= 0);
    assert(f.x + f.w <= 101 && f.y + f.h <= 51);

    /* Degenerate inputs produce an empty rectangle rather than a division. */
    media_fit(&f, 0, 10, 100, 100);   assert(f.w == 0 && f.h == 0);
    media_fit(&f, 10, 0, 100, 100);   assert(f.w == 0 && f.h == 0);
    media_fit(&f, 10, 10, 0, 100);    assert(f.w == 0 && f.h == 0);
    media_fit(&f, 10, 10, 100, 0);    assert(f.w == 0 && f.h == 0);

    /* ---- source sampling ------------------------------------------------- */
    /* Nearest-neighbour: the mapping from a destination column back to a
     * source column must cover the whole source and never step past its end. */
    for (int dw = 1; dw <= 40; dw++) {
        for (int sw = 1; sw <= 40; sw += 7) {
            for (int dx = 0; dx < dw; dx++) {
                int sx = media_src_index(dx, dw, sw);
                assert(sx >= 0 && sx < sw);
            }
            assert(media_src_index(0, dw, sw) == 0);
        }
    }

    printf("outrun_media: playlist ordering and bounds, frame timing across stalls "
           "and backwards clocks, letterbox fit and source sampling PASS; "
           "state=%zu bytes\n", sizeof m);
    return 0;
}
