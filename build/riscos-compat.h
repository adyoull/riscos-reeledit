/* riscos-compat.h - what MLT uses that UnixLib 5.0.3.3 doesn't have.
 *
 * Included first in every MLT source file (-include) by
 * build/build-riscos.sh. These are POSIX thread attributes for scheduling,
 * which UnixLib's threads don't have: one CPU, one scope, switched by a
 * ticker. They are accepted and do nothing. MLT is built with
 * MLT_NO_THREADS, so they're only reached if threads are turned back on
 * (MLT_NO_THREADS=0).
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#ifndef RISCOS_COMPAT_H
#define RISCOS_COMPAT_H
#include <pthread.h>
#include <sched.h>

#ifndef PTHREAD_SCOPE_SYSTEM
#define PTHREAD_SCOPE_SYSTEM 0
#define PTHREAD_SCOPE_PROCESS 1
static inline int pthread_attr_setscope(pthread_attr_t *attr, int scope)
{
    (void) attr;
    (void) scope;
    return 0;
}
#endif

#ifndef PTHREAD_EXPLICIT_SCHED
#define PTHREAD_INHERIT_SCHED 0
#define PTHREAD_EXPLICIT_SCHED 1
static inline int pthread_attr_setinheritsched(pthread_attr_t *attr, int inherit)
{
    (void) attr;
    (void) inherit;
    return 0;
}
static inline int pthread_attr_setschedpolicy(pthread_attr_t *attr, int policy)
{
    (void) attr;
    (void) policy;
    return 0;
}
#endif

#endif
