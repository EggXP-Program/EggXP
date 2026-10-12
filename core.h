/*
 * EggXP core - portable (Win32 / POSIX) pieces: buffers, base64, protobuf,
 * TLS+HTTP connection, and the Egg, Inc. API messages.
 */
#ifndef EGGXP_CORE_H
#define EGGXP_CORE_H

#include <stddef.h>
#include <stdint.h>

/* ---- growable byte buffer ---------------------------------------------- */
typedef struct {
    unsigned char *p;
    size_t len, cap;
} buf_t;

int  buf_reserve(buf_t *b, size_t extra);
int  buf_append(buf_t *b, const void *data, size_t n);
int  buf_append_str(buf_t *b, const char *s);
void buf_free(buf_t *b);

/* ---- base64 ------------------------------------------------------------- */
int b64_encode(buf_t *out, const unsigned char *in, size_t n);
/* Decodes standard or URL-safe base64, skipping whitespace. */
int b64_decode(buf_t *out, const char *in, size_t n);

/* ---- protobuf ----------------------------------------------------------- */
typedef struct {
    const unsigned char *p, *end;
} pb_rd;

typedef struct {
    uint32_t field;
    int wt;                       /* 0 varint, 1 fixed64, 2 len-delimited, 5 fixed32 */
    uint64_t v;                   /* varint / fixed value */
    const unsigned char *data;    /* wt 2 */
    size_t len;                   /* wt 2 */
} pb_field;

/* 1 = field read, 0 = clean end, -1 = malformed */
int    pb_next(pb_rd *r, pb_field *f);
double pb_double(const pb_field *f);

int pb_w_varint(buf_t *b, uint32_t field, uint64_t v);
int pb_w_bytes(buf_t *b, uint32_t field, const void *data, size_t n);
int pb_w_string(buf_t *b, uint32_t field, const char *s);

/* ---- connection (TLS via BearSSL, or plain for testing) ----------------- */
typedef struct conn conn_t;

int     net_init(void);
conn_t *conn_new(const char *host, int port, int use_tls);
/* Optional HTTP CONNECT proxy (used by the test harness). */
void    conn_set_proxy(conn_t *c, const char *proxy_host, int proxy_port);
void    conn_close(conn_t *c);
void    conn_free(conn_t *c);
/* POST application/x-www-form-urlencoded. Returns 0 on success (any HTTP status). */
int     http_post(conn_t *c, const char *path, const unsigned char *body, size_t len,
                  int *status, buf_t *resp, char *err, size_t errlen);

/* ---- Egg, Inc. API ------------------------------------------------------ */
#define EI_API_HOST "ctx-dot-auxbrainhome.appspot.com"
#define EI_EP_FIRST_CONTACT "/ei/bot_first_contact"
#define EI_EP_COMPLETE_MISSION "/ei_afx/complete_mission"

typedef struct {
    int name, level, rarity, egg;
    char server_id[48];
} ei_drop;

typedef struct {
    char id[64];
    int ship, status, duration_type, type, level, capacity;
    int target;                   /* -1 = no target */
    double start, duration, quality_bump;
    int state;                    /* 0 pending, 1 fetched, -1 failed */
    char error[160];
    ei_drop *drops;
    int ndrops;
} ei_mission;

typedef struct {
    char eid[40];
    char user_name[128];
    double last_backup;
    ei_mission *missions;
    int nmissions;
} ei_account;

/* Builds the form body "data=<urlencoded base64(protobuf)>". */
int ei_build_first_contact(buf_t *form, const char *eid);
int ei_build_complete_mission(buf_t *form, const char *eid, const char *mission_id);

/* Parse decoded (base64 already removed) payloads. Return 0 on success. */
int ei_parse_first_contact(const unsigned char *p, size_t n, ei_account *acct,
                           char *err, size_t errlen);
int ei_parse_complete_mission(const unsigned char *p, size_t n, ei_mission *m,
                              char *err, size_t errlen);

/* High level: one API round trip, response decoded from base64.
 * 'raw' receives the protobuf payload (before AuthenticatedMessage unwrapping). */
int ei_call(conn_t *c, const char *path, const buf_t *form, buf_t *raw,
            char *err, size_t errlen);

void ei_mission_free(ei_mission *m);
void ei_account_free(ei_account *a);

/* Normalizes user input (trim, upper-case "ei" prefix). Returns 0 if plausible. */
int ei_normalize_eid(const char *in, char *out, size_t outlen);

/* ---- display names ------------------------------------------------------ */
const char *ei_ship_name(int ship);
const char *ei_duration_name(int d);
const char *ei_artifact_name(int name);
const char *ei_rarity_name(int r);
const char *ei_mission_type_name(int t);
int         ei_artifact_kind(int name);   /* 0 artifact, 1 stone, 2 ingredient, 3 fragment */
const char *ei_kind_name(int kind);

#endif
