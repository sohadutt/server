#ifndef RATE_LIMIT_H
#define RATE_LIMIT_H

#include <pthread.h>
#include <stdint.h>

#define RATE_LIMITER_CAPACITY 1024U
#define RATE_LIMITER_KEY_CAPACITY 96U

typedef enum {
  RATE_LIMIT_LOGIN = 0,
  RATE_LIMIT_SIGNUP = 1,
  RATE_LIMIT_ADMIN = 2,
  RATE_LIMIT_API = 3,
  RATE_LIMIT_SESSION = 4,
  RATE_LIMIT_PRINT = 5,
  RATE_LIMIT_MALFORMED = 6
} RateLimitKind;

typedef struct {
  char key[RATE_LIMITER_KEY_CAPACITY];
  uint64_t window_start;
  uint64_t last_seen;
  unsigned counts[7];
} RateLimitEntry;

typedef struct {
  pthread_mutex_t mutex;
  RateLimitEntry entries[RATE_LIMITER_CAPACITY];
} RateLimiter;

#define RATE_LIMITER_INITIALIZER { .mutex = PTHREAD_MUTEX_INITIALIZER }

/* Returns 1 while below the action limit, 0 after the limit is reached. */
int rate_limiter_allow(RateLimiter *limiter, const char *key,
                       RateLimitKind kind, uint64_t now_seconds);

#endif
