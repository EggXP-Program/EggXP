/*
 * TCP + TLS 1.2 (BearSSL) + minimal HTTP/1.1 client with keep-alive.
 *
 * Windows XP's SChannel only speaks TLS 1.0, which Google's front ends
 * refuse, so TLS is done entirely in-process with BearSSL and an embedded
 * copy of the Mozilla root store. Nothing here depends on the OS cert store.
 */
#ifdef _WIN32
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0501
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
typedef SOCKET sock_t;
#  define BAD_SOCK INVALID_SOCKET
#  define sock_close closesocket
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <errno.h>
typedef int sock_t;
#  define BAD_SOCK (-1)
#  define sock_close close
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "bearssl.h"
#include "core.h"

#include "trust_anchors.inc"
#ifdef EGGXP_EXTRA_TA
#  include EGGXP_EXTRA_TA   /* test builds only: defines XTAs / XTAs_NUM */
#endif

#define IO_TIMEOUT_MS 20000

struct conn {
    char host[256];
    int port;
    int use_tls;
    char proxy_host[256];
    int proxy_port;

    sock_t fd;
    int connected;
    int reuse_count;

    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    br_sslio_context ioc;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];

    unsigned char rb[16384];
    size_t rpos, rlen;
};

static const br_x509_trust_anchor *g_tas;
static size_t g_tas_num;

int net_init(void)
{
#ifdef _WIN32
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w))
        return -1;
#endif
#ifdef EGGXP_EXTRA_TA
    static br_x509_trust_anchor *all;
    all = (br_x509_trust_anchor *)malloc(sizeof(TAs) + sizeof(XTAs));
    if (!all) return -1;
    memcpy(all, TAs, sizeof(TAs));
    memcpy(all + TAs_NUM, XTAs, sizeof(XTAs));
    g_tas = all;
    g_tas_num = TAs_NUM + XTAs_NUM;
#else
    g_tas = TAs;
    g_tas_num = TAs_NUM;
#endif
    return 0;
}

conn_t *conn_new(const char *host, int port, int use_tls)
{
    conn_t *c = (conn_t *)calloc(1, sizeof *c);
    if (!c)
        return NULL;
    snprintf(c->host, sizeof c->host, "%s", host);
    c->port = port;
    c->use_tls = use_tls;
    c->fd = BAD_SOCK;
    return c;
}

void conn_set_proxy(conn_t *c, const char *proxy_host, int proxy_port)
{
    snprintf(c->proxy_host, sizeof c->proxy_host, "%s", proxy_host ? proxy_host : "");
    c->proxy_port = proxy_port;
}

void conn_close(conn_t *c)
{
    if (c->fd != BAD_SOCK)
        sock_close(c->fd);
    c->fd = BAD_SOCK;
    c->connected = 0;
    c->reuse_count = 0;
    c->rpos = c->rlen = 0;
}

void conn_free(conn_t *c)
{
    if (!c) return;
    conn_close(c);
    free(c);
}

/* ---- raw socket I/O ----------------------------------------------------- */
static int raw_read(void *ctx, unsigned char *buf, size_t len)
{
    sock_t fd = *(sock_t *)ctx;
    for (;;) {
        int n = recv(fd, (char *)buf, (int)(len > 65536 ? 65536 : len), 0);
        if (n > 0)
            return n;
#ifndef _WIN32
        if (n < 0 && errno == EINTR)
            continue;
#endif
        return -1;
    }
}

static int raw_write(void *ctx, const unsigned char *buf, size_t len)
{
    sock_t fd = *(sock_t *)ctx;
    for (;;) {
        int n = send(fd, (const char *)buf, (int)(len > 65536 ? 65536 : len), 0);
        if (n > 0)
            return n;
#ifndef _WIN32
        if (n < 0 && errno == EINTR)
            continue;
#endif
        return -1;
    }
}

static int raw_write_all(sock_t fd, const char *s, size_t n)
{
    while (n) {
        int w = raw_write(&fd, (const unsigned char *)s, n);
        if (w <= 0) return -1;
        s += w;
        n -= (size_t)w;
    }
    return 0;
}

static void set_timeouts(sock_t fd)
{
#ifdef _WIN32
    DWORD ms = IO_TIMEOUT_MS;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof ms);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof ms);
#else
    struct timeval tv;
    tv.tv_sec = IO_TIMEOUT_MS / 1000;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
}

static sock_t tcp_connect(const char *host, int port, char *err, size_t errlen)
{
    struct addrinfo hints, *res = NULL, *ai;
    char portstr[16];
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;          /* XP rarely has a working IPv6 stack */
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    snprintf(portstr, sizeof portstr, "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) || !res) {
        snprintf(err, errlen, "DNS lookup failed for %s", host);
        return BAD_SOCK;
    }
    sock_t fd = BAD_SOCK;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == BAD_SOCK)
            continue;
        set_timeouts(fd);
        if (connect(fd, ai->ai_addr, (int)ai->ai_addrlen) == 0)
            break;
        sock_close(fd);
        fd = BAD_SOCK;
    }
    freeaddrinfo(res);
    if (fd == BAD_SOCK)
        snprintf(err, errlen, "could not connect to %s:%d", host, port);
    return fd;
}

/* ---- TLS error text ----------------------------------------------------- */
static const char *tls_err_text(int e)
{
    switch (e) {
    case BR_ERR_X509_EXPIRED:
        return "server certificate is expired or not yet valid - check the PC's date and time";
    case BR_ERR_X509_NOT_TRUSTED:
        return "server certificate is not trusted (unknown root CA)";
    case BR_ERR_X509_BAD_SERVER_NAME:
        return "server certificate does not match the host name";
    case BR_ERR_BAD_VERSION:
    case BR_ERR_BAD_CIPHER_SUITE:
        return "server refused TLS 1.2 parameters";
    case BR_ERR_IO:
        return "network I/O error";
    default:
        return NULL;
    }
}

static void tls_error(conn_t *c, const char *what, char *err, size_t errlen)
{
    int e = br_ssl_engine_last_error(&c->sc.eng);
    const char *t = tls_err_text(e);
    if (e == 0)
        snprintf(err, errlen, "%s: connection closed or timed out", what);
    else if (t)
        snprintf(err, errlen, "%s: %s (BearSSL error %d)", what, t, e);
    else
        snprintf(err, errlen, "%s: TLS error %d", what, e);
}

/* ---- connect (+ optional CONNECT proxy) -------------------------------- */
static int conn_open(conn_t *c, char *err, size_t errlen)
{
    conn_close(c);
    int via_proxy = c->proxy_host[0] != 0;
    c->fd = via_proxy ? tcp_connect(c->proxy_host, c->proxy_port, err, errlen)
                      : tcp_connect(c->host, c->port, err, errlen);
    if (c->fd == BAD_SOCK)
        return -1;

    if (via_proxy) {
        char req[600];
        snprintf(req, sizeof req, "CONNECT %s:%d HTTP/1.1\r\nHost: %s:%d\r\n\r\n",
                 c->host, c->port, c->host, c->port);
        if (raw_write_all(c->fd, req, strlen(req))) {
            snprintf(err, errlen, "proxy write failed");
            conn_close(c);
            return -1;
        }
        /* read proxy response headers byte by byte (tiny) */
        char hdr[2048];
        size_t hl = 0;
        while (hl < sizeof hdr - 1) {
            unsigned char ch;
            if (raw_read(&c->fd, &ch, 1) != 1) break;
            hdr[hl++] = (char)ch;
            if (hl >= 4 && !memcmp(hdr + hl - 4, "\r\n\r\n", 4)) break;
        }
        hdr[hl] = 0;
        if (strncmp(hdr, "HTTP/1.", 7) || strncmp(hdr + 9, "200", 3)) {
            snprintf(err, errlen, "proxy CONNECT refused");
            conn_close(c);
            return -1;
        }
    }

    if (c->use_tls) {
        br_ssl_client_init_full(&c->sc, &c->xc, g_tas, g_tas_num);
        br_ssl_engine_set_versions(&c->sc.eng, BR_TLS12, BR_TLS12);
        br_ssl_engine_set_buffer(&c->sc.eng, c->iobuf, sizeof c->iobuf, 1);
        if (!br_ssl_client_reset(&c->sc, c->host, 0)) {
            tls_error(c, "TLS init", err, errlen);
            conn_close(c);
            return -1;
        }
        br_sslio_init(&c->ioc, &c->sc.eng, raw_read, &c->fd, raw_write, &c->fd);
    }
    c->connected = 1;
    c->reuse_count = 0;
    return 0;
}

static int c_write_all(conn_t *c, const void *data, size_t n)
{
    if (c->use_tls)
        return br_sslio_write_all(&c->ioc, data, n);
    return raw_write_all(c->fd, (const char *)data, n);
}

static int c_flush(conn_t *c)
{
    return c->use_tls ? br_sslio_flush(&c->ioc) : 0;
}

/* buffered reader */
static int c_fill(conn_t *c)
{
    int n = c->use_tls ? br_sslio_read(&c->ioc, c->rb, sizeof c->rb)
                       : raw_read(&c->fd, c->rb, sizeof c->rb);
    if (n <= 0)
        return -1;
    c->rpos = 0;
    c->rlen = (size_t)n;
    return 0;
}

static int c_getc(conn_t *c)
{
    if (c->rpos >= c->rlen && c_fill(c))
        return -1;
    return c->rb[c->rpos++];
}

static int c_read_line(conn_t *c, char *line, size_t max)
{
    size_t n = 0;
    for (;;) {
        int ch = c_getc(c);
        if (ch < 0)
            return -1;
        if (ch == '\n')
            break;
        if (n + 1 < max)
            line[n++] = (char)ch;
    }
    if (n && line[n - 1] == '\r')
        n--;
    line[n] = 0;
    return (int)n;
}

static int c_read_n(conn_t *c, buf_t *out, size_t n)
{
    if (buf_reserve(out, n))
        return -1;
    while (n) {
        if (c->rpos >= c->rlen && c_fill(c))
            return -1;
        size_t take = c->rlen - c->rpos;
        if (take > n) take = n;
        memcpy(out->p + out->len, c->rb + c->rpos, take);
        out->len += take;
        c->rpos += take;
        n -= take;
    }
    out->p[out->len] = 0;
    return 0;
}

static int ci_prefix(const char *s, const char *pfx)
{
    while (*pfx) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*pfx))
            return 0;
        s++, pfx++;
    }
    return 1;
}

static int ci_contains(const char *s, const char *needle)
{
    for (; *s; s++)
        if (ci_prefix(s, needle))
            return 1;
    return 0;
}

/* One request/response on an open connection. *got_any is set once a byte of response arrives. */
static int do_request(conn_t *c, const char *path, const unsigned char *body, size_t len,
                      int *status, buf_t *resp, int *got_any, char *err, size_t errlen)
{
    char hdr[1024];
    *got_any = 0;
    snprintf(hdr, sizeof hdr,
             "POST %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "User-Agent: EggXP/1.0\r\n"
             "Accept: */*\r\n"
             "Content-Type: application/x-www-form-urlencoded\r\n"
             "Content-Length: %lu\r\n"
             "Connection: keep-alive\r\n\r\n",
             path, c->host, (unsigned long)len);
    if (c_write_all(c, hdr, strlen(hdr)) || c_write_all(c, body, len) || c_flush(c)) {
        if (c->use_tls) tls_error(c, "send", err, errlen);
        else snprintf(err, errlen, "send failed");
        return -1;
    }

    char line[4096];
    if (c_read_line(c, line, sizeof line) < 0) {
        if (c->use_tls) tls_error(c, "receive", err, errlen);
        else snprintf(err, errlen, "receive failed");
        return -1;
    }
    *got_any = 1;
    if (strncmp(line, "HTTP/1.", 7) || strlen(line) < 12) {
        snprintf(err, errlen, "bad HTTP status line");
        return -1;
    }
    *status = atoi(line + 9);

    long clen = -1;
    int chunked = 0, close_after = 0;
    if (line[7] == '0') close_after = 1;   /* HTTP/1.0 */
    for (;;) {
        int n = c_read_line(c, line, sizeof line);
        if (n < 0) { snprintf(err, errlen, "connection lost in headers"); return -1; }
        if (n == 0) break;
        if (ci_prefix(line, "content-length:")) clen = atol(line + 15);
        else if (ci_prefix(line, "transfer-encoding:") && ci_contains(line + 18, "chunked")) chunked = 1;
        else if (ci_prefix(line, "connection:")) {
            if (ci_contains(line + 11, "close")) close_after = 1;
            else if (ci_contains(line + 11, "keep-alive")) close_after = 0;
        }
    }

    resp->len = 0;
    if (chunked) {
        for (;;) {
            if (c_read_line(c, line, sizeof line) < 0) { snprintf(err, errlen, "connection lost in chunk"); return -1; }
            unsigned long sz = strtoul(line, NULL, 16);
            if (sz == 0) {
                while ((c_read_line(c, line, sizeof line)) > 0) {}   /* trailers */
                break;
            }
            if (sz > 256ul * 1024 * 1024 || c_read_n(c, resp, sz)) { snprintf(err, errlen, "connection lost in chunk data"); return -1; }
            if (c_read_line(c, line, sizeof line) < 0) { snprintf(err, errlen, "bad chunk terminator"); return -1; }
        }
    } else if (clen >= 0) {
        if (c_read_n(c, resp, (size_t)clen)) { snprintf(err, errlen, "connection lost in body"); return -1; }
    } else {
        /* read to EOF */
        for (;;) {
            int ch = c_getc(c);
            if (ch < 0) break;
            unsigned char b = (unsigned char)ch;
            if (buf_append(resp, &b, 1)) return -1;
        }
        close_after = 1;
    }
    if (!resp->p) buf_append(resp, "", 0);
    if (close_after)
        conn_close(c);
    else
        c->reuse_count++;
    return 0;
}

int http_post(conn_t *c, const char *path, const unsigned char *body, size_t len,
              int *status, buf_t *resp, char *err, size_t errlen)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        int reused = c->connected && c->reuse_count > 0;
        if (!c->connected && conn_open(c, err, errlen))
            return -1;
        int got_any = 0;
        if (do_request(c, path, body, len, status, resp, &got_any, err, errlen) == 0)
            return 0;
        conn_close(c);
        /* a kept-alive connection the server already dropped: retry once on a fresh one */
        if (!(reused && !got_any))
            return -1;
    }
    return -1;
}
