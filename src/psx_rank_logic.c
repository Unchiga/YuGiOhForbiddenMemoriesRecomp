/* Live duel-rank logic -- see psx_rank_logic.h.
 *
 * The presentation half of this feature lives in psx_rank_meter.c, which knows
 * how to composite a badge, a letter and two digits into a canvas. This file is
 * the other half: the guest addresses, the score arithmetic the game itself
 * runs, and the rules for when the meter may appear. Keeping the two apart is
 * what lets the score be checked against the game's own answer without dragging
 * a renderer into the test.
 *
 * psx_rank_meter_debug / psx_rank_meter_fade_debug keep their names because the
 * debug server declares them by name; they report THIS file's state (mode,
 * anchor, fade), not the compositor's.
 */

#include "psx_rank_logic.h"

#include "psx_rank_meter.h"
#include "gpu.h"
#include "gpu.h"
#include "host_osd.h"
#include "mod_plugins.h"
#include "psx_game_hooks.h"
#include "psx_video_menu.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Defined in gpu.c: the general "watch for a sprite drawn through this CLUT"
 * facility. The constants that pick out THIS game's FIELD box are below. */
void gpu_sprite_watch(int clut_x, int clut_y, int u, int v);
void gpu_sprite_watch_occlusion(int x, int y, int w, int h);
int  gpu_sprite_watch_query(int max_age, int *x, int *y, int *occluded);
int  gpu_sprite_watch_brightness(void);

/* --- Live duel rank meter -------------------------------------------------
 *
 * Yu-Gi-Oh! Forbidden Memories grades every duel on a single 0-99 score and
 * shows it only on the RESULTS OF DUEL screen, after it is too late to steer.
 * The score is not reconstructed at the end though: the game keeps every input
 * counter live for the whole duel and func_80021598 merely sums them, so the
 * same arithmetic can be run every frame.
 *
 *     score = 50 + (s8)block[+0x00]              victory bonus, already points
 *     for kind in 0..9: score += lookup(kind, counter[kind])
 *
 *     lookup(kind, v):                           func_80021558
 *         p = 0x801798A8 + kind * 20             5 x (s16 threshold, s16 delta)
 *         while (v >= p->threshold) p += 4
 *         return p->delta
 *
 * Verified against the game's own answer: at duel end func_80021598 writes the
 * finished scores to 0x80179A04 (you) and 0x80179A08 (opponent), and a live
 * duel recomputed 86/84 against an oracle of 86/84 while the results screen
 * displayed POW / A Rank — 86 being in the 89-80 A-POW band.
 *
 * The coefficient table is NOT in the EXE image; it arrives with a disc load,
 * so it reads as zeros outside a duel and is validated rather than trusted. */
/* The duel HUD's FIELD box: five sprites drawn through this CLUT, spanning
 * x 12..68 / y 24..48 when the HUD is fully on screen. The meter anchors to it.
 * gpu_sprite_watch is the general facility; these constants are the game's. */
void gpu_sprite_watch(int clut_x, int clut_y, int u, int v);
void gpu_sprite_watch_occlusion(int x, int y, int w, int h);
int  gpu_sprite_watch_query(int max_age, int *x, int *y, int *occluded);
int  gpu_sprite_watch_brightness(void);
static const int PSX_FIELDBOX_CLUT_X = 720;
static const int PSX_FIELDBOX_CLUT_Y = 252;
/* Anchor on the box's RIGHT-hand cap specifically (u,v = 32,96 — an 8x24 piece
 * drawn at x=60 when the HUD is home). Anchoring on the leftmost piece, or on
 * the minimum x across the group, jitters: the game drops parts of the box from
 * the stream as they leave the screen during the slide-out. The right cap is
 * the last piece to go, so it tracks the whole tween. */
static const int PSX_FIELDBOX_U      = 32;
static const int PSX_FIELDBOX_V      = 96;
static const int PSX_FIELDBOX_CAP_W  = 8;
/* Purely aesthetic 1px lift, applied ON TOP of a correct alignment — not a
 * correction for one. The badge's top row sits exactly on the box's top edge
 * without it; this just gives the widget a little air. */
static const int PSX_RANK_LIFT_Y     = 1;
/* No vertical fudge factor: the widget's top is aligned to the FIELD box's top
 * as an IDENTITY (see the set_origin call below), which holds at every
 * magnification. A tuned offset does not — it was dialled in at one window size
 * and read wrong at others. */

static const uint32_t PSX_RANK_BLOCK  = 0x800E9FF0u;  /* you; +0x20 = opponent */
static const uint32_t PSX_RANK_TABLE  = 0x801798A8u;
static const uint32_t PSX_RANK_RESULT = 0x801799D8u;  /* +44 you, +48 opponent */
static const int      PSX_RANK_BASE   = 50;

static int g_rank_meter = PSX_VM_RANK_OFF;

/* Counter offsets within the block, with the game's own name for each (read off
 * the RESULTS OF DUEL screen, which the same routine populates). Only the ten
 * that carry a `kind` feed the score; the screen shows several more.
 *
 * `name` is what the results screen calls it; `tag` is the abbreviation the
 * meter draws. The OSD has one line, no wrapping, and its 8x8 glyphs land at
 * roughly 32 window pixels each, so about 26 characters reach the player before
 * the line runs off the edge — "EFFECTIVE ATTACKS" alone blew that budget and
 * the delta was clipped off the end. Longest tag is 10, which keeps the worst
 * case ("S-POW 99  PURE MAGIC -40") at 24. */
typedef struct {
    uint8_t off, width, kind;
    const char *name;
    const char *tag;
} RankTerm;

static const RankTerm PSX_RANK_TERMS[] = {
    { 0x01, 1, 0, "TURNS",             "TURNS" },
    { 0x02, 1, 1, "EFFECTIVE ATTACKS", "ATTACKS" },
    { 0x03, 1, 2, "DEFENSIVE WINS",    "DEF WINS" },
    { 0x04, 1, 3, "FACE-DOWN PLAYS",   "FACE-DOWN" },
    { 0x05, 1, 4, "PURE MAGIC",        "PURE MAGIC" },
    { 0x06, 1, 5, "TRIGGER TRAP",      "TRAPS" },
    { 0x08, 1, 8, "INITIATE FUSION",   "FUSIONS" },
    { 0x09, 1, 9, "EQUIP MAGIC",       "EQUIPS" },
    { 0x14, 2, 7, "REMAINING LP",      "LOW LP" },
    { 0x18, 1, 6, "CARDS USED",        "CARDS USED" },
};

static int rank_s16(uint32_t addr) { return (int16_t)psx_mod_read_half(addr); }

/* The table's last threshold is the 32767 sentinel the walk relies on to
 * terminate, so checking it (and monotonicity) both validates residency and
 * guarantees rank_lookup cannot run off the end. */
static bool rank_table_resident(void) {
    for (int kind = 0; kind < 10; kind++) {
        const uint32_t base = PSX_RANK_TABLE + (uint32_t)kind * 20u;
        int prev = -0x8000;
        for (int i = 0; i < 5; i++) {
            int thr = rank_s16(base + (uint32_t)i * 4u);
            if (thr < prev) return false;
            prev = thr;
        }
        if (prev != 32767) return false;
    }
    return true;
}

static int rank_lookup(int kind, int value) {
    const uint32_t base = PSX_RANK_TABLE + (uint32_t)kind * 20u;
    for (int i = 0; i < 5; i++) {
        if (value < rank_s16(base + (uint32_t)i * 4u))
            return rank_s16(base + (uint32_t)i * 4u + 2u);
    }
    return rank_s16(base + 4u * 4u + 2u);
}

static int rank_counter(uint32_t block, const RankTerm *t) {
    return (t->width == 2) ? rank_s16(block + t->off)
                          : (int)psx_mod_read_byte(block + t->off);
}

/* Worst term = the most negative delta; ties go to the earlier row so the
 * message does not flicker between two equally bad terms. */
static int rank_score(uint32_t block, const RankTerm **worst, int *worst_delta) {
    int score = PSX_RANK_BASE + (int8_t)psx_mod_read_byte(block);
    int wd = 0;
    const RankTerm *wt = NULL;
    for (size_t i = 0; i < sizeof(PSX_RANK_TERMS) / sizeof(PSX_RANK_TERMS[0]); i++) {
        const RankTerm *t = &PSX_RANK_TERMS[i];
        int d = rank_lookup(t->kind, rank_counter(block, t));
        score += d;
        if (d < wd) { wd = d; wt = t; }
    }
    if (worst) *worst = wt;
    if (worst_delta) *worst_delta = wd;
    return score;
}

/* Score -> band. High is POWER, low is TECHNIQUE; the game clamps the displayed
 * band to 0..99, which is why a 139-point theoretical maximum still only ever
 * reads S-POW. `letter` comes back as a PSX_RANK_* index (0=D .. 4=S). */
static void rank_split(int score, int *pow, int *letter) {
    int s = score < 0 ? 0 : (score > 99 ? 99 : score);
    if (s >= 50) { *pow = 1; *letter = (s - 50) / 10; }
    else         { *pow = 0; *letter = (49 - s) / 10; }
}

static const char *rank_band(int score) {
    static const char LETTER[5] = { 'D', 'C', 'B', 'A', 'S' };
    static char buf[8];
    int pow = 0, letter = 0;
    rank_split(score, &pow, &letter);
    buf[0] = LETTER[letter];
    buf[1] = '-';
    buf[2] = pow ? 'P' : 'T';
    buf[3] = pow ? 'O' : 'E';
    buf[4] = pow ? 'W' : 'C';
    buf[5] = '\0';
    return buf;
}

/* Is a duel actually running?
 *
 * The block is zeroed by the duel init (func_800175A0) but nothing is known to
 * clear it afterwards, so "the numbers look plausible" would keep showing the
 * last duel's rank out on the world map. Instead latch the two transitions the
 * game itself makes:
 *
 *   ARM    the freshly-initialised state — both players' counters all zero and
 *          remaining LP still equal to starting LP. Only duel init produces it.
 *   CLEAR  the scored state — func_80021598 has written both finished scores to
 *          0x80179A04/08, which happens exactly once, at duel end.
 *
 * Known limitation: restoring a save state taken mid-duel skips the arming
 * transition, so the meter stays off until the next duel starts. */
static int  s_rank_active;
static char s_rank_last_line[64];
/* Last tick's decision inputs, for the rank_meter_state debug command. "The
 * meter is not on screen" has several possible causes — no duel, HUD off
 * screen, occluded, mode off — and they are indistinguishable from the pixels,
 * which is how the occlusion deadlock survived a play session unnoticed. */
static int s_rank_dbg_anchor, s_rank_dbg_occluded, s_rank_dbg_x, s_rank_dbg_y;

/* ---- flicker hold ---------------------------------------------------------
 *
 * Showing the meter the instant its anchor is visible makes it STROBE through
 * a fusion: the summon animation slides the HUD out and back and draws card art
 * across the meter's rect, so "anchor present and not occluded" flips several
 * times a second and the meter blinks in and out with it.
 *
 * So the two directions are deliberately asymmetric. Hiding is immediate --
 * the moment anything covers the meter it must go, or it draws over the game's
 * own art. Showing has to wait for the condition to hold steady for a stretch,
 * which is what turns a strobe into "hidden for the duration of the fusion,
 * back once the board settles". Frames, at 60 Hz, so about a fifth of a second
 * -- long enough to swallow an animation's worth of flapping, short enough
 * that returning to a quiet board does not feel laggy. */
#define PSX_RANK_SHOW_HOLD 12
static int s_rank_show_steady;
/* Duel-start fade ramp. Expressed as a FRAME COUNT rather than a per-frame
 * step so the duration is exact and halving it is halving one number.
 *
 * Shape and length are MEASURED against the game's own arena fade rather than
 * eyeballed. Method matters here: `rank_fade_ring` and the display ring are
 * both stamped with s_frame_count, so the meter's alpha and the guest's screen
 * brightness are joined on IDENTICAL frames. Polling cannot do this — `step`
 * and `run_to_frame` are removed, so a sampler necessarily reads a free-running
 * guest, and an earlier pass that inferred the offset across two runs (whose
 * arm frames differed) shipped a delay several frames wrong.
 *
 * Measured against this ramp's own fade_t (2026-08-16):
 *   fade_t 0..9    screen still black
 *   fade_t 10..46  brightness rises, fitting t^1.5 to within a few points
 *   fade_t 46+     plateau
 *
 * The original ramp was LINEAR over 44 frames starting immediately, which held
 * the meter 16-20 percentage points brighter than the scene for the whole
 * transition and finished 6 frames early. Total duration was never the problem;
 * the dead time and the curve were. */
#define PSX_RANK_FADE_DELAY  10
#define PSX_RANK_FADE_FRAMES 36
/* Rects needed before a semi-transparent band counts as a screen dimmer. The
 * post-fight fade draws 30; ordinary UI never draws anything like that many. */
#define PSX_RANK_DIM_MIN_RECTS 16
static int s_rank_fade = 255;
static int s_rank_fade_t;

/* Fade state, for rank_meter_state. The ramp only advances on frames the HUD
 * anchor is present, so "how far in is the fade" cannot be derived from a frame
 * count outside — it has to be read from the thing that owns it.
 *
 * Reports the EFFECTIVE alpha (ramp combined with the screen dimmer), not the
 * ramp alone: the effective value is what actually reaches the screen, so it is
 * the only one worth checking against measured brightness. s_rank_fade_t still
 * gives the ramp's own progress. */
static int s_rank_fade_eff = 255;

void psx_rank_meter_fade_debug(int *fade, int *fade_t) {
    if (fade)   *fade   = s_rank_fade_eff;
    if (fade_t) *fade_t = s_rank_fade_t;
}

void psx_rank_meter_debug(int *mode, int *active, int *anchor,
                                     int *occluded, int *x, int *y,
                                     int *show_hold) {
    if (mode)      *mode      = g_rank_meter;
    if (active)    *active    = s_rank_active;
    if (anchor)    *anchor    = s_rank_dbg_anchor;
    if (occluded)  *occluded  = s_rank_dbg_occluded;
    if (x)         *x         = s_rank_dbg_x;
    if (y)         *y         = s_rank_dbg_y;
    /* Counts up to PSX_RANK_SHOW_HOLD; below it the meter is deliberately
     * hidden. Without this a "why is the meter gone" question has no answer
     * short of re-deriving the flicker rule from the pixels. */
    if (show_hold) *show_hold = s_rank_show_steady;
}

/* Drop the meter and forget what was on screen, so the next duel re-pushes
 * even if its first line happens to read the same as the last one's. */
static void rank_meter_clear(void) {
    s_rank_active = 0;
    s_rank_fade = 255;
    s_rank_fade_t = 0;
    s_rank_last_line[0] = '\0';
    host_osd_set_status(NULL);
    psx_rank_meter_set(0, 0, 0, 0, 0);
}

static bool rank_block_freshly_init(void) {
    for (int p = 0; p < 2; p++) {
        const uint32_t b = PSX_RANK_BLOCK + (uint32_t)p * 0x20u;
        for (uint32_t o = 0x00; o <= 0x0C; o++)
            if (psx_mod_read_byte(b + o) != 0) return false;
        int lp = rank_s16(b + 0x14), start = rank_s16(b + 0x16);
        if (start <= 0 || lp != start) return false;
    }
    return true;
}

void psx_rank_logic_tick(void) {
    if (g_rank_meter == PSX_VM_RANK_OFF || !psx_mod_game_started() ||
        !rank_table_resident()) {
        if (s_rank_active) rank_meter_clear();
        return;
    }

    const uint32_t you = PSX_RANK_BLOCK;
    const uint32_t opp = PSX_RANK_BLOCK + 0x20u;
    const RankTerm *worst = NULL;
    int worst_delta = 0;
    int score = rank_score(you, &worst, &worst_delta);

    int bx = 0, by = 0, occluded = 0;
    const int have_anchor = gpu_sprite_watch_query(2, &bx, &by, &occluded);
    s_rank_dbg_anchor = have_anchor;
    s_rank_dbg_occluded = occluded;
    s_rank_dbg_x = bx;
    s_rank_dbg_y = by;

    if (!s_rank_active) {
        /* Arm on the duel-init state, OR simply on the duel HUD being on
         * screen. The second clause matters more than it looks: the init
         * transition happens once, so anything that skips it — restoring a save
         * state taken mid-duel — used to leave the meter dead until the next
         * duel started. The HUD being drawn is proof enough that a duel is
         * running, and it recovers from any missed transition. */
        if (!rank_block_freshly_init() && !have_anchor) return;
        s_rank_active = 1;
        s_rank_fade = 0;          /* start the duel-start ramp */
        s_rank_fade_t = 0;
    } else {
        /* Scored: the routine has run and both results are in place.
         *
         * The score match alone is not enough to bet a whole duel on — mid-duel
         * that memory holds unrelated card data, and a chance agreement would
         * clear the latch with no way back until the next duel starts. So also
         * require the routine's own signature: it stores 68 / 64 / 69 at
         * +0x34..+0x36 on the way in, and nothing else writes that triple. */
        int o_you = (int32_t)psx_mod_read_word(PSX_RANK_RESULT + 44);
        int o_opp = (int32_t)psx_mod_read_word(PSX_RANK_RESULT + 48);
        const int tag_ok = psx_mod_read_byte(PSX_RANK_RESULT + 52) == 68 &&
                           psx_mod_read_byte(PSX_RANK_RESULT + 54) == 69;
        if (tag_ok && o_you == score && o_opp == rank_score(opp, NULL, NULL)) {
            rank_meter_clear();
            return;
        }
    }

    const int shown = score < 0 ? 0 : (score > 99 ? 99 : score);

    /* Follow the game's own FIELD box rather than sitting at fixed coordinates.
     *
     * The duel HUD is not simply shown or hidden: it TWEENS off the screen
     * edges — the FIELD box to the left, the LP panel to the right — whenever a
     * card view, an attack animation, the 3D monster fight or the results
     * screen takes over. A meter pinned to (74,24) would hang in mid-air for
     * the whole slide and then pop out. Anchoring to the box means the meter
     * rides the same tween, and "the HUD is gone" needs no separate test: the
     * box stops being drawn and the meter goes with it.
     *
     * PSX_FIELDBOX_CLUT is the palette the box's five sprites are drawn
     * through, measured off the GP0 stream; PSX_FIELDBOX_W is its width. */
    int pow = 0, letter = 0;
    rank_split(shown, &pow, &letter);

    const int in_game = (g_rank_meter == PSX_VM_RANK_INGAME ||
                         g_rank_meter == PSX_VM_RANK_INGAME_SCORE);
    const int with_score = (g_rank_meter == PSX_VM_RANK_INGAME_SCORE);

    if (in_game) {
        if (!have_anchor) {
            /* HUD is off screen entirely. Disarm the occlusion rect as well —
             * see below for why a stale one is not merely useless but fatal. */
            gpu_sprite_watch_occlusion(0, 0, 0, 0);
            s_rank_show_steady = 0;
            psx_rank_meter_set(0, 0, 0, 0, 0);
        } else {
            /* Update values and position FIRST, then re-arm the occlusion rect
             * from where the meter actually is this frame — unconditionally,
             * including on frames where it is hidden by occlusion.
             *
             * Arming only while visible deadlocks: once hidden, the rect stays
             * frozen at the meter's old home position, and when the HUD slides
             * back in the game is drawing right there, so it reads as occluded
             * forever and the rect never updates to say otherwise. The meter
             * disappears after a couple of transitions and never returns. */
            /* Fade-in at duel start. SYNTHETIC, and deliberately so.
             *
             * Three candidate mechanisms for the game's own fade were tested
             * and all ruled out by measurement: primitive-colour modulation
             * (the HUD is always drawn at neutral 0x808080), a CLUT ramp (the
             * FIELD box palette sits at full brightness through a duel start)
             * and a full-screen darkening quad (none exists in the 2.5s after
             * the duel begins). The HUD does not fade at all — it SLIDES in,
             * anchor_x ramping 20 -> 49 -> 60 in about 90ms, and the meter
             * already rides that slide because it is anchored to it. Only the
             * 3D arena fades, by its own geometry, which a HUD-anchored overlay
             * has nothing to read.
             *
             * So this ramp is ours, not the game's. It runs once per duel — on
             * the latch arming — rather than on every reappearance, or the
             * meter would fade back in after every card view and read as lag. */
            if (s_rank_fade < 255) {
                const int n = ++s_rank_fade_t - PSX_RANK_FADE_DELAY;
                if (n <= 0) {
                    /* Arena is still black; showing anything here is what read
                     * as the meter arriving early. */
                    s_rank_fade = 0;
                } else if (n >= PSX_RANK_FADE_FRAMES) {
                    s_rank_fade = 255;
                } else {
                    const double t = (double)n / (double)PSX_RANK_FADE_FRAMES;
                    s_rank_fade = (int)(255.0 * t * sqrt(t) + 0.5);
                }
            }
            /* Returning from the 3D monster fight is a SECOND fade, and a
             * different mechanism: the field fades back under a band of
             * semi-transparent rects (see gpu_fade_dimmer_level) rather than by
             * arena geometry. The ramp above cannot cover it — it runs once per
             * duel by design, so it is long finished by then and the meter
             * popped in at full opacity over a 24%-lit scene (+68 points, the
             * worst mismatch measured).
             *
             * Reading the dimmer needs no timer, no "was I hidden long enough"
             * heuristic, and nothing special for card views: those draw no
             * dimmer band, so the level is -1 and the meter stays bright.
             * PSX_RANK_DIM_MIN_RECTS keeps stray semi-transparent UI rects from
             * reading as a fade — the real band is 30.
             *
             * The two combine by taking whichever is DIMMER, so neither has to
             * know about the other: at duel start there is no band, and after a
             * fight the ramp is already at 255. */
            int fade_out = s_rank_fade;
            const int dim = gpu_fade_dimmer_level(PSX_RANK_DIM_MIN_RECTS);
            if (dim >= 0) {
                const int by_dim = 255 - (dim > 255 ? 255 : dim);
                if (by_dim < fade_out) fade_out = by_dim;
            }
            s_rank_fade_eff = fade_out;
            psx_rank_meter_set_fade(fade_out);
            if (occluded) s_rank_show_steady = 0;
            else if (s_rank_show_steady < PSX_RANK_SHOW_HOLD)
                s_rank_show_steady++;
            psx_rank_meter_set(s_rank_show_steady >= PSX_RANK_SHOW_HOLD ? 1 : 0,
                               shown, pow, letter, with_score);
            /* Line the POW/TEC badge's top up with the FIELD box's top edge.
             *
             * set_origin takes where the LETTER goes, and the canvas starts
             * s_letter_y above that, so passing by + letter_offset_y puts the
             * canvas top — which IS the badge's top row — exactly on `by`. That
             * is an identity in guest pixels, so it survives every scale
             * factor; the previous hand-tuned -8 was dialled in at one window
             * size and necessarily read wrong at the others.
             *
             * x: +2 pairs with RM_LETTER_X = 2 so the letter and number sit two
             * pixels left of the badge without moving the badge itself. */
            int loff_x = 0, loff_y = 0;
            psx_rank_meter_letter_offset(&loff_x, &loff_y);
            psx_rank_meter_set_origin(bx + PSX_FIELDBOX_CAP_W + 2,
                                      by + loff_y - PSX_RANK_LIFT_Y);
            int ex = 0, ey = 0, ew = 0, eh = 0;
            psx_rank_meter_extent(&ex, &ey, &ew, &eh);
            gpu_sprite_watch_occlusion(ex, ey, ew, eh);
        }
    } else {
        s_rank_show_steady = 0;
        psx_rank_meter_set(0, 0, 0, 0, 0);
        gpu_sprite_watch_occlusion(0, 0, 0, 0);
    }

    /* OVERLAY TEXT mode. Drawn at the window's top-left rather than inside the
     * game area, so it does not follow the HUD and does not hide behind card
     * views — that is the point of it, and it doubles as a fallback if the
     * in-game anchoring ever misbehaves. */
    char line[64];
    if (g_rank_meter == PSX_VM_RANK_TEXT) {
        if (worst)
            snprintf(line, sizeof(line), "%s %2d  %s %d",
                          rank_band(score), shown, worst->tag, worst_delta);
        else
            snprintf(line, sizeof(line), "%s %2d", rank_band(score), shown);
    } else {
        line[0] = '\0';
    }

    /* Only push on a CHANGE: host_osd_set_status marks the status dirty
     * unconditionally, so setting the same string every frame would re-
     * rasterize ~25k pixels and re-upload the overlay 60 times a second to
     * draw exactly what is already there. */
    if (strcmp(line, s_rank_last_line) != 0) {
        snprintf(s_rank_last_line, sizeof(s_rank_last_line), "%s", line);
        host_osd_set_status(line[0] ? line : NULL);
    }
}

/* Apply a menu choice. The clamp, the teardown when switching off and the
 * cached-line reset all belong together: turning the overlay off has to drop
 * the status line it owns or the last rank stays on screen for the rest of the
 * session, and changing shape (RANK <-> DETAIL) has to forget the cached text
 * or the tick would call the new line "unchanged" until the score next moved. */
/* VIEW > DUEL RANK. The hint changes with the selection, so each choice
 * carries its own -- a single hint could only describe one of four states. */
static const char *const RANK_LABELS[] = {
    "Off", "In game", "In game + score", "Overlay text"
};
static const char *const RANK_HINTS[] = {
    "Show live rank in duel",
    "Game sprites beside the field box",
    "Sprites plus the 0-99 score",
    "Text in the corner \xe2\x80\x94 never covered"
};

static void rank_row_changed(int value) {
    psx_rank_logic_set_mode(value);
}

PSX_MOD_CONSTRUCTOR(psx_rank_logic_install) {
    psx_rank_logic_register_menu();
    (void)psx_game_add_start_hook(psx_rank_logic_arm_sprite_watch);
    (void)psx_game_add_frame_hook(psx_rank_logic_tick);
}

void psx_rank_logic_register_menu(void) {
    const int h = psx_video_menu_add_option(
        PSX_VM_MENU_VIEW, "Duel rank", RANK_HINTS[0],
        RANK_LABELS, 4, "rank_meter", PSX_VM_RANK_INGAME, rank_row_changed);
    psx_video_menu_set_row_hints(h, RANK_HINTS);
}

void psx_rank_logic_set_mode(int mode) {
    g_rank_meter = (mode >= 0 && mode <= 3) ? mode : PSX_VM_RANK_OFF;
    if (g_rank_meter == PSX_VM_RANK_OFF && s_rank_active) rank_meter_clear();
    s_rank_last_line[0] = '\0';
}

/* Arming lives here because the constants that pick the FIELD box out of the
 * sprite stream are this game's, not the renderer's. */
void psx_rank_logic_arm_sprite_watch(void) {
    gpu_sprite_watch(PSX_FIELDBOX_CLUT_X, PSX_FIELDBOX_CLUT_Y,
                     PSX_FIELDBOX_U, PSX_FIELDBOX_V);
}

