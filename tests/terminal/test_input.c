/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "buf.h"
#include "harness.h"
#include "xalloc.h"
#include "system/locale.h"
#include "terminal/ansi.h"
#include "terminal/input.h"
#include "terminal/input_core.h"
#include "terminal/theme.h"

static void test_submitted_message_ascii_gutter(void)
{
    char *out = NULL;
    size_t len = 0;
    FILE *stream = open_memstream(&out, &len);
    EXPECT(stream != NULL);
    if (!stream)
        return;
    EXPECT(theme_set("off") == 0);
    input_render_user_message_to(stream, "abc def\nx", 9, 7);
    fclose(stream);
    EXPECT_STR_EQ(out, "┃ abc " ANSI_ERASE_LINE "\r\n"
                       "┃ def" ANSI_ERASE_LINE "\r\n"
                       "┃ x" ANSI_ERASE_LINE "\r\n");
    free(out);
    EXPECT(theme_set("ansi") == 0);
}

static void test_edit_rows_repeat_prompt_gutter(void)
{
    struct input in = {
        .buf = (char *)"abc def\n\nx",
        .len = 10,
        .prompt = ANSI_BOLD "┃" ANSI_BOLD_OFF " ",
        .display_columns = 7,
    };
    struct buf frame;
    buf_init(&frame);
    input_render_edit_rows(&frame, &in, 0, 3);
    EXPECT_STR_EQ(frame.data, ANSI_BOLD "┃" ANSI_BOLD_OFF " abc " ANSI_ERASE_LINE "\r\n" ANSI_BOLD
                                        "┃" ANSI_BOLD_OFF " def" ANSI_ERASE_LINE "\r\n" ANSI_BOLD
                                        "┃" ANSI_BOLD_OFF " " ANSI_ERASE_LINE "\r\n" ANSI_BOLD
                                        "┃" ANSI_BOLD_OFF " x");

    buf_reset(&frame);
    input_render_edit_rows(&frame, &in, 1, 2);
    EXPECT_STR_EQ(frame.data, ANSI_BOLD "┃" ANSI_BOLD_OFF " def" ANSI_ERASE_LINE "\r\n" ANSI_BOLD
                                        "┃" ANSI_BOLD_OFF " ");
    buf_free(&frame);
}

static void test_search_continuations_stay_at_column_zero(void)
{
    struct input in = {
        .buf = (char *)"a\nb",
        .len = 3,
        .prompt = "search: ",
        .display_columns = 20,
        .continuation_at_column_zero = 1,
    };
    struct buf frame;
    buf_init(&frame);
    input_render_edit_rows(&frame, &in, 0, 1);
    EXPECT_STR_EQ(frame.data, "search: a" ANSI_ERASE_LINE "\r\nb");
    buf_reset(&frame);
    input_render_edit_rows(&frame, &in, 1, 1);
    EXPECT_STR_EQ(frame.data, "b");
    buf_free(&frame);
}

static char *history_fixture(const char *body)
{
    char *path = xasprintf("%s/history", t_tempdir());
    FILE *f = fopen(path, "w");
    EXPECT(f != NULL);
    if (f) {
        fputs(body, f);
        fclose(f);
    }
    return path;
}

static long file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static void test_history_load_is_read_only(void)
{
    char *path = history_fixture("first\nsecond\n");
    long before = file_size(path);

    struct input *in = input_new();
    input_history_load(in, path);

    EXPECT(in->hist_n == 2);
    EXPECT_STR_EQ(in->hist[0], "first");
    EXPECT_STR_EQ(in->hist[1], "second");

    input_history_add(in, "typed this run");
    EXPECT(in->hist_n == 3);
    EXPECT_STR_EQ(in->hist[2], "typed this run");
    EXPECT(file_size(path) == before);

    input_free(in);
    free(path);
}

static void test_history_open_appends(void)
{
    char *path = history_fixture("first\nsecond\n");
    long before = file_size(path);

    struct input *in = input_new();
    input_history_open(in, path);
    EXPECT(in->hist_n == 2);

    input_history_add(in, "typed this run");
    EXPECT(in->hist_n == 3);
    EXPECT(file_size(path) > before);

    struct input *reloaded = input_new();
    input_history_load(reloaded, path);
    EXPECT(reloaded->hist_n == 3);
    EXPECT_STR_EQ(reloaded->hist[2], "typed this run");

    input_free(reloaded);
    input_free(in);
    free(path);
}

static void test_history_missing_file(void)
{
    char *path = xasprintf("%s/nope/history", t_tempdir());

    struct input *in = input_new();
    input_history_load(in, path);
    EXPECT(in->hist_n == 0);
    EXPECT(file_size(path) == -1);

    input_history_add(in, "typed this run");
    EXPECT(in->hist_n == 1);
    EXPECT(file_size(path) == -1);

    input_free(in);
    free(path);
}

static void test_history_session_entry_persists_on_resubmit(void)
{
    char *path = history_fixture("first\n");

    struct input *in = input_new();
    input_history_open(in, path);
    long before = file_size(path);

    input_history_add_session(in, "stashed draft");
    EXPECT(in->hist_n == 2);
    EXPECT(file_size(path) == before);

    /* Submitting the recalled draft unchanged must reach the file despite being
     * a repeat of the newest in-memory entry. */
    input_history_add(in, "stashed draft");
    EXPECT(in->hist_n == 2);
    long after = file_size(path);
    EXPECT(after > before);

    input_history_add(in, "stashed draft");
    EXPECT(file_size(path) == after);

    struct input *reloaded = input_new();
    input_history_load(reloaded, path);
    EXPECT(reloaded->hist_n == 2);
    EXPECT_STR_EQ(reloaded->hist[1], "stashed draft");

    input_free(reloaded);
    input_free(in);
    free(path);
}

static void noop_view(void *user)
{
    (void)user;
}

static void other_view(void *user)
{
    (void)user;
}

static void test_modal_key_macro(void)
{
    EXPECT(INPUT_KEY_CTRL('O') == 0x0f);
    EXPECT(INPUT_KEY_CTRL('T') == 0x14);
    EXPECT(INPUT_KEY_CTRL('A') == 0x01);
}

static void test_modal_key_bind_and_rebind(void)
{
    struct input *in = input_new();
    int binding_user = 0;

    EXPECT(input_bind_modal_key(in, INPUT_KEY_CTRL('O'), noop_view, &binding_user) == 0);
    EXPECT(input_bind_modal_key(in, INPUT_KEY_CTRL('T'), noop_view, NULL) == 0);
    EXPECT(in->modal_keys[0].key == INPUT_KEY_CTRL('O'));
    EXPECT(in->modal_keys[0].user == &binding_user);
    EXPECT(in->modal_keys[1].key == INPUT_KEY_CTRL('T'));

    EXPECT(input_bind_modal_key(in, INPUT_KEY_CTRL('O'), other_view, NULL) == 0);
    EXPECT(in->modal_keys[0].fn == other_view);
    EXPECT(in->modal_keys[0].user == NULL);
    EXPECT(in->modal_keys[2].fn == NULL);

    EXPECT(input_bind_modal_key(in, INPUT_KEY_CTRL('O'), NULL, NULL) == 0);
    EXPECT(in->modal_keys[0].fn == NULL);

    input_free(in);
}

static void test_modal_key_rejects_printable(void)
{
    struct input *in = input_new();
    EXPECT(input_bind_modal_key(in, 'q', noop_view, NULL) == -1);
    EXPECT(in->modal_keys[0].fn == NULL);
    input_free(in);
}

static void test_modal_key_table_full(void)
{
    struct input *in = input_new();
    for (int i = 0; i < INPUT_MODAL_KEYS_MAX; i++)
        EXPECT(input_bind_modal_key(in, (unsigned char)(i + 1), noop_view, NULL) == 0);
    EXPECT(input_bind_modal_key(in, 0x1f, noop_view, NULL) == -1);
    EXPECT(input_bind_modal_key(in, 0x1f, NULL, NULL) == 0);
    input_free(in);
}

int main(void)
{
    locale_init_utf8();
    test_edit_rows_repeat_prompt_gutter();
    test_search_continuations_stay_at_column_zero();
    test_submitted_message_ascii_gutter();
    test_history_load_is_read_only();
    test_history_open_appends();
    test_history_missing_file();
    test_history_session_entry_persists_on_resubmit();
    test_modal_key_macro();
    test_modal_key_bind_and_rebind();
    test_modal_key_rejects_printable();
    test_modal_key_table_full();
    T_REPORT();
}
