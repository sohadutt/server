#include "thread_pool.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct {
  ThreadPoolTask function;
  void *argument;
} ThreadPoolWork;

struct ThreadPool {
  pthread_t *threads;
  size_t thread_count;
  ThreadPoolWork *queue;
  size_t queue_capacity;
  size_t queue_head;
  size_t queue_count;
  int stopping;
  pthread_mutex_t mutex;
  pthread_cond_t work_available;
};

static void *thread_pool_worker(void *argument) {
  ThreadPool *pool = argument;

  for (;;) {
    pthread_mutex_lock(&pool->mutex);
    while (pool->queue_count == 0 && !pool->stopping) {
      pthread_cond_wait(&pool->work_available, &pool->mutex);
    }

    if (pool->queue_count == 0 && pool->stopping) {
      pthread_mutex_unlock(&pool->mutex);
      return NULL;
    }

    ThreadPoolWork work = pool->queue[pool->queue_head];
    pool->queue_head++;
    if (pool->queue_head == pool->queue_capacity) {
      pool->queue_head = 0;
    }
    pool->queue_count--;
    pthread_mutex_unlock(&pool->mutex);

    work.function(work.argument);
  }
}

static void release_pool(ThreadPool *pool) {
  pthread_cond_destroy(&pool->work_available);
  pthread_mutex_destroy(&pool->mutex);
  free(pool->queue);
  free(pool->threads);
  free(pool);
}

ThreadPool *thread_pool_create(size_t worker_count, size_t queue_capacity) {
  if (worker_count == 0 || queue_capacity == 0 ||
      worker_count > SIZE_MAX / sizeof(pthread_t) ||
      queue_capacity > SIZE_MAX / sizeof(ThreadPoolWork)) {
    return NULL;
  }

  ThreadPool *pool = calloc(1, sizeof(*pool));
  if (pool == NULL) {
    return NULL;
  }

  pool->threads = calloc(worker_count, sizeof(*pool->threads));
  pool->queue = calloc(queue_capacity, sizeof(*pool->queue));
  if (pool->threads == NULL || pool->queue == NULL) {
    free(pool->queue);
    free(pool->threads);
    free(pool);
    return NULL;
  }

  if (pthread_mutex_init(&pool->mutex, NULL) != 0) {
    free(pool->queue);
    free(pool->threads);
    free(pool);
    return NULL;
  }
  if (pthread_cond_init(&pool->work_available, NULL) != 0) {
    pthread_mutex_destroy(&pool->mutex);
    free(pool->queue);
    free(pool->threads);
    free(pool);
    return NULL;
  }

  pool->thread_count = worker_count;
  pool->queue_capacity = queue_capacity;
  for (size_t i = 0; i < worker_count; i++) {
    if (pthread_create(&pool->threads[i], NULL, thread_pool_worker, pool) != 0) {
      pthread_mutex_lock(&pool->mutex);
      pool->stopping = 1;
      pthread_cond_broadcast(&pool->work_available);
      pthread_mutex_unlock(&pool->mutex);
      for (size_t j = 0; j < i; j++) {
        pthread_join(pool->threads[j], NULL);
      }
      release_pool(pool);
      return NULL;
    }
  }

  return pool;
}

int thread_pool_submit(ThreadPool *pool, ThreadPoolTask task, void *argument) {
  if (pool == NULL || task == NULL) {
    return -1;
  }

  pthread_mutex_lock(&pool->mutex);
  if (pool->stopping || pool->queue_count == pool->queue_capacity) {
    pthread_mutex_unlock(&pool->mutex);
    return -1;
  }

  size_t queue_end = pool->queue_capacity - pool->queue_head;
  size_t tail = pool->queue_count >= queue_end
                    ? pool->queue_count - queue_end
                    : pool->queue_head + pool->queue_count;
  pool->queue[tail] = (ThreadPoolWork){.function = task, .argument = argument};
  pool->queue_count++;
  pthread_cond_signal(&pool->work_available);
  pthread_mutex_unlock(&pool->mutex);
  return 0;
}

void thread_pool_destroy(ThreadPool *pool) {
  if (pool == NULL) {
    return;
  }

  pthread_mutex_lock(&pool->mutex);
  pool->stopping = 1;
  pthread_cond_broadcast(&pool->work_available);
  pthread_mutex_unlock(&pool->mutex);

  for (size_t i = 0; i < pool->thread_count; i++) {
    pthread_join(pool->threads[i], NULL);
  }

  release_pool(pool);
}
