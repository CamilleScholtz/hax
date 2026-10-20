/* SPDX-License-Identifier: MIT */
#ifndef HAX_RENDER_SPINNER_H
#define HAX_RENDER_SPINNER_H

/* The animated glyph and status label that the footer paints while work is in progress. Pure: the
 * footer owns the terminal, the animation thread, and locking. */

struct buf;

#define SPINNER_FRAME_INTERVAL_MS 80

/* Return a borrowed, NUL-terminated one-cell UTF-8 glyph selected from monotonic time. */
const char *spinner_glyph_now(void);

/* Append the status row: the glyph and the label, prefixed by elapsed_ms once it marks a long wait
 * and both fit. The row fits max_cells; returns its cell width. */
int spinner_format_status(struct buf *out, const char *glyph, const char *label, long elapsed_ms,
                          int max_cells);

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

#endif /* HAX_RENDER_SPINNER_H */
