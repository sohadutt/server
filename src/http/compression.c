#include "compression.h"
#include "logger.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

int compress_gzip(const unsigned char *input, size_t input_length,
                  unsigned char **dest, size_t *dest_len) {
  if (dest == NULL || dest_len == NULL)
    return -1;
  *dest = NULL;
  *dest_len = 0;
  if (input == NULL || input_length == 0 || input_length > UINT_MAX) {
    log_error("compress_gzip: Invalid input parameters");
    return -1;
  }
  z_stream stream;
  memset(&stream, 0, sizeof(stream));

  int windowBits = 15 + 16;
  int memLevel = 8;

  int init_status = deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                                 windowBits, memLevel, Z_DEFAULT_STRATEGY);
  if (init_status != Z_OK) {
    log_errorf("gzip compressor initialization failed (zlib status %d: %s)",
               init_status,
               stream.msg != NULL ? stream.msg : "no detail");
    return -1;
  }

  uLong max_dest_len = deflateBound(&stream, (uLong)input_length);
  if (max_dest_len == 0 || max_dest_len > UINT_MAX) {
    deflateEnd(&stream);
    return -1;
  }
  *dest = malloc((size_t)max_dest_len);
  if (*dest == NULL) {
    deflateEnd(&stream);
    log_error("gzip compressor could not allocate its output buffer");
    return -1;
  }

  stream.next_in = (Bytef *)input;
  stream.avail_in = (uInt)input_length;
  stream.next_out = (Bytef *)*dest;
  stream.avail_out = (uInt)max_dest_len;

  int res = deflate(&stream, Z_FINISH);
  if (res != Z_STREAM_END) {
    free(*dest);
    *dest = NULL;
    deflateEnd(&stream);
    log_errorf("gzip compression failed (zlib status %d)", res);
    return -1;
  }

  *dest_len = stream.total_out;
  deflateEnd(&stream);
  return 0;
}
