/* psx_card_share.c -- see psx_card_share.h.
 *
 * The container is a plain zip so a player can look inside it, but this
 * file neither depends on a zip library nor trusts the archive: names are
 * checked against the exact shapes we write (cards/<id>/<one of four
 * files>, manifest.ini, drop_table_edits.ini), sizes are bounded, and
 * nothing outside the player's cards/ folder is ever written. */

#include "psx_textfile.h"
#include "psx_card_share.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

#include "mod_plugins.h"
#include "psx_card_packs.h"
#include "psx_drop_edits.h"

/* zlib, which the runtime links for its save states, inflates hand-made
 * (deflated) archives. stb_image's decoder was tried first and rejects the
 * tiny fixed-Huffman streams a zip tool emits for a 30-byte card.ini. */
#include <zlib.h>

#define CARD_COUNT 722
#define ENTRY_MAX  (8u * 1024u * 1024u)     /* one file inside the archive */
#define ARCHIVE_MAX (256u * 1024u * 1024u)

/* ---- crc32 -------------------------------------------------------------- */
static uint32_t s_crc_tab[256];
static void crc_init(void)
{
    if (s_crc_tab[1]) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        s_crc_tab[i] = c;
    }
}
static uint32_t crc32_buf(const uint8_t *p, size_t n)
{
    crc_init();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = s_crc_tab[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ---- small file helpers ---------------------------------------------------- */
static unsigned char *read_file(const char *path, long *size)
{
    FILE *f = psx_fopen_utf8(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || (unsigned long)n > ARCHIVE_MAX) { fclose(f); return NULL; }
    unsigned char *b = (unsigned char *)malloc((size_t)n + 1);
    if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f);
    b[n] = 0;
    *size = n;
    return b;
}

static int write_file(const char *path, const void *data, size_t n)
{
    FILE *f = psx_fopen_utf8(path, "wb");
    if (!f) return 0;
    const int ok = fwrite(data, 1, n, f) == n;
    fclose(f);
    return ok;
}

static void cards_dir(char *out, size_t cap)
{
    const char *d = psx_card_packs_dir();
    if (d && d[0]) snprintf(out, cap, "%s", d);
    else snprintf(out, cap, "%s/cards", psx_mod_player_data_dir());
}

static void drops_path(char *out, size_t cap)
{
    snprintf(out, cap, "%s/drop_table_edits.ini", psx_mod_player_data_dir());
}

static const char *const CARD_FILES[4] = { "card.ini", "art.png", "thumb.png", "title.png" };

/* ---- zip writer (stored) --------------------------------------------------- */
typedef struct { char name[64]; uint32_t crc, size, offset; } Central;

typedef struct {
    FILE *f;
    Central *ents;
    int n, cap;
    uint16_t dos_time, dos_date;
} Zip;

static void put16(FILE *f, unsigned v) { fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f); }
static void put32(FILE *f, uint32_t v) { put16(f, v & 0xFFFFu); put16(f, (v >> 16) & 0xFFFFu); }

static int zip_add(Zip *z, const char *name, const void *data, size_t n)
{
    if (z->n == z->cap) {
        const int nc = z->cap ? z->cap * 2 : 64;
        Central *e = (Central *)realloc(z->ents, (size_t)nc * sizeof *e);
        if (!e) return 0;
        z->ents = e; z->cap = nc;
    }
    Central *c = &z->ents[z->n++];
    snprintf(c->name, sizeof c->name, "%s", name);
    c->crc = crc32_buf((const uint8_t *)data, n);
    c->size = (uint32_t)n;
    c->offset = (uint32_t)ftell(z->f);
    const unsigned nl = (unsigned)strlen(c->name);
    put32(z->f, 0x04034b50u); put16(z->f, 20); put16(z->f, 0); put16(z->f, 0);
    put16(z->f, z->dos_time); put16(z->f, z->dos_date);
    put32(z->f, c->crc); put32(z->f, c->size); put32(z->f, c->size);
    put16(z->f, nl); put16(z->f, 0);
    fwrite(c->name, 1, nl, z->f);
    return fwrite(data, 1, n, z->f) == n;
}

static int zip_finish(Zip *z)
{
    const uint32_t cd = (uint32_t)ftell(z->f);
    for (int i = 0; i < z->n; i++) {
        const Central *c = &z->ents[i];
        const unsigned nl = (unsigned)strlen(c->name);
        put32(z->f, 0x02014b50u); put16(z->f, 20); put16(z->f, 20); put16(z->f, 0); put16(z->f, 0);
        put16(z->f, z->dos_time); put16(z->f, z->dos_date);
        put32(z->f, c->crc); put32(z->f, c->size); put32(z->f, c->size);
        put16(z->f, nl); put16(z->f, 0); put16(z->f, 0); put16(z->f, 0); put16(z->f, 0);
        put32(z->f, 0); put32(z->f, c->offset);
        fwrite(c->name, 1, nl, z->f);
    }
    const uint32_t cd_size = (uint32_t)ftell(z->f) - cd;
    put32(z->f, 0x06054b50u); put16(z->f, 0); put16(z->f, 0);
    put16(z->f, (unsigned)z->n); put16(z->f, (unsigned)z->n);
    put32(z->f, cd_size); put32(z->f, cd); put16(z->f, 0);
    const int ok = !ferror(z->f);
    fclose(z->f);
    free(z->ents);
    return ok;
}

int psx_card_share_export(const char *path, char *msg, unsigned cap)
{
    char dir[1024]; cards_dir(dir, sizeof dir);
    Zip z; memset(&z, 0, sizeof z);
    z.f = psx_fopen_utf8(path, "wb");
    if (!z.f) { if (msg) snprintf(msg, cap, "Could not create %s", path); return 0; }
    {
        const time_t t = time(NULL);
        const struct tm *tm = localtime(&t);
        z.dos_time = (uint16_t)((tm->tm_hour << 11) | (tm->tm_min << 5) | (tm->tm_sec / 2));
        z.dos_date = (uint16_t)(((tm->tm_year - 80) << 9) | ((tm->tm_mon + 1) << 5) | tm->tm_mday);
    }
    /* manifest first: what the file is, which cards, whether drops ride along */
    int ids[CARD_COUNT], n = 0;
    for (int id = 1; id <= CARD_COUNT; id++) if (psx_card_packs_get(id, NULL)) ids[n++] = id;
    int drops = 0;
    if (psx_drop_edits_any()) { psx_drop_edits_save(); drops = 1; }
    {
        static char m[8192];
        unsigned k = (unsigned)snprintf(m, sizeof m,
            "; Yu-Gi-Oh! Forbidden Memories Recompiled -- edited cards\n"
            "format = %s\nversion = %d\ngame = SLUS-01411\ncards = ", PSX_CARD_SHARE_FORMAT, PSX_CARD_SHARE_VERSION);
        for (int i = 0; i < n && k + 8 < sizeof m; i++) k += (unsigned)snprintf(m + k, sizeof m - k, "%s%d", i ? ", " : "", ids[i]);
        k += (unsigned)snprintf(m + k, sizeof m - k, "\ndrop_table_edits = %d\n", drops);
        if (!zip_add(&z, "manifest.ini", m, k)) goto fail;
    }
    int files = 0;
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < 4; j++) {
            char p[1200]; snprintf(p, sizeof p, "%s/%d/%s", dir, ids[i], CARD_FILES[j]);
            long sz; unsigned char *b = read_file(p, &sz);
            if (!b) continue;
            char name[64]; snprintf(name, sizeof name, "cards/%d/%s", ids[i], CARD_FILES[j]);
            const int ok = zip_add(&z, name, b, (size_t)sz);
            free(b);
            if (!ok) goto fail;
            files++;
        }
    }
    if (drops) {
        char p[1200]; drops_path(p, sizeof p);
        long sz; unsigned char *b = read_file(p, &sz);
        if (b) { const int ok = zip_add(&z, "drop_table_edits.ini", b, (size_t)sz); free(b); if (!ok) goto fail; }
    }
    if (!zip_finish(&z)) { if (msg) snprintf(msg, cap, "Writing %s failed", path); return 0; }
    if (msg) snprintf(msg, cap, "Exported %d edited card%s (%d file%s)%s", n, n == 1 ? "" : "s", files, files == 1 ? "" : "s", drops ? " and the drop table edits" : "");
    return 1;
fail:
    fclose(z.f); free(z.ents);
    if (msg) snprintf(msg, cap, "Writing %s failed", path);
    return 0;
}

/* ---- zip reader ------------------------------------------------------------- */
typedef struct {
    char name[64];
    uint32_t method, csize, usize, offset, crc;
} Entry;

static uint32_t rd16(const unsigned char *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const unsigned char *p) { return rd16(p) | (rd16(p + 2) << 16); }

/* Walk the central directory; cb gets every entry. Returns the entry count or -1. */
static int zip_entries(const unsigned char *b, long n, Entry *out, int max, char *err, unsigned errcap)
{
    long eocd = -1;
    for (long i = n - 22; i >= 0 && i >= n - 22 - 65536; i--) if (rd32(b + i) == 0x06054b50u) { eocd = i; break; }
    if (eocd < 0) { snprintf(err, errcap, "not a .ygocards file (no zip directory)"); return -1; }
    const uint32_t count = rd16(b + eocd + 10);
    uint32_t off = rd32(b + eocd + 16);
    int k = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (off + 46 > (uint32_t)n || rd32(b + off) != 0x02014b50u) { snprintf(err, errcap, "damaged zip directory"); return -1; }
        Entry e; memset(&e, 0, sizeof e);
        e.method = rd16(b + off + 10);
        e.crc = rd32(b + off + 16);
        e.csize = rd32(b + off + 20);
        e.usize = rd32(b + off + 24);
        const uint32_t nl = rd16(b + off + 28), xl = rd16(b + off + 30), cl = rd16(b + off + 32);
        e.offset = rd32(b + off + 42);
        if (off + 46 + nl > (uint32_t)n) { snprintf(err, errcap, "damaged zip directory"); return -1; }
        snprintf(e.name, sizeof e.name, "%.*s", (int)(nl < 63 ? nl : 63), (const char *)b + off + 46);
        off += 46 + nl + xl + cl;
        if (k < max) out[k++] = e;
    }
    return k;
}

/* Extract one entry into a malloc'd buffer (NUL-terminated). */
static unsigned char *zip_extract(const unsigned char *b, long n, const Entry *e, long *size)
{
    if (e->offset + 30 > (uint32_t)n || rd32(b + e->offset) != 0x04034b50u) return NULL;
    const uint32_t nl = rd16(b + e->offset + 26), xl = rd16(b + e->offset + 28);
    const uint32_t data = e->offset + 30 + nl + xl;
    if (data + e->csize > (uint32_t)n || e->usize > ENTRY_MAX) return NULL;
    unsigned char *out = NULL;
    if (e->method == 0) {
        if (e->csize != e->usize) return NULL;
        out = (unsigned char *)malloc(e->usize + 1);
        if (!out) return NULL;
        memcpy(out, b + data, e->usize);
    } else if (e->method == 8) {
        out = (unsigned char *)malloc(e->usize + 1);
        if (!out) return NULL;
        z_stream zs; memset(&zs, 0, sizeof zs);
        if (inflateInit2(&zs, -15) != Z_OK) { free(out); return NULL; }   /* raw deflate, no zlib header */
        zs.next_in = (Bytef *)(b + data); zs.avail_in = e->csize;
        zs.next_out = out; zs.avail_out = e->usize;
        const int r = inflate(&zs, Z_FINISH);
        const int good = (r == Z_STREAM_END || (r == Z_BUF_ERROR && zs.avail_in == 0)) && zs.total_out == e->usize;
        inflateEnd(&zs);
        if (!good) { free(out); return NULL; }
    } else return NULL;
    out[e->usize] = 0;
    if (crc32_buf(out, e->usize) != e->crc) { free(out); return NULL; }
    *size = (long)e->usize;
    return out;
}

/* "cards/<id>/<file>" -> id and file index, or 0 */
static int parse_card_name(const char *name, int *id, int *file)
{
    if (strncmp(name, "cards/", 6) != 0) return 0;
    const char *p = name + 6;
    int v = 0, digits = 0;
    while (*p >= '0' && *p <= '9' && digits < 4) { v = v * 10 + (*p - '0'); p++; digits++; }
    if (!digits || *p != '/' || v < 1 || v > CARD_COUNT) return 0;
    p++;
    for (int j = 0; j < 4; j++) if (!strcmp(p, CARD_FILES[j])) { *id = v; *file = j; return 1; }
    return 0;
}

static void parse_manifest(const unsigned char *m, PsxCardShareInfo *info)
{
    const char *p = (const char *)m;
    while (*p) {
        const char *e = strchr(p, '\n');
        const size_t len = e ? (size_t)(e - p) : strlen(p);
        char line[256]; snprintf(line, sizeof line, "%.*s", (int)(len < 255 ? len : 255), p);
        if (!strncmp(line, "version", 7)) { const char *eq = strchr(line, '='); if (eq) info->version = atoi(eq + 1); }
        else if (!strncmp(line, "title", 5)) { const char *eq = strchr(line, '='); if (eq) { eq++; while (*eq == ' ') eq++; snprintf(info->title, sizeof info->title, "%s", eq); } }
        if (!e) break;
        p = e + 1;
    }
}

int psx_card_share_inspect(const char *path, PsxCardShareInfo *info)
{
    memset(info, 0, sizeof *info);
    long n; unsigned char *b = read_file(path, &n);
    if (!b) { snprintf(info->error, sizeof info->error, "Could not read the file"); return 0; }
    info->bytes = n;
    static Entry ents[4096];
    const int k = zip_entries(b, n, ents, 4096, info->error, sizeof info->error);
    if (k < 0) { free(b); return 0; }
    static unsigned char have[CARD_COUNT + 1];
    memset(have, 0, sizeof have);
    int manifest = 0;
    for (int i = 0; i < k; i++) {
        int id, file;
        if (!strcmp(ents[i].name, "manifest.ini")) {
            manifest = 1;
            long sz; unsigned char *m = zip_extract(b, n, &ents[i], &sz);
            if (m) { parse_manifest(m, info); free(m); }
        } else if (!strcmp(ents[i].name, "drop_table_edits.ini")) info->has_drops = 1;
        else if (parse_card_name(ents[i].name, &id, &file)) have[id] = 1;
    }
    if (!manifest) { snprintf(info->error, sizeof info->error, "not a .ygocards file (no manifest)"); free(b); return 0; }
    if (info->version > PSX_CARD_SHARE_VERSION) { snprintf(info->error, sizeof info->error, "made by a newer version (%d); update the game", info->version); free(b); return 0; }
    for (int id = 1; id <= CARD_COUNT; id++) {
        if (!have[id]) continue;
        info->card_ids[info->card_n++] = id;
        if (psx_card_packs_get(id, NULL)) info->replace_ids[info->replace_n++] = id;
    }
    info->drops_here = psx_drop_edits_any();
    info->ok = 1;
    free(b);
    return 1;
}

int psx_card_share_import(const char *path, char *msg, unsigned cap)
{
    PsxCardShareInfo info;
    if (!psx_card_share_inspect(path, &info)) { if (msg) snprintf(msg, cap, "%s", info.error); return 0; }
    long n; unsigned char *b = read_file(path, &n);
    if (!b) { if (msg) snprintf(msg, cap, "Could not read the file"); return 0; }
    static Entry ents[4096];
    char err[160];
    const int k = zip_entries(b, n, ents, 4096, err, sizeof err);
    if (k < 0) { free(b); if (msg) snprintf(msg, cap, "%s", err); return 0; }
    char dir[1024]; cards_dir(dir, sizeof dir);
    MKDIR(dir);
    /* the file's cards replace the player's: clear those folders first */
    for (int i = 0; i < info.card_n; i++) {
        char p[1200];
        for (int j = 0; j < 4; j++) { snprintf(p, sizeof p, "%s/%d/%s", dir, info.card_ids[i], CARD_FILES[j]); remove(p); }
        snprintf(p, sizeof p, "%s/%d", dir, info.card_ids[i]);
        MKDIR(p);
    }
    int files = 0, bad = 0;
    for (int i = 0; i < k; i++) {
        int id, file;
        if (parse_card_name(ents[i].name, &id, &file)) {
            long sz; unsigned char *d = zip_extract(b, n, &ents[i], &sz);
            if (!d) { bad++; continue; }
            char p[1200]; snprintf(p, sizeof p, "%s/%d/%s", dir, id, CARD_FILES[file]);
            if (write_file(p, d, (size_t)sz)) files++; else bad++;
            free(d);
        } else if (!strcmp(ents[i].name, "drop_table_edits.ini")) {
            long sz; unsigned char *d = zip_extract(b, n, &ents[i], &sz);
            if (!d) { bad++; continue; }
            char p[1200]; snprintf(p, sizeof p, "%s/drop_table_edits.import.ini", psx_mod_player_data_dir());
            if (write_file(p, d, (size_t)sz)) {
                if (psx_drop_edits_load_file(p) >= 0) psx_drop_edits_save();
                remove(p);
            } else bad++;
            free(d);
        }
    }
    free(b);
    psx_card_packs_reload(0);
    if (msg) snprintf(msg, cap, "Imported %d card%s (%d file%s)%s%s", info.card_n, info.card_n == 1 ? "" : "s", files, files == 1 ? "" : "s",
                      info.has_drops ? " and the drop table edits" : "", bad ? "; some entries were damaged and skipped" : "");
    return bad == 0;
}
