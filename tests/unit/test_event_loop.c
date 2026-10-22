#include "event_loop.h"

#include <assert.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
  int calls;
  char received;
} CallbackState;

static void read_ready(int file_descriptor, short events, void *context) {
  CallbackState *state = context;
  assert((events & POLLIN) != 0);
  state->calls++;
  assert(read(file_descriptor, &state->received, 1) == 1);
}

int main(void) {
  int sockets[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  EventLoop *loop = event_loop_create();
  assert(loop != NULL);
  CallbackState state = {0};
  assert(event_loop_add(loop, sockets[0], POLLIN, read_ready, &state) == 0);
  assert(event_loop_add(loop, sockets[0], POLLIN, read_ready, &state) == -1);
  assert(event_loop_count(loop) == 1);
  assert(event_loop_run_once(loop, 0) == 0);
  assert(write(sockets[1], "x", 1) == 1);
  assert(event_loop_run_once(loop, 1000) == 1);
  assert(state.calls == 1 && state.received == 'x');
  assert(event_loop_remove(loop, sockets[0]) == 0);
  assert(event_loop_remove(loop, sockets[0]) == -1);
  assert(event_loop_count(loop) == 0);
  event_loop_destroy(loop);
  close(sockets[0]);
  close(sockets[1]);
  puts("Event loop unit tests passed");
  return 0;
}
