#include "startup_recovery.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// A completed initialization clears the streak. Elapsed time cannot prove
// recovery: noticing a hang and forcing a reboot can take several minutes.
static int interrupted(const char *path)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return errno == ENOENT ? 0 : -1;
    struct stat status;
    char text[80] = { 0 };
    bool regular = fstat(fd, &status) == 0 && S_ISREG(status.st_mode) && status.st_size >= 0
            && status.st_size < (off_t)sizeof(text);
    ssize_t size = regular ? read(fd, text, sizeof(text) - 1) : -1;
    bool closed = close(fd) == 0;
    if (!closed || !regular || size != status.st_size)
        return -1;
    if (strlen(text) != (size_t)size)
        return 1;
    if (!strcmp(text, "Kobalt startup did not complete.\n"))
        return 1;
    if (!strcmp(text, "kobalt-startup-v2 1\n"))
        return 1;
    if (!strcmp(text, "kobalt-startup-v2 2\n"))
        return 2;
    // Read the previous version strictly, but no longer expire its count.
    const char prefix[] = "kobalt-startup-v1 ";
    const size_t offset = sizeof(prefix) - 1;
    if (strncmp(text, prefix, offset) || (text[offset] != '1' && text[offset] != '2')
        || text[offset + 1] != ' ')
        return 1;
    const char *digits = text + offset + 2;
    if (*digits < '1' || *digits > '9')
        return 1;
    const char *cursor = digits;
    while (*cursor >= '0' && *cursor <= '9')
        ++cursor;
    if (cursor == digits || *cursor != '\n' || cursor[1] != '\0')
        return 1;
    char *end;
    errno = 0;
    long long started = strtoll(digits, &end, 10);
    if (errno || started <= 0 || *end != '\n' || end[1] != '\0')
        return 1;
    int count = text[offset] - '0';
    return count;
}

static bool sync_parent(const char *path)
{
    char *parent = strdup(path);
    if (!parent)
        return false;
    char *slash = strrchr(parent, '/');
    if (!slash) {
        free(parent);
        return false;
    }
    *slash = '\0';
    int fd = open(*parent ? parent : "/", O_RDONLY | O_DIRECTORY);
    free(parent);
    if (fd < 0)
        return false;
    bool ok = fsync(fd) == 0;
    return close(fd) == 0 && ok;
}

bool kobalt_startup_remove(const char *path)
{
    return interrupted(path) == 2;
}

bool kobalt_startup_complete(const char *path)
{
    if (unlink(path) != 0)
        return errno == ENOENT;
    return sync_parent(path);
}

static bool write_record(const char *path, const char *text)
{
    char *temporary = malloc(strlen(path) + sizeof(".XXXXXX"));
    if (!temporary)
        return false;
    sprintf(temporary, "%s.XXXXXX", path);
    int fd = mkstemp(temporary);
    if (fd < 0) {
        free(temporary);
        return false;
    }
    size_t size = strlen(text);
    bool ok = write(fd, text, size) == (ssize_t)size && fsync(fd) == 0;
    if (close(fd) != 0)
        ok = false;
    if (ok)
        ok = rename(temporary, path) == 0;
    if (ok && !sync_parent(path)) {
        // Do not initialize if durable recovery state cannot be established.
        unlink(path);
        ok = false;
    }
    unlink(temporary);
    free(temporary);
    return ok;
}

bool kobalt_startup_begin(const char *path)
{
    int previous = interrupted(path);
    if (previous < 0 || previous >= 2)
        return false;
    char text[80];
    snprintf(text, sizeof(text), "kobalt-startup-v2 %d\n", previous + 1);
    return write_record(path, text);
}

bool kobalt_startup_removed(const char *notice)
{
    return write_record(notice,
                        "Kobalt was removed after two startups in a row did not finish.\n"
                        "Your books, settings and reading data are kept.\n"
                        "Installing Kobalt again retries it.\n");
}

bool kobalt_startup_recovered(const char *path, const char *notice)
{
    const bool completed = kobalt_startup_complete(path);
    return kobalt_startup_complete(notice) && completed;
}
