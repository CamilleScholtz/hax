/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "harness.h"
#include "xalloc.h"
#include "render/progress.h"
#include "system/locale.h"
#include "terminal/ansi.h"

static void expect_bar(double fraction, int width, const char *expected)
{
    fflush(stdout);
    EXPECT(ftruncate(fileno(stdout), 0) == 0);
    rewind(stdout);
    progress_bar_print(fraction, width);
    fflush(stdout);
    rewind(stdout);
    char output[128];
    size_t len = fread(output, 1, sizeof(output) - 1, stdout);
    output[len] = '\0';
    EXPECT_STR_EQ(output, expected);
}

int main(void)
{
    locale_init_utf8();
    char *path = xasprintf("%s/stdout", t_tempdir());
    if (!freopen(path, "w+", stdout)) {
        perror("freopen");
        free(path);
        return 1;
    }
    free(path);
    expect_bar(0, 4, ANSI_DIM "...." ANSI_BOLD_OFF);
    expect_bar(0.5, 4, ANSI_DIM "##.." ANSI_BOLD_OFF);
    expect_bar(0.625, 4, ANSI_DIM "###." ANSI_BOLD_OFF);
    expect_bar(1, 4, ANSI_DIM "####" ANSI_BOLD_OFF);
    expect_bar(-1, 4, ANSI_DIM "...." ANSI_BOLD_OFF);
    expect_bar(2, 4, ANSI_DIM "####" ANSI_BOLD_OFF);
    expect_bar(0, 0, ANSI_DIM "." ANSI_BOLD_OFF);
    T_REPORT();
}
