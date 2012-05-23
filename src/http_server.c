#define _GNU_SOURCE
#include "http_server.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/resource.h>
#include "sstr.h"

#define ACCEPTOR_STACK_SIZE 8192
#define MIN_HTTP_REQ_SIZE 5 // method(3) + space(1) + URI(1) + optional VER...

SSTRL(HEAD, "HEAD " );
SSTRL(GET,  "GET "  );
SSTRL(POST, "POST " );
SSTRL(PUT,  "PUT "  );

SSTRL(CRLFCRLF, "\r\n\r\n");
SSTRL(CRLF, "\r\n");

SSTR(CONNECTION, "\r\nConnection: ");
SSTR(CONNECTION_CLOSE, "close");
SSTR(CONNECTION_KEEPALIVE, "Keep-Alive");
SSTR(CONTENT_LENGTH, "\r\nContent-Length: ");

/* 1xx */
SSTRL(HTTP_STATUS_100, "100 Continue");
SSTRL(EXPECT_100, "\r\nExpect: 100");
/* 4xx */
SSTRL(HTTP_STATUS_411, "411 Length Required");
/* 5xx */
SSTRL(HTTP_STATUS_500, "500 Internal Server Error");
SSTRL(HTTP_STATUS_501, "501 Not Implemented");
SSTRL(HTTP_STATUS_503, "503 Service Unavailable");


SSTR(HTTP_SERVER_VER, "HTTP/1.1");

extern struct ribs_context *current_ctx;

#define IDLE_STACK_SIZE 4096

static void http_server_fiber_cleanup(void) {
    struct http_server *server = (struct http_server *)current_ctx->data.ptr;
    ctx_pool_put(&server->ctx_pool, current_ctx);
}

static void http_server_idle_handler(void) {
    for (;;) {
        struct ribs_context *old_ctx = current_ctx;
        struct http_server *server = (struct http_server *)current_ctx->data.ptr;
        current_ctx = ctx_pool_get(&server->ctx_pool);
        ribs_makecontext(current_ctx, &main_ctx, current_ctx, http_server_fiber_main, http_server_fiber_cleanup);
        current_ctx->fd = current_epollev.data.fd;
        current_ctx->data.ptr = server;
        fd_to_ctx[current_epollev.data.fd] = current_ctx;
        ribs_swapcontext(current_ctx, old_ctx);
    }
}

int http_server_init(struct http_server *server, uint16_t port, void (*func)(void)) {
    server->user_func = func;

    /*
     * idle connection handler
     */
    server->idle_stack = malloc(IDLE_STACK_SIZE);
    ribs_makecontext(&server->idle_ctx, &main_ctx, server->idle_stack + IDLE_STACK_SIZE, http_server_idle_handler, NULL);
    server->idle_ctx.data.ptr = server;

    /*
     * context pool
     */
    struct rlimit rlim;
    if (0 > getrlimit(RLIMIT_STACK, &rlim))
        return perror("getrlimit(RLIMIT_STACK)"), -1;

    long total_mem = sysconf(_SC_PHYS_PAGES);
    if (total_mem < 0)
        return perror("sysconf"), -1;
    total_mem *= getpagesize();
    size_t num_ctx_in_one_map = total_mem / rlim.rlim_cur;
    /* half of total mem to start with so we don't need to enable overcommit */
    num_ctx_in_one_map >>= 1;
    printf("pool: initial=%zu, grow=%zu\n", num_ctx_in_one_map, num_ctx_in_one_map);
    ctx_pool_init(&server->ctx_pool, num_ctx_in_one_map, num_ctx_in_one_map, rlim.rlim_cur, sizeof(struct http_server_context));

    /*
     * listen socket
     */
    const int LISTEN_BACKLOG = 32768;
    int lfd = socket(PF_INET, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);
    if (0 > lfd)
        return -1;

    int rc;
    const int option = 1;
    rc = setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));
    if (0 > rc)
        return perror("setsockopt, SO_REUSEADDR"), rc;

    rc = setsockopt(lfd, IPPROTO_TCP, TCP_NODELAY, &option, sizeof(option));
    if (0 > rc)
        return perror("setsockopt, TCP_NODELAY"), rc;

    struct linger ls;
    ls.l_onoff = 0;
    ls.l_linger = 0;
    rc = setsockopt(lfd, SOL_SOCKET, SO_LINGER, (void *)&ls, sizeof(ls));
    if (0 > rc)
        return perror("setsockopt, SO_LINGER"), rc;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(struct sockaddr_in));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (0 > bind(lfd, (struct sockaddr *)&addr, sizeof(addr)))
        return perror("bind"), -1;

    if (0 > listen(lfd, LISTEN_BACKLOG))
        return perror("listen"), -1;

    printf("listening on port: %d, backlog: %d\n", port, LISTEN_BACKLOG);

    /*
     * acceptor
     */
    server->accept_stack = malloc(ACCEPTOR_STACK_SIZE);
    ribs_makecontext(&server->accept_ctx, &main_ctx, server->accept_stack + ACCEPTOR_STACK_SIZE, http_server_accept_connections, NULL);
    server->accept_ctx.fd = lfd;
    server->accept_ctx.data.ptr = server;
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    fd_to_ctx[lfd] = &server->accept_ctx;
    ev.data.fd = lfd;
    if (0 > epoll_ctl(ribs_epoll_fd, EPOLL_CTL_ADD, lfd, &ev))
        return perror("epoll_ctl"), -1;

    return 0;
}

void http_server_accept_connections(void) {
    for (;; yield()) {
        struct sockaddr_in new_addr;
        socklen_t new_addr_size = sizeof(struct sockaddr_in);
        int fd = accept4(current_ctx->fd, (struct sockaddr *)&new_addr, &new_addr_size, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0)
            continue;

        struct http_server *server = (struct http_server *)current_ctx->data.ptr;
        fd_to_ctx[fd] = &server->idle_ctx;

        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = fd;
        if (0 > epoll_ctl(ribs_epoll_fd, EPOLL_CTL_ADD, fd, &ev))
            perror("epoll_ctl");
    }
}

static int check_persistent(char *p) {
    char *conn = strstr(p, CONNECTION);
    char *h1_1 = strstr(p, " HTTP/1.1");
    // HTTP/1.1
    if ((NULL != h1_1 &&
         (NULL == conn ||
          0 != SSTRNCMPI(CONNECTION_CLOSE, conn + SSTRLEN(CONNECTION)))) ||
        // HTTP/1.0
        (NULL == h1_1 &&
         NULL != conn &&
         0 == SSTRNCMPI(CONNECTION_KEEPALIVE, conn + SSTRLEN(CONNECTION))))
        return 1;
    else
        return 0;
}

#define READ_FROM_SOCKET()                                              \
    res = vmbuf_read(&ctx->request, fd);                                \
    if (0 >= res) {                                                     \
        close(fd); /* remote side closed or other error occured */      \
        return;                                                         \
    }

void http_server_fiber_main(void) {
    struct http_server_context *ctx = http_server_get_context();
    struct http_server *server = (struct http_server *)current_ctx->data.ptr;
    int fd = current_ctx->fd;

    char *URI;
    char *headers;
    char *content;
    size_t content_length;
    int persistent = 0;
    int res;

    vmbuf_init(&ctx->request, 4096);
    vmbuf_init(&ctx->response, 4096);

    /*
      TODO:
      if (inbuf.wlocpos() > max_req_size)
      return response(HTTP_STATUS_413, HTTP_CONTENT_TYPE_TEXT_PLAIN);
    */
    for (;; yield()) {
        READ_FROM_SOCKET();
        if (vmbuf_wlocpos(&ctx->request) > MIN_HTTP_REQ_SIZE)
            break;
    }

    if (0 == SSTRNCMP(GET, vmbuf_data(&ctx->request)) || 0 == SSTRNCMP(HEAD, vmbuf_data(&ctx->request))) {
        /* GET or HEAD */
        while (0 != SSTRNCMP(CRLFCRLF,  vmbuf_wloc(&ctx->request) - SSTRLEN(CRLFCRLF))) {
            yield();
            READ_FROM_SOCKET();
        }
        /* make sure the string is \0 terminated */
        /* this will overwrite the first CR */
        *(vmbuf_wloc(&ctx->request) - SSTRLEN(CRLFCRLF)) = 0;
        char *p = vmbuf_data(&ctx->request);
        persistent = check_persistent(p);
        URI = strchrnul(p, ' '); /* can't be NULL GET and HEAD constants have space at the end */
        *URI = 0;
        ++URI; // skip the space
        p = strchrnul(URI, '\r'); /* HTTP/1.0 */
        headers = p;
        if (0 != *headers) /* are headers present? */
            headers += SSTRLEN(CRLF); /* skip the new line */
        *p = 0;
        p = strchrnul(URI, ' '); /* truncate the version part */
        *p = 0; /* \0 at the end of URI */
        content = NULL;
        content_length = 0;
        server->user_func();
    } else if (0 == SSTRNCMP(POST, vmbuf_data(&ctx->request)) || 0 == SSTRNCMP(PUT, vmbuf_data(&ctx->request))) {
        /* POST or PUT */
        for (;;) {
            *vmbuf_wloc(&ctx->request) = 0;
            /* wait until we have the header */
            if (NULL != (content = strstr(vmbuf_data(&ctx->request), CRLFCRLF)))
                break;
            yield();
            READ_FROM_SOCKET();
        }

        *content = 0; /* terminate at the first CR like in GET */
        content += SSTRLEN(CRLFCRLF);

        if (strstr(vmbuf_data(&ctx->request), EXPECT_100)) {
            vmbuf_sprintf(&ctx->response, "%s %s\r\n\r\n", HTTP_SERVER_VER, HTTP_STATUS_100);
            if (0 > vmbuf_write(&ctx->response, fd)) {
                close(fd);
                return;
            }
            vmbuf_reset(&ctx->response);
        }
        persistent = check_persistent(vmbuf_data(&ctx->request));

        /* parse the content length */
        char *p = strcasestr(vmbuf_data(&ctx->request), CONTENT_LENGTH);
        if (NULL == p) {
            vmbuf_sprintf(&ctx->response, "%s %s\r\nServer: test%s%s\r\n\r\n", HTTP_SERVER_VER, HTTP_STATUS_411, CONNECTION, CONNECTION_CLOSE);
            goto http_server_write_response;
        }

        p += SSTRLEN(CONTENT_LENGTH);
        content_length = atoi(p);
        for (;;) {
            if (content + content_length <= vmbuf_wloc(&ctx->request))
                break;
            yield();
            READ_FROM_SOCKET();
        }
        p = vmbuf_data(&ctx->request);
        URI = strchrnul(p, ' '); /* can't be NULL PUT and POST constants have space at the end */
        *URI = 0;
        ++URI; /* skip the space */
        p = strchrnul(URI, '\r'); /* HTTP/1.0 */
        headers = p;
        if (0 != *headers) /* are headers present? */
            headers += SSTRLEN(CRLF); /* skip the new line */
        *p = 0;
        p = strchrnul(URI, ' '); /* truncate http version */
        *p = 0; /* \0 at the end of URI */
        *(content + content_length) = 0;
        server->user_func();
    } else {
        vmbuf_sprintf(&ctx->response, "%s %s\r\nServer: test%s%s\r\n\r\n", HTTP_SERVER_VER, HTTP_STATUS_501, CONNECTION, CONNECTION_CLOSE);
        goto http_server_write_response;
    }
http_server_write_response:
    for (;;yield()) {
        int res = vmbuf_write(&ctx->response, fd);
        if (res < 0)
            persistent = 0;
        if (0 != res)
            break;
    }

    if (persistent)
        fd_to_ctx[fd] = &server->idle_ctx;
    else
        close(fd);

}
