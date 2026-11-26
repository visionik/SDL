/*
  Simple DirectMedia Layer
  Copyright (C) 2026 Visionik <visionik@pobox.com>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not
     be misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#ifndef KITTY_RECT_STANDALONE
#include "SDL_internal.h"
#endif

#if defined(SDL_VIDEO_DRIVER_KITTY) || defined(KITTY_RECT_STANDALONE)

#include "SDL_kittyparallel.h"

#ifdef __APPLE__
#include <dispatch/dispatch.h>

void KITTY_ParallelFor(size_t count, KITTY_ParallelFn fn, void *arg)
{
    if (!fn || count == 0) {
        return;
    }
    if (count == 1) {
        fn(arg, 0);
        return;
    }
    dispatch_apply_f(count,
                     dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0),
                     arg, fn);
}

#else /* Linux / POSIX persistent pthread worker pool */

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>

#define KITTY_POOL_MAX_WORKERS 15

typedef struct KITTY_ThreadPool
{
    pthread_mutex_t mu;
    pthread_cond_t cv_start;
    pthread_cond_t cv_done;
    pthread_t workers[KITTY_POOL_MAX_WORKERS];
    int n_workers;
    int started;
    int stopping;

    /* Current batch state (serialized by call_mu). */
    pthread_mutex_t call_mu;
    KITTY_ParallelFn fn;
    void *arg;
    size_t count;
    atomic_size_t next_idx;
    unsigned long generation;
    int active_workers;
} KITTY_ThreadPool;

static KITTY_ThreadPool g_kitty_pool = {
    PTHREAD_MUTEX_INITIALIZER,
    PTHREAD_COND_INITIALIZER,
    PTHREAD_COND_INITIALIZER,
    { 0 },
    0,
    0,
    0,
    PTHREAD_MUTEX_INITIALIZER,
    NULL,
    NULL,
    0,
    0,
    0,
    0
};

static void *KITTY_PoolWorkerMain(void *unused)
{
    unsigned long my_gen = 0;
    (void)unused;

    pthread_mutex_lock(&g_kitty_pool.mu);
    for (;;) {
        while (g_kitty_pool.generation == my_gen && !g_kitty_pool.stopping) {
            pthread_cond_wait(&g_kitty_pool.cv_start, &g_kitty_pool.mu);
        }
        if (g_kitty_pool.stopping) {
            break;
        }
        my_gen = g_kitty_pool.generation;
        {
            const KITTY_ParallelFn fn = g_kitty_pool.fn;
            void *arg = g_kitty_pool.arg;
            const size_t count = g_kitty_pool.count;
            pthread_mutex_unlock(&g_kitty_pool.mu);

            for (;;) {
                const size_t idx = atomic_fetch_add_explicit(&g_kitty_pool.next_idx, 1u, memory_order_relaxed);
                if (idx >= count) {
                    break;
                }
                fn(arg, idx);
            }

            pthread_mutex_lock(&g_kitty_pool.mu);
            g_kitty_pool.active_workers--;
            if (g_kitty_pool.active_workers == 0) {
                pthread_cond_signal(&g_kitty_pool.cv_done);
            }
        }
    }
    pthread_mutex_unlock(&g_kitty_pool.mu);
    return NULL;
}

static void KITTY_PoolAtExit(void)
{
    int i;
    pthread_mutex_lock(&g_kitty_pool.mu);
    if (!g_kitty_pool.started) {
        pthread_mutex_unlock(&g_kitty_pool.mu);
        return;
    }
    g_kitty_pool.stopping = 1;
    pthread_cond_broadcast(&g_kitty_pool.cv_start);
    pthread_mutex_unlock(&g_kitty_pool.mu);

    for (i = 0; i < g_kitty_pool.n_workers; ++i) {
        pthread_join(g_kitty_pool.workers[i], NULL);
    }
    g_kitty_pool.n_workers = 0;
    g_kitty_pool.started = 0;
}

static void KITTY_PoolEnsureStarted(void)
{
    long ncpu;
    int want, i;

    if (g_kitty_pool.started) {
        return;
    }
    ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    want = (ncpu > 1) ? (int)(ncpu - 1) : 0;
    if (want > KITTY_POOL_MAX_WORKERS) {
        want = KITTY_POOL_MAX_WORKERS;
    }
    for (i = 0; i < want; ++i) {
        if (pthread_create(&g_kitty_pool.workers[i], NULL, KITTY_PoolWorkerMain, NULL) != 0) {
            break;
        }
        g_kitty_pool.n_workers++;
    }
    g_kitty_pool.started = 1;
    if (g_kitty_pool.n_workers > 0) {
        atexit(KITTY_PoolAtExit);
    }
}

void KITTY_ParallelFor(size_t count, KITTY_ParallelFn fn, void *arg)
{
    if (!fn || count == 0) {
        return;
    }
    if (count == 1) {
        fn(arg, 0);
        return;
    }

    pthread_mutex_lock(&g_kitty_pool.call_mu);
    KITTY_PoolEnsureStarted();
    if (g_kitty_pool.n_workers == 0 || g_kitty_pool.stopping) {
        size_t i;
        pthread_mutex_unlock(&g_kitty_pool.call_mu);
        for (i = 0; i < count; ++i) {
            fn(arg, i);
        }
        return;
    }

    pthread_mutex_lock(&g_kitty_pool.mu);
    g_kitty_pool.fn = fn;
    g_kitty_pool.arg = arg;
    g_kitty_pool.count = count;
    atomic_store_explicit(&g_kitty_pool.next_idx, 0u, memory_order_relaxed);
    g_kitty_pool.active_workers = g_kitty_pool.n_workers;
    g_kitty_pool.generation++;
    pthread_cond_broadcast(&g_kitty_pool.cv_start);
    pthread_mutex_unlock(&g_kitty_pool.mu);

    /* Main thread participates in work-stealing loop alongside workers. */
    for (;;) {
        const size_t idx = atomic_fetch_add_explicit(&g_kitty_pool.next_idx, 1u, memory_order_relaxed);
        if (idx >= count) {
            break;
        }
        fn(arg, idx);
    }

    pthread_mutex_lock(&g_kitty_pool.mu);
    while (g_kitty_pool.active_workers > 0) {
        pthread_cond_wait(&g_kitty_pool.cv_done, &g_kitty_pool.mu);
    }
    pthread_mutex_unlock(&g_kitty_pool.mu);
    pthread_mutex_unlock(&g_kitty_pool.call_mu);
}

#endif /* __APPLE__ */

#endif /* SDL_VIDEO_DRIVER_KITTY || KITTY_RECT_STANDALONE */
