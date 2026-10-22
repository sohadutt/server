#ifndef EVENT_LOOP_H
#define EVENT_LOOP_H

#include <stddef.h>
#include <poll.h>

#define EVENT_LOOP_MAX_WATCHES 256U

typedef struct EventLoop EventLoop;
typedef void (*EventLoopCallback)(int file_descriptor, short events,
                                  void *context);

EventLoop *event_loop_create(void);
void event_loop_destroy(EventLoop *loop);
int event_loop_add(EventLoop *loop, int file_descriptor, short events,
                   EventLoopCallback callback, void *context);
int event_loop_remove(EventLoop *loop, int file_descriptor);
/* Waits once; returns number of callbacks, 0 on timeout, or -1 on error. */
/* Watch registration must not be changed while a callback is being dispatched. */
int event_loop_run_once(EventLoop *loop, int timeout_milliseconds);
size_t event_loop_count(const EventLoop *loop);

#endif
