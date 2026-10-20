#include <errno.h>
#include <openssl/crypto.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>

#include "api.h"
#include "http.h"
#include "print_api.h"
#include "response.h"

#define API_BODY_MAX 1024U

typedef struct {
  char phone[DATA_PHONE_CAPACITY];
  char password[129];
  char authorization[320];
  char reference[DATA_REFERENCE_CAPACITY];
  int64_t credits;
  int active;
  int has_credits;
  int has_active;
  size_t content_length;
  size_t body_offset;
} ApiInput;

static int name_character(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static size_t header_end(const HttpRequest *request) {
  for (size_t i = 0; i + 3 < request->length; i++)
    if (memcmp(request->data + i, "\r\n\r\n", 4) == 0)
      return i + 4;
  for (size_t i = 0; i + 1 < request->length; i++)
    if (memcmp(request->data + i, "\n\n", 2) == 0)
      return i + 2;
  return 0;
}

static int parse_input_headers(const HttpRequest *request, ApiInput *input) {
  size_t end = header_end(request);
  if (end == 0)
    return -1;
  size_t position = 0;
  while (position < end && request->data[position] != '\n')
    position++;
  if (position < end)
    position++;
  int length_seen = 0;
  int authorization_seen = 0;
  while (position < end) {
    size_t line_end = position;
    while (line_end < end && request->data[line_end] != '\r' &&
           request->data[line_end] != '\n')
      line_end++;
    if (line_end == position)
      break;
    size_t colon = position;
    while (colon < line_end && request->data[colon] != ':') {
      if (!name_character((unsigned char)request->data[colon]))
        return -1;
      colon++;
    }
    if (colon == position || colon == line_end)
      return -1;
    size_t first = colon + 1;
    while (first < line_end &&
           (request->data[first] == ' ' || request->data[first] == '\t'))
      first++;
    size_t last = line_end;
    while (last > first &&
           (request->data[last - 1] == ' ' || request->data[last - 1] == '\t'))
      last--;
    size_t name_size = colon - position;
    size_t value_size = last - first;
    if (name_size == 14 &&
        strncasecmp(request->data + position, "content-length", 14) == 0) {
      if (length_seen || value_size == 0)
        return -1;
      length_seen = 1;
      for (size_t i = first; i < last; i++) {
        unsigned char c = (unsigned char)request->data[i];
        if (c < '0' || c > '9' ||
            input->content_length > (API_BODY_MAX - (size_t)(c - '0')) / 10)
          return -1;
        input->content_length = input->content_length * 10 + (size_t)(c - '0');
      }
    } else if (name_size == 17 && strncasecmp(request->data + position,
                                              "transfer-encoding", 17) == 0) {
      return -1;
    } else if (name_size == 13 && strncasecmp(request->data + position,
                                              "authorization", 13) == 0) {
      if (authorization_seen || value_size == 0 ||
          value_size >= sizeof(input->authorization))
        return -1;
      authorization_seen = 1;
      memcpy(input->authorization, request->data + first, value_size);
      input->authorization[value_size] = '\0';
    }
    while (line_end < end &&
           (request->data[line_end] == '\r' || request->data[line_end] == '\n'))
      line_end++;
    position = line_end;
  }
  input->body_offset = end;
  if ((!length_seen && request->length != end) ||
      input->content_length > API_BODY_MAX ||
      request->length - end > input->content_length)
    return -1;
  return 0;
}

static int read_body(int client, const HttpRequest *request, ApiInput *input,
                     char body[API_BODY_MAX + 1]) {
  size_t received = request->length - input->body_offset;
  if (received > input->content_length)
    return -1;
  memcpy(body, request->data + input->body_offset, received);
  while (received < input->content_length) {
    ssize_t count =
        recv(client, body + received, input->content_length - received, 0);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return -1;
    received += (size_t)count;
  }
  body[received] = '\0';
  return 0;
}

static void skip_space(const char **cursor) {
  while (**cursor == ' ' || **cursor == '\t' || **cursor == '\r' ||
         **cursor == '\n')
    (*cursor)++;
}

static int json_string(const char **cursor, char *output, size_t capacity) {
  if (**cursor != '"')
    return -1;
  (*cursor)++;
  size_t used = 0;
  while (**cursor != '\0' && **cursor != '"') {
    unsigned char c = (unsigned char)*(*cursor)++;
    if (c == '\\') {
      c = (unsigned char)*(*cursor)++;
      if (c == '"' || c == '\\' || c == '/') {
      } else if (c == 'b')
        c = '\b';
      else if (c == 'f')
        c = '\f';
      else if (c == 'n')
        c = '\n';
      else if (c == 'r')
        c = '\r';
      else if (c == 't')
        c = '\t';
      else {
        OPENSSL_cleanse(output, capacity);
        return -1;
      }
    }
    if (c < 32 || c == 0 || used + 1 >= capacity) {
      OPENSSL_cleanse(output, capacity);
      return -1;
    }
    output[used++] = (char)c;
  }
  if (**cursor != '"') {
    OPENSSL_cleanse(output, capacity);
    return -1;
  }
  (*cursor)++;
  output[used] = '\0';
  return 0;
}

static int parse_credentials(const char *body, ApiInput *input,
                             int password_required) {
  const char *cursor = body;
  int phone_seen = 0, password_seen = 0, reference_seen = 0;
  skip_space(&cursor);
  if (*cursor++ != '{')
    return -1;
  for (;;) {
    char key[32] = {0}, value[DATA_REFERENCE_CAPACITY] = {0};
    skip_space(&cursor);
    if (*cursor == '}') {
      cursor++;
      break;
    }
    if (json_string(&cursor, key, sizeof(key)) != 0)
      return -1;
    skip_space(&cursor);
    if (*cursor++ != ':')
      return -1;
    skip_space(&cursor);
    if (strcmp(key, "credits") == 0) {
      if (input->has_credits || *cursor < '0' || *cursor > '9')
        return -1;
      uint64_t parsed = 0;
      do {
        unsigned int digit = (unsigned int)(*cursor - '0');
        if (parsed > ((uint64_t)INT64_MAX - digit) / 10)
          return -1;
        parsed = parsed * 10 + digit;
        cursor++;
      } while (*cursor >= '0' && *cursor <= '9');
      input->credits = (int64_t)parsed;
      input->has_credits = 1;
    } else if (strcmp(key, "active") == 0) {
      if (input->has_active)
        return -1;
      if (strncmp(cursor, "true", 4) == 0) {
        input->active = 1;
        cursor += 4;
      } else if (strncmp(cursor, "false", 5) == 0) {
        input->active = 0;
        cursor += 5;
      } else {
        return -1;
      }
      input->has_active = 1;
    } else {
      if (json_string(&cursor, value, sizeof(value)) != 0)
        return -1;
    }
    if (strcmp(key, "phone") == 0 || strcmp(key, "phone_number") == 0) {
      if (phone_seen || strlen(value) >= sizeof(input->phone)) {
        OPENSSL_cleanse(value, sizeof(value));
        return -1;
      }
      strcpy(input->phone, value);
      phone_seen = 1;
    } else if (strcmp(key, "password") == 0 ||
               strcmp(key, "new_password") == 0) {
      if (password_seen) {
        OPENSSL_cleanse(value, sizeof(value));
        return -1;
      }
      strcpy(input->password, value);
      password_seen = 1;
    } else if (strcmp(key, "reference") == 0) {
      if (reference_seen) {
        OPENSSL_cleanse(value, sizeof(value));
        return -1;
      }
      strcpy(input->reference, value);
      reference_seen = 1;
    } else if (strcmp(key, "credits") == 0 || strcmp(key, "active") == 0) {
      /* Parsed above as an integer or boolean. */
    } else {
      OPENSSL_cleanse(value, sizeof(value));
      return -1;
    }
    OPENSSL_cleanse(value, sizeof(value));
    skip_space(&cursor);
    if (*cursor == '}') {
      cursor++;
      break;
    }
    if (*cursor++ != ',')
      return -1;
  }
  skip_space(&cursor);
  return *cursor == '\0' && phone_seen &&
                 (!password_required || password_seen)
             ? 0
             : -1;
}

static int bearer(const char *header, char token[256]) {
  if (strncasecmp(header, "Bearer ", 7) != 0 || strlen(header + 7) >= 256)
    return -1;
  strcpy(token, header + 7);
  return token[0] == '\0' ? -1 : 0;
}

static int equal_secret(const char *a, const char *b) {
  size_t al = strlen(a), bl = strlen(b), n = al > bl ? al : bl;
  unsigned int difference = (unsigned int)(al ^ bl);
  for (size_t i = 0; i < n; i++)
    difference |= (unsigned int)((i < al ? (unsigned char)a[i] : 0) ^
                                 (i < bl ? (unsigned char)b[i] : 0));
  return difference == 0;
}

static size_t json_response(int client, int status, const char *reason,
                            const char *body) {
  return http_response_send(client, status, reason,
                            "application/json; charset=utf-8", body,
                            strlen(body));
}

static int read_json_input(int client, const HttpRequest *request,
                           ApiInput *input, int password_required) {
  char body[API_BODY_MAX + 1] = {0};
  if (parse_input_headers(request, input) != 0 ||
      read_body(client, request, input, body) != 0) {
    OPENSSL_cleanse(body, sizeof(body));
    return -1;
  }
  int status = parse_credentials(body, input, password_required);
  OPENSSL_cleanse(body, sizeof(body));
  return status;
}

static size_t handle_credentials(int client, const HttpRequest *request,
                                DataStore *data_store, int signup) {
  ApiInput input = {0};
  if (read_json_input(client, request, &input, 1) != 0) {
    OPENSSL_cleanse(&input, sizeof(input));
    return json_response(client, 400, "Bad Request",
                         "{\"error\":\"Invalid phone or password payload\"}");
  }

  char token[DATA_TOKEN_CAPACITY] = {0};
  DataUser user = {0};
  DataResult result = signup
                          ? data_user_register(data_store, input.phone,
                                               input.password, token,
                                               sizeof(token), &user)
                          : data_user_login(data_store, input.phone,
                                            input.password, token,
                                            sizeof(token), &user);
  size_t sent;
  if (signup && result == DATA_DUPLICATE) {
    sent = json_response(client, 409, "Conflict",
                         "{\"error\":\"An account with this phone number already exists\"}");
  } else if (result == DATA_INVALID) {
    sent = json_response(client, 400, "Bad Request",
                         "{\"error\":\"Use an E.164 phone number and an 8-128 character password\"}");
  } else if (result == DATA_UNAUTHORIZED) {
    sent = json_response(client, 401, "Unauthorized",
                         "{\"error\":\"Phone number or password is incorrect\"}");
  } else if (result != DATA_OK) {
    sent = json_response(client, 500, "Internal Server Error",
                         "{\"error\":\"Could not create account session\"}");
  } else {
    char body[512];
    snprintf(body, sizeof(body),
             "{\"token\":\"%s\",\"phone\":\"%s\",\"credits\":%lld}",
             token, user.phone_e164, (long long)user.credits);
    sent = json_response(client, signup ? 201 : 200,
                         signup ? "Created" : "OK", body);
  }
  OPENSSL_cleanse(token, sizeof(token));
  OPENSSL_cleanse(&input, sizeof(input));
  OPENSSL_cleanse(&user, sizeof(user));
  return sent;
}

int api_handle_request(int client, const HttpRequest *request,
                       const char *method, const char *target,
                       const ServerConfig *config, DataStore *data_store,
                       size_t *bytes_received, size_t *bytes_sent) {
  if (target == NULL || strncmp(target, "/api/", 5) != 0)
    return 0;
  size_t sent = 0;
  if (bytes_received != NULL)
    *bytes_received = request != NULL ? request->length : 0;
  if (strcmp(target, "/api/print") == 0) {
    if (strcmp(method, "POST") == 0)
      sent =
          print_api_handle(client, request, config, data_store, bytes_received);
    else
      sent = json_response(client, 405, "Method Not Allowed",
                           "{\"error\":\"Use POST to submit a print job\"}");
  } else if (strcmp(target, "/api/login") == 0) {
    if (strcmp(method, "POST") != 0) {
      sent = json_response(client, 405, "Method Not Allowed",
                           "{\"error\":\"Use POST\"}");
    } else {
      sent = handle_credentials(client, request, data_store, 0);
    }
  } else if (strcmp(target, "/api/signup") == 0) {
    if (strcmp(method, "POST") != 0)
      sent = json_response(client, 405, "Method Not Allowed",
                           "{\"error\":\"Use POST\"}");
    else
      sent = handle_credentials(client, request, data_store, 1);
  } else if (strcmp(target, "/api/me") == 0) {
    ApiInput input = {0};
    char token[256] = {0};
    DataUser user = {0};
    if (strcmp(method, "GET") != 0) {
      sent = json_response(client, 405, "Method Not Allowed",
                           "{\"error\":\"Use GET\"}");
    } else if (parse_input_headers(request, &input) != 0 ||
               bearer(input.authorization, token) != 0 ||
               data_token_authenticate(data_store, token, &user) != DATA_OK) {
      sent = json_response(client, 401, "Unauthorized",
                           "{\"error\":\"Login required\"}");
    } else {
      char body[256];
      snprintf(body, sizeof(body), "{\"phone\":\"%s\",\"credits\":%lld}",
               user.phone_e164, (long long)user.credits);
      sent = json_response(client, 200, "OK", body);
    }
    OPENSSL_cleanse(input.authorization, sizeof(input.authorization));
    OPENSSL_cleanse(token, sizeof(token));
  } else if (strcmp(target, "/api/logout") == 0) {
    ApiInput input = {0};
    char token[256] = {0};
    DataUser user = {0};
    if (strcmp(method, "POST") != 0) {
      sent = json_response(client, 405, "Method Not Allowed",
                           "{\"error\":\"Use POST\"}");
    } else if (parse_input_headers(request, &input) != 0 ||
               bearer(input.authorization, token) != 0 ||
               data_token_authenticate(data_store, token, &user) != DATA_OK) {
      sent = json_response(client, 401, "Unauthorized",
                           "{\"error\":\"Login required\"}");
    } else {
      (void)data_token_revoke(data_store, user.phone_e164, token);
      sent = json_response(client, 200, "OK", "{\"ok\":true}");
    }
    OPENSSL_cleanse(input.authorization, sizeof(input.authorization));
    OPENSSL_cleanse(token, sizeof(token));
  } else if (strncmp(target, "/api/admin/", 11) == 0) {
    ApiInput input = {0};
    int valid_admin =
        parse_input_headers(request, &input) == 0 && config != NULL &&
        config->admin_api_token[0] != '\0' &&
        strncasecmp(input.authorization, "Bearer ", 7) == 0 &&
        equal_secret(input.authorization + 7, config->admin_api_token);
    if (strcmp(method, "POST") != 0) {
      sent = json_response(client, 405, "Method Not Allowed",
                           "{\"error\":\"Use POST\"}");
    } else if (!valid_admin) {
      sent = json_response(client, 401, "Unauthorized",
                           "{\"error\":\"Admin authorization required\"}");
    } else {
      ApiInput credentials = {0};
      int password_required = strcmp(target, "/api/admin/users") == 0 ||
                              strcmp(target, "/api/admin/users/password") == 0;
      DataResult result = read_json_input(client, request, &credentials,
                                          password_required) != 0
                              ? DATA_INVALID
                              : DATA_NOT_FOUND;
      char result_body[512] = "{\"ok\":true}";
      int result_status = 200;
      const char *result_reason = "OK";

      if (result != DATA_INVALID && strcmp(target, "/api/admin/users") == 0) {
        result = data_user_create_account(data_store, credentials.phone,
                                          credentials.password, &(DataUser){0});
        result_status = 201;
        result_reason = "Created";
      } else if (result != DATA_INVALID &&
                 strcmp(target, "/api/admin/users/password") == 0) {
        result = data_user_set_password(data_store, credentials.phone,
                                        credentials.password);
        snprintf(result_body, sizeof(result_body),
                 "{\"ok\":true,\"sessions_revoked\":true}");
      } else if (result != DATA_INVALID &&
                 strcmp(target, "/api/admin/users/lookup") == 0) {
        DataUser user = {0};
        result = data_user_get(data_store, credentials.phone, &user);
        if (result == DATA_OK) {
          snprintf(result_body, sizeof(result_body),
                   "{\"id\":%lld,\"phone\":\"%s\",\"credits\":%lld,"
                   "\"created_at\":%lld,\"active\":%s}",
                   (long long)user.id, user.phone_e164,
                   (long long)user.credits, (long long)user.created_at,
                   user.active ? "true" : "false");
        }
        OPENSSL_cleanse(&user, sizeof(user));
      } else if (result != DATA_INVALID &&
                 strcmp(target, "/api/admin/users/token") == 0) {
        char token[DATA_TOKEN_CAPACITY] = {0};
        DataUser user = {0};
        result = data_token_issue(data_store, credentials.phone, token,
                                  sizeof(token));
        if (result == DATA_OK) {
          result = data_user_get(data_store, credentials.phone, &user);
          if (result == DATA_OK) {
            snprintf(result_body, sizeof(result_body),
                     "{\"token\":\"%s\",\"phone\":\"%s\","
                     "\"credits\":%lld}", token, user.phone_e164,
                     (long long)user.credits);
          }
        }
        OPENSSL_cleanse(token, sizeof(token));
        OPENSSL_cleanse(&user, sizeof(user));
      } else if (result != DATA_INVALID &&
                 strcmp(target, "/api/admin/users/revoke") == 0) {
        result = data_user_get(data_store, credentials.phone, &(DataUser){0});
        if (result == DATA_OK) {
          DataResult revoke_result =
              data_token_revoke_all(data_store, credentials.phone);
          result = revoke_result == DATA_NOT_FOUND ? DATA_OK : revoke_result;
        }
        snprintf(result_body, sizeof(result_body),
                 "{\"ok\":true,\"sessions_revoked\":true}");
      } else if (result != DATA_INVALID &&
                 strcmp(target, "/api/admin/users/credits") == 0) {
        if (!credentials.has_credits || credentials.credits <= 0) {
          result = DATA_INVALID;
        } else {
          int64_t balance = 0;
          const char *reference = credentials.reference[0] != '\0'
                                      ? credentials.reference
                                      : NULL;
          result = data_credit_add(data_store, credentials.phone,
                                   credentials.credits, reference,
                                   "Admin credit adjustment", &balance);
          if (result == DATA_OK)
            snprintf(result_body, sizeof(result_body),
                     "{\"ok\":true,\"credits\":%lld}",
                     (long long)balance);
        }
      } else if (result != DATA_INVALID &&
                 strcmp(target, "/api/admin/users/active") == 0) {
        if (!credentials.has_active) {
          result = DATA_INVALID;
        } else {
          result = data_user_set_active(data_store, credentials.phone,
                                        credentials.active);
          if (result == DATA_OK && !credentials.active)
            snprintf(result_body, sizeof(result_body),
                     "{\"ok\":true,\"sessions_revoked\":true}");
        }
      }

      if (result == DATA_DUPLICATE) {
        sent = json_response(client, 409, "Conflict",
                             "{\"error\":\"Account or reference already exists\"}");
      } else if (result == DATA_INVALID) {
        sent = json_response(client, 400, "Bad Request",
                             "{\"error\":\"Invalid admin request fields\"}");
      } else if (result != DATA_OK) {
        sent = json_response(client, result == DATA_NOT_FOUND ? 404 : 400,
                             result == DATA_NOT_FOUND ? "Not Found" : "Bad Request",
                             "{\"error\":\"Admin operation failed for this user\"}");
      } else {
        sent = json_response(client, result_status, result_reason, result_body);
      }
      OPENSSL_cleanse(&credentials, sizeof(credentials));
    }
    OPENSSL_cleanse(&input, sizeof(input));
  } else {
    sent = json_response(client, 404, "Not Found",
                         "{\"error\":\"Unknown API route\"}");
  }
  if (bytes_sent != NULL)
    *bytes_sent = sent;
  return 1;
}
