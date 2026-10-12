/*
 * Egg, Inc. API: request building and response parsing.
 *
 * Field numbers follow the community ei.proto (github.com/carpetsage/egg),
 * and the request shape mirrors EggLedger's ApiClient:
 *   POST /ei/bot_first_contact       EggIncFirstContactRequest -> EggIncFirstContactResponse (bare)
 *   POST /ei_afx/complete_mission    MissionRequest -> AuthenticatedMessage{CompleteMissionResponse}
 * Bodies are form-encoded "data=<base64 protobuf>", responses are base64 text.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "core.h"
#include "tinf.h"

#define EI_CLIENT_VERSION 72
#define EI_APP_VERSION "1.35.7"
#define EI_APP_BUILD "111343"
#define EI_PLATFORM_IOS 1
#define EI_DEVICE_ID "EggXP"

/* ---- requests ----------------------------------------------------------- */
static int build_rinfo(buf_t *b, const char *eid)
{
    return pb_w_string(b, 1, eid) ||
           pb_w_varint(b, 2, EI_CLIENT_VERSION) ||
           pb_w_string(b, 3, EI_APP_VERSION) ||
           pb_w_string(b, 4, EI_APP_BUILD) ||
           pb_w_string(b, 5, "IOS") ? -1 : 0;
}

static int form_wrap(buf_t *form, const buf_t *msg)
{
    buf_t b64 = {0};
    int rc = -1;
    if (b64_encode(&b64, msg->p, msg->len))
        goto out;
    form->len = 0;
    if (buf_append_str(form, "data="))
        goto out;
    for (size_t i = 0; i < b64.len; i++) {
        char ch = (char)b64.p[i];
        const char *esc = ch == '+' ? "%2B" : ch == '/' ? "%2F" : ch == '=' ? "%3D" : NULL;
        if (esc ? buf_append_str(form, esc) : buf_append(form, &ch, 1))
            goto out;
    }
    rc = 0;
out:
    buf_free(&b64);
    return rc;
}

int ei_build_first_contact(buf_t *form, const char *eid)
{
    buf_t rinfo = {0}, msg = {0};
    int rc = -1;
    if (build_rinfo(&rinfo, eid))
        goto out;
    if (pb_w_varint(&msg, 2, EI_CLIENT_VERSION) ||          /* client_version */
        pb_w_varint(&msg, 3, EI_PLATFORM_IOS) ||            /* platform */
        pb_w_string(&msg, 4, eid) ||                        /* ei_user_id */
        pb_w_string(&msg, 5, EI_DEVICE_ID) ||               /* device_id */
        pb_w_bytes(&msg, 8, rinfo.p, rinfo.len))            /* rinfo */
        goto out;
    rc = form_wrap(form, &msg);
out:
    buf_free(&rinfo);
    buf_free(&msg);
    return rc;
}

int ei_build_complete_mission(buf_t *form, const char *eid, const char *mission_id)
{
    buf_t rinfo = {0}, info = {0}, msg = {0};
    int rc = -1;
    if (build_rinfo(&rinfo, eid) || pb_w_string(&info, 7, mission_id))  /* MissionInfo.identifier */
        goto out;
    if (pb_w_varint(&msg, 1, EI_CLIENT_VERSION) ||          /* client_version */
        pb_w_bytes(&msg, 2, info.p, info.len) ||            /* info */
        pb_w_string(&msg, 3, eid) ||                        /* ei_user_id */
        pb_w_bytes(&msg, 4, rinfo.p, rinfo.len))            /* rinfo */
        goto out;
    rc = form_wrap(form, &msg);
out:
    buf_free(&rinfo);
    buf_free(&info);
    buf_free(&msg);
    return rc;
}

/* ---- response parsing --------------------------------------------------- */
static void copy_str(char *dst, size_t dstlen, const pb_field *f)
{
    size_t n = f->len < dstlen - 1 ? f->len : dstlen - 1;
    memcpy(dst, f->data, n);
    dst[n] = 0;
}

/* MissionInfo */
static int parse_mission_info(const unsigned char *p, size_t n, ei_mission *m)
{
    pb_rd r = { p, p + n };
    pb_field f;
    int rc;
    memset(m, 0, sizeof *m);
    m->target = -1;
    while ((rc = pb_next(&r, &f)) == 1) {
        switch (f.field) {
        case 1:  m->ship = (int)f.v; break;
        case 2:  m->status = (int)f.v; break;
        case 3:  m->duration_type = (int)f.v; break;
        case 5:  m->duration = pb_double(&f); break;
        case 7:  if (f.wt == 2) copy_str(m->id, sizeof m->id, &f); break;
        case 8:  m->start = pb_double(&f); break;
        case 9:  m->capacity = (int)f.v; break;
        case 11: m->quality_bump = pb_double(&f); break;
        case 12: m->level = (int)f.v; break;
        case 13: m->target = (int)f.v; break;
        case 14: m->type = (int)f.v; break;
        default: break;
        }
    }
    return rc < 0 ? -1 : 0;
}

typedef struct {
    ei_mission *v;
    int n, cap;
} mvec;

static int mvec_push(mvec *mv, const ei_mission *m)
{
    if (mv->n == mv->cap) {
        int nc = mv->cap ? mv->cap * 2 : 256;
        ei_mission *nv = (ei_mission *)realloc(mv->v, (size_t)nc * sizeof *nv);
        if (!nv) return -1;
        mv->v = nv;
        mv->cap = nc;
    }
    mv->v[mv->n++] = *m;
    return 0;
}

static int parse_artifacts_db(const unsigned char *p, size_t n, mvec *mv)
{
    pb_rd r = { p, p + n };
    pb_field f;
    int rc;
    while ((rc = pb_next(&r, &f)) == 1) {
        /* 4 = mission_infos, 5 = mission_archive */
        if ((f.field == 4 || f.field == 5) && f.wt == 2) {
            ei_mission m;
            if (parse_mission_info(f.data, f.len, &m))
                return -1;
            if (mvec_push(mv, &m))
                return -1;
        }
    }
    return rc < 0 ? -1 : 0;
}

static int parse_backup(const unsigned char *p, size_t n, ei_account *a, mvec *mv, int *has_afx)
{
    pb_rd r = { p, p + n };
    pb_field f;
    int rc;
    while ((rc = pb_next(&r, &f)) == 1) {
        if (f.field == 2 && f.wt == 2) {
            copy_str(a->user_name, sizeof a->user_name, &f);
        } else if (f.field == 4 && f.wt == 2) {               /* settings */
            pb_rd s = { f.data, f.data + f.len };
            pb_field g;
            while (pb_next(&s, &g) == 1)
                if (g.field == 24) a->last_backup = pb_double(&g);
        } else if (f.field == 15 && f.wt == 2) {              /* artifacts_db */
            *has_afx = 1;
            if (parse_artifacts_db(f.data, f.len, mv))
                return -1;
        }
    }
    return rc < 0 ? -1 : 0;
}

static int cmp_start(const void *x, const void *y)
{
    const ei_mission *a = (const ei_mission *)x, *b = (const ei_mission *)y;
    if (a->start < b->start) return -1;
    if (a->start > b->start) return 1;
    return strcmp(a->id, b->id);
}

static int cmp_id(const void *x, const void *y)
{
    return strcmp(((const ei_mission *)x)->id, ((const ei_mission *)y)->id);
}

int ei_parse_first_contact(const unsigned char *p, size_t n, ei_account *acct,
                           char *err, size_t errlen)
{
    pb_rd r = { p, p + n };
    pb_field f;
    int rc, error_code = 0, has_backup = 0, has_afx = 0;
    char error_message[200] = "";
    mvec mv = {0};

    while ((rc = pb_next(&r, &f)) == 1) {
        if (f.field == 1 && f.wt == 2) {
            has_backup = 1;
            if (parse_backup(f.data, f.len, acct, &mv, &has_afx)) { rc = -1; break; }
        } else if (f.field == 4) {
            error_code = (int)f.v;
        } else if (f.field == 5 && f.wt == 2) {
            copy_str(error_message, sizeof error_message, &f);
        }
    }
    if (rc < 0) {
        snprintf(err, errlen, "malformed first_contact response");
        free(mv.v);
        return -1;
    }
    if (error_code > 0) {
        snprintf(err, errlen, "server error %d%s%s%s", error_code,
                 error_code == 2 ? " (user not found - check the EID)" : "",
                 error_message[0] ? ": " : "", error_message);
        free(mv.v);
        return -1;
    }
    if (!has_backup && error_message[0]) {
        snprintf(err, errlen, "server says: %s", error_message);
        free(mv.v);
        return -1;
    }
    if (!has_backup) {
        snprintf(err, errlen, "no backup returned - check the EID, or open the game once so it uploads a backup");
        free(mv.v);
        return -1;
    }
    if (!has_afx) {
        snprintf(err, errlen, "backup has no artifacts database (has this account unlocked missions?)");
        free(mv.v);
        return -1;
    }

    /* keep COMPLETE (20) / ARCHIVED (25), de-duplicate by id, sort by launch time */
    int k = 0;
    for (int i = 0; i < mv.n; i++)
        if ((mv.v[i].status == 20 || mv.v[i].status == 25) && mv.v[i].id[0])
            mv.v[k++] = mv.v[i];
    qsort(mv.v, (size_t)k, sizeof *mv.v, cmp_id);
    int u = 0;
    for (int i = 0; i < k; i++)
        if (u == 0 || strcmp(mv.v[u - 1].id, mv.v[i].id))
            mv.v[u++] = mv.v[i];
    qsort(mv.v, (size_t)u, sizeof *mv.v, cmp_start);

    acct->missions = mv.v;
    acct->nmissions = u;
    return 0;
}

static int inflate_zlib(const unsigned char *in, size_t n, size_t hint, buf_t *out)
{
    static int inited;
    if (!inited) { tinf_init(); inited = 1; }
    size_t cap = hint ? hint : n * 4 + 1024;
    for (int tries = 0; tries < 8; tries++) {
        out->len = 0;
        if (buf_reserve(out, cap))
            return -1;
        unsigned int dlen = (unsigned int)cap;
        int r = tinf_zlib_uncompress(out->p, &dlen, in, (unsigned int)n);
        if (r == TINF_OK) {
            out->len = dlen;
            out->p[out->len] = 0;
            return 0;
        }
        if (r != TINF_BUF_ERROR)
            return -1;
        cap *= 4;
    }
    return -1;
}

static int parse_secure_spec(const unsigned char *p, size_t n, ei_drop *d)
{
    pb_rd r = { p, p + n };
    pb_field f;
    int rc;
    memset(d, 0, sizeof *d);
    while ((rc = pb_next(&r, &f)) == 1) {
        if (f.field == 1 && f.wt == 2) {               /* ArtifactSpec */
            pb_rd s = { f.data, f.data + f.len };
            pb_field g;
            int rc2;
            while ((rc2 = pb_next(&s, &g)) == 1) {
                if (g.field == 1) d->name = (int)g.v;
                else if (g.field == 2) d->level = (int)g.v;
                else if (g.field == 3) d->rarity = (int)g.v;
                else if (g.field == 4) d->egg = (int)g.v;
            }
            if (rc2 < 0) return -1;
        } else if (f.field == 2 && f.wt == 2) {
            copy_str(d->server_id, sizeof d->server_id, &f);
        }
    }
    return rc < 0 ? -1 : 0;
}

int ei_parse_complete_mission(const unsigned char *p, size_t n, ei_mission *m,
                              char *err, size_t errlen)
{
    /* AuthenticatedMessage { 1: bytes message, 4: bool compressed, 5: original_size } */
    pb_rd r = { p, p + n };
    pb_field f;
    const unsigned char *msg = NULL;
    size_t msglen = 0, orig = 0;
    int compressed = 0, rc;
    while ((rc = pb_next(&r, &f)) == 1) {
        if (f.field == 1 && f.wt == 2) { msg = f.data; msglen = f.len; }
        else if (f.field == 4) compressed = f.v != 0;
        else if (f.field == 5) orig = (size_t)f.v;
    }
    if (rc < 0 || !msg) {
        snprintf(err, errlen, "malformed AuthenticatedMessage");
        return -1;
    }

    buf_t infl = {0};
    if (compressed) {
        if (inflate_zlib(msg, msglen, orig, &infl)) {
            snprintf(err, errlen, "zlib inflate failed");
            buf_free(&infl);
            return -1;
        }
        msg = infl.p;
        msglen = infl.len;
    }

    /* CompleteMissionResponse { 1 success, 2 info, 3 repeated SecureArtifactSpec artifacts } */
    pb_rd c = { msg, msg + msglen };
    int success = 0, cap = 0;
    ei_drop *drops = NULL;
    int nd = 0;
    rc = 0;
    while ((rc = pb_next(&c, &f)) == 1) {
        if (f.field == 1) {
            success = f.v != 0;
        } else if (f.field == 3 && f.wt == 2) {
            if (nd == cap) {
                cap = cap ? cap * 2 : 16;
                ei_drop *nv = (ei_drop *)realloc(drops, (size_t)cap * sizeof *nv);
                if (!nv) { rc = -1; break; }
                drops = nv;
            }
            if (parse_secure_spec(f.data, f.len, &drops[nd])) { rc = -1; break; }
            nd++;
        }
    }
    buf_free(&infl);
    if (rc < 0) {
        free(drops);
        snprintf(err, errlen, "malformed CompleteMissionResponse (if this repeats, it may be the known server-side invalid UTF-8 bug)");
        return -1;
    }
    if (!success) {
        free(drops);
        snprintf(err, errlen, "server returned success=false");
        return -1;
    }
    free(m->drops);
    m->drops = drops;
    m->ndrops = nd;
    return 0;
}

int ei_call(conn_t *c, const char *path, const buf_t *form, buf_t *raw,
            char *err, size_t errlen)
{
    buf_t resp = {0};
    int status = 0;
    if (http_post(c, path, form->p, form->len, &status, &resp, err, errlen)) {
        buf_free(&resp);
        return -1;
    }
    if (status != 200) {
        size_t show = resp.len > 120 ? 120 : resp.len;
        for (size_t i = 0; i < show; i++)
            if (resp.p[i] < 32 || resp.p[i] > 126) resp.p[i] = ' ';
        snprintf(err, errlen, "HTTP %d: %.*s", status, (int)show, resp.p ? (char *)resp.p : "");
        buf_free(&resp);
        return -1;
    }
    raw->len = 0;
    int rc = b64_decode(raw, (const char *)resp.p, resp.len);
    buf_free(&resp);
    if (rc) {
        snprintf(err, errlen, "response was not base64");
        return -1;
    }
    return 0;
}

void ei_mission_free(ei_mission *m)
{
    free(m->drops);
    m->drops = NULL;
    m->ndrops = 0;
}

void ei_account_free(ei_account *a)
{
    for (int i = 0; i < a->nmissions; i++)
        ei_mission_free(&a->missions[i]);
    free(a->missions);
    a->missions = NULL;
    a->nmissions = 0;
}

int ei_normalize_eid(const char *in, char *out, size_t outlen)
{
    size_t n = 0;
    for (; *in && n + 1 < outlen; in++) {
        unsigned char ch = (unsigned char)*in;
        if (isspace(ch))
            continue;
        out[n] = (char)(n < 2 ? toupper(ch) : ch);
        n++;
    }
    out[n] = 0;
    if (n < 4 || out[0] != 'E' || out[1] != 'I')
        return -1;
    for (size_t i = 2; i < n; i++)
        if (!isdigit((unsigned char)out[i]))
            return -1;
    return 0;
}

/* ---- names -------------------------------------------------------------- */
const char *ei_ship_name(int s)
{
    static const char *n[] = {
        "Chicken One", "Chicken Nine", "Chicken Heavy", "BCR", "Quintillion Chicken",
        "Cornish-Hen Corvette", "Galeggtica", "Defihent", "Voyegger", "Henerprise",
        "Atreggies Henliner",
    };
    return (s >= 0 && s < (int)(sizeof n / sizeof *n)) ? n[s] : "Unknown ship";
}

const char *ei_duration_name(int d)
{
    switch (d) {
    case 0: return "Short";
    case 1: return "Standard";
    case 2: return "Extended";
    case 3: return "Tutorial";
    default: return "?";
    }
}

const char *ei_mission_type_name(int t) { return t == 1 ? "Virtue" : "Home"; }

const char *ei_rarity_name(int r)
{
    switch (r) {
    case 0: return "Common";
    case 1: return "Rare";
    case 2: return "Epic";
    case 3: return "Legendary";
    default: return "?";
    }
}

typedef struct { int id; const char *name; int kind; } art_name;
static const art_name ARTS[] = {
    {0, "Lunar Totem", 0}, {3, "Neodymium Medallion", 0}, {4, "Beak of Midas", 0},
    {5, "Light of Eggendil", 0}, {6, "Demeters Necklace", 0}, {7, "Vial of Martian Dust", 0},
    {8, "Gusset", 0}, {9, "The Chalice", 0}, {10, "Book of Basan", 0},
    {11, "Phoenix Feather", 0}, {12, "Tungsten Ankh", 0}, {21, "Aurelian Brooch", 0},
    {22, "Carved Rainstick", 0}, {23, "Puzzle Cube", 0}, {24, "Quantum Metronome", 0},
    {25, "Ship in a Bottle", 0}, {26, "Tachyon Deflector", 0}, {27, "Interstellar Compass", 0},
    {28, "Dilithium Monocle", 0}, {29, "Titanium Actuator", 0}, {30, "Mercury's Lens", 0},
    {1, "Tachyon Stone", 1}, {31, "Dilithium Stone", 1}, {32, "Shell Stone", 1},
    {33, "Lunar Stone", 1}, {34, "Soul Stone", 1}, {39, "Prophecy Stone", 1},
    {36, "Quantum Stone", 1}, {37, "Terra Stone", 1}, {38, "Life Stone", 1},
    {40, "Clarity Stone", 1},
    {13, "Extraterrestrial Aluminum", 2}, {14, "Ancient Tungsten", 2}, {15, "Space Rocks", 2},
    {16, "Alien Wood", 2}, {17, "Gold Meteorite", 2}, {18, "Tau Ceti Geode", 2},
    {19, "Centaurian Steel", 2}, {20, "Eridani Feather", 2}, {35, "Drone Parts", 2},
    {41, "Celestial Bronze", 2}, {42, "Lalande Hide", 2}, {43, "Solar Titanium", 2},
    {2, "Tachyon Stone Fragment", 3}, {44, "Dilithium Stone Fragment", 3},
    {45, "Shell Stone Fragment", 3}, {46, "Lunar Stone Fragment", 3},
    {47, "Soul Stone Fragment", 3}, {48, "Prophecy Stone Fragment", 3},
    {49, "Quantum Stone Fragment", 3}, {50, "Terra Stone Fragment", 3},
    {51, "Life Stone Fragment", 3}, {52, "Clarity Stone Fragment", 3},
};

static const art_name *find_art(int id)
{
    for (size_t i = 0; i < sizeof ARTS / sizeof *ARTS; i++)
        if (ARTS[i].id == id)
            return &ARTS[i];
    return NULL;
}

const char *ei_artifact_name(int id)
{
    const art_name *a = find_art(id);
    return a ? a->name : "Unknown item";
}

int ei_artifact_kind(int id)
{
    const art_name *a = find_art(id);
    return a ? a->kind : 0;
}

const char *ei_kind_name(int k)
{
    switch (k) {
    case 1: return "Stone";
    case 2: return "Ingredient";
    case 3: return "Fragment";
    default: return "Artifact";
    }
}
