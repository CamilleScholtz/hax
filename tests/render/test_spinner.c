/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "buf.h"
#include "harness.h"
#include "xalloc.h"
#include "render/spinner.h"
#include "system/locale.h"
#include "terminal/ansi.h"
#include "terminal/live_region.h"

#define BRAILLE_PREFIX "\xE2\xA0"

static char capture_buf[4096];

static void capture_init(void)
{
    locale_init_utf8();
    char *path = xasprintf("%s/stdout", t_tempdir());
    if (!freopen(path, "w+", stdout)) {
        perror("freopen");
        exit(1);
    }
    free(path);
}

static void capture_reset(void)
{
    fflush(stdout);
    if (ftruncate(fileno(stdout), 0) != 0) {
        perror("ftruncate");
        exit(1);
    }
    rewind(stdout);
}

static const char *capture_read(void)
{
    fflush(stdout);
    rewind(stdout);
    size_t length = fread(capture_buf, 1, sizeof(capture_buf) - 1, stdout);
    capture_buf[length] = '\0';
    return capture_buf;
}

static void test_glyph_is_one_braille_codepoint(void)
{
    const char *glyph = spinner_glyph_now();
    EXPECT(strncmp(glyph, BRAILLE_PREFIX, 2) == 0);
    EXPECT(glyph[3] == '\0');
}

/* All spinner entry points must be silent no-ops on the NULL spinner non-TTY runs carry. */
static void test_spinner_is_null_and_silent_without_tty(void)
{
    capture_reset();
    struct spinner *spinner = spinner_new(NULL);
    EXPECT(spinner == NULL);

    spinner_show(spinner);
    spinner_set_label(spinner, "thinking", "thinking...");
    spinner_request_label(spinner, "reading", "reading...");
    spinner_set_timer(spinner, 1);
    spinner_park(spinner, 4);
    struct spinner_row row = {.bytes = "| live output", .cells = 13};
    spinner_set_tool_status_view(spinner, &row, 1);
    spinner_swap_begin(spinner);
    spinner_swap_end(spinner);
    spinner_hide(spinner);
    spinner_free(spinner);
    EXPECT_STR_EQ(capture_read(), "");
}

#define FRAME_CHROME ANSI_DIM ANSI_CYAN

static void test_tool_frame_paints_rows_verbatim(void)
{
    struct buf frame;
    buf_init(&frame);
    struct live_region region = {0};
    struct spinner_row rows[] = {{.bytes = "<older>", .cells = 7},
                                 {.bytes = "<newest>", .cells = 8}};

    spinner_build_tool_frame(&frame, &region, rows, 2, "*", 80);
    EXPECT_STR_EQ(frame.data, ANSI_SYNC_BEGIN "\r"
                                              "<older>" ANSI_ERASE_LINE "\r\n"
                                              "<newest>" ANSI_ERASE_BELOW "\r" FRAME_CHROME
                                              "*" ANSI_RESET ANSI_SYNC_END);
    EXPECT(region.row_count == 2);
    EXPECT(region.cursor_row == 1);
    EXPECT(region.cursor_col == 0);
    live_region_free(&region);
    buf_free(&frame);
}

static void test_label_set_replaces_at_once(void)
{
    struct spinner_label label;
    spinner_label_init(&label, NULL);
    EXPECT_STR_EQ(label.text, "working...");
    spinner_label_request(&label, "thinking", "thinking...", 1000);

    EXPECT(spinner_label_set(&label, "compose", "[bash] composing..."));
    EXPECT_STR_EQ(label.text, "[bash] composing...");
    EXPECT(label.pending_key == NULL);
    spinner_label_settle(&label, 1000 + SPINNER_LABEL_SETTLE_MS);
    EXPECT_STR_EQ(label.text, "[bash] composing...");
    EXPECT(!spinner_label_set(&label, "compose", "[bash] composing..."));
    spinner_label_free(&label);
}

static void test_label_request_waits_until_stable(void)
{
    struct spinner_label label;
    spinner_label_init(&label, NULL);

    EXPECT(!spinner_label_request(&label, "thinking", "thinking...", 1000));
    spinner_label_settle(&label, 1000 + SPINNER_LABEL_SETTLE_MS - 1);
    EXPECT_STR_EQ(label.text, "working...");
    spinner_label_settle(&label, 1000 + SPINNER_LABEL_SETTLE_MS);
    EXPECT_STR_EQ(label.text, "thinking...");
    EXPECT_STR_EQ(label.key, "thinking");
    spinner_label_free(&label);
}

static void test_label_repeats_update_text_but_keep_settling_time(void)
{
    struct spinner_label label;
    spinner_label_init(&label, NULL);

    spinner_label_request(&label, "processing", "processing... 10%", 1000);
    spinner_label_request(&label, "processing", "processing... 50%", 2500);
    spinner_label_settle(&label, 1000 + SPINNER_LABEL_SETTLE_MS);
    EXPECT_STR_EQ(label.text, "processing... 50%");

    EXPECT(spinner_label_request(&label, "processing", "processing... 90%", 4000));
    EXPECT_STR_EQ(label.text, "processing... 90%");
    spinner_label_free(&label);
}

static void test_label_churn_returns_to_default(void)
{
    struct spinner_label label;
    spinner_label_init(&label, NULL);
    spinner_label_set(&label, "thinking", "thinking...");

    /* No candidate stays stable long enough to settle, so the stale label gives way. */
    spinner_label_request(&label, "a", "a...", 1000);
    spinner_label_request(&label, "b", "b...", 1500);
    spinner_label_request(&label, "a", "a...", 2500);
    spinner_label_settle(&label, 1000 + SPINNER_LABEL_SETTLE_MS - 1);
    EXPECT_STR_EQ(label.text, "thinking...");
    spinner_label_settle(&label, 1000 + SPINNER_LABEL_SETTLE_MS);
    EXPECT_STR_EQ(label.text, "working...");
    spinner_label_free(&label);
}

static void test_label_displayed_key_ends_contradiction(void)
{
    struct spinner_label label;
    spinner_label_init(&label, NULL);
    spinner_label_set(&label, "thinking", "thinking...");

    spinner_label_request(&label, "a", "a...", 1000);
    spinner_label_request(&label, "thinking", "thinking...", 1500);
    spinner_label_settle(&label, 1000 + 2 * SPINNER_LABEL_SETTLE_MS);
    EXPECT_STR_EQ(label.text, "thinking...");
    spinner_label_free(&label);
}

int main(void)
{
    capture_init();
    test_glyph_is_one_braille_codepoint();
    test_spinner_is_null_and_silent_without_tty();
    test_tool_frame_paints_rows_verbatim();
    test_label_set_replaces_at_once();
    test_label_request_waits_until_stable();
    test_label_repeats_update_text_but_keep_settling_time();
    test_label_churn_returns_to_default();
    test_label_displayed_key_ends_contradiction();
    T_REPORT();
}
