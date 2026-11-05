#include "platforms/kobo/common/startup_recovery.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(value)                                            \
    do {                                                        \
        if (!(value)) {                                         \
            fprintf(stderr, "line %d: %s\n", __LINE__, #value); \
            return 1;                                           \
        }                                                       \
    } while (0)

// Checks the startup marker: removal after two unfinished starts, the removal notice, streak resets, old and invalid markers and unusable paths.
int main(void)
{
    char directory[4096], path[4096], link[4096], notice[4096];
    const char *tmp = getenv("TMPDIR");
    snprintf(directory, sizeof(directory), "%s/kobalt-startup.XXXXXX", tmp ? tmp : "/tmp");
    CHECK(mkdtemp(directory));
    CHECK(snprintf(path, sizeof(path), "%s/startup-session", directory) < (int)sizeof(path));
    CHECK(snprintf(link, sizeof(link), "%s/link", directory) < (int)sizeof(link));
    CHECK(snprintf(notice, sizeof(notice), "%s/startup-removed.txt", directory)
          < (int)sizeof(notice));
    CHECK(!kobalt_startup_remove(path));
    CHECK(kobalt_startup_begin(path));
    CHECK(!kobalt_startup_remove(path)); // One crash is insufficient.
    CHECK(kobalt_startup_begin(path));
    CHECK(kobalt_startup_remove(path)); // Two unfinished attempts.
    CHECK(!kobalt_startup_begin(path));
    CHECK(kobalt_startup_removed(notice));
    FILE *explanation = fopen(notice, "r");
    char message[256] = { 0 };
    CHECK(explanation && fread(message, 1, sizeof(message) - 1, explanation) > 0);
    CHECK(fclose(explanation) == 0);
    CHECK(strstr(message, "two startups in a row did not finish"));
    CHECK(strstr(message, "books, settings and reading data are kept"));
    CHECK(strstr(message, "Installing Kobalt again retries it"));
    CHECK(kobalt_startup_remove(path));
    // Uninstall clears the marker but keeps the explanation for USB inspection.
    CHECK(kobalt_startup_complete(path));
    CHECK(access(notice, F_OK) == 0);
    CHECK(kobalt_startup_begin(path));
    CHECK(kobalt_startup_recovered(path, notice));
    CHECK(access(notice, F_OK) != 0);
    CHECK(kobalt_startup_recovered(path, notice));
    // A failed explanation write does not change the removal decision.
    CHECK(kobalt_startup_begin(path));
    CHECK(kobalt_startup_begin(path));
    CHECK(mkdir(notice, 0700) == 0);
    CHECK(!kobalt_startup_removed(notice));
    CHECK(kobalt_startup_remove(path));
    CHECK(rmdir(notice) == 0);
    CHECK(kobalt_startup_complete(path));

    // A successful retry breaks the streak, even if stock Nickel crashes next.
    CHECK(kobalt_startup_begin(path));
    CHECK(kobalt_startup_begin(path));
    CHECK(kobalt_startup_complete(path));
    for (int boot = 0; boot < 5; ++boot) {
        CHECK(!kobalt_startup_remove(path));
        CHECK(kobalt_startup_begin(path));
        CHECK(kobalt_startup_complete(path));
    }
    // A requested shutdown/restart cancels an unfinished attempt.
    CHECK(kobalt_startup_begin(path));
    CHECK(kobalt_startup_complete(path));
    CHECK(kobalt_startup_begin(path));
    CHECK(!kobalt_startup_remove(path));
    CHECK(kobalt_startup_complete(path));

    // Migrate the exact old marker and preserve timestamped counts regardless
    // of clock changes or time spent waiting before restarting a hung device.
    const char *old[] = { "Kobalt startup did not complete.\n", "kobalt-startup-v1 1 1\n",
                          "kobalt-startup-v1 1 9999999999\n" };
    for (unsigned i = 0; i < sizeof(old) / sizeof(*old); ++i) {
        FILE *file = fopen(path, "w");
        CHECK(file && fputs(old[i], file) >= 0 && fclose(file) == 0);
        CHECK(!kobalt_startup_remove(path));
        CHECK(kobalt_startup_begin(path));
        CHECK(kobalt_startup_remove(path));
        CHECK(kobalt_startup_complete(path));
    }

    const char *twice[] = { "kobalt-startup-v1 2 1\n", "kobalt-startup-v1 2 9999999999\n" };
    for (unsigned i = 0; i < sizeof(twice) / sizeof(*twice); ++i) {
        FILE *previous = fopen(path, "w");
        CHECK(previous && fputs(twice[i], previous) >= 0 && fclose(previous) == 0);
        CHECK(kobalt_startup_remove(path));
        CHECK(kobalt_startup_complete(path));
    }

    const char *invalid[] = { "",
                              "Kobalt startup did not complete.",
                              "Kobalt startup did not complete.\nextra",
                              "kobalt-startup-v2 3\n",
                              "kobalt-startup-v1 3 1800000000\n",
                              "kobalt-startup-v1 2 +1\n",
                              "kobalt-startup-v1 2  1\n",
                              "kobalt-startup-v1 2 \n1\n",
                              "kobalt-startup-v1 2 01\n",
                              "kobalt-startup-v1 2 1800000000 garbage",
                              "kobalt-startup-v1 2 -1\n",
                              "kobalt-startup-v1 2 999999999999999999999999999999999999\n" };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        FILE *file = fopen(path, "w");
        CHECK(file);
        CHECK(fputs(invalid[i], file) >= 0);
        CHECK(fclose(file) == 0);
        CHECK(!kobalt_startup_remove(path));
        CHECK(kobalt_startup_begin(path));
        CHECK(kobalt_startup_remove(path));
        CHECK(kobalt_startup_complete(path));
    }
    FILE *binary = fopen(path, "w");
    const char corrupt[] = "kobalt-startup-v1 2 1800000000\0garbage";
    CHECK(binary && fwrite(corrupt, 1, sizeof(corrupt), binary) == sizeof(corrupt));
    CHECK(fclose(binary) == 0);
    CHECK(!kobalt_startup_remove(path));
    CHECK(kobalt_startup_begin(path));
    CHECK(kobalt_startup_remove(path));
    CHECK(kobalt_startup_complete(path));
    FILE *oversized = fopen(path, "w");
    CHECK(oversized);
    for (int i = 0; i < 80; ++i)
        CHECK(fputc('x', oversized) != EOF);
    CHECK(fclose(oversized) == 0);
    CHECK(!kobalt_startup_begin(path));
    CHECK(!kobalt_startup_remove(path));
    CHECK(unlink(path) == 0);
    FILE *unreadable = fopen(path, "w");
    CHECK(unreadable && fputs("garbage", unreadable) >= 0 && fclose(unreadable) == 0);
    CHECK(chmod(path, 0000) == 0);
    if (geteuid() != 0) {
        CHECK(!kobalt_startup_begin(path));
        CHECK(!kobalt_startup_remove(path));
    }
    CHECK(chmod(path, 0600) == 0);
    CHECK(unlink(path) == 0);
    CHECK(mkdir(path, 0700) == 0);
    CHECK(!kobalt_startup_remove(path));
    CHECK(!kobalt_startup_begin(path));
    CHECK(rmdir(path) == 0);
    CHECK(mkfifo(path, 0600) == 0);
    CHECK(!kobalt_startup_begin(path));
    CHECK(!kobalt_startup_remove(path));
    CHECK(unlink(path) == 0);
    CHECK(symlink("missing", path) == 0);
    CHECK(!kobalt_startup_remove(path));
    CHECK(!kobalt_startup_begin(path));
    CHECK(unlink(path) == 0);
    CHECK(rmdir(directory) == 0);
    CHECK(!kobalt_startup_begin(path)); // Unavailable storage.
    CHECK(!kobalt_startup_remove(path));
    puts("Startup recovery: consecutive failures, single crash, stock crashes, normal restart, legacy migration and invalid storage passed");
    return 0;
}
