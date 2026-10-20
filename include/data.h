#ifndef DATA_H
#define DATA_H

#include <stddef.h>
#include <stdint.h>

typedef struct DataStore DataStore;

typedef enum {
  DATA_OK = 0,
  DATA_ERROR = -1,
  DATA_INVALID = -2,
  DATA_NOT_FOUND = -3,
  DATA_DUPLICATE = -4,
  DATA_UNAUTHORIZED = -5,
  DATA_INSUFFICIENT_CREDITS = -6,
  DATA_BUFFER_TOO_SMALL = -7
} DataResult;

#define DATA_PHONE_CAPACITY 17U
#define DATA_TOKEN_CAPACITY 44U
#define DATA_REFERENCE_CAPACITY 129U

typedef struct {
  int64_t id;
  char phone_e164[DATA_PHONE_CAPACITY];
  int64_t credits;
  int64_t created_at;
  int active;
} DataUser;

/* Opens or creates a private SQLite database and initializes its schema. */
DataStore *data_store_open(const char *path);
void data_store_close(DataStore *store);

/* Phone numbers are user IDs and must use E.164 format, for example +14155550123. */
DataResult data_user_create(DataStore *store, const char *phone_e164,
                            DataUser *user, char *token, size_t token_capacity);
DataResult data_user_get(DataStore *store, const char *phone_e164,
                         DataUser *user);
DataResult data_user_list(DataStore *store, DataUser **users, size_t *count);
void data_user_list_free(DataUser *users);
DataResult data_user_set_active(DataStore *store, const char *phone_e164,
                                int active);
/* Passwords are salted PBKDF2-HMAC-SHA256 hashes; plaintext is never stored. */
DataResult data_user_set_password(DataStore *store, const char *phone_e164,
                                  const char *password);
DataResult data_user_create_account(DataStore *store, const char *phone_e164,
                                    const char *password, DataUser *user);
DataResult data_user_register(DataStore *store, const char *phone_e164,
                              const char *password, char *token,
                              size_t token_capacity, DataUser *user);
DataResult data_user_login(DataStore *store, const char *phone_e164,
                          const char *password, char *token,
                          size_t token_capacity, DataUser *user);

/* Random bearer sessions expire after 30 days; only SHA-256 digests are stored. */
DataResult data_token_issue(DataStore *store, const char *phone_e164,
                            char *token, size_t token_capacity);
DataResult data_token_revoke(DataStore *store, const char *phone_e164,
                             const char *token);
DataResult data_token_revoke_all(DataStore *store, const char *phone_e164);
DataResult data_token_authenticate(DataStore *store, const char *token,
                                   DataUser *user);

/* Credit values are integer application units, never floating-point money. */
DataResult data_credit_add(DataStore *store, const char *phone_e164,
                           int64_t credits, const char *reference,
                           const char *note, int64_t *balance_after);
DataResult data_credit_charge(DataStore *store, const char *phone_e164,
                              int64_t credits, const char *reference,
                              const char *note, int64_t *balance_after);

#endif
