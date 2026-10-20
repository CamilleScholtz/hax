/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "buf.h"
#include "harness.h"
#include "xalloc.h"
#include "render/footer.h"
#include "system/locale.h"
#include "terminal/ansi.h"
#include "terminal/live_region.h"

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

/* All footer entry points must be silent no-ops on the NULL footer non-TTY runs carry. */
static void test_footer_is_null_and_silent_without_tty(void)
{
    capture_reset();
    struct footer *footer = footer_new(NULL);
    EXPECT(footer == NULL);

    footer_show(footer);
    footer_set_label(footer, "thinking", "thinking...");
    footer_request_label(footer, "reading", "reading...");
    footer_set_timer(footer, 1);
    footer_park(footer, 4);
    struct footer_row row = {.bytes = "| live output", .cells = 13};
    footer_set_rows(footer, &row, 1);
    footer_swap_begin(footer);
    footer_swap_end(footer);
    footer_hide(footer);
    footer_free(footer);
    EXPECT_STR_EQ(capture_read(), "");
}

#define FRAME_CHROME ANSI_DIM ANSI_CYAN

static void test_frame_paints_rows_verbatim_under_glyph(void)
{
    struct buf frame;
    buf_init(&frame);
    struct live_region region = {0};
    struct footer_row rows[] = {{.bytes = "<older>", .cells = 7},
                                {.bytes = "<newest>", .cells = 8}};

    footer_build_frame(&frame, &region, rows, 2, "*", 80);
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

static void test_frame_without_glyph_rests_cursor_at_row_start(void)
{
    struct buf frame;
    buf_init(&frame);
    struct live_region region = {0};
    struct footer_row row = {.bytes = "<status>", .cells = 8};

    footer_build_frame(&frame, &region, &row, 1, NULL, 80);
    EXPECT_STR_EQ(frame.data, ANSI_SYNC_BEGIN "\r<status>" ANSI_ERASE_BELOW "\r" ANSI_SYNC_END);
    EXPECT(region.cursor_col == 0);
    live_region_free(&region);
    buf_free(&frame);
}

int main(void)
{
    capture_init();
    test_footer_is_null_and_silent_without_tty();
    test_frame_paints_rows_verbatim_under_glyph();
    test_frame_without_glyph_rests_cursor_at_row_start();
    T_REPORT();
}
