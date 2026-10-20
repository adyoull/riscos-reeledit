/* count_threads.so - LD_PRELOAD into a program to count the threads it
 * starts: "count_threads: N" goes to stderr when it exits. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>

static int created = 0;

int pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*f)(void *), void *arg)
{
    static int (*real)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
    if (!real)
        real = dlsym(RTLD_NEXT, "pthread_create");
    created++;
    return real(t, a, f, arg);
}

__attribute__((destructor)) static void report(void)
{
    fprintf(stderr, "count_threads: %d\n", created);
}
