/* psx_monster_effects.c -- see psx_monster_effects.h.
 *
 * Forbidden Memories' monsters have no effects; every one of these is made
 * from function-entry hooks and per-frame writes, nothing injected:
 *
 * BATTLE (indestructible / mutual / slayer). State 3 of the attack action
 *   decides the outcome words gp+0x2A8 (attacker) / gp+0x2A9 (defender):
 *   -1 destroyed, 0 untouched, 1 survives. The action's entry hook, on the
 *   first frame of state 4, rewrites them; states 6..10 then destroy or keep
 *   the cards, popups included. LP damage was already dealt in state 3, so
 *   an indestructible monster still costs its owner the difference. When the
 *   player watched the 3D battle scene the game takes a second path: state 5
 *   writes the rows to destroy to gp+0x300/0x301 and action 11 destroys
 *   them after the scene; that action's entry hook rewrites the two bytes
 *   from the same decision.
 * FACE-DOWN: a monster set face-down (row flag 0x1000) has no effect: its
 *   summon trigger is simply lost (on_flip is the trigger for being turned
 *   face-up: attacked, flipped by the player, revealed by a spell), and its
 *   bonus and each-turn effect count only while face-up. Battle kinds and
 *   on_death still apply, since a battle turns it over first.
 * TRIGGERS (on_summon / on_death / on_attack / each_turn) queue a cast.
 *   A cast runs a magic effect class through the game's own per-frame
 *   effect driver (psx_card_effects_cast) the moment the duel is idle
 *   between actions; the driver freezes the action machine while the
 *   handler animates, exactly as a played spell does. The handlers act for
 *   the "playing side", so for a trigger owned by the other side the side
 *   byte, its record pointer and its row-map pointer are flipped for the
 *   duration of the cast and put back after.
 *   summon: the summon action's state 5 (row in gp+0x294). attack: the
 *   attack action's first frame (attacker object D_800E9EF0[0]). death: the
 *   battle decision above, a trap firing (state 10 with gp+0x322 set), or
 *   the destroy helper called while a magic effect runs. turn: the
 *   per-side turn counter at side+0x01, which action 2 increments.
 * BONUS: the row modifier +0x12 (the field equips and curses use) gets the
 *   difference between the wanted bonus (flat + per ally + per enemy) and
 *   what this row already carries, tracked per row and card.
 * IMMUNE: traps -- the trap check's entry hook zeroes the six ceilings for
 *   an immune attacker (the effects layer restores them next frame);
 *   magic -- the destroy helper's entry hook, while an effect runs, points
 *   a0 at a scratch row instead, so the popup plays and nothing dies. */

#include "psx_monster_effects.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cpu_state.h"
#include "mod_plugins.h"
#include "psx_game_hooks.h"
#include "psx_card_db.h"
#include "psx_card_packs.h"
#include "psx_card_effects.h"
#include "psx_lp_popup.h"

#define CARD_COUNT 722

/* ---- guest addresses (gp = 0x8009AF08) ---------------------------------- */
#define ROWS          0x801A7AD8u
#define ROW_STRIDE    0x1Cu
#define SIDES         0x800E9FF0u     /* 2 x 0x20: +1 turn counter, +0x14 LP, +0x16 max LP */
#define ROWMAP        0x800907D8u     /* u8[2][20] */
#define BATTLE_OBJS   0x800E9EF0u     /* Obj*[4]: attacker, defender, gates */
#define SIDE_BYTE     0x8009B1D5u     /* D_8009B1D5, playing side */
#define SIDE_REC_PTR  0x8009B1C8u     /* D_8009B1C8 -> sides[S] */
#define ROWMAP_PTR    0x8009B22Cu     /* D_8009B22C -> ROWMAP + S*20 */
#define ACTION        0x8009B23Au     /* u16: low nibble action, 0x8000 entered */
#define STATE_BYTE    0x8009B174u     /* u8: low nibble state, 0x80 initialised */
#define MODE_BYTE     0x8009B26Cu     /* 0xC3 = duel screen */
#define FX_STATE      0x8009B220u     /* u16 magic effect flags */
#define DIALOG_ARMED  0x8009B164u
#define BUSY_COUNT    0x8009B162u
#define OUT_ATTACKER  0x8009B1B0u     /* s8 gp+0x2A8 */
#define OUT_DEFENDER  0x8009B1B1u     /* s8 gp+0x2A9 */
#define DEST_ROW      0x8009B19Cu     /* u8 gp+0x294: summon destination row */
#define SCENE_FLAG    0x8009B229u     /* u8 gp+0x321: 1 = the 3D battle scene path */
#define KILL_ROW_A    0x8009B208u     /* u8 gp+0x300: attacker row to destroy after the scene, 0xFF none */
#define KILL_ROW_D    0x8009B209u
#define TRAP_ID       0x8009B22Au     /* u16 gp+0x322 */
#define TRAP_TAB      0x8009AF24u     /* u8[6] ceilings */

#define HOOK_SUMMON   0x8001B170u
#define HOOK_ATTACK   0x8001F560u
#define HOOK_ACTION11 0x8001825Cu
#define HOOK_TRAP     0x8001F0D0u
#define HOOK_DESTROY  0x80024954u
#define HOOK_AFTER_FX 0x8002892Cu     /* runs after the effect driver, before the action dispatch */

/* ---- state ---------------------------------------------------------------- */
typedef struct { int present; PsxCardPack cfg; } Mfx;
static Mfx     *s_m[CARD_COUNT + 1];
static unsigned s_seen_gen = (unsigned)-1;
static unsigned s_frame;
static uint32_t s_scratch_row;

typedef struct { int side, fx, amount, target, terrain, card; } Cast;
static Cast s_q[24];
static int  s_qh, s_qt;
static int  s_casting, s_flipped, s_orig_side;
static unsigned s_cast_frame;
static int  s_casts_done;

typedef struct { int aid, arow, did, drow, decided, a_dead, d_dead, pathA; } Battle;
static Battle s_bat;

typedef struct { int id, applied; } Bonus;
static Bonus s_bonus[30];
static int   s_facedown[30];      /* card id of a monster set face-down in this row (its flip is still to come) */
static uint8_t s_turn_last[2];
static int s_in_duel;

typedef struct { unsigned frame; const char *what; int a, b, c; } Ev;
static Ev s_ev[16]; static unsigned s_ev_n;
static void ev(const char *what, int a, int b, int c) { Ev *e = &s_ev[s_ev_n++ & 15u]; e->frame = s_frame; e->what = what; e->a = a; e->b = b; e->c = c; }

static const Mfx *mfx(int id) { if (id < 1 || id > CARD_COUNT) return NULL; const Mfx *m = s_m[id]; return (m && m->present) ? m : NULL; }
int psx_card_effects_monster_has_effect(int id) { return mfx(id) != NULL; }

static int row_id(int row) { return (int)(int16_t)psx_mod_read_half(ROWS + (uint32_t)row * ROW_STRIDE + 0xC); }
static unsigned row_flags(int row) { return psx_mod_read_half(ROWS + (uint32_t)row * ROW_STRIDE + 0x16); }
static int side_of_row(int row) { return row >= 15; }
static int is_monster_row(int row) { const int r = row % 15; return r >= 5 && r <= 9; }
static int playing_side(void) { return psx_mod_read_byte(SIDE_BYTE) & 1; }

static void enqueue_raw(int side, int card, int fx, int amount, int target, int terrain)
{
    if (((s_qt + 1) % 24) == s_qh) return;
    Cast *c = &s_q[s_qt]; s_qt = (s_qt + 1) % 24;
    c->side = side; c->card = card; c->fx = fx; c->amount = amount; c->target = target; c->terrain = terrain;
    ev("queue", card, fx, side);
}

/* Time Wizard's coin. Heads: Raigeki as the owner (their monsters go).
 * Tails: Raigeki cast as the opponent, so the owner's own monsters go, then
 * their total ATK as damage to the owner: the burn popup for what its
 * byte can show (2550) and a direct LP write for any remainder. */
static void gamble(int side, int card)
{
    const unsigned r = (unsigned)rand() ^ s_frame;
    const int heads = (r & 1u) != 0;
    if (heads) { enqueue_raw(side, card, PSX_CARD_FX_RAIGEKI, -1, -1, -1); ev("coin_heads", card, side, 0); return; }
    int total = 0;
    for (int r5 = 5; r5 <= 9; r5++) {
        const int row = 15 * side + r5;
        if (!(row_flags(row) & 0x8000u)) continue;
        const uint32_t at = ROWS + (uint32_t)row * ROW_STRIDE;
        int atk = (int)(int16_t)psx_mod_read_half(at + 0xE) + (int)(int16_t)psx_mod_read_half(at + 0x12) + (int)(int16_t)psx_mod_read_half(at + 0x14);
        if (atk < 0) atk = 0; if (atk > 9999) atk = 9999;
        total += atk;
    }
    enqueue_raw(side ^ 1, card, PSX_CARD_FX_RAIGEKI, -1, -1, -1);
    const int shown = total > 2550 ? 2550 : total;
    if (shown > 0) enqueue_raw(side ^ 1, card, PSX_CARD_FX_DAMAGE, shown, -1, -1);
    if (total > shown) {
        const uint32_t lpat = SIDES + (uint32_t)side * 0x20u + 0x14u;
        int lp = psx_mod_read_half(lpat) - (total - shown);
        if (lp < 0) lp = 0;
        psx_mod_write_half(lpat, (uint16_t)lp);
    }
    ev("coin_tails", card, side, total);
}

static void enqueue_fx(int side, int card, int fx, int amount, int target, int terrain)
{
    if (fx < 0 || fx == PSX_CARD_FX_RITUAL || fx == PSX_CARD_FX_NONE) return;
    if (fx == PSX_CARD_FX_GAMBLE) { gamble(side, card); return; }
    if (fx == PSX_CARD_FX_DESTROY_OWN) { enqueue_raw(side ^ 1, card, PSX_CARD_FX_RAIGEKI, -1, -1, -1); return; }
    enqueue_raw(side, card, fx, amount, target, terrain);
}

/* A trigger's branches in order: every plain branch rolls its own chance,
 * an "else" branch fires only when the branch before it did not. */
static void enqueue(int side, int card, const PsxCardTrigger *t)
{
    if (!t || t->n <= 0) return;
    int prev_failed = 0;
    for (int k = 0; k < t->n && k < PSX_CARD_BRANCHES; k++) {
        const PsxCardFxBranch *b = &t->b[k];
        int fire;
        if (b->is_else) fire = prev_failed;
        else { const int roll = (int)(((unsigned)rand() ^ s_frame) % 100u); fire = roll < b->chance; }
        ev("branch", card, k, fire);
        if (fire) enqueue_fx(side, card, b->fx, b->amount, b->target, b->terrain);
        prev_failed = !fire;
    }
}


static void rebuild(void)
{
    for (int id = 1; id <= CARD_COUNT; id++) {
        PsxCardPack c;
        const int present = psx_card_packs_get(id, &c) && psx_card_packs_has_monster_effect(&c);
        if (present) {
            if (!s_m[id]) s_m[id] = (Mfx *)calloc(1, sizeof(Mfx));
            if (!s_m[id]) continue;
            s_m[id]->present = 1; s_m[id]->cfg = c;
        } else if (s_m[id]) s_m[id]->present = 0;
    }
}

/* ---- idle / casts --------------------------------------------------------------- */
static int duel_idle(void)
{
    if (psx_mod_read_byte(MODE_BYTE) != 0xC3) return 0;
    if (psx_mod_read_half(FX_STATE) != 0) return 0;
    if (psx_mod_read_byte(DIALOG_ARMED) != 0) return 0;
    if (psx_mod_read_half(BUSY_COUNT) != 0) return 0;
    if (psx_card_effects_hold_active()) return 0;
    const unsigned act = psx_mod_read_half(ACTION);
    if (!(act & 0x8000u)) return 0;
    const unsigned a = act & 0xFu;
    return a == 5 || a == 4;
}

static void flip_side(int side)
{
    s_orig_side = playing_side();
    if (side == s_orig_side) { s_flipped = 0; return; }
    psx_mod_write_byte(SIDE_BYTE, (uint8_t)side);
    psx_mod_write_word(SIDE_REC_PTR, SIDES + (uint32_t)side * 0x20u);
    psx_mod_write_word(ROWMAP_PTR, ROWMAP + (uint32_t)side * 20u);
    s_flipped = 1;
}
static void unflip(void)
{
    if (!s_flipped) return;
    psx_mod_write_byte(SIDE_BYTE, (uint8_t)s_orig_side);
    psx_mod_write_word(SIDE_REC_PTR, SIDES + (uint32_t)s_orig_side * 0x20u);
    psx_mod_write_word(ROWMAP_PTR, ROWMAP + (uint32_t)s_orig_side * 20u);
    s_flipped = 0;
}

static void casts_tick(void)
{
    if (s_casting) {
        const unsigned st = psx_mod_read_half(FX_STATE);
        if ((s_frame > s_cast_frame + 2 && st == 0) || s_frame > s_cast_frame + 900 || psx_mod_read_byte(MODE_BYTE) != 0xC3) {
            unflip(); s_casting = 0; s_casts_done++;
            ev("cast_done", (int)st, 0, 0);
        }
        return;
    }
    if (s_qh == s_qt || !duel_idle()) return;
    const Cast c = s_q[s_qh]; s_qh = (s_qh + 1) % 24;
    if (c.fx == PSX_CARD_FX_LOSE_LP || c.fx == PSX_CARD_FX_GAMBLE_LP) {
        /* the owner's own LP, no handler needed: direct write and the popup */
        const uint32_t lpat = SIDES + (uint32_t)c.side * 0x20u + 0x14u;
        const int lp = psx_mod_read_half(lpat);
        int loss = 0;
        if (c.fx == PSX_CARD_FX_LOSE_LP) loss = c.amount >= 0 ? c.amount : 500;
        else if (((unsigned)rand() ^ s_frame) & 1u) loss = lp / 2;
        if (loss > lp) loss = lp;
        if (loss > 0) { psx_mod_write_half(lpat, (uint16_t)(lp - loss)); psx_lp_popup_show(loss, 0); }
        ev("lose_lp", c.card, loss, c.side);
        return;
    }
    flip_side(c.side);
    if (!psx_card_effects_cast(c.fx, c.amount, c.target, c.terrain)) { unflip(); return; }
    s_casting = 1; s_cast_frame = s_frame;
    ev("cast", c.card, c.fx, c.side);
}

/* ---- bonuses -------------------------------------------------------------------- */
/* How many of side s's field monsters a filter counts: any monster, or
 * (identity known only face-up) that card / that type. `self` is left out. */
static int count_matching(int s, int filter, int self_row)
{
    int n = 0;
    if (filter == PSX_CARD_PACK_FILTER_HAND) {
        for (int r = 0; r <= 4; r++) if (row_flags(15 * s + r) & 0x8000u) n++;
        return n;
    }
    for (int r = 5; r <= 9; r++) {
        const int row = 15 * s + r;
        if (row == self_row) continue;
        const unsigned fl = row_flags(row);
        if (!(fl & 0x8000u)) continue;
        if (filter <= 0) { n++; continue; }
        if (fl & 0x1000u) continue;
        const int id = row_id(row);
        if (filter >= PSX_CARD_PACK_FILTER_TYPE) {
            const uint32_t w = psx_mod_read_word(0x801D4244u + (uint32_t)(id - 1) * 4u);
            if ((int)((w >> 26) & 0x1Fu) == filter - PSX_CARD_PACK_FILTER_TYPE) n++;
        } else if (id == filter) n++;
    }
    return n;
}

static void bonus_tick(void)
{
    for (int row = 0; row < 30; row++) {
        if (!is_monster_row(row)) continue;
        const unsigned fl = row_flags(row);
        const int id = row_id(row);
        Bonus *b = &s_bonus[row];
        if (fl == 0 || id != b->id) { b->id = id; b->applied = 0; s_facedown[row] = 0; }
        if (!(fl & 0x8000u)) continue;
        const Mfx *m = mfx(id);
        /* turned face-up (flipped by the player, attacked, revealed): the flip trigger */
        if (s_facedown[row] == id && !(fl & 0x1000u)) {
            s_facedown[row] = 0;
            if (m && m->cfg.on_flip.n > 0) { ev("flip", id, row, side_of_row(row)); enqueue(side_of_row(row), id, &m->cfg.on_flip); }
        }
        int want = 0;
        if (m && !(fl & 0x1000u)) {
            const PsxCardPack *c = &m->cfg;
            const int s = side_of_row(row);
            if (c->bonus_flat != PSX_CARD_PACK_BOOST_UNSET) want += c->bonus_flat;
            if (c->bonus_ally != PSX_CARD_PACK_BOOST_UNSET) want += c->bonus_ally * count_matching(s, c->bonus_ally_filter, row);
            if (c->bonus_enemy != PSX_CARD_PACK_BOOST_UNSET) want += c->bonus_enemy * count_matching(s ^ 1, c->bonus_enemy_filter, -1);
        }
        if (want != b->applied) {
            const uint32_t at = ROWS + (uint32_t)row * ROW_STRIDE + 0x12;
            const int cur = (int)(int16_t)psx_mod_read_half(at);
            psx_mod_write_half(at, (uint16_t)(int16_t)(cur + want - b->applied));
            b->applied = want;
        }
    }
}

/* ---- turns -------------------------------------------------------------------- */
static void turn_tick(void)
{
    for (int s = 0; s < 2; s++) {
        const uint8_t t = psx_mod_read_byte(SIDES + (uint32_t)s * 0x20u + 1u);
        if (t == s_turn_last[s]) continue;
        s_turn_last[s] = t;
        if (!s_in_duel) continue;
        for (int r = 5; r <= 9; r++) {
            const int row = 15 * s + r;
            const unsigned fl = row_flags(row);
            if (!(fl & 0x8000u) || (fl & 0x1000u)) continue;     /* on the field and face-up */
            const Mfx *m = mfx(row_id(row));
            if (m && m->cfg.each_turn.n > 0) enqueue(s, row_id(row), &m->cfg.each_turn);
        }
    }
}

static void tick(void)
{
    if (!psx_mod_game_started()) return;
    s_frame++;
    const unsigned gen = psx_card_packs_generation();
    if (gen != s_seen_gen) { s_seen_gen = gen; rebuild(); }
    if (!s_scratch_row) {
        s_scratch_row = psx_mod_alloc_guest_memory(0x20, 4);
        if (s_scratch_row) for (uint32_t i = 0; i < 0x20; i += 4) psx_mod_write_word(s_scratch_row + i, 0);
    }
    const int in_duel = psx_mod_read_byte(MODE_BYTE) == 0xC3 && psx_card_db_ready();
    if (!in_duel) {
        if (s_in_duel) { s_in_duel = 0; s_qh = s_qt = 0; s_casting = 0; s_flipped = 0; memset(&s_bat, 0, sizeof s_bat); memset(s_bonus, 0, sizeof s_bonus); memset(s_facedown, 0, sizeof s_facedown); }
        /* the 3D battle scene is mode 1: keep the pending path-A decision */
        if (psx_mod_read_byte(MODE_BYTE) != 1) { s_turn_last[0] = psx_mod_read_byte(SIDES + 1u); s_turn_last[1] = psx_mod_read_byte(SIDES + 0x21u); }
        return;
    }
    if (!s_in_duel) { s_in_duel = 1; s_turn_last[0] = psx_mod_read_byte(SIDES + 1u); s_turn_last[1] = psx_mod_read_byte(SIDES + 0x21u); }
    turn_tick();
    bonus_tick();
    casts_tick();
}

/* ---- hooks ------------------------------------------------------------------------ */
static void hook_summon(struct CPUState *cpu, uint32_t address)
{
    (void)cpu; (void)address;
    static int last_row = -1, last_id = -1;
    const unsigned st = psx_mod_read_byte(STATE_BYTE);
    if ((st & 0xFu) != 5) { if ((st & 0xFu) < 5) last_row = -1; return; }
    const int row = psx_mod_read_byte(DEST_ROW);
    if (row < 0 || row >= 30) return;
    const int id = row_id(row);
    if (row == last_row && id == last_id) return;
    last_row = row; last_id = id;
    const Mfx *m = mfx(id);
    const int facedown = (row_flags(row) & 0x1000u) != 0;
    ev("summon", id, row, facedown ? 2 : playing_side());
    if (!is_monster_row(row)) return;
    if (facedown) { s_facedown[row] = id; return; }      /* set face-down: the summon effect is forfeited */
    if (m && m->cfg.on_summon.n > 0) enqueue(side_of_row(row), id, &m->cfg.on_summon);
}

static int battle_kind(int id) { const Mfx *m = mfx(id); return (m && m->cfg.battle > 0) ? m->cfg.battle : 0; }

static void hook_attack(struct CPUState *cpu, uint32_t address)
{
    (void)cpu; (void)address;
    const unsigned act = psx_mod_read_half(ACTION);
    const unsigned st = psx_mod_read_byte(STATE_BYTE);
    if (!(act & 0x8000u)) {
        /* an attack was declared: who fights whom */
        memset(&s_bat, 0, sizeof s_bat);
        const uint32_t a = psx_mod_read_word(BATTLE_OBJS), d = psx_mod_read_word(BATTLE_OBJS + 4);
        s_bat.arow = a ? psx_mod_read_byte(a + 0x6A) : -1;
        s_bat.aid = s_bat.arow >= 0 ? row_id(s_bat.arow) : 0;
        s_bat.drow = d ? psx_mod_read_byte(d + 0x6A) : -1;
        s_bat.did = s_bat.drow >= 0 ? row_id(s_bat.drow) : 0;
        ev("attack", s_bat.aid, s_bat.did, playing_side());
        const Mfx *m = mfx(s_bat.aid);
        if (m && m->cfg.on_attack.n > 0) enqueue(playing_side(), s_bat.aid, &m->cfg.on_attack);
        return;
    }
    if (s_bat.decided) return;
    const unsigned state = st & 0xFu;
    if (state == 10 && !(st & 0x80u)) {
        /* a trap fired: the attacker dies (Fake Trap 690 spares it) */
        const int trap = psx_mod_read_half(TRAP_ID);
        if (trap && trap != 690) {
            s_bat.decided = 1; s_bat.a_dead = 1;
            const Mfx *m = mfx(s_bat.aid);
            if (m && m->cfg.on_death.n > 0) enqueue(playing_side(), s_bat.aid, &m->cfg.on_death);
            ev("trapped", s_bat.aid, trap, 0);
        }
        return;
    }
    if (state != 4 || (st & 0x80u)) return;
    s_bat.decided = 1;
    const int a_out = (int)(int8_t)psx_mod_read_byte(OUT_ATTACKER);
    const int d_out = (int)(int8_t)psx_mod_read_byte(OUT_DEFENDER);
    int a_dead = a_out == -1, d_dead = s_bat.drow >= 0 && d_out == -1;
    const int ka = battle_kind(s_bat.aid), kd = s_bat.drow >= 0 ? battle_kind(s_bat.did) : 0;
    if (ka == PSX_CARD_BATTLE_MUTUAL) { a_dead = 1; if (s_bat.drow >= 0) d_dead = 1; }
    if (kd == PSX_CARD_BATTLE_MUTUAL) { d_dead = 1; a_dead = 1; }
    if (ka == PSX_CARD_BATTLE_SLAYER && s_bat.drow >= 0) d_dead = 1;
    if (kd == PSX_CARD_BATTLE_SLAYER) a_dead = 1;
    if (ka == PSX_CARD_BATTLE_INDESTRUCTIBLE) a_dead = 0;
    if (kd == PSX_CARD_BATTLE_INDESTRUCTIBLE) d_dead = 0;
    if (ka || kd) {
        psx_mod_write_byte(OUT_ATTACKER, (uint8_t)(int8_t)(a_dead ? -1 : (a_out == -1 ? 0 : a_out)));
        if (s_bat.drow >= 0) psx_mod_write_byte(OUT_DEFENDER, (uint8_t)(int8_t)(d_dead ? -1 : (d_out == -1 ? 0 : d_out)));
    }
    s_bat.a_dead = a_dead; s_bat.d_dead = d_dead;
    s_bat.pathA = psx_mod_read_byte(SCENE_FLAG) != 0;
    ev("battle", a_out * 10 + (a_dead ? 1 : 0), d_out * 10 + (d_dead ? 1 : 0), s_bat.pathA);
    const int side = playing_side();
    const Mfx *ma = mfx(s_bat.aid), *md = s_bat.drow >= 0 ? mfx(s_bat.did) : NULL;
    if (a_dead && ma && ma->cfg.on_death.n > 0) enqueue(side, s_bat.aid, &ma->cfg.on_death);
    if (d_dead && md && md->cfg.on_death.n > 0) enqueue(side ^ 1, s_bat.did, &md->cfg.on_death);
}

static void hook_action11(struct CPUState *cpu, uint32_t address)
{
    (void)cpu; (void)address;
    if (!s_bat.pathA || !s_bat.decided) return;
    if (psx_mod_read_half(ACTION) & 0x8000u) return;
    if (battle_kind(s_bat.aid) || (s_bat.drow >= 0 && battle_kind(s_bat.did))) {
        psx_mod_write_byte(KILL_ROW_A, (uint8_t)(s_bat.a_dead ? s_bat.arow : 0xFF));
        psx_mod_write_byte(KILL_ROW_D, (uint8_t)(s_bat.d_dead && s_bat.drow >= 0 ? s_bat.drow : 0xFF));
        ev("scene_rows", s_bat.a_dead ? s_bat.arow : -1, s_bat.d_dead ? s_bat.drow : -1, 0);
    }
    s_bat.pathA = 0;
}

static void hook_trap(struct CPUState *cpu, uint32_t address)
{
    (void)address;
    const uint32_t obj = cpu->gpr[4];
    if (!obj) return;
    const int row = psx_mod_read_byte(obj + 0x6A);
    if (row < 0 || row >= 30) return;
    const Mfx *m = mfx(row_id(row));
    if (m && m->cfg.immune > 0 && (m->cfg.immune & PSX_CARD_IMMUNE_TRAPS)) {
        for (int i = 0; i < 6; i++) psx_mod_write_byte(TRAP_TAB + (uint32_t)i, 0);
        ev("trap_immune", row_id(row), row, 0);
    }
}

static void hook_destroy(struct CPUState *cpu, uint32_t address)
{
    (void)address;
    const uint32_t a0 = cpu->gpr[4];
    if (a0 < ROWS || a0 >= ROWS + 30u * ROW_STRIDE) return;
    const int row = (int)((a0 - ROWS) / ROW_STRIDE);
    if (!is_monster_row(row)) return;
    const int id = row_id(row);
    const unsigned fl = row_flags(row);
    const int magic = (psx_mod_read_half(FX_STATE) & 0x8000u) != 0;
    const Mfx *m = mfx(id);
    if (!magic || !m) return;
    if ((m->cfg.immune > 0) && (m->cfg.immune & PSX_CARD_IMMUNE_MAGIC) && s_scratch_row) {
        cpu->gpr[4] = s_scratch_row;
        ev("magic_immune", id, row, 0);
        return;
    }
    if ((fl & 0x8000u) && m->cfg.on_death.n > 0) enqueue(side_of_row(row), id, &m->cfg.on_death);
}

/* The frame an effect finishes, the driver returns 0 and the action machine
 * dispatches in the same call; this runs in between, so the side goes back
 * before the field phase sees it. */
static void hook_after_fx(struct CPUState *cpu, uint32_t address)
{
    (void)cpu; (void)address;
    if (s_casting && s_flipped && psx_mod_read_half(FX_STATE) == 0) { unflip(); ev("unflip", 0, 0, 0); }
}

/* ---- debug ------------------------------------------------------------------------ */
int psx_monster_effects_state_json(char *out, unsigned cap)
{
    int n_m = 0;
    for (int id = 1; id <= CARD_COUNT; id++) if (mfx(id)) n_m++;
    unsigned n = (unsigned)snprintf(out, cap,
        "\"cards\":%d,\"in_duel\":%d,\"queue\":%d,\"casting\":%d,\"flipped\":%d,\"casts_done\":%d,\"idle\":%d,"
        "\"battle\":{\"aid\":%d,\"arow\":%d,\"did\":%d,\"drow\":%d,\"decided\":%d,\"a_dead\":%d,\"d_dead\":%d,\"pathA\":%d},\"events\":[",
        n_m, s_in_duel, (s_qt - s_qh + 24) % 24, s_casting, s_flipped, s_casts_done, s_in_duel ? duel_idle() : 0,
        s_bat.aid, s_bat.arow, s_bat.did, s_bat.drow, s_bat.decided, s_bat.a_dead, s_bat.d_dead, s_bat.pathA);
    const unsigned first = s_ev_n > 16u ? s_ev_n - 16u : 0u;
    for (unsigned i = first; i < s_ev_n && n + 80 < cap; i++) {
        const Ev *e = &s_ev[i & 15u];
        n += (unsigned)snprintf(out + n, cap - n, "%s{\"frame\":%u,\"what\":\"%s\",\"a\":%d,\"b\":%d,\"c\":%d}", i > first ? "," : "", e->frame, e->what, e->a, e->b, e->c);
    }
    n += (unsigned)snprintf(out + n, cap - n, "]");
    return n < cap;
}

PSX_MOD_CONSTRUCTOR(psx_monster_effects_install)
{
    (void)psx_mod_register_function_entry_plugin("monster_fx_summon",  HOOK_SUMMON,   hook_summon);
    (void)psx_mod_register_function_entry_plugin("monster_fx_attack",  HOOK_ATTACK,   hook_attack);
    (void)psx_mod_register_function_entry_plugin("monster_fx_scene",   HOOK_ACTION11, hook_action11);
    (void)psx_mod_register_function_entry_plugin("monster_fx_trap",    HOOK_TRAP,     hook_trap);
    (void)psx_mod_register_function_entry_plugin("monster_fx_destroy", HOOK_DESTROY,  hook_destroy);
    (void)psx_mod_register_function_entry_plugin("monster_fx_afterfx", HOOK_AFTER_FX, hook_after_fx);
    (void)psx_game_add_frame_hook(tick);
    srand((unsigned)time(NULL));
}
