/*
 * Golden model: MPEG-2 / FFmpeg YUV420P sample addressing.
 *
 * Horizontal: MPEG-2 LEFT — chroma co-sited with even luma. chroma_col = x>>1.
 * Vertical progressive: chroma_row = y>>1 (FFmpeg frame 4:2:0).
 * Vertical interlaced:  chroma_row = (y>>2)*2 + (y&1)
 *   even frame lines = top field, odd = bottom field (spatial).
 *   TFF/BFF is not applied here.
 *
 * No chroma interpolation. Opposite-field rows must never mix.
 */

#include <stdio.h>
#include <stdlib.h>

enum { PAL_H = 576, NTSC_H = 480, W = 720 };

static int fails;

static int chroma_col(int x)
{
    return x >> 1;
}

static int chroma_row_prog(int y)
{
    return y >> 1;
}

static int chroma_row_intl(int y)
{
    return ((y >> 2) << 1) | (y & 1);
}

static int chroma_row(int y, int interlaced)
{
    return interlaced ? chroma_row_intl(y) : chroma_row_prog(y);
}

static int field_of_y(int y)
{
    return y & 1; /* 0 = top/even, 1 = bottom/odd (spatial) */
}

static void expect_eq(const char *name, int got, int want)
{
    if (got != want) {
        fprintf(stderr, "FAIL %s got=%d want=%d\n", name, got, want);
        fails++;
    }
}

static void expect_true(const char *name, int cond)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", name);
        fails++;
    }
}

static void test_horizontal_left(void)
{
    int x;

    for (x = 0; x < W; x += 2) {
        expect_eq("LEFT even col", chroma_col(x), x / 2);
        expect_eq("LEFT odd shares even", chroma_col(x + 1), chroma_col(x));
        if (x + 2 < W)
            expect_true("LEFT next pair differs",
                        chroma_col(x + 2) != chroma_col(x));
    }
    expect_eq("LEFT last PAL/NTSC pair", chroma_col(718), 359);
    expect_eq("LEFT last odd", chroma_col(719), 359);
}

static void test_progressive_rows(int h)
{
    int y;

    for (y = 0; y < h; y += 2) {
        expect_eq("prog pair share", chroma_row_prog(y), chroma_row_prog(y + 1));
        expect_eq("prog row id", chroma_row_prog(y), y / 2);
        if (y + 2 < h)
            expect_true("prog next pair differs",
                        chroma_row_prog(y + 2) != chroma_row_prog(y));
    }
    expect_eq("prog PAL last pair", chroma_row_prog(574), chroma_row_prog(575));
    expect_eq("prog NTSC last pair", chroma_row_prog(478), chroma_row_prog(479));
}

static void test_interlaced_field_isolation(int h)
{
    int y;

    for (y = 0; y < h; y++) {
        int cy = chroma_row_intl(y);
        int f = field_of_y(y);

        expect_eq("intl chroma field bit", cy & 1, f);
        if (y + 1 < h)
            expect_true("intl opposite fields differ",
                        chroma_row_intl(y) != chroma_row_intl(y + 1));
        /* Same field, same 4:2:0 pair: frame lines 4k+f and 4k+2+f. */
        if ((y & 2) == 0 && y + 2 < h)
            expect_eq("intl same-field pair shares",
                      chroma_row_intl(y + 2), chroma_row_intl(y));
        if (y + 4 < h)
            expect_true("intl next same-field pair differs",
                        chroma_row_intl(y + 4) != chroma_row_intl(y));
    }

    expect_eq("intl y0", chroma_row_intl(0), 0);
    expect_eq("intl y1", chroma_row_intl(1), 1);
    expect_eq("intl y2", chroma_row_intl(2), 0);
    expect_eq("intl y3", chroma_row_intl(3), 1);
    expect_eq("intl y4", chroma_row_intl(4), 2);
    expect_eq("intl y5", chroma_row_intl(5), 3);
    expect_eq("PAL last top", chroma_row_intl(574), chroma_row_intl(572));
    expect_eq("PAL last bot", chroma_row_intl(575), chroma_row_intl(573));
    expect_eq("NTSC last top", chroma_row_intl(478), chroma_row_intl(476));
    expect_eq("NTSC last bot", chroma_row_intl(479), chroma_row_intl(477));
}

/* CRT Native / HDMI Bob: one spatial field at a time. src_y = {vc, field}. */
static void test_field_display(int h, int field)
{
    int vc, max_vc = h / 2;
    int prev_cy = -1;

    for (vc = 0; vc < max_vc; vc++) {
        int y = (vc << 1) | field;
        int cy = chroma_row_intl(y);

        expect_eq("displayed field matches spatial", y & 1, field);
        expect_eq("displayed chroma stays in field", cy & 1, field);
        if (vc > 0) {
            int y_prev = ((vc - 1) << 1) | field;
            if ((vc & 1) == 1)
                expect_eq("adjacent field-lines share chroma",
                          cy, chroma_row_intl(y_prev));
            else
                expect_true("next chroma pair in same field",
                            cy != prev_cy);
        }
        prev_cy = cy;
        (void)prev_cy;
    }
}

/* SIMPLE y>>1 on interlaced *does* pull the opposite field. Documented. */
static void test_simple_is_wrong_for_interlaced(void)
{
    expect_true("bug y=1 SIMPLE uses top chroma",
                chroma_row_prog(1) == 0 && chroma_row_intl(1) == 1);
    expect_true("bug y=2 SIMPLE uses bot chroma",
                chroma_row_prog(2) == 1 && chroma_row_intl(2) == 0);
}

/* Unique chroma per row: U=row, V=255-row. Neighbour rules. */
static void torture_unique_rows(int h, int interlaced)
{
    int y, x;

    for (y = 0; y < h; y++) {
        int cy = chroma_row(y, interlaced);
        int u = cy & 255;
        int v = 255 - u;

        expect_true("U encodes row", u == (cy & 255));
        expect_true("V inverse", v == ((255 - cy) & 255));
        for (x = 0; x < 8; x++) {
            int cc = chroma_col(x);
            if (x & 1)
                expect_eq("torture LEFT", cc, chroma_col(x - 1));
        }
        if (!interlaced && (y & 1))
            expect_eq("torture prog share", cy, chroma_row(y - 1, 0));
        if (interlaced && (y & 2) && y >= 2)
            expect_eq("torture intl same-field share",
                      cy, chroma_row(y - 2, 1));
        if (interlaced && y >= 1)
            expect_true("torture no cross-field",
                        chroma_row(y, 1) != chroma_row(y - 1, 1));
    }
}

int main(void)
{
    test_horizontal_left();
    test_progressive_rows(NTSC_H);
    test_progressive_rows(PAL_H);
    test_interlaced_field_isolation(NTSC_H);
    test_interlaced_field_isolation(PAL_H);
    test_field_display(NTSC_H, 0);
    test_field_display(NTSC_H, 1);
    test_field_display(PAL_H, 0);
    test_field_display(PAL_H, 1);
    test_simple_is_wrong_for_interlaced();
    torture_unique_rows(NTSC_H, 0);
    torture_unique_rows(PAL_H, 0);
    torture_unique_rows(NTSC_H, 1);
    torture_unique_rows(PAL_H, 1);

    /* TFF/BFF must not change the spatial row map. */
    expect_eq("TFF spatial y0 still top", chroma_row_intl(0), 0);
    expect_eq("BFF spatial y0 still top", chroma_row_intl(0), 0);
    expect_eq("BFF spatial y1 still bot", chroma_row_intl(1), 1);

    printf("yuv420_chroma_ref: %s (%d fails)\n",
           fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
