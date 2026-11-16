#define _POSIX_C_SOURCE 200809L

#include "posix_https_port.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

static void wipe(void *p, size_t len)
{
    volatile unsigned char *v = (volatile unsigned char *)p;
    for (size_t i = 0; i < len; i++) {
        v[i] = 0;
    }
}

bool posix_port_init(posix_port_t *p, const char *ca_file)
{
    memset(p, 0, sizeof *p);
    snprintf(p->ca_file, sizeof p->ca_file, "%s", ca_file);
    snprintf(p->verify_name, sizeof p->verify_name, "localhost");
    snprintf(p->host, sizeof p->host, "localhost");
    snprintf(p->path, sizeof p->path, "/api/v1/events");
    p->timeout_ms = 2000;
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == NULL) {
        return false;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL); /* chain verification always on */
    if (SSL_CTX_load_verify_locations(ctx, p->ca_file, NULL) != 1) {
        SSL_CTX_free(ctx);
        return false;
    }
    p->ssl_ctx = ctx;
    return true;
}

void posix_port_set_target(posix_port_t *p, const char *host, uint16_t port, const char *path)
{
    snprintf(p->host, sizeof p->host, "%s", host);
    p->port = port;
    snprintf(p->path, sizeof p->path, "%s", path);
}

void posix_port_free(posix_port_t *p)
{
    if (p->ssl_ctx != NULL) {
        SSL_CTX_free((SSL_CTX *)p->ssl_ctx);
        p->ssl_ctx = NULL;
    }
}

/* Non-blocking connect bounded by timeout_ms; then blocking IO with socket timeouts. */
static int connect_bounded(const char *host, uint16_t port, uint32_t timeout_ms)
{
    char service[8];
    snprintf(service, sizeof service, "%u", (unsigned)port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, service, &hints, &res) != 0 || res == NULL) {
        return -1; /* DNS failure */
    }
    int fd = -1;
    for (struct addrinfo *a = res; a != NULL && fd < 0; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) {
            continue;
        }
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = connect(fd, a->ai_addr, a->ai_addrlen);
        if (rc != 0 && errno == EINPROGRESS) {
            struct pollfd pfd = {fd, POLLOUT, 0};
            int err = 0;
            socklen_t len = sizeof err;
            if (poll(&pfd, 1, (int)timeout_ms) == 1 &&
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) {
                rc = 0;
            }
        }
        if (rc != 0) {
            close(fd);
            fd = -1;
            continue;
        }
        fcntl(fd, F_SETFL, flags);
        struct timeval tv = {(time_t)(timeout_ms / 1000), (suseconds_t)((timeout_ms % 1000) * 1000)};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    }
    freeaddrinfo(res);
    return fd;
}

static void parse_response(const char *buf, size_t len, dsp_http_outcome_t *out)
{
    int status = 0;
    if (len < 12 || sscanf(buf, "HTTP/1.%*c %3d", &status) != 1 || status < 100 || status > 599) {
        out->phase = DSP_HTTP_NO_RESPONSE;
        return;
    }
    const char *end = NULL;
    for (size_t i = 0; i + 3 < len; i++) {
        if (memcmp(buf + i, "\r\n\r\n", 4) == 0) {
            end = buf + i + 4;
            break;
        }
    }
    if (end == NULL) {
        out->phase = DSP_HTTP_NO_RESPONSE; /* headers incomplete */
        return;
    }
    out->phase = DSP_HTTP_RESPONSE;
    out->status = (uint16_t)status;
    size_t body_len = len - (size_t)(end - buf);
    size_t copy = body_len < DSP_PROTO_RESPONSE_MAX ? body_len : DSP_PROTO_RESPONSE_MAX;
    memcpy(out->body, end, copy);
    out->body_len = (uint16_t)copy;
    out->body_truncated = body_len > DSP_PROTO_RESPONSE_MAX;
}

static bool port_open(void *ctx)
{
    posix_port_t *p = ctx;
    p->open = p->ssl_ctx != NULL;
    return p->open;
}

static void port_post(void *ctx, const char *body, size_t len, const char *token,
                      dsp_http_outcome_t *out)
{
    posix_port_t *p = ctx;
    memset(out, 0, sizeof *out);
    out->phase = DSP_HTTP_NOT_STARTED;
    p->posts_total++;
    if (p->log_len < POSIX_PORT_LOG) {
        snprintf(p->log[p->log_len].body, sizeof p->log[p->log_len].body, "%.*s", (int)len, body);
    }
    if (!p->open || len >= DSP_PROTO_BODY_MAX) {
        goto done;
    }
    int fd = connect_bounded(p->host, p->port, p->timeout_ms);
    if (fd < 0) {
        out->phase = DSP_HTTP_CONNECT_FAILED;
        goto done;
    }
    SSL *ssl = SSL_new((SSL_CTX *)p->ssl_ctx);
    if (ssl == NULL) {
        close(fd);
        out->phase = DSP_HTTP_TLS_FAILED;
        goto done;
    }
    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, p->verify_name);
    SSL_set1_host(ssl, p->verify_name); /* hostname verification */
    if (SSL_connect(ssl) != 1 || SSL_get_verify_result(ssl) != X509_V_OK) {
        out->phase = DSP_HTTP_TLS_FAILED; /* nothing of the request was sent */
        ERR_clear_error();
        SSL_free(ssl);
        close(fd);
        goto done;
    }
    static char req[DSP_PROTO_BODY_MAX + DSP_API_TOKEN_MAX_LEN + 512];
    int n = snprintf(req, sizeof req,
                     "POST %s HTTP/1.1\r\nHost: %s:%u\r\nContent-Type: application/json\r\n"
                     "Authorization: Bearer %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                     p->path, p->verify_name, (unsigned)p->port, token, len);
    bool sent_ok = n > 0 && (size_t)n + len < sizeof req;
    if (sent_ok) {
        memcpy(req + n, body, len);
        size_t total = (size_t)n + len;
        size_t off = 0;
        while (off < total) {
            int w = SSL_write(ssl, req + off, (int)(total - off));
            if (w <= 0) {
                sent_ok = false;
                break;
            }
            off += (size_t)w;
        }
    }
    wipe(req, sizeof req); /* contained the token */
    if (!sent_ok) {
        out->phase = DSP_HTTP_SEND_FAILED;
    } else {
        static char resp[8192];
        size_t got = 0;
        for (;;) {
            int r = SSL_read(ssl, resp + got, (int)(sizeof resp - 1 - got));
            if (r <= 0) {
                break; /* close, reset or timeout */
            }
            got += (size_t)r;
            if (got >= sizeof resp - 1) {
                break;
            }
        }
        resp[got] = '\0';
        if (got == 0) {
            out->phase = DSP_HTTP_NO_RESPONSE;
        } else {
            parse_response(resp, got, out);
        }
    }
    ERR_clear_error();
    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(fd);
done:
    if (p->log_len < POSIX_PORT_LOG) {
        p->log[p->log_len].outcome = *out;
        p->log_len++;
    }
}

static void port_close(void *ctx)
{
    posix_port_t *p = ctx;
    p->open = false;
}

dsp_http_port_t posix_port_iface(posix_port_t *p)
{
    dsp_http_port_t port = {p, port_open, port_post, port_close};
    return port;
}
