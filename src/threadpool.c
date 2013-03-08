/*
 * Copyright (c) 2011, Mathias Brossard <mathias@brossard.org>.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file threadpool.c
 * @brief Threadpool implementation file
 */

#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "threadpool.h"

/**
 *  @struct threadpool_task
 *  @brief the work struct
 *
 *  @var function Pointer to the function that will perform the task.
 *  @var argument Argument to be passed to the function.
 */

typedef struct {
    //void (*function)(void *);
	void *argument;
} threadpool_task_t;

typedef struct wq_item
{
	void *data;
	struct wq_item *next;
}wq_t;

typedef struct
{
	wq_t *head;
	wq_t *tail;
}list_t;

/**
 *  @struct threadpool
 *  @brief The threadpool struct
 *
 *  @var notify       Condition variable to notify worker threads.
 *  @var threads      Array containing worker threads ID.
 *  @var thread_count Number of threads
 *  @var queue        Queue containing head & tail.function
 *  @var function	  Func ptr to process all the items
 *  @var shutdown     Flag indicating if the pool is shutting down
 */
struct threadpool_t {
	pthread_mutex_t lock;
	pthread_cond_t notify;
	pthread_t *threads;
	list_t *queue;
	void (*function)(void *);
	int thread_count;
	int count;
	int shutdown;
	int started;
};

/**
 * @function void *threadpool_thread(void *threadpool)
 * @brief the worker thread
 * @param threadpool the pool which own the thread
 */
static void *threadpool_thread(void *threadpool);

int threadpool_free(threadpool_t *pool);

threadpool_t *threadpool_create(int thread_count, void (*function)( void *), int flags)
{
	threadpool_t *pool;
	int i, ret;

	/* TODO: Check for negative or otherwise very big input parameters */

	if((pool = (threadpool_t *)malloc(sizeof(threadpool_t))) == NULL) {
		goto err;
	}

	/* Initialize */
	pool->thread_count = thread_count;
	pool->function = function;
	pool->count = 0;
	pool->shutdown = pool->started = 0;

	/* Allocate thread and task queue */
	pool->threads = (pthread_t *)malloc(sizeof (pthread_t) * thread_count);

	pool->queue = ( list_t * ) calloc( 1, sizeof( list_t ) );

	/* Initialize mutex and conditional variable first */
	if((pthread_mutex_init(&(pool->lock), NULL) != 0) ||
			(pthread_cond_init(&(pool->notify), NULL) != 0) ||
			(pool->threads == NULL)
			|| (pool->queue == NULL)
	  )
	{
		goto err;
	}

	/* Start worker threads */
	for(i = 0; i < thread_count; i++) {
		if( pthread_create(&(pool->threads[i]), NULL,
					threadpool_thread, (void*)pool) != 0) {
			pool->thread_count = i;
			threadpool_destroy(pool, 0);
			return NULL;
		} else {
			pool->started++;
		}
	}

	return pool;

err:
	if(pool) {
		threadpool_free(pool);
	}
	return NULL;
}

int threadpool_add(threadpool_t *pool, void *argument, int flags)
{
	int err = 0;

	if(pool == NULL) {
		return threadpool_invalid;
	}


	// enqueue this new item
	wq_t *item = calloc( 1, sizeof( wq_t ) );

	if( item == NULL )
	{
		return threadpool_queue_full;
	}

	item->data = argument;
	//item->next = NULL;	calloc ensures NULL


	if(pthread_mutex_lock(&(pool->lock)) != 0) {
		free( item );
		return threadpool_lock_failure;
	}

	do {
		/* Are we shutting down ? */
		if(pool->shutdown) {
			free( item );
			err = threadpool_shutdown;
			break;
		}

		pool->count += 1;

		if( pool->queue->head == NULL )
		{
			pool->queue->head = pool->queue->tail = item;
		}
		else
		{
			pool->queue->tail->next = item;
			pool->queue->tail = item;
		}

		/* pthread_cond_broadcast */
		if(pthread_cond_signal(&(pool->notify)) != 0) {
			err = threadpool_lock_failure;
			break;
		}

	} while(0);

	if(pthread_mutex_unlock(&pool->lock) != 0) {
		err = threadpool_lock_failure;
	}

	return err;
}

int threadpool_destroy(threadpool_t *pool, int flags)
{
	int i, err = 0;

	if(pool == NULL) {
		return threadpool_invalid;
	}

	if(pthread_mutex_lock(&(pool->lock)) != 0) {
		return threadpool_lock_failure;
	}

	do {
		/* Already shutting down */
		if(pool->shutdown) {
			err = threadpool_shutdown;
			break;
		}

		pool->shutdown = 1;

		/* Wake up all worker threads */
		if((pthread_cond_broadcast(&(pool->notify)) != 0) ||
				(pthread_mutex_unlock(&(pool->lock)) != 0)) {
			err = threadpool_lock_failure;
			break;
		}

		/* Join all worker thread */
		for(i = 0; i < pool->thread_count; i++) {
			if(pthread_join(pool->threads[i], NULL) != 0) {
				err = threadpool_thread_failure;
			}
		}
	} while(0);

	if(pthread_mutex_unlock(&pool->lock) != 0) {
		err = threadpool_lock_failure;
	}

	/* Only if everything went well do we deallocate the pool */
	if(!err) {
		threadpool_free(pool);
	}
	return err;
}

int threadpool_free(threadpool_t *pool)
{
	if(pool == NULL || pool->started > 0) {
		return -1;
	}

	/* Did we manage to allocate ? */
	if(pool->threads) {
		free(pool->threads);
		free(pool->queue);

		/* Because we allocate pool->threads after initializing the
		   mutex and condition variable, we're sure they're
		   initialized. Let's lock the mutex just in case. */
		pthread_mutex_lock(&(pool->lock));
		pthread_mutex_destroy(&(pool->lock));
		pthread_cond_destroy(&(pool->notify));
	}
	free(pool);    
	return 0;
}


static void *threadpool_thread(void *threadpool)
{
	threadpool_t *pool = (threadpool_t *)threadpool;
	threadpool_task_t task;

	wq_t *item;

	for(;;) {
		/* Lock must be taken to wait on conditional variable */
		pthread_mutex_lock(&(pool->lock));

		/* Wait on condition variable, check for spurious wakeups.
		   When returning from pthread_cond_wait(), we own the lock. */
		while((pool->count == 0) && (!pool->shutdown)) {
			pthread_cond_wait(&(pool->notify), &(pool->lock));
		}

		if(pool->shutdown) {
			break;
		}


		item = pool->queue->head;

		pool->queue->head = item->next;

		pool->count -= 1;

		task.argument = item->data;

		/* Unlock */
		pthread_mutex_unlock(&(pool->lock));

		/* Get to work */
		(*(pool->function))(task.argument);

		free( item );
	}

	pool->started--;

	pthread_mutex_unlock(&(pool->lock));
	pthread_exit(NULL);
	return(NULL);
}
