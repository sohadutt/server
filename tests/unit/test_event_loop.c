#include "event_loop.h"

#include <assert.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
  int calls;
  char received;
} CallbackState;

typedef struct {
  EventLoop *loop;
  int first_fd;
  int second_fd;
  int calls;
} MutationState;

static void read_ready(int file_descriptor, short events, void *context) {
  CallbackState *state = context;
  assert((events & POLLIN) != 0);
  state->calls++;
  assert(read(file_descriptor, &state->received, 1) == 1);
}

static void second_ready(int file_descriptor, short events, void *context) {
  MutationState *state = context;
  char byte;
  assert((events & POLLIN) != 0);
  assert(read(file_descriptor, &byte, 1) == 1 && byte == 'b');
  state->calls++;
}

static void add_watch_during_dispatch(int file_descriptor, short events,
                                     void *context) {
  MutationState *state = context;
  char byte;
  assert((events & POLLIN) != 0);
  assert(read(file_descriptor, &byte, 1) == 1);
  state->calls++;
  assert(event_loop_remove(state->loop, state->first_fd) == 0);
  assert(event_loop_add(state->loop, state->second_fd, POLLIN, second_ready,
                        state) == 0);
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

  int first[2];
  int second[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, first) == 0);
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, second) == 0);
  loop = event_loop_create();
  assert(loop != NULL);
  MutationState mutation = {.loop = loop, .first_fd = first[0],
                            .second_fd = second[0]};
  assert(event_loop_add(loop, first[0], POLLIN, add_watch_during_dispatch,
                        &mutation) == 0);
  assert(write(first[1], "a", 1) == 1);
  assert(event_loop_run_once(loop, 1000) == 1);
  assert(mutation.calls == 1 && event_loop_count(loop) == 1);
  assert(write(second[1], "b", 1) == 1);
  assert(event_loop_run_once(loop, 1000) == 1);
  assert(mutation.calls == 2 && event_loop_count(loop) == 1);
  event_loop_destroy(loop);
  close(first[0]);
  close(first[1]);
  close(second[0]);
  close(second[1]);
  puts("Event loop unit tests passed");
  return 0;
}
