#define _DEFAULT_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
typedef int (*EventCallback)(void *, const char *, void *, void *);
typedef int (*PropertyCallback)(void *, const char *, void *, void *);
static struct Binding { const char *name; EventCallback callback; void *data; } bindings[6];
static atomic_uint subscriptions;
static atomic_int closed;
static pthread_t worker;
static void *events(void *unused) {
    (void)unused;
    while (atomic_load(&subscriptions) != 6 && !atomic_load(&closed)) usleep(1000);
    if (atomic_load(&closed)) return NULL;
    const char *sequence[] = {"goingToScreenSaver", "readyToSuspend", "readyToSuspend",
                             "outOfScreenSaver", "goingToScreenSaver", "readyToSuspend"};
    for (unsigned i = 0; i < sizeof(sequence) / sizeof(sequence[0]); ++i)
        for (unsigned j = 0; j < 6; ++j)
            if (!strcmp(sequence[i], bindings[j].name))
                bindings[j].callback((void *)1, sequence[i], NULL, bindings[j].data);
    return NULL;
}
void *LipcOpenEx(const char *name, int *error) {
    *error = 17;
    if (strcmp(name, "com.lab126.kaf") || getenv("FAKE_DUPLICATE")) return NULL;
    *error = 0;
    if (pthread_create(&worker, NULL, events, NULL)) return NULL;
    return (void *)1;
}
void LipcClose(void *handle) {
    (void)handle;
    atomic_store(&closed, 1);
    pthread_join(worker, NULL);
    fprintf(stderr, "closed\n");
}
int LipcRegisterIntProperty(void *handle, const char *name, PropertyCallback get, PropertyCallback set, void *data) {
    int value = 0;
    return strcmp(name, "frameworkStarted") || set || !get || get(handle, name, &value, data) || value != 1;
}
int LipcSubscribeExt(void *handle, const char *source, const char *name, EventCallback callback, void *data) {
    (void)handle; (void)source;
    if (getenv("FAKE_SUBSCRIBE_FAIL") && !strcmp(name, "readyToSuspend")) return 1;
    unsigned at = atomic_load(&subscriptions);
    if (at >= 6) return 1;
    bindings[at] = (struct Binding){name, callback, data};
    atomic_store(&subscriptions, at + 1);
    return 0;
}
int LipcSetIntProperty(void *handle, const char *source, const char *name, int value) {
    (void)handle; (void)source;
    if (!strcmp(name, "deferSuspend")) fprintf(stderr, "defer:%d\n", value);
    return 0;
}
int LipcSetStringProperty(void *handle, const char *source, const char *name, const char *value) {
    (void)handle; (void)source; (void)name; (void)value;
    return getenv("FAKE_UNLOAD_FAIL") ? 1 : 0;
}
int LipcGetStringProperty(void *handle, const char *source, const char *name, char **value) {
    (void)handle; (void)source; (void)name;
    *value = strdup("langpicker blankwindow");
    return *value ? 0 : 1;
}
void LipcFreeString(char *value) { free(value); }
