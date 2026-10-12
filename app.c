/*
 * EggXP - Egg, Inc. mission history fetcher for Windows XP SP3 and newer.
 * Win32 GUI front end. Network/protocol code lives in net.c / ei.c.
 */
#define _WIN32_WINNT 0x0501
#define _WIN32_IE 0x0600
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "core.h"

#define APP_TITLE L"EggXP - Egg, Inc. Mission Fetcher"
#define APP_VERSION L"1.0.1"
#define WORKERS 4
#define MAX_RETRIES 5

enum {
    IDC_EID_LABEL = 100, IDC_EID, IDC_SHOW, IDC_FETCH, IDC_STOP, IDC_EXPORT,
    IDC_PROGRESS, IDC_STATUS, IDC_ACCOUNT, IDC_MISSIONS, IDC_DROPS_LABEL,
    IDC_RADIO_SEL, IDC_RADIO_TOTAL, IDC_DROPS,
};

enum {
    WM_APP_ACCOUNT = WM_APP + 1,   /* lParam = ei_account*, wParam = missions loaded from cache */
    WM_APP_MISSION,                /* wParam = mission index finished (ok or failed) */
    WM_APP_STATUS,                 /* lParam = malloc'd wchar_t* */
    WM_APP_DONE,                   /* wParam = 0 ok / 1 stopped / 2 error, lParam = malloc'd wchar_t* */
};

#define TIMER_TOTALS 1

typedef struct {
    int name, level, rarity, kind, count;
} agg_t;

static HINSTANCE g_inst;
static HWND g_wnd, g_eid, g_show, g_fetch, g_stopbtn, g_export, g_progress, g_status,
            g_account, g_missions, g_dropslbl, g_rsel, g_rtot, g_drops, g_eidlbl;
static HFONT g_font;

static ei_account *g_acct;
static int *g_order;
static int g_norder;
static int g_sort_col = 0, g_sort_asc = 1;
static agg_t *g_agg;
static int g_nagg, g_agg_cap;
static int g_drop_sort_col = 4, g_drop_sort_asc = 0;
static int g_show_totals;
static int g_done, g_failed;
static volatile LONG g_fetching, g_stop;
static wchar_t g_cache_root[MAX_PATH];

/* ======================================================================== */
/* helpers                                                                  */
/* ======================================================================== */
static wchar_t *wdup_printf(const wchar_t *fmt, ...)
{
    wchar_t tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(tmp, 1023, fmt, ap);
    va_end(ap);
    tmp[1023] = 0;
    size_t n = wcslen(tmp) + 1;
    wchar_t *p = (wchar_t *)malloc(n * sizeof(wchar_t));
    if (p) memcpy(p, tmp, n * sizeof(wchar_t));
    return p;
}

static void utf8_to_w(const char *s, wchar_t *out, int outlen)
{
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, out, outlen))
        out[0] = 0;
    out[outlen - 1] = 0;
}

static void post_status(const wchar_t *fmt, const char *arg)
{
    wchar_t warg[600];
    utf8_to_w(arg ? arg : "", warg, 600);
    PostMessageW(g_wnd, WM_APP_STATUS, 0, (LPARAM)wdup_printf(fmt, warg));
}

static void fmt_time(double epoch, wchar_t *out, int outlen)
{
    if (epoch <= 0) { _snwprintf(out, outlen, L"-"); out[outlen - 1] = 0; return; }
    ULONGLONG t = (ULONGLONG)(epoch * 10000000.0) + 116444736000000000ULL;
    FILETIME ft, lft;
    SYSTEMTIME st;
    ft.dwLowDateTime = (DWORD)t;
    ft.dwHighDateTime = (DWORD)(t >> 32);
    FileTimeToLocalFileTime(&ft, &lft);
    FileTimeToSystemTime(&lft, &st);
    _snwprintf(out, outlen, L"%04u-%02u-%02u %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    out[outlen - 1] = 0;
}

static void fmt_time_a(double epoch, char *out, size_t outlen)
{
    wchar_t w[64];
    fmt_time(epoch, w, 64);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)outlen, NULL, NULL);
}

/* ======================================================================== */
/* disk cache: one raw complete_mission payload per mission                 */
/* ======================================================================== */
static int dir_writable(const wchar_t *dir)
{
    if (!CreateDirectoryW(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return 0;
    wchar_t probe[MAX_PATH];
    _snwprintf(probe, MAX_PATH, L"%s\\.probe", dir);
    probe[MAX_PATH - 1] = 0;
    HANDLE h = CreateFileW(probe, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    CloseHandle(h);
    return 1;
}

static void init_cache_root(void)
{
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (n && n < MAX_PATH) {
        wchar_t *slash = wcsrchr(exe, L'\\');
        if (slash) *slash = 0;
        _snwprintf(g_cache_root, MAX_PATH, L"%s\\EggXP-cache", exe);
        g_cache_root[MAX_PATH - 1] = 0;
        if (dir_writable(g_cache_root))
            return;
    }
    wchar_t appdata[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA | CSIDL_FLAG_CREATE, NULL, 0, appdata))) {
        _snwprintf(g_cache_root, MAX_PATH, L"%s\\EggXP", appdata);
        g_cache_root[MAX_PATH - 1] = 0;
        CreateDirectoryW(g_cache_root, NULL);
        wcsncat(g_cache_root, L"\\cache", MAX_PATH - wcslen(g_cache_root) - 1);
        if (dir_writable(g_cache_root))
            return;
    }
    g_cache_root[0] = 0;   /* no cache */
}

static void cache_path(const char *eid, const char *mission_id, wchar_t *out)
{
    char safe[80];
    size_t i;
    for (i = 0; mission_id[i] && i < sizeof safe - 1; i++) {
        char c = mission_id[i];
        safe[i] = ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') ? c : '_';
    }
    safe[i] = 0;
    _snwprintf(out, MAX_PATH, L"%s\\%hs\\%hs.pb", g_cache_root, eid, safe);
    out[MAX_PATH - 1] = 0;
}

static int cache_load(const char *eid, ei_mission *m)
{
    if (!g_cache_root[0]) return -1;
    wchar_t path[MAX_PATH];
    cache_path(eid, m->id, path);
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    DWORD size = GetFileSize(h, NULL), got = 0;
    int rc = -1;
    if (size != INVALID_FILE_SIZE && size > 0 && size < 16 * 1024 * 1024) {
        unsigned char *p = (unsigned char *)malloc(size);
        if (p && ReadFile(h, p, size, &got, NULL) && got == size) {
            char err[256];
            rc = ei_parse_complete_mission(p, size, m, err, sizeof err);
        }
        free(p);
    }
    CloseHandle(h);
    return rc;
}

static void cache_store(const char *eid, const char *mission_id, const buf_t *raw)
{
    if (!g_cache_root[0]) return;
    wchar_t dir[MAX_PATH], path[MAX_PATH], tmp[MAX_PATH];
    _snwprintf(dir, MAX_PATH, L"%s\\%hs", g_cache_root, eid);
    dir[MAX_PATH - 1] = 0;
    CreateDirectoryW(dir, NULL);
    cache_path(eid, mission_id, path);
    _snwprintf(tmp, MAX_PATH, L"%s.tmp%lu", path, GetCurrentThreadId());
    tmp[MAX_PATH - 1] = 0;
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    DWORD w = 0;
    BOOL ok = WriteFile(h, raw->p, (DWORD)raw->len, &w, NULL) && w == raw->len;
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING))
        DeleteFileW(tmp);
}

/* ======================================================================== */
/* fetch thread + workers                                                   */
/* ======================================================================== */
typedef struct {
    ei_account *acct;
    char eid[40];
    int *todo;
    int ntodo;
    volatile LONG next;
} fetch_ctx;

static conn_t *new_api_conn(void)
{
#ifdef EGGXP_TEST
    /* test builds only: EGGXP_TEST_PLAIN=host:port talks plain HTTP to a mock server */
    char v[128];
    DWORD n = GetEnvironmentVariableA("EGGXP_TEST_PLAIN", v, sizeof v);
    if (n && n < sizeof v) {
        char host[100]; int port = 80;
        if (sscanf(v, "%99[^:]:%d", host, &port) >= 1)
            return conn_new(host, port, 0);
    }
#endif
    return conn_new(EI_API_HOST, 443, 1);
}

static int sleep_unless_stopped(DWORD ms)
{
    while (ms) {
        DWORD step = ms > 100 ? 100 : ms;
        Sleep(step);
        ms -= step;
        if (g_stop) return 1;
    }
    return 0;
}

static unsigned __stdcall worker_proc(void *arg)
{
    fetch_ctx *fc = (fetch_ctx *)arg;
    conn_t *c = new_api_conn();
    buf_t form = {0}, raw = {0};
    if (!c) return 0;
    for (;;) {
        if (g_stop) break;
        LONG k = InterlockedIncrement(&fc->next) - 1;
        if (k >= fc->ntodo) break;
        ei_mission *m = &fc->acct->missions[fc->todo[k]];
        char err[sizeof m->error];
        int ok = 0;
        DWORD delay = 500;
        for (int attempt = 0; attempt <= MAX_RETRIES && !g_stop; attempt++) {
            if (ei_build_complete_mission(&form, fc->eid, m->id) == 0 &&
                ei_call(c, EI_EP_COMPLETE_MISSION, &form, &raw, err, sizeof err) == 0 &&
                ei_parse_complete_mission(raw.p, raw.len, m, err, sizeof err) == 0) {
                ok = 1;
                break;
            }
            conn_close(c);
            if (attempt < MAX_RETRIES && sleep_unless_stopped(delay)) break;
            delay = delay * 2 > 4000 ? 4000 : delay * 2;
        }
        if (ok) {
            cache_store(fc->eid, m->id, &raw);
        } else {
            if (g_stop) snprintf(err, sizeof err, "stopped");
            memcpy(m->error, err, sizeof m->error);
            m->error[sizeof m->error - 1] = 0;
        }
        MemoryBarrier();
        InterlockedExchange((volatile LONG *)&m->state, ok ? 1 : -1);
        PostMessageW(g_wnd, WM_APP_MISSION, (WPARAM)fc->todo[k], 0);
    }
    buf_free(&form);
    buf_free(&raw);
    conn_free(c);
    return 0;
}

static unsigned __stdcall fetch_proc(void *arg)
{
    fetch_ctx *fc = (fetch_ctx *)arg;
    char err[512] = "";
    buf_t form = {0}, raw = {0};
    ei_account *acct = (ei_account *)calloc(1, sizeof *acct);
    int result = 2;
    wchar_t *msg = NULL;

    snprintf(acct->eid, sizeof acct->eid, "%s", fc->eid);
    post_status(L"Connecting to Egg, Inc. server...%s", NULL);

    conn_t *c = new_api_conn();
    int ok = 0;
    if (!c) snprintf(err, sizeof err, "out of memory");
    for (int attempt = 0; attempt < 3 && !g_stop && c; attempt++) {
        if (ei_build_first_contact(&form, fc->eid) == 0 &&
            ei_call(c, EI_EP_FIRST_CONTACT, &form, &raw, err, sizeof err) == 0) {
            ok = 1;
            break;
        }
        conn_close(c);
        if (sleep_unless_stopped(1000)) break;
    }
    conn_free(c);
    if (!ok) {
        wchar_t w[600];
        utf8_to_w(g_stop ? "stopped" : err, w, 600);
        msg = wdup_printf(L"Could not download backup: %s", w);
        goto fail;
    }
    if (ei_parse_first_contact(raw.p, raw.len, acct, err, sizeof err)) {
        wchar_t w[600];
        utf8_to_w(err, w, 600);
        msg = wdup_printf(L"%s", w);
        goto fail;
    }

    /* load cached missions, queue the rest */
    post_status(L"Backup received, checking local cache...%s", NULL);
    fc->acct = acct;
    fc->todo = (int *)malloc(sizeof(int) * (acct->nmissions + 1));
    fc->ntodo = 0;
    int cached = 0;
    for (int i = 0; i < acct->nmissions; i++) {
        if (cache_load(fc->eid, &acct->missions[i]) == 0) {
            acct->missions[i].state = 1;
            cached++;
        } else {
            fc->todo[fc->ntodo++] = i;
        }
    }
    /* newest first, so recent missions show up quickly */
    for (int a = 0, b = fc->ntodo - 1; a < b; a++, b--) {
        int t = fc->todo[a]; fc->todo[a] = fc->todo[b]; fc->todo[b] = t;
    }
    PostMessageW(g_wnd, WM_APP_ACCOUNT, (WPARAM)cached, (LPARAM)acct);
    acct = NULL;   /* GUI owns it now */

    if (fc->ntodo) {
        post_status(L"Downloading mission loot...%s", NULL);
        HANDLE th[WORKERS];
        int nth = 0;
        for (int i = 0; i < WORKERS && i < fc->ntodo; i++) {
            th[nth] = (HANDLE)_beginthreadex(NULL, 0, worker_proc, fc, 0, NULL);
            if (th[nth]) nth++;
        }
        WaitForMultipleObjects((DWORD)nth, th, TRUE, INFINITE);
        for (int i = 0; i < nth; i++) CloseHandle(th[i]);
    }
    result = g_stop ? 1 : 0;
    goto out;

fail:
    if (acct) { ei_account_free(acct); free(acct); }
out:
    buf_free(&form);
    buf_free(&raw);
    free(fc->todo);
    free(fc);
    PostMessageW(g_wnd, WM_APP_DONE, (WPARAM)result, (LPARAM)msg);
    return 0;
}

/* ======================================================================== */
/* list views                                                               */
/* ======================================================================== */
static void add_column(HWND lv, int idx, const wchar_t *title, int width, int right)
{
    LVCOLUMNW col;
    memset(&col, 0, sizeof col);
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    col.fmt = right ? LVCFMT_RIGHT : LVCFMT_LEFT;
    col.cx = width;
    col.pszText = (wchar_t *)title;
    SendMessageW(lv, LVM_INSERTCOLUMNW, idx, (LPARAM)&col);
}

static const ei_mission *row_mission(int row)
{
    if (!g_acct || row < 0 || row >= g_norder) return NULL;
    return &g_acct->missions[g_order[row]];
}

static int mission_state(const ei_mission *m)
{
    return (int)InterlockedCompareExchange((volatile LONG *)&m->state, 0, 0);
}

static int cmp_dbl(double a, double b) { return a < b ? -1 : a > b ? 1 : 0; }

static int cmp_missions(const void *x, const void *y)
{
    const ei_mission *a = &g_acct->missions[*(const int *)x];
    const ei_mission *b = &g_acct->missions[*(const int *)y];
    int r = 0;
    switch (g_sort_col) {
    case 0: case 1: r = cmp_dbl(a->start, b->start); break;
    case 2: r = cmp_dbl(a->start + a->duration, b->start + b->duration); break;
    case 3: r = a->ship - b->ship; break;
    case 4: r = a->duration_type - b->duration_type; break;
    case 5: r = a->level - b->level; break;
    case 6: r = a->capacity - b->capacity; break;
    case 7: r = strcmp(a->target < 0 ? "" : ei_artifact_name(a->target), b->target < 0 ? "" : ei_artifact_name(b->target)); break;
    case 8: r = a->type - b->type; break;
    case 9: r = (mission_state(a) == 1 ? a->ndrops : -1) - (mission_state(b) == 1 ? b->ndrops : -1); break;
    case 10: r = mission_state(a) - mission_state(b); break;
    }
    if (!r) r = cmp_dbl(a->start, b->start);
    return g_sort_asc ? r : -r;
}

static int selected_mission_index(void)
{
    int row = (int)SendMessageW(g_missions, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    return (g_acct && row >= 0 && row < g_norder) ? g_order[row] : -1;
}

static void sort_missions(void)
{
    if (!g_acct) return;
    int keep = selected_mission_index();
    qsort(g_order, (size_t)g_norder, sizeof(int), cmp_missions);
    if (keep >= 0) {
        for (int r = 0; r < g_norder; r++) {
            if (g_order[r] == keep) {
                LVITEMW it;
                memset(&it, 0, sizeof it);
                it.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
                SendMessageW(g_missions, LVM_SETITEMSTATE, (WPARAM)-1, (LPARAM)&it);
                it.state = LVIS_SELECTED | LVIS_FOCUSED;
                SendMessageW(g_missions, LVM_SETITEMSTATE, (WPARAM)r, (LPARAM)&it);
                SendMessageW(g_missions, LVM_ENSUREVISIBLE, (WPARAM)r, FALSE);
                break;
            }
        }
    }
    InvalidateRect(g_missions, NULL, FALSE);
}

static void agg_add(const ei_drop *d)
{
    for (int i = 0; i < g_nagg; i++) {
        if (g_agg[i].name == d->name && g_agg[i].level == d->level && g_agg[i].rarity == d->rarity) {
            g_agg[i].count++;
            return;
        }
    }
    if (g_nagg == g_agg_cap) {
        int nc = g_agg_cap ? g_agg_cap * 2 : 64;
        agg_t *nv = (agg_t *)realloc(g_agg, (size_t)nc * sizeof *nv);
        if (!nv) return;
        g_agg = nv;
        g_agg_cap = nc;
    }
    agg_t a = { d->name, d->level, d->rarity, ei_artifact_kind(d->name), 1 };
    g_agg[g_nagg++] = a;
}

static int cmp_agg(const void *x, const void *y)
{
    const agg_t *a = (const agg_t *)x, *b = (const agg_t *)y;
    int r = 0;
    switch (g_drop_sort_col) {
    case 0: r = strcmp(ei_artifact_name(a->name), ei_artifact_name(b->name)); break;
    case 1: r = a->kind - b->kind; break;
    case 2: r = a->level - b->level; break;
    case 3: r = a->rarity - b->rarity; break;
    case 4: r = a->count - b->count; break;
    }
    if (!r) r = strcmp(ei_artifact_name(a->name), ei_artifact_name(b->name));
    if (!r) r = a->level - b->level;
    if (!r) r = a->rarity - b->rarity;
    return g_drop_sort_asc ? r : -r;
}

static void rebuild_drops(void)
{
    g_nagg = 0;
    wchar_t label[300];
    if (!g_acct) {
        wcscpy(label, L"Drops:");
    } else if (g_show_totals) {
        int nm = 0;
        long total = 0;
        for (int i = 0; i < g_acct->nmissions; i++) {
            const ei_mission *m = &g_acct->missions[i];
            if (mission_state(m) != 1) continue;
            nm++;
            total += m->ndrops;
            for (int k = 0; k < m->ndrops; k++) agg_add(&m->drops[k]);
        }
        _snwprintf(label, 300, L"Lifetime totals: %ld items from %d missions", total, nm);
    } else {
        int idx = selected_mission_index();
        if (idx < 0) {
            wcscpy(label, L"Select a mission to see its drops");
        } else {
            const ei_mission *m = &g_acct->missions[idx];
            int st = mission_state(m);
            wchar_t when[64], wid[80];
            fmt_time(m->start, when, 64);
            utf8_to_w(m->id, wid, 80);
            if (st == 1) {
                for (int k = 0; k < m->ndrops; k++) agg_add(&m->drops[k]);
                _snwprintf(label, 300, L"%hs %hs launched %s: %d items", ei_ship_name(m->ship),
                           ei_duration_name(m->duration_type), when, m->ndrops);
            } else if (st == -1) {
                wchar_t werr[200];
                utf8_to_w(m->error, werr, 200);
                _snwprintf(label, 300, L"Mission %s failed: %s", wid, werr);
            } else {
                _snwprintf(label, 300, L"Mission %s: not downloaded yet", wid);
            }
        }
    }
    label[299] = 0;
    if (g_nagg > 1) qsort(g_agg, (size_t)g_nagg, sizeof *g_agg, cmp_agg);
    SetWindowTextW(g_dropslbl, label);
    SendMessageW(g_drops, LVM_SETITEMCOUNT, (WPARAM)g_nagg, LVSICF_NOSCROLL);
    InvalidateRect(g_drops, NULL, FALSE);
}

static void mission_text(const ei_mission *m, int row, int col, wchar_t *out, int n)
{
    int st = mission_state(m);
    switch (col) {
    case 0: _snwprintf(out, n, L"%d", row + 1); break;
    case 1: fmt_time(m->start, out, n); break;
    case 2: fmt_time(m->start + m->duration, out, n); break;
    case 3: _snwprintf(out, n, L"%hs", ei_ship_name(m->ship)); break;
    case 4: _snwprintf(out, n, L"%hs", ei_duration_name(m->duration_type)); break;
    case 5: _snwprintf(out, n, L"%d", m->level); break;
    case 6: _snwprintf(out, n, L"%d", m->capacity); break;
    case 7: _snwprintf(out, n, L"%hs", m->target < 0 ? "" : ei_artifact_name(m->target)); break;
    case 8: _snwprintf(out, n, L"%hs", ei_mission_type_name(m->type)); break;
    case 9: if (st == 1) _snwprintf(out, n, L"%d", m->ndrops); else out[0] = 0; break;
    case 10: _snwprintf(out, n, L"%s", st == 1 ? L"OK" : st == -1 ? L"Failed" : L"Pending"); break;
    default: out[0] = 0;
    }
    out[n - 1] = 0;
}

static void drop_text(const agg_t *a, int col, wchar_t *out, int n)
{
    switch (col) {
    case 0: _snwprintf(out, n, L"%hs", ei_artifact_name(a->name)); break;
    case 1: _snwprintf(out, n, L"%hs", ei_kind_name(a->kind)); break;
    case 2: _snwprintf(out, n, L"T%d", a->level + 1); break;
    case 3: _snwprintf(out, n, L"%hs", ei_rarity_name(a->rarity)); break;
    case 4: _snwprintf(out, n, L"%d", a->count); break;
    default: out[0] = 0;
    }
    out[n - 1] = 0;
}

/* ======================================================================== */
/* CSV export                                                               */
/* ======================================================================== */
static void csv_field(buf_t *b, const char *s, int last)
{
    int quote = strpbrk(s, ",\"\r\n") != NULL;
    if (quote) buf_append(b, "\"", 1);
    for (; *s; s++) {
        if (*s == '"') buf_append(b, "\"", 1);
        buf_append(b, s, 1);
    }
    if (quote) buf_append(b, "\"", 1);
    buf_append(b, last ? "\r\n" : ",", last ? 2 : 1);
}

static void export_csv(void)
{
    if (!g_acct) return;
    wchar_t file[MAX_PATH], wname[200];
    utf8_to_w(g_acct->user_name[0] ? g_acct->user_name : "missions", wname, 200);
    for (wchar_t *p = wname; *p; p++)
        if (wcschr(L"\\/:*?\"<>|", *p)) *p = L'_';
    _snwprintf(file, MAX_PATH, L"EggXP-%s.csv", wname);
    file[MAX_PATH - 1] = 0;

    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_wnd;
    ofn.lpstrFilter = L"CSV files (*.csv)\0*.csv\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"csv";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn))
        return;

    buf_t b = {0};
    buf_append(&b, "\xEF\xBB\xBF", 3);   /* UTF-8 BOM for Excel */
    const char *hdr[] = { "Mission ID", "Launched", "Returned", "Ship", "Duration", "Level",
                          "Capacity", "Target", "Mission Type", "Item", "Kind", "Tier",
                          "Rarity", "Server ID" };
    for (int i = 0; i < 14; i++) csv_field(&b, hdr[i], i == 13);

    long rows = 0;
    for (int r = 0; r < g_acct->nmissions; r++) {
        const ei_mission *m = &g_acct->missions[r];
        char launched[64], returned[64], lvl[16], cap[16], tier[16];
        fmt_time_a(m->start, launched, sizeof launched);
        fmt_time_a(m->start + m->duration, returned, sizeof returned);
        snprintf(lvl, sizeof lvl, "%d", m->level);
        snprintf(cap, sizeof cap, "%d", m->capacity);
        int st = mission_state(m);
        int n = st == 1 ? m->ndrops : 1;
        for (int k = 0; k < n; k++) {
            csv_field(&b, m->id, 0);
            csv_field(&b, launched, 0);
            csv_field(&b, returned, 0);
            csv_field(&b, ei_ship_name(m->ship), 0);
            csv_field(&b, ei_duration_name(m->duration_type), 0);
            csv_field(&b, lvl, 0);
            csv_field(&b, cap, 0);
            csv_field(&b, m->target < 0 ? "" : ei_artifact_name(m->target), 0);
            csv_field(&b, ei_mission_type_name(m->type), 0);
            if (st == 1 && m->ndrops > 0) {
                const ei_drop *d = &m->drops[k];
                snprintf(tier, sizeof tier, "T%d", d->level + 1);
                csv_field(&b, ei_artifact_name(d->name), 0);
                csv_field(&b, ei_kind_name(ei_artifact_kind(d->name)), 0);
                csv_field(&b, tier, 0);
                csv_field(&b, ei_rarity_name(d->rarity), 0);
                csv_field(&b, d->server_id, 1);
            } else {
                csv_field(&b, st == -1 ? "(download failed)" : st == 0 ? "(not downloaded)" : "(no drops)", 0);
                csv_field(&b, "", 0); csv_field(&b, "", 0); csv_field(&b, "", 0); csv_field(&b, "", 1);
            }
            rows++;
        }
    }

    HANDLE h = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD w = 0;
    BOOL ok = h != INVALID_HANDLE_VALUE && WriteFile(h, b.p, (DWORD)b.len, &w, NULL) && w == b.len;
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    buf_free(&b);
    wchar_t msg[MAX_PATH + 100];
    if (ok) _snwprintf(msg, MAX_PATH + 100, L"Exported %ld rows to %s", rows, file);
    else _snwprintf(msg, MAX_PATH + 100, L"Could not write %s", file);
    msg[MAX_PATH + 99] = 0;
    SetWindowTextW(g_status, msg);
}

/* ======================================================================== */
/* UI                                                                       */
/* ======================================================================== */
static void set_fetching(int on)
{
    InterlockedExchange(&g_fetching, on);
    EnableWindow(g_fetch, !on);
    EnableWindow(g_eid, !on);
    EnableWindow(g_stopbtn, on);
    EnableWindow(g_export, !on && g_acct != NULL);
}

static void update_progress(void)
{
    int total = g_acct ? g_acct->nmissions : 0;
    SendMessageW(g_progress, PBM_SETRANGE32, 0, total ? total : 1);
    SendMessageW(g_progress, PBM_SETPOS, (WPARAM)g_done, 0);
}

static void save_eid(const char *eid)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\EggXP", 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(k, "LastEID", 0, REG_SZ, (const BYTE *)eid, (DWORD)strlen(eid) + 1);
        RegCloseKey(k);
    }
}

static void load_eid(void)
{
    HKEY k;
    char eid[64];
    DWORD len = sizeof eid - 1, type = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\EggXP", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExA(k, "LastEID", NULL, &type, (BYTE *)eid, &len) == ERROR_SUCCESS && type == REG_SZ) {
            eid[len < sizeof eid ? len : sizeof eid - 1] = 0;
            SetWindowTextA(g_eid, eid);
        }
        RegCloseKey(k);
    }
}

static void start_fetch(void)
{
    if (g_fetching) return;
    char raw[128], eid[40];
    GetWindowTextA(g_eid, raw, sizeof raw);
    if (ei_normalize_eid(raw, eid, sizeof eid)) {
        MessageBoxW(g_wnd, L"Enter your Egg, Inc. ID (EID). It starts with \"EI\" followed by digits.\n\n"
                           L"You can find it in the game's settings menu.",
                    APP_TITLE, MB_ICONINFORMATION);
        SetFocus(g_eid);
        return;
    }
    SetWindowTextA(g_eid, eid);
    save_eid(eid);

    /* drop the previous result (no workers are running at this point) */
    SendMessageW(g_missions, LVM_SETITEMCOUNT, 0, 0);
    if (g_acct) { ei_account_free(g_acct); free(g_acct); g_acct = NULL; }
    free(g_order); g_order = NULL; g_norder = 0;
    g_done = g_failed = 0;
    SetWindowTextW(g_account, L"");
    rebuild_drops();
    update_progress();

    fetch_ctx *fc = (fetch_ctx *)calloc(1, sizeof *fc);
    if (!fc) return;
    snprintf(fc->eid, sizeof fc->eid, "%s", eid);
    InterlockedExchange(&g_stop, 0);
    set_fetching(1);
    HANDLE th = (HANDLE)_beginthreadex(NULL, 0, fetch_proc, fc, 0, NULL);
    if (!th) {
        free(fc);
        set_fetching(0);
        SetWindowTextW(g_status, L"Could not start the download thread.");
        return;
    }
    CloseHandle(th);
}

static void layout(int w, int h)
{
    const int M = 8, RH = 23;
    int x = M, y = M;
    MoveWindow(g_eidlbl, x, y + 4, 30, RH - 4, TRUE); x += 32;
    int ew = w - x - M - (64 + 84 + 74 + 104 + 4 * 6);
    if (ew < 160) ew = 160;
    if (ew > 260) ew = 260;
    MoveWindow(g_eid, x, y, ew, RH, TRUE); x += ew + 6;
    MoveWindow(g_show, x, y + 2, 64, RH - 4, TRUE); x += 64 + 6;
    MoveWindow(g_fetch, x, y, 84, RH, TRUE); x += 84 + 6;
    MoveWindow(g_stopbtn, x, y, 74, RH, TRUE); x += 74 + 6;
    MoveWindow(g_export, x, y, 104, RH, TRUE);

    y += RH + 8;
    MoveWindow(g_progress, M, y, 260, 18, TRUE);
    MoveWindow(g_status, M + 270, y + 2, w - M - 270 - M, 18, TRUE);
    y += 24;
    MoveWindow(g_account, M, y, w - 2 * M, 18, TRUE);
    y += 22;

    int avail = h - y - M - 26;
    if (avail < 100) avail = 100;
    int top = avail * 58 / 100;
    MoveWindow(g_missions, M, y, w - 2 * M, top, TRUE);
    y += top + 6;
    MoveWindow(g_rsel, M, y, 120, 20, TRUE);
    MoveWindow(g_rtot, M + 124, y, 120, 20, TRUE);
    MoveWindow(g_dropslbl, M + 252, y + 3, w - M - 252 - M, 18, TRUE);
    y += 24;
    MoveWindow(g_drops, M, y, w - 2 * M, h - y - M, TRUE);
}

static HWND mk(const wchar_t *cls, const wchar_t *text, DWORD style, int id)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
                             g_wnd, (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, FALSE);
    return c;
}

static HWND mk_list(int id)
{
    HWND lv = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA |
                              LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                              0, 0, 10, 10, g_wnd, (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessageW(lv, WM_SETFONT, (WPARAM)g_font, FALSE);
    SendMessageW(lv, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                 LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP);
    return lv;
}

static void create_controls(void)
{
    NONCLIENTMETRICSW ncm;
    memset(&ncm, 0, sizeof ncm);
    /* built with WINVER 0x0501, so this is the XP-sized struct that every Windows accepts */
    ncm.cbSize = sizeof ncm;
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, ncm.cbSize, &ncm, 0))
        g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    if (!g_font)
        g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    g_eidlbl = mk(L"STATIC", L"EID:", 0, IDC_EID_LABEL);
    g_eid = mk(L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | ES_PASSWORD, IDC_EID);
    SendMessageW(g_eid, EM_LIMITTEXT, 60, 0);
    g_show = mk(L"BUTTON", L"Show", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_SHOW);
    g_fetch = mk(L"BUTTON", L"&Fetch", WS_TABSTOP | BS_DEFPUSHBUTTON, IDC_FETCH);
    g_stopbtn = mk(L"BUTTON", L"&Stop", WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED, IDC_STOP);
    g_export = mk(L"BUTTON", L"&Export CSV...", WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED, IDC_EXPORT);
    g_progress = mk(PROGRESS_CLASSW, L"", PBS_SMOOTH, IDC_PROGRESS);
    g_status = mk(L"STATIC", L"Enter your EID and click Fetch.", SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, IDC_STATUS);
    g_account = mk(L"STATIC", L"", SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, IDC_ACCOUNT);

    g_missions = mk_list(IDC_MISSIONS);
    add_column(g_missions, 0, L"#", 48, 1);
    add_column(g_missions, 1, L"Launched", 112, 0);
    add_column(g_missions, 2, L"Returned", 112, 0);
    add_column(g_missions, 3, L"Ship", 130, 0);
    add_column(g_missions, 4, L"Duration", 64, 0);
    add_column(g_missions, 5, L"Lvl", 34, 1);
    add_column(g_missions, 6, L"Cap", 40, 1);
    add_column(g_missions, 7, L"Target", 120, 0);
    add_column(g_missions, 8, L"Type", 50, 0);
    add_column(g_missions, 9, L"Drops", 48, 1);
    add_column(g_missions, 10, L"Status", 56, 0);

    g_rsel = mk(L"BUTTON", L"Selected mission", WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON, IDC_RADIO_SEL);
    g_rtot = mk(L"BUTTON", L"Lifetime totals", BS_AUTORADIOBUTTON, IDC_RADIO_TOTAL);
    SendMessageW(g_rsel, BM_SETCHECK, BST_CHECKED, 0);
    g_dropslbl = mk(L"STATIC", L"Drops:", SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, IDC_DROPS_LABEL);

    g_drops = mk_list(IDC_DROPS);
    add_column(g_drops, 0, L"Item", 200, 0);
    add_column(g_drops, 1, L"Kind", 80, 0);
    add_column(g_drops, 2, L"Tier", 44, 0);
    add_column(g_drops, 3, L"Rarity", 80, 0);
    add_column(g_drops, 4, L"Count", 60, 1);

    load_eid();
}

static LRESULT on_notify(NMHDR *nh)
{
    if (nh->idFrom == IDC_MISSIONS || nh->idFrom == IDC_DROPS) {
        int is_m = nh->idFrom == IDC_MISSIONS;
        if (nh->code == LVN_GETDISPINFOW) {
            NMLVDISPINFOW *di = (NMLVDISPINFOW *)nh;
            if (di->item.mask & LVIF_TEXT) {
                di->item.pszText[0] = 0;
                if (is_m) {
                    const ei_mission *m = row_mission(di->item.iItem);
                    if (m) mission_text(m, di->item.iItem, di->item.iSubItem, di->item.pszText, di->item.cchTextMax);
                } else if (di->item.iItem >= 0 && di->item.iItem < g_nagg) {
                    drop_text(&g_agg[di->item.iItem], di->item.iSubItem, di->item.pszText, di->item.cchTextMax);
                }
            }
            return 0;
        }
        if (nh->code == LVN_COLUMNCLICK) {
            NMLISTVIEW *lv = (NMLISTVIEW *)nh;
            if (is_m) {
                if (g_sort_col == lv->iSubItem) g_sort_asc = !g_sort_asc;
                else { g_sort_col = lv->iSubItem; g_sort_asc = 1; }
                sort_missions();
            } else {
                if (g_drop_sort_col == lv->iSubItem) g_drop_sort_asc = !g_drop_sort_asc;
                else { g_drop_sort_col = lv->iSubItem; g_drop_sort_asc = lv->iSubItem != 4; }
                rebuild_drops();
            }
            return 0;
        }
        if (is_m && nh->code == LVN_ITEMCHANGED) {
            NMLISTVIEW *lv = (NMLISTVIEW *)nh;
            if ((lv->uChanged & LVIF_STATE) && ((lv->uNewState ^ lv->uOldState) & LVIS_SELECTED) && !g_show_totals)
                rebuild_drops();
            return 0;
        }
    }
    return 0;
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_wnd = h;
        create_controls();
        return 0;
    case WM_SIZE:
        layout(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = 640;
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = 420;
        return 0;
    case WM_SETFOCUS:
        SetFocus(g_eid);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_FETCH: start_fetch(); break;
        case IDC_STOP:
            InterlockedExchange(&g_stop, 1);
            EnableWindow(g_stopbtn, FALSE);
            SetWindowTextW(g_status, L"Stopping after the requests in flight finish...");
            break;
        case IDC_EXPORT: export_csv(); break;
        case IDC_SHOW:
            SendMessageW(g_eid, EM_SETPASSWORDCHAR,
                         SendMessageW(g_show, BM_GETCHECK, 0, 0) == BST_CHECKED ? 0 : 0x25CF, 0);
            InvalidateRect(g_eid, NULL, TRUE);
            break;
        case IDC_RADIO_SEL:
        case IDC_RADIO_TOTAL:
            g_show_totals = SendMessageW(g_rtot, BM_GETCHECK, 0, 0) == BST_CHECKED;
            rebuild_drops();
            break;
        }
        return 0;
    case WM_NOTIFY:
        return on_notify((NMHDR *)lp);
    case WM_TIMER:
        if (wp == TIMER_TOTALS) {
            KillTimer(h, TIMER_TOTALS);
            if (g_show_totals) rebuild_drops();
        }
        return 0;
    case WM_APP_STATUS: {
        wchar_t *s = (wchar_t *)lp;
        if (s) { SetWindowTextW(g_status, s); free(s); }
        return 0;
    }
    case WM_APP_ACCOUNT: {
        g_acct = (ei_account *)lp;
        g_done = (int)wp;
        g_failed = 0;
        g_norder = g_acct->nmissions;
        g_order = (int *)malloc(sizeof(int) * (g_norder + 1));
        for (int i = 0; i < g_norder; i++) g_order[i] = i;
        g_sort_col = 1; g_sort_asc = 0;   /* newest first */
        qsort(g_order, (size_t)g_norder, sizeof(int), cmp_missions);
        SendMessageW(g_missions, LVM_SETITEMCOUNT, (WPARAM)g_norder, 0);
        wchar_t name[160], line[300], last[64];
        utf8_to_w(g_acct->user_name[0] ? g_acct->user_name : "(no name)", name, 160);
        fmt_time(g_acct->last_backup, last, 64);
        _snwprintf(line, 300, L"Account: %s    Completed missions: %d    Last game backup: %s",
                   name, g_acct->nmissions, last);
        line[299] = 0;
        SetWindowTextW(g_account, line);
        update_progress();
        rebuild_drops();
        return 0;
    }
    case WM_APP_MISSION: {
        g_done++;
        if (g_acct && (int)wp < g_acct->nmissions && mission_state(&g_acct->missions[wp]) == -1)
            g_failed++;
        update_progress();
        wchar_t s[200];
        _snwprintf(s, 200, L"Downloading mission loot... %d / %d%s", g_done, g_acct ? g_acct->nmissions : 0,
                   g_failed ? L" (some failed)" : L"");
        s[199] = 0;
        if (!g_stop) SetWindowTextW(g_status, s);
        InvalidateRect(g_missions, NULL, FALSE);
        if (g_show_totals) SetTimer(h, TIMER_TOTALS, 400, NULL);
        else if (selected_mission_index() == (int)wp) rebuild_drops();
        return 0;
    }
    case WM_APP_DONE: {
        wchar_t *err = (wchar_t *)lp;
        set_fetching(0);
        if (wp == 2) {
            SetWindowTextW(g_status, err ? err : L"Fetch failed.");
            if (err) MessageBoxW(h, err, APP_TITLE, MB_ICONWARNING);
        } else if (g_acct) {
            long items = 0;
            int ok = 0, failed = 0;
            for (int i = 0; i < g_acct->nmissions; i++) {
                int st = mission_state(&g_acct->missions[i]);
                if (st == 1) { ok++; items += g_acct->missions[i].ndrops; }
                else failed++;
            }
            wchar_t s[300];
            if (failed)
                _snwprintf(s, 300, L"%s %d missions, %ld items. %d not downloaded - click Fetch again to retry them.",
                           wp == 1 ? L"Stopped." : L"Done with errors.", ok, items, failed);
            else
                _snwprintf(s, 300, L"Done. %d missions, %ld items.", ok, items);
            s[299] = 0;
            SetWindowTextW(g_status, s);
            InvalidateRect(g_missions, NULL, FALSE);
            rebuild_drops();
        }
        free(err);
        set_fetching(0);
        return 0;
    }
    case WM_CLOSE:
        if (g_fetching) InterlockedExchange(&g_stop, 1);
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show)
{
    (void)prev; (void)cmd;
    g_inst = inst;
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    if (net_init()) {
        MessageBoxW(NULL, L"Winsock could not be initialized.", APP_TITLE, MB_ICONERROR);
        return 1;
    }
    init_cache_root();

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    if (!wc.hIcon) wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"EggXPMain";
    RegisterClassExW(&wc);

    HWND w = CreateWindowExW(0, L"EggXPMain", APP_TITLE L" " APP_VERSION, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 940, 640, NULL, NULL, inst, NULL);
    if (!w) return 1;
    ShowWindow(w, show);
    UpdateWindow(w);

#ifdef EGGXP_TEST
    {
        char v[64];
        if (GetEnvironmentVariableA("EGGXP_TEST_EID", v, sizeof v)) {
            SetWindowTextA(g_eid, v);
            start_fetch();
        }
    }
#endif
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (m.message == WM_KEYDOWN && m.wParam == VK_RETURN && m.hwnd == g_eid) {
            start_fetch();
            continue;
        }
        if (IsDialogMessageW(w, &m))
            continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
