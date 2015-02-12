/*
 *  TurboXSL XML+XSLT processing library
 *  Thread pool library
 *
 *
 *  (c) Egor Voznessenski, voznyak@mail.ru
 *
 *  $Id$
 *
**/


#include "ltr_xsl.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct threadpool;

struct threadpool_task {
    void (*routine_cb)();

    TRANSFORM_CONTEXT *pctx;
    XMLNODE *ret;
    XMLNODE *source;
    XMLNODE *params;
    XMLNODE *locals;
    void *mode;
    pthread_t signature;
    struct threadpool *pool;
    pthread_mutex_t ownMutex;
    pthread_cond_t rcond;
    pthread_mutex_t rmutex;
};

struct threadpool {
    struct threadpool_task *tasks;
    pthread_t *thr_arr;
    unsigned short num_of_threads;

    pthread_mutex_t mutex;
    pthread_mutex_t lock;
    pthread_mutex_t block;
};

struct thread_lock {
    pthread_t thread;
    int is_locked;
};

struct thread_lock *locks = NULL;

int threadpool_lock_on()
{
    if (!locks) return 0;

    pthread_t self = pthread_self();
    for (int i = 0; i < 10; i++)
    {
        if (locks[i].thread == self)
        {
            if (locks[i].is_locked) return 0;

            debug("threadpool_lock_on:: locked");
            locks[i].is_locked = 1;
            return 1;
        }
    }

    return 0;
}

void threadpool_lock_off()
{
    if (!locks) return;

    pthread_t self = pthread_self();
    for (int i = 0; i < 10; i++)
    {
        if (locks[i].thread == self)
        {
            debug("threadpool_lock_off:: unlocked");
            locks[i].is_locked = 0;
            break;
        }
    }
}

void threadpool_wait(struct threadpool *pool)
{
    if (!pool) return;

    debug("threadpool_wait:: waiting");
    for (; ;)
    {
        int n = 0;
        for (int i = 0; i < pool->num_of_threads; ++i)
        {
            if (pool->tasks[i].signature) ++n;
        }

        if (n == 0) break;

        struct timespec polling_interval;
        polling_interval.tv_sec = 0;
        polling_interval.tv_nsec = 1000000;
        nanosleep(&polling_interval, NULL);
    }
}

void threadpool_start_full(void (*routine)(TRANSFORM_CONTEXT *, XMLNODE *, XMLNODE *, XMLNODE *, XMLNODE *, void *), TRANSFORM_CONTEXT *pctx, XMLNODE *ret, XMLNODE *source, XMLNODE *params, XMLNODE *locals, void *mode)
{
    struct threadpool *pool = pctx->gctx->pool;
    if (!pool)
    {
        (*routine)(pctx, ret, source, params, locals, mode);
        return;
    }

    pthread_t sig = pthread_self();
    for (int i = 0; i < 10; i++)
    {
        if (locks[i].thread == sig && locks[i].is_locked == 1)
        {
            debug("threadpool_start_full:: thread %p is locked", sig);
            (*routine)(pctx, ret, source, params, locals, mode);
            return;
        }
    }

    /* Obtain a task */
    if (pthread_mutex_lock(&(pool->mutex)))
    {
        perror("pthread_mutex_lock: ");
        return;
    }

    int i;
    for (i = 0; i < pool->num_of_threads; ++i)
    {
        if (pool->tasks[i].signature == 0)
        {
            debug("threadpool_start_full:: free task found");
            pool->tasks[i].pctx = pctx;
            pool->tasks[i].ret = ret;
            pool->tasks[i].source = source;
            pool->tasks[i].params = params;
            pool->tasks[i].locals = locals;
            pool->tasks[i].mode = mode;

            pool->tasks[i].routine_cb = routine;
            pool->tasks[i].signature = sig;

            pthread_mutex_lock(&(pool->tasks[i].rmutex));
            pthread_cond_broadcast(&(pool->tasks[i].rcond));
            pthread_mutex_unlock(&(pool->tasks[i].rmutex));

            break;
        }
    }

	if (pthread_mutex_unlock(&(pool->mutex)))
    {
		perror("pthread_mutex_unlock: ");
		return;
	}

	if (i >= pool->num_of_threads)
    {
        debug("threadpool_start_full:: no free task");
        (*routine)(pctx, ret, source, params, locals, mode);
    }
}


static void *worker_thr_routine(void *data)
{
    struct threadpool_task *task = (struct threadpool_task *) data;

    while (1)
    {
        pthread_mutex_lock(&(task->rmutex));
        while (task->signature == 0)
        {
            /* wait for task */
            pthread_cond_wait(&(task->rcond), &(task->rmutex));
        }
        pthread_mutex_unlock(&(task->rmutex));

        /* Execute routine (if any). */
        if (task->routine_cb)
        {
            task->routine_cb(task->pctx, task->ret, task->source, task->params, task->locals, task->mode);
        }
        task->signature = 0;
    }

    return NULL;
}

struct threadpool *threadpool_init(int num_of_threads)
{
    if (num_of_threads == 0) return NULL;

    /* Create the thread pool struct. */
    struct threadpool *pool;
    if ((pool = malloc(sizeof(struct threadpool))) == NULL)
    {
        perror("malloc: ");
        return NULL;
    }

    /* Init the mutex and cond vars. */
    if (pthread_mutex_init(&(pool->mutex), NULL))
    {
        perror("pthread_mutex_init: ");
        free(pool);
        return NULL;
    }
    if (pthread_mutex_init(&(pool->lock), NULL))
    {
        perror("pthread_mutex_init: ");
        free(pool);
        return NULL;
    }
    if (pthread_mutex_init(&(pool->block), NULL))
    {
        perror("pthread_mutex_init: ");
        free(pool);
        return NULL;
    }

    /* Create the thr_arr. */
    if ((pool->thr_arr = malloc(sizeof(pthread_t) * num_of_threads)) == NULL)
    {
        perror("malloc: ");
        free(pool);
        return NULL;
    }

    /* Create the task array. */
    if ((pool->tasks = malloc(sizeof(struct threadpool_task) * num_of_threads)) == NULL)
    {
        perror("malloc: ");
        free(pool->thr_arr);
        free(pool);
        return NULL;
    }

    locks = malloc(sizeof(struct thread_lock) * 10);
    memset(locks, 0, sizeof(struct thread_lock) * 10);
    locks[0].thread = pthread_self();
    locks[0].is_locked = 0;

    /* Start the worker threads. */
    for (pool->num_of_threads = 0; pool->num_of_threads < num_of_threads; (pool->num_of_threads)++)
    {
        if (pthread_mutex_init(&(pool->tasks[pool->num_of_threads].rmutex), NULL))
        {
            perror("pthread_mutex_init: ");
            free(pool);
            return NULL;
        }

        if (pthread_cond_init(&(pool->tasks[pool->num_of_threads].rcond), NULL))
        {
            perror("pthread_mutex_init: ");
            free(pool);
            return NULL;
        }

        if (pthread_mutex_init(&(pool->tasks[pool->num_of_threads].ownMutex), NULL)) {
            perror("thread_own_mutex_init: ");
            free(pool);
            return NULL;
        }

        pool->tasks[pool->num_of_threads].pool = pool;
        pool->tasks[pool->num_of_threads].signature = 0;

        if (pthread_create(&(pool->thr_arr[pool->num_of_threads]), NULL, worker_thr_routine, &(pool->tasks[pool->num_of_threads])))
        {
            perror("pthread_create:");
            threadpool_free(pool);
            return NULL;
        }

        locks[pool->num_of_threads + 1].thread = pool->thr_arr[pool->num_of_threads];
        locks[pool->num_of_threads + 1].is_locked = 0;
    }
    return pool;
}

void threadpool_free(struct threadpool *pool)
{
    // TODO
}

void threadpool_set_cache(struct threadpool *pool, node_cache *cache)
{
    debug("threadpool_set_cache:: setup");
    for (int i = 0; i < pool->num_of_threads; i++)
    {
        node_cache_add_entry(cache, pool->thr_arr[i], 100000);
    }
}
