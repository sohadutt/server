#include "data.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <pthread.h>
#include <sqlite3.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

struct DataStore {
  sqlite3 *database;
  pthread_mutex_t mutex;
};

static const char schema_sql[] =
    "CREATE TABLE users ("
    " id INTEGER PRIMARY KEY,"
    " phone_e164 TEXT NOT NULL UNIQUE,"
    " credits INTEGER NOT NULL DEFAULT 0 CHECK(credits >= 0),"
    " active INTEGER NOT NULL DEFAULT 1 CHECK(active IN (0, 1)),"
    " password_salt BLOB CHECK(password_salt IS NULL OR length(password_salt) = 16),"
    " password_hash BLOB CHECK(password_hash IS NULL OR length(password_hash) = 32),"
    " created_at INTEGER NOT NULL"
    ");"
    "CREATE TABLE user_tokens ("
    " id INTEGER PRIMARY KEY,"
    " user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,"
    " token_hash BLOB NOT NULL UNIQUE CHECK(length(token_hash) = 32),"
    " created_at INTEGER NOT NULL,"
    " expires_at INTEGER,"
    " revoked_at INTEGER,"
    " last_used_at INTEGER"
    ");"
    "CREATE INDEX user_tokens_user_id ON user_tokens(user_id);"
    "CREATE TABLE credit_ledger ("
    " id INTEGER PRIMARY KEY,"
    " user_id INTEGER NOT NULL REFERENCES users(id),"
    " amount INTEGER NOT NULL CHECK(amount != 0),"
    " balance_after INTEGER NOT NULL CHECK(balance_after >= 0),"
    " entry_type TEXT NOT NULL CHECK(entry_type IN"
    " ('topup', 'print', 'adjustment', 'refund')),"
    " reference TEXT,"
    " note TEXT,"
    " created_at INTEGER NOT NULL"
    ");"
    "CREATE UNIQUE INDEX credit_ledger_reference"
    " ON credit_ledger(user_id, reference) WHERE reference IS NOT NULL;"
    "CREATE INDEX credit_ledger_user_time"
    " ON credit_ledger(user_id, created_at DESC);"
    "PRAGMA user_version = 2;";

static int valid_phone(const char *phone) {
  if (phone == NULL || phone[0] != '+')
    return 0;
  size_t digits = 0;
  for (size_t i = 1; phone[i] != '\0'; i++) {
    if (phone[i] < '0' || phone[i] > '9')
      return 0;
    digits++;
  }
  return digits >= 8 && digits <= 15 && phone[1] != '0';
}

static int valid_optional_text(const char *value, size_t maximum_length) {
  return value == NULL || strlen(value) <= maximum_length;
}

static int execute_sql(sqlite3 *database, const char *sql) {
  return sqlite3_exec(database, sql, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

static DataResult map_sqlite_result(int result) {
  if (result == SQLITE_CONSTRAINT_UNIQUE ||
      result == SQLITE_CONSTRAINT_PRIMARYKEY)
    return DATA_DUPLICATE;
  if (result == SQLITE_OK || result == SQLITE_DONE || result == SQLITE_ROW)
    return DATA_OK;
  return DATA_ERROR;
}

static int transaction_begin(DataStore *store) {
  return execute_sql(store->database, "BEGIN IMMEDIATE;");
}

static void transaction_rollback(DataStore *store) {
  (void)execute_sql(store->database, "ROLLBACK;");
}

static DataResult transaction_commit(DataStore *store) {
  if (execute_sql(store->database, "COMMIT;") == 0)
    return DATA_OK;
  transaction_rollback(store);
  return DATA_ERROR;
}

static void fill_user(sqlite3_stmt *statement, DataUser *user) {
  const unsigned char *phone = sqlite3_column_text(statement, 1);
  *user = (DataUser){
      .id = sqlite3_column_int64(statement, 0),
      .credits = sqlite3_column_int64(statement, 2),
      .created_at = sqlite3_column_int64(statement, 3),
      .active = sqlite3_column_int(statement, 4),
  };
  if (phone != NULL)
    snprintf(user->phone_e164, sizeof(user->phone_e164), "%s", phone);
}

static DataResult query_user_by_phone(DataStore *store, const char *phone,
                                      DataUser *user) {
  static const char sql[] =
      "SELECT id, phone_e164, credits, created_at, active FROM users "
      "WHERE phone_e164 = ?1;";
  sqlite3_stmt *statement = NULL;
  int result = sqlite3_prepare_v2(store->database, sql, -1, &statement, NULL);
  if (result != SQLITE_OK)
    return DATA_ERROR;
  result = sqlite3_bind_text(statement, 1, phone, -1, SQLITE_TRANSIENT);
  if (result == SQLITE_OK)
    result = sqlite3_step(statement);
  DataResult status = DATA_NOT_FOUND;
  if (result == SQLITE_ROW) {
    fill_user(statement, user);
    status = DATA_OK;
  } else if (result != SQLITE_DONE) {
    status = DATA_ERROR;
  }
  sqlite3_finalize(statement);
  return status;
}

static int encode_token(const unsigned char *bytes, size_t length, char *output,
                        size_t capacity) {
  static const char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  size_t out = 0;
  unsigned int accumulator = 0;
  unsigned int bits = 0;
  for (size_t i = 0; i < length; i++) {
    accumulator = (accumulator << 8) | bytes[i];
    bits += 8;
    while (bits >= 6) {
      bits -= 6;
      if (out + 1 >= capacity)
        return -1;
      output[out++] = alphabet[(accumulator >> bits) & 0x3fU];
    }
  }
  if (bits > 0) {
    if (out + 1 >= capacity)
      return -1;
    output[out++] = alphabet[(accumulator << (6U - bits)) & 0x3fU];
  }
  if (out >= capacity)
    return -1;
  output[out] = '\0';
  return 0;
}

static int create_token(char *token, size_t token_capacity,
                        unsigned char digest[SHA256_DIGEST_LENGTH]) {
  unsigned char random_bytes[32];
  if (token == NULL || token_capacity < DATA_TOKEN_CAPACITY ||
      RAND_bytes(random_bytes, (int)sizeof(random_bytes)) != 1) {
    OPENSSL_cleanse(random_bytes, sizeof(random_bytes));
    return -1;
  }
  if (encode_token(random_bytes, sizeof(random_bytes), token, token_capacity) !=
      0) {
    OPENSSL_cleanse(random_bytes, sizeof(random_bytes));
    return -1;
  }
  if (SHA256((const unsigned char *)token, strlen(token), digest) == NULL) {
    OPENSSL_cleanse(random_bytes, sizeof(random_bytes));
    OPENSSL_cleanse(token, token_capacity);
    return -1;
  }
  OPENSSL_cleanse(random_bytes, sizeof(random_bytes));
  return 0;
}

static int insert_token(DataStore *store, int64_t user_id,
                        const unsigned char digest[SHA256_DIGEST_LENGTH]) {
  static const char sql[] =
      "INSERT INTO user_tokens(user_id, token_hash, created_at, expires_at) "
      "VALUES(?1, ?2, CAST(strftime('%s', 'now') AS INTEGER), "
      "CAST(strftime('%s', 'now') AS INTEGER) + 2592000);";
  sqlite3_stmt *statement = NULL;
  int result = sqlite3_prepare_v2(store->database, sql, -1, &statement, NULL);
  if (result == SQLITE_OK)
    result = sqlite3_bind_int64(statement, 1, user_id);
  if (result == SQLITE_OK)
    result = sqlite3_bind_blob(statement, 2, digest, SHA256_DIGEST_LENGTH,
                               SQLITE_TRANSIENT);
  if (result == SQLITE_OK)
    result = sqlite3_step(statement);
  sqlite3_finalize(statement);
  return result == SQLITE_DONE ? SQLITE_OK : result;
}

static int schema_version(sqlite3 *database) {
  sqlite3_stmt *statement = NULL;
  int result = sqlite3_prepare_v2(database, "PRAGMA user_version;", -1,
                                  &statement, NULL);
  if (result == SQLITE_OK)
    result = sqlite3_step(statement);
  int version = result == SQLITE_ROW ? sqlite3_column_int(statement, 0) : -1;
  sqlite3_finalize(statement);
  return version;
}

static int schema_initialize(sqlite3 *database) {
  int version = schema_version(database);
  if (version == 2)
    return 0;
  if ((version != 0 && version != 1) ||
      execute_sql(database, "BEGIN EXCLUSIVE;") != 0)
    return -1;
  version = schema_version(database);
  if (version == 2)
    return execute_sql(database, "COMMIT;");
  if (version == 1) {
    if (execute_sql(database,
                    "ALTER TABLE users ADD COLUMN password_salt BLOB;"
                    "ALTER TABLE users ADD COLUMN password_hash BLOB;"
                    "PRAGMA user_version = 2;") != 0) {
      (void)sqlite3_exec(database, "ROLLBACK;", NULL, NULL, NULL);
      return -1;
    }
    return execute_sql(database, "COMMIT;");
  }
  if (version != 0) {
    (void)sqlite3_exec(database, "ROLLBACK;", NULL, NULL, NULL);
    return -1;
  }
  if (execute_sql(database, schema_sql) != 0) {
    (void)sqlite3_exec(database, "ROLLBACK;", NULL, NULL, NULL);
    return -1;
  }
  if (execute_sql(database, "COMMIT;") != 0) {
    (void)sqlite3_exec(database, "ROLLBACK;", NULL, NULL, NULL);
    return -1;
  }
  return 0;
}

DataStore *data_store_open(const char *path) {
  if (path == NULL || path[0] == '\0')
    return NULL;
  char parent[512];
  size_t path_length = strlen(path);
  if (path_length >= sizeof(parent))
    return NULL;
  memcpy(parent, path, path_length + 1);
  char *slash = strrchr(parent, '/');
  if (slash != NULL) {
    *slash = '\0';
    if (parent[0] != '\0') {
      struct stat directory_status;
      if (stat(parent, &directory_status) != 0) {
        if (errno != ENOENT || mkdir(parent, S_IRWXU) != 0)
          return NULL;
      } else if (!S_ISDIR(directory_status.st_mode)) {
        return NULL;
      }
    }
  }
  struct stat file_status;
  if (lstat(path, &file_status) == 0) {
    if (!S_ISREG(file_status.st_mode))
      return NULL;
  } else if (errno != ENOENT) {
    return NULL;
  }

  DataStore *store = calloc(1, sizeof(*store));
  if (store == NULL)
    return NULL;
  if (pthread_mutex_init(&store->mutex, NULL) != 0) {
    free(store);
    return NULL;
  }
  int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
#ifdef SQLITE_OPEN_NOFOLLOW
  flags |= SQLITE_OPEN_NOFOLLOW;
#endif
  if (sqlite3_open_v2(path, &store->database, flags, NULL) != SQLITE_OK)
    goto fail;
  sqlite3_extended_result_codes(store->database, 1);
  sqlite3_busy_timeout(store->database, 5000);
  if (chmod(path, S_IRUSR | S_IWUSR) != 0 ||
      execute_sql(store->database, "PRAGMA foreign_keys = ON;") != 0 ||
      execute_sql(store->database, "PRAGMA journal_mode = WAL;") != 0 ||
      execute_sql(store->database, "PRAGMA synchronous = FULL;") != 0 ||
      execute_sql(store->database, "PRAGMA secure_delete = ON;") != 0 ||
      execute_sql(store->database, "PRAGMA trusted_schema = OFF;") != 0 ||
      schema_initialize(store->database) != 0)
    goto fail;
  if (sqlite3_db_config(store->database, SQLITE_DBCONFIG_DEFENSIVE, 1, NULL) !=
      SQLITE_OK)
    goto fail;
  return store;

fail:
  if (store->database != NULL)
    sqlite3_close(store->database);
  pthread_mutex_destroy(&store->mutex);
  free(store);
  return NULL;
}

void data_store_close(DataStore *store) {
  if (store == NULL)
    return;
  pthread_mutex_lock(&store->mutex);
  sqlite3 *database = store->database;
  store->database = NULL;
  pthread_mutex_unlock(&store->mutex);
  if (database != NULL)
    sqlite3_close(database);
  pthread_mutex_destroy(&store->mutex);
  free(store);
}

DataResult data_user_create(DataStore *store, const char *phone_e164,
                            DataUser *user, char *token,
                            size_t token_capacity) {
  if (store == NULL || user == NULL || !valid_phone(phone_e164))
    return DATA_INVALID;
  if (token == NULL || token_capacity < DATA_TOKEN_CAPACITY)
    return DATA_BUFFER_TOO_SMALL;

  unsigned char digest[SHA256_DIGEST_LENGTH];
  char generated_token[DATA_TOKEN_CAPACITY];
  if (create_token(generated_token, sizeof(generated_token), digest) != 0)
    return DATA_ERROR;

  pthread_mutex_lock(&store->mutex);
  DataResult status = DATA_ERROR;
  if (transaction_begin(store) != 0)
    goto done;
  sqlite3_stmt *statement = NULL;
  int result = sqlite3_prepare_v2(
      store->database,
      "INSERT INTO users(phone_e164, created_at) "
      "VALUES(?1, CAST(strftime('%s', 'now') AS INTEGER));",
      -1, &statement, NULL);
  if (result == SQLITE_OK)
    result = sqlite3_bind_text(statement, 1, phone_e164, -1, SQLITE_TRANSIENT);
  if (result == SQLITE_OK)
    result = sqlite3_step(statement);
  sqlite3_finalize(statement);
  if (result != SQLITE_DONE) {
    status = map_sqlite_result(result);
    transaction_rollback(store);
    goto done;
  }
  int64_t user_id = sqlite3_last_insert_rowid(store->database);
  result = insert_token(store, user_id, digest);
  if (result != SQLITE_OK) {
    status = map_sqlite_result(result);
    transaction_rollback(store);
    goto done;
  }
  status = query_user_by_phone(store, phone_e164, user);
  if (status != DATA_OK) {
    transaction_rollback(store);
    goto done;
  }
  status = transaction_commit(store);
  if (status == DATA_OK)
    memcpy(token, generated_token, sizeof(generated_token));

done:
  pthread_mutex_unlock(&store->mutex);
  OPENSSL_cleanse(digest, sizeof(digest));
  OPENSSL_cleanse(generated_token, sizeof(generated_token));
  return status;
}

DataResult data_user_get(DataStore *store, const char *phone_e164,
                         DataUser *user) {
  if (store == NULL || user == NULL || !valid_phone(phone_e164))
    return DATA_INVALID;
  pthread_mutex_lock(&store->mutex);
  DataResult status = query_user_by_phone(store, phone_e164, user);
  pthread_mutex_unlock(&store->mutex);
  return status;
}

DataResult data_user_list(DataStore *store, DataUser **users, size_t *count) {
  if (store == NULL || users == NULL || count == NULL)
    return DATA_INVALID;
  *users = NULL;
  *count = 0;
  pthread_mutex_lock(&store->mutex);
  sqlite3_stmt *statement = NULL;
  int result = sqlite3_prepare_v2(
      store->database,
      "SELECT id, phone_e164, credits, created_at, active FROM users "
      "ORDER BY phone_e164;",
      -1, &statement, NULL);
  DataResult status = result == SQLITE_OK ? DATA_OK : DATA_ERROR;
  size_t capacity = 0;
  while (status == DATA_OK && (result = sqlite3_step(statement)) == SQLITE_ROW) {
    if (*count == capacity) {
      size_t next_capacity = capacity == 0 ? 16 : capacity * 2;
      if (next_capacity < capacity || next_capacity > SIZE_MAX / sizeof(**users)) {
        status = DATA_ERROR;
        break;
      }
      DataUser *grown = realloc(*users, next_capacity * sizeof(**users));
      if (grown == NULL) {
        status = DATA_ERROR;
        break;
      }
      *users = grown;
      capacity = next_capacity;
    }
    fill_user(statement, &(*users)[*count]);
    (*count)++;
  }
  if (status == DATA_OK && result != SQLITE_DONE)
    status = DATA_ERROR;
  sqlite3_finalize(statement);
  pthread_mutex_unlock(&store->mutex);
  if (status != DATA_OK) {
    free(*users);
    *users = NULL;
    *count = 0;
  }
  return status;
}

void data_user_list_free(DataUser *users) { free(users); }

DataResult data_user_set_active(DataStore *store, const char *phone_e164,
                                int active) {
  if (store == NULL || !valid_phone(phone_e164) ||
      (active != 0 && active != 1))
    return DATA_INVALID;
  pthread_mutex_lock(&store->mutex);
  if (transaction_begin(store) != 0) {
    pthread_mutex_unlock(&store->mutex);
    return DATA_ERROR;
  }
  sqlite3_stmt *statement = NULL;
  int result = sqlite3_prepare_v2(
      store->database, "UPDATE users SET active = ?1 WHERE phone_e164 = ?2;",
      -1, &statement, NULL);
  if (result == SQLITE_OK)
    result = sqlite3_bind_int(statement, 1, active);
  if (result == SQLITE_OK)
    result = sqlite3_bind_text(statement, 2, phone_e164, -1, SQLITE_TRANSIENT);
  if (result == SQLITE_OK)
    result = sqlite3_step(statement);
  int changed = sqlite3_changes(store->database);
  sqlite3_finalize(statement);
  if (result != SQLITE_DONE) {
    transaction_rollback(store);
    pthread_mutex_unlock(&store->mutex);
    return DATA_ERROR;
  }
  if (changed != 1) {
    transaction_rollback(store);
    pthread_mutex_unlock(&store->mutex);
    return DATA_NOT_FOUND;
  }
  if (!active) {
    result = sqlite3_prepare_v2(
        store->database,
        "UPDATE user_tokens SET revoked_at = "
        "CAST(strftime('%s', 'now') AS INTEGER) WHERE user_id = "
        "(SELECT id FROM users WHERE phone_e164 = ?1) AND revoked_at IS NULL;",
        -1, &statement, NULL);
    if (result == SQLITE_OK)
      result = sqlite3_bind_text(statement, 1, phone_e164, -1, SQLITE_TRANSIENT);
    if (result == SQLITE_OK)
      result = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE) {
      transaction_rollback(store);
      pthread_mutex_unlock(&store->mutex);
      return DATA_ERROR;
    }
  }
  DataResult status = transaction_commit(store);
  pthread_mutex_unlock(&store->mutex);
  return status;
}

#define PASSWORD_SALT_SIZE 16
#define PASSWORD_HASH_SIZE 32
#define PASSWORD_ITERATIONS 310000

static int password_valid(const char *password) {
  if (password == NULL)
    return 0;
  size_t length = strlen(password);
  return length >= 8 && length <= 128;
}

static int password_hash(const char *password,
                         const unsigned char salt[PASSWORD_SALT_SIZE],
                         unsigned char hash[PASSWORD_HASH_SIZE]) {
  return PKCS5_PBKDF2_HMAC(password, (int)strlen(password), salt,
                            PASSWORD_SALT_SIZE, PASSWORD_ITERATIONS,
                            EVP_sha256(), PASSWORD_HASH_SIZE, hash);
}

DataResult data_user_create_account(DataStore *store, const char *phone_e164,
                                    const char *password, DataUser *user) {
  if (store == NULL || user == NULL || !valid_phone(phone_e164) ||
      !password_valid(password))
    return DATA_INVALID;
  unsigned char salt[PASSWORD_SALT_SIZE];
  unsigned char hash[PASSWORD_HASH_SIZE];
  if (RAND_bytes(salt, sizeof(salt)) != 1 ||
      password_hash(password, salt, hash) != 1) {
    OPENSSL_cleanse(salt, sizeof(salt));
    OPENSSL_cleanse(hash, sizeof(hash));
    return DATA_ERROR;
  }
  pthread_mutex_lock(&store->mutex);
  DataResult status = DATA_ERROR;
  if (transaction_begin(store) != 0)
    goto create_done;
  sqlite3_stmt *statement = NULL;
  int result = sqlite3_prepare_v2(
      store->database,
      "INSERT INTO users(phone_e164, created_at, password_salt, password_hash) "
      "VALUES(?1, CAST(strftime('%s', 'now') AS INTEGER), ?2, ?3);",
      -1, &statement, NULL);
  if (result == SQLITE_OK)
    result = sqlite3_bind_text(statement, 1, phone_e164, -1, SQLITE_TRANSIENT);
  if (result == SQLITE_OK)
    result = sqlite3_bind_blob(statement, 2, salt, sizeof(salt), SQLITE_TRANSIENT);
  if (result == SQLITE_OK)
    result = sqlite3_bind_blob(statement, 3, hash, sizeof(hash), SQLITE_TRANSIENT);
  if (result == SQLITE_OK)
    result = sqlite3_step(statement);
  sqlite3_finalize(statement);
  if (result != SQLITE_DONE) {
    status = map_sqlite_result(result);
    transaction_rollback(store);
    goto create_done;
  }
  status = query_user_by_phone(store, phone_e164, user);
  if (status != DATA_OK) {
    transaction_rollback(store);
    goto create_done;
  }
  status = transaction_commit(store);

create_done:
  pthread_mutex_unlock(&store->mutex);
  OPENSSL_cleanse(salt, sizeof(salt));
  OPENSSL_cleanse(hash, sizeof(hash));
  return status;
}

DataResult data_user_register(DataStore *store, const char *phone_e164,
                              const char *password, char *token,
                              size_t token_capacity, DataUser *user) {
  if (store == NULL || user == NULL || !valid_phone(phone_e164) ||
      !password_valid(password) || token == NULL ||
      token_capacity < DATA_TOKEN_CAPACITY)
    return DATA_INVALID;

  unsigned char salt[PASSWORD_SALT_SIZE] = {0};
  unsigned char hash[PASSWORD_HASH_SIZE] = {0};
  unsigned char digest[SHA256_DIGEST_LENGTH] = {0};
  char generated[DATA_TOKEN_CAPACITY] = {0};
  if (RAND_bytes(salt, sizeof(salt)) != 1 ||
      password_hash(password, salt, hash) != 1 ||
      create_token(generated, sizeof(generated), digest) != 0) {
    OPENSSL_cleanse(salt, sizeof(salt));
    OPENSSL_cleanse(hash, sizeof(hash));
    OPENSSL_cleanse(digest, sizeof(digest));
    OPENSSL_cleanse(generated, sizeof(generated));
    return DATA_ERROR;
  }

  pthread_mutex_lock(&store->mutex);
  DataResult status = DATA_ERROR;
  if (transaction_begin(store) != 0)
    goto register_done;
  sqlite3_stmt *statement = NULL;
  int result = sqlite3_prepare_v2(
      store->database,
      "INSERT INTO users(phone_e164, created_at, password_salt, password_hash) "
      "VALUES(?1, CAST(strftime('%s', 'now') AS INTEGER), ?2, ?3);",
      -1, &statement, NULL);
  if (result == SQLITE_OK)
    result = sqlite3_bind_text(statement, 1, phone_e164, -1, SQLITE_TRANSIENT);
  if (result == SQLITE_OK)
    result = sqlite3_bind_blob(statement, 2, salt, sizeof(salt), SQLITE_TRANSIENT);
  if (result == SQLITE_OK)
    result = sqlite3_bind_blob(statement, 3, hash, sizeof(hash), SQLITE_TRANSIENT);
  if (result == SQLITE_OK)
    result = sqlite3_step(statement);
  sqlite3_finalize(statement);
  if (result != SQLITE_DONE) {
    status = map_sqlite_result(result);
    transaction_rollback(store);
    goto register_done;
  }

  int64_t user_id = sqlite3_last_insert_rowid(store->database);
  if (insert_token(store, user_id, digest) != SQLITE_OK) {
    transaction_rollback(store);
    goto register_done;
  }
  status = query_user_by_phone(store, phone_e164, user);
  if (status != DATA_OK) {
    transaction_rollback(store);
    goto register_done;
  }
  status = transaction_commit(store);
  if (status == DATA_OK)
    memcpy(token, generated, sizeof(generated));

register_done:
  pthread_mutex_unlock(&store->mutex);
  OPENSSL_cleanse(salt, sizeof(salt));
  OPENSSL_cleanse(hash, sizeof(hash));
  OPENSSL_cleanse(digest, sizeof(digest));
  OPENSSL_cleanse(generated, sizeof(generated));
  return status;
}

DataResult data_user_set_password(DataStore *store, const char *phone_e164,
                                  const char *password) {
  if (store == NULL || !valid_phone(phone_e164) || !password_valid(password))
    return DATA_INVALID;
  unsigned char salt[PASSWORD_SALT_SIZE];
  unsigned char hash[PASSWORD_HASH_SIZE];
  if (RAND_bytes(salt, sizeof(salt)) != 1 ||
      password_hash(password, salt, hash) != 1) {
    OPENSSL_cleanse(salt, sizeof(salt));
    OPENSSL_cleanse(hash, sizeof(hash));
    return DATA_ERROR;
  }
  pthread_mutex_lock(&store->mutex);
  DataResult status = DATA_ERROR;
  if (transaction_begin(store) != 0)
    goto password_done;
  sqlite3_stmt *statement = NULL;
  int result = sqlite3_prepare_v2(
      store->database,
      "UPDATE users SET password_salt = ?1, password_hash = ?2 "
      "WHERE phone_e164 = ?3;", -1, &statement, NULL);
  if (result == SQLITE_OK)
    result = sqlite3_bind_blob(statement, 1, salt, sizeof(salt), SQLITE_TRANSIENT);
  if (result == SQLITE_OK)
    result = sqlite3_bind_blob(statement, 2, hash, sizeof(hash), SQLITE_TRANSIENT);
  if (result == SQLITE_OK)
    result = sqlite3_bind_text(statement, 3, phone_e164, -1, SQLITE_TRANSIENT);
  if (result == SQLITE_OK)
    result = sqlite3_step(statement);
  int changed = sqlite3_changes(store->database);
  sqlite3_finalize(statement);
  if (result != SQLITE_DONE || changed != 1) {
    status = result == SQLITE_DONE ? DATA_NOT_FOUND : DATA_ERROR;
    transaction_rollback(store);
    goto password_done;
  }
  result = sqlite3_prepare_v2(
      store->database,
      "UPDATE user_tokens SET revoked_at = "
      "CAST(strftime('%s', 'now') AS INTEGER) WHERE user_id = "
      "(SELECT id FROM users WHERE phone_e164 = ?1) AND revoked_at IS NULL;",
      -1, &statement, NULL);
  if (result == SQLITE_OK)
    result = sqlite3_bind_text(statement, 1, phone_e164, -1, SQLITE_TRANSIENT);
  if (result == SQLITE_OK)
    result = sqlite3_step(statement);
  sqlite3_finalize(statement);
  if (result != SQLITE_DONE) {
    transaction_rollback(store);
    goto password_done;
  }
  status = transaction_commit(store);

password_done:
  pthread_mutex_unlock(&store->mutex);
  OPENSSL_cleanse(salt, sizeof(salt));
  OPENSSL_cleanse(hash, sizeof(hash));
  return status;
}

DataResult data_user_login(DataStore *store, const char *phone_e164,
                          const char *password, char *token,
                          size_t token_capacity, DataUser *user) {
  if (store == NULL || !valid_phone(phone_e164) || !password_valid(password) ||
      token == NULL || token_capacity < DATA_TOKEN_CAPACITY || user == NULL)
    return DATA_INVALID;
  unsigned char salt[PASSWORD_SALT_SIZE] = {0};
  unsigned char expected[PASSWORD_HASH_SIZE] = {0};
  unsigned char candidate[PASSWORD_HASH_SIZE] = {0};
  unsigned char digest[SHA256_DIGEST_LENGTH] = {0};
  char generated[DATA_TOKEN_CAPACITY] = {0};
  if (create_token(generated, sizeof(generated), digest) != 0)
    return DATA_ERROR;

  pthread_mutex_lock(&store->mutex);
  DataResult status = DATA_ERROR;
  if (transaction_begin(store) != 0)
    goto login_done;
  sqlite3_stmt *statement = NULL;
  int result = sqlite3_prepare_v2(
      store->database,
      "SELECT password_salt, password_hash, active FROM users "
      "WHERE phone_e164 = ?1;", -1, &statement, NULL);
  if (result == SQLITE_OK)
    result = sqlite3_bind_text(statement, 1, phone_e164, -1, SQLITE_TRANSIENT);
  if (result == SQLITE_OK)
    result = sqlite3_step(statement);
  int active = 0;
  int has_password = 0;
  if (result == SQLITE_ROW) {
    const void *salt_blob = sqlite3_column_blob(statement, 0);
    const void *hash_blob = sqlite3_column_blob(statement, 1);
    if (salt_blob != NULL && hash_blob != NULL &&
        sqlite3_column_bytes(statement, 0) == PASSWORD_SALT_SIZE &&
        sqlite3_column_bytes(statement, 1) == PASSWORD_HASH_SIZE) {
      memcpy(salt, salt_blob, sizeof(salt));
      memcpy(expected, hash_blob, sizeof(expected));
      has_password = 1;
      active = sqlite3_column_int(statement, 2);
    }
  }
  sqlite3_finalize(statement);
  if (result != SQLITE_ROW && result != SQLITE_DONE) {
    status = DATA_ERROR;
    transaction_rollback(store);
    goto login_done;
  }
  int hashed = password_hash(password, salt, candidate);
  int matches = hashed == 1 &&
                CRYPTO_memcmp(candidate, expected, sizeof(expected)) == 0;
  if (!has_password || !active || !matches) {
    status = DATA_UNAUTHORIZED;
    transaction_rollback(store);
    goto login_done;
  }
  DataUser authenticated = {0};
  status = query_user_by_phone(store, phone_e164, &authenticated);
  if (status != DATA_OK || !authenticated.active) {
    transaction_rollback(store);
    if (status == DATA_OK)
      status = DATA_UNAUTHORIZED;
    goto login_done;
  }
  int inserted = insert_token(store, authenticated.id, digest);
  if (inserted != SQLITE_OK) {
    status = map_sqlite_result(inserted);
    transaction_rollback(store);
    goto login_done;
  }
  status = transaction_commit(store);
  if (status == DATA_OK) {
    *user = authenticated;
    memcpy(token, generated, sizeof(generated));
  }

login_done:
  pthread_mutex_unlock(&store->mutex);
  OPENSSL_cleanse(salt, sizeof(salt));
  OPENSSL_cleanse(expected, sizeof(expected));
  OPENSSL_cleanse(candidate, sizeof(candidate));
  OPENSSL_cleanse(digest, sizeof(digest));
  OPENSSL_cleanse(generated, sizeof(generated));
  return status;
}

DataResult data_token_issue(DataStore *store, const char *phone_e164,
                            char *token, size_t token_capacity) {
  if (store == NULL || !valid_phone(phone_e164))
    return DATA_INVALID;
  if (token == NULL || token_capacity < DATA_TOKEN_CAPACITY)
    return DATA_BUFFER_TOO_SMALL;
  unsigned char digest[SHA256_DIGEST_LENGTH];
  char generated_token[DATA_TOKEN_CAPACITY];
  if (create_token(generated_token, sizeof(generated_token), digest) != 0)
    return DATA_ERROR;

  pthread_mutex_lock(&store->mutex);
  DataUser user = {0};
  DataResult status = query_user_by_phone(store, phone_e164, &user);
  if (status == DATA_OK && user.active) {
    int result = insert_token(store, user.id, digest);
    status = result == SQLITE_OK ? DATA_OK : map_sqlite_result(result);
    if (status == DATA_OK)
      memcpy(token, generated_token, sizeof(generated_token));
  } else if (status == DATA_OK) {
    status = DATA_UNAUTHORIZED;
  }
  pthread_mutex_unlock(&store->mutex);
  OPENSSL_cleanse(digest, sizeof(digest));
  OPENSSL_cleanse(generated_token, sizeof(generated_token));
  return status;
}

DataResult data_token_revoke(DataStore *store, const char *phone_e164,
                             const char *token) {
  if (store == NULL || !valid_phone(phone_e164) || token == NULL ||
      token[0] == '\0' || strlen(token) >= 256)
    return DATA_INVALID;
  unsigned char digest[SHA256_DIGEST_LENGTH];
  if (SHA256((const unsigned char *)token, strlen(token), digest) == NULL)
    return DATA_ERROR;
  pthread_mutex_lock(&store->mutex);
  sqlite3_stmt *statement = NULL;
  int result = sqlite3_prepare_v2(
      store->database,
      "UPDATE user_tokens SET revoked_at = "
      "CAST(strftime('%s', 'now') AS INTEGER) "
      "WHERE user_id = (SELECT id FROM users WHERE phone_e164 = ?1) "
      "AND token_hash = ?2 AND revoked_at IS NULL;",
      -1, &statement, NULL);
  if (result == SQLITE_OK)
    result = sqlite3_bind_text(statement, 1, phone_e164, -1, SQLITE_TRANSIENT);
  if (result == SQLITE_OK)
    result = sqlite3_bind_blob(statement, 2, digest, sizeof(digest),
                               SQLITE_TRANSIENT);
  if (result == SQLITE_OK)
    result = sqlite3_step(statement);
  int changed = sqlite3_changes(store->database);
  sqlite3_finalize(statement);
  pthread_mutex_unlock(&store->mutex);
  OPENSSL_cleanse(digest, sizeof(digest));
  if (result != SQLITE_DONE)
    return DATA_ERROR;
  return changed == 1 ? DATA_OK : DATA_NOT_FOUND;
}

DataResult data_token_revoke_all(DataStore *store, const char *phone_e164) {
  if (store == NULL || !valid_phone(phone_e164))
    return DATA_INVALID;
  pthread_mutex_lock(&store->mutex);
  sqlite3_stmt *statement = NULL;
  int result = sqlite3_prepare_v2(
      store->database,
      "UPDATE user_tokens SET revoked_at = "
      "CAST(strftime('%s', 'now') AS INTEGER) "
      "WHERE user_id = (SELECT id FROM users WHERE phone_e164 = ?1) "
      "AND revoked_at IS NULL;",
      -1, &statement, NULL);
  if (result == SQLITE_OK)
    result = sqlite3_bind_text(statement, 1, phone_e164, -1, SQLITE_TRANSIENT);
  if (result == SQLITE_OK)
    result = sqlite3_step(statement);
  int changed = sqlite3_changes(store->database);
  sqlite3_finalize(statement);
  pthread_mutex_unlock(&store->mutex);
  if (result != SQLITE_DONE)
    return DATA_ERROR;
  return changed > 0 ? DATA_OK : DATA_NOT_FOUND;
}

DataResult data_token_authenticate(DataStore *store, const char *token,
                                   DataUser *user) {
  if (store == NULL || token == NULL || token[0] == '\0' ||
      strlen(token) >= 256 || user == NULL)
    return DATA_INVALID;
  unsigned char digest[SHA256_DIGEST_LENGTH];
  if (SHA256((const unsigned char *)token, strlen(token), digest) == NULL)
    return DATA_ERROR;
  pthread_mutex_lock(&store->mutex);
  static const char sql[] =
      "SELECT u.id, u.phone_e164, u.credits, u.created_at, u.active, t.id "
      "FROM user_tokens AS t JOIN users AS u ON u.id = t.user_id "
      "WHERE t.token_hash = ?1 AND t.revoked_at IS NULL "
      "AND (t.expires_at IS NULL OR t.expires_at > "
      "CAST(strftime('%s', 'now') AS INTEGER)) "
      "AND u.active = 1;";
  sqlite3_stmt *statement = NULL;
  int result = sqlite3_prepare_v2(store->database, sql, -1, &statement, NULL);
  if (result == SQLITE_OK)
    result = sqlite3_bind_blob(statement, 1, digest, sizeof(digest),
                               SQLITE_TRANSIENT);
  if (result == SQLITE_OK)
    result = sqlite3_step(statement);
  DataResult status = DATA_UNAUTHORIZED;
  sqlite3_int64 token_id = 0;
  if (result == SQLITE_ROW) {
    fill_user(statement, user);
    token_id = sqlite3_column_int64(statement, 5);
    status = DATA_OK;
  } else if (result != SQLITE_DONE) {
    status = DATA_ERROR;
  }
  sqlite3_finalize(statement);
  if (status == DATA_OK) {
    sqlite3_stmt *update = NULL;
    int update_result = sqlite3_prepare_v2(
        store->database,
        "UPDATE user_tokens SET last_used_at = "
        "CAST(strftime('%s', 'now') AS INTEGER) WHERE id = ?1;",
        -1, &update, NULL);
    if (update_result == SQLITE_OK)
      update_result = sqlite3_bind_int64(update, 1, token_id);
    if (update_result == SQLITE_OK)
      update_result = sqlite3_step(update);
    sqlite3_finalize(update);
    status = update_result == SQLITE_DONE ? DATA_OK : DATA_ERROR;
  }
  pthread_mutex_unlock(&store->mutex);
  OPENSSL_cleanse(digest, sizeof(digest));
  return status;
}

static DataResult change_credits(DataStore *store, const char *phone_e164,
                                 int64_t credits, const char *reference,
                                 const char *note, int adding,
                                 int64_t *balance_after) {
  if (store == NULL || !valid_phone(phone_e164) || credits <= 0 ||
      !valid_optional_text(reference, DATA_REFERENCE_CAPACITY - 1) ||
      !valid_optional_text(note, 255))
    return DATA_INVALID;
  pthread_mutex_lock(&store->mutex);
  DataResult status = DATA_ERROR;
  if (transaction_begin(store) != 0)
    goto done;
  DataUser user = {0};
  status = query_user_by_phone(store, phone_e164, &user);
  if (status != DATA_OK) {
    transaction_rollback(store);
    goto done;
  }
  if (!user.active) {
    status = DATA_UNAUTHORIZED;
    transaction_rollback(store);
    goto done;
  }
  if ((adding && user.credits > INT64_MAX - credits) ||
      (!adding && user.credits < credits)) {
    status = adding ? DATA_ERROR : DATA_INSUFFICIENT_CREDITS;
    transaction_rollback(store);
    goto done;
  }
  int64_t new_balance = adding ? user.credits + credits : user.credits - credits;
  int64_t signed_amount = adding ? credits : -credits;
  const char *entry_type = adding ? "topup" : "print";

  sqlite3_stmt *statement = NULL;
  int result = sqlite3_prepare_v2(
      store->database,
      "INSERT INTO credit_ledger(user_id, amount, balance_after, entry_type, "
      "reference, note, created_at) VALUES(?1, ?2, ?3, ?4, ?5, ?6, "
      "CAST(strftime('%s', 'now') AS INTEGER));",
      -1, &statement, NULL);
  if (result == SQLITE_OK)
    result = sqlite3_bind_int64(statement, 1, user.id);
  if (result == SQLITE_OK)
    result = sqlite3_bind_int64(statement, 2, signed_amount);
  if (result == SQLITE_OK)
    result = sqlite3_bind_int64(statement, 3, new_balance);
  if (result == SQLITE_OK)
    result = sqlite3_bind_text(statement, 4, entry_type, -1, SQLITE_STATIC);
  if (result == SQLITE_OK) {
    result = reference != NULL
                 ? sqlite3_bind_text(statement, 5, reference, -1, SQLITE_TRANSIENT)
                 : sqlite3_bind_null(statement, 5);
  }
  if (result == SQLITE_OK) {
    result = note != NULL
                 ? sqlite3_bind_text(statement, 6, note, -1, SQLITE_TRANSIENT)
                 : sqlite3_bind_null(statement, 6);
  }
  if (result == SQLITE_OK)
    result = sqlite3_step(statement);
  sqlite3_finalize(statement);
  if (result != SQLITE_DONE) {
    status = map_sqlite_result(result);
    transaction_rollback(store);
    goto done;
  }

  result = sqlite3_prepare_v2(
      store->database,
      "UPDATE users SET credits = ?1 WHERE id = ?2 AND active = 1;", -1,
      &statement, NULL);
  if (result == SQLITE_OK)
    result = sqlite3_bind_int64(statement, 1, new_balance);
  if (result == SQLITE_OK)
    result = sqlite3_bind_int64(statement, 2, user.id);
  if (result == SQLITE_OK)
    result = sqlite3_step(statement);
  sqlite3_finalize(statement);
  if (result != SQLITE_DONE || sqlite3_changes(store->database) != 1) {
    transaction_rollback(store);
    status = DATA_ERROR;
    goto done;
  }
  status = transaction_commit(store);
  if (status == DATA_OK && balance_after != NULL)
    *balance_after = new_balance;

done:
  pthread_mutex_unlock(&store->mutex);
  return status;
}

DataResult data_credit_add(DataStore *store, const char *phone_e164,
                           int64_t credits, const char *reference,
                           const char *note, int64_t *balance_after) {
  return change_credits(store, phone_e164, credits, reference, note, 1,
                        balance_after);
}

DataResult data_credit_charge(DataStore *store, const char *phone_e164,
                              int64_t credits, const char *reference,
                              const char *note, int64_t *balance_after) {
  return change_credits(store, phone_e164, credits, reference, note, 0,
                        balance_after);
}
