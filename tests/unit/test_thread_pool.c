#include "thread_pool.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>

typedef struct PoolState {
  pthread_mutex_t mutex;
  pthread_cond_t condition;
  int first_task_started;
  int release_first_task;
  int completed_tasks;
} PoolState;

static void blocking_task(void *argument) {
  PoolState *state = argument;
  pthread_mutex_lock(&state->mutex);
  state->first_task_started = 1;
  pthread_cond_broadcast(&state->condition);
  while (!state->release_first_task) {
    pthread_cond_wait(&state->condition, &state->mutex);
  }
  state->completed_tasks++;
  pthread_mutex_unlock(&state->mutex);
}

static void queued_task(void *argument) {
  PoolState *state = argument;
  pthread_mutex_lock(&state->mutex);
  state->completed_tasks++;
  pthread_mutex_unlock(&state->mutex);
}

int main(void) {
  PoolState state = {0};
  assert(pthread_mutex_init(&state.mutex, NULL) == 0);
  assert(pthread_cond_init(&state.condition, NULL) == 0);

  assert(thread_pool_create(0, 1) == NULL);
  assert(thread_pool_create(1, 0) == NULL);
  ThreadPool *pool = thread_pool_create(1, 1);
  assert(pool != NULL);
  assert(thread_pool_submit(NULL, queued_task, &state) == -1);
  assert(thread_pool_submit(pool, NULL, &state) == -1);
  assert(thread_pool_submit(pool, blocking_task, &state) == 0);

  pthread_mutex_lock(&state.mutex);
  while (!state.first_task_started) {
    pthread_cond_wait(&state.condition, &state.mutex);
  }
  pthread_mutex_unlock(&state.mutex);

  assert(thread_pool_submit(pool, queued_task, &state) == 0);
  assert(thread_pool_submit(pool, queued_task, &state) == -1);

  pthread_mutex_lock(&state.mutex);
  state.release_first_task = 1;
  pthread_cond_broadcast(&state.condition);
  pthread_mutex_unlock(&state.mutex);

  /* Destroy drains the queued task before joining its worker. */
  thread_pool_destroy(pool);
  assert(state.completed_tasks == 2);
  thread_pool_destroy(NULL);

  pthread_cond_destroy(&state.condition);
  pthread_mutex_destroy(&state.mutex);
  puts("Thread pool unit tests passed");
  return 0;
}
