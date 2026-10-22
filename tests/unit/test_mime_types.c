#include "logger.h"
#include "mime_types.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void log_error(const char *message) { (void)message; }

static void write_text_file(const char *path, const char *text) {
  FILE *file = fopen(path, "w");
  assert(file != NULL);
  assert(fputs(text, file) >= 0);
  assert(fclose(file) == 0);
}

int main(void) {
  char path[] = "/tmp/server-mime-test-XXXXXX";
  int descriptor = mkstemp(path);
  assert(descriptor >= 0);
  close(descriptor);

  MimeTypes types = {0};
  write_text_file(path, "# test mappings\nhtml text/html\nCSS text/css\n");
  assert(mime_types_load(&types, path) == 0);
  assert(types.count == 2);
  assert(strcmp(mime_types_lookup(&types, "HTML", "fallback"), "text/html") == 0);
  assert(strcmp(mime_types_lookup(&types, "css", "fallback"), "text/css") == 0);
  assert(strcmp(mime_types_lookup(&types, "unknown", "fallback"), "fallback") == 0);
  assert(mime_types_lookup(NULL, "html", "fallback") != NULL);

  write_text_file(path, "invalid-extension! text/plain\n");
  assert(mime_types_load(&types, path) == -1);
  assert(types.count == 0);
  assert(mime_content_type_valid("application/json") == 1);
  assert(mime_content_type_valid("text/plain; charset=utf-8") == 0);
  assert(mime_content_type_valid("/plain") == 0);
  assert(mime_content_type_valid("text/") == 0);
  assert(mime_content_type_valid(NULL) == 0);

  unlink(path);
  puts("MIME type unit tests passed");
  return 0;
}
