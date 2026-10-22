#include "buffer.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int reserve(ByteBuffer *buffer, size_t required_payload) {
  if (required_payload > buffer->maximum_capacity ||
      required_payload == SIZE_MAX)
    return -1;
  size_t required = required_payload + 1;
  if (required <= buffer->capacity)
    return 0;

  size_t limit = buffer->maximum_capacity + 1;
  size_t capacity = buffer->capacity;
  while (capacity < required) {
    size_t next = capacity > limit / 2 ? limit : capacity * 2;
    if (next <= capacity)
      return -1;
    capacity = next;
  }

  unsigned char *data;
  if (buffer->data == buffer->inline_data) {
    data = malloc(capacity);
    if (data != NULL)
      memcpy(data, buffer->inline_data, buffer->length + 1);
  } else {
    data = realloc(buffer->data, capacity);
  }
  if (data == NULL)
    return -1;
  buffer->data = data;
  buffer->capacity = capacity;
  return 0;
}

int byte_buffer_init(ByteBuffer *buffer, size_t max_payload) {
  if (buffer == NULL || max_payload == SIZE_MAX)
    return -1;
  buffer->length = 0;
  buffer->data = buffer->inline_data;
  buffer->capacity = max_payload < BYTE_BUFFER_INLINE_CAPACITY - 1
                         ? max_payload + 1
                         : BYTE_BUFFER_INLINE_CAPACITY;
  buffer->maximum_capacity = max_payload;
  buffer->data[0] = '\0';
  return 0;
}

int byte_buffer_append(ByteBuffer *buffer, const void *data, size_t length) {
  if (buffer == NULL || buffer->data == NULL ||
      buffer->length > buffer->maximum_capacity ||
      (data == NULL && length != 0) ||
      length > buffer->maximum_capacity - buffer->length)
    return -1;

  size_t old_length = buffer->length;
  size_t source_offset = 0;
  int source_is_internal = 0;
  if (length != 0 && buffer->data != NULL) {
    uintptr_t source = (uintptr_t)data;
    uintptr_t start = (uintptr_t)buffer->data;
    if (source >= start && source - start < buffer->capacity) {
      size_t offset = (size_t)(source - start);
      if (offset > old_length || length > old_length - offset)
        return -1;
      source_is_internal = 1;
      source_offset = offset;
    }
  }

  if (reserve(buffer, old_length + length) != 0)
    return -1;
  const void *source = source_is_internal ? buffer->data + source_offset : data;
  if (length != 0)
    memmove(buffer->data + old_length, source, length);
  buffer->length = old_length + length;
  buffer->data[buffer->length] = '\0';
  return 0;
}

int byte_buffer_vappendf(ByteBuffer *buffer, const char *format, va_list args) {
  if (buffer == NULL || buffer->data == NULL || format == NULL)
    return -1;
  if (buffer->length > buffer->maximum_capacity)
    return -1;
  va_list measure;
  va_copy(measure, args);
  int needed = vsnprintf(NULL, 0, format, measure);
  va_end(measure);
  if (needed < 0 || (size_t)needed > buffer->maximum_capacity - buffer->length)
    return -1;
  if (reserve(buffer, buffer->length + (size_t)needed) != 0)
    return -1;

  va_list write_args;
  va_copy(write_args, args);
  int written = vsnprintf((char *)buffer->data + buffer->length,
                          buffer->capacity - buffer->length, format,
                          write_args);
  va_end(write_args);
  if (written != needed)
    return -1;
  buffer->length += (size_t)written;
  return 0;
}

int byte_buffer_appendf(ByteBuffer *buffer, const char *format, ...) {
  va_list args;
  va_start(args, format);
  int result = byte_buffer_vappendf(buffer, format, args);
  va_end(args);
  return result;
}

void byte_buffer_reset(ByteBuffer *buffer) {
  if (buffer == NULL || buffer->data == NULL)
    return;
  buffer->length = 0;
  buffer->data[0] = '\0';
}

void byte_buffer_destroy(ByteBuffer *buffer) {
  if (buffer == NULL)
    return;
  if (buffer->data != NULL && buffer->data != buffer->inline_data) {
    free(buffer->data);
  }
  buffer->data = NULL;
  buffer->length = 0;
  buffer->capacity = 0;
  buffer->maximum_capacity = 0;
}
