#define _XOPEN_SOURCE 700

#include "static.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "logger.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define STATIC_FILE_MEMORY_BUDGET (256ULL * 1024ULL * 1024ULL)
#define STATIC_FILE_MAX_SIZE STATIC_FILE_MEMORY_BUDGET
#define BYTES_PER_MEGABYTE (1024ULL * 1024ULL)

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#error "Static file serving requires O_NOFOLLOW support"
#endif

static pthread_mutex_t file_memory_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t file_memory_available = PTHREAD_COND_INITIALIZER;
static size_t file_memory_in_use;

static int reserve_file_memory(size_t amount) {
  pthread_mutex_lock(&file_memory_mutex);
  if ((unsigned long long)amount > STATIC_FILE_MEMORY_BUDGET) {
    pthread_mutex_unlock(&file_memory_mutex);
    errno = EFBIG;
    return -1;
  }
  while (amount > (size_t)STATIC_FILE_MEMORY_BUDGET - file_memory_in_use) {
    if (pthread_cond_wait(&file_memory_available, &file_memory_mutex) != 0) {
      pthread_mutex_unlock(&file_memory_mutex);
      errno = EBUSY;
      return -1;
    }
  }
  file_memory_in_use += amount;
  pthread_mutex_unlock(&file_memory_mutex);
  return 0;
}

static void release_file_memory(size_t amount) {
  if (amount == 0) {
    return;
  }
  pthread_mutex_lock(&file_memory_mutex);
  file_memory_in_use -= amount;
  pthread_cond_broadcast(&file_memory_available);
  pthread_mutex_unlock(&file_memory_mutex);
}

static int copy_string(char *destination, size_t capacity, const char *source) {
  size_t length = strlen(source);
  if (length == 0 || length >= capacity)
    return -1;
  memcpy(destination, source, length + 1);
  return 0;
}

static int is_safe_relative_path(const char *path) {
  if (path == NULL || path[0] == '\0' || path[0] == '/' ||
      strchr(path, '\\') != NULL) {
    return 0;
  }

  const char *segment = path;
  for (const char *cursor = path;; cursor++) {
    unsigned char character = (unsigned char)*cursor;
    if (character != '\0' && character != '/' &&
        (character < 0x20 || character == 0x7f)) {
      return 0;
    }
    if (character == '/' || character == '\0') {
      size_t length = (size_t)(cursor - segment);
      if (length == 0 || (length == 1 && segment[0] == '.') ||
          (length == 2 && segment[0] == '.' && segment[1] == '.')) {
        return 0;
      }
      if (character == '\0')
        break;
      segment = cursor + 1;
    }
  }
  return 1;
}

static int hex_value(char character) {
  if (character >= '0' && character <= '9')
    return character - '0';
  if (character >= 'a' && character <= 'f')
    return character - 'a' + 10;
  if (character >= 'A' && character <= 'F')
    return character - 'A' + 10;
  return -1;
}

static int decode_request_path(const char *target, char *relative_path,
                               size_t capacity) {
  if (target == NULL || target[0] != '/' || capacity == 0)
    return -1;

  size_t output = 0;
  for (size_t input = 1;
       target[input] != '\0' && target[input] != '?' && target[input] != '#';
       input++) {
    unsigned char character = (unsigned char)target[input];
    if (character == '%') {
      int high = hex_value(target[input + 1]);
      int low = target[input + 1] == '\0' ? -1 : hex_value(target[input + 2]);
      if (high < 0 || low < 0)
        return -1;
      character = (unsigned char)((high << 4) | low);
      input += 2;
    }
    if (character == '\0' || character == '\\' || character < 0x20 ||
        character == 0x7f || output + 1 >= capacity) {
      return -1;
    }
    relative_path[output++] = (char)character;
  }
  relative_path[output] = '\0';

  char *first_component = relative_path;
  while (*first_component == '/')
    first_component++;
  if (first_component != relative_path)
    memmove(relative_path, first_component,
            strlen(first_component) + 1);
  if (*relative_path == '\0') {
    return 0;
  }
  char *segment = relative_path;
  while (*segment != '\0') {
    while (*segment == '/')
      segment++;
    if (*segment == '\0')
      break;
    char *end = strchr(segment, '/');
    if (end == NULL)
      end = segment + strlen(segment);
    size_t length = (size_t)(end - segment);
    if ((length == 1 && segment[0] == '.') ||
        (length == 2 && segment[0] == '.' && segment[1] == '.')) {
      return -1;
    }
    segment = end;
  }
  return 0;
}

static const char *content_type_for_path(const StaticFileServer *server,
                                         const char *path) {
  const char *extension = strrchr(path, '.');
  if (extension == NULL || extension[1] == '\0') {
    return server->default_mime_type;
  }
  return mime_types_lookup(&server->mime_types, extension + 1,
                           server->default_mime_type);
}

static int open_file_beneath(const StaticFileServer *server,
                             const char *relative_path,
                             int allow_directory_index,
                             char *opened_relative, size_t path_capacity) {
  char path[PATH_MAX];
  if (relative_path == NULL || *relative_path == '\0' ||
      strlen(relative_path) >= sizeof(path) ||
      copy_string(path, sizeof(path), relative_path) != 0) {
    errno = ENAMETOOLONG;
    return -1;
  }

  int directory_fd = dup(server->document_root_fd);
  if (directory_fd < 0) {
    return -1;
  }

  char *save = NULL;
  char *component = strtok_r(path, "/", &save);
  while (component != NULL) {
    char *next = strtok_r(NULL, "/", &save);
    if (next != NULL) {
      int child_fd = openat(directory_fd, component,
                            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      int saved_errno = errno;
      close(directory_fd);
      if (child_fd < 0) {
        errno = saved_errno;
        return -1;
      }
      directory_fd = child_fd;
      component = next;
      continue;
    }

    int file_fd = openat(directory_fd, component,
                         O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    int saved_errno = errno;
    close(directory_fd);
    if (file_fd < 0) {
      errno = saved_errno;
      return -1;
    }

    struct stat status;
    if (fstat(file_fd, &status) != 0) {
      saved_errno = errno;
      close(file_fd);
      errno = saved_errno;
      return -1;
    }

    if (S_ISDIR(status.st_mode) && allow_directory_index) {
      close(file_fd);
      char index_path[PATH_MAX];
      int written = snprintf(index_path, sizeof(index_path), "%s/%s",
                             relative_path, server->index_file);
      if (written < 0 || (size_t)written >= sizeof(index_path) ||
          copy_string(opened_relative, path_capacity, index_path) != 0) {
        errno = ENAMETOOLONG;
        return -1;
      }
      return open_file_beneath(server, index_path, 0, opened_relative,
                               path_capacity);
    }

    if (!S_ISREG(status.st_mode)) {
      close(file_fd);
      errno = EACCES;
      return -1;
    }
    if (copy_string(opened_relative, path_capacity, relative_path) != 0) {
      close(file_fd);
      errno = ENAMETOOLONG;
      return -1;
    }
    return file_fd;
  }

  close(directory_fd);
  errno = EINVAL;
  return -1;
}

static int load_relative_file(const StaticFileServer *server,
                              const char *relative_path, StaticFile *file) {
  file->fd = -1;
  file->is_stream = 0;
  file->data = NULL;
  file->length = 0;
  file->memory_charge = 0;
  file->content_type = NULL;
  file->relative_path[0] = '\0';

  char opened_relative[PATH_MAX];
  int input = open_file_beneath(server, relative_path, 1, opened_relative,
                                sizeof(opened_relative));
  if (input < 0)
    return -1;

  struct stat status;
  if (fstat(input, &status) != 0) {
    close(input);
    return -1;
  }
  if (!S_ISREG(status.st_mode)) {
    close(input);
    errno = EACCES;
    return -1;
  }
  if (status.st_size < 0 ||
      (unsigned long long)status.st_size > server->max_file_size_bytes) {
    close(input);
    errno = EFBIG;
    return -1;
  }

  size_t length = (size_t)status.st_size;
  if (reserve_file_memory(length) != 0) {
    close(input);
    return -1;
  }
  unsigned char *data = malloc(length == 0 ? 1 : length);
  if (data == NULL) {
    release_file_memory(length);
    close(input);
    return -1;
  }

  size_t bytes_read = 0;
  while (bytes_read < length) {
    ssize_t result = read(input, data + bytes_read, length - bytes_read);
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      free(data);
      release_file_memory(length);
      close(input);
      return -1;
    }
    bytes_read += (size_t)result;
  }
  close(input);

  file->data = data;
  file->length = length;
  file->memory_charge = length;
  file->content_type = content_type_for_path(server, opened_relative);
  if (copy_string(file->relative_path, sizeof(file->relative_path),
                  opened_relative) != 0) {
    free(data);
    release_file_memory(length);
    file->data = NULL;
    file->length = 0;
    file->memory_charge = 0;
    file->content_type = NULL;
    return -1;
  }
  return 0;
}

static int open_relative_file(const StaticFileServer *server,
                              const char *relative_path, StaticFile *file) {
  file->fd = -1;
  file->is_stream = 0;
  file->data = NULL;
  file->length = 0;
  file->memory_charge = 0;
  file->content_type = NULL;
  file->relative_path[0] = '\0';

  char opened_relative[PATH_MAX];
  int fd = open_file_beneath(server, relative_path, 1, opened_relative,
                             sizeof(opened_relative));
  if (fd < 0)
    return -1;

  struct stat status;
  if (fstat(fd, &status) != 0) {
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return -1;
  }
  if (!S_ISREG(status.st_mode) || status.st_size < 0 ||
      (unsigned long long)status.st_size > server->max_file_size_bytes) {
    int saved_errno = !S_ISREG(status.st_mode) ? EACCES : EFBIG;
    close(fd);
    errno = saved_errno;
    return -1;
  }
  if (copy_string(file->relative_path, sizeof(file->relative_path),
                  opened_relative) != 0) {
    close(fd);
    errno = ENAMETOOLONG;
    return -1;
  }
  file->fd = fd;
  file->is_stream = 1;
  file->length = (size_t)status.st_size;
  file->content_type = content_type_for_path(server, opened_relative);
  return 0;
}

int static_file_server_init(StaticFileServer *server,
                            const ServerConfig *config) {
  if (server == NULL || config == NULL || config->max_static_file_size_mb < 1 ||
      config->max_static_file_size_mb > 256) {
    return -1;
  }

  memset(server, 0, sizeof(*server));
  server->document_root_fd = -1;
  char resolved_root[PATH_MAX];
  if (realpath(config->document_root, resolved_root) == NULL ||
      strlen(resolved_root) >= sizeof(server->document_root) ||
      copy_string(server->document_root, sizeof(server->document_root),
                  resolved_root) != 0) {
    char message[384];
    snprintf(message, sizeof(message), "Invalid document root '%s'",
             config->document_root);
    log_error(message);
    return -1;
  }

  server->document_root_fd =
      open(server->document_root,
           O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  struct stat root_status;
  if (server->document_root_fd < 0 ||
      fstat(server->document_root_fd, &root_status) != 0 ||
      !S_ISDIR(root_status.st_mode)) {
    log_error("Could not securely open the document root directory");
    static_file_server_destroy(server);
    return -1;
  }

  if (!is_safe_relative_path(config->index_file) ||
      !is_safe_relative_path(config->not_found_page) ||
      !mime_content_type_valid(config->default_mime_type) ||
      copy_string(server->index_file, sizeof(server->index_file),
                  config->index_file) != 0 ||
      copy_string(server->not_found_page, sizeof(server->not_found_page),
                  config->not_found_page) != 0 ||
      copy_string(server->default_mime_type, sizeof(server->default_mime_type),
                  config->default_mime_type) != 0) {
    log_error("Invalid static file configuration");
    static_file_server_destroy(server);
    return -1;
  }

  server->max_file_size_bytes =
      (size_t)config->max_static_file_size_mb * BYTES_PER_MEGABYTE;
  if (server->max_file_size_bytes > STATIC_FILE_MAX_SIZE) {
    log_error("Configured static file limit exceeds the server maximum");
    static_file_server_destroy(server);
    return -1;
  }

  if (mime_types_load(&server->mime_types, config->mime_types_file) != 0) {
    static_file_server_destroy(server);
    return -1;
  }

  if (config->static_mount_count > SERVER_STATIC_MOUNTS_MAX) {
    log_error("Too many static application mounts");
    static_file_server_destroy(server);
    return -1;
  }
  for (size_t index = 0; index < config->static_mount_count; index++) {
    const StaticMountConfig *mount = &config->static_mounts[index];
    if (mount->url_prefix[0] != '/' || mount->url_prefix[1] == '\0' ||
        mount->url_prefix[strlen(mount->url_prefix) - 1] == '/' ||
        !is_safe_relative_path(mount->url_prefix + 1) ||
        strchr(mount->url_prefix, '?') != NULL ||
        strchr(mount->url_prefix, '#') != NULL ||
        !is_safe_relative_path(mount->directory) ||
        copy_string(server->mounts[index].url_prefix,
                    sizeof(server->mounts[index].url_prefix),
                    mount->url_prefix) != 0 ||
        copy_string(server->mounts[index].directory,
                    sizeof(server->mounts[index].directory),
                    mount->directory) != 0) {
      log_error("Invalid static application mount configuration");
      static_file_server_destroy(server);
      return -1;
    }
    server->mounts[index].spa_fallback = mount->spa_fallback;
    if (mount->spa_fallback) {
      char app_index[PATH_MAX];
      int written = snprintf(app_index, sizeof(app_index), "%s/%s",
                             mount->directory, server->index_file);
      StaticFile check = {0};
      if (written < 0 || (size_t)written >= sizeof(app_index) ||
          open_relative_file(server, app_index, &check) != 0) {
        static_file_release(&check);
        log_error("SPA mount index cannot be loaded from its directory");
        static_file_server_destroy(server);
        return -1;
      }
      static_file_release(&check);
    }
    server->mount_count++;
  }

  StaticFile check = {0};
  if (open_relative_file(server, server->index_file, &check) != 0) {
    log_error("Configured index file cannot be loaded from the document root");
    static_file_server_destroy(server);
    return -1;
  }
  static_file_release(&check);

  if (open_relative_file(server, server->not_found_page, &check) != 0) {
    log_error("Configured 404 page cannot be loaded from the document root");
    static_file_server_destroy(server);
    return -1;
  }
  static_file_release(&check);
  return 0;
}

void static_file_server_destroy(StaticFileServer *server) {
  if (server != NULL) {
    if (server->document_root_fd >= 0) {
      close(server->document_root_fd);
    }
    memset(server, 0, sizeof(*server));
    server->document_root_fd = -1;
  }
}

int static_file_load(const StaticFileServer *server, const char *request_target,
                     StaticFile *file) {
  if (server == NULL || request_target == NULL || file == NULL)
    return -1;
  file->data = NULL;
  file->fd = -1;
  file->is_stream = 0;
  file->length = 0;
  file->memory_charge = 0;
  file->content_type = NULL;
  file->relative_path[0] = '\0';

  char relative_path[PATH_MAX];
  if (decode_request_path(request_target, relative_path,
                          sizeof(relative_path)) != 0) {
    return -1;
  }

  const StaticMountConfig *matched_mount = NULL;
  size_t matched_prefix_length = 0;
  for (size_t index = 0; index < server->mount_count; index++) {
    const StaticMountConfig *mount = &server->mounts[index];
    size_t prefix_length = strlen(mount->url_prefix);
    if (strncmp(relative_path, mount->url_prefix + 1, prefix_length - 1) != 0 ||
        (relative_path[prefix_length - 1] != '\0' &&
         relative_path[prefix_length - 1] != '/'))
      continue;
    if (prefix_length > matched_prefix_length) {
      matched_mount = mount;
      matched_prefix_length = prefix_length;
    }
  }
  if (matched_mount != NULL) {
    const char *tail = relative_path + matched_prefix_length;
    while (*tail == '/')
      tail++;
    char mounted_path[PATH_MAX];
    int written = *tail == '\0'
                      ? snprintf(mounted_path, sizeof(mounted_path), "%s",
                                 matched_mount->directory)
                      : snprintf(mounted_path, sizeof(mounted_path), "%s/%s",
                                 matched_mount->directory, tail);
    if (written < 0 || (size_t)written >= sizeof(mounted_path)) {
      errno = ENAMETOOLONG;
      return -1;
    }
    if (load_relative_file(server, mounted_path, file) == 0)
      return 0;
    if (!matched_mount->spa_fallback || (errno != ENOENT && errno != ENOTDIR))
      return -1;
    written = snprintf(mounted_path, sizeof(mounted_path), "%s/%s",
                       matched_mount->directory, server->index_file);
    if (written < 0 || (size_t)written >= sizeof(mounted_path)) {
      errno = ENAMETOOLONG;
      return -1;
    }
    return load_relative_file(server, mounted_path, file);
  }
  const char *path_to_load =
      relative_path[0] == '\0' ? server->index_file : relative_path;
  return load_relative_file(server, path_to_load, file);
}

int static_file_load_not_found(const StaticFileServer *server,
                               StaticFile *file) {
  if (server == NULL || file == NULL)
    return -1;
  file->data = NULL;
  file->fd = -1;
  file->is_stream = 0;
  file->length = 0;
  file->memory_charge = 0;
  file->content_type = NULL;
  file->relative_path[0] = '\0';
  return load_relative_file(server, server->not_found_page, file);
}

static int open_request_file(const StaticFileServer *server,
                             const char *request_target, StaticFile *file) {
  char relative_path[PATH_MAX];
  if (decode_request_path(request_target, relative_path,
                          sizeof(relative_path)) != 0)
    return -1;

  const StaticMountConfig *matched_mount = NULL;
  size_t matched_prefix_length = 0;
  for (size_t index = 0; index < server->mount_count; index++) {
    const StaticMountConfig *mount = &server->mounts[index];
    size_t prefix_length = strlen(mount->url_prefix);
    if (strncmp(relative_path, mount->url_prefix + 1, prefix_length - 1) != 0 ||
        (relative_path[prefix_length - 1] != '\0' &&
         relative_path[prefix_length - 1] != '/'))
      continue;
    if (prefix_length > matched_prefix_length) {
      matched_mount = mount;
      matched_prefix_length = prefix_length;
    }
  }

  if (matched_mount == NULL) {
    const char *path = relative_path[0] == '\0' ? server->index_file
                                                 : relative_path;
    return open_relative_file(server, path, file);
  }

  const char *tail = relative_path + matched_prefix_length;
  while (*tail == '/')
    tail++;
  char mounted_path[PATH_MAX];
  int written = *tail == '\0'
                    ? snprintf(mounted_path, sizeof(mounted_path), "%s",
                               matched_mount->directory)
                    : snprintf(mounted_path, sizeof(mounted_path), "%s/%s",
                               matched_mount->directory, tail);
  if (written < 0 || (size_t)written >= sizeof(mounted_path)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  if (open_relative_file(server, mounted_path, file) == 0)
    return 0;
  if (!matched_mount->spa_fallback || (errno != ENOENT && errno != ENOTDIR))
    return -1;

  written = snprintf(mounted_path, sizeof(mounted_path), "%s/%s",
                     matched_mount->directory, server->index_file);
  if (written < 0 || (size_t)written >= sizeof(mounted_path)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  return open_relative_file(server, mounted_path, file);
}

int static_file_open(const StaticFileServer *server, const char *request_target,
                     StaticFile *file) {
  if (server == NULL || request_target == NULL || file == NULL)
    return -1;
  return open_request_file(server, request_target, file);
}

int static_file_open_not_found(const StaticFileServer *server,
                               StaticFile *file) {
  if (server == NULL || file == NULL)
    return -1;
  return open_relative_file(server, server->not_found_page, file);
}

void static_file_release(StaticFile *file) {
  if (file == NULL)
    return;
  if (file->is_stream && file->fd >= 0)
    close(file->fd);
  free(file->data);
  release_file_memory(file->memory_charge);
  file->data = NULL;
  file->fd = -1;
  file->is_stream = 0;
  file->length = 0;
  file->memory_charge = 0;
  file->content_type = NULL;
  file->relative_path[0] = '\0';
}
