#ifndef POSIX_HTTPS_PORT_H
#define POSIX_HTTPS_PORT_H

/*
 * Host implementation of dsp_http_port_t for the M1.8 integration harness: HTTP/1.1 POST
 * over real TLS (OpenSSL) with certificate-chain verification against a test CA and
 * hostname verification, bounded connect/IO timeouts and the same phase semantics as the
 * ESP-IDF adapter (connect failure / TLS failure before any byte, send failure, no response,
 * response). One connection per request ("Connection: close"). Test infrastructure only:
 * never part of the firmware image.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dsp_https_transport.h"

#define POSIX_PORT_LOG 64

typedef struct {
    char body[DSP_PROTO_BODY_MAX];
    dsp_http_outcome_t outcome;
} posix_port_log_t;

typedef struct {
    char host[128];
    uint16_t port;
    char path[128];
    char verify_name[64]; /* expected certificate host name */
    char ca_file[512];
    uint32_t timeout_ms;
    void *ssl_ctx; /* SSL_CTX * */
    bool open;
    /* Exchanges since the last posix_port_take_log(). */
    posix_port_log_t log[POSIX_PORT_LOG];
    int log_len;
    int posts_total;
} posix_port_t;

bool posix_port_init(posix_port_t *p, const char *ca_file);
void posix_port_set_target(posix_port_t *p, const char *host, uint16_t port, const char *path);
void posix_port_free(posix_port_t *p);
dsp_http_port_t posix_port_iface(posix_port_t *p);

#endif /* POSIX_HTTPS_PORT_H */
