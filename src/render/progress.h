/* SPDX-License-Identifier: MIT */
#ifndef HAX_RENDER_PROGRESS_H
#define HAX_RENDER_PROGRESS_H

/* Render a single-line progress bar to stdout (no trailing newline).
 *
 * `frac` is clamped to [0,1]; `width` is the bar's cell count.
 * Whole-cell resolution — `frac * width` rounds to the nearest cell.
 * The whole bar is drawn at ANSI_DIM with the fill/empty contrast
 * coming from ASCII '#' fill and '.' track. Quiet by design, intended for
 * status output that should recede behind conversation text. */
void progress_bar_print(double frac, int width);

#endif /* HAX_RENDER_PROGRESS_H */
