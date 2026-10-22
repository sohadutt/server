#ifndef BYTE_BUFFER_H
#define BYTE_BUFFER_H

#include <stdarg.h>
#include <stddef.h>

#define BYTE_BUFFER_INLINE_CAPACITY 512U

#if defined(__GNUC__) || defined(__clang__)
#define BYTE_BUFFER_PRINTF_FORMAT(format_index, argument_index) \
  __attribute__((format(printf, format_index, argument_index)))
#else
#define BYTE_BUFFER_PRINTF_FORMAT(format_index, argument_index)
#endif

typedef struct ByteBuffer {
  unsigned char *data;
  size_t length;
  size_t capacity;
  size_t maximum_capacity;
  unsigned char inline_data[BYTE_BUFFER_INLINE_CAPACITY];
} ByteBuffer;

/* Buffers own their heap storage and must not be copied by value. */
/* max_payload is the maximum number of bytes excluding the trailing NUL. */
int byte_buffer_init(ByteBuffer *buffer, size_t max_payload);
int byte_buffer_append(ByteBuffer *buffer, const void *data, size_t length);
int byte_buffer_appendf(ByteBuffer *buffer, const char *format, ...)
    BYTE_BUFFER_PRINTF_FORMAT(2, 3);
int byte_buffer_vappendf(ByteBuffer *buffer, const char *format, va_list args)
    BYTE_BUFFER_PRINTF_FORMAT(2, 0);
void byte_buffer_reset(ByteBuffer *buffer);
void byte_buffer_destroy(ByteBuffer *buffer);

#endif
