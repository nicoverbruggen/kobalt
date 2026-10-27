#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/prctl.h>
#include <unistd.h>

/* Stock liblipc owns its callback threads. Keep its opaque handles private. */
typedef void Lipc;
typedef void LipcEvent;
typedef int (*EventCallback)(Lipc *, const char *, LipcEvent *, void *);
typedef int (*PropertyCallback)(Lipc *, const char *, void *, void *);
static int wake_pipe[2] = {-1, -1};
static volatile sig_atomic_t stopping;
static volatile sig_atomic_t overflow;
static int module_present(const char *modules, const char *name) {
    const size_t length = strlen(name);
    const char *at = modules;
    while ((at = strstr(at, name)) != NULL) {
        if ((at == modules || at[-1] == ' ' || at[-1] == '\t' || at[-1] == '\n') &&
            (at[length] == 0 || at[length] == ' ' || at[length] == '\t' || at[length] == '\n'))
            return 1;
        at += length;
    }
    return 0;
}
static void terminate(int sig) {
    (void)sig;
    const int saved = errno;
    const char byte = 0;
    stopping = 1;
    if (wake_pipe[1] >= 0) {
        const ssize_t ignored = write(wake_pipe[1], &byte, 1);
        (void)ignored;
    }
    errno = saved;
}
static int framework_started(Lipc *handle, const char *name, void *value, void *data) {
    (void)handle; (void)name; (void)data;
    *(int *)value = 1;
    return 0;
}
static int event_received(Lipc *handle, const char *name, LipcEvent *event, void *data) {
    (void)handle; (void)event; (void)data;
    /* One short write is atomic across callbacks and never waits for Qt. */
    char line[128];
    const int length = snprintf(line, sizeof(line), "%s\n", name);
    if (length <= 0 || length >= (int)sizeof(line) || write(wake_pipe[1], line, (size_t)length) != length) {
        overflow = 1;
        terminate(0);
    }
    return 0;
}
#define LOAD(name) do { *(void **)(&name) = dlsym(library, #name); if (!name) { fprintf(stderr, "Missing %s\n", #name); return 1; } } while (0)
int main(void) {
    const pid_t parent = getppid();
    if (parent == 1 || getppid() != parent) {
        fprintf(stderr, "Power bridge lost its application parent\n");
        return 1;
    }
    if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0) {
        perror("Power bridge parent signal");
        return 1;
    }
    if (getppid() != parent) {
        fprintf(stderr, "Power bridge parent exited during startup\n");
        return 1;
    }
    if (pipe2(wake_pipe, O_NONBLOCK | O_CLOEXEC) != 0) {
        perror("Power bridge event pipe");
        return 1;
    }
    signal(SIGTERM, terminate);
    signal(SIGINT, terminate);
    signal(SIGPIPE, SIG_IGN);
    if (fcntl(STDOUT_FILENO, F_SETFL, fcntl(STDOUT_FILENO, F_GETFL) | O_NONBLOCK) < 0) {
        perror("Power bridge output flags");
        return 1;
    }
    void *library = dlopen("liblipc.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!library) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    Lipc *(*LipcOpenEx)(const char *, int *);
    void (*LipcClose)(Lipc *);
    int (*LipcRegisterIntProperty)(Lipc *, const char *, PropertyCallback, PropertyCallback, void *);
    int (*LipcSubscribeExt)(Lipc *, const char *, const char *, EventCallback, void *);
    int (*LipcSetIntProperty)(Lipc *, const char *, const char *, int);
    int (*LipcSetStringProperty)(Lipc *, const char *, const char *, const char *);
    int (*LipcGetStringProperty)(Lipc *, const char *, const char *, char **);
    void (*LipcFreeString)(char *);
    LOAD(LipcOpenEx); LOAD(LipcClose); LOAD(LipcRegisterIntProperty); LOAD(LipcSubscribeExt); LOAD(LipcSetIntProperty);
    LOAD(LipcSetStringProperty);
    LOAD(LipcGetStringProperty); LOAD(LipcFreeString);
    int error = 0;
    Lipc *handle = LipcOpenEx("com.lab126.kaf", &error);
    if (!handle) { fprintf(stderr, "Cannot own framework service: %d\n", error); return 1; }
    int result = 1;
    error = LipcRegisterIntProperty(handle, "frameworkStarted", framework_started, NULL, NULL);
    if (error != 0) {
        fprintf(stderr, "Cannot publish framework state: %d\n", error);
        goto done;
    }
    static const struct { const char *source, *name; } events[] = {
        {"com.lab126.powerd", "goingToScreenSaver"},
        {"com.lab126.powerd", "outOfScreenSaver"},
        {"com.lab126.powerd", "readyToSuspend"},
        {"com.lab126.powerd", "wakeupFromSuspend"},
        {"com.lab126.hal", "usbPlugIn"},
        {"com.lab126.volumd", "userstoreIsLikelyToUnMount"}
    };
    for (unsigned i = 0; i < sizeof(events) / sizeof(events[0]); ++i) {
        error = LipcSubscribeExt(handle, events[i].source, events[i].name, event_received, NULL);
        if (error != 0) { fprintf(stderr, "Cannot subscribe %s: %d\n", events[i].name, error); goto done; }
    }
    if (LipcSetStringProperty(handle, "com.lab126.blanket", "unload", "splash") != 0 ||
        LipcSetStringProperty(handle, "com.lab126.blanket", "unload", "screensaver") != 0) {
        fprintf(stderr, "Cannot release stock screen overlays\n");
        goto done;
    }
    struct timespec began, now;
    clock_gettime(CLOCK_MONOTONIC, &began);
    for (;;) {
        char *modules = NULL;
        error = LipcGetStringProperty(handle, "com.lab126.blanket", "load", &modules);
        const int present = error == 0 && modules && module_present(modules, "screensaver");
        if (modules) LipcFreeString(modules);
        if (error != 0 || stopping) {
            fprintf(stderr, "Cannot inspect stock screen overlays: %d (stopping=%d)\n", error, stopping);
            goto done;
        }
        if (!present) break;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - began.tv_sec >= 5) {
            fprintf(stderr, "Stock screensaver did not unload\n");
            goto done;
        }
        usleep(100000);
    }
    if (write(STDOUT_FILENO, "ready\n", 6) != 6) {
        perror("Power bridge readiness output");
        goto done;
    }
    struct pollfd fds[] = {{wake_pipe[0], POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}};
    char pending[512];
    size_t used = 0;
    int deferred = 0;
    while (!stopping && !overflow) {
        if (poll(fds, 2, -1) < 0) { if (errno == EINTR) continue; break; }
        if (fds[1].revents & (POLLHUP | POLLERR | POLLNVAL)) break;
        if (fds[1].revents & POLLIN) {
            char commands[128];
            const ssize_t n = read(STDIN_FILENO, commands, sizeof(commands));
            if (n <= 0) break;
            /* Commands carry no user values. Coalesce activity notifications. */
            if (memchr(commands, 'a', (size_t)n))
                LipcSetIntProperty(handle, "com.lab126.powerd", "touchScreenSaverTimeout", 1);
        }
        if (!(fds[0].revents & POLLIN)) continue;
        const ssize_t n = read(wake_pipe[0], pending + used, sizeof(pending) - used);
        if (n <= 0) continue;
        used += (size_t)n;
        size_t first = 0;
        for (size_t i = 0; i < used; ++i) {
            if (pending[i] != '\n') continue;
            const size_t name_length = i - first;
            if ((name_length == strlen("goingToScreenSaver") && !memcmp(pending + first, "goingToScreenSaver", name_length)) ||
                (name_length == strlen("outOfScreenSaver") && !memcmp(pending + first, "outOfScreenSaver", name_length)))
                deferred = 0;
            if (!deferred && name_length == strlen("readyToSuspend") && !memcmp(pending + first, "readyToSuspend", name_length)) {
                deferred = 1;
                LipcSetIntProperty(handle, "com.lab126.powerd", "deferSuspend", 2);
            }
            const size_t length = i - first + 1;
            if (write(STDOUT_FILENO, pending + first, length) != (ssize_t)length) {
                stopping = 1;
                break;
            }
            first = i + 1;
        }
        memmove(pending, pending + first, used - first);
        used -= first;
        if (used == sizeof(pending)) break;
    }
    result = overflow ? 1 : 0;
done:
    LipcClose(handle);
    dlclose(library);
    close(wake_pipe[0]);
    close(wake_pipe[1]);
    return result;
}
