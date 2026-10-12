/* Console test harness for the EggXP core (not shipped as the main program). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core.h"

static int read_file(const char *path, buf_t *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    unsigned char tmp[8192];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) buf_append(out, tmp, n);
    fclose(f);
    return 0;
}

static void print_mission(const ei_mission *m)
{
    printf("%s | %s %s L%d cap=%d type=%s target=%s start=%.0f dur=%.0f drops=%d\n",
           m->id, ei_ship_name(m->ship), ei_duration_name(m->duration_type), m->level,
           m->capacity, ei_mission_type_name(m->type),
           m->target < 0 ? "-" : ei_artifact_name(m->target), m->start, m->duration, m->ndrops);
}

static void print_drops(const ei_mission *m)
{
    for (int i = 0; i < m->ndrops; i++)
        printf("    %s T%d %s [%s]\n", ei_artifact_name(m->drops[i].name), m->drops[i].level + 1,
               ei_rarity_name(m->drops[i].rarity), m->drops[i].server_id);
}

int main(int argc, char **argv)
{
    const char *eid_in = NULL, *proxy = NULL, *plain = NULL, *dump_form = NULL;
    const char *parse_fc = NULL, *parse_cm = NULL;
    int max = 3;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--proxy") && i + 1 < argc) proxy = argv[++i];
        else if (!strcmp(argv[i], "--plain") && i + 1 < argc) plain = argv[++i];
        else if (!strcmp(argv[i], "--max") && i + 1 < argc) max = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump-form") && i + 1 < argc) dump_form = argv[++i];
        else if (!strcmp(argv[i], "--parse-fc") && i + 1 < argc) parse_fc = argv[++i];
        else if (!strcmp(argv[i], "--parse-cm") && i + 1 < argc) parse_cm = argv[++i];
        else eid_in = argv[i];
    }
    char err[512];

    if (parse_fc || parse_cm) {     /* offline: parse a raw (decoded) payload file */
        buf_t b = {0};
        if (read_file(parse_fc ? parse_fc : parse_cm, &b)) { puts("read failed"); return 1; }
        if (parse_fc) {
            ei_account a = {0};
            if (ei_parse_first_contact(b.p, b.len, &a, err, sizeof err)) { printf("ERR %s\n", err); return 1; }
            printf("user=%s last_backup=%.0f missions=%d\n", a.user_name, a.last_backup, a.nmissions);
            for (int i = 0; i < a.nmissions; i++) print_mission(&a.missions[i]);
        } else {
            ei_mission m = {0};
            if (ei_parse_complete_mission(b.p, b.len, &m, err, sizeof err)) { printf("ERR %s\n", err); return 1; }
            print_drops(&m);
            printf("drops=%d\n", m.ndrops);
        }
        return 0;
    }

    if (!eid_in) {
        fprintf(stderr, "usage: eggxp_cli [--proxy h:p] [--plain h:p] [--max N] EID\n");
        return 2;
    }
    char eid[40];
    if (ei_normalize_eid(eid_in, eid, sizeof eid))
        fprintf(stderr, "warning: EID does not look like EI + digits\n");

    if (dump_form) {
        buf_t f = {0};
        if (!strcmp(dump_form, "fc")) ei_build_first_contact(&f, eid);
        else ei_build_complete_mission(&f, eid, dump_form);
        printf("%s\n", (char *)f.p);
        return 0;
    }

    net_init();
    conn_t *c;
    if (plain) {
        char host[128]; int port = 80;
        sscanf(plain, "%127[^:]:%d", host, &port);
        c = conn_new(host, port, 0);
    } else {
        c = conn_new(EI_API_HOST, 443, 1);
    }
    if (proxy) {
        char host[128]; int port = 0;
        sscanf(proxy, "%127[^:]:%d", host, &port);
        conn_set_proxy(c, host, port);
    }

    buf_t form = {0}, raw = {0};
    ei_build_first_contact(&form, eid);
    if (ei_call(c, EI_EP_FIRST_CONTACT, &form, &raw, err, sizeof err)) {
        printf("first_contact transport error: %s\n", err);
        return 1;
    }
    printf("first_contact: %lu bytes decoded\n", (unsigned long)raw.len);
    ei_account a = {0};
    if (ei_parse_first_contact(raw.p, raw.len, &a, err, sizeof err)) {
        printf("first_contact: %s\n", err);
        return 1;
    }
    printf("user=%s missions=%d\n", a.user_name, a.nmissions);
    for (int i = 0; i < a.nmissions && i < max; i++) {
        ei_mission *m = &a.missions[i];
        ei_build_complete_mission(&form, eid, m->id);
        if (ei_call(c, EI_EP_COMPLETE_MISSION, &form, &raw, err, sizeof err) ||
            ei_parse_complete_mission(raw.p, raw.len, m, err, sizeof err)) {
            printf("mission %s: %s\n", m->id, err);
            continue;
        }
        print_mission(m);
        print_drops(m);
    }
    conn_free(c);
    return 0;
}
