/* psx_drop_edits.c — see psx_drop_edits.h.
 *
 * THE INI
 * -------
 * drop_table_edits.ini, beside drop_missing_cards.ini in the player-data
 * folder, in the same shape the player already knows from that file:
 *
 *     [Duelist Name]
 *     <card id> = <POW>, <BCD>, <TEC>
 *
 * with one deliberate difference: here a line is the card's COMPLETE weight
 * vector for that duelist — a 0 really means "not in this band", not "leave
 * that band alone". That is what makes band moves and removals expressible,
 * and it is why the viewer records the untouched bands' current values into
 * the entry when the player edits one band.
 *
 * Duelist names come from PSX_DROP_DB (the baked drop database), which is
 * also where the viewer gets them, so section lookup is self-consistent.
 */

#include "psx_drop_edits.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#include "mod_plugins.h"
#include "psx_card_db.h"        /* the randomizer draws monsters, so it needs each card's type */
#include "psx_cpu_data.h"
#include "psx_drop_db.h"
#include "psx_drop_missing.h"
#include "psx_textfile.h"      /* psx_fopen_utf8(): the player folder may have an accent (Windows) */

#define INI_NAME  "drop_table_edits.ini"
#define NDUEL     PSX_DROP_DB_DUELISTS
#define NCARDS    PSX_DROP_DB_CARDS
/* Per duelist. One entry per card at most, and Randomize spells out whole
 * tables (a stock duelist touches up to 161 distinct cards across the three
 * bands, and the random ones on top), so the cap is simply every card. */
#define MAX_EDITS NCARDS

typedef struct { uint16_t card; uint16_t w[3]; } Edit;
static Edit     g_edit[NDUEL][MAX_EDITS];
static int      g_n[NDUEL];
/* Exact replacement bands let Clear expose a truthful empty authoring canvas
 * without ever feeding card 0 to the guest's reward path. */
static uint16_t g_replace[NDUEL][3][NCARDS];
static uint8_t  g_replace_mask[NDUEL];
/* The scripted first-win card per duelist, and whether it repeats. Same file,
 * same Save, same Import / Export as the weights above. */
static uint16_t g_reward[NDUEL];
static uint8_t  g_reward_every[NDUEL];
static uint8_t  g_smart_drop;
static uint8_t  g_smart_drop_present;
static PsxStarchipRule g_starchip[PSX_STARCHIP_RULE_MAX];
static int      g_starchip_n;
static int      g_loaded;
static int      g_dirty;
static unsigned g_gen = 1;
static char     g_ini_path[1024] = "";
static char     g_status[96] = "not loaded";

#define DROP_EDIT_FORMAT 3

static void reset_data(void)
{
    memset(g_edit, 0, sizeof g_edit);
    memset(g_n, 0, sizeof g_n);
    memset(g_reward, 0, sizeof g_reward);
    memset(g_reward_every, 0, sizeof g_reward_every);
    g_smart_drop = 0;
    g_smart_drop_present = 0;
    memset(g_starchip, 0, sizeof g_starchip);
    g_starchip_n = 0;
    memset(g_replace, 0, sizeof g_replace);
    memset(g_replace_mask, 0, sizeof g_replace_mask);
}

static void ini_path(char *out, size_t cap)
{
    const char *dir = psx_mod_player_data_dir();
    if (dir && dir[0]) snprintf(out, cap, "%s/%s", dir, INI_NAME);
    else               snprintf(out, cap, "%s", INI_NAME);
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '
                     || e[-1] == '\t')) *--e = 0;
    return s;
}

static char *key_value(char *line, const char *key)
{
    const size_t n = strlen(key);
    if (strncmp(line, key, n) ||
        (line[n] != ' ' && line[n] != '\t' && line[n] != '=')) return NULL;
    char *eq = strchr(line + n, '=');
    return eq ? trim(eq + 1) : NULL;
}

static int starts_key(const char *line, const char *key)
{
    const size_t n = strlen(key);
    return !strncmp(line, key, n) &&
           (line[n] == ' ' || line[n] == '\t' || line[n] == '=');
}

static int starchip_rule_valid(const PsxStarchipRule *r)
{
    return r && r->mode >= -1 && r->mode <= PSX_STARCHIP_MODE_FREE_DUEL &&
           r->opponent >= -1 && r->opponent < NDUEL &&
           r->outcome >= -1 && r->outcome <= PSX_STARCHIP_OUTCOME_WIN &&
           r->rank >= -1 && r->rank < 10 && r->amount <= 999999u;
}

static int parse_starchip_choice(const char *value,
                                 const char *const *names, int count,
                                 int *out)
{
    if (!strcmp(value, "any")) { *out = -1; return 1; }
    for (int i = 0; i < count; i++) {
        if (!strcmp(value, names[i])) { *out = i; return 1; }
    }
    return 0;
}

static int parse_starchip_line(char *line, PsxStarchipRule *rule,
                               char *err, unsigned errcap, int line_no)
{
    static const char *const modes[] = { "campaign", "free_duel" };
    static const char *const outcomes[] = { "loss", "win" };
    static const char *const ranks[] = {
        "D-TEC", "C-TEC", "B-TEC", "A-TEC", "S-TEC",
        "D-POW", "C-POW", "B-POW", "A-POW", "S-POW"
    };
    char *v = key_value(line, "mode");
    if (!v && starts_key(line, "mode")) goto bad;
    if (v) {
        if (!parse_starchip_choice(v, modes, 2, &rule->mode)) goto bad;
        return 1;
    }
    v = key_value(line, "opponent");
    if (!v && starts_key(line, "opponent")) goto bad;
    if (v) {
        if (!strcmp(v, "any")) rule->opponent = -1;
        else {
            char *end = NULL;
            const long n = strtol(v, &end, 10);
            if (end == v || *trim(end) || n < 1 || n > NDUEL) goto bad;
            rule->opponent = (int)n - 1;
        }
        return 1;
    }
    v = key_value(line, "outcome");
    if (!v && starts_key(line, "outcome")) goto bad;
    if (v) {
        if (!parse_starchip_choice(v, outcomes, 2, &rule->outcome)) goto bad;
        return 1;
    }
    v = key_value(line, "rank");
    if (!v && starts_key(line, "rank")) goto bad;
    if (v) {
        if (!parse_starchip_choice(v, ranks, 10, &rule->rank)) goto bad;
        return 1;
    }
    v = key_value(line, "amount");
    if (!v && starts_key(line, "amount")) goto bad;
    if (v) {
        char *end = NULL;
        const unsigned long n = strtoul(v, &end, 10);
        if (end == v || *trim(end) || n > 999999ul) goto bad;
        rule->amount = (uint32_t)n;
        return 1;
    }
    return 0; /* unknown additive key */

bad:
    if (err && errcap)
        snprintf(err, errcap, "invalid starchip rule on line %d", line_no);
    return -1;
}

static int parse_replacement(char *value, uint16_t out[NCARDS],
                             char *err, unsigned errcap)
{
    memset(out, 0, NCARDS * sizeof(*out));
    char *p = trim(value);
    uint32_t total = 0;
    int entries = 0;
    if (!*p || !strcmp(p, "none")) {
        if (err && errcap) snprintf(err, errcap,
            "an empty band is pending only; add a card before importing");
        return 0;
    }
    while (*p) {
        char *end = NULL;
        const long card = strtol(p, &end, 10);
        if (end == p || card < 1 || card > NCARDS || *end != ':') {
            if (err && errcap) snprintf(err, errcap,
                "replacement entries must be card:weight pairs");
            return 0;
        }
        p = end + 1;
        const long weight = strtol(p, &end, 10);
        if (end == p || weight < 1 || weight > PSX_DROP_DB_TOTAL || out[card - 1]) {
            if (err && errcap) snprintf(err, errcap,
                "replacement IDs must be unique and weights 1 to 2048");
            return 0;
        }
        out[card - 1] = (uint16_t)weight;
        total += (uint32_t)weight;
        entries++;
        p = end;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p != ',') {
            if (err && errcap) snprintf(err, errcap,
                "replacement entries must be comma-separated");
            return 0;
        }
        p++;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) {
            if (err && errcap) snprintf(err, errcap,
                "replacement lists cannot end with a comma");
            return 0;
        }
    }
    if (!entries || total != PSX_DROP_DB_TOTAL) {
        if (err && errcap) snprintf(err, errcap,
            "a replacement band must total exactly 2048 (got %u)", total);
        return 0;
    }
    return 1;
}

static int validate_loaded_edits(char *err, unsigned errcap)
{
    static uint16_t w[NCARDS], cards[MAX_EDITS], weights[MAX_EDITS];
    for (int d = 0; d < NDUEL; d++) {
        if (!g_n[d]) continue;
        for (int t = 0; t < 3; t++) {
            if (g_replace_mask[d] & (1u << t)) continue;
            memset(w, 0, sizeof w);
            const PsxDropDbDuelist *db = &PSX_DROP_DB[d];
            for (int i = 0; i < db->count[t]; i++)
                w[db->tier[t][i].card - 1] = db->tier[t][i].weight;
            for (int i = 0; i < g_n[d]; i++) {
                cards[i] = g_edit[d][i].card;
                weights[i] = g_edit[d][i].w[t];
            }
            const int rc = psx_drop_pins_rescale(w, cards, weights, g_n[d]);
            if (rc == 1) continue;
            if (err && errcap) snprintf(err, errcap,
                "%s %s cannot form a valid 2048-weight table",
                psx_cpu_display_name(d), PSX_DROP_TIER_NAMES[t]);
            return 0;
        }
    }
    return 1;
}

static int read_ini(const char *path, char *err, unsigned errcap)
{
    FILE *f = psx_fopen_utf8(path, "r");
    if (!f) return -1;
    reset_data();
    char line[16384];
    int cur = -1, chip_cur = -1, entries = 0, format = 1, line_no = 0;
    while (fgets(line, sizeof(line), f)) {
        line_no++;
        if (!strchr(line, '\n') && !feof(f)) {
            if (err && errcap) snprintf(err, errcap, "line %d is too long", line_no);
            fclose(f);
            return -2;
        }
        char *s = trim(line);
        if (!*s || *s == ';' || *s == '#') continue;
        if (cur < 0 && chip_cur < 0 && !strncmp(s, "format", 6) &&
            (s[6] == ' ' || s[6] == '\t' || s[6] == '=')) {
            int parsed = 0;
            char trailing = 0;
            if (sscanf(s, "format = %d %c", &parsed, &trailing) != 1) {
                if (err && errcap) snprintf(err, errcap,
                    "malformed format declaration on line %d", line_no);
                fclose(f);
                return -2;
            }
            format = parsed;
            if (format < 1 || format > DROP_EDIT_FORMAT) {
                if (err && errcap) snprintf(err, errcap,
                    "unsupported drop-table format %d", format);
                fclose(f);
                return -2;
            }
            continue;
        }
        if (cur < 0 && chip_cur < 0 && starts_key(s, "smart_drop")) {
            char *v = key_value(s, "smart_drop");
            if (!v || (strcmp(v, "on") && strcmp(v, "off") &&
                       strcmp(v, "1") && strcmp(v, "0"))) {
                if (err && errcap) snprintf(err, errcap,
                    "smart_drop must be on or off on line %d", line_no);
                fclose(f);
                return -2;
            }
            g_smart_drop = (uint8_t)(!strcmp(v, "on") || !strcmp(v, "1"));
            g_smart_drop_present = 1;
            continue;
        }
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (!e) {
                if (err && errcap) snprintf(err, errcap,
                    "unterminated section header on line %d", line_no);
                fclose(f);
                return -2;
            }
            *e = 0;
            char *tail = trim(e + 1);
            if (*tail && *tail != ';' && *tail != '#') {
                if (err && errcap) snprintf(err, errcap,
                    "trailing text after section header on line %d", line_no);
                fclose(f);
                return -2;
            }
            cur = -1;
            chip_cur = -1;
            if (!strncmp(s + 1, "Starchip Reward ", 16)) {
                char *end = NULL;
                const long label = strtol(s + 17, &end, 10);
                if (end == s + 17 || *trim(end) || label < 1 ||
                    g_starchip_n >= PSX_STARCHIP_RULE_MAX) {
                    if (err && errcap) snprintf(err, errcap,
                        "invalid starchip section on line %d", line_no);
                    fclose(f);
                    return -2;
                }
                chip_cur = g_starchip_n++;
                g_starchip[chip_cur].mode = -1;
                g_starchip[chip_cur].opponent = -1;
                g_starchip[chip_cur].outcome = -1;
                g_starchip[chip_cur].rank = -1;
                g_starchip[chip_cur].amount = UINT32_MAX;
                continue;
            }
            for (int d = 0; d < NDUEL; d++)
                if (!strcmp(PSX_DROP_DB[d].name, s + 1)) { cur = d; break; }
            continue;
        }
        if (chip_cur >= 0) {
            const int rc = parse_starchip_line(
                s, &g_starchip[chip_cur], err, errcap, line_no);
            if (rc < 0) { fclose(f); return -2; }
            continue;
        }
        if (cur < 0) continue;
        {
            static const char *const key[3] = {
                "pow_table", "bcd_table", "tec_table"
            };
            int handled = 0;
            for (int t = 0; t < 3; t++) {
                const size_t kn = strlen(key[t]);
                if (strncmp(s, key[t], kn) ||
                    (s[kn] != ' ' && s[kn] != '\t' && s[kn] != '=')) continue;
                char *eq = strchr(s + kn, '=');
                if (!eq || !parse_replacement(eq + 1, g_replace[cur][t], err, errcap)) {
                    if (err && errcap && !err[0]) snprintf(err, errcap,
                        "invalid %s on line %d", key[t], line_no);
                    fclose(f);
                    return -2;
                }
                g_replace_mask[cur] |= (uint8_t)(1u << t);
                handled = 1;
                break;
            }
            if (handled) continue;
        }
        {   /* the section's scripted reward, if it has one */
            char *v = key_value(s, "card");
            if (!v && starts_key(s, "card")) {
                if (err && errcap) snprintf(err, errcap,
                    "malformed scripted card on line %d", line_no);
                fclose(f);
                return -2;
            }
            if (v) {
                char *end = NULL;
                const long card = strtol(v, &end, 10);
                if (end == v || *trim(end) || card < 1 || card > NCARDS) {
                    if (err && errcap) snprintf(err, errcap,
                        "scripted card must be 1 through %d on line %d",
                        NCARDS, line_no);
                    fclose(f);
                    return -2;
                }
                g_reward[cur] = (uint16_t)card;
                continue;
            }
            v = key_value(s, "when");
            if (!v && starts_key(s, "when")) {
                if (err && errcap) snprintf(err, errcap,
                    "malformed scripted reward mode on line %d", line_no);
                fclose(f);
                return -2;
            }
            if (v) {
                if (strcmp(v, "first") && strcmp(v, "every")) {
                    if (err && errcap) snprintf(err, errcap,
                        "scripted reward mode must be first or every on line %d",
                        line_no);
                    fclose(f);
                    return -2;
                }
                g_reward_every[cur] = (uint8_t)!strcmp(v, "every");
                continue;
            }
        }
        int card = 0, w0 = 0, w1 = 0, w2 = 0;
        /* Fewer than four numbers is a malformed line, not a partial edit —
         * an entry is always the full vector (see the header comment). */
        if (sscanf(s, "%d = %d , %d , %d", &card, &w0, &w1, &w2) != 4) {
            /* Keep unknown additive keys forward-tolerant, but never turn a
             * visibly numeric, malformed weight row into a successful no-op. */
            if (s[0] >= '0' && s[0] <= '9') {
                if (err && errcap) snprintf(err, errcap,
                    "malformed weight entry on line %d", line_no);
                fclose(f);
                return -2;
            }
            continue;
        }
        if (card < 1 || card > NCARDS || w0 < 0 || w1 < 0 || w2 < 0 ||
            w0 > PSX_DROP_DB_TOTAL || w1 > PSX_DROP_DB_TOTAL ||
            w2 > PSX_DROP_DB_TOTAL) {
            if (err && errcap) snprintf(err, errcap,
                "invalid weight entry on line %d", line_no);
            fclose(f);
            return -2;
        }
        Edit *e = NULL;
        for (int i = 0; i < g_n[cur]; i++)
            if (g_edit[cur][i].card == card) { e = &g_edit[cur][i]; break; }
        if (!e) {
            if (g_n[cur] >= MAX_EDITS) {
                if (err && errcap) snprintf(err, errcap,
                    "too many entries on line %d", line_no);
                fclose(f);
                return -2;
            }
            e = &g_edit[cur][g_n[cur]++];
            entries++;
        }
        e->card = (uint16_t)card;
        e->w[0] = (uint16_t)w0; e->w[1] = (uint16_t)w1; e->w[2] = (uint16_t)w2;
    }
    fclose(f);
    if ((g_starchip_n || g_smart_drop_present) && format < 3) {
        if (err && errcap) snprintf(err, errcap,
            "smart drops and starchip rewards require format 3");
        return -2;
    }
    for (int i = 0; i < g_starchip_n; i++) {
        if (g_starchip[i].amount == UINT32_MAX) {
            if (err && errcap) snprintf(err, errcap,
                "starchip reward %d has no amount", i + 1);
            return -2;
        }
        if (!starchip_rule_valid(&g_starchip[i])) {
            if (err && errcap) snprintf(err, errcap,
                "starchip reward %d is outside the supported range", i + 1);
            return -2;
        }
    }
    if (!validate_loaded_edits(err, errcap)) return -2;
    return entries;
}

void psx_drop_edits_ensure_loaded(void)
{
    if (g_loaded) return;
    g_loaded = 1;
    ini_path(g_ini_path, sizeof(g_ini_path));
    char why[160] = "";
    const int n = read_ini(g_ini_path, why, sizeof why);
    if (n == -1)     snprintf(g_status, sizeof(g_status), "no ini (no edits)");
    else if (n < 0) { reset_data(); snprintf(g_status, sizeof(g_status), "invalid ini: %.72s", why); }
    else            snprintf(g_status, sizeof(g_status), "%d entries from ini", n);
    g_dirty = 0;
    g_gen++;
}

int psx_drop_edits_any(void)
{
    psx_drop_edits_ensure_loaded();
    for (int d = 0; d < NDUEL; d++)
        if (g_n[d] || g_replace_mask[d]) return 1;
    return 0;
}

int psx_drop_edits_has_export_content(void)
{
    psx_drop_edits_ensure_loaded();
    if (g_starchip_n || g_smart_drop_present) return 1;
    for (int d = 0; d < NDUEL; d++)
        if (g_n[d] || g_reward[d] || g_replace_mask[d]) return 1;
    return 0;
}

int psx_drop_edits_count(int duelist)
{
    psx_drop_edits_ensure_loaded();
    if (duelist < 0 || duelist >= NDUEL) return 0;
    int n = g_n[duelist];
    for (int t = 0; t < 3; t++) n += !!(g_replace_mask[duelist] & (1u << t));
    return n;
}

int psx_drop_edits_dirty(void)
{
    psx_drop_edits_ensure_loaded();
    return g_dirty;
}

unsigned psx_drop_edits_generation(void)
{
    psx_drop_edits_ensure_loaded();
    return g_gen;
}

static Edit *find(int duelist, int card)
{
    for (int i = 0; i < g_n[duelist]; i++)
        if (g_edit[duelist][i].card == card) return &g_edit[duelist][i];
    return 0;
}

int psx_drop_edits_get(int duelist, int card, uint16_t w[3])
{
    psx_drop_edits_ensure_loaded();
    if (duelist < 0 || duelist >= NDUEL) return 0;
    const Edit *e = find(duelist, card);
    if (!e) return 0;
    if (w) { w[0] = e->w[0]; w[1] = e->w[1]; w[2] = e->w[2]; }
    return 1;
}

int psx_drop_edits_set(int duelist, int card, const uint16_t w[3])
{
    psx_drop_edits_ensure_loaded();
    if (duelist < 0 || duelist >= NDUEL || card < 1 || card > NCARDS || !w)
        return 0;
    Edit *e = find(duelist, card);
    if (!e) {
        if (g_n[duelist] >= MAX_EDITS) return 0;
        e = &g_edit[duelist][g_n[duelist]++];
        e->card = (uint16_t)card;
    }
    e->w[0] = w[0]; e->w[1] = w[1]; e->w[2] = w[2];
    g_dirty = 1;
    g_gen++;
    return 1;
}

int psx_drop_edits_unset(int duelist, int card)
{
    psx_drop_edits_ensure_loaded();
    if (duelist < 0 || duelist >= NDUEL) return 0;
    for (int i = 0; i < g_n[duelist]; i++) {
        if (g_edit[duelist][i].card != card) continue;
        g_edit[duelist][i] = g_edit[duelist][--g_n[duelist]];
        g_dirty = 1;
        g_gen++;
        return 1;
    }
    return 0;
}

int psx_drop_edits_clear(int duelist)
{
    psx_drop_edits_ensure_loaded();
    int removed = 0;
    if (duelist < 0) {
        for (int d = 0; d < NDUEL; d++) {
            removed += g_n[d] + (g_replace_mask[d] != 0);
            g_n[d] = 0;
            g_replace_mask[d] = 0;
        }
    } else if (duelist < NDUEL) {
        removed = g_n[duelist] + (g_replace_mask[duelist] != 0);
        g_n[duelist] = 0;
        g_replace_mask[duelist] = 0;
    }
    if (removed) { g_dirty = 1; g_gen++; }
    return removed;
}

int psx_drop_edits_replace_band(int duelist, int tier,
                                const uint16_t weights[NCARDS])
{
    psx_drop_edits_ensure_loaded();
    if (duelist < 0 || duelist >= NDUEL || tier < 0 || tier >= 3 || !weights)
        return 0;
    uint32_t total = 0;
    for (int i = 0; i < NCARDS; i++) total += weights[i];
    if (total != 0 && total != PSX_DROP_DB_TOTAL) return 0;
    memcpy(g_replace[duelist][tier], weights,
           sizeof g_replace[duelist][tier]);
    g_replace_mask[duelist] |= (uint8_t)(1u << tier);
    g_dirty = 1;
    g_gen++;
    return 1;
}

int psx_drop_edits_replacement(int duelist, int tier)
{
    psx_drop_edits_ensure_loaded();
    return duelist >= 0 && duelist < NDUEL && tier >= 0 && tier < 3 &&
           (g_replace_mask[duelist] & (1u << tier));
}

int psx_drop_edits_band_empty(int duelist, int tier)
{
    if (!psx_drop_edits_replacement(duelist, tier)) return 0;
    for (int i = 0; i < NCARDS; i++)
        if (g_replace[duelist][tier][i]) return 0;
    return 1;
}

int psx_drop_edits_empty_count(void)
{
    psx_drop_edits_ensure_loaded();
    int n = 0;
    for (int d = 0; d < NDUEL; d++)
        for (int t = 0; t < 3; t++) n += psx_drop_edits_band_empty(d, t);
    return n;
}

int psx_drop_edits_validate(char *err, unsigned errcap)
{
    psx_drop_edits_ensure_loaded();
    for (int d = 0; d < NDUEL; d++) {
        for (int t = 0; t < 3; t++) {
            if (!psx_drop_edits_band_empty(d, t)) continue;
            if (err && errcap) snprintf(err, errcap,
                "%s %s is empty; add a card, Randomize, or restore defaults before Save/export",
                psx_cpu_display_name(d), PSX_DROP_TIER_NAMES[t]);
            return 0;
        }
    }
    if (err && errcap) err[0] = 0;
    return 1;
}

static int write_to(const char *path, int all_tables)
{
    if (!psx_drop_edits_validate(NULL, 0)) return 0;
    FILE *f = psx_fopen_utf8(path, "w");
    if (!f) return 0;
    fprintf(f,
"; Yu-Gi-Oh! Forbidden Memories - Recompiled : %s\n"
"format = 3\n"
";\n"
"; Written by the Drop Table Manager (VIEW > DROP TABLE MANAGER); hand-editing\n"
"; works too. One section per duelist, one line per edited card:\n"
";\n"
";     <card id> = <POW>, <BCD>, <TEC>\n"
";\n"
"; The line is the card's COMPLETE weight vector for that duelist - a 0 means\n"
"; \"not in this band\", so all three numbers matter. Weights are out of 2048;\n"
"; whatever they claim is taken from the duelist's other drops in proportion,\n"
"; and every band still totals 2048 exactly.\n"
";\n"
"; Clear-and-rebuilt bands use an exact sparse list instead:\n"
";     pow_table = card:weight, card:weight   (or bcd_table / tec_table)\n"
"; Exact lists must be nonempty, use unique card IDs, and total exactly 2048.\n"
"; An editor-cleared empty band is pending only and cannot be saved/exported.\n"
";\n"
"; These edits apply on top of MODS > DROP MISSING CARDS when that row is on.\n"
"; Delete a line (or the file) to fall back to the table underneath.\n"
";\n"
"; smart_drop = on applies ownership-aware filtering to EVERY normal reward\n"
"; roll in the duel. Cards at 3 total copies across deck and trunk are removed\n"
"; and survivor weights retain their relative probability. Missing means Off.\n"
";\n"
"; A duelist section may also carry a SCRIPTED REWARD - the card that duelist is\n"
"; guaranteed to drop when the campaign beats them:\n"
";\n"
";     card = 92        the card id\n"
";     when = every     optional; without it, only the FIRST win gives it\n"
";\n"
"; Ordered first-match-wins STARCHIP REWARD sections may match any or one\n"
"; campaign/free_duel mode, opponent (1..39), win/loss, and duel rank.\n"
"; Amount is 0..999999. If no rule matches, the disc's exact 1..5 reward is\n"
"; retained. Earlier sections have priority when conditions overlap.\n"
"\n", all_tables ? "complete drop tables (bulk-editable export)"
                  : "drop table edits");
    if (g_smart_drop_present)
        fprintf(f, "smart_drop = %s\n\n", g_smart_drop ? "on" : "off");
    for (int d = 0; d < NDUEL; d++) {
        if (!all_tables && !g_n[d] && !g_reward[d] && !g_replace_mask[d]) continue;
        fprintf(f, "[%s]\n", PSX_DROP_DB[d].name);
        if (g_reward[d]) {
            fprintf(f, "card = %d\n", g_reward[d]);
            if (g_reward_every[d]) fprintf(f, "when = every\n");
        }
        for (int i = 0; !all_tables && i < g_n[d]; i++) {
            const Edit *e = &g_edit[d][i];
            fprintf(f, "%-3d = %4d, %4d, %4d\n",
                    e->card, e->w[0], e->w[1], e->w[2]);
        }
        for (int t = 0; t < 3; t++) {
            if (!all_tables && !(g_replace_mask[d] & (1u << t))) continue;
            static const char *const key[3] = {
                "pow_table", "bcd_table", "tec_table"
            };
            fprintf(f, "%s = ", key[t]);
            uint16_t effective[NCARDS];
            const uint16_t *weights = g_replace[d][t];
            if (all_tables) {
                memset(effective, 0, sizeof effective);
                const PsxDropDbDuelist *db = &PSX_DROP_DB[d];
                for (int i = 0; i < db->count[t]; i++) {
                    const PsxDropWeight *e = &db->tier[t][i];
                    if (e->card >= 1 && e->card <= NCARDS)
                        effective[e->card - 1] = e->weight;
                }
                (void)psx_drop_edits_apply(d, t, effective);
                weights = effective;
            }
            int first = 1;
            for (int card = 1; card <= NCARDS; card++) {
                const unsigned w = weights[card - 1];
                if (!w) continue;
                fprintf(f, "%s%d:%u", first ? "" : ", ", card, w);
                first = 0;
            }
            fprintf(f, "\n");
        }
        fprintf(f, "\n");
    }
    {
        static const char *const mode[] = { "campaign", "free_duel" };
        static const char *const outcome[] = { "loss", "win" };
        static const char *const rank[] = {
            "D-TEC", "C-TEC", "B-TEC", "A-TEC", "S-TEC",
            "D-POW", "C-POW", "B-POW", "A-POW", "S-POW"
        };
        for (int i = 0; i < g_starchip_n; i++) {
            const PsxStarchipRule *r = &g_starchip[i];
            fprintf(f, "[Starchip Reward %d]\n", i + 1);
            fprintf(f, "mode = %s\n", r->mode < 0 ? "any" : mode[r->mode]);
            if (r->opponent < 0) fprintf(f, "opponent = any\n");
            else fprintf(f, "opponent = %d\n", r->opponent + 1);
            fprintf(f, "outcome = %s\n",
                    r->outcome < 0 ? "any" : outcome[r->outcome]);
            fprintf(f, "rank = %s\n", r->rank < 0 ? "any" : rank[r->rank]);
            fprintf(f, "amount = %u\n\n", (unsigned)r->amount);
        }
    }
    fclose(f);
    return 1;
}

int psx_drop_edits_save(void)
{
    psx_drop_edits_ensure_loaded();
    char why[160];
    if (!psx_drop_edits_validate(why, sizeof why)) {
        snprintf(g_status, sizeof(g_status), "save refused: %.76s", why);
        return 0;
    }
    if (!write_to(g_ini_path, 0)) {
        snprintf(g_status, sizeof(g_status), "save FAILED");
        return 0;
    }
    g_dirty = 0;
    snprintf(g_status, sizeof(g_status), "saved");
    return 1;
}

/* --- sharing -------------------------------------------------------------- */

static void share_dir(char *out, size_t cap)
{
    const char *dir = psx_mod_player_data_dir();
    if (dir && dir[0]) snprintf(out, cap, "%s/drop_tables", dir);
    else               snprintf(out, cap, "drop_tables");
#ifdef _WIN32
    (void)_mkdir(out);
#else
    (void)mkdir(out, 0755);
#endif
}

void psx_drop_edits_share_dir(char *out, unsigned cap)
{
    if (out && cap) share_dir(out, cap);
}

/* The last path component, whichever slash the platform wrote. */
static const char *base_name(const char *path)
{
    const char *b = path;
    for (const char *q = path; *q; q++)
        if (*q == '/' || *q == '\\') b = q + 1;
    return b;
}

static int entry_total(void)
{
    int n = 0;
    for (int d = 0; d < NDUEL; d++) {
        n += g_n[d];
        for (int t = 0; t < 3; t++) n += !!(g_replace_mask[d] & (1u << t));
    }
    return n;
}

int psx_drop_edits_export_file(const char *path, char *msg, unsigned cap)
{
    psx_drop_edits_ensure_loaded();
    if (!path || !path[0]) {
        if (msg && cap) snprintf(msg, cap, "No file to export to");
        return 0;
    }
    char why[192];
    if (!psx_drop_edits_validate(why, sizeof why)) {
        if (msg && cap) snprintf(msg, cap, "%s", why);
        return 0;
    }
    /* A dialog that came back without an extension still means an ini. */
    char p[1200];
    snprintf(p, sizeof p, "%s", path);
    if (!strchr(base_name(p), '.')) {
        const size_t n = strlen(p);
        snprintf(p + n, sizeof p - n, ".ini");
    }
    if (!write_to(p, 0)) {
        snprintf(g_status, sizeof(g_status), "export FAILED");
        if (msg && cap) snprintf(msg, cap, "Could not write that file");
        return 0;
    }
    const char *base = base_name(p);
    const int n = entry_total();
    const int r = psx_drop_edits_reward_count();
    const int sc = psx_drop_edits_starchip_count();
    const int sd = psx_drop_edits_smart_drop_present();
    /* Exporting is a copy for someone else; it neither saves the live ini
     * nor clears the unsaved-changes marker. */
    snprintf(g_status, sizeof(g_status), "exported %.60s", base);
    if (msg && cap) {
        if (r || sc || sd)
            snprintf(msg, cap, "Exported %d weight entr%s, %d scripted drop%s, %d starchip rule%s%s%s as %.28s",
                     n, n == 1 ? "y" : "ies", r, r == 1 ? "" : "s",
                     sc, sc == 1 ? "" : "s",
                     sd ? ", Smart drops " : "", sd ? (g_smart_drop ? "On" : "Off") : "",
                     base);
        else
            snprintf(msg, cap, "Exported %d weight entr%s as %.48s",
                     n, n == 1 ? "y" : "ies", base);
    }
    return 1;
}

int psx_drop_edits_export_all_file(const char *path, char *msg, unsigned cap)
{
    psx_drop_edits_ensure_loaded();
    if (!path || !path[0]) {
        if (msg && cap) snprintf(msg, cap, "No file to export to");
        return 0;
    }
    char why[192];
    if (!psx_drop_edits_validate(why, sizeof why)) {
        if (msg && cap) snprintf(msg, cap, "%s", why);
        return 0;
    }
    char p[1200];
    snprintf(p, sizeof p, "%s", path);
    if (!strchr(base_name(p), '.')) {
        const size_t n = strlen(p);
        snprintf(p + n, sizeof p - n, ".ini");
    }
    if (!write_to(p, 1)) {
        if (msg && cap) snprintf(msg, cap, "Could not write that file");
        return 0;
    }
    snprintf(g_status, sizeof g_status, "exported all tables as %.52s",
             base_name(p));
    if (msg && cap)
        snprintf(msg, cap,
                 "Exported all %d duelists and %d rank tables for bulk editing as %.36s",
                 NDUEL, NDUEL * 3, base_name(p));
    return 1;
}

int psx_drop_edits_import_file(const char *path, char *msg, unsigned cap)
{
    const int n = psx_drop_edits_load_file(path);
    if (n < 0) {
        if (msg && cap) snprintf(msg, cap, "%s",
            n == -1 ? "Could not read that file" : g_status);
        return 0;
    }
    /* An import STICKS. Every other manager's import already writes what it
     * imported -- the Fusion Manager saves its edits, the Dialogue Manager
     * imports with persist set, the Card Manager writes the card folders --
     * and a table that vanished at the next launch unless the player also
     * found the Save button was the odd one out. The live layer is already
     * what the game rolls; this makes the file agree with it. */
    const int kept = psx_drop_edits_save();
    const int r = psx_drop_edits_reward_count();
    const int sc = psx_drop_edits_starchip_count();
    const int sd = psx_drop_edits_smart_drop_present();
    int tables = 0;
    for (int d = 0; d < NDUEL; d++)
        for (int t = 0; t < 3; t++)
            tables += !!(g_replace_mask[d] & (1u << t));
    if (msg && cap) {
        if (!kept)
            snprintf(msg, cap, "Imported %d entr%s, but %s could not be written",
                     n, n == 1 ? "y" : "ies", INI_NAME);
        else if (r || sc || sd || tables)
            snprintf(msg, cap, "Imported %d entr%s, %d complete table%s, %d scripted drop%s, %d starchip rule%s%s%s; kept",
                     n, n == 1 ? "y" : "ies",
                     tables, tables == 1 ? "" : "s",
                     r, r == 1 ? "" : "s",
                     sc, sc == 1 ? "" : "s",
                     sd ? ", Smart drops " : "", sd ? (g_smart_drop ? "On" : "Off") : "");
        else
            snprintf(msg, cap, "Imported %d entr%s and kept them",
                     n, n == 1 ? "y" : "ies");
    }
    return 1;
}

int psx_drop_edits_load_file(const char *name_or_path)
{
    psx_drop_edits_ensure_loaded();
    if (!name_or_path || !name_or_path[0]) return -1;
    char path[1200];
    if (strchr(name_or_path, '/') || strchr(name_or_path, '\\')) {
        snprintf(path, sizeof path, "%s", name_or_path);
    } else {
        char dir[1024];
        share_dir(dir, sizeof dir);
        snprintf(path, sizeof path, "%s/%s", dir, name_or_path);
    }
    static Edit backup_edit[NDUEL][MAX_EDITS];
    static int backup_n[NDUEL];
    static uint16_t backup_reward[NDUEL];
    static uint8_t backup_every[NDUEL], backup_mask[NDUEL];
    static uint16_t backup_replace[NDUEL][3][NCARDS];
    static PsxStarchipRule backup_starchip[PSX_STARCHIP_RULE_MAX];
    const int backup_starchip_n = g_starchip_n;
    const int backup_smart = g_smart_drop;
    const int backup_smart_present = g_smart_drop_present;
    memcpy(backup_edit, g_edit, sizeof g_edit);
    memcpy(backup_n, g_n, sizeof g_n);
    memcpy(backup_reward, g_reward, sizeof g_reward);
    memcpy(backup_every, g_reward_every, sizeof g_reward_every);
    memcpy(backup_mask, g_replace_mask, sizeof g_replace_mask);
    memcpy(backup_replace, g_replace, sizeof g_replace);
    memcpy(backup_starchip, g_starchip, sizeof g_starchip);
    char why[160] = "";
    const int n = read_ini(path, why, sizeof why);
    if (n < 0) {
        memcpy(g_edit, backup_edit, sizeof g_edit);
        memcpy(g_n, backup_n, sizeof g_n);
        memcpy(g_reward, backup_reward, sizeof g_reward);
        memcpy(g_reward_every, backup_every, sizeof g_reward_every);
        memcpy(g_replace_mask, backup_mask, sizeof g_replace_mask);
        memcpy(g_replace, backup_replace, sizeof g_replace);
        memcpy(g_starchip, backup_starchip, sizeof g_starchip);
        g_starchip_n = backup_starchip_n;
        g_smart_drop = (uint8_t)backup_smart;
        g_smart_drop_present = (uint8_t)backup_smart_present;
        snprintf(g_status, sizeof(g_status), "load FAILED: %.76s",
                 n == -1 ? "could not read file" : why);
        return n;
    }
    g_dirty = 1;
    g_gen++;
    snprintf(g_status, sizeof(g_status), "loaded %d entries", n);
    return n;
}

/* --- randomize -------------------------------------------------------------
 *
 * Every duelist's three bands rebuilt from STOCK: each band keeps its number
 * of drops, every monster slot gets a monster drawn from all the monsters in
 * the game (no repeats within a band), the magic, trap, equip and ritual
 * slots keep their card, and every slot gets a fresh weight. The band still
 * totals 2048 by construction, and each finished table is trial-applied over
 * the stock tier through the one renormalizer before anything is kept, so a
 * table the game could not roll is never recorded.
 *
 * Expressed in the edit layer as a complete vector per touched card: the
 * stock cards that fell out are pinned to 0 and the new ones to their
 * weights. Scripted rewards are not drops and are left alone.
 */

/* xorshift32: small, seedable, and not the host's rand(), which the monster
 * effects roll on. */
static uint32_t s_rand_state;
static uint32_t rnd(void)
{
    uint32_t x = s_rand_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return s_rand_state = x;
}
static uint32_t rnd_below(uint32_t n) { return n ? rnd() % n : 0; }

/* Magic, Trap, Ritual and Equip are the four codes past the monster types,
 * in psx_card_db's table. */
#define TYPE_MAGIC 20

static int is_monster(int id)
{
    int a = 0, d = 0, ty = -1;
    if (!psx_card_db_stats(id, &a, &d, &ty)) return 0;
    return ty >= 0 && ty < TYPE_MAGIC;
}

/* n random weights that total 2048, each at least 1. The draw is a square,
 * so a band has a few heavy drops and a long tail of light ones, the way
 * the stock tables read. */
static void random_weights(uint16_t *w, int n)
{
    uint32_t raw[NCARDS], sum = 0;
    for (int i = 0; i < n; i++) {
        const uint32_t k = 1 + rnd_below(12);
        raw[i] = 4 + k * k;
        sum += raw[i];
    }
    uint32_t got = 0;
    for (int i = 0; i < n; i++) {
        uint32_t v = raw[i] * PSX_DROP_DB_TOTAL / sum;
        if (!v) v = 1;
        w[i] = (uint16_t)v;
        got += v;
    }
    while (got < PSX_DROP_DB_TOTAL) { const int i = (int)rnd_below((uint32_t)n); w[i]++; got++; }
    while (got > PSX_DROP_DB_TOTAL) { const int i = (int)rnd_below((uint32_t)n); if (w[i] > 1) { w[i]--; got--; } }
}

int psx_drop_edits_randomize(uint32_t seed, char *msg, unsigned cap)
{
    psx_drop_edits_ensure_loaded();
    if (!psx_card_db_ready()) {
        if (msg && cap) snprintf(msg, cap, "Card types are not readable yet: start the game first");
        return 0;
    }
    uint16_t pool[NCARDS];
    int pool_n = 0;
    for (int id = 1; id <= NCARDS; id++)
        if (is_monster(id)) pool[pool_n++] = (uint16_t)id;
    if (pool_n < 160) {          /* a stock band holds up to 149 drops */
        if (msg && cap) snprintf(msg, cap, "Only %d monsters readable, cannot randomize", pool_n);
        return 0;
    }

    s_rand_state = seed ? seed : 0x9E3779B9u;
    /* Built for every duelist first, kept only if all of them pass. */
    static Edit    built[NDUEL][MAX_EDITS];
    static int     built_n[NDUEL];
    static uint16_t vec[NCARDS + 1][3];
    static uint8_t  touched[NCARDS + 1];
    static uint8_t  used[NCARDS + 1];
    int entries = 0;

    for (int d = 0; d < NDUEL; d++) {
        const PsxDropDbDuelist *D = &PSX_DROP_DB[d];
        memset(vec, 0, sizeof vec);
        memset(touched, 0, sizeof touched);
        for (int t = 0; t < PSX_DROP_DB_TIERS; t++) {
            const int n = D->count[t];
            uint16_t cards[NCARDS], weights[NCARDS];
            memset(used, 0, sizeof used);
            /* the stock cards leave the band unless drawn again */
            for (int i = 0; i < n; i++) touched[D->tier[t][i].card] = 1;
            /* the kept (non-monster) slots claim their cards first */
            for (int i = 0; i < n; i++) {
                const int c = D->tier[t][i].card;
                cards[i] = 0;
                if (c >= 1 && c <= NCARDS && !is_monster(c)) { cards[i] = (uint16_t)c; used[c] = 1; }
            }
            for (int i = 0; i < n; i++) {
                if (cards[i]) continue;
                uint16_t c;
                do c = pool[rnd_below((uint32_t)pool_n)]; while (used[c]);
                used[c] = 1;
                cards[i] = c;
            }
            if (n) random_weights(weights, n);
            for (int i = 0; i < n; i++) { vec[cards[i]][t] = weights[i]; touched[cards[i]] = 1; }

            /* the game must be able to roll it: apply over the stock tier */
            uint16_t w[NCARDS];
            memset(w, 0, sizeof w);
            for (int i = 0; i < n; i++) w[D->tier[t][i].card - 1] = D->tier[t][i].weight;
            if (n && psx_drop_pins_rescale(w, cards, weights, n) != 1) {
                if (msg && cap) snprintf(msg, cap, "Randomize refused: %s band %d would not balance", D->name, t + 1);
                return 0;
            }
        }
        built_n[d] = 0;
        for (int c = 1; c <= NCARDS; c++) {
            if (!touched[c]) continue;
            Edit *e = &built[d][built_n[d]++];
            e->card = (uint16_t)c;
            e->w[0] = vec[c][0]; e->w[1] = vec[c][1]; e->w[2] = vec[c][2];
        }
        entries += built_n[d];
    }

    for (int d = 0; d < NDUEL; d++) {
        memcpy(g_edit[d], built[d], sizeof(Edit) * (size_t)built_n[d]);
        g_n[d] = built_n[d];
        g_replace_mask[d] = 0;
    }
    g_dirty = 1;
    g_gen++;
    snprintf(g_status, sizeof(g_status), "randomized (seed %u)", (unsigned)seed);
    if (msg && cap)
        snprintf(msg, cap, "Randomized every duelist's drops (%d entries). Save to keep it.", entries);
    return entries;
}

int psx_drop_edits_reward(int duelist, int *out_every)
{
    psx_drop_edits_ensure_loaded();
    if (duelist < 0 || duelist >= NDUEL) return 0;
    if (out_every) *out_every = g_reward_every[duelist];
    return g_reward[duelist];
}

int psx_drop_edits_reward_set(int duelist, int card, int every)
{
    psx_drop_edits_ensure_loaded();
    if (duelist < 0 || duelist >= NDUEL || card < 0 || card > NCARDS) return 0;
    const uint8_t ev = (uint8_t)(card && every ? 1 : 0);
    if (g_reward[duelist] == (uint16_t)card && g_reward_every[duelist] == ev)
        return 0;
    g_reward[duelist] = (uint16_t)card;
    g_reward_every[duelist] = ev;
    g_dirty = 1;
    g_gen++;
    return 1;
}

int psx_drop_edits_reward_count(void)
{
    psx_drop_edits_ensure_loaded();
    int n = 0;
    for (int d = 0; d < NDUEL; d++) n += g_reward[d] != 0;
    return n;
}

int psx_drop_edits_smart_drop(void)
{
    psx_drop_edits_ensure_loaded();
    return g_smart_drop != 0;
}

int psx_drop_edits_smart_drop_present(void)
{
    psx_drop_edits_ensure_loaded();
    return g_smart_drop_present != 0;
}

int psx_drop_edits_smart_drop_set(int enabled)
{
    psx_drop_edits_ensure_loaded();
    const uint8_t value = enabled ? 1u : 0u;
    if (g_smart_drop_present && g_smart_drop == value) return 0;
    g_smart_drop = value;
    g_smart_drop_present = 1;
    g_dirty = 1;
    g_gen++;
    return 1;
}

int psx_drop_edits_smart_drop_reset(void)
{
    psx_drop_edits_ensure_loaded();
    if (!g_smart_drop && !g_smart_drop_present) return 0;
    g_smart_drop = 0;
    g_smart_drop_present = 0;
    g_dirty = 1;
    g_gen++;
    return 1;
}

int psx_drop_edits_starchip_count(void)
{
    psx_drop_edits_ensure_loaded();
    return g_starchip_n;
}

int psx_drop_edits_starchip_get(int index, PsxStarchipRule *out)
{
    psx_drop_edits_ensure_loaded();
    if (index < 0 || index >= g_starchip_n || !out) return 0;
    *out = g_starchip[index];
    return 1;
}

int psx_drop_edits_starchip_set(int index, const PsxStarchipRule *rule)
{
    psx_drop_edits_ensure_loaded();
    if (!starchip_rule_valid(rule) || index < 0 || index > g_starchip_n ||
        (index == g_starchip_n && g_starchip_n >= PSX_STARCHIP_RULE_MAX))
        return 0;
    if (index == g_starchip_n) g_starchip_n++;
    else if (!memcmp(&g_starchip[index], rule, sizeof *rule)) return 0;
    g_starchip[index] = *rule;
    g_dirty = 1;
    g_gen++;
    return 1;
}

int psx_drop_edits_starchip_remove(int index)
{
    psx_drop_edits_ensure_loaded();
    if (index < 0 || index >= g_starchip_n) return 0;
    memmove(&g_starchip[index], &g_starchip[index + 1],
            (size_t)(g_starchip_n - index - 1) * sizeof g_starchip[0]);
    g_starchip_n--;
    g_dirty = 1;
    g_gen++;
    return 1;
}

int psx_drop_edits_starchip_move(int index, int delta)
{
    psx_drop_edits_ensure_loaded();
    const int to = index + delta;
    if (index < 0 || index >= g_starchip_n || to < 0 || to >= g_starchip_n ||
        !delta) return 0;
    const PsxStarchipRule moving = g_starchip[index];
    if (to > index)
        memmove(&g_starchip[index], &g_starchip[index + 1],
                (size_t)(to - index) * sizeof g_starchip[0]);
    else
        memmove(&g_starchip[to + 1], &g_starchip[to],
                (size_t)(index - to) * sizeof g_starchip[0]);
    g_starchip[to] = moving;
    g_dirty = 1;
    g_gen++;
    return 1;
}

int psx_drop_edits_starchip_clear(void)
{
    psx_drop_edits_ensure_loaded();
    const int n = g_starchip_n;
    if (!n) return 0;
    g_starchip_n = 0;
    g_dirty = 1;
    g_gen++;
    return n;
}

int psx_drop_edits_starchip_select(int mode, int opponent, int outcome,
                                   int rank, uint32_t *amount,
                                   int *rule_index)
{
    psx_drop_edits_ensure_loaded();
    for (int i = 0; i < g_starchip_n; i++) {
        const PsxStarchipRule *r = &g_starchip[i];
        if (r->mode >= 0 && r->mode != mode) continue;
        if (r->opponent >= 0 && r->opponent != opponent) continue;
        if (r->outcome >= 0 && r->outcome != outcome) continue;
        if (r->rank >= 0 && r->rank != rank) continue;
        if (amount) *amount = r->amount;
        if (rule_index) *rule_index = i;
        return 1;
    }
    if (rule_index) *rule_index = -1;
    return 0;
}

int psx_drop_edits_apply(int duelist, int tier, uint16_t *w)
{
    psx_drop_edits_ensure_loaded();
    if (duelist < 0 || duelist >= NDUEL || tier < 0 || tier >= 3 || !w)
        return -1;
    if (g_replace_mask[duelist] & (1u << tier)) {
        uint32_t total = 0;
        for (int i = 0; i < NCARDS; i++) total += g_replace[duelist][tier][i];
        if (!total) return -5; /* pending empty: never reaches guest RAM */
        if (total != PSX_DROP_DB_TOTAL) return -3;
        memcpy(w, g_replace[duelist][tier], sizeof g_replace[duelist][tier]);
        return 1;
    }
    if (!g_n[duelist]) return -1;
    uint16_t cards[MAX_EDITS], weights[MAX_EDITS];
    int n = 0;
    for (int i = 0; i < g_n[duelist]; i++) {
        cards[n]   = g_edit[duelist][i].card;
        weights[n] = g_edit[duelist][i].w[tier];
        n++;
    }
    return psx_drop_pins_rescale(w, cards, weights, n);
}

int psx_drop_edits_state_json(char *out, unsigned cap)
{
    if (!out || cap < 128u) return 0;
    psx_drop_edits_ensure_loaded();
    int total = 0, duelists = 0, replacements = 0;
    for (int d = 0; d < NDUEL; d++) {
        total += g_n[d];
        if (g_n[d] || g_replace_mask[d]) duelists++;
        for (int t = 0; t < 3; t++)
            replacements += !!(g_replace_mask[d] & (1u << t));
    }
    return snprintf(out, cap,
        "\"entries\":%d,\"duelists\":%d,\"replacements\":%d,"
        "\"empty_bands\":%d,\"smart_drop\":%d,\"smart_drop_present\":%d,"
        "\"starchip_rules\":%d,"
        "\"dirty\":%d,\"gen\":%u,"
        "\"status\":\"%s\"",
        total, duelists, replacements, psx_drop_edits_empty_count(),
        g_smart_drop, g_smart_drop_present, g_starchip_n,
        g_dirty, g_gen, g_status);
}
