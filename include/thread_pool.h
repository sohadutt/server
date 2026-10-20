#ifndef SERVER_THREAD_POOL_H
#define SERVER_THREAD_POOL_H

#include <stddef.h>

typedef struct ThreadPool ThreadPool;
typedef void (*ThreadPoolTask)(void *argument);

/* Creates a bounded pool; submit fails immediately when its queue is full. */
ThreadPool *thread_pool_create(size_t worker_count, size_t queue_capacity);
/* On success the task will run once; on failure the caller retains ownership. */
int thread_pool_submit(ThreadPool *pool, ThreadPoolTask task, void *argument);
/* Stops accepting work, drains queued tasks, joins workers, and frees the pool. */
void thread_pool_destroy(ThreadPool *pool);

#endif
