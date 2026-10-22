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

  int64_t print_job_id = 0;
  assert(data_print_job_record(store, "+14155550123", 42, "Office Printer",
                               "application/pdf", 2,
                               "two-sided-long-edge", "1-3,5",
                               "test-batch-1", "notes.pdf", "request-1",
                               &print_job_id) == DATA_OK);
  assert(print_job_id > 0);
  DataPrintJob *print_jobs = NULL;
  size_t print_job_count = 0;
  assert(data_print_job_list(store, "+14155550123", &print_jobs,
                             &print_job_count) == DATA_OK);
  assert(print_job_count == 1);
  assert(print_jobs[0].id == print_job_id);
  assert(print_jobs[0].cups_job_id == 42);
  assert(strcmp(print_jobs[0].printer, "Office Printer") == 0);
  assert(strcmp(print_jobs[0].pages, "1-3,5") == 0);
  assert(strcmp(print_jobs[0].batch_id, "test-batch-1") == 0);
  assert(strcmp(print_jobs[0].document_name, "notes.pdf") == 0);
  assert(strcmp(print_jobs[0].request_id, "request-1") == 0);
  assert(strcmp(print_jobs[0].status, "submitted") == 0);
  data_print_job_list_free(print_jobs);
  DataPrintJob existing_job = {0};
  assert(data_print_job_find_request(store, "+14155550123", "request-1",
                                    &existing_job) == DATA_OK);
  assert(existing_job.id == print_job_id);
  assert(data_print_job_set_status(store, "+14155550123", print_job_id,
                                   "processing") == DATA_OK);
  assert(data_print_job_set_status(store, "+14155550124", print_job_id,
                                   "completed") == DATA_NOT_FOUND);
  assert(data_print_job_get(store, "+14155550123", print_job_id,
                            &existing_job) == DATA_OK);
  assert(strcmp(existing_job.status, "processing") == 0);
  assert(data_print_job_list(store, "+14155550124", &print_jobs,
                             &print_job_count) == DATA_OK);
  assert(print_job_count == 0 && print_jobs == NULL);

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
