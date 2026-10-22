#include "rate_limit.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  RateLimiter limiter = RATE_LIMITER_INITIALIZER;

  for (unsigned i = 0; i < 12; i++)
    assert(rate_limiter_allow(&limiter, "192.0.2.10", RATE_LIMIT_LOGIN, 100));
  assert(!rate_limiter_allow(&limiter, "192.0.2.10", RATE_LIMIT_LOGIN, 100));
  assert(rate_limiter_allow(&limiter, "192.0.2.10", RATE_LIMIT_LOGIN, 160));
  assert(rate_limiter_allow(&limiter, "192.0.2.11", RATE_LIMIT_LOGIN, 160));

  for (unsigned i = 0; i < 5; i++)
    assert(rate_limiter_allow(&limiter, "192.0.2.10", RATE_LIMIT_SIGNUP, 161));
  assert(!rate_limiter_allow(&limiter, "192.0.2.10", RATE_LIMIT_SIGNUP, 161));

  assert(!rate_limiter_allow(&limiter, "", RATE_LIMIT_ADMIN, 100));
  for (unsigned i = 0; i < 30; i++)
    assert(rate_limiter_allow(&limiter, "192.0.2.10", RATE_LIMIT_ADMIN, 161));
  assert(!rate_limiter_allow(&limiter, "192.0.2.10", RATE_LIMIT_ADMIN, 161));

  for (unsigned i = 0; i < 120; i++)
    assert(rate_limiter_allow(&limiter, "sess:account-a", RATE_LIMIT_SESSION,
                              200));
  assert(!rate_limiter_allow(&limiter, "sess:account-a", RATE_LIMIT_SESSION,
                             200));
  assert(rate_limiter_allow(&limiter, "sess:account-b", RATE_LIMIT_SESSION,
                            200));
  for (unsigned i = 0; i < 3; i++)
    assert(rate_limiter_allow(&limiter, "sess:account-a", RATE_LIMIT_PRINT,
                              200));
  assert(!rate_limiter_allow(&limiter, "sess:account-a", RATE_LIMIT_PRINT,
                             200));
  for (unsigned i = 0; i < 10; i++)
    assert(rate_limiter_allow(&limiter, "192.0.2.10", RATE_LIMIT_MALFORMED,
                              200));
  assert(!rate_limiter_allow(&limiter, "192.0.2.10", RATE_LIMIT_MALFORMED,
                             200));

  assert(pthread_mutex_destroy(&limiter.mutex) == 0);

  RateLimiter full = RATE_LIMITER_INITIALIZER;
  char key[RATE_LIMITER_KEY_CAPACITY];
  for (unsigned i = 0; i < RATE_LIMITER_CAPACITY; i++) {
    assert(snprintf(key, sizeof(key), "client-%u", i) > 0);
    assert(rate_limiter_allow(&full, key, RATE_LIMIT_LOGIN, 100));
  }
  assert(!rate_limiter_allow(&full, "new-client", RATE_LIMIT_LOGIN, 101));
  assert(rate_limiter_allow(&full, "new-client", RATE_LIMIT_LOGIN, 160));
  assert(pthread_mutex_destroy(&full.mutex) == 0);
  return 0;
}
