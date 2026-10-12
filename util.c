/* Buffers, base64, minimal protobuf reader/writer. */
#include <stdlib.h>
#include <string.h>
#include "core.h"

/* ---- buffer ------------------------------------------------------------- */
int buf_reserve(buf_t *b, size_t extra)
{
    size_t need = b->len + extra + 1;   /* keep room for a NUL terminator */
    if (need <= b->cap)
        return 0;
    size_t ncap = b->cap ? b->cap : 256;
    while (ncap < need) {
        if (ncap > ((size_t)-1) / 2)
            return -1;
        ncap *= 2;
    }
    unsigned char *np = (unsigned char *)realloc(b->p, ncap);
    if (!np)
        return -1;
    b->p = np;
    b->cap = ncap;
    return 0;
}

int buf_append(buf_t *b, const void *data, size_t n)
{
    if (buf_reserve(b, n))
        return -1;
    if (n)
        memcpy(b->p + b->len, data, n);
    b->len += n;
    b->p[b->len] = 0;
    return 0;
}

int buf_append_str(buf_t *b, const char *s) { return buf_append(b, s, strlen(s)); }

void buf_free(buf_t *b)
{
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

/* ---- base64 ------------------------------------------------------------- */
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int b64_encode(buf_t *out, const unsigned char *in, size_t n)
{
    if (buf_reserve(out, (n + 2) / 3 * 4))
        return -1;
    char *o = (char *)out->p + out->len;
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
        unsigned v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        *o++ = B64[v >> 18]; *o++ = B64[(v >> 12) & 63];
        *o++ = B64[(v >> 6) & 63]; *o++ = B64[v & 63];
    }
    if (i < n) {
        unsigned v = in[i] << 16;
        if (i + 1 < n) v |= in[i + 1] << 8;
        *o++ = B64[v >> 18]; *o++ = B64[(v >> 12) & 63];
        *o++ = (i + 1 < n) ? B64[(v >> 6) & 63] : '=';
        *o++ = '=';
    }
    out->len = (size_t)((unsigned char *)o - out->p);
    out->p[out->len] = 0;
    return 0;
}

static int b64val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

int b64_decode(buf_t *out, const char *in, size_t n)
{
    if (buf_reserve(out, n / 4 * 3 + 3))
        return -1;
    unsigned acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        int c = (unsigned char)in[i];
        if (c == '=')
            break;
        if (c == ' ' || c == '\r' || c == '\n' || c == '\t')
            continue;
        int v = b64val(c);
        if (v < 0)
            return -1;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out->p[out->len++] = (unsigned char)(acc >> bits);
        }
    }
    out->p[out->len] = 0;
    return 0;
}

/* ---- protobuf ----------------------------------------------------------- */
static int rd_varint(pb_rd *r, uint64_t *out)
{
    uint64_t v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (r->p >= r->end)
            return -1;
        unsigned char b = *r->p++;
        v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) {
            *out = v;
            return 0;
        }
    }
    return -1;
}

int pb_next(pb_rd *r, pb_field *f)
{
    if (r->p >= r->end)
        return 0;
    uint64_t key;
    if (rd_varint(r, &key))
        return -1;
    f->field = (uint32_t)(key >> 3);
    f->wt = (int)(key & 7);
    f->data = NULL;
    f->len = 0;
    f->v = 0;
    switch (f->wt) {
    case 0:
        return rd_varint(r, &f->v) ? -1 : 1;
    case 1:
        if (r->end - r->p < 8) return -1;
        for (int i = 7; i >= 0; i--) f->v = (f->v << 8) | r->p[i];
        r->p += 8;
        return 1;
    case 5:
        if (r->end - r->p < 4) return -1;
        for (int i = 3; i >= 0; i--) f->v = (f->v << 8) | r->p[i];
        r->p += 4;
        return 1;
    case 2: {
        uint64_t l;
        if (rd_varint(r, &l) || l > (uint64_t)(r->end - r->p))
            return -1;
        f->data = r->p;
        f->len = (size_t)l;
        r->p += l;
        return 1;
    }
    default:
        return -1;   /* groups are not used by the Egg, Inc. API */
    }
}

double pb_double(const pb_field *f)
{
    double d = 0;
    if (f->wt == 1) {
        uint64_t v = f->v;
        memcpy(&d, &v, sizeof d);
    } else if (f->wt == 5) {
        uint32_t v = (uint32_t)f->v;
        float fl;
        memcpy(&fl, &v, sizeof fl);
        d = fl;
    }
    return d;
}

static int w_raw_varint(buf_t *b, uint64_t v)
{
    unsigned char tmp[10];
    int n = 0;
    do {
        unsigned char c = v & 0x7f;
        v >>= 7;
        if (v) c |= 0x80;
        tmp[n++] = c;
    } while (v);
    return buf_append(b, tmp, n);
}

int pb_w_varint(buf_t *b, uint32_t field, uint64_t v)
{
    return w_raw_varint(b, ((uint64_t)field << 3) | 0) || w_raw_varint(b, v) ? -1 : 0;
}

int pb_w_bytes(buf_t *b, uint32_t field, const void *data, size_t n)
{
    if (w_raw_varint(b, ((uint64_t)field << 3) | 2) || w_raw_varint(b, n))
        return -1;
    return buf_append(b, data, n);
}

int pb_w_string(buf_t *b, uint32_t field, const char *s)
{
    return pb_w_bytes(b, field, s, strlen(s));
}
