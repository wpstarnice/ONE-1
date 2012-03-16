/*
 * lwan - simple web server
 * Copyright (c) 2012 Leandro A. F. Pereira <leandro@hardinfo.org>
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
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "lwan.h"

static jmp_buf cleanup_jmp_buf;

const char *
lwan_determine_mime_type_for_file_name(char *file_name)
{
    char *last_dot = strrchr(file_name, '.');
    if (UNLIKELY(!last_dot))
        goto fallback;

    STRING_SWITCH(last_dot) {
    case EXT_CSS: return "text/css";
    case EXT_HTM: return "text/html";
    case EXT_JPG: return "image/jpeg";
    case EXT_JS:  return "application/javascript";
    case EXT_PNG: return "image/png";
    case EXT_TXT: return "text/plain";
    }

fallback:
    return "application/octet-stream";
}

const char *
lwan_http_status_as_string(lwan_http_status_t status)
{
    switch (status) {
    case HTTP_OK: return "OK";
    case HTTP_BAD_REQUEST: return "Bad request";
    case HTTP_NOT_FOUND: return "Not found";
    case HTTP_FORBIDDEN: return "Forbidden";
    case HTTP_NOT_ALLOWED: return "Not allowed";
    case HTTP_TOO_LARGE: return "Request too large";
    case HTTP_INTERNAL_ERROR: return "Internal server error";
    }
    return "Invalid";
}

#define SET_SOCKET_OPTION(_domain,_option,_param,_size) \
    do { \
        if (setsockopt(fd, (_domain), (_option), (_param), (_size)) < 0) { \
            perror("setsockopt"); \
            goto handle_error; \
        } \
    } while(0)

static void
_socket_init(lwan_t *l)
{
    struct sockaddr_in sin;
    int fd;

    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        perror("socket");
        exit(-1);
    }

    SET_SOCKET_OPTION(SOL_SOCKET, SO_REUSEADDR, (int[]){ 1 }, sizeof(int));
    if (l->config.enable_linger)
        SET_SOCKET_OPTION(SOL_SOCKET, SO_LINGER,
            ((struct linger[]){{ .l_onoff = 1, .l_linger = 1 }}), sizeof(struct linger));

    memset(&sin, 0, sizeof(sin));
    sin.sin_port = htons(l->config.port);
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_family = AF_INET;

    if (bind(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        perror("bind");
        goto handle_error;
    }

    if (listen(fd, l->thread.count * l->thread.max_fd) < 0) {
        perror("listen");
        goto handle_error;
    }

    l->main_socket = fd;
    return;

handle_error:
    close(fd);
    exit(-1);
}

#undef SET_SOCKET_OPTION

static void
_socket_shutdown(lwan_t *l)
{
    if (shutdown(l->main_socket, SHUT_RDWR) < 0) {
        perror("shutdown");
        close(l->main_socket);
        exit(-4);
    }
    close(l->main_socket);
}

ALWAYS_INLINE void
_reset_request(lwan_request_t *request, int fd)
{
    strbuf_t *response_buffer = request->response.buffer;

    memset(request, 0, sizeof(*request));
    request->fd = fd;
    request->response.buffer = response_buffer;
    strbuf_reset(request->response.buffer);
}

static void *
_thread(void *data)
{
    lwan_thread_t *t = data;
    struct epoll_event events[t->lwan->thread.max_fd];
    int epoll_fd = t->epoll_fd, n_fds, i;
    unsigned int death_time = 0;

    lwan_request_t *requests = t->lwan->requests;
    int *death_queue = calloc(1, t->lwan->thread.max_fd * sizeof(int));
    int death_queue_last = 0, death_queue_first = 0, death_queue_population = 0;

    for (; ; ) {
        switch (n_fds = epoll_wait(epoll_fd, events, N_ELEMENTS(events),
                                            death_queue_population ? 1000 : -1)) {
        case -1:
            switch (errno) {
            case EBADF:
            case EINVAL:
                goto epoll_fd_closed;
            case EINTR:
                perror("epoll_wait");
            }
            continue;
        case 0: /* timeout: shutdown waiting sockets */
            death_time++;

            while (death_queue_population) {
                lwan_request_t *request = &requests[death_queue[death_queue_first]];

                if (request->time_to_die <= death_time) {
                    /* One request just died, advance the queue. */
                    ++death_queue_first;
                    --death_queue_population;
                    death_queue_first %= t->lwan->thread.max_fd;

                    request->flags.alive = false;
                    close(request->fd);
                } else {
                    /* Next time. Next time. */
                    break;
                }
            }
            break;
        default: /* activity in some of this poller's file descriptor */
            for (i = 0; i < n_fds; ++i) {
                lwan_request_t *request = &requests[events[i].data.fd];

                if (events[i].events & (EPOLLRDHUP | EPOLLHUP)) {
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, events[i].data.fd, &events[i]) < 0)
                        perror("epoll_ctl");
                    goto invalidate_request;
                }

                if (!request->flags.alive)
                    _reset_request(request, events[i].data.fd);

                /*
                 * Even if the request couldn't be handled correctly,
                 * we still need to see if this is a keep-alive connection and
                 * act accordingly.
                 */
                lwan_process_request(t->lwan, request);

                if (request->flags.is_keep_alive) {
                    /*
                     * Update the time to die. This might overflow in ~136 years,
                     * so plan ahead.
                     */
                    request->time_to_die = death_time + t->lwan->config.keep_alive_timeout;

                    /*
                     * The connection hasn't been added to the keep-alive
                     * list-to-kill. Do it now and mark it as alive so that
                     * we know what to do whenever there's activity on its
                     * socket again. Or not. Mwahahaha.
                     */
                    if (!request->flags.alive) {
                        death_queue[death_queue_last++] = events[i].data.fd;
                        ++death_queue_population;
                        death_queue_last %= t->lwan->thread.max_fd;
                        request->flags.alive = true;
                    }
                    continue;
                }

                close(events[i].data.fd);
invalidate_request:
                request->flags.alive = false;
            }
        }
    }

epoll_fd_closed:
    free(death_queue);

    return NULL;
}

static void
_create_thread(lwan_t *l, int thread_n)
{
    pthread_attr_t attr;
    lwan_thread_t *thread = &l->thread.threads[thread_n];

    thread->lwan = l;
    if ((thread->epoll_fd = epoll_create1(0)) < 0) {
        perror("epoll_create");
        exit(-1);
    }

    if (pthread_attr_init(&attr)) {
        perror("pthread_attr_init");
        exit(-1);
    }

    if (pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM)) {
        perror("pthread_attr_setscope");
        exit(-1);
    }

    if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE)) {
        perror("pthread_attr_setdetachstate");
        exit(-1);
    }

    if (pthread_create(&thread->id, &attr, _thread, thread)) {
        perror("pthread_create");
        pthread_attr_destroy(&attr);
        exit(-1);
    }

    if (l->config.enable_thread_affinity) {
        cpu_set_t cpuset;

        CPU_ZERO(&cpuset);
        CPU_SET(thread_n, &cpuset);
        if (pthread_setaffinity_np(thread->id, sizeof(cpu_set_t), &cpuset)) {
            perror("pthread_setaffinity_np");
            exit(-1);
        }
    }

    if (pthread_attr_destroy(&attr)) {
        perror("pthread_attr_destroy");
        exit(-1);
    }
}

static void
_thread_init(lwan_t *l)
{
    int i;

    l->thread.threads = malloc(sizeof(lwan_thread_t) * l->thread.count);

    for (i = l->thread.count - 1; i >= 0; i--)
        _create_thread(l, i);
}

static void
_thread_shutdown(lwan_t *l)
{
    int i;

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
        pthread_join(l->thread.threads[i].id, NULL);

    free(l->thread.threads);
}

void
lwan_init(lwan_t *l)
{
    int max_threads = sysconf(_SC_NPROCESSORS_ONLN);
    struct rlimit r;

    l->thread.count = max_threads > 0 ? max_threads : 2;

    if (getrlimit(RLIMIT_NOFILE, &r) < 0) {
        perror("getrlimit");
        exit(-1);
    }
    if (r.rlim_max == RLIM_INFINITY)
        r.rlim_cur *= 8;
    else if (r.rlim_cur < r.rlim_max)
        r.rlim_cur = r.rlim_max;
    if (setrlimit(RLIMIT_NOFILE, &r) < 0) {
        perror("setrlimit");
        exit(-1);
    }

    l->requests = calloc(r.rlim_cur, sizeof(lwan_request_t));
    l->thread.max_fd = r.rlim_cur / l->thread.count;
    printf("Using %d threads, maximum %d sockets per thread.\n",
        l->thread.count, l->thread.max_fd);

    for (--r.rlim_cur; r.rlim_cur; --r.rlim_cur)
        l->requests[r.rlim_cur].response.buffer = strbuf_new();

    srand(time(NULL));
    signal(SIGPIPE, SIG_IGN);
    close(STDIN_FILENO);

    _socket_init(l);
    _thread_init(l);
}

void
lwan_shutdown(lwan_t *l)
{
    _thread_shutdown(l);
    _socket_shutdown(l);
    lwan_trie_destroy(l->url_map_trie);

    int i;
    for (i = l->thread.max_fd * l->thread.count - 1; i >= 0; --i)
        strbuf_free(l->requests[i].response.buffer);

    free(l->requests);
}

void
lwan_set_url_map(lwan_t *l, lwan_url_map_t *url_map)
{
    lwan_trie_destroy(l->url_map_trie);

    l->url_map_trie = lwan_trie_new();
    if (!l->url_map_trie) {
        perror("lwan_trie_new");
        exit(-1);
    }

    for (; url_map->prefix; url_map++) {
        url_map->prefix_len = strlen(url_map->prefix);
        lwan_trie_add(l->url_map_trie, url_map->prefix, url_map);
    }
}

ALWAYS_INLINE static int
_schedule_request(lwan_t *l)
{
#if defined(USE_LORENTZ_WATERWHEEL_SCHEDULER) && USE_LORENTZ_WATERWHEEL_SCHEDULER==1
    static unsigned int counter = 0;
    return ((rand() & 15) > 7 ? ++counter : --counter) % l->thread.count;
#else
    static int counter = 0;
    return counter++ % l->thread.count;
#endif
}

ALWAYS_INLINE static void
_push_request_fd(lwan_t *l, int fd)
{
    int epoll_fd = l->thread.threads[_schedule_request(l)].epoll_fd;
    struct epoll_event event = {
        .events = EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLET,
        .data.fd = fd
    };

    if (UNLIKELY(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0)) {
        perror("epoll_ctl");
        exit(-1);
    }
}

static void
_cleanup(int signal_number)
{
    printf("Signal %d received.\n", signal_number);
    longjmp(cleanup_jmp_buf, 1);
}

void
lwan_main_loop(lwan_t *l)
{
    if (setjmp(cleanup_jmp_buf))
        return;

    signal(SIGINT, _cleanup);

    int epoll_fd = epoll_create1(0);
    struct epoll_event events[128];
    struct epoll_event ev = {
        .events = EPOLLIN,
    };

    if (fcntl(l->main_socket, F_SETFL, O_NONBLOCK) < 0) {
        perror("fcntl: main socket");
        exit(-1);
    }
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, l->main_socket, &ev) < 0) {
        perror("epoll_ctl");
        exit(-1);
    }

    for (;;) {
        int n_fds;
        for (n_fds = epoll_wait(epoll_fd, events, N_ELEMENTS(events), -1);
                n_fds > 0;
                --n_fds) {
            int child_fd = accept4(l->main_socket, NULL, NULL, SOCK_NONBLOCK);
            if (UNLIKELY(child_fd < 0)) {
                perror("accept");
                continue;
            }

            _push_request_fd(l, child_fd);
        }
    }

    close(epoll_fd);
}
