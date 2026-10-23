#ifndef COMPRESSION_H
#define COMPRESSION_H
#include <stddef.h>

int compress_gzip(const unsigned char *input, size_t input_length,
                  unsigned char **dest, size_t *dest_len);

#endif
