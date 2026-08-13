#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <pthread.h>

#include "core/config.h"

#define WD_MAX_SLOTS 8

int watchdog_register_thread(pthread_t tid, int timeout, int max_miss);
int watchdog_unregister_thread(pthread_t tid);
int watchdog_feed_thread(pthread_t tid);
int watchdog_init(void *arg);
void *watchdog_task(void *arg);

#endif /* WATCHDOG_H */
