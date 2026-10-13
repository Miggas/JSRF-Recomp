/* Linux test shim: just what recomp_manual.c's heap checker uses. */
#pragma once
#include <pthread.h>
typedef pthread_mutex_t CRITICAL_SECTION;
static inline void InitializeCriticalSection(CRITICAL_SECTION *c) { pthread_mutex_init(c, 0); }
static inline void EnterCriticalSection(CRITICAL_SECTION *c) { pthread_mutex_lock(c); }
static inline void LeaveCriticalSection(CRITICAL_SECTION *c) { pthread_mutex_unlock(c); }
