#include "event_loop.h"

#include <errno.h>
#include <stdlib.h>

typedef struct EventWatch {
  int file_descriptor;
  short events;
  EventLoopCallback callback;
  void *context;
} EventWatch;

struct EventLoop {
  size_t count;
  int dispatching;
  EventWatch watches[EVENT_LOOP_MAX_WATCHES];
};

EventLoop *event_loop_create(void) { return calloc(1, sizeof(EventLoop)); }

void event_loop_destroy(EventLoop *loop) { free(loop); }

int event_loop_add(EventLoop *loop, int file_descriptor, short events,
                   EventLoopCallback callback, void *context) {
  if (loop == NULL || file_descriptor < 0 || events == 0 || callback == NULL ||
      loop->count >= EVENT_LOOP_MAX_WATCHES) {
    errno = EINVAL;
    return -1;
  }
  if (loop->dispatching) {
    errno = EBUSY;
    return -1;
  }
  for (size_t index = 0; index < loop->count; index++) {
    if (loop->watches[index].file_descriptor == file_descriptor) {
      errno = EEXIST;
      return -1;
    }
  }
  loop->watches[loop->count++] = (EventWatch){
      .file_descriptor = file_descriptor,
      .events = events,
      .callback = callback,
      .context = context,
  };
  return 0;
}

int event_loop_remove(EventLoop *loop, int file_descriptor) {
  if (loop == NULL || file_descriptor < 0) {
    errno = EINVAL;
    return -1;
  }
  if (loop->dispatching) {
    errno = EBUSY;
    return -1;
  }
  for (size_t index = 0; index < loop->count; index++) {
    if (loop->watches[index].file_descriptor == file_descriptor) {
      loop->count--;
      loop->watches[index] = loop->watches[loop->count];
      loop->watches[loop->count] = (EventWatch){0};
      return 0;
    }
  }
  errno = ENOENT;
  return -1;
}

int event_loop_run_once(EventLoop *loop, int timeout_milliseconds) {
  if (loop == NULL || timeout_milliseconds < -1) {
    errno = EINVAL;
    return -1;
  }
  struct pollfd descriptors[EVENT_LOOP_MAX_WATCHES];
  for (size_t index = 0; index < loop->count; index++) {
    descriptors[index] = (struct pollfd){
        .fd = loop->watches[index].file_descriptor,
        .events = loop->watches[index].events,
        .revents = 0,
    };
  }

  int ready;
  nfds_t descriptor_count = (nfds_t)loop->count;
  do {
    ready = poll(descriptors, descriptor_count, timeout_milliseconds);
  } while (ready < 0 && errno == EINTR);
  if (ready <= 0)
    return ready;

  int callbacks = 0;
  loop->dispatching = 1;
  for (size_t index = 0; index < loop->count; index++) {
    if (descriptors[index].revents != 0) {
      EventWatch watch = loop->watches[index];
      watch.callback(watch.file_descriptor, descriptors[index].revents,
                     watch.context);
      callbacks++;
    }
  }
  loop->dispatching = 0;
  return callbacks;
}

size_t event_loop_count(const EventLoop *loop) {
  return loop != NULL ? loop->count : 0;
}
