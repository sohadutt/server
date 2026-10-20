#include "data.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void remove_database(const char *path) {
  char sidecar[512];
  unlink(path);
  assert(snprintf(sidecar, sizeof(sidecar), "%s-wal", path) > 0);
  unlink(sidecar);
  assert(snprintf(sidecar, sizeof(sidecar), "%s-shm", path) > 0);
  unlink(sidecar);
}

int main(void) {
  char path[] = "server-data-test-XXXXXX";
  int descriptor = mkstemp(path);
  assert(descriptor >= 0);
  close(descriptor);

  DataStore *store = data_store_open(path);
  assert(store != NULL);

  DataUser user = {0};
  char token[DATA_TOKEN_CAPACITY] = {0};
  assert(data_user_register(store, "bad", "valid-pass-123", token,
                            sizeof(token), &user) == DATA_INVALID);
  assert(data_user_register(store, "+14155550123", "short", token,
                            sizeof(token), &user) == DATA_INVALID);

  assert(data_user_register(store, "+14155550123", "correct-horse-battery",
                            token, sizeof(token), &user) == DATA_OK);
  assert(strcmp(user.phone_e164, "+14155550123") == 0);
  assert(user.active == 1 && user.credits == 0);
  assert(strlen(token) == DATA_TOKEN_CAPACITY - 1);
  assert(data_token_authenticate(store, token, &user) == DATA_OK);

  char duplicate_token[DATA_TOKEN_CAPACITY] = {0};
  assert(data_user_register(store, "+14155550123", "correct-horse-battery",
                            duplicate_token, sizeof(duplicate_token), &user) ==
         DATA_DUPLICATE);
  assert(data_user_login(store, "+14155550123", "wrong-password", token,
                         sizeof(token), &user) == DATA_UNAUTHORIZED);
  assert(data_user_login(store, "+14155550123", "correct-horse-battery",
                         duplicate_token, sizeof(duplicate_token), &user) ==
         DATA_OK);
  assert(data_token_authenticate(store, duplicate_token, &user) == DATA_OK);

  int64_t balance = -1;
  assert(data_credit_add(store, "+14155550123", 100, "test-topup", "unit test",
                         &balance) == DATA_OK);
  assert(balance == 100);
  assert(data_credit_add(store, "+14155550123", 100, "test-topup", "retry",
                         &balance) == DATA_DUPLICATE);
  assert(data_credit_charge(store, "+14155550123", 30, "test-print", "print",
                            &balance) == DATA_OK);
  assert(balance == 70);
  assert(data_credit_charge(store, "+14155550123", 100, "too-large", "print",
                            &balance) == DATA_INSUFFICIENT_CREDITS);

  assert(data_user_set_active(store, "+14155550123", 0) == DATA_OK);
  assert(data_token_authenticate(store, duplicate_token, &user) ==
         DATA_UNAUTHORIZED);
  assert(data_user_login(store, "+14155550123", "correct-horse-battery", token,
                         sizeof(token), &user) == DATA_UNAUTHORIZED);
  assert(data_user_set_active(store, "+14155550123", 1) == DATA_OK);

  DataUser *users = NULL;
  size_t count = 0;
  assert(data_user_list(store, &users, &count) == DATA_OK);
  assert(count == 1);
  assert(users[0].credits == 70);
  assert(users[0].active == 1);
  data_user_list_free(users);

  data_store_close(store);
  remove_database(path);
  puts("data store unit tests passed");
  return 0;
}
