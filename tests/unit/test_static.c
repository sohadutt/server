#include "config.h"
#include "static.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void write_file(const char *path, const char *contents) {
  FILE *file = fopen(path, "w");
  assert(file != NULL);
  assert(fputs(contents, file) >= 0);
  assert(fclose(file) == 0);
}

static void join_path(char *destination, size_t capacity, const char *root,
                      const char *name) {
  int written = snprintf(destination, capacity, "%s/%s", root, name);
  assert(written > 0 && (size_t)written < capacity);
}

int main(void) {
  char root[] = "/tmp/server-static-test-XXXXXX";
  assert(mkdtemp(root) != NULL);
  char public_dir[512], nested_dir[512], index_path[512], not_found_path[512];
  char nested_index_path[512], css_path[512], mime_path[512], outside_path[512];
  join_path(public_dir, sizeof(public_dir), root, "public");
  assert(mkdir(public_dir, 0700) == 0);
  join_path(nested_dir, sizeof(nested_dir), public_dir, "nested");
  assert(mkdir(nested_dir, 0700) == 0);
  join_path(index_path, sizeof(index_path), public_dir, "index.html");
  join_path(not_found_path, sizeof(not_found_path), public_dir, "404.html");
  join_path(nested_index_path, sizeof(nested_index_path), nested_dir,
            "index.html");
  join_path(css_path, sizeof(css_path), public_dir, "app.css");
  join_path(mime_path, sizeof(mime_path), root, "mime.types");
  join_path(outside_path, sizeof(outside_path), root, "outside.txt");
  write_file(index_path, "home");
  write_file(not_found_path, "missing");
  write_file(nested_index_path, "nested");
  write_file(css_path, "body{}");
  write_file(mime_path, "html text/html\ncss text/css\ntxt text/plain\n");
  write_file(outside_path, "private");
  char link_path[512];
  join_path(link_path, sizeof(link_path), public_dir, "outside.txt");
  assert(symlink(outside_path, link_path) == 0);

  ServerConfig config;
  config_defaults(&config);
  assert(strlen(public_dir) < sizeof(config.document_root));
  assert(strlen(mime_path) < sizeof(config.mime_types_file));
  strcpy(config.document_root, public_dir);
  strcpy(config.mime_types_file, mime_path);
  StaticFileServer server = {0};
  assert(static_file_server_init(&server, &config) == 0);

  StaticFile file = {0};
  assert(static_file_load(&server, "/", &file) == 0);
  assert(file.length == 4 && memcmp(file.data, "home", 4) == 0);
  assert(strcmp(file.content_type, "text/html") == 0);
  static_file_release(&file);
  assert(file.data == NULL && file.length == 0 && file.memory_charge == 0);

  assert(static_file_load(&server, "/nested/", &file) == 0);
  assert(file.length == 6 && memcmp(file.data, "nested", 6) == 0);
  static_file_release(&file);
  assert(static_file_load(&server, "/app.css?v=1", &file) == 0);
  assert(strcmp(file.content_type, "text/css") == 0);
  static_file_release(&file);
  assert(static_file_load(&server, "/missing.txt", &file) == -1);
  assert(static_file_load(&server, "/../outside.txt", &file) == -1);
  assert(static_file_load(&server, "/%2e%2e/outside.txt", &file) == -1);
  assert(static_file_load(&server, "/outside.txt", &file) == -1);
  assert(static_file_load_not_found(&server, &file) == 0);
  assert(file.length == 7 && memcmp(file.data, "missing", 7) == 0);
  static_file_release(&file);

  static_file_server_destroy(&server);
  unlink(link_path);
  unlink(outside_path);
  unlink(css_path);
  unlink(nested_index_path);
  unlink(index_path);
  unlink(not_found_path);
  unlink(mime_path);
  rmdir(nested_dir);
  rmdir(public_dir);
  rmdir(root);
  puts("Static file unit tests passed");
  return 0;
}
