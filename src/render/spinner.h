/* SPDX-License-Identifier: MIT */
#ifndef HAX_RENDER_SPINNER_H
#define HAX_RENDER_SPINNER_H

/* Thread-safe live indicators written directly to stdout, for interactive terminals only.
 * Operations taking a spinner pointer are NULL-safe no-ops.
 *
 * Between show and hide, callers must not write to stdout; the spinner owns the cursor. */
struct spinner;

#define SPINNER_TOOL_VIEW_ROWS_MAX 8

/* Create a spinner with a copied initial label. NULL or an empty label selects "working...".
 * Returns NULL when stdout is not a terminal or the animation thread cannot start. */
struct spinner *spinner_new(const char *label);
void spinner_free(struct spinner *spinner);

/* Show the labeled spinner at column zero of the current empty row. Hide erases that row. A
 * first appearance from hidden is deferred briefly; any show or hide within the grace period
 * cancels it. */
void spinner_show(struct spinner *spinner);

/* Show the labeled spinner one blank row below the current cursor, then restore the cursor on
 * hide. cursor_col is the current zero-based column. The current line must not wrap. First
 * appearances are deferred as for spinner_show(). */
void spinner_park(struct spinner *spinner, int cursor_col);

/* One prepared tool-status row: fully styled bytes for a single physical row without a line
 * ending, and its painted cell width for reflow-aware repaints. Rows must be clipped to the
 * terminal by the caller; a wrapped row breaks the view's row accounting. */
struct spinner_row {
    const char *bytes;
    int cells;
};

/* Replace the tool-status view with up to SPINNER_TOOL_VIEW_ROWS_MAX copied prepared rows, newest
 * last; excess oldest rows are dropped. Rows are painted verbatim, with the animated glyph
 * overprinting the last row's first cell. Shows the view if it is hidden; content changes repaint
 * on the next animation frame, not synchronously. */
void spinner_set_tool_status_view(struct spinner *spinner, const struct spinner_row *rows,
                                  int count);

/* Erase the visible row and restore the cursor position saved by spinner_park(). */
void spinner_hide(struct spinner *spinner);

/* Hide as spinner_hide(), but begin DEC 2026 synchronized output atomically with the erase and
 * leave the bracket open for the caller's replacement writes. Pair with spinner_swap_end();
 * brackets do not nest. Until then the caller may only write replacement output and optionally
 * re-show the spinner as the final act: a show inside the bracket paints immediately, without
 * the first-appearance grace, so the indicator survives the swap without blinking. */
void spinner_swap_begin(struct spinner *spinner);

/* End the swap's synchronized output. Emits nothing when no swap is open. */
void spinner_swap_end(struct spinner *spinner);

/* Change the label as spinner_label_set() and spinner_label_request(), repainting a visible
 * labeled spinner when its text changes. */
void spinner_set_label(struct spinner *spinner, const char *key, const char *label);
void spinner_request_label(struct spinner *spinner, const char *key, const char *label);

/* Set the labeled spinner's elapsed-time origin to a positive monotonic_ms() value, or disable the
 * counter with zero. The counter appears only for long waits and never on tool-status rows. */
void spinner_set_timer(struct spinner *spinner, long started_at_ms);

/* Return a borrowed, NUL-terminated one-cell UTF-8 glyph selected from monotonic time. */
const char *spinner_glyph_now(void);

/* How long a requested label must stay stable before it is displayed. */
#define SPINNER_LABEL_SETTLE_MS 2000

/* The label's displayed text, with hysteresis for requested changes. A key names a logical state
 * independently of its display text, and NULL or empty values select the "working" and
 * "working..." defaults. Strings are owned copies. Not thread-safe; initialize with
 * spinner_label_init(). */
struct spinner_label {
    char *text;
    char *key;
    char *pending_text; /* NULL when no request is pending */
    char *pending_key;
    long pending_since_ms;
    /* When requests began contradicting the displayed key, or 0. Silence contradicts nothing. */
    long contradicted_since_ms;
};

/* Display text under the default key. */
void spinner_label_init(struct spinner_label *label, const char *text);
void spinner_label_free(struct spinner_label *label);

/* Replace the displayed state at once and discard any pending request. Returns whether the
 * displayed text changed. */
int spinner_label_set(struct spinner_label *label, const char *key, const char *text);

/* Request a change at now_ms. Repeating the displayed key updates its text at once, and repeating
 * the pending key updates its text but keeps its settling time; any other key becomes pending.
 * Returns whether the displayed text changed. */
int spinner_label_request(struct spinner_label *label, const char *key, const char *text,
                          long now_ms);

/* Display a pending key that has stayed stable for SPINNER_LABEL_SETTLE_MS by now_ms. Requests
 * that keep contradicting the displayed key that long without one settling return the display to
 * "working...". */
void spinner_label_settle(struct spinner_label *label, long now_ms);

struct buf;
struct live_region;

/* Append one synchronized tool-view repaint of region: overprint every row before erasing stale
 * tails, so terminals without synchronized output never show a blank frame. Rows are painted
 * verbatim with the glyph overprinting the last row's first cell. Pure with respect to the
 * terminal. */
void spinner_build_tool_frame(struct buf *frame, struct live_region *region,
                              const struct spinner_row *rows, int row_count, const char *glyph,
                              int terminal_cols);

#endif /* HAX_RENDER_SPINNER_H */
