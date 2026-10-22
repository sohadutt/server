#include "rate_limit.h"

#include <stdio.h>
#include <string.h>

#define RATE_WINDOW_SECONDS 60U

static const unsigned action_limits[] = {12U, 5U, 30U, 300U, 120U, 3U, 10U};

int rate_limiter_allow(RateLimiter *limiter, const char *key,
                       RateLimitKind kind, uint64_t now_seconds) {
  if (limiter == NULL || key == NULL || key[0] == '\0' ||
      strlen(key) >= RATE_LIMITER_KEY_CAPACITY || kind < RATE_LIMIT_LOGIN ||
      kind > RATE_LIMIT_MALFORMED)
    return 0;

  if (pthread_mutex_lock(&limiter->mutex) != 0)
    return 0;

  RateLimitEntry *entry = NULL;
  RateLimitEntry *empty = NULL;
  RateLimitEntry *expired = NULL;
  for (size_t i = 0; i < RATE_LIMITER_CAPACITY; i++) {
    RateLimitEntry *candidate = &limiter->entries[i];
    if (strcmp(candidate->key, key) == 0) {
      entry = candidate;
      break;
    }
    if (candidate->key[0] == '\0') {
      if (empty == NULL)
        empty = candidate;
      continue;
    }
    if (now_seconds >= candidate->last_seen &&
        now_seconds - candidate->last_seen >= RATE_WINDOW_SECONDS &&
        (expired == NULL || candidate->last_seen < expired->last_seen))
      expired = candidate;
  }

  if (entry == NULL)
    entry = empty != NULL ? empty : expired;
  if (entry == NULL) {
    /* Fail closed if the bounded table is full of active client entries. */
    pthread_mutex_unlock(&limiter->mutex);
    return 0;
  }
  if (strcmp(entry->key, key) != 0) {
    memset(entry, 0, sizeof(*entry));
    (void)snprintf(entry->key, sizeof(entry->key), "%s", key);
    entry->window_start = now_seconds;
  } else if (now_seconds < entry->window_start ||
             now_seconds - entry->window_start >= RATE_WINDOW_SECONDS) {
    memset(entry->counts, 0, sizeof(entry->counts));
    entry->window_start = now_seconds;
  }

  entry->last_seen = now_seconds;
  unsigned *count = &entry->counts[(size_t)kind];
  int allowed = *count < action_limits[(size_t)kind];
  if (allowed)
    (*count)++;

  pthread_mutex_unlock(&limiter->mutex);
  return allowed;
}
