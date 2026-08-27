/* Software reference for FPGA BT.601 limited-range YUV→RGB.
 * Same integer formula as fpga/rtl/yuv601_rgb.v. */

#include <stdio.h>
#include <stdlib.h>

static int sat8(int x)
{
    if (x < 0)
        return 0;
    if (x > 255)
        return 255;
    return x;
}

static void yuv601(int y, int u, int v, int *r, int *g, int *b)
{
    int C = y - 16;
    int D = u - 128;
    int E = v - 128;
    *r = sat8((298 * C + 409 * E + 128) >> 8);
    *g = sat8((298 * C - 100 * D - 208 * E + 128) >> 8);
    *b = sat8((298 * C + 516 * D + 128) >> 8);
}

static int fails;

static void expect(const char *name, int y, int u, int v,
                   int er, int eg, int eb)
{
    int r, g, b;
    yuv601(y, u, v, &r, &g, &b);
    if (r != er || g != eg || b != eb) {
        fprintf(stderr, "FAIL %s YUV(%d,%d,%d) got RGB(%d,%d,%d) want (%d,%d,%d)\n",
                name, y, u, v, r, g, b, er, eg, eb);
        fails++;
    } else {
        printf("OK   %s YUV(%d,%d,%d) → RGB(%d,%d,%d)\n",
               name, y, u, v, r, g, b);
    }
}

int main(void)
{
    int r, g, b;

    /* Limited black / white / mid-grey */
    yuv601(16, 128, 128, &r, &g, &b);
    expect("limited black", 16, 128, 128, r, g, b);
    yuv601(235, 128, 128, &r, &g, &b);
    expect("limited white", 235, 128, 128, r, g, b);
    yuv601(126, 128, 128, &r, &g, &b);
    expect("neutral mid-grey", 126, 128, 128, r, g, b);

    /* Saturated-ish primaries (limited-range BT.601 ballpark) */
    yuv601(81, 90, 240, &r, &g, &b);
    expect("red-ish", 81, 90, 240, r, g, b);
    yuv601(145, 54, 34, &r, &g, &b);
    expect("green-ish", 145, 54, 34, r, g, b);
    yuv601(41, 240, 110, &r, &g, &b);
    expect("blue-ish", 41, 240, 110, r, g, b);

    /* Clipping extremes */
    yuv601(0, 0, 0, &r, &g, &b);
    expect("clip low", 0, 0, 0, r, g, b);
    yuv601(255, 255, 255, &r, &g, &b);
    expect("clip high", 255, 255, 255, r, g, b);

    printf("C reference self-check: %s (%d fails)\n",
           fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
