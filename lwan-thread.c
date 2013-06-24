/*
 * lwan - simple web server
 * Copyright (c) 2012, 2013 Leandro A. F. Pereira <leandro@hardinfo.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include "lwan.h"

struct death_queue_t {
    unsigned last;
    unsigned first;
    unsigned population;
    unsigned max;
    unsigned time;
    int *queue;
    lwan_request_t *requests;
};

static lwan_key_value_t empty_query_string_kv[] = {
    { .key = NULL, .value = NULL }
};

#define ONE_HOUR 3600
#define ONE_DAY (ONE_HOUR * 24)
#define ONE_WEEK (ONE_DAY * 7)
#define ONE_MONTH (ONE_DAY * 31)

ALWAYS_INLINE void
_reset_request(lwan_request_t *request)
{
    strbuf_t *response_buffer = request->response.buffer;
    lwan_t *lwan = request->lwan;
    lwan_thread_t *thread = request->thread;
    coro_t *coro = request->coro;
    int fd = request->fd;
    struct sockaddr_in remote_address = request->remote_address;

    if (request->query_string_kv.base != empty_query_string_kv)
        free(request->query_string_kv.base);

    memset(request, 0, sizeof(*request));

    request->fd = fd;
    request->lwan = lwan;
    request->thread = thread;
    request->coro = coro;
    request->response.buffer = response_buffer;
    request->query_string_kv.base = empty_query_string_kv;
    request->remote_address = remote_address;
    strbuf_reset(request->response.buffer);
}

static ALWAYS_INLINE void
_cleanup_coro(lwan_request_t *request)
{
    if (!request->coro || request->flags.should_resume_coro)
        return;
    /* FIXME: Reuse coro? */
    coro_free(request->coro);
    request->coro = NULL;
}

static ALWAYS_INLINE void
_handle_hangup(lwan_request_t *request)
{
    request->flags.alive = false;
    close(request->fd);
}

static int
_process_request_coro(coro_t *coro)
{
    lwan_request_t *request = coro_get_data(coro);

    _reset_request(request);
    if (LIKELY(lwan_process_request(request)))
        return 0;

    /*
     * If we fail sending the request (such as the connection has been reset by
     * peer), just forcefully kill it from our side as well, as there's probably
     * no point in keep their resources further.
     *
     * FIXME: Check if lwan_process_request() actually returns false only on non-
     *        recoverable errors; EWOULDBLOCK, EAGAIN, EINTR and such should just
     *        yield from the coroutine and processing the request should be tried
     *        again. Maybe a boolean doesn't suffice here.
     * FIXME: Double-check if the request socket will be removed from the epoll.
     * FIXME: Double-check that the coroutine's resources are really freed.
     */
    _reset_request(request);
    _handle_hangup(request);

    return 0;
}

static ALWAYS_INLINE void
_spawn_coro_if_needed(lwan_request_t *request, coro_switcher_t *switcher)
{
    if (request->coro)
        return;
    request->coro = coro_new(switcher, _process_request_coro, request);
    request->flags.should_resume_coro = true;
    request->flags.write_events = false;
}

static ALWAYS_INLINE void
_resume_coro_if_needed(lwan_request_t *request, int epoll_fd)
{
    assert(request->coro);

    if (!request->flags.should_resume_coro)
        return;

    request->flags.should_resume_coro = coro_resume(request->coro);
    if (request->flags.should_resume_coro == request->flags.write_events)
        return;

    static const int const events_by_write_flag[] = {
        EPOLLOUT | EPOLLRDHUP | EPOLLERR,
        EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLET
    };
    struct epoll_event event = {
        .events = events_by_write_flag[request->flags.write_events],
        .data.fd = request->fd
    };

    if (UNLIKELY(epoll_ctl(epoll_fd, EPOLL_CTL_MOD, request->fd, &event) < 0))
        lwan_status_perror("epoll_ctl");

    request->flags.write_events ^= 1;
}

static void
_death_queue_init(struct death_queue_t *dq, lwan_request_t *requests, unsigned max)
{
    dq->queue = calloc(1, max * sizeof(int));
    dq->last = 0;
    dq->first = 0;
    dq->population = 0;
    dq->time = 0;
    dq->max = max;
    dq->requests = requests;
}

static void
_death_queue_shutdown(struct death_queue_t *dq)
{
    if (!dq)
        return;
    free(dq->queue);
}

static void
_death_queue_pop(struct death_queue_t *dq)
{
    dq->first++;
    dq->population--;
    dq->first %= dq->max;
}

static void
_death_queue_push(struct death_queue_t *dq, lwan_request_t *request)
{
    dq->queue[dq->last] = request->fd;
    dq->last++;
    dq->population++;
    dq->last %= dq->max;
    request->flags.alive = true;
}

static ALWAYS_INLINE lwan_request_t *
_death_queue_first(struct death_queue_t *dq)
{
    return &dq->requests[dq->queue[dq->first]];
}

static ALWAYS_INLINE int
_death_queue_epoll_timeout(struct death_queue_t *dq)
{
    return dq->population ? 1000 : -1;
}

static void
_death_queue_kill_waiting(struct death_queue_t *dq)
{
    dq->time++;

    while (dq->population) {
        lwan_request_t *request = _death_queue_first(dq);

        if (request->time_to_die > dq->time)
            break;

        _death_queue_pop(dq);

        /* This request might have died from a hangup event */
        if (!request->flags.alive)
            continue;

        _cleanup_coro(request);

        request->flags.alive = false;
        close(request->fd);
    }
}

void
lwan_format_rfc_time(time_t t, char buffer[32])
{
    if (UNLIKELY(!strftime(buffer, 31, "%a, %d %b %Y %H:%M:%S GMT", gmtime(&t))))
        lwan_status_perror("strftime");
}

static void
_update_date_cache(lwan_thread_t *thread)
{
    time_t now = time(NULL);
    if (now != thread->date.last) {
        thread->date.last = now;
        lwan_format_rfc_time(now, thread->date.date);
        lwan_format_rfc_time(now + ONE_WEEK, thread->date.expires);
    }
}

static void *
_thread_io_loop(void *data)
{
    lwan_thread_t *t = data;
    struct epoll_event *events;
    lwan_request_t *requests = t->lwan->requests;
    coro_switcher_t switcher;
    struct death_queue_t dq;
    int epoll_fd = t->epoll_fd;
    int n_fds;
    int i;

    lwan_status_debug("Starting IO loop on thread #%d", t->id + 1);

    events = calloc(t->lwan->thread.max_fd, sizeof(*events));
    if (UNLIKELY(!events))
        lwan_status_critical("Could not allocate memory for events");

    _death_queue_init(&dq, requests, t->lwan->thread.max_fd);

    for (;;) {
        switch (n_fds = epoll_wait(epoll_fd, events, t->lwan->thread.max_fd,
                                   _death_queue_epoll_timeout(&dq))) {
        case -1:
            switch (errno) {
            case EBADF:
            case EINVAL:
                goto epoll_fd_closed;
            }
            continue;
        case 0: /* timeout: shutdown waiting sockets */
            _death_queue_kill_waiting(&dq);
            break;
        default: /* activity in some of this poller's file descriptor */
            _update_date_cache(t);

            for (i = 0; i < n_fds; ++i) {
                lwan_request_t *request = &requests[events[i].data.fd];

                request->fd = events[i].data.fd;

                if (UNLIKELY(events[i].events & (EPOLLRDHUP | EPOLLHUP))) {
                    _handle_hangup(request);
                    continue;
                }

                _cleanup_coro(request);
                _spawn_coro_if_needed(request, &switcher);
                _resume_coro_if_needed(request, epoll_fd);

                /*
                 * If the connection isn't keep alive, it might have a
                 * coroutine that should be resumed.  If that's the case,
                 * schedule for this request to die according to the keep
                 * alive timeout.
                 *
                 * If it's not a keep alive connection, or the coroutine
                 * shouldn't be resumed -- then just mark it to be reaped
                 * right away.
                 */
                if (LIKELY(request->flags.is_keep_alive || request->flags.should_resume_coro))
                    request->time_to_die = dq.time + t->lwan->config.keep_alive_timeout;
                else
                    request->time_to_die = dq.time;

                /*
                 * The connection hasn't been added to the keep-alive and
                 * resumable coro list-to-kill.  Do it now and mark it as
                 * alive so that we know what to do whenever there's
                 * activity on its socket again.  Or not.  Mwahahaha.
                 */
                if (!request->flags.alive)
                    _death_queue_push(&dq, request);
            }
        }
    }

epoll_fd_closed:
    _death_queue_shutdown(&dq);
    free(events);

    return NULL;
}

static void
_create_thread(lwan_t *l, int thread_n)
{
    pthread_attr_t attr;
    lwan_thread_t *thread = &l->thread.threads[thread_n];

    thread->lwan = l;
    thread->id = thread_n;
    if ((thread->epoll_fd = epoll_create1(0)) < 0) {
        lwan_status_perror("epoll_create");
        exit(-1);
    }

    if (pthread_attr_init(&attr)) {
        lwan_status_perror("pthread_attr_init");
        exit(-1);
    }

    if (pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM)) {
        lwan_status_perror("pthread_attr_setscope");
        exit(-1);
    }

    if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE)) {
        lwan_status_perror("pthread_attr_setdetachstate");
        exit(-1);
    }

    if (pthread_create(&thread->self, &attr, _thread_io_loop, thread)) {
        lwan_status_perror("pthread_create");
        pthread_attr_destroy(&attr);
        exit(-1);
    }

    if (pthread_attr_destroy(&attr)) {
        lwan_status_perror("pthread_attr_destroy");
        exit(-1);
    }
}

void
lwan_thread_init(lwan_t *l)
{
    int i;

    lwan_status_debug("Initializing threads");

    l->thread.threads = malloc(sizeof(lwan_thread_t) * l->thread.count);

    for (i = l->thread.count - 1; i >= 0; i--)
        _create_thread(l, i);
}

void
lwan_thread_shutdown(lwan_t *l)
{
    int i;

    lwan_status_debug("Shutting down threads");

    /*
     * Closing epoll_fd makes the thread gracefully finish; it might
     * take a while to notice this if keep-alive timeout is high.
     * Thread shutdown is performed in separate loops so that we
     * don't wait one thread to join when there are others to be
     * finalized.
     */
    for (i = l->thread.count - 1; i >= 0; i--)
        close(l->thread.threads[i].epoll_fd);
    for (i = l->thread.count - 1; i >= 0; i--)
#ifdef __linux__
        pthread_tryjoin_np(l->thread.threads[i].self, NULL);
#else
        pthread_join(l->thread.threads[i].self, NULL);
#endif /* __linux__ */

    free(l->thread.threads);
}