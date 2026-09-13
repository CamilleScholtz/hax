/* SPDX-License-Identifier: MIT */
#include "render/progress.h"

#include <stdio.h>

#include "terminal/ansi.h"

void progress_bar_print(double frac, int width)
{
    if (frac < 0)
        frac = 0;
    if (frac > 1)
        frac = 1;
    if (width < 1)
        width = 1;

    int filled = (int)(frac * width + 0.5);

    fputs(ANSI_DIM, stdout);
    for (int i = 0; i < filled; i++)
        fputc('#', stdout);
    for (int i = filled; i < width; i++)
        fputc('.', stdout);
    /* Close DIM specifically (SGR 22) instead of ANSI_RESET so any
     * caller-level attributes (e.g. an outer color span around a row
     * that contains the bar) survive. */
    fputs(ANSI_BOLD_OFF, stdout);
}
