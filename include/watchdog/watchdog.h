#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <pthread.h>

#include "core/config.h"

#define WD_MAX_SLOTS 8
#define WD_NAME_MAX 32

int watchdog_register_thread(pthread_t tid, const char *name, int timeout, int max_miss);
int watchdog_unregister_thread(pthread_t tid);
int watchdog_feed_thread(pthread_t tid, const char *name);

static inline int watchdog_feed_self(const char *name)
{
    return watchdog_feed_thread(pthread_self(), name);
}
int watchdog_init(void *arg);
void *watchdog_task(void *arg);

#endif /* WATCHDOG_H */
