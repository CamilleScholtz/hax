/* SPDX-License-Identifier: MIT */
#include "render/spinner.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "buf.h"
#include "xalloc.h"
#include "system/clock.h"
#include "terminal/ansi.h"
#include "terminal/live_region.h"
#include "terminal/theme.h"
#include "terminal/width.h"
#include "text/fmt.h"
#include "text/width.h"

#define FRAME_INTERVAL_MS 80
#define TIMER_MIN_MS      30000
/* An indicator that would be replaced this quickly reads as flicker, not feedback. */
#define LABEL_SHOW_GRACE_MS 300

#define DEFAULT_LABEL     "working..."
#define DEFAULT_LABEL_KEY "working"

static const char *const SPINNER_FRAMES[] = {
    "\xE2\xA0\x8B", "\xE2\xA0\x99", "\xE2\xA0\xB9", "\xE2\xA0\xB8", "\xE2\xA0\xBC",
    "\xE2\xA0\xB4", "\xE2\xA0\xA6", "\xE2\xA0\xA7", "\xE2\xA0\x87", "\xE2\xA0\x8F",
};
#define SPINNER_FRAME_COUNT (sizeof(SPINNER_FRAMES) / sizeof(SPINNER_FRAMES[0]))

enum spinner_mode {
    SPINNER_HIDDEN = 0,
    SPINNER_LABEL,
    SPINNER_TOOL_STATUS,
};

struct spinner {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t wake;
    enum spinner_mode mode;
    int stop_requested;

    struct spinner_label label;
    long timer_started_at_ms;
    int parked_rows;
    int origin_col;
    /* A swap bracket opened by spinner_swap_begin() and not yet closed. */
    int swap_open;
    /* A deferred label show waiting out its grace period on the animation thread. */
    int label_show_pending;
    long label_show_at_ms;
    int pending_parked_rows;
    int pending_origin_col;
    /* Prepared tool-status rows, newest last; the animated glyph overprints the last row. */
    char *tool_view_bytes[SPINNER_TOOL_VIEW_ROWS_MAX];
    int tool_view_cells[SPINNER_TOOL_VIEW_ROWS_MAX];
    int tool_view_count;
    int tool_view_dirty;
    struct live_region tool_region;
};

const char *spinner_glyph_now(void)
{
    long now_ms = monotonic_ms();
    if (now_ms < 0)
        now_ms = 0;
    size_t frame = (size_t)(now_ms / FRAME_INTERVAL_MS) % SPINNER_FRAME_COUNT;
    return SPINNER_FRAMES[frame];
}

/* Erasing the old row tail after repaint avoids a blank frame without synchronized output. */
static void finish_row_repaint(void)
{
    fputs(ANSI_RESET ANSI_ERASE_LINE, stdout);
    fflush(stdout);
}

static void draw_label_row_locked(struct spinner *spinner, const char *glyph)
{
    /* Reserve the last terminal column because filling it can trigger deferred autowrap. */
    int label_budget = term_width() - 1 - 2; /* glyph and separating space */
    if (label_budget < 0)
        label_budget = 0;

    fputs("\r" ANSI_DIM, stdout);
    fputs(glyph, stdout);
    fputc(' ', stdout);

    if (spinner->timer_started_at_ms > 0) {
        long elapsed_ms = monotonic_ms() - spinner->timer_started_at_ms;
        if (elapsed_ms >= TIMER_MIN_MS) {
            char duration[32];
            format_duration_steady(duration, sizeof(duration), elapsed_ms);
            size_t prefix_cells = strlen(duration) + 3; /* spaces and middle dot */
            if (prefix_cells + display_cells(spinner->label.text) <= (size_t)label_budget) {
                fputs(duration, stdout);
                fputs(" \xC2\xB7 ", stdout);
                label_budget -= (int)prefix_cells;
            }
        }
    }

    char *label = truncate_for_display(spinner->label.text, (size_t)label_budget);
    fputs(label, stdout);
    free(label);
    finish_row_repaint();
}

void spinner_build_tool_frame(struct buf *frame, struct live_region *region,
                              const struct spinner_row *rows, int row_count, const char *glyph,
                              int terminal_cols)
{
    buf_append_str(frame, ANSI_SYNC_BEGIN);
    live_region_rewind(region, frame, terminal_cols);
    for (int row = 0; row < row_count; row++)
        live_region_add_row(region, frame, rows[row].bytes, strlen(rows[row].bytes),
                            rows[row].cells);
    /* The animated gutter overprints the last row's first cell, so it must follow the erase. */
    live_region_end_frame(region, frame, row_count - 1, 0);
    buf_append_str(frame, theme_open(THEME_CHROME_DIM));
    buf_append_str(frame, glyph);
    buf_append_str(frame, ANSI_RESET);
    buf_append_str(frame, ANSI_SYNC_END);
}

/* The cursor idles at the end of the glyph row between frames. */
static void draw_glyph_only_locked(const char *glyph)
{
    fputs("\r", stdout);
    fputs(theme_open(THEME_CHROME_DIM), stdout);
    fputs(glyph, stdout);
    fputs(ANSI_RESET, stdout);
    fflush(stdout);
}

static void draw_tool_view_locked(struct spinner *spinner, const char *glyph)
{
    if (!spinner->tool_view_dirty && spinner->tool_region.row_count > 0) {
        draw_glyph_only_locked(glyph);
        return;
    }

    struct spinner_row rows[SPINNER_TOOL_VIEW_ROWS_MAX];
    for (int row = 0; row < spinner->tool_view_count; row++) {
        rows[row].bytes = spinner->tool_view_bytes[row];
        rows[row].cells = spinner->tool_view_cells[row];
    }

    struct buf frame;
    buf_init(&frame);
    spinner_build_tool_frame(&frame, &spinner->tool_region, rows, spinner->tool_view_count, glyph,
                             term_width());
    fwrite(frame.data ? frame.data : "", 1, frame.len, stdout);
    fflush(stdout);
    spinner->tool_view_dirty = 0;
    buf_free(&frame);
}

static void draw_frame_locked(struct spinner *spinner)
{
    const char *glyph = spinner_glyph_now();

    switch (spinner->mode) {
    case SPINNER_HIDDEN:
        break;
    case SPINNER_LABEL:
        draw_label_row_locked(spinner, glyph);
        break;
    case SPINNER_TOOL_STATUS:
        draw_tool_view_locked(spinner, glyph);
        break;
    }
}

void spinner_label_init(struct spinner_label *label, const char *text)
{
    memset(label, 0, sizeof(*label));
    label->text = xstrdup(text && *text ? text : DEFAULT_LABEL);
    label->key = xstrdup(DEFAULT_LABEL_KEY);
}

static void clear_pending(struct spinner_label *label)
{
    free(label->pending_text);
    free(label->pending_key);
    label->pending_text = NULL;
    label->pending_key = NULL;
}

void spinner_label_free(struct spinner_label *label)
{
    clear_pending(label);
    free(label->text);
    free(label->key);
    label->text = NULL;
    label->key = NULL;
}

static int display(struct spinner_label *label, const char *key, const char *text)
{
    if (strcmp(label->key, key) != 0) {
        free(label->key);
        label->key = xstrdup(key);
    }
    if (strcmp(label->text, text) == 0)
        return 0;

    free(label->text);
    label->text = xstrdup(text);
    return 1;
}

int spinner_label_set(struct spinner_label *label, const char *key, const char *text)
{
    clear_pending(label);
    label->contradicted_since_ms = 0;
    return display(label, key && *key ? key : DEFAULT_LABEL_KEY,
                   text && *text ? text : DEFAULT_LABEL);
}

int spinner_label_request(struct spinner_label *label, const char *key, const char *text,
                          long now_ms)
{
    const char *requested_key = key && *key ? key : DEFAULT_LABEL_KEY;
    const char *requested_text = text && *text ? text : DEFAULT_LABEL;

    if (strcmp(label->key, requested_key) == 0) {
        clear_pending(label);
        label->contradicted_since_ms = 0;
        return display(label, requested_key, requested_text);
    }
    if (label->pending_key && strcmp(label->pending_key, requested_key) == 0) {
        if (strcmp(label->pending_text, requested_text) != 0) {
            free(label->pending_text);
            label->pending_text = xstrdup(requested_text);
        }
        return 0;
    }

    clear_pending(label);
    label->pending_text = xstrdup(requested_text);
    label->pending_key = xstrdup(requested_key);
    label->pending_since_ms = now_ms;
    /* Keep the first contradiction time across changing candidates so churn can demote. */
    if (!label->contradicted_since_ms)
        label->contradicted_since_ms = now_ms;
    return 0;
}

void spinner_label_settle(struct spinner_label *label, long now_ms)
{
    if (label->pending_key && now_ms - label->pending_since_ms >= SPINNER_LABEL_SETTLE_MS) {
        free(label->text);
        free(label->key);
        label->text = label->pending_text;
        label->key = label->pending_key;
        label->pending_text = NULL;
        label->pending_key = NULL;
        label->contradicted_since_ms = 0;
        return;
    }

    if (!label->contradicted_since_ms ||
        now_ms - label->contradicted_since_ms < SPINNER_LABEL_SETTLE_MS ||
        strcmp(label->key, DEFAULT_LABEL_KEY) == 0)
        return;

    display(label, DEFAULT_LABEL_KEY, DEFAULT_LABEL);
    label->contradicted_since_ms = 0;
    if (label->pending_key && strcmp(label->pending_key, DEFAULT_LABEL_KEY) == 0)
        clear_pending(label);
}

static void erase_locked(struct spinner *spinner)
{
    if (spinner->tool_region.row_count > 0) {
        /* Erase under the current width so a resize between paint and erase still clears every
         * row. */
        struct buf erase;
        buf_init(&erase);
        live_region_erase(&spinner->tool_region, &erase, term_width());
        fwrite(erase.data, 1, erase.len, stdout);
        fflush(stdout);
        buf_free(&erase);
        return;
    }

    fputs("\r" ANSI_ERASE_LINE, stdout);
    if (spinner->parked_rows > 0) {
        fprintf(stdout, "\x1b[%dA", spinner->parked_rows);
        if (spinner->origin_col > 0)
            fprintf(stdout, "\x1b[%dG", spinner->origin_col + 1);
    }
    fflush(stdout);
}

static void show_locked(struct spinner *spinner, enum spinner_mode mode, int parked_rows,
                        int origin_col)
{
    spinner->label_show_pending = 0;
    spinner_label_settle(&spinner->label, monotonic_ms());
    if (spinner->mode == mode && spinner->parked_rows == parked_rows &&
        spinner->origin_col == origin_col)
        return;

    /* A transition erases one indicator before painting the next; synchronized output keeps the
     * swap from rendering as a blank-and-repaint. */
    enum spinner_mode previous_mode = spinner->mode;
    int synced = previous_mode != SPINNER_HIDDEN;
    if (synced) {
        fputs(ANSI_SYNC_BEGIN, stdout);
        erase_locked(spinner);
    }

    spinner->mode = mode;
    spinner->parked_rows = parked_rows;
    spinner->origin_col = origin_col;
    for (int i = 0; i < parked_rows; i++)
        fputc('\n', stdout);
    draw_frame_locked(spinner);
    if (synced) {
        fputs(ANSI_SYNC_END, stdout);
        fflush(stdout);
    }

    if (previous_mode == SPINNER_HIDDEN)
        pthread_cond_signal(&spinner->wake);
}

/* A first appearance from hidden waits out a grace period on the animation thread, so a label
 * whose wait ends quickly never blinks. Transitions from a visible mode stay immediate, and so
 * does a show inside an open swap bracket: there the erase and this repaint land in one
 * synchronized frame, while deferring would end the bracket with the indicator erased. */
static void request_label_show_locked(struct spinner *spinner, int parked_rows, int origin_col)
{
    if (spinner->mode != SPINNER_HIDDEN || spinner->swap_open) {
        show_locked(spinner, SPINNER_LABEL, parked_rows, origin_col);
        return;
    }
    spinner->label_show_pending = 1;
    spinner->label_show_at_ms = monotonic_ms() + LABEL_SHOW_GRACE_MS;
    spinner->pending_parked_rows = parked_rows;
    spinner->pending_origin_col = origin_col;
    pthread_cond_signal(&spinner->wake);
}

void spinner_show(struct spinner *spinner)
{
    if (!spinner)
        return;

    pthread_mutex_lock(&spinner->mutex);
    request_label_show_locked(spinner, 0, 0);
    pthread_mutex_unlock(&spinner->mutex);
}

void spinner_park(struct spinner *spinner, int cursor_col)
{
    if (!spinner)
        return;

    pthread_mutex_lock(&spinner->mutex);
    /* At column zero the current empty row is the gap; an open line needs another row. */
    int parked_rows = cursor_col > 0 ? 2 : 1;
    request_label_show_locked(spinner, parked_rows, cursor_col > 0 ? cursor_col : 0);
    pthread_mutex_unlock(&spinner->mutex);
}

static void free_tool_view_locked(struct spinner *spinner)
{
    for (int row = 0; row < spinner->tool_view_count; row++) {
        free(spinner->tool_view_bytes[row]);
        spinner->tool_view_bytes[row] = NULL;
    }
    spinner->tool_view_count = 0;
}

static int tool_view_equals_locked(const struct spinner *spinner, const struct spinner_row *rows,
                                   int count)
{
    if (spinner->tool_view_count != count)
        return 0;
    for (int row = 0; row < count; row++) {
        if (spinner->tool_view_cells[row] != rows[row].cells ||
            strcmp(spinner->tool_view_bytes[row], rows[row].bytes ? rows[row].bytes : "") != 0)
            return 0;
    }
    return 1;
}

void spinner_set_tool_status_view(struct spinner *spinner, const struct spinner_row *rows,
                                  int count)
{
    if (!spinner || count < 1)
        return;

    if (count > SPINNER_TOOL_VIEW_ROWS_MAX) {
        rows += count - SPINNER_TOOL_VIEW_ROWS_MAX;
        count = SPINNER_TOOL_VIEW_ROWS_MAX;
    }

    pthread_mutex_lock(&spinner->mutex);
    if (!tool_view_equals_locked(spinner, rows, count)) {
        free_tool_view_locked(spinner);
        for (int row = 0; row < count; row++) {
            spinner->tool_view_bytes[row] = xstrdup(rows[row].bytes ? rows[row].bytes : "");
            spinner->tool_view_cells[row] = rows[row].cells;
        }
        spinner->tool_view_count = count;
        spinner->tool_view_dirty = 1;
    }
    show_locked(spinner, SPINNER_TOOL_STATUS, 0, 0);
    pthread_mutex_unlock(&spinner->mutex);
}

void spinner_set_label(struct spinner *spinner, const char *key, const char *label)
{
    if (!spinner)
        return;

    pthread_mutex_lock(&spinner->mutex);
    if (spinner_label_set(&spinner->label, key, label) && spinner->mode == SPINNER_LABEL)
        draw_frame_locked(spinner);
    pthread_mutex_unlock(&spinner->mutex);
}

void spinner_request_label(struct spinner *spinner, const char *key, const char *label)
{
    if (!spinner)
        return;

    pthread_mutex_lock(&spinner->mutex);
    if (spinner_label_request(&spinner->label, key, label, monotonic_ms()) &&
        spinner->mode == SPINNER_LABEL)
        draw_frame_locked(spinner);
    pthread_mutex_unlock(&spinner->mutex);
}

void spinner_set_timer(struct spinner *spinner, long started_at_ms)
{
    if (!spinner)
        return;

    pthread_mutex_lock(&spinner->mutex);
    spinner->timer_started_at_ms = started_at_ms > 0 ? started_at_ms : 0;
    pthread_mutex_unlock(&spinner->mutex);
}

static void hide_locked(struct spinner *spinner)
{
    spinner->label_show_pending = 0;
    if (spinner->mode == SPINNER_HIDDEN)
        return;

    erase_locked(spinner);
    spinner->mode = SPINNER_HIDDEN;
    spinner->parked_rows = 0;
    spinner->origin_col = 0;
    free_tool_view_locked(spinner);
}

void spinner_hide(struct spinner *spinner)
{
    if (!spinner)
        return;

    pthread_mutex_lock(&spinner->mutex);
    hide_locked(spinner);
    pthread_mutex_unlock(&spinner->mutex);
}

void spinner_swap_begin(struct spinner *spinner)
{
    if (!spinner)
        return;

    /* The bracket must open under the lock: a frame drawn between opening and hiding would end
     * it before the caller's replacement writes. */
    pthread_mutex_lock(&spinner->mutex);
    if (spinner->mode != SPINNER_HIDDEN) {
        fputs(ANSI_SYNC_BEGIN, stdout);
        spinner->swap_open = 1;
    }
    hide_locked(spinner);
    pthread_mutex_unlock(&spinner->mutex);
}

void spinner_swap_end(struct spinner *spinner)
{
    if (!spinner)
        return;

    pthread_mutex_lock(&spinner->mutex);
    if (spinner->swap_open) {
        fputs(ANSI_SYNC_END, stdout);
        fflush(stdout);
        spinner->swap_open = 0;
    }
    pthread_mutex_unlock(&spinner->mutex);
}

/* pthread_cond_timedwait portably uses CLOCK_REALTIME; macOS cannot select CLOCK_MONOTONIC. */
static struct timespec deadline_after_ms(long wait_ms)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += wait_ms / 1000;
    deadline.tv_nsec += (wait_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += deadline.tv_nsec / 1000000000L;
        deadline.tv_nsec %= 1000000000L;
    }
    return deadline;
}

static void *spinner_thread(void *arg)
{
    struct spinner *spinner = arg;

    /* Keep process signals on threads that own the foreground operation and terminal state. */
    sigset_t mask;
    sigfillset(&mask);
    pthread_sigmask(SIG_SETMASK, &mask, NULL);

    pthread_mutex_lock(&spinner->mutex);
    while (!spinner->stop_requested) {
        if (spinner->mode == SPINNER_HIDDEN) {
            if (!spinner->label_show_pending) {
                pthread_cond_wait(&spinner->wake, &spinner->mutex);
                continue;
            }
            long wait_ms = spinner->label_show_at_ms - monotonic_ms();
            if (wait_ms <= 0) {
                show_locked(spinner, SPINNER_LABEL, spinner->pending_parked_rows,
                            spinner->pending_origin_col);
                continue;
            }
            struct timespec show_deadline = deadline_after_ms(wait_ms);
            pthread_cond_timedwait(&spinner->wake, &spinner->mutex, &show_deadline);
            continue;
        }

        spinner_label_settle(&spinner->label, monotonic_ms());
        draw_frame_locked(spinner);
        struct timespec deadline = deadline_after_ms(FRAME_INTERVAL_MS);
        pthread_cond_timedwait(&spinner->wake, &spinner->mutex, &deadline);
    }
    pthread_mutex_unlock(&spinner->mutex);
    return NULL;
}

struct spinner *spinner_new(const char *label)
{
    if (!isatty(fileno(stdout)))
        return NULL;

    struct spinner *spinner = xcalloc(1, sizeof(*spinner));
    spinner_label_init(&spinner->label, label);
    pthread_mutex_init(&spinner->mutex, NULL);
    pthread_cond_init(&spinner->wake, NULL);

    if (pthread_create(&spinner->thread, NULL, spinner_thread, spinner) != 0) {
        pthread_mutex_destroy(&spinner->mutex);
        pthread_cond_destroy(&spinner->wake);
        spinner_label_free(&spinner->label);
        free(spinner);
        return NULL;
    }
    return spinner;
}

void spinner_free(struct spinner *spinner)
{
    if (!spinner)
        return;

    pthread_mutex_lock(&spinner->mutex);
    hide_locked(spinner);
    /* Never leave the terminal in synchronized mode, even on an abandoned swap. */
    if (spinner->swap_open) {
        fputs(ANSI_SYNC_END, stdout);
        spinner->swap_open = 0;
    }
    spinner->stop_requested = 1;
    pthread_cond_signal(&spinner->wake);
    pthread_mutex_unlock(&spinner->mutex);

    pthread_join(spinner->thread, NULL);
    pthread_mutex_destroy(&spinner->mutex);
    pthread_cond_destroy(&spinner->wake);
    spinner_label_free(&spinner->label);
    live_region_free(&spinner->tool_region);
    free(spinner);
}
