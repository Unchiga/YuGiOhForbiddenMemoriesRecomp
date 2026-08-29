/* psx_drop_missing.c — see psx_drop_missing.h.
 *
 * HOW THE DROP ROLL WORKS (measured, not assumed)
 *
 * `func_80021810(a0 = tier)` picks the reward: `roll = (rng & 0x7FF) + 1`, so
 * 1..2048, then it walks 722 u16 weights accumulating until the running sum
 * reaches the roll and returns that index + 1. The weights live at
 * `0x8017878C + tier*1460`, and they are the CURRENT opponent's — the game
 * loads that duelist's record into RAM for the duel.
 *
 * **Every tier must total exactly 2048.** Under it, high rolls fall out of the
 * loop and return 0 — no drop at all. Over it, every card past the crossing
 * point becomes unreachable. So a card cannot simply be added: the weight has
 * to come out of the duelist's existing drops, which is what apply_tier() does.
 *
 * Tier indices are the game's, from `func_800218F0` at 0x80021A4C..0x80021C58:
 *   a0 = (letter < 3) ? 1 : (b57 ? 2 : 0),  b57 = 1 iff score < 50 = TECHNIQUE
 *   => tier 0 = S/A POW, tier 1 = B/C/D, tier 2 = S/A TEC.
 *
 * WHICH DUELIST IS THIS?
 *
 * The game's duelist index was never located, and guessing it would put every
 * card on the wrong opponent. Instead the resident record identifies itself:
 * the four weight arrays it leaves in RAM (the deck pool plus the three drop
 * tiers) are unique per duelist, so a fingerprint over them is the id. Drops
 * alone are NOT enough — Heishin and Heishin 2nd share a drop table byte for
 * byte, as do Villager3 and Duel Master K — which is why the deck is included.
 *
 * This fails safe. An unrecognised fingerprint means "not a duel table I know",
 * and the mod does nothing rather than corrupting a table it misread.
 *
 * WHY THIS DOES NOT DOUBLE-APPLY
 *
 * Rewriting the table changes it, so it no longer matches the stock
 * fingerprint. Only a stock table is ever transformed; a modified one matches
 * nothing and is left alone. That is the whole re-entry guard.
 */

#include "psx_drop_missing.h"
#include "psx_drop_edits.h"
#include "psx_drop_missing_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_osd.h"
#include "mod_plugins.h"
#include "psx_game_hooks.h"
#include "psx_video_menu.h"

#define INI_NAME "drop_missing_cards.ini"
#define MAX_ADDS 64

/* Runtime placement table: the built-in defaults unless the ini replaces them. */
typedef struct {
    PsxDropAdd add[MAX_ADDS];
    int        n;
} DuelistAdds;
static DuelistAdds g_adds[39];
static int  g_enabled     = 0;
static int  g_loaded      = 0;
static int  g_from_ini    = 0;
static int  g_applied     = 0;      /* duels rewritten this session */
static int  g_last_duelist = -1;
static int      g_throttle = 0;     /* frames until the next check */
static char g_status[128] = "not started";
static char g_ini_path[1024] = "";
static uint32_t g_last_fp = 0;      /* observability: last fingerprint seen */
static int  g_matched = -1;         /* duelist it resolved to, -1 = none */
static int  g_tier_ok[3] = { -1, -1, -1 };  /* per-tier apply result (mod)   */
static int  g_edit_ok[3] = { -1, -1, -1 };  /* per-tier apply result (edits) */

static uint32_t tbl_addr(int table, unsigned card_index)
{
    return PSX_DROP_RESIDENT_BASE + (uint32_t)table * PSX_DROP_TABLE_STRIDE
           + card_index * 2u;
}

/* FNV-1a over the four 1444-byte weight arrays, byte order as in RAM. */
static uint32_t resident_fingerprint(void)
{
    uint32_t h = 0x811C9DC5u;
    for (int t = 0; t < 4; t++) {
        for (unsigned i = 0; i < PSX_DROP_CARDS; i++) {
            const uint16_t v = psx_mod_read_half(tbl_addr(t, i));
            h = (h ^ (uint32_t)(v & 0xFFu)) * 0x01000193u;
            h = (h ^ (uint32_t)(v >> 8))    * 0x01000193u;
        }
    }
    return h;
}

/* Rewrite one tier so a set of pinned cards hold exactly the given weights and
 * the total is still exactly 2048.
 *
 * Existing weights are scaled by (2048 - added) / 2048. A nonzero weight is
 * never allowed to round to zero: that would silently delete one of the
 * duelist's stock drops, which is a bigger change than the one being asked
 * for. Integer division loses a few units, so the shortfall is handed back to
 * the heaviest entries afterwards and the tier lands on 2048 exactly.
 *
 * This is THE renormalizer: the mod's placements and the viewer's user edits
 * both go through it, so there is exactly one piece of arithmetic keeping the
 * 2048 invariant. A pinned weight of 0 is a removal — the card's old weight
 * is released to everyone else by the same rescale. */
int psx_drop_pins_rescale(uint16_t *w, const uint16_t *cards,
                          const uint16_t *weights, int n)
{
    /* All work happens on a copy: a failing return leaves w exactly as it
     * came, which is what every caller assumes a negative code means. */
    uint16_t t[PSX_DROP_CARDS];
    if (!w || !cards || !weights || n <= 0) return -1;
    memcpy(t, w, sizeof(t));

    uint32_t added = 0;
    for (int i = 0; i < n; i++) added += weights[i];
    /* Pins may not squeeze the rest of the tier below breathing room: every
     * surviving stock entry keeps at least weight 1, and 64 spare units is
     * the margin that guarantees the shortfall walk can land on 2048. */
    if (added > PSX_DROP_TIER_TOTAL - 64u) return -4;

    /* zero the pinned cards first, so a card the duelist already drops is
     * replaced by the pinned weight rather than added to it */
    for (int i = 0; i < n; i++)
        if (cards[i] >= 1 && cards[i] <= PSX_DROP_CARDS)
            t[cards[i] - 1] = 0;

    uint32_t old_sum = 0;
    for (unsigned i = 0; i < PSX_DROP_CARDS; i++) old_sum += t[i];
    if (!old_sum) return -2;                    /* empty table: nothing to scale */

    const uint32_t target = PSX_DROP_TIER_TOTAL - added;
    uint32_t got = 0;
    for (unsigned i = 0; i < PSX_DROP_CARDS; i++) {
        if (!t[i]) continue;
        uint32_t v = (uint32_t)t[i] * target / old_sum;
        if (!v) v = 1;
        t[i] = (uint16_t)v;
        got += v;
    }
    /* hand the rounding shortfall to the heaviest entries, one unit at a time */
    while (got < target) {
        unsigned best = 0; uint16_t bw = 0;
        for (unsigned i = 0; i < PSX_DROP_CARDS; i++)
            if (t[i] > bw) { bw = t[i]; best = i; }
        if (!bw) break;
        t[best]++; got++;
    }
    while (got > target) {
        unsigned best = 0; uint16_t bw = 0;
        for (unsigned i = 0; i < PSX_DROP_CARDS; i++)
            if (t[i] > bw) { bw = t[i]; best = i; }
        if (bw <= 1) break;
        t[best]--; got--;
    }
    for (int i = 0; i < n; i++)
        if (cards[i] >= 1 && cards[i] <= PSX_DROP_CARDS)
            t[cards[i] - 1] = weights[i];

    uint32_t total = 0;
    for (unsigned i = 0; i < PSX_DROP_CARDS; i++) total += t[i];
    if (total != PSX_DROP_TIER_TOTAL) return -3; /* a table like this is wrong */
    memcpy(w, t, sizeof(t));
    return 1;
}

/* The mod's placements for one band, as pins. Returns 1 on success, or the
 * negative codes above. w is 722 stock weights in, transformed weights out. */
int psx_drop_missing_transform(int duelist, int tier, uint16_t *w)
{
    if (duelist < 0 || duelist >= 39 || tier < 0 || tier >= 3 || !w) return -1;
    const DuelistAdds *da = &g_adds[duelist];
    uint16_t cards[MAX_ADDS], weights[MAX_ADDS];
    int n = 0;
    uint32_t added = 0;
    for (int i = 0; i < da->n; i++) {
        if (da->add[i].tier != tier) continue;
        cards[n] = da->add[i].card;
        weights[n] = da->add[i].weight;
        added += da->add[i].weight;
        n++;
    }
    if (!n || !added) return -1;    /* nothing configured for this band */
    return psx_drop_pins_rescale(w, cards, weights, n);
}

/* Read the resident tier, run the full layering — mod placements when the row
 * is on, then the player's drop-table edits — and write it back only if a
 * layer changed it. ONE writer for both layers on purpose: a second writer
 * would race this one over the same guest bytes, and the write-once guard
 * (a rewritten table no longer matches the stock fingerprint) only holds if
 * everything lands in the same write. Split from the arithmetic above so
 * nothing but this touches guest memory. edit_rc reports the edit layer's
 * result the way the transform reports its own. */
static int apply_tier(int duelist, int tier, int *edit_rc)
{
    const int table = tier + 1;        /* table 0 is the deck pool */
    uint16_t w[PSX_DROP_CARDS];
    for (unsigned i = 0; i < PSX_DROP_CARDS; i++)
        w[i] = psx_mod_read_half(tbl_addr(table, i));

    int changed = 0;
    int rc = -1;
    if (g_enabled) {
        rc = psx_drop_missing_transform(duelist, tier, w);
        if (rc == 1) changed = 1;
    }
    *edit_rc = psx_drop_edits_apply(duelist, tier, w);
    if (*edit_rc == 1) changed = 1;
    if (!changed) return rc;           /* refuse to write a bad table */

    for (unsigned i = 0; i < PSX_DROP_CARDS; i++)
        psx_mod_write_half(tbl_addr(table, i), w[i]);
    return 1;
}

/* --- the ini ------------------------------------------------------------- */

static void ini_path(char *out, size_t cap)
{
    const char *dir = psx_mod_player_data_dir();
    if (dir && dir[0]) snprintf(out, cap, "%s/%s", dir, INI_NAME);
    else               snprintf(out, cap, "%s", INI_NAME);
}

static void load_defaults(void)
{
    for (int r = 0; r < 39; r++) {
        const PsxDropDuelist *d = &PSX_DROP_DUELISTS[r];
        g_adds[r].n = 0;
        for (int i = 0; i < d->n_adds && g_adds[r].n < MAX_ADDS; i++)
            g_adds[r].add[g_adds[r].n++] = d->adds[i];
    }
}

static void write_ini(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f,
"; Yu-Gi-Oh! Forbidden Memories - Recompiled : DROP MISSING CARDS\n"
";\n"
"; 82 cards are dropped by no duelist in the stock game. This file decides\n"
"; where each one comes from. Edit it freely; delete it to get these defaults\n"
"; back on the next launch.\n"
";\n"
"; One section per duelist, one line per card:\n"
";\n"
";     <card id> = <POW>, <BCD>, <TEC>\n"
";\n"
"; POW = the S/A POW table, BCD = B/C/D (any rank), TEC = the S/A TEC table.\n"
"; Each number is a weight out of 2048, and 0 means \"not from this duelist in\n"
"; that band\". For scale: 20 = about 1%%, 41 = about 2%%, 10 = about 0.5%%.\n"
";\n"
"; Every band always totals 2048, so whatever you add here is taken off that\n"
"; duelist's normal drops in proportion. Adding 40 to a band costs its existing\n"
"; drops about 2%% of their rate - fine. Adding 400 would gut them.\n"
";\n"
"; A card id may appear under more than one duelist. Ids run 1..722.\n"
"\n");
    for (int r = 0; r < 39; r++) {
        const PsxDropDuelist *d = &PSX_DROP_DUELISTS[r];
        if (!g_adds[r].n) continue;
        fprintf(f, "[%s]\n", d->name);
        /* one line per card, merging its bands */
        uint16_t done[MAX_ADDS]; int nd = 0;
        for (int i = 0; i < g_adds[r].n; i++) {
            const uint16_t c = g_adds[r].add[i].card;
            int seen = 0;
            for (int k = 0; k < nd; k++) if (done[k] == c) { seen = 1; break; }
            if (seen) continue;
            done[nd++] = c;
            int w[3] = { 0, 0, 0 };
            for (int k = 0; k < g_adds[r].n; k++)
                if (g_adds[r].add[k].card == c && g_adds[r].add[k].tier < 3)
                    w[g_adds[r].add[k].tier] = g_adds[r].add[k].weight;
            const char *cn = "";
            for (int k = 0; k < PSX_DROP_NAME_COUNT; k++)
                if (PSX_DROP_NAMES[k].card == c) { cn = PSX_DROP_NAMES[k].name; break; }
            fprintf(f, "%-3d = %3d, %3d, %3d   ; %s\n", c, w[0], w[1], w[2], cn);
        }
        fprintf(f, "\n");
    }
    fclose(f);
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    return s;
}

static int read_ini(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    for (int r = 0; r < 39; r++) g_adds[r].n = 0;
    char line[256];
    int cur = -1, placed = 0;
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (!*s || *s == ';' || *s == '#') continue;
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (!e) continue;
            *e = 0;
            cur = -1;
            for (int r = 0; r < 39; r++)
                if (!strcmp(PSX_DROP_DUELISTS[r].name, s + 1)) { cur = r; break; }
            continue;
        }
        if (cur < 0) continue;
        int card = 0, w0 = 0, w1 = 0, w2 = 0;
        if (sscanf(s, "%d = %d , %d , %d", &card, &w0, &w1, &w2) < 2) continue;
        if (card < 1 || card > (int)PSX_DROP_CARDS) continue;
        const int ws[3] = { w0, w1, w2 };
        for (int t = 0; t < 3; t++) {
            if (ws[t] <= 0 || g_adds[cur].n >= MAX_ADDS) continue;
            PsxDropAdd *a = &g_adds[cur].add[g_adds[cur].n++];
            a->card = (uint16_t)card; a->tier = (uint8_t)t;
            a->weight = (uint16_t)(ws[t] > 1024 ? 1024 : ws[t]);
            placed++;
        }
    }
    fclose(f);
    return placed;
}

static void ensure_loaded(void)
{
    if (g_loaded) return;
    g_loaded = 1;
    load_defaults();
    char path[1024];
    ini_path(path, sizeof(path));
    snprintf(g_ini_path, sizeof(g_ini_path), "%s", path);
    FILE *probe = fopen(path, "r");
    if (probe) {
        fclose(probe);
        const int n = read_ini(path);
        if (n > 0) { g_from_ini = 1; snprintf(g_status, sizeof(g_status), "%d placements from ini", n); }
        else       { load_defaults(); snprintf(g_status, sizeof(g_status), "ini unreadable, using defaults"); }
    } else {
        write_ini(path);
        snprintf(g_status, sizeof(g_status), "wrote %s, using defaults", INI_NAME);
        host_osd_push("Drop table written: " INI_NAME, 3500);
    }
}

/* --- per frame ------------------------------------------------------------ */

/* The placements come from the ini on first use. The viewer reads them too, so
 * loading cannot stay private to the tick — a viewer opened before the first
 * duel would otherwise show the built-in defaults while the ini said
 * otherwise. */
void psx_drop_missing_ensure_loaded(void) { ensure_loaded(); }

void psx_drop_missing_tick(void)
{
    if (!psx_mod_game_started()) return;
    /* The tick also carries the viewer's drop-table edits into the game (see
     * apply_tier), so it keeps watching when the mod row is off but edits
     * exist. */
    if (!g_enabled && !psx_drop_edits_any()) return;
    ensure_loaded();

    /* Poll the whole fingerprint on a throttle rather than sampling a few
     * slots. A sparse table has only ~47 of 722 weights set, so a scattered
     * sample reads all zeros and never registers the change -- that bug made
     * this do nothing at all. 2888 halfword reads twice a second is free, and
     * the table only has to be rewritten before the duel ENDS, not on the
     * frame it loads. */
    if (--g_throttle > 0) return;
    g_throttle = 30;

    const uint32_t fp = resident_fingerprint();
    if (fp == g_last_fp) return;              /* nothing moved */
    g_last_fp = fp;
    g_matched = -1;
    for (int r = 0; r < 39; r++) {
        if (PSX_DROP_DUELISTS[r].fingerprint != fp) continue;
        g_matched = r;
        int ok = 0;
        for (int t = 0; t < 3; t++) {
            g_tier_ok[t] = apply_tier(r, t, &g_edit_ok[t]);
            if (g_tier_ok[t] == 1) ok++;
        }
        if (ok) {
            g_applied++;
            g_last_duelist = r;
            g_last_fp = resident_fingerprint();  /* our own write is not a change */
            snprintf(g_status, sizeof(g_status), "applied to %s", PSX_DROP_DUELISTS[r].name);
        } else {
            g_last_duelist = r;
        }
        return;
    }
    /* not a stock table: either already ours, or not a duel table at all */
}

int psx_drop_missing_enabled(void) { return g_enabled; }

int psx_drop_missing_state_json(char *out, unsigned cap)
{
    if (!out || cap < 160u) return 0;
    return snprintf(out, cap,
        "\"enabled\":%d,\"loaded\":%d,\"from_ini\":%d,\"applied\":%d,"
        "\"fingerprint\":\"0x%08X\",\"matched\":%d,"
        "\"tier_result\":[%d,%d,%d],\"edit_result\":[%d,%d,%d],"
        "\"last_duelist\":\"%s\",\"status\":\"%s\"",
        g_enabled, g_loaded, g_from_ini, g_applied,
        g_last_fp, g_matched, g_tier_ok[0], g_tier_ok[1], g_tier_ok[2],
        g_edit_ok[0], g_edit_ok[1], g_edit_ok[2],
        (g_last_duelist >= 0) ? PSX_DROP_DUELISTS[g_last_duelist].name : "-",
        g_status);
}

/* --- the row -------------------------------------------------------------- */

static const char *const ONOFF[] = { "Off", "On" };

/* One hint, not one per value: what the row does is the same sentence either
 * way, and the value already reads OFF/ON beside it. */
static const char *const HINT = "Adds unobtainable cards to drop tables";

static void enabled_changed(int value)
{
    g_enabled = value ? 1 : 0;
    g_last_fp = 0; g_throttle = 1;     /* re-examine the table on the next tick */
    if (psx_video_menu_is_restoring()) return;
    host_osd_push(g_enabled ? "Missing-card drops: on" : "Missing-card drops: off", 900);
}

void psx_drop_missing_register_menu(void)
{
    (void)psx_video_menu_add_option(
        PSX_VM_MENU_MODS, "Drop missing cards", HINT,
        ONOFF, 2, "drop_missing_cards", 0, enabled_changed);
}

PSX_MOD_CONSTRUCTOR(psx_drop_missing_install) {
    psx_drop_missing_register_menu();
    (void)psx_game_add_frame_hook(psx_drop_missing_tick);
}
