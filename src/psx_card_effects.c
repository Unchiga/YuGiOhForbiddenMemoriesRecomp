/* psx_card_effects.c -- make the game honour an edited card's effect fields.
 *
 * WHAT DRIVES EACH EFFECT CLASS IN THE STOCK GAME (decomp, 2026-09-04):
 *
 *   Magic cards. func_80026BA4(cardId, phase) turns the id into an effect
 *   index (301..350 -> 0..49, 651..700 -> 50..99, 721 -> 100), the class
 *   byte table at 0x80090AD4 picks one of 15 handlers, and each handler
 *   reads its magnitude from a small table keyed by the card id:
 *     heal   338..342  byte at 0x8009AF30[id-338] x100
 *     burn   343..347  byte at 0x8009AF38[id-343] x10
 *     kill   class 5   pairs {id-600, arg} at 0x80090A4C: arg<21 = a type,
 *                      else ATK >= arg*10
 *     weaken 349/669   immediates -500 / -1000 in func_80025D30
 *     field  330..335  terrain = id-329
 *   The rest (Raigeki, Dark Hole/Dragon Capture Jar, Stop Defense, flip,
 *   Swords, Cursebreaker, Harpie) take no number.
 *
 *   Equip cards. The pair is answered by equip_table_lookup(equip, card)
 *   over the group table streamed at duel start from the terrain package
 *   (WA_MRG +0x22000, 5 sectors, one copy per terrain), and the bonus is the
 *   immediate 500 (1000 for Megamorph) in the combine state machine.
 *
 *   Traps 681..686: the attack check compares the attacker's ATK with
 *   byte 0x8009AF24[id-681] x100 and the tightest qualifying trap fires.
 *   Field cards: gDuel_aTerrainBoost 0x800909D4, s8[20 types][6 terrains]
 *   x10. Rituals: {spell, m1, m2, m3, result} records streamed at duel start
 *   from the package (+0x2B800, one sector).
 *
 * HOW THE EDITS LAND:
 *   - tables in the EXE (terrain, trap ceilings, class bytes) are asserted
 *     every frame, the way psx_card_packs asserts a card's stats word;
 *   - disc-streamed tables (equip groups, ritual recipes) are rebuilt from
 *     the stock sectors and served as sector overrides for all 7 copies;
 *   - a Magic effect that is not the card's own class is delivered by an
 *     entry hook on func_80026BA4: a0 is rewritten to the stock card whose
 *     class implements the effect, and that class's parameter (a table
 *     byte or the weaken immediate) is HELD at the edited value while the
 *     handler runs (D_8009B220 bit 0x8000), then put back;
 *   - an equip's type list is answered from an entry hook on
 *     equip_table_lookup: the rebuilt table starts with a scratch group
 *     {0xFFFE, 1, 0}, and when the pair is allowed the hook points that
 *     group at the pair, so the stock scan says yes. The hook also sets the
 *     bonus immediates for the equip being resolved.
 *   Immediates are written with psx_mod_write_code_word, which sends that
 *   function to the interpreter; only functions with an edited value are
 *   ever touched, so a stock install keeps everything compiled.
 *
 * WHAT STAYS CODE (honest list): the trap response itself (destroy the
 * attacker), Goblin Fan / Bad Reaction to Simochi / Reverse Trap / Fake
 * Trap, which cards count as traps (681..686 only), Weaken acting on the
 * opponent's side only, the guardian-star wheel and its +/-500, and the
 * heal/burn granularity (x100 / x10, ceilings 25500 / 2550). */

#include "psx_card_effects.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu_state.h"
#include "mod_plugins.h"
#include "psx_game_hooks.h"
#include "psx_card_db.h"
#include "psx_card_extend.h"
#include "psx_lp_popup.h"

#define CARD_COUNT 722
#define SECTOR 2048

/* ---- guest addresses ---------------------------------------------------- */
#define PKG_LBA(k)     (15932u + 235u * (uint32_t)(k))   /* terrain package k = 0..6 */
#define EQUIP_LBA(k)   (PKG_LBA(k) + 68u)                /* 5 sectors */
#define EQUIP_SECTORS  5
#define EQUIP_USED     0x2100                             /* bytes the game keeps */
#define RITUAL_LBA(k)  (PKG_LBA(k) + 105u)               /* 1 sector */
#define TERRAINS       7

#define EQUIP_RAM      0x8017A1D8u
#define TERRAIN_TAB    0x800909D4u   /* s8[120] */
#define TRAP_TAB       0x8009AF24u   /* u8[6] */
#define HEAL_TAB       0x8009AF30u   /* u8[5] */
#define BURN_TAB       0x8009AF38u   /* u8[5] */
#define KILL_TAB       0x80090A4Cu   /* u8[16] */
#define CLASS_TAB      0x80090AD4u   /* u8[101] */
#define FX_STATE       0x8009B220u   /* u16, bit 0x8000 = a handler is running */
#define MALUS_WORD     0x80025E60u   /* addiu v0,v0,-500  (Spellbinding Circle path) */
#define EQ_WORD_TARGET 0x8001A7F8u   /* addiu v0,zero,500 */
#define EQ_WORD_PEND   0x8001A494u   /* addiu v0,v0,500   pending bonus */
#define EQ_WORD_MM_T   0x8001A820u   /* addiu v1,zero,1000  Megamorph target */
#define EQ_WORD_MM_P   0x8001A82Cu   /* addiu v0,v0,500     Megamorph extra pending */
#define HOOK_MAGIC     0x80026BA4u
#define HOOK_EQUIP     0x80019A08u
#define STATS_STOCK    0x801D4244u

#define SCRATCH_KEY    0xFFFEu

/* ---- state -------------------------------------------------------------- */
typedef struct {
    int present;
    PsxCardPack cfg;
} Fx;
static Fx      *s_fx[CARD_COUNT + 1];
static unsigned s_seen_gen = (unsigned)-1;
static int      s_booted;

/* stock snapshots */
static int     s_stock_ok;
static uint8_t s_stock_terrain[120], s_stock_trap[6], s_stock_heal[5], s_stock_burn[5], s_stock_kill[16], s_stock_class[101];
static uint32_t s_stock_malus, s_stock_eq_t, s_stock_eq_p, s_stock_mm_t, s_stock_mm_p;
static uint16_t s_stock_ritual[SECTOR / 2];
static int      s_stock_ritual_ok;

/* desired EXE tables (stock + edits), recomputed on rebuild */
static uint8_t s_want_terrain[120], s_want_trap[6], s_want_class[101];

/* overrides */
static int s_equip_override, s_ritual_override;
static int s_equip_bytes, s_equip_dropped, s_ritual_records;

/* the magic hold */
static struct {
    int active;
    unsigned since;          /* frame it was set */
    int kind;                /* PSX_CARD_FX_* */
    uint8_t heal, burn, kill_id, kill_arg;
    uint32_t malus;
} s_hold;
static int s_malus_dirty;
static int s_hold_amount;

/* the equip bonus in force */
static int s_eq_active, s_eq_dirty, s_eq_bonus, s_eq_card;
static uint16_t s_scratch_key = SCRATCH_KEY;

/* hook log for the debug server */
typedef struct { unsigned frame; uint32_t at; int a, b, out; } Ev;
static Ev s_ev[16];
static unsigned s_ev_n;
static unsigned s_frame;

static void ev(uint32_t at, int a, int b, int out)
{
    Ev *e = &s_ev[s_ev_n++ & 15u];
    e->frame = s_frame; e->at = at; e->a = a; e->b = b; e->out = out;
}

/* ---- helpers -------------------------------------------------------------- */
static int fx_index(int id)
{
    if (id >= 301 && id <= 350) return id - 301;
    if (id >= 651 && id <= 700) return id - 601;
    if (id == 721) return 100;
    return -1;
}

static int card_type(int id)
{
    if (id < 1 || id > CARD_COUNT) return -1;
    const uint32_t w = psx_mod_read_word(psx_card_extend_stats_base() + (uint32_t)(id - 1) * 4u);
    return (int)((w >> 26) & 0x1Fu);
}

static const Fx *fx_of(int id)
{
    if (id < 1 || id > CARD_COUNT) return NULL;
    const Fx *f = s_fx[id];
    return (f && f->present) ? f : NULL;
}

static uint8_t clamp_u8(long v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

static void take_stock(void)
{
    if (s_stock_ok || !psx_card_db_ready()) return;
    for (int i = 0; i < 120; i++) s_stock_terrain[i] = psx_mod_read_byte(TERRAIN_TAB + (uint32_t)i);
    for (int i = 0; i < 6; i++)   s_stock_trap[i] = psx_mod_read_byte(TRAP_TAB + (uint32_t)i);
    for (int i = 0; i < 5; i++)   s_stock_heal[i] = psx_mod_read_byte(HEAL_TAB + (uint32_t)i);
    for (int i = 0; i < 5; i++)   s_stock_burn[i] = psx_mod_read_byte(BURN_TAB + (uint32_t)i);
    for (int i = 0; i < 16; i++)  s_stock_kill[i] = psx_mod_read_byte(KILL_TAB + (uint32_t)i);
    for (int i = 0; i < 101; i++) s_stock_class[i] = psx_mod_read_byte(CLASS_TAB + (uint32_t)i);
    s_stock_malus = psx_mod_read_word(MALUS_WORD);
    s_stock_eq_t = psx_mod_read_word(EQ_WORD_TARGET);
    s_stock_eq_p = psx_mod_read_word(EQ_WORD_PEND);
    s_stock_mm_t = psx_mod_read_word(EQ_WORD_MM_T);
    s_stock_mm_p = psx_mod_read_word(EQ_WORD_MM_P);
    /* the words must be what the decomp says they are, or the layer stays off */
    if (s_stock_malus != 0x2442FE0Cu || s_stock_eq_t != 0x240201F4u || s_stock_eq_p != 0x244201F4u ||
        s_stock_mm_t != 0x240303E8u || s_stock_mm_p != 0x244201F4u || s_stock_trap[0] != 5 || s_stock_heal[0] != 2) {
        return;
    }
    {
        static uint8_t sec[SECTOR];
        if (psx_mod_cd_read_stock_sector(RITUAL_LBA(0), sec)) {
            for (int i = 0; i < SECTOR / 2; i++) s_stock_ritual[i] = (uint16_t)(sec[i * 2] | (sec[i * 2 + 1] << 8));
            s_stock_ritual_ok = 1;
        }
    }
    memcpy(s_want_terrain, s_stock_terrain, sizeof s_want_terrain);
    memcpy(s_want_trap, s_stock_trap, sizeof s_want_trap);
    memcpy(s_want_class, s_stock_class, sizeof s_want_class);
    s_stock_ok = 1;
}

/* ---- stock facts for the manager ------------------------------------------ */
void psx_card_effects_stock(int id, PsxCardStock *out)
{
    if (!out) return;
    out->effect = out->amount = out->equip_bonus = out->trap_atk_max = -1;
    for (int t = 0; t < 20; t++) out->boost[t] = PSX_CARD_PACK_BOOST_UNSET;
    out->ritual_mat[0] = out->ritual_mat[1] = out->ritual_mat[2] = out->ritual_result = -1;
    take_stock();
    if (!s_stock_ok || id < 1 || id > CARD_COUNT) return;
    const int type = card_type(id);
    if (type == 23) out->equip_bonus = (id == 657) ? 1000 : 500;
    if (id >= 681 && id <= 686) out->trap_atk_max = (int)s_stock_trap[id - 681] * 100;
    if (id >= 330 && id <= 335) {
        const int terrain = id - 329;
        for (int t = 0; t < 20; t++) out->boost[t] = (int)(int8_t)s_stock_terrain[t * 6 + terrain - 1] * 10;
    }
    const int idx = fx_index(id);
    if (idx < 0) return;
    switch (s_stock_class[idx]) {
    case 0:  out->effect = PSX_CARD_FX_NONE; break;
    case 1:  out->effect = PSX_CARD_FX_FIELD; out->amount = id - 329; break;
    case 2:  out->effect = PSX_CARD_FX_HEAL; if (id >= 338 && id <= 342) out->amount = (int)s_stock_heal[id - 338] * 100; break;
    case 3:  out->effect = PSX_CARD_FX_DAMAGE; if (id >= 343 && id <= 347) out->amount = (int)s_stock_burn[id - 343] * 10; break;
    case 4:  out->effect = (id == 329) ? PSX_CARD_FX_DRAGON_JAR : PSX_CARD_FX_DARK_HOLE; break;
    case 5:
        for (int i = 0; i < 8; i++) {
            if (s_stock_kill[i * 2] == 0) break;
            if ((int)s_stock_kill[i * 2] + 600 != id) continue;
            const int arg = s_stock_kill[i * 2 + 1];
            if (arg < 21) { out->effect = PSX_CARD_FX_DESTROY_TYPE; out->amount = arg; }
            else          { out->effect = PSX_CARD_FX_DESTROY_ATK;  out->amount = arg * 10; }
        }
        break;
    case 6:  out->effect = PSX_CARD_FX_STOP_DEFENSE; break;
    case 7:  out->effect = PSX_CARD_FX_RAIGEKI; break;
    case 8:  out->effect = PSX_CARD_FX_FLIP; break;
    case 9:  out->effect = PSX_CARD_FX_WEAKEN; out->amount = (id == 349) ? 500 : 1000; break;
    case 10: out->effect = PSX_CARD_FX_SWORDS; break;
    case 11: out->effect = PSX_CARD_FX_CURSEBREAKER; break;
    case 12:
        out->effect = PSX_CARD_FX_RITUAL;
        if (s_stock_ritual_ok) {
            for (int i = 0; i + 5 <= SECTOR / 2; i += 5) {
                if (s_stock_ritual[i] == 0) break;
                if ((s_stock_ritual[i] & 0x3FFu) != (unsigned)id) continue;
                out->ritual_mat[0] = s_stock_ritual[i + 1]; out->ritual_mat[1] = s_stock_ritual[i + 2];
                out->ritual_mat[2] = s_stock_ritual[i + 3]; out->ritual_result = s_stock_ritual[i + 4];
                break;
            }
        }
        break;
    case 13: out->effect = PSX_CARD_FX_HARPIE; break;
    default: break;
    }
}

const char *psx_card_effects_note(int id, int type)
{
    if (type == 23) return "An equip: set its bonus and the monsters it fits (types, card ids, all or none).";
    if (type == 21) return (id >= 681 && id <= 686) ? "A trap that stops an attacker at or under its ATK ceiling."
                                                     : "This trap is pure code (Goblin Fan, Simochi, Reverse Trap, Fake Trap): nothing to set.";
    if (type == 22 && fx_index(id) >= 0) return "A ritual: three material ids on your field become the result.";
    if (id >= 330 && id <= 335) return "A field card: each monster type's boost while this field is up.";
    if (type == 20 || type == 22) return (fx_index(id) >= 0) ? "A magic card: pick what it does when played and how much."
                                                            : "Played as a spell: pick what it does and how much (a card born a monster cannot be a ritual or do nothing).";
    if (type < 20) return "Monster effects: how it fights, what it casts when summoned, destroyed, attacking or each turn, bonuses, immunities.";
    return "";
}

/* ---- rebuild: read the packs, redo the overrides and the wanted tables ----- */
static void rebuild_equip_override(void)
{
    static uint8_t stock[EQUIP_SECTORS * SECTOR], out[EQUIP_SECTORS * SECTOR];
    static uint8_t seen[CARD_COUNT + 1];
    int any = 0;
    for (int id = 1; id <= CARD_COUNT; id++) {
        const Fx *f = fx_of(id);
        if (f && (f->cfg.equips_set || f->cfg.equip_types)) { any = 1; break; }
    }
    if (!any) {
        if (s_equip_override) for (int k = 0; k < TERRAINS; k++) for (int s = 0; s < EQUIP_SECTORS; s++) psx_mod_cd_override_clear(EQUIP_LBA(k) + (uint32_t)s);
        s_equip_override = 0; s_equip_bytes = 0; s_equip_dropped = 0;
        return;
    }
    for (int k = 0; k < TERRAINS; k++) {
        int ok = 1;
        for (int s = 0; s < EQUIP_SECTORS && ok; s++) ok = psx_mod_cd_read_stock_sector(EQUIP_LBA(k) + (uint32_t)s, stock + s * SECTOR);
        if (!ok) return;
        memset(out, 0xFF, sizeof out);
        memset(seen, 0, sizeof seen);
        int o = 0, dropped = 0;
        #define PUT16(v) do { out[o++] = (uint8_t)(v); out[o++] = (uint8_t)((v) >> 8); } while (0)
        PUT16(SCRATCH_KEY); PUT16(1); PUT16(0);
        int p = 0;
        for (;;) {
            if (p + 4 > EQUIP_USED) break;
            const int key = stock[p] | (stock[p + 1] << 8);
            if (key == 0) break;
            const int cnt = stock[p + 2] | (stock[p + 3] << 8);
            const int glen = 4 + 2 * cnt;
            const Fx *f = (key >= 1 && key <= CARD_COUNT) ? fx_of(key) : NULL;
            if (f && f->cfg.equips_set) {
                seen[key] = 1;
                if (f->cfg.equip_n > 0) {
                    if (o + 4 + 2 * f->cfg.equip_n + 2 <= EQUIP_USED) {
                        PUT16(key); PUT16(f->cfg.equip_n);
                        for (int i = 0; i < f->cfg.equip_n; i++) PUT16(f->cfg.equip_ids[i]);
                    } else dropped++;
                }
            } else if (o + glen + 2 <= EQUIP_USED) {
                memcpy(out + o, stock + p, (size_t)glen); o += glen;
            } else dropped++;
            p += glen;
        }
        for (int id = 1; id <= CARD_COUNT; id++) {
            const Fx *f = fx_of(id);
            if (!f || !f->cfg.equips_set || seen[id] || f->cfg.equip_n <= 0) continue;
            if (o + 4 + 2 * f->cfg.equip_n + 2 <= EQUIP_USED) {
                PUT16(id); PUT16(f->cfg.equip_n);
                for (int i = 0; i < f->cfg.equip_n; i++) PUT16(f->cfg.equip_ids[i]);
            } else dropped++;
        }
        PUT16(0);
        #undef PUT16
        for (int s = 0; s < EQUIP_SECTORS; s++) psx_mod_cd_override_set(EQUIP_LBA(k) + (uint32_t)s, out + s * SECTOR, SECTOR);
        s_equip_bytes = o; s_equip_dropped = dropped;
    }
    s_equip_override = 1;
}

static void rebuild_ritual_override(void)
{
    static uint8_t stock[SECTOR], out[SECTOR];
    static uint8_t seen[CARD_COUNT + 1];
    int any = 0;
    for (int id = 1; id <= CARD_COUNT; id++) { const Fx *f = fx_of(id); if (f && f->cfg.ritual_set) { any = 1; break; } }
    if (!any) {
        if (s_ritual_override) for (int k = 0; k < TERRAINS; k++) psx_mod_cd_override_clear(RITUAL_LBA(k));
        s_ritual_override = 0; s_ritual_records = 0;
        return;
    }
    for (int k = 0; k < TERRAINS; k++) {
        if (!psx_mod_cd_read_stock_sector(RITUAL_LBA(k), stock)) return;
        memset(out, 0xFF, sizeof out);
        memset(seen, 0, sizeof seen);
        int o = 0, n = 0;
        #define PUT16(v) do { out[o++] = (uint8_t)(v); out[o++] = (uint8_t)((v) >> 8); } while (0)
        #define REC_MAX 200
        for (int p = 0; p + 10 <= SECTOR; p += 10) {
            const int key = stock[p] | (stock[p + 1] << 8);
            if (key == 0) break;
            const int id = key & 0x3FF;
            const Fx *f = (id >= 1 && id <= CARD_COUNT) ? fx_of(id) : NULL;
            if (n >= REC_MAX) break;
            if (f && f->cfg.ritual_set) {
                seen[id] = 1;
                PUT16(id); PUT16(f->cfg.ritual_mat[0]); PUT16(f->cfg.ritual_mat[1]); PUT16(f->cfg.ritual_mat[2]); PUT16(f->cfg.ritual_result);
            } else { memcpy(out + o, stock + p, 10); o += 10; }
            n++;
        }
        for (int id = 1; id <= CARD_COUNT && n < REC_MAX; id++) {
            const Fx *f = fx_of(id);
            if (!f || !f->cfg.ritual_set || seen[id]) continue;
            PUT16(id); PUT16(f->cfg.ritual_mat[0]); PUT16(f->cfg.ritual_mat[1]); PUT16(f->cfg.ritual_mat[2]); PUT16(f->cfg.ritual_result);
            n++;
        }
        for (int i = 0; i < 5; i++) PUT16(0);
        #undef PUT16
        #undef REC_MAX
        psx_mod_cd_override_set(RITUAL_LBA(k), out, SECTOR);
        s_ritual_records = n;
    }
    s_ritual_override = 1;
}

static void rebuild_wanted_tables(void)
{
    memcpy(s_want_terrain, s_stock_terrain, sizeof s_want_terrain);
    memcpy(s_want_trap, s_stock_trap, sizeof s_want_trap);
    memcpy(s_want_class, s_stock_class, sizeof s_want_class);
    for (int id = 1; id <= CARD_COUNT; id++) {
        const Fx *f = fx_of(id);
        if (!f) continue;
        const PsxCardPack *c = &f->cfg;
        if (id >= 330 && id <= 335 && c->boost_set) {
            const int terrain = id - 329;
            for (int t = 0; t < 20; t++)
                if (c->boost[t] != PSX_CARD_PACK_BOOST_UNSET) s_want_terrain[t * 6 + terrain - 1] = (uint8_t)(int8_t)(c->boost[t] / 10);
        }
        if (id >= 681 && id <= 686 && c->trap_atk_max >= 0) s_want_trap[id - 681] = clamp_u8(c->trap_atk_max / 100);
        const int idx = fx_index(id);
        if (idx >= 0) {
            if (c->effect == PSX_CARD_FX_RITUAL) s_want_class[idx] = 12;
            else if (c->effect == PSX_CARD_FX_NONE) s_want_class[idx] = 0;
            else if (c->effect > 0 && s_stock_class[idx] == 12) s_want_class[idx] = 0;   /* a ritual card given another effect: the proxy class 0 would be wrong, but the hook rewrites a0 before the class byte is read, so any non-12 class is fine */
        }
    }
}

static void rebuild(void)
{
    for (int id = 1; id <= CARD_COUNT; id++) {
        PsxCardPack c;
        const int have = psx_card_packs_get(id, &c);
        int present = 0;
        if (have) {
            present = c.effect >= 0 || c.amount >= 0 || c.equip_bonus >= 0 || c.equips_set || c.equip_types ||
                      c.boost_set || c.trap_atk_max >= 0 || c.ritual_set;
        }
        if (present) {
            if (!s_fx[id]) s_fx[id] = (Fx *)calloc(1, sizeof(Fx));
            if (!s_fx[id]) continue;
            s_fx[id]->present = 1;
            s_fx[id]->cfg = c;
        } else if (s_fx[id]) {
            s_fx[id]->present = 0;
        }
    }
    rebuild_equip_override();
    rebuild_ritual_override();
    rebuild_wanted_tables();
}

/* ---- per-frame assertion ---------------------------------------------------- */
static void assert_byte(uint32_t at, uint8_t v) { if (psx_mod_read_byte(at) != v) psx_mod_write_byte(at, v); }
static void assert_code(uint32_t at, uint32_t v) { if (psx_mod_read_word(at) != v) psx_mod_write_code_word(at, v); }

/* The LP popups (heal kind 5, burn kinds 6 and 7) are phase records in the
 * D_800EAD88 pool that the duel overlay draws after the handler is gone;
 * they read the parameter table live, so the hold must outlast them. */
#define PHASE_POOL 0x800EAD88u
static int lp_popup_live(void)
{
    for (int i = 0; i < 8; i++) {
        const uint32_t rec = PHASE_POOL + (uint32_t)i * 0x20u;
        if (!(psx_mod_read_byte(rec + 0x1C) & 0x80u)) continue;
        const unsigned k = psx_mod_read_half(rec + 0x18);
        if (k == 5 || k == 6 || k == 7) return 1;
    }
    return 0;
}

/* The burn popup (kind 6/7) and the heal popup (kind 5) are fixed sprites
 * for the stock amounts ("-50", "+200"...), picked by the table index the
 * handler leaves in f1A; no hold can change what they say. While an edited
 * amount is held, the record is released before the overlay draws it and
 * the number is shown by psx_lp_popup instead. */
static void popup_fix(void)
{
    if (!s_hold.active || (s_hold.kind != PSX_CARD_FX_DAMAGE && s_hold.kind != PSX_CARD_FX_HEAL)) return;
    for (int i = 0; i < 8; i++) {
        const uint32_t rec = PHASE_POOL + (uint32_t)i * 0x20u;
        if (!(psx_mod_read_byte(rec + 0x1C) & 0x80u)) continue;
        const unsigned k = psx_mod_read_half(rec + 0x18);
        const int burn = (k == 6 || k == 7), heal = (k == 5);
        if ((s_hold.kind == PSX_CARD_FX_DAMAGE && burn) || (s_hold.kind == PSX_CARD_FX_HEAL && heal)) {
            psx_mod_write_byte(rec + 0x1C, 0);
            psx_mod_write_half(rec + 0x18, 0);
            psx_lp_popup_show(s_hold_amount, heal);
            ev(0x600u, (int)k, s_hold_amount, i);
        }
    }
}

static void hold_release(void)
{
    s_hold.active = 0;
    assert_byte(HEAL_TAB, s_stock_heal[0]);
    assert_byte(BURN_TAB, s_stock_burn[0]);
    assert_byte(KILL_TAB, s_stock_kill[0]);
    assert_byte(KILL_TAB + 1, s_stock_kill[1]);
    if (s_malus_dirty) assert_code(MALUS_WORD, s_stock_malus);
}

static void hold_apply(void)
{
    switch (s_hold.kind) {
    case PSX_CARD_FX_HEAL:   assert_byte(HEAL_TAB, s_hold.heal); break;
    case PSX_CARD_FX_DAMAGE: assert_byte(BURN_TAB, s_hold.burn); break;
    case PSX_CARD_FX_DESTROY_TYPE:
    case PSX_CARD_FX_DESTROY_ATK:
    case PSX_CARD_FX_DESTROY_STRONGEST: assert_byte(KILL_TAB, s_hold.kill_id); assert_byte(KILL_TAB + 1, s_hold.kill_arg); break;
    case PSX_CARD_FX_WEAKEN: s_malus_dirty = 1; assert_code(MALUS_WORD, s_hold.malus); break;
    default: break;
    }
}

static void eq_apply(void)
{
    if (s_eq_active) {
        const uint16_t n = (uint16_t)s_eq_bonus;
        s_eq_dirty = 1;
        assert_code(EQ_WORD_TARGET, 0x24020000u | n);
        assert_code(EQ_WORD_PEND,   0x24420000u | n);
        assert_code(EQ_WORD_MM_T,   0x24030000u | n);
        assert_code(EQ_WORD_MM_P,   0x24420000u);
    } else if (s_eq_dirty) {
        assert_code(EQ_WORD_TARGET, s_stock_eq_t);
        assert_code(EQ_WORD_PEND,   s_stock_eq_p);
        assert_code(EQ_WORD_MM_T,   s_stock_mm_t);
        assert_code(EQ_WORD_MM_P,   s_stock_mm_p);
    }
}

static void tick(void)
{
    if (!psx_mod_game_started()) return;
    s_frame++;
    take_stock();
    if (!s_stock_ok) return;
    s_booted = 1;
    const unsigned gen = psx_card_packs_generation();
    if (gen != s_seen_gen) { s_seen_gen = gen; rebuild(); }
    for (int i = 0; i < 120; i++) assert_byte(TERRAIN_TAB + (uint32_t)i, s_want_terrain[i]);
    for (int i = 0; i < 6; i++)   assert_byte(TRAP_TAB + (uint32_t)i, s_want_trap[i]);
    for (int i = 0; i < 101; i++) assert_byte(CLASS_TAB + (uint32_t)i, s_want_class[i]);
    if (s_hold.active) {
        const int done = s_frame > s_hold.since + 2 && !(psx_mod_read_half(FX_STATE) & 0x8000u);
        if ((done && !lp_popup_live() && s_frame > s_hold.since + 6) || s_frame > s_hold.since + 900) hold_release();
        else { hold_apply(); popup_fix(); }
    }
    eq_apply();
}

/* ---- the hooks ------------------------------------------------------------------ */
/* func_80026BA4(cardId, phase): deliver an edited Magic effect through the
 * stock card that implements it. */
/* Fill the hold for an effect and return the stock card id whose class
 * implements it, or -1 (none, ritual: the class byte does those). */
static int fx_prepare(int fx, int amount, int target, int terrain, int phase)
{
    int proxy = -1;
    memset(&s_hold, 0, sizeof s_hold);
    switch (fx) {
    case PSX_CARD_FX_HEAL:         proxy = 338; s_hold.heal = clamp_u8((amount >= 0 ? amount : 500) / 100); s_hold_amount = s_hold.heal * 100; break;
    case PSX_CARD_FX_DAMAGE:       proxy = 343; s_hold.burn = clamp_u8((amount >= 0 ? amount : 500) / 10); s_hold_amount = s_hold.burn * 10; break;
    case PSX_CARD_FX_DESTROY_TYPE: proxy = 653; s_hold.kill_id = 53; s_hold.kill_arg = (uint8_t)(target >= 0 && target < 20 ? target : 3); break;
    case PSX_CARD_FX_DESTROY_ATK: {
        long a = (amount >= 0 ? amount : 1500) / 10;
        if (a < 21) a = 21;
        proxy = 653; s_hold.kill_id = 53; s_hold.kill_arg = clamp_u8(a); break;
    }
    case PSX_CARD_FX_RAIGEKI:      proxy = 337; break;
    case PSX_CARD_FX_DARK_HOLE:    proxy = 336; break;
    case PSX_CARD_FX_DRAGON_JAR:   proxy = 329; break;
    case PSX_CARD_FX_STOP_DEFENSE: proxy = 320; break;
    case PSX_CARD_FX_FLIP:         proxy = 350; break;
    case PSX_CARD_FX_WEAKEN: {
        int a = (amount != -1) ? amount : 500;
        if (a > 9999) a = 9999; if (a < -9999) a = -9999;
        proxy = 349; s_hold.malus = 0x24420000u | ((uint32_t)(-a) & 0xFFFFu); break;
    }
    case PSX_CARD_FX_SWORDS:       proxy = 348; break;
    case PSX_CARD_FX_CURSEBREAKER: proxy = 655; break;
    case PSX_CARD_FX_HARPIE:       proxy = 672; break;
    case PSX_CARD_FX_FIELD:        proxy = 329 + (terrain >= 1 && terrain <= 6 ? terrain : 1); break;
    case PSX_CARD_FX_DESTROY_STRONGEST: {
        /* the kill class with its threshold set to the strongest monster the
         * acting side's opponent has right now: that one goes (ties all go) */
        const int side = psx_mod_read_byte(0x8009B1D5u) & 1;
        int best = 0;
        for (int r = 5; r <= 9; r++) {
            const uint32_t row = 0x801A7AD8u + (uint32_t)(15 * (side ^ 1) + r) * 0x1Cu;
            if (!(psx_mod_read_half(row + 0x16) & 0x8000u)) continue;
            int atk = (int)(int16_t)psx_mod_read_half(row + 0xE) + (int)(int16_t)psx_mod_read_half(row + 0x12) + (int)(int16_t)psx_mod_read_half(row + 0x14);
            if (atk < 0) atk = 0; if (atk > 9999) atk = 9999;
            if (atk > best) best = atk;
        }
        long a = best / 10; if (a < 21) a = 21; if (a > 255) a = 255;
        ev(0x653u, best, (int)a, side);
        proxy = 653; s_hold.kill_id = 53; s_hold.kill_arg = (uint8_t)a; break;
    }
    case PSX_CARD_FX_LOSE_LP:
    case PSX_CARD_FX_GAMBLE_LP: {
        /* the caster's own LP: written directly (the game has no such class),
         * shown with the number popup, and the play itself becomes a no-op
         * spell (class 0) */
        /* the dispatcher runs once per phase of a play: act on phase 1 only */
        proxy = 301;
        if (phase != 1) break;
        const int side = psx_mod_read_byte(0x8009B1D5u) & 1;
        const uint32_t lpat = 0x800E9FF0u + (uint32_t)side * 0x20u + 0x14u;
        const int lp = psx_mod_read_half(lpat);
        int loss = 0;
        if (fx == PSX_CARD_FX_LOSE_LP) loss = amount >= 0 ? amount : 500;
        else if (((unsigned)rand() ^ s_frame) & 1u) loss = lp / 2;
        if (loss > lp) loss = lp;
        if (loss > 0) { psx_mod_write_half(lpat, (uint16_t)(lp - loss)); psx_lp_popup_show(loss, 0); }
        ev(0x700u, fx, loss, side);
        break;
    }
    default: break;
    }
    if (proxy < 0) return -1;
    s_hold.active = 1;
    s_hold.since = s_frame;
    s_hold.kind = fx;
    hold_apply();
    return proxy;
}

static void hook_magic(struct CPUState *cpu, uint32_t address)
{
    (void)address;
    if (!s_stock_ok) return;
    const int id = (int)(int16_t)cpu->gpr[4];
    const Fx *f = fx_of(id);
    if (!f || f->cfg.effect < 0) return;
    const PsxCardPack *c = &f->cfg;
    const int proxy = fx_prepare(c->effect, c->amount, c->target, c->terrain, (int)cpu->gpr[5]);
    if (proxy < 0) return;
    cpu->gpr[4] = (uint32_t)proxy;
    ev(HOOK_MAGIC, id, (int)cpu->gpr[5], proxy);
}

int psx_card_effects_hold_active(void) { return s_hold.active; }

int psx_card_effects_cast(int fx, int amount, int target, int terrain)
{
    if (!s_stock_ok) return 0;
    const int proxy = fx_prepare(fx, amount, target, terrain, 1);
    if (proxy < 0) return 0;
    /* what func_80026BA4(proxy, 1) would set */
    const int idx = fx_index(proxy);
    psx_mod_write_half(0x8009B1A8u, (uint16_t)idx);
    psx_mod_write_half(0x8009B1D2u, (uint16_t)proxy);
    psx_mod_write_half(FX_STATE, 0xC000u);
    ev(0xC000u, fx, amount, proxy);
    return 1;
}

/* equip_table_lookup(key, card): answer an edited equip's type list through
 * the scratch group, and set the bonus for the equip being resolved. */
static void hook_equip(struct CPUState *cpu, uint32_t address)
{
    (void)address;
    if (!s_stock_ok) return;
    const int a = (int)cpu->gpr[4], b = (int)cpu->gpr[5];
    const int ta = card_type(a), tb = card_type(b);
    const Fx *fa = (ta == 23) ? fx_of(a) : NULL;
    const Fx *fb = (tb == 23) ? fx_of(b) : NULL;
    /* the scratch group: only touch it when the streamed table has one */
    if (s_equip_override) {
        const uint16_t k0 = psx_mod_read_half(EQUIP_RAM), c0 = psx_mod_read_half(EQUIP_RAM + 2);
        if (c0 == 1 && (k0 == SCRATCH_KEY || k0 == s_scratch_key)) {
            int yes = 0;
            if (fa && tb >= 0 && tb < 20) {
                const uint32_t m = fa->cfg.equip_types;
                const int attr = psx_mod_read_byte(psx_card_extend_aux_base() + (uint32_t)b) >> 4;
                yes = (m & PSX_CARD_PACK_EQUIP_ALL) || (m & (1u << tb)) || (attr < 6 && (m & PSX_CARD_PACK_EQUIP_ATTR_BIT(attr)));
            }
            const uint16_t key = yes ? (uint16_t)a : SCRATCH_KEY;
            psx_mod_write_half(EQUIP_RAM, key);
            psx_mod_write_half(EQUIP_RAM + 4, yes ? (uint16_t)b : 0);
            s_scratch_key = key;
            ev(HOOK_EQUIP, a, b, yes);
        }
    }
    /* the bonus for whichever of the pair is an equip */
    const Fx *fe = fa ? fa : fb;
    const int eid = fa ? a : (fb ? b : 0);
    if (fe && fe->cfg.equip_bonus >= 0) { s_eq_active = 1; s_eq_bonus = fe->cfg.equip_bonus; s_eq_card = eid; }
    else if (ta == 23 || tb == 23) { s_eq_active = 0; s_eq_card = ta == 23 ? a : b; }
    eq_apply();
}

/* Wrap `text` into "|"-separated lines of at most 20 columns, appended to out. */
static void wrap_append(char *out, unsigned cap, const char *text)
{
    unsigned n = (unsigned)strlen(out);
    const char *p = text;
    int col = 0;
    if (n && out[n - 1] != '|' && n + 1 < cap) { out[n++] = '|'; out[n] = 0; }
    while (*p && n + 2 < cap) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *e = p; while (*e && *e != ' ') e++;
        const int wl = (int)(e - p);
        if (col && col + 1 + wl > 20) { out[n++] = '|'; col = 0; }
        else if (col) { out[n++] = ' '; col++; }
        for (const char *q = p; q < e && n + 1 < cap; q++) { out[n++] = *q; col++; }
        p = e;
    }
    out[n] = 0;
}

#define FXCAP 512
static void psx_card_effects_describe_effect_only(const PsxCardPack *c, char *out, unsigned cap);
static int psx_card_effects_describe_body(const PsxCardPack *c, char *out, unsigned cap);

static void psx_card_effects_describe_effect_only(const PsxCardPack *c, char *out, unsigned cap)
{
    char t[256]; t[0] = 0;
    const int amt = c->amount;
    switch (c->effect) {
    case PSX_CARD_FX_NONE:         snprintf(t, sizeof t, "No effect."); break;
    case PSX_CARD_FX_HEAL:         snprintf(t, sizeof t, "Restores %d of your LP.", amt >= 0 ? amt : 500); break;
    case PSX_CARD_FX_DAMAGE:       snprintf(t, sizeof t, "Inflicts %d damage to the opponent.", amt >= 0 ? amt : 500); break;
    case PSX_CARD_FX_DESTROY_TYPE: snprintf(t, sizeof t, "Destroys all %s monsters on the opponent's field.", psx_card_packs_type_name(c->target >= 0 ? c->target : 3)); break;
    case PSX_CARD_FX_DESTROY_ATK:  snprintf(t, sizeof t, "Destroys the opponent's monsters with %d ATK or more.", amt >= 0 ? amt : 1500); break;
    case PSX_CARD_FX_RAIGEKI:      snprintf(t, sizeof t, "Destroys all monsters on the opponent's field."); break;
    case PSX_CARD_FX_DARK_HOLE:    snprintf(t, sizeof t, "Destroys every card on both fields."); break;
    case PSX_CARD_FX_DRAGON_JAR:   snprintf(t, sizeof t, "Destroys all Dragon monsters on the opponent's field."); break;
    case PSX_CARD_FX_STOP_DEFENSE: snprintf(t, sizeof t, "Switches the opponent's defenders to attack position."); break;
    case PSX_CARD_FX_FLIP:         snprintf(t, sizeof t, "Turns every face-down monster face up."); break;
    case PSX_CARD_FX_WEAKEN:
        if (amt < 0 && amt != -1) snprintf(t, sizeof t, "Raises the ATK and DEF of the opponent's monsters by %d.", -amt);
        else snprintf(t, sizeof t, "Lowers the ATK and DEF of the opponent's monsters by %d.", amt >= 0 ? amt : 500);
        break;
    case PSX_CARD_FX_SWORDS:       snprintf(t, sizeof t, "Flips the opponent's monsters face up and stops their attacks for 3 turns."); break;
    case PSX_CARD_FX_CURSEBREAKER: snprintf(t, sizeof t, "Removes the curses on your monsters."); break;
    case PSX_CARD_FX_HARPIE:       snprintf(t, sizeof t, "Destroys every magic and trap card on the opponent's field."); break;
    case PSX_CARD_FX_FIELD:        snprintf(t, sizeof t, "Changes the field to %s.", psx_card_packs_terrain_name(c->terrain >= 1 ? c->terrain : 1)); break;
    case PSX_CARD_FX_RITUAL:
        if (c->ritual_set) snprintf(t, sizeof t, "Offer monsters %d, %d and %d on your field to summon %s.", c->ritual_mat[0], c->ritual_mat[1], c->ritual_mat[2], psx_card_db_name(c->ritual_result));
        else snprintf(t, sizeof t, "A ritual summon.");
        break;
    case PSX_CARD_FX_GAMBLE:
        snprintf(t, sizeof t, "Flip a coin. Heads: destroy all the opponent's monsters. Tails: destroy all your own monsters and lose LP equal to half their total ATK.");
        break;
    case PSX_CARD_FX_DESTROY_STRONGEST: snprintf(t, sizeof t, "Destroys the opponent's strongest monster."); break;
    case PSX_CARD_FX_LOSE_LP: snprintf(t, sizeof t, "You lose %d LP.", amt >= 0 ? amt : 500); break;
    case PSX_CARD_FX_GAMBLE_LP: snprintf(t, sizeof t, "Flip a coin. Tails: you lose half your LP."); break;
    case PSX_CARD_FX_DESTROY_OWN: snprintf(t, sizeof t, "Destroys all your own monsters."); break;
    case PSX_CARD_FX_DESTROY_OWN_LP: snprintf(t, sizeof t, "Destroys all your own monsters; you lose LP equal to half their total ATK."); break;
    default: break;
    }
    if (t[0]) wrap_append(out, cap, t);
}

int psx_card_effects_describe(const PsxCardPack *c, char *out, unsigned cap)
{
    out[0] = 0;
    if (!c) return 0;
    char body[FXCAP * 2]; body[0] = 0;
    const int ok = psx_card_effects_describe_body(c, body, sizeof body);
    if (!ok) return 0;
    /* "Effect:" leads, then the wording re-wrapped after it */
    char flat[FXCAP * 2]; unsigned n = 0;
    for (const char *q = body; *q && n + 1 < sizeof flat; q++) flat[n++] = (*q == '|') ? ' ' : *q;
    flat[n] = 0;
    char lead[FXCAP * 2]; snprintf(lead, sizeof lead, "Effect: %s", flat);
    wrap_append(out, cap, lead);
    return out[0] != 0;
}

static int psx_card_effects_describe_body(const PsxCardPack *c, char *out, unsigned cap)
{
    out[0] = 0;
    psx_card_effects_describe_effect_only(c, out, cap);
    if (c->equip_bonus >= 0 || c->equips_set || c->equip_types) {
        char e[200] = "";
        if (c->equips_set || c->equip_types) {
            char list[512]; psx_card_packs_format_equips(c, list, sizeof list);
            snprintf(e, sizeof e, "Equip to %s: ATK and DEF +%d.", list, c->equip_bonus >= 0 ? c->equip_bonus : 500);
        } else snprintf(e, sizeof e, "Raises the ATK and DEF of the equipped monster by %d.", c->equip_bonus);
        wrap_append(out, cap, e);
    }
    if (c->trap_atk_max >= 0) {
        char e[120]; snprintf(e, sizeof e, "Destroys an attacking monster with %d ATK or less.", c->trap_atk_max);
        wrap_append(out, cap, e);
    }
    if (c->boost_set) {
        char list[512]; psx_card_packs_format_boost(c, list, sizeof list);
        char e[560]; snprintf(e, sizeof e, "Field: %s.", list);
        wrap_append(out, cap, e);
    }
    /* monster effects */
    {
        static const char *const when[6] = { "When summoned face-up", "When flipped face-up", "When destroyed", "When it attacks", "Each of your turns", "Each of the opponent's turns" };
        const PsxCardTrigger *trigs[6] = { &c->on_summon, &c->on_flip, &c->on_death, &c->on_attack, &c->each_turn, &c->opp_turn };
        for (int i = 0; i < 6; i++) {
            if (trigs[i]->n <= 0) continue;
            char e[FXCAP * 2]; unsigned n = (unsigned)snprintf(e, sizeof e, "%s", when[i]);
            for (int k = 0; k < trigs[i]->n; k++) {
                const PsxCardFxBranch *b = &trigs[i]->b[k];
                PsxCardPack tmp; memset(&tmp, 0, sizeof tmp);
                tmp.effect = b->fx; tmp.amount = b->amount; tmp.target = b->target; tmp.terrain = b->terrain;
                tmp.equip_bonus = -1; tmp.trap_atk_max = -1;
                char inner[FXCAP]; inner[0] = 0;
                psx_card_effects_describe_effect_only(&tmp, inner, sizeof inner);
                for (char *q = inner; *q; q++) if (*q == '|') *q = ' ';
                if (inner[0] >= 'A' && inner[0] <= 'Z') inner[0] = (char)(inner[0] + 32);
                { size_t l = strlen(inner); while (l && (inner[l - 1] == '.' || inner[l - 1] == ' ')) inner[--l] = 0; }
                const char *lead = b->is_else ? "; otherwise" : (b->chance < 100 ? (k ? "; also" : "") : (k ? "; also" : ""));
                if (b->is_else) n += (unsigned)snprintf(e + n, sizeof e - n, "%s: %s", lead, inner);
                else if (b->chance < 100) n += (unsigned)snprintf(e + n, sizeof e - n, "%s, %d%% of the time: %s", lead, b->chance, inner);
                else n += (unsigned)snprintf(e + n, sizeof e - n, "%s: %s", lead, inner);
            }
            if (n + 1 < sizeof e) { e[n++] = '.'; e[n] = 0; }
            wrap_append(out, cap, e);
        }
        switch (c->battle) {
        case PSX_CARD_BATTLE_INDESTRUCTIBLE: wrap_append(out, cap, "Cannot be destroyed in battle."); break;
        case PSX_CARD_BATTLE_MUTUAL: wrap_append(out, cap, "Destroys itself and any monster it battles."); break;
        case PSX_CARD_BATTLE_SLAYER: wrap_append(out, cap, "Destroys any monster it battles."); break;
        default: break;
        }
        if (c->bonus_n > 0) {
            char e[400] = ""; unsigned n = 0;
            for (int i = 0; i < c->bonus_n && n < sizeof e; i++) {
                const PsxCardBonus *b = &c->bonus[i];
                if (b->filter < 0) n += (unsigned)snprintf(e + n, sizeof e - n, "%s%+d on the field", n ? ", " : "ATK and DEF ", b->amount);
                else n += (unsigned)snprintf(e + n, sizeof e - n, "%s%+d per %s", n ? ", " : "ATK and DEF ", b->amount, psx_card_packs_filter_name(b->filter, b->enemy));
            }
            snprintf(e + n, sizeof e - n, ".");
            wrap_append(out, cap, e);
        }
        if (c->immune > 0) {
            wrap_append(out, cap, c->immune == 3 ? "Unaffected by traps and destruction magic." : c->immune == 1 ? "Unaffected by traps." : "Unaffected by destruction magic.");
        }
    }
    return out[0] != 0;
}

/* ---- debug -------------------------------------------------------------------- */
int psx_card_effects_state_json(char *out, unsigned cap)
{
    int n_fx = 0;
    for (int id = 1; id <= CARD_COUNT; id++) if (fx_of(id)) n_fx++;
    unsigned n = (unsigned)snprintf(out, cap,
        "\"ready\":%d,\"cards\":%d,\"equip_override\":%d,\"equip_bytes\":%d,\"equip_dropped\":%d,"
        "\"ritual_override\":%d,\"ritual_records\":%d,\"hold\":{\"active\":%d,\"kind\":%d,\"since\":%u},"
        "\"equip_bonus\":{\"active\":%d,\"bonus\":%d,\"card\":%d,\"dirty\":%d},\"scratch_key\":%u,\"frame\":%u,"
        "\"fx_state\":%u,\"events\":[",
        s_stock_ok, n_fx, s_equip_override, s_equip_bytes, s_equip_dropped, s_ritual_override, s_ritual_records,
        s_hold.active, s_hold.kind, s_hold.since, s_eq_active, s_eq_bonus, s_eq_card, s_eq_dirty, s_scratch_key, s_frame,
        s_stock_ok ? psx_mod_read_half(FX_STATE) : 0);
    const unsigned first = s_ev_n > 16u ? s_ev_n - 16u : 0u;
    for (unsigned i = first; i < s_ev_n && n + 80 < cap; i++) {
        const Ev *e = &s_ev[i & 15u];
        n += (unsigned)snprintf(out + n, cap - n, "%s{\"frame\":%u,\"at\":\"%08X\",\"a\":%d,\"b\":%d,\"out\":%d}",
                                i > first ? "," : "", e->frame, e->at, e->a, e->b, e->out);
    }
    n += (unsigned)snprintf(out + n, cap - n, "],\"ids\":[");
    { int first_id = 1; for (int id = 1; id <= CARD_COUNT && n + 8 < cap; id++) if (fx_of(id)) { n += (unsigned)snprintf(out + n, cap - n, "%s%d", first_id ? "" : ",", id); first_id = 0; } }
    n += (unsigned)snprintf(out + n, cap - n, "]");
    return n < cap;
}

PSX_MOD_CONSTRUCTOR(psx_card_effects_install)
{
    (void)psx_mod_register_function_entry_plugin("card_effects_magic", HOOK_MAGIC, hook_magic);
    (void)psx_mod_register_function_entry_plugin("card_effects_equip", HOOK_EQUIP, hook_equip);
    (void)psx_game_add_frame_hook(tick);
}
