/* psx_card_manager.c — see psx_card_manager.h.
 *
 * The window is the front end of psx_card_packs.c and owns no game state of
 * its own: every field it shows is either the stock value read from the
 * game's tables or the player's edit read back from that module, and SAVE
 * goes through psx_card_packs_save(), the same path a hand-edited card.ini
 * takes. The art preview is decoded from the very sectors the game will
 * stream, so what the preview shows is what the password screen draws.
 *
 * DRAWN LIKE THE F10 MENU: the same typeface (psx_ui_font), the same
 * antialiased panels and pills (psx_ui_draw), the same palette, and a layout
 * in DESIGN UNITS (1 unit = 1/480 of the window height) so a bigger window
 * gets more detail rather than bigger blocks. Everything is measured, never
 * assumed: text that would run past its box is wrapped or ellipsised.
 *
 * WINDOW PLUMBING: an SDL renderer is the only way this SDL3/Wayland build
 * can put pixels in a second window (no plain window framebuffer), and every
 * renderer call may make ITS GL context current on the emulation thread. So
 * the game's context is captured when the window opens and put back after
 * each renderer call; without that the game window goes black and the
 * driver faults at present. */

#include "psx_textfile.h"
#include "psx_card_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define MKDIR(p) _mkdir(p)
#else
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

#include "psx_sdl.h"

#include "host_osd.h"
#include "mod_plugins.h"
#include "psx_card_db.h"
#include "psx_card_packs.h"
#include "psx_card_effects.h"
#include "psx_card_colors.h"
#include "psx_card_share.h"
#include "psx_card_texts.h"
#include "psx_game_hooks.h"
#include "psx_ui_draw.h"
#include "psx_ui_font.h"
#include "psx_video_menu.h"

/* --- window ---------------------------------------------------------------- */
#define WIN_W  1480
#define WIN_H   820

static SDL_Window   *s_win;
static SDL_Renderer *s_ren;
static SDL_Texture  *s_tex;
static uint32_t     *s_px;
static int           s_w, s_h, s_dirty;
static int           s_open_req;
static PsxUiCanvas   s_cv;
static float         s_u = 1.0f;          /* design unit in pixels */
static SDL_Window   *s_gl_win;
static SDL_GLContext s_gl_ctx;

/* --- palette (the F10 menu's, opaque where this window is opaque) ----------- */
#define COL_BG        0xFF0F1219u
#define COL_BAR       0xFF141826u
#define COL_PANEL     0xFF1E2233u
#define COL_TEXT      0xFFC9CFDDu
#define COL_DIM       0xFF7C8598u
#define COL_ACCENT    0xFF7FA6FFu
#define COL_SEL_BG    0xFF2C3B60u
#define COL_HOVER     0x14FFFFFFu
#define COL_EDIT_BG   0xFF0E1119u
#define COL_EDITED    0xFF8BD48Bu   /* a value the player changed */
#define COL_BTN       0xFF2A3147u
#define COL_BTN_ON    0xFF3D5A9Cu
#define COL_TRACK     0x40FFFFFFu
#define COL_THUMB     0xFF7C8598u
#define COL_WARN      0xFFE8C36Au

/* --- design units ------------------------------------------------------------ */
#define U_BAR_H     26.0f
#define U_GAP        6.0f
#define U_PAD       10.0f
#define U_LIST_W   190.0f
#define U_ROW_H     14.0f
#define U_HDR_H     16.0f
#define U_FIELD_H   18.0f
#define U_BOX_H     15.0f
#define U_LABEL_W   64.0f
#define U_BTN_H     17.0f
#define U_STEP_W    15.0f
#define U_R_PANEL    9.0f
#define U_R_BOX      5.0f
#define U_FS_TITLE  11.0f
#define U_FS_BODY    9.5f
#define U_FS_SMALL   8.5f

static int px(float u) { return (int)(u * s_u + 0.5f); }

static const PsxUiFace *face_title(void) { return psx_ui_font_face(U_FS_TITLE * s_u, PSX_UI_FONT_SEMIBOLD); }
static const PsxUiFace *face_body(void)  { return psx_ui_font_face(U_FS_BODY * s_u, PSX_UI_FONT_REGULAR); }
static const PsxUiFace *face_bold(void)  { return psx_ui_font_face(U_FS_BODY * s_u, PSX_UI_FONT_SEMIBOLD); }
static const PsxUiFace *face_small(void) { return psx_ui_font_face(U_FS_SMALL * s_u, PSX_UI_FONT_REGULAR); }

/* --- list ------------------------------------------------------------------ */
#define CARDS 722
static int  s_order[CARDS];
static int  s_order_n;
static char s_search[32];
static int  s_scroll;
static int  s_hover_row = -1;
static int  s_sel = 1;
static int  s_sb_drag, s_sb_grab;

/* --- editor ---------------------------------------------------------------- */
enum { F_NAME, F_DESC, F_ATK, F_DEF, F_STAR1, F_STAR2, F_TYPE, F_LEVEL, F_ATTR, F_PRICE, F_PASSWORD, F_COLOR,
       F_EFFECT, F_AMOUNT, F_TARGET, F_TERRAIN, F_RITUAL, F_EQUIP_BONUS, F_EQUIPS, F_BOOST, F_TRAP_MAX,
       F_RULE_FIRST,                                      /* the effects list: RULE_MAX rows of RP_N boxes */
       F_RULE_END = F_RULE_FIRST + 16 * 5,
       F_IMMUNE = F_RULE_END, F_COUNT };
#define RULE_MAX 16
enum { RP_WHEN, RP_CHANCE, RP_FX, RP_PARAM, RP_PER, RP_N };
#define F_FX_FIRST F_EFFECT
static const char *const TAB_LABEL[2] = { "Card", "Effects" };
static int s_tab;                     /* 0 the card's own fields, 1 its effects */
enum { B_SAVE, B_RESTORE, B_FOLDER, B_ART, B_THUMB, B_TITLE, B_EFFECT_TEXT, B_ADD_RULE,
       B_EXPORT_TEXTS, B_IMPORT_TEXTS, B_EXPORT, B_IMPORT, B_DEV, B_COUNT };   /* B_EXPORT_TEXTS.. sit in the top bar */
#define B_BAR_FIRST B_EXPORT_TEXTS
static const char *const BTN_LABEL[B_COUNT] = { "Save", "Restore stock", "Open folder", "Pick art\xE2\x80\xA6", "Pick thumbnail\xE2\x80\xA6", "Pick title\xE2\x80\xA6",
                                                "Effect text \xE2\x86\x92 description", "+ Add effect", "Export Descriptions", "Import Descriptions", "Export Config", "Import Config", "Dev Card Effects: OFF" };
static char s_dev_label[32];
#define FTEXT 2048                    /* the longest field text (an equip list) */

static int          s_stock_ok;
static int          s_has_pack;      /* the card has a folder (is edited) */
static int          s_changed;
static int          s_focus = -1;
static char         s_buf[FTEXT];
static int          s_caret_on = 1;
static char         s_msg[128];
static unsigned     s_msg_until;
static unsigned     s_seen_gen;
static int          s_hover_btn = -1;

static uint8_t  s_art[102 * 96 * 3], s_thumb[40 * 32 * 3];
static uint32_t s_art_argb[102 * 96], s_thumb_argb[40 * 32];
static int      s_art_ok, s_thumb_ok, s_art_id = -1;
static unsigned s_art_gen;

static char s_pick_path[1024];
static int  s_pick_kind;              /* 1 art, 2 thumb, 3 title, 4 export target, 5 import source, 6 descriptions out, 7 descriptions in */
static unsigned s_present_count;

/* the import preview (1) or the Card Effects activation question (2) */
static int  s_modal;
#define MODAL_IMPORT 1
#define MODAL_ACTIVATE 2
static PsxCardShareInfo s_share;
static char s_share_path[1024];
static int  s_modal_hover = -1;

/* --- geometry, recomputed from the window size on every draw and click ------- */
typedef struct { int x, y, w, h; } Rect;
static int in_rect(const Rect *r, int x, int y) { return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h; }

typedef struct {
    Rect bar, search, list, list_rows, sb, ed;
    int  row_h, rows;
    Rect value[F_COUNT], step_l[F_COUNT], step_r[F_COUNT], clear[F_COUNT], label[F_COUNT];
    Rect btn[B_COUNT];                /* btn[B_BAR_FIRST..] sit in the top bar, right-aligned */
    int  bar_free;                    /* room left in the bar after the search box */
    Rect art, thumb;
    int  info_x, info_y;
    int  status_y;
    int  fx_note_x, fx_note_y;        /* the effects note line, 0 = none */
    Rect tab[2];
    int  preview_y;                   /* the card-text preview under the rules, 0 = none */
    Rect modal, modal_ok, modal_cancel;
} Layout;
static Layout s_L;

/* --- choices ------------------------------------------------------------------
 * Every choice field is a list the player picks from (a dropdown, or the
 * < > steppers). A trigger row is an effect list plus, when that effect
 * takes one, a number box or a type / field list beside it. */
static PsxCardPack  s_edit;
static PsxCardStock s_stock;
static int magic_dispatchable(int id);
static const int TRIG_FX[] = { -1, PSX_CARD_FX_HEAL, PSX_CARD_FX_DAMAGE, PSX_CARD_FX_DESTROY_TYPE, PSX_CARD_FX_DESTROY_ATK,
    PSX_CARD_FX_RAIGEKI, PSX_CARD_FX_DARK_HOLE, PSX_CARD_FX_DRAGON_JAR, PSX_CARD_FX_STOP_DEFENSE, PSX_CARD_FX_FLIP,
    PSX_CARD_FX_WEAKEN, PSX_CARD_FX_SWORDS, PSX_CARD_FX_CURSEBREAKER, PSX_CARD_FX_HARPIE, PSX_CARD_FX_FIELD,
    PSX_CARD_FX_DESTROY_STRONGEST, PSX_CARD_FX_LOSE_LP, PSX_CARD_FX_GAMBLE_LP, PSX_CARD_FX_GAMBLE, PSX_CARD_FX_DESTROY_OWN, PSX_CARD_FX_DESTROY_OWN_LP };
#define TRIG_N ((int)(sizeof TRIG_FX / sizeof TRIG_FX[0]))
#define TRIG_MONSTER_ONLY 3            /* the coin and the two "destroy your own" only make sense on a monster */
static const char *const IMMUNE_LABEL[4] = { "Nothing", "Traps", "Destruction magic", "Traps and magic" };
/* the odds a rule can roll; "Otherwise" (an else branch) is offered when a rule follows another on the same trigger */
static const int CHANCES[] = { 100, 90, 80, 75, 66, 50, 40, 33, 25, 20, 10, 5 };
#define CHANCE_N ((int)(sizeof CHANCES / sizeof CHANCES[0]))
#define CHANCE_ELSE (-1)

/* --- rules --------------------------------------------------------------------
 * A monster's effects are shown as one list of rules, each a sentence:
 *   When [Summoned face-up] [50%] do [Destroy all the opponent's monsters]
 *   When [While face-up]          do [Gain ATK/DEF] [500] per [each Lava Battleguard you control]
 * Behind it sit the six triggers' branch lists, the ATK/DEF bonus rules
 * and the battle behaviour; the list is rebuilt from them after every
 * change, in that order. A rule row is RP_N fields. */
#define WHEN_N 7                       /* 0..5 the triggers, 6 "While face-up" */
#define WHEN_FACEUP 6
static const char *const WHEN_LABEL[WHEN_N] = { "Summoned face-up", "Flipped face-up", "Destroyed", "Attacks", "Your turn starts", "Opponent's turn starts", "While face-up" };
/* what a face-up rule can do */
enum { PFX_BONUS, PFX_INDESTRUCTIBLE, PFX_SLAYER, PFX_MUTUAL, PFX_N };
static const char *const PFX_LABEL[PFX_N] = { "Gain ATK/DEF", "Never destroyed in battle", "Destroys its foe", "Destroys itself and its foe" };
static const int PFX_BATTLE[PFX_N] = { -1, PSX_CARD_BATTLE_INDESTRUCTIBLE, PSX_CARD_BATTLE_SLAYER, PSX_CARD_BATTLE_MUTUAL };
enum { RK_TRIG, RK_BONUS, RK_BATTLE };
typedef struct { int kind, t, k, bi; } Rule;
static Rule s_rules[RULE_MAX];
static int  s_rule_n;
/* "per" choices of a bonus rule */
#define PER_N (5 + 40 + 2)
#define PER_PICK_OWN (PER_N - 2)
#define PER_PICK_ENEMY (PER_N - 1)

static PsxCardTrigger *trig_at(int t)
{
    switch (t) {
    case 0: return &s_edit.on_summon;
    case 1: return &s_edit.on_flip;
    case 2: return &s_edit.on_death;
    case 3: return &s_edit.on_attack;
    case 4: return &s_edit.each_turn;
    default: return &s_edit.opp_turn;
    }
}
static void branch_reset(PsxCardFxBranch *b) { b->chance = 100; b->is_else = 0; b->fx = b->amount = b->target = b->terrain = -1; }
static void trig_remove(PsxCardTrigger *t, int k)
{
    if (k < 0 || k >= t->n) return;
    for (int i = k; i + 1 < t->n; i++) t->b[i] = t->b[i + 1];
    t->n--;
    branch_reset(&t->b[t->n]);
    if (t->n) t->b[0].is_else = 0;
}
static int trig_append(PsxCardTrigger *t, const PsxCardFxBranch *b)
{
    if (t->n >= PSX_CARD_BRANCHES) return -1;
    t->b[t->n] = *b;
    if (t->n == 0) t->b[0].is_else = 0;
    return t->n++;
}
/* branches with no effect are dropped before a save */
static void trigger_prune(PsxCardTrigger *t)
{
    int w = 0;
    for (int i = 0; i < t->n; i++) if (t->b[i].fx >= 0) t->b[w++] = t->b[i];
    t->n = w;
    for (int i = w; i < PSX_CARD_BRANCHES; i++) branch_reset(&t->b[i]);
    if (w) t->b[0].is_else = 0;
}
static void bonus_remove(int i)
{
    if (i < 0 || i >= s_edit.bonus_n) return;
    for (int k = i; k + 1 < s_edit.bonus_n; k++) s_edit.bonus[k] = s_edit.bonus[k + 1];
    s_edit.bonus_n--;
}
static int bonus_append(int amount, int enemy, int filter)
{
    if (s_edit.bonus_n >= PSX_CARD_BONUSES) return -1;
    s_edit.bonus[s_edit.bonus_n].amount = amount; s_edit.bonus[s_edit.bonus_n].enemy = enemy; s_edit.bonus[s_edit.bonus_n].filter = filter;
    return s_edit.bonus_n++;
}
static void rules_build(void)
{
    s_rule_n = 0;
    for (int t = 0; t < 6; t++)
        for (int k = 0; k < trig_at(t)->n && s_rule_n < RULE_MAX; k++) { Rule r = { RK_TRIG, t, k, -1 }; s_rules[s_rule_n++] = r; }
    if (s_edit.battle > 0 && s_rule_n < RULE_MAX) { Rule r = { RK_BATTLE, WHEN_FACEUP, 0, -1 }; s_rules[s_rule_n++] = r; }
    for (int i = 0; i < s_edit.bonus_n && s_rule_n < RULE_MAX; i++) { Rule r = { RK_BONUS, WHEN_FACEUP, 0, i }; s_rules[s_rule_n++] = r; }
}
static int is_rulef(int f)   { return f >= F_RULE_FIRST && f < F_RULE_END; }
static int rule_of(int f)    { return (f - F_RULE_FIRST) / RP_N; }
static int part_of(int f)    { return (f - F_RULE_FIRST) % RP_N; }
static int rule_field(int r, int part) { return F_RULE_FIRST + r * RP_N + part; }
static const Rule *rule_ptr(int f) { if (!is_rulef(f)) return NULL; const int r = rule_of(f); return (r >= 0 && r < s_rule_n) ? &s_rules[r] : NULL; }
static PsxCardFxBranch *rule_branch(int f) { const Rule *r = rule_ptr(f); return (r && r->kind == RK_TRIG) ? &trig_at(r->t)->b[r->k] : NULL; }
static PsxCardBonus *rule_bonus(int f) { const Rule *r = rule_ptr(f); return (r && r->kind == RK_BONUS) ? &s_edit.bonus[r->bi] : NULL; }
static int is_when(int f)    { return is_rulef(f) && part_of(f) == RP_WHEN; }
static int is_chance(int f)  { return is_rulef(f) && part_of(f) == RP_CHANCE; }
static int is_trig(int f)    { return is_rulef(f) && part_of(f) == RP_FX; }
static int is_param(int f)   { return is_rulef(f) && part_of(f) == RP_PARAM; }
static int is_per(int f)     { return is_rulef(f) && part_of(f) == RP_PER; }
static int no_steppers(int f) { return is_rulef(f); }
/* what a rule's parameter box holds: 'a' amount, 't' monster type, 'f' field, 0 nothing */
static int param_kind(int f)
{
    if (!is_param(f)) return 0;
    const Rule *r = rule_ptr(f);
    if (!r) return 0;
    if (r->kind == RK_BONUS) return 'a';
    if (r->kind != RK_TRIG) return 0;
    switch (rule_branch(f)->fx) {
    case PSX_CARD_FX_HEAL: case PSX_CARD_FX_DAMAGE: case PSX_CARD_FX_DESTROY_ATK: case PSX_CARD_FX_WEAKEN: case PSX_CARD_FX_LOSE_LP: return 'a';
    case PSX_CARD_FX_DESTROY_TYPE: return 't';
    case PSX_CARD_FX_FIELD: return 'f';
    default: return 0;
    }
}
static const char *field_label(int f)
{
    static const char *const FIELD_LABEL[F_RULE_FIRST] = {
        "Name", "Description", "Attack", "Defense", "Star 1", "Star 2", "Type", "Level", "Attribute", "Price", "Password", "Frame",
        "Effect", "Amount", "Target type", "Terrain", "Recipe", "Equip bonus", "Equips", "Boosts", "Trap ATK max"
    };
    if (f < F_RULE_FIRST) return FIELD_LABEL[f];
    if (f == F_IMMUNE) return "Immune to";
    if (is_when(f)) return "When";
    if (is_trig(f)) return "do";
    if (is_per(f)) return "per";
    return "";
}
static int field_is_enum(int f)
{
    if (is_param(f)) { const int k = param_kind(f); return k == 't' || k == 'f'; }
    return f == F_STAR1 || f == F_STAR2 || f == F_TYPE || f == F_ATTR || f == F_COLOR || f == F_EFFECT || f == F_TARGET || f == F_TERRAIN
        || f == F_IMMUNE || is_when(f) || is_chance(f) || is_trig(f) || is_per(f);
}
/* the open dropdown; a "per" list may be showing the 722 cards instead */
static int s_drop = -1, s_drop_scroll, s_drop_hover = -1, s_drop_cards;
static char s_drop_filter[32];
static int enum_count(int f)
{
    switch (f) {
    case F_STAR1: case F_STAR2: return 10;
    case F_TYPE: return 24;
    case F_ATTR: return 8;
    case F_COLOR: return PSX_CARD_COLOR_COUNT;
    case F_EFFECT: return magic_dispatchable(s_sel) ? PSX_CARD_FX_GAMBLE : TRIG_N - 1 - TRIG_MONSTER_ONLY;   /* every spell effect; outside the game's own spell ids also no "none" / ritual */
    case F_TARGET: return 20;
    case F_TERRAIN: return 6;
    case F_IMMUNE: return 4;
    default: {
        const Rule *r = rule_ptr(f);
        if (!r) return 0;
        if (is_when(f)) return WHEN_N;
        if (is_chance(f)) return CHANCE_N + (r->kind == RK_TRIG && r->k > 0 ? 1 : 0);
        if (is_trig(f)) return r->kind == RK_TRIG ? TRIG_N : PFX_N;
        if (is_param(f)) return param_kind(f) == 't' ? 20 : 6;
        if (is_per(f)) return (s_drop == f && s_drop_cards) ? CARDS : PER_N;
        return 0;
    }
    }
}
/* the value option i stands for */
static int enum_value(int f, int i)
{
    if (f == F_STAR1 || f == F_STAR2 || f == F_TERRAIN) return i + 1;
    if (f == F_EFFECT && !magic_dispatchable(s_sel)) return TRIG_FX[i + 1];
    if (is_chance(f)) {
        const Rule *r = rule_ptr(f);
        if (r && r->kind == RK_TRIG && r->k > 0) { if (i == 1) return CHANCE_ELSE; if (i > 1) i--; }
        return CHANCES[i];
    }
    if (is_trig(f)) { const Rule *r = rule_ptr(f); return (r && r->kind == RK_TRIG) ? TRIG_FX[i] : i; }
    if (is_param(f)) return param_kind(f) == 'f' ? i + 1 : i;
    if (is_per(f)) return (s_drop == f && s_drop_cards) ? i + 1 : i;
    return i;
}
/* a "per" option as (enemy, filter); i = PER_PICK_* stands for "a card…" */
static void per_decode(int i, int *enemy, int *filter)
{
    if (i == 0) { *enemy = 0; *filter = -1; }
    else if (i <= 4) { *enemy = i >= 3; *filter = (i == 2 || i == 4) ? PSX_CARD_PACK_FILTER_HAND : 0; }
    else if (i < 45) { *enemy = i >= 25; *filter = PSX_CARD_PACK_FILTER_TYPE + (i - 5) % 20; }
    else { *enemy = i == PER_PICK_ENEMY; *filter = 0; }
}
static int per_encode(int enemy, int filter)
{
    if (filter < 0) return 0;
    if (filter == 0) return enemy ? 3 : 1;
    if (filter == PSX_CARD_PACK_FILTER_HAND) return enemy ? 4 : 2;
    if (filter >= PSX_CARD_PACK_FILTER_TYPE) return 5 + (enemy ? 20 : 0) + (filter - PSX_CARD_PACK_FILTER_TYPE) % 20;
    return enemy ? PER_PICK_ENEMY : PER_PICK_OWN;   /* a card */
}
static const char *per_label(int enemy, int filter)
{
    static char b[96];
    if (filter < 0) return "always";
    if (filter == 0) return enemy ? "each monster the opponent controls" : "each monster you control";
    if (filter == PSX_CARD_PACK_FILTER_HAND) return enemy ? "each card in the opponent's hand" : "each card in your hand";
    snprintf(b, sizeof b, "each %s", psx_card_packs_filter_name(filter, enemy));
    return b;
}
static const char *enum_label(int f, int i)
{
    static char b[96];
    const int v = enum_value(f, i);
    switch (f) {
    case F_STAR1: case F_STAR2: return psx_card_packs_star_name(v);
    case F_TYPE: return psx_card_packs_type_name(v);
    case F_ATTR: return psx_card_packs_attribute_name(v);
    case F_COLOR: return psx_card_packs_color_name(v);
    case F_EFFECT: return psx_card_packs_effect_label(v);
    case F_TARGET: return psx_card_packs_type_name(v);
    case F_TERRAIN: return psx_card_packs_terrain_name(v);
    case F_IMMUNE: return IMMUNE_LABEL[v];
    default: {
        const Rule *r = rule_ptr(f);
        if (!r) return "?";
        if (is_when(f)) return WHEN_LABEL[v];
        if (is_chance(f)) { if (v == CHANCE_ELSE) return "Otherwise"; if (v == 100) return "Always"; snprintf(b, sizeof b, "%d%%", v); return b; }
        if (is_trig(f)) return r->kind == RK_TRIG ? (v < 0 ? "Choose an effect\xE2\x80\xA6" : psx_card_packs_effect_label(v)) : PFX_LABEL[v];
        if (is_param(f)) return param_kind(f) == 'f' ? psx_card_packs_terrain_name(v) : psx_card_packs_type_name(v);
        if (is_per(f)) {
            if (s_drop == f && s_drop_cards) { snprintf(b, sizeof b, "%d  %s", v, psx_card_db_name(v)); return b; }
            if (i == PER_PICK_OWN) return "a card you control\xE2\x80\xA6";
            if (i == PER_PICK_ENEMY) return "a card the opponent controls\xE2\x80\xA6";
            int e, fl; per_decode(i, &e, &fl);
            return per_label(e, fl);
        }
        return "?";
    }
    }
}
static int effective_type(void);
/* the value shown now: the edit, else stock, else the layer's default */
static int enum_current_value(int f)
{
    switch (f) {
    case F_STAR1: return s_edit.star1 >= 1 ? s_edit.star1 : s_stock.star1;
    case F_STAR2: return s_edit.star2 >= 1 ? s_edit.star2 : s_stock.star2;
    case F_TYPE: return s_edit.type >= 0 ? s_edit.type : s_stock.type;
    case F_ATTR: return s_edit.attribute >= 0 ? s_edit.attribute : s_stock.attribute;
    case F_COLOR: return s_edit.color >= 0 ? s_edit.color : psx_card_colors_slot(s_sel);
    case F_EFFECT: return s_edit.effect >= 0 ? s_edit.effect : (s_stock.effect >= 0 ? s_stock.effect : (magic_dispatchable(s_sel) ? 0 : PSX_CARD_FX_HEAL));
    case F_TARGET: return s_edit.target >= 0 ? s_edit.target : ((s_stock.effect == PSX_CARD_FX_DESTROY_TYPE && s_stock.amount >= 0) ? s_stock.amount : 3);
    case F_TERRAIN: return s_edit.terrain >= 1 ? s_edit.terrain : ((s_stock.effect == PSX_CARD_FX_FIELD && s_stock.amount >= 1) ? s_stock.amount : 1);
    case F_IMMUNE: return s_edit.immune >= 0 ? s_edit.immune : 0;
    default: {
        const Rule *r = rule_ptr(f);
        if (!r) return 0;
        if (is_when(f)) return r->kind == RK_TRIG ? r->t : WHEN_FACEUP;
        if (is_chance(f)) { const PsxCardFxBranch *b = rule_branch(f); return !b ? 100 : b->is_else ? CHANCE_ELSE : b->chance; }
        if (is_trig(f)) {
            if (r->kind == RK_TRIG) return rule_branch(f)->fx;
            if (r->kind == RK_BONUS) return PFX_BONUS;
            for (int i = 1; i < PFX_N; i++) if (PFX_BATTLE[i] == s_edit.battle) return i;
            return PFX_INDESTRUCTIBLE;
        }
        if (is_param(f)) { const PsxCardFxBranch *b = rule_branch(f); if (!b) return 0; return param_kind(f) == 'f' ? (b->terrain >= 1 ? b->terrain : 1) : (b->target >= 0 ? b->target : 3); }
        if (is_per(f)) { const PsxCardBonus *b = rule_bonus(f); if (!b) return 0; if (s_drop == f && s_drop_cards) return (b->filter >= 1 && b->filter <= CARDS) ? b->filter : 1; return per_encode(b->enemy, b->filter); }
        return 0;
    }
    }
}
static int enum_current_index(int f)
{
    const int v = enum_current_value(f);
    for (int i = 0; i < enum_count(f); i++) if (enum_value(f, i) == v) return i;
    if (is_chance(f)) {
        /* an odd number from a hand-written card.ini: the nearest listed one */
        int best = 0;
        for (int i = 0; i < enum_count(f); i++) { const int e = enum_value(f, i); if (e > 0 && abs(e - v) < abs(enum_value(f, best) - v)) best = i; }
        return best;
    }
    return 0;
}
static void say(const char *m);
static void drop_open_cards(int f);
/* a rule's "When" moves it between the triggers' lists and the face-up rules */
static void rule_set_when(int f, int when)
{
    const Rule r = *rule_ptr(f);
    const int cur = enum_current_value(f);
    if (when == cur) return;
    if (r.kind == RK_TRIG && when != WHEN_FACEUP) {
        PsxCardFxBranch b = trig_at(r.t)->b[r.k];
        if (trig_at(when)->n >= PSX_CARD_BRANCHES) { say("That trigger already holds four effects"); return; }
        trig_remove(trig_at(r.t), r.k);
        trig_append(trig_at(when), &b);
    } else if (r.kind == RK_TRIG) {
        /* becomes an ATK/DEF bonus */
        if (s_edit.bonus_n >= PSX_CARD_BONUSES) { say("Six ATK/DEF bonuses at most"); return; }
        trig_remove(trig_at(r.t), r.k);
        bonus_append(500, 0, -1);
    } else {
        if (trig_at(when)->n >= PSX_CARD_BRANCHES) { say("That trigger already holds four effects"); return; }
        if (r.kind == RK_BONUS) bonus_remove(r.bi); else s_edit.battle = -1;
        PsxCardFxBranch b; branch_reset(&b);
        trig_append(trig_at(when), &b);
    }
}
static void enum_set(int f, int i)
{
    const int v = enum_value(f, i);
    switch (f) {
    case F_STAR1: s_edit.star1 = v; break;
    case F_STAR2: s_edit.star2 = v; break;
    case F_TYPE: s_edit.type = v; break;
    case F_ATTR: s_edit.attribute = v; break;
    case F_COLOR: s_edit.color = v; break;
    case F_EFFECT: s_edit.effect = v; break;
    case F_TARGET: s_edit.target = v; break;
    case F_TERRAIN: s_edit.terrain = v; break;
    case F_IMMUNE: s_edit.immune = v; break;
    default: {
        const Rule *r = rule_ptr(f);
        if (!r) return;
        if (is_when(f)) rule_set_when(f, v);
        else if (is_chance(f)) {
            PsxCardFxBranch *b = rule_branch(f);
            if (b) { if (v == CHANCE_ELSE) { b->is_else = 1; b->chance = 100; } else { b->is_else = 0; b->chance = v; } }
        } else if (is_trig(f)) {
            if (r->kind == RK_TRIG) {
                PsxCardFxBranch *b = rule_branch(f);
                if (b->fx != v) { b->fx = v; b->amount = -1; b->target = -1; b->terrain = -1; }
            } else if (v == PFX_BONUS) {
                if (r->kind == RK_BATTLE) { s_edit.battle = -1; bonus_append(500, 0, -1); }
            } else {
                if (r->kind == RK_BONUS) bonus_remove(r->bi);
                s_edit.battle = PFX_BATTLE[v];
            }
        } else if (is_param(f)) {
            PsxCardFxBranch *b = rule_branch(f);
            if (b) { if (param_kind(f) == 'f') b->terrain = v; else b->target = v; }
        } else if (is_per(f)) {
            PsxCardBonus *b = rule_bonus(f);
            if (!b) return;
            if (s_drop == f && s_drop_cards) { b->filter = v; }
            else if (i == PER_PICK_OWN || i == PER_PICK_ENEMY) { b->enemy = i == PER_PICK_ENEMY; drop_open_cards(f); s_changed = 1; return; }   /* the card list follows */
            else per_decode(i, &b->enemy, &b->filter);
        }
        break;
    }
    }
    rules_build();
    s_changed = 1; s_dirty = 1;
}
/* "+ Add effect": a new rule on the summon trigger, effect still to choose */
static void rule_add(void)
{
    PsxCardFxBranch b; branch_reset(&b);
    int t = 0;
    while (t < 6 && trig_at(t)->n >= PSX_CARD_BRANCHES) t++;
    if (t >= 6 || s_rule_n >= RULE_MAX) { say("No room for another effect on this card"); return; }
    trig_append(trig_at(t), &b);
    rules_build();
    s_changed = 1; s_dirty = 1;
}
static void rule_delete(int f)
{
    const Rule *r = rule_ptr(f);
    if (!r) return;
    if (r->kind == RK_TRIG) trig_remove(trig_at(r->t), r->k);
    else if (r->kind == RK_BONUS) bonus_remove(r->bi);
    else s_edit.battle = -1;
    rules_build();
    s_changed = 1; s_dirty = 1;
}

#define DROP_ROWS 12
static int field_is_set(int f);
/* The type the game will see: the edit when set, else stock. Magic, Trap,
 * Ritual and Equip (20..23) have no ATK/DEF, stars, level or attribute
 * anywhere the game draws them, so those fields are monster-only. */
static int effective_type(void) { return s_edit.type >= 0 ? s_edit.type : s_stock.type; }
static int is_monster(void) { return effective_type() < 20; }
/* Cards the game's magic dispatcher knows: 301..350, 651..700, 721. */
static int magic_dispatchable(int id) { return (id >= 301 && id <= 350) || (id >= 651 && id <= 700) || id == 721; }
static int eff_effect(void) { return s_edit.effect >= 0 ? s_edit.effect : s_stock.effect; }
static int field_tab(int f) { return f < F_FX_FIRST ? 0 : 1; }
/* whether the card's type has a use for the field (the tab aside) */
static int field_fits(int f)
{
    const int t = effective_type();
    switch (f) {
    case F_ATK: case F_DEF: case F_STAR1: case F_STAR2: case F_LEVEL: case F_ATTR: return is_monster();
    case F_EFFECT:  return t == 20 || t == 22;   /* any card the game plays as a spell; the hook rewrites the id */
    case F_AMOUNT: {
        if (!field_fits(F_EFFECT)) return 0;
        const int e = eff_effect();
        return e == PSX_CARD_FX_HEAL || e == PSX_CARD_FX_DAMAGE || e == PSX_CARD_FX_DESTROY_ATK || e == PSX_CARD_FX_WEAKEN || e == PSX_CARD_FX_LOSE_LP;
    }
    case F_TARGET:  return field_fits(F_EFFECT) && eff_effect() == PSX_CARD_FX_DESTROY_TYPE;
    case F_TERRAIN: return field_fits(F_EFFECT) && eff_effect() == PSX_CARD_FX_FIELD;
    case F_RITUAL:  return field_fits(F_EFFECT) && eff_effect() == PSX_CARD_FX_RITUAL;
    case F_EQUIP_BONUS: case F_EQUIPS: return t == 23;
    case F_BOOST:   return s_sel >= 330 && s_sel <= 335;
    case F_TRAP_MAX: return s_sel >= 681 && s_sel <= 686;
    case F_IMMUNE: return is_monster();
    default:
        if (is_rulef(f)) {
            const Rule *r = rule_ptr(f);
            if (!is_monster() || !r) return 0;
            switch (part_of(f)) {
            case RP_CHANCE: return r->kind == RK_TRIG;
            case RP_PARAM: return param_kind(f) != 0;
            case RP_PER: return r->kind == RK_BONUS;
            default: return 1;
            }
        }
        return 1;
    }
}
static int field_applies(int f) { return field_fits(f) && field_tab(f) == s_tab; }

static void layout_compute(void)
{
    Layout *L = &s_L;
    memset(L, 0, sizeof *L);
    const int gap = px(U_GAP), pad = px(U_PAD);
    L->bar = (Rect){ 0, 0, s_w, px(U_BAR_H) };
    L->search = (Rect){ px(118.0f), (L->bar.h - px(16.0f)) / 2, px(130.0f), px(16.0f) };
    const int top = L->bar.h + gap;
    L->list = (Rect){ gap, top, px(U_LIST_W), s_h - top - gap };
    L->row_h = px(U_ROW_H);
    L->list_rows = (Rect){ L->list.x, L->list.y + px(U_HDR_H), L->list.w - px(10.0f), L->list.h - px(U_HDR_H) - px(4.0f) };
    L->rows = L->list_rows.h / L->row_h; if (L->rows < 1) L->rows = 1;
    L->sb = (Rect){ L->list.x + L->list.w - px(8.0f), L->list_rows.y, px(4.0f), L->rows * L->row_h };
    L->ed = (Rect){ L->list.x + L->list.w + gap, top, s_w - (L->list.x + L->list.w + gap) - gap, s_h - top - gap };

    const int ex = L->ed.x + pad;
    const int right = L->ed.x + L->ed.w - pad;
    int y = L->ed.y + pad + px(16.0f) + px(8.0f);        /* below the header line */
    /* the two tabs */
    {
        const PsxUiFace *fb = face_bold();
        const int th = px(U_BTN_H);
        int tx = ex;
        for (int i = 0; i < 2; i++) {
            const int tw = psx_ui_font_text_w(fb, TAB_LABEL[i]) + px(26.0f);
            L->tab[i] = (Rect){ tx, y, tw, th };
            tx += tw + px(4.0f);
        }
        y += th + px(8.0f);
    }
    if (s_tab == 0) {
        /* previews */
        const int art_h = px(72.0f), art_w = art_h * 102 / 96;
        L->art = (Rect){ ex, y, art_w, art_h };
        const int th_h = px(24.0f), th_w = th_h * 40 / 32;
        L->thumb = (Rect){ ex + art_w + pad, y, th_w, th_h };
        L->info_x = L->thumb.x + th_w + pad;
        L->info_y = y;
        y += art_h + px(12.0f);
    }
    /* fields */
    const int label_w = px(U_LABEL_W), box_h = px(U_BOX_H), step_w = px(U_STEP_W), sgap = px(3.0f);
    const int desc_h = psx_ui_font_line_height(face_body()) * 6 + px(6.0f);   /* the game's six lines */
    const int rules_here = s_tab == 1 && is_monster();
    int trow_y = 0, trow_x = 0;
    if (rules_here) {
        /* the note above the list */
        y += px(2.0f);
        L->fx_note_x = ex; L->fx_note_y = y;
        y += psx_ui_font_line_height(face_small()) + px(6.0f);
    }
    for (int f = 0; f < F_COUNT; f++) {
        if (!field_applies(f)) {
            /* the monster-only rows collapse to one note line for a spell */
            if (f == F_ATK && s_tab == 0) { L->label[f] = (Rect){ ex, y, label_w, box_h }; y += px(U_FIELD_H); }
            continue;
        }
        if (is_rulef(f)) {
            /* When [when] [chance] do [effect] [parameter] per [per]                x */
            const Rule *r = rule_ptr(f);
            const int part = part_of(f);
            if (part == RP_WHEN) {
                trow_y = y;
                L->label[f] = (Rect){ ex, trow_y, px(34.0f), box_h };
                L->value[f] = (Rect){ ex + px(34.0f), trow_y, px(118.0f), box_h };
                L->clear[f] = (Rect){ right - step_w, trow_y, step_w, box_h };
                trow_x = L->value[f].x + L->value[f].w + px(4.0f);
                y += px(U_FIELD_H);
            } else if (part == RP_CHANCE) {
                L->value[f] = (Rect){ trow_x, trow_y, px(66.0f), box_h };
                trow_x += px(66.0f) + px(4.0f);
            } else if (part == RP_FX) {
                L->label[f] = (Rect){ trow_x, trow_y, px(16.0f), box_h };
                trow_x += px(16.0f);
                const int w = r->kind == RK_TRIG ? px(168.0f) : px(126.0f);
                L->value[f] = (Rect){ trow_x, trow_y, w, box_h };
                trow_x += w + px(4.0f);
            } else if (part == RP_PARAM) {
                const int w = param_kind(f) == 'a' ? px(46.0f) : px(62.0f);
                L->value[f] = (Rect){ trow_x, trow_y, w, box_h };
                trow_x += w + px(4.0f);
            } else {
                L->label[f] = (Rect){ trow_x, trow_y, px(18.0f), box_h };
                trow_x += px(18.0f);
                int w = right - step_w - sgap * 2 - trow_x; if (w < px(60.0f)) w = px(60.0f);
                L->value[f] = (Rect){ trow_x, trow_y, w, box_h };
            }
            continue;
        }
        if (f == F_IMMUNE) {
            /* "+ Add effect" sits between the rules and the immunity row */
            const PsxUiFace *fb = face_bold();
            const int bh = px(U_BTN_H), bw = psx_ui_font_text_w(fb, BTN_LABEL[B_ADD_RULE]) + px(18.0f);
            L->btn[B_ADD_RULE] = (Rect){ ex, y + px(2.0f), bw, bh };
            y += bh + px(10.0f);
        }
        if (f >= F_FX_FIRST && f < F_IMMUNE && !L->fx_note_y) {
            y += px(2.0f);
            L->fx_note_x = ex; L->fx_note_y = y;
            y += psx_ui_font_line_height(face_small()) + px(6.0f);
        }
        const int fh = (f == F_DESC) ? desc_h + px(3.0f) + psx_ui_font_line_height(face_small()) + px(2.0f) : px(U_FIELD_H);
        L->label[f] = (Rect){ ex, y, label_w, box_h };
        int vx = ex + label_w, vw;
        if (field_is_enum(f)) {
            L->step_l[f] = (Rect){ vx, y, step_w, box_h };
            vx += step_w + sgap;
            vw = (f == F_EFFECT) ? px(190.0f) : (f == F_COLOR || f == F_IMMUNE) ? px(120.0f) : px(90.0f);
            L->value[f] = (Rect){ vx, y, vw, box_h };
            L->step_r[f] = (Rect){ vx + vw + sgap, y, step_w, box_h };
            L->clear[f] = (Rect){ L->step_r[f].x + step_w + sgap * 2, y, step_w, box_h };
        } else {
            const int wide = (f == F_EQUIPS || f == F_BOOST || f == F_RITUAL);
            vw = (f == F_NAME) ? px(170.0f) : (f == F_DESC) ? px(300.0f) : wide ? (right - vx - step_w - sgap * 2) : px(70.0f);
            if (vw > right - vx - step_w - sgap * 2) vw = right - vx - step_w - sgap * 2;
            if (vw < px(40.0f)) vw = px(40.0f);
            L->value[f] = (Rect){ vx, y, vw, (f == F_DESC) ? desc_h : box_h };
            L->clear[f] = (Rect){ vx + vw + sgap * 2, y, step_w, box_h };
        }
        y += fh;
    }
    if (rules_here) {
        /* what the card would say, under the list */
        y += px(2.0f);
        L->preview_y = y;
        y += psx_ui_font_line_height(face_small()) * 4 + px(4.0f);
    }
    y += px(6.0f);
    /* buttons: flow layout, wrapping to a new line when the panel runs out */
    {
        const PsxUiFace *fb = face_bold();
        int bx = ex, bh = px(U_BTN_H);
        snprintf(s_dev_label, sizeof s_dev_label, "Dev Card Effects: %s", psx_card_packs_is_dev() ? "ON" : "OFF");
        /* the bar's buttons, right to left, in the small face so five fit */
        {
            const PsxUiFace *fs = face_small();
            int rx = s_w - px(8.0f);
            for (int b = B_COUNT - 1; b >= B_BAR_FIRST; b--) {
                const int bw = psx_ui_font_text_w(fs, b == B_DEV ? s_dev_label : BTN_LABEL[b]) + px(14.0f);
                rx -= bw;
                L->btn[b] = (Rect){ rx, (L->bar.h - bh) / 2, bw, bh };
                rx -= px(4.0f);
            }
            L->bar_free = rx - (L->search.x + L->search.w);
        }
        for (int b = 0; b < B_BAR_FIRST; b++) {
            if (b == B_ADD_RULE) continue;                    /* placed with the rules */
            const int bw = psx_ui_font_text_w(fb, BTN_LABEL[b]) + px(18.0f);
            if (bx + bw > right && bx > ex) { bx = ex; y += bh + px(5.0f); }
            L->btn[b] = (Rect){ bx, y, bw, bh };
            bx += bw + px(5.0f);
        }
        y += bh + px(8.0f);
    }
    L->status_y = y;
    /* the import preview panel, centred */
    {
        const int mw = px(360.0f), mh = px(s_modal == MODAL_ACTIVATE ? 112.0f : 200.0f);
        L->modal = (Rect){ (s_w - mw) / 2, (s_h - mh) / 2, mw, mh };
        const int bw = px(80.0f), bh = px(U_BTN_H);
        L->modal_ok = (Rect){ L->modal.x + L->modal.w - pad - bw * 2 - px(6.0f), L->modal.y + L->modal.h - pad - bh, bw, bh };
        L->modal_cancel = (Rect){ L->modal.x + L->modal.w - pad - bw, L->modal.y + L->modal.h - pad - bh, bw, bh };
    }
}

/* --- text helpers -------------------------------------------------------------- */
/* Greedy word wrap of `s` into at most `max_lines` lines that each fit
 * `max_w`; a word longer than the box is split. Returns the line count. */
static int wrap_text(const PsxUiFace *f, const char *s, int max_w, char lines[][256], int max_lines)
{
    int n = 0;
    const char *p = s;
    while (*p && n < max_lines) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *q = p, *last_space = NULL;
        while (*q) {
            if (*q == ' ') last_space = q;
            if (psx_ui_font_text_w_n(f, p, (int)(q - p) + 1) > max_w) break;
            q++;
        }
        if (*q && last_space && last_space > p) q = last_space;
        if (q == p) q = p + 1;                       /* never stall on a too-narrow box */
        int len = (int)(q - p); if (len > 255) len = 255;
        memcpy(lines[n], p, (size_t)len); lines[n][len] = 0;
        n++;
        p = q;
    }
    return n;
}

static int draw_wrapped(int x, int y, int max_w, const char *s, uint32_t col, const PsxUiFace *f, int max_lines)
{
    char lines[8][256];
    if (max_lines > 8) max_lines = 8;
    const int n = wrap_text(f, s, max_w, lines, max_lines);
    const int lh = psx_ui_font_line_height(f);
    for (int i = 0; i < n; i++)
        psx_ui_text(&s_cv, x, y + i * lh + psx_ui_font_ascent(f), lines[i], col, f);
    return y + n * lh;
}

static void text_in(const Rect *r, int inset, const char *s, uint32_t col, const PsxUiFace *f)
{
    psx_ui_text_clip(&s_cv, r->x + inset, psx_ui_baseline_in(r->y, r->h, f), s, col, f, r->w - inset * 2);
}

static void text_centered(const Rect *r, const char *s, uint32_t col, const PsxUiFace *f)
{
    const int w = psx_ui_font_text_w(f, s);
    psx_ui_text(&s_cv, r->x + (r->w - w) / 2, psx_ui_baseline_in(r->y, r->h, f), s, col, f);
}

/* --- helpers ------------------------------------------------------------------ */
static void say(const char *m)
{
    snprintf(s_msg, sizeof s_msg, "%s", m);
    s_msg_until = SDL_GetTicks() + 3500u;
    s_dirty = 1;
}

static int ci_contains(const char *hay, const char *needle)
{
    const size_t n = strlen(needle);
    if (!n) return 1;
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] && (hay[i] | 32) == (needle[i] | 32)) i++;
        if (i == n) return 1;
    }
    return 0;
}

static void clamp_scroll(void)
{
    int m = s_order_n - s_L.rows; if (m < 0) m = 0;
    if (s_scroll > m) s_scroll = m;
    if (s_scroll < 0) s_scroll = 0;
}

static void rebuild_order(void)
{
    s_order_n = 0;
    const int numeric = s_search[0] >= '0' && s_search[0] <= '9';
    for (int id = 1; id <= CARDS; id++) {
        if (numeric) {
            char idbuf[8]; snprintf(idbuf, sizeof idbuf, "%d", id);
            if (strncmp(idbuf, s_search, strlen(s_search)) != 0) continue;
        } else if (!ci_contains(psx_card_db_name(id), s_search)) {
            continue;
        }
        s_order[s_order_n++] = id;
    }
    clamp_scroll();
    s_dirty = 1;
}

static void load_editor(void)
{
    s_focus = -1;
    s_drop = -1;
    s_has_pack = psx_card_packs_get(s_sel, &s_edit);
    if (!s_has_pack) {
        memset(&s_edit, 0, sizeof s_edit);
        s_edit.id = s_sel;
        s_edit.attack = s_edit.defense = s_edit.star1 = s_edit.star2 = s_edit.type =
            s_edit.level = s_edit.attribute = s_edit.price = -1;
        psx_card_packs_effects_reset(&s_edit);
    }
    s_stock_ok = psx_card_packs_stock(s_sel, &s_stock);
    rules_build();
    s_changed = 0;
    s_art_id = -1;
    s_dirty = 1;
}

static void select_card(int id)
{
    if (id < 1 || id > CARDS) return;
    s_sel = id;
    load_editor();
}

static void refresh_preview(void)
{
    const unsigned gen = psx_card_packs_generation();
    if (s_art_id == s_sel && s_art_gen == gen) return;
    s_art_ok = psx_card_packs_art_rgb(s_sel, s_art);
    s_thumb_ok = psx_card_packs_thumb_rgb(s_sel, s_thumb);
    for (int i = 0; i < 102 * 96; i++) s_art_argb[i] = 0xFF000000u | ((uint32_t)s_art[i*3] << 16) | ((uint32_t)s_art[i*3+1] << 8) | s_art[i*3+2];
    for (int i = 0; i < 40 * 32; i++) s_thumb_argb[i] = 0xFF000000u | ((uint32_t)s_thumb[i*3] << 16) | ((uint32_t)s_thumb[i*3+1] << 8) | s_thumb[i*3+2];
    s_art_id = s_sel; s_art_gen = gen;
    s_dirty = 1;
}

static int field_is_set(int f)
{
    switch (f) {
    case F_NAME: return s_edit.name[0] != 0;
    case F_DESC: return s_edit.description[0] != 0;
    case F_ATK: return s_edit.attack >= 0;
    case F_DEF: return s_edit.defense >= 0;
    case F_STAR1: return s_edit.star1 >= 1;
    case F_STAR2: return s_edit.star2 >= 1;
    case F_TYPE: return s_edit.type >= 0;
    case F_LEVEL: return s_edit.level >= 0;
    case F_ATTR: return s_edit.attribute >= 0;
    case F_PRICE: return s_edit.price >= 0;
    case F_PASSWORD: return s_edit.password[0] != 0;
    case F_COLOR: return s_edit.color >= 0;
    case F_EFFECT: return s_edit.effect >= 0;
    case F_AMOUNT: return s_edit.amount != -1;
    case F_TARGET: return s_edit.target >= 0;
    case F_TERRAIN: return s_edit.terrain >= 1;
    case F_RITUAL: return s_edit.ritual_set;
    case F_EQUIP_BONUS: return s_edit.equip_bonus >= 0;
    case F_EQUIPS: return s_edit.equips_set || s_edit.equip_types != 0;
    case F_BOOST: return s_edit.boost_set;
    case F_TRAP_MAX: return s_edit.trap_atk_max >= 0;
    case F_IMMUNE: return s_edit.immune >= 0;
    default:
        return is_when(f);                /* a rule's row carries one x, on its When box */
    }
    return 0;
}

static void field_clear(int f)
{
    switch (f) {
    case F_NAME: s_edit.name[0] = 0; break;
    case F_DESC: s_edit.description[0] = 0; break;
    case F_ATK: s_edit.attack = -1; break;
    case F_DEF: s_edit.defense = -1; break;
    case F_STAR1: s_edit.star1 = -1; break;
    case F_STAR2: s_edit.star2 = -1; break;
    case F_TYPE: s_edit.type = -1; break;
    case F_LEVEL: s_edit.level = -1; break;
    case F_ATTR: s_edit.attribute = -1; break;
    case F_PRICE: s_edit.price = -1; break;
    case F_PASSWORD: s_edit.password[0] = 0; break;
    case F_COLOR: s_edit.color = -1; break;
    case F_EFFECT: s_edit.effect = -1; break;
    case F_AMOUNT: s_edit.amount = -1; break;
    case F_TARGET: s_edit.target = -1; break;
    case F_TERRAIN: s_edit.terrain = -1; break;
    case F_RITUAL: s_edit.ritual_set = 0; s_edit.ritual_mat[0] = s_edit.ritual_mat[1] = s_edit.ritual_mat[2] = s_edit.ritual_result = -1; break;
    case F_EQUIP_BONUS: s_edit.equip_bonus = -1; break;
    case F_EQUIPS: s_edit.equips_set = 0; s_edit.equip_types = 0; s_edit.equip_n = 0; break;
    case F_BOOST: s_edit.boost_set = 0; for (int t = 0; t < 20; t++) s_edit.boost[t] = PSX_CARD_PACK_BOOST_UNSET; break;
    case F_TRAP_MAX: s_edit.trap_atk_max = -1; break;
    case F_IMMUNE: s_edit.immune = -1; break;
    default:
        if (is_when(f)) { rule_delete(f); return; }
        if (is_param(f)) {
            PsxCardFxBranch *b = rule_branch(f); const int k = param_kind(f);
            if (b) { if (k == 'a') b->amount = -1; else if (k == 't') b->target = -1; else b->terrain = -1; }
        }
        break;
    }
    s_changed = 1; s_dirty = 1;
}

static void field_text(int f, int stock, char *out, size_t cap)
{
    const int set = field_is_set(f) && !stock;
    if (!s_stock_ok && stock) { snprintf(out, cap, "-"); return; }
    switch (f) {
    case F_NAME: snprintf(out, cap, "%s", set ? s_edit.name : s_stock.name); break;
    case F_DESC: snprintf(out, cap, "%s", set ? s_edit.description : s_stock.description); break;
    case F_ATK: snprintf(out, cap, "%d", set ? s_edit.attack : s_stock.attack); break;
    case F_DEF: snprintf(out, cap, "%d", set ? s_edit.defense : s_stock.defense); break;
    case F_STAR1: snprintf(out, cap, "%s", psx_card_packs_star_name(set ? s_edit.star1 : s_stock.star1)); break;
    case F_STAR2: snprintf(out, cap, "%s", psx_card_packs_star_name(set ? s_edit.star2 : s_stock.star2)); break;
    case F_TYPE: snprintf(out, cap, "%s", psx_card_packs_type_name(set ? s_edit.type : s_stock.type)); break;
    case F_LEVEL: snprintf(out, cap, "%d", set ? s_edit.level : s_stock.level); break;
    case F_ATTR: snprintf(out, cap, "%s", psx_card_packs_attribute_name(set ? s_edit.attribute : s_stock.attribute)); break;
    case F_PRICE: snprintf(out, cap, "%d", set ? s_edit.price : s_stock.price); break;
    case F_PASSWORD: snprintf(out, cap, "%s", set ? s_edit.password : (s_stock.password[0] ? s_stock.password : "none")); break;
    case F_COLOR: snprintf(out, cap, "%s", psx_card_packs_color_name(set ? s_edit.color : (stock ? s_stock.color : psx_card_colors_slot(s_sel)))); break;
    case F_EFFECT: {
        const int e = set ? s_edit.effect : s_stock.effect;
        snprintf(out, cap, "%s", e >= 0 ? psx_card_packs_effect_label(e) : "code only");
        break;
    }
    case F_AMOUNT:
        if (set) snprintf(out, cap, "%d", s_edit.amount);
        else if (s_stock.amount >= 0 && s_stock.effect == eff_effect()) snprintf(out, cap, "%d", s_stock.amount);
        else snprintf(out, cap, "%d", eff_effect() == PSX_CARD_FX_DESTROY_ATK ? 1500 : 500);   /* the layer's default */
        break;
    case F_TARGET: snprintf(out, cap, "%s", psx_card_packs_type_name(set ? s_edit.target : (s_stock.effect == PSX_CARD_FX_DESTROY_TYPE && s_stock.amount >= 0 ? s_stock.amount : 3))); break;
    case F_TERRAIN: snprintf(out, cap, "%s", psx_card_packs_terrain_name(set ? s_edit.terrain : (s_stock.effect == PSX_CARD_FX_FIELD && s_stock.amount >= 1 ? s_stock.amount : 1))); break;
    case F_RITUAL:
        if (set) psx_card_packs_format_ritual(&s_edit, out, (unsigned)cap);
        else if (s_stock.ritual_result >= 1) snprintf(out, cap, "%d, %d, %d -> %d", s_stock.ritual_mat[0], s_stock.ritual_mat[1], s_stock.ritual_mat[2], s_stock.ritual_result);
        else snprintf(out, cap, "none");
        break;
    case F_EQUIP_BONUS: snprintf(out, cap, "%d", set ? s_edit.equip_bonus : s_stock.equip_bonus); break;
    case F_EQUIPS:
        if (set) psx_card_packs_format_equips(&s_edit, out, (unsigned)cap);
        else snprintf(out, cap, "stock list");
        break;
    case F_BOOST:
        if (set) psx_card_packs_format_boost(&s_edit, out, (unsigned)cap);
        else { PsxCardPack tmp; memset(&tmp, 0, sizeof tmp); memcpy(tmp.boost, s_stock.boost, sizeof tmp.boost); psx_card_packs_format_boost(&tmp, out, (unsigned)cap); }
        break;
    case F_TRAP_MAX: snprintf(out, cap, "%d", set ? s_edit.trap_atk_max : s_stock.trap_atk_max); break;
    case F_IMMUNE: snprintf(out, cap, "%s", IMMUNE_LABEL[set ? s_edit.immune : 0]); break;
    default: {
        const Rule *r = rule_ptr(f);
        out[0] = 0;
        if (!r || stock) break;
        if (is_when(f) || is_chance(f) || is_trig(f)) snprintf(out, cap, "%s", enum_label(f, enum_current_index(f)));
        else if (is_param(f)) {
            const int k = param_kind(f);
            if (r->kind == RK_BONUS) snprintf(out, cap, "%d", rule_bonus(f)->amount);
            else if (k == 'a' && rule_branch(f)) { const PsxCardFxBranch *b = rule_branch(f); snprintf(out, cap, "%d", b->amount != -1 ? b->amount : (b->fx == PSX_CARD_FX_DESTROY_ATK ? 1500 : 500)); }
            else if (k) snprintf(out, cap, "%s", enum_label(f, enum_current_index(f)));
        } else if (is_per(f)) { const PsxCardBonus *b = rule_bonus(f); if (b) snprintf(out, cap, "%s", per_label(b->enemy, b->filter)); }
        break;
    }
    }
}

static void field_step(int f, int dir)
{
    const int n = enum_count(f);
    if (n <= 0) return;
    int i = enum_current_index(f) + dir;
    if (i >= n) i = 0;
    if (i < 0) i = n - 1;
    enum_set(f, i);
}

/* Editing starts from what the box shows, so a stock description can be
 * touched up rather than retyped; committing the stock text unchanged
 * leaves the field at stock. Boxes that show a placeholder rather than a
 * value ("stock list", "none") start empty. */
static int field_placeholder(int f)
{
    if (f == F_EQUIPS && !field_is_set(f)) return 1;
    if (f == F_PASSWORD && !field_is_set(f) && !s_stock.password[0]) return 1;
    return 0;
}

/* --- text editing: caret, selection, clipboard --------------------------------
 * s_buf is the text being edited; s_car is the caret (a byte index), s_anchor
 * the other end of the selection or -1. The description box wraps like the
 * game does ("|" is a hard line break), other boxes are one line that scrolls
 * to keep the caret in view. */
static int s_car, s_anchor = -1, s_drag_text;
#if defined(PSX_SDL3)
#define KEY_MOD(ev) ((ev)->key.mod)
#else
#define KEY_MOD(ev) ((ev)->key.keysym.mod)
#endif
static size_t field_cap(int f)
{
    return f == F_NAME ? PSX_CARD_PACK_NAME_MAX : f == F_DESC ? PSX_CARD_PACK_DESC_MAX :
           f == F_PASSWORD ? 8 : f == F_EQUIPS ? FTEXT - 8 : f == F_BOOST ? 500 : f == F_RITUAL ? 40 : 6;
}
static int buf_len(void) { return (int)strlen(s_buf); }
static int sel_lo(void) { return s_anchor < 0 ? s_car : (s_anchor < s_car ? s_anchor : s_car); }
static int sel_hi(void) { return s_anchor < 0 ? s_car : (s_anchor > s_car ? s_anchor : s_car); }
static int has_sel(void) { return s_anchor >= 0 && s_anchor != s_car; }
static void caret_clamp(void) { const int n = buf_len(); if (s_car < 0) s_car = 0; if (s_car > n) s_car = n; if (s_anchor > n) s_anchor = n; }
static void sel_delete(void)
{
    if (!has_sel()) { s_anchor = -1; return; }
    const int lo = sel_lo(), hi = sel_hi();
    memmove(s_buf + lo, s_buf + hi, strlen(s_buf + hi) + 1);
    s_car = lo; s_anchor = -1;
}
/* insert printable text at the caret (a selection is replaced); newlines
 * become the game's "|" in the description and are dropped elsewhere */
static void text_insert(const char *s)
{
    if (s_focus < 0) return;
    sel_delete();
    const size_t cap = field_cap(s_focus);
    for (const char *p = s; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch == '\r') continue;
        if (ch == '\n') { if (s_focus != F_DESC) continue; ch = '|'; }
        if (ch < 32u || ch >= 127u) continue;
        const size_t n = strlen(s_buf);
        if (n >= cap) break;
        memmove(s_buf + s_car + 1, s_buf + s_car, n - (size_t)s_car + 1);
        s_buf[s_car++] = (char)ch;
    }
    s_dirty = 1;
}
static void clip_copy(int cut)
{
    if (!has_sel()) return;
    char tmp[FTEXT];
    const int lo = sel_lo(), hi = sel_hi();
    memcpy(tmp, s_buf + lo, (size_t)(hi - lo)); tmp[hi - lo] = 0;
    SDL_SetClipboardText(tmp);
    if (cut) { sel_delete(); s_dirty = 1; }
}
static void clip_paste(void)
{
    char *t = SDL_GetClipboardText();
    if (!t) return;
    text_insert(t);
    SDL_free(t);
}
static void select_all(void) { s_anchor = 0; s_car = buf_len(); s_dirty = 1; }
static void select_word(void)
{
    const int n = buf_len();
    int a = s_car, b = s_car;
    while (a > 0 && s_buf[a - 1] != ' ' && s_buf[a - 1] != '|') a--;
    while (b < n && s_buf[b] != ' ' && s_buf[b] != '|') b++;
    s_anchor = a; s_car = b; s_dirty = 1;
}

/* Visual lines of the text being edited: byte ranges [a,b) of s_buf. */
typedef struct { int a, b; } Span;
#define SPAN_MAX 24
static int text_spans(const PsxUiFace *f, const char *s, int max_w, int hard_bar, Span *sp, int max)
{
    int n = 0;
    const int len = (int)strlen(s);
    int p = 0;
    for (;;) {
        if (n >= max) break;
        int q = p, last_space = -1, hard = 0;
        while (q < len) {
            if (hard_bar && s[q] == '|') { hard = 1; break; }
            if (psx_ui_font_text_w_n(f, s + p, q - p + 1) > max_w) break;
            if (s[q] == ' ') last_space = q;
            q++;
        }
        if (q < len && !hard) {
            /* wrap: after the last space when there is one, else mid-word */
            if (last_space >= 0 && last_space + 1 > p) q = last_space + 1;
            if (q == p) q = p + 1;
        }
        sp[n].a = p; sp[n].b = q; n++;
        if (q >= len) break;
        p = hard ? q + 1 : q;
        if (hard && p >= len && n < max) { sp[n].a = p; sp[n].b = p; n++; break; }   /* a trailing break: an empty last line */
    }
    if (!n) { sp[0].a = sp[0].b = 0; n = 1; }
    return n;
}
static int span_of(const Span *sp, int n, int i)
{
    for (int k = 0; k < n; k++) {
        if (i < sp[k].b) return k;
        if (i == sp[k].b) { if (k + 1 < n && sp[k + 1].a == sp[k].b) continue; return k; }   /* a soft wrap: the next line */
    }
    return n - 1;
}
/* the layout of the focused box: spans, the first visible line and the
 * horizontal skip of a one-line box */
static int focus_layout(Span *sp, int *first, int *skip)
{
    const PsxUiFace *fb = face_body();
    const Rect *v = &s_L.value[s_focus];
    const int inner = v->w - px(12.0f);
    *first = 0; *skip = 0;
    if (s_focus == F_DESC) {
        const int n = text_spans(fb, s_buf, inner, 1, sp, SPAN_MAX);
        const int lh = psx_ui_font_line_height(fb);
        const int fitl = (v->h - px(4.0f)) / lh;
        const int cl = span_of(sp, n, s_car);
        if (cl >= fitl) *first = cl - fitl + 1;
        return n;
    }
    sp[0].a = 0; sp[0].b = buf_len();
    /* one line: skip leading characters until the caret fits */
    int st = 0;
    while (st < s_car && psx_ui_font_text_w_n(fb, s_buf + st, s_car - st) > inner) st++;
    *skip = st;
    return 1;
}
/* the byte index under a point inside the focused box */
static int text_index_at(int x, int y)
{
    const PsxUiFace *fb = face_body();
    const Rect *v = &s_L.value[s_focus];
    Span sp[SPAN_MAX]; int first, skip;
    const int n = focus_layout(sp, &first, &skip);
    int line = 0;
    if (s_focus == F_DESC) {
        const int lh = psx_ui_font_line_height(fb);
        line = first + (y - v->y - px(2.0f)) / lh;
        if (line < 0) line = 0;
        if (line >= n) line = n - 1;
    }
    const int a = (s_focus == F_DESC) ? sp[line].a : skip, b = sp[line].b;
    const int rx = x - (v->x + px(6.0f));
    int i = a;
    while (i < b) {
        const int w0 = psx_ui_font_text_w_n(fb, s_buf + a, i - a), w1 = psx_ui_font_text_w_n(fb, s_buf + a, i - a + 1);
        if (rx < (w0 + w1) / 2) break;
        i++;
    }
    /* clicking past a soft wrap's trailing space lands before it */
    if (i == b && b > a && s_buf[b - 1] == ' ' && line + 1 < n && sp[line + 1].a == b) i = b - 1;
    return i;
}
static void caret_move(int to, int extend)
{
    if (extend) { if (s_anchor < 0) s_anchor = s_car; }
    else s_anchor = -1;
    s_car = to; caret_clamp();
    s_dirty = 1;
}
/* up/down in the description: the same x on the neighbouring line */
static void caret_line(int dir, int extend)
{
    const PsxUiFace *fb = face_body();
    Span sp[SPAN_MAX]; int first, skip;
    const int n = focus_layout(sp, &first, &skip);
    const int cl = span_of(sp, n, s_car);
    const int nl = cl + dir;
    if (nl < 0 || nl >= n) { caret_move(dir < 0 ? 0 : buf_len(), extend); return; }
    const int cx = psx_ui_font_text_w_n(fb, s_buf + sp[cl].a, s_car - sp[cl].a);
    int i = sp[nl].a;
    while (i < sp[nl].b) {
        const int w0 = psx_ui_font_text_w_n(fb, s_buf + sp[nl].a, i - sp[nl].a), w1 = psx_ui_font_text_w_n(fb, s_buf + sp[nl].a, i - sp[nl].a + 1);
        if (cx < (w0 + w1) / 2) break;
        i++;
    }
    if (i == sp[nl].b && sp[nl].b > sp[nl].a && nl + 1 < n && sp[nl + 1].a == sp[nl].b) i--;
    caret_move(i, extend);
}
/* the focused box: selection, text and caret */
static void draw_focused_text(int f)
{
    const PsxUiFace *fb = face_body();
    const Rect *v = &s_L.value[f];
    Span sp[SPAN_MAX]; int first, skip;
    const int n = focus_layout(sp, &first, &skip);
    const int lh = psx_ui_font_line_height(fb);
    const int fitl = (f == F_DESC) ? (v->h - px(4.0f)) / lh : 1;
    const int x0 = v->x + px(6.0f), inner = v->w - px(12.0f);
    const int lo = sel_lo(), hi = sel_hi();
    for (int k = first; k < n && k - first < fitl; k++) {
        const int a = (f == F_DESC) ? sp[k].a : skip, b = sp[k].b;
        const int ly = (f == F_DESC) ? v->y + px(2.0f) + (k - first) * lh : v->y + (v->h - lh) / 2;
        const int base = ly + psx_ui_font_ascent(fb);
        if (has_sel() && hi > a && lo < b) {
            const int sa = lo > a ? lo : a, sb = hi < b ? hi : b;
            const int xa = psx_ui_font_text_w_n(fb, s_buf + a, sa - a), xb = psx_ui_font_text_w_n(fb, s_buf + a, sb - a);
            int w = xb - xa; if (w < px(2.0f)) w = px(2.0f);
            if (xa < inner) psx_ui_fill(&s_cv, x0 + xa, ly, (xa + w > inner ? inner - xa : w), lh, COL_SEL_BG);
        }
        char line[FTEXT];
        int len = b - a; if (len > (int)sizeof line - 1) len = (int)sizeof line - 1;
        memcpy(line, s_buf + a, (size_t)len); line[len] = 0;
        psx_ui_text_clip(&s_cv, x0, base, line, COL_TEXT, fb, inner + px(4.0f));
        if (s_caret_on && span_of(sp, n, s_car) == k && s_car >= a) {
            const int cx = x0 + psx_ui_font_text_w_n(fb, s_buf + a, s_car - a);
            if (cx <= x0 + inner + 1) psx_ui_fill(&s_cv, cx, ly, 1, lh, COL_TEXT);
        }
    }
}
/* a key while a box has focus; returns 1 when handled */
static int text_key(int key, int mod)
{
    const int shift = (mod & KMOD_SHIFT) != 0, ctrl = (mod & (KMOD_CTRL | KMOD_GUI)) != 0;
    caret_clamp();
    if (ctrl) {
        if (key == 'a') { select_all(); return 1; }
        if (key == 'c') { clip_copy(0); return 1; }
        if (key == 'x') { clip_copy(1); return 1; }
        if (key == 'v') { clip_paste(); return 1; }
        if (key == SDLK_LEFT || key == SDLK_RIGHT) {
            /* by word */
            int i = s_car;
            if (key == SDLK_LEFT) { while (i > 0 && (s_buf[i - 1] == ' ' || s_buf[i - 1] == '|')) i--; while (i > 0 && s_buf[i - 1] != ' ' && s_buf[i - 1] != '|') i--; }
            else { const int n = buf_len(); while (i < n && s_buf[i] != ' ' && s_buf[i] != '|') i++; while (i < n && (s_buf[i] == ' ' || s_buf[i] == '|')) i++; }
            caret_move(i, shift); return 1;
        }
    }
    switch (key) {
    case SDLK_LEFT:  if (has_sel() && !shift) caret_move(sel_lo(), 0); else caret_move(s_car - 1, shift); return 1;
    case SDLK_RIGHT: if (has_sel() && !shift) caret_move(sel_hi(), 0); else caret_move(s_car + 1, shift); return 1;
    case SDLK_HOME:  caret_move(0, shift); return 1;
    case SDLK_END:   caret_move(buf_len(), shift); return 1;
    case SDLK_UP:    if (s_focus == F_DESC) { caret_line(-1, shift); return 1; } return 0;
    case SDLK_DOWN:  if (s_focus == F_DESC) { caret_line(+1, shift); return 1; } return 0;
    case SDLK_BACKSPACE:
        if (has_sel()) sel_delete();
        else if (s_car > 0) { memmove(s_buf + s_car - 1, s_buf + s_car, strlen(s_buf + s_car) + 1); s_car--; }
        s_anchor = -1; s_dirty = 1; return 1;
    case SDLK_DELETE:
        if (has_sel()) sel_delete();
        else if (s_car < buf_len()) memmove(s_buf + s_car, s_buf + s_car + 1, strlen(s_buf + s_car + 1) + 1);
        s_anchor = -1; s_dirty = 1; return 1;
    default: return 0;
    }
}

static void focus_begin(int f)
{
    s_focus = f;
    if (field_placeholder(f)) s_buf[0] = 0;
    else field_text(f, 0, s_buf, sizeof s_buf);
    s_car = buf_len(); s_anchor = -1; s_drag_text = 0;
    s_dirty = 1;
}

static void focus_commit(void)
{
    const int f = s_focus;
    s_focus = -1;
    s_dirty = 1;
    if (f < 0) return;
    if (!s_buf[0]) { field_clear(f); return; }
    if (!is_param(f)) {
        /* the stock text, unchanged: nothing to keep */
        char st[FTEXT]; field_text(f, 1, st, sizeof st);
        if (!strcmp(st, s_buf)) { if (field_is_set(f)) field_clear(f); return; }
    }
    const int v = atoi(s_buf);
    switch (f) {
    case F_NAME: snprintf(s_edit.name, sizeof s_edit.name, "%s", s_buf); break;
    case F_DESC: snprintf(s_edit.description, sizeof s_edit.description, "%s", s_buf); break;
    case F_ATK: if (v < 0 || v > 5110) { say("Attack is 0 to 5110"); return; } s_edit.attack = v / 10 * 10; break;
    case F_DEF: if (v < 0 || v > 5110) { say("Defense is 0 to 5110"); return; } s_edit.defense = v / 10 * 10; break;
    case F_LEVEL: if (v < 0 || v > 12) { say("Level is 0 to 12"); return; } s_edit.level = v; break;
    case F_PRICE: if (v < 0 || v > 999999) { say("Price is 0 to 999999"); return; } s_edit.price = v; break;
    case F_PASSWORD: {
        int ok = strlen(s_buf) == 8;
        for (int i = 0; ok && i < 8; i++) if (s_buf[i] < '0' || s_buf[i] > '9') ok = 0;
        if (!ok) { say("A password is 8 digits"); return; }
        memcpy(s_edit.password, s_buf, 9);
        break;
    }
    case F_AMOUNT: {
        const int e = eff_effect();
        if (e == PSX_CARD_FX_HEAL && (v < 0 || v > 25500)) { say("Heal is 0 to 25500, in steps of 100"); return; }
        if (e == PSX_CARD_FX_DAMAGE && (v < 0 || v > 2550)) { say("Damage is 0 to 2550, in steps of 10"); return; }
        if (e == PSX_CARD_FX_DESTROY_ATK && (v < 210 || v > 2550)) { say("The ATK threshold is 210 to 2550"); return; }
        if (e == PSX_CARD_FX_WEAKEN && (v < -9990 || v > 9990)) { say("Weaken is -9990 to 9990 (negative strengthens)"); return; }
        if (e == PSX_CARD_FX_LOSE_LP && (v < 0 || v > 9999)) { say("LP lost is 0 to 9999"); return; }
        s_edit.amount = (e == PSX_CARD_FX_HEAL) ? v / 100 * 100 : (e == PSX_CARD_FX_LOSE_LP) ? v : v / 10 * 10;
        break;
    }
    case F_EQUIP_BONUS: if (v < 0 || v > 9990) { say("The equip bonus is 0 to 9990"); return; } s_edit.equip_bonus = v / 10 * 10; break;
    case F_TRAP_MAX: if (v < 0 || v > 25500) { say("The ceiling is 0 to 25500, in steps of 100"); return; } s_edit.trap_atk_max = v / 100 * 100; break;
    case F_RITUAL: { char err[96]; if (!psx_card_packs_parse_ritual(s_buf, &s_edit, err, sizeof err)) { say(err); return; } break; }
    case F_EQUIPS: { char err[96]; if (!psx_card_packs_parse_equips(s_buf, &s_edit, err, sizeof err)) { say(err); return; } break; }
    case F_BOOST:  { char err[96]; if (!psx_card_packs_parse_boost(s_buf, &s_edit, err, sizeof err)) { say(err); return; } break; }
    default: {
        if (!is_param(f) || param_kind(f) != 'a') return;
        if (rule_bonus(f)) {
            if (v < -9990 || v > 9990) { say("A bonus is -9990 to 9990"); return; }
            rule_bonus(f)->amount = v / 10 * 10;
            break;
        }
        PsxCardFxBranch *sp = rule_branch(f);
        const int e = sp->fx;
        if (e == PSX_CARD_FX_HEAL && (v < 0 || v > 25500)) { say("Heal is 0 to 25500, in steps of 100"); return; }
        if (e == PSX_CARD_FX_DAMAGE && (v < 0 || v > 2550)) { say("Damage is 0 to 2550, in steps of 10"); return; }
        if (e == PSX_CARD_FX_DESTROY_ATK && (v < 210 || v > 2550)) { say("The ATK threshold is 210 to 2550"); return; }
        if (e == PSX_CARD_FX_WEAKEN && (v < -9990 || v > 9990)) { say("Weaken is -9990 to 9990 (negative strengthens)"); return; }
        if (e == PSX_CARD_FX_LOSE_LP && (v < 0 || v > 9999)) { say("LP lost is 0 to 9999"); return; }
        sp->amount = (e == PSX_CARD_FX_HEAL) ? v / 100 * 100 : (e == PSX_CARD_FX_LOSE_LP) ? v : v / 10 * 10;
        break;
    }
    }
    s_changed = 1;
}

static void do_save(void)
{
    if (s_focus >= 0) focus_commit();
    if (!is_monster()) {
        /* nothing the game would draw; do not carry stale monster numbers */
        s_edit.attack = s_edit.defense = s_edit.star1 = s_edit.star2 = s_edit.level = s_edit.attribute = -1;
    }
    /* effect fields the card cannot use are dropped rather than saved blind */
    if (!field_fits(F_EFFECT)) { s_edit.effect = s_edit.amount = s_edit.target = s_edit.terrain = -1; s_edit.ritual_set = 0; }
    if (!field_fits(F_EQUIPS)) { s_edit.equip_bonus = -1; s_edit.equips_set = 0; s_edit.equip_types = 0; s_edit.equip_n = 0; }
    if (!field_fits(F_BOOST))  s_edit.boost_set = 0;
    if (!field_fits(F_TRAP_MAX)) s_edit.trap_atk_max = -1;
    if (!is_monster()) {
        s_edit.battle = -1; s_edit.immune = -1;
        for (int t = 0; t < 6; t++) memset(trig_at(t), 0, sizeof(PsxCardTrigger));
        s_edit.bonus_n = 0;
    } else for (int t = 0; t < 6; t++) trigger_prune(trig_at(t));
    rules_build();
    int any = 0;
    for (int f = 0; f < F_COUNT; f++) any |= field_is_set(f);
    if (!any && !s_edit.has_art && !s_edit.has_thumb && !s_edit.has_title) {
        if (s_has_pack) { psx_card_packs_remove(s_sel); say("Nothing left to keep; the card is stock again"); }
        else say("Nothing to save");
        load_editor();
        return;
    }
    if (psx_card_packs_save(&s_edit)) say("Saved. It shows on the next screen that draws the card.");
    else say("Save failed: could not write the card folder");
    load_editor();
    rebuild_order();
}

static void do_restore(void)
{
    if (!s_has_pack) { say("This card is already stock"); return; }
    psx_card_packs_remove(s_sel);
    say("Card restored to stock");
    load_editor();
    rebuild_order();
}

static void card_folder(char *out, size_t cap, int create)
{
    const char *dir = psx_card_packs_dir();
    if (create) MKDIR(dir);
    snprintf(out, cap, "%s/%d", dir, s_sel);
    if (create) MKDIR(out);
}

static void do_open_folder(void)
{
    char path[1200];
    if (!psx_card_packs_dir()[0]) { say("The game has not booted yet"); return; }
    card_folder(path, sizeof path, 1);
#ifdef _WIN32
    ShellExecuteA(NULL, "open", path, NULL, NULL, SW_SHOWNORMAL);
#else
    const pid_t pid = fork();
    if (pid == 0) { execlp("xdg-open", "xdg-open", path, (char *)NULL); _exit(127); }
#endif
    say("Opened the card's folder");
}

static int install_pick(const char *src, int kind)
{
    static const char *const names[4] = { "", "art.png", "thumb.png", "title.png" };
    char dst[1200];
    card_folder(dst, sizeof dst, 1);
    const size_t n = strlen(dst);
    snprintf(dst + n, sizeof dst - n, "/%s", names[kind]);
    FILE *in = psx_fopen_utf8(src, "rb");
    if (!in) return 0;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return 0; }
    char buf[65536];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, got, out);
    fclose(in); fclose(out);
    return 1;
}

#if defined(PSX_SDL3)
static void SDLCALL pick_cb(void *userdata, const char *const *filelist, int filter)
{
    (void)filter;
    if (!filelist || !filelist[0]) return;
    snprintf(s_pick_path, sizeof s_pick_path, "%s", filelist[0]);
    s_pick_kind = (int)(intptr_t)userdata;
}
#endif

static void do_pick(int kind)
{
#if defined(PSX_SDL3)
    static const SDL_DialogFileFilter filters[] = { { "PNG images", "png" } };
    SDL_ShowOpenFileDialog(pick_cb, (void *)(intptr_t)kind, s_win, filters, 1, NULL, false);
#else
    (void)kind;
    say("No file dialog in this build; drop a PNG in the card's folder");
#endif
}

/* Append the effect's wording to the description, or tell why not. */
static void do_effect_text(void)
{
    if (s_focus >= 0) focus_commit();
    char fx[FTEXT];
    if (!psx_card_effects_describe(&s_edit, fx, sizeof fx)) { say("This card has no edited effect to describe"); return; }
    char cur[FTEXT]; field_text(F_DESC, 0, cur, sizeof cur);
    if (strstr(cur, fx)) { say("The description already has the effect text"); return; }
    char out[FTEXT];
    if (cur[0]) snprintf(out, sizeof out, "%s|%s", cur, fx); else snprintf(out, sizeof out, "%s", fx);
    int replaced = 0;
    if (strlen(out) > PSX_CARD_PACK_DESC_MAX) { snprintf(out, sizeof out, "%s", fx); replaced = 1; }
    if (strlen(out) > PSX_CARD_PACK_DESC_MAX) { say("The effect text alone is longer than a description can be"); return; }
    snprintf(s_edit.description, sizeof s_edit.description, "%s", out);
    s_changed = 1; s_dirty = 1;
    int lines = 0, longest = 0, wide = 0;
    const int fits = psx_card_packs_desc_layout(out, &lines, &longest, &wide);
    if (replaced) say("The old text and the effect did not both fit: the description is now the effect text");
    else if (!fits) say("Effect text added; the card shows 6 lines, so trim the text above it");
    else say("Effect text added to the description");
}

static void do_export(void)
{
    int n = 0;
    for (int id = 1; id <= CARDS; id++) n += psx_card_packs_get(id, NULL) != 0;
    if (!n) { say("No edited cards to export yet"); return; }
#if defined(PSX_SDL3)
    static const SDL_DialogFileFilter filters[] = { { "Edited cards", PSX_CARD_SHARE_EXT } };
    static char def[1200];
    snprintf(def, sizeof def, "%s/edited-cards.%s", psx_mod_player_data_dir(), PSX_CARD_SHARE_EXT);
    SDL_ShowSaveFileDialog(pick_cb, (void *)(intptr_t)4, s_win, filters, 1, def);
#else
    say("No file dialog in this build");
#endif
}

/* every card's name and description as one text file, and back */
static void do_export_texts(void)
{
#if defined(PSX_SDL3)
    static const SDL_DialogFileFilter filters[] = { { "Text files", "txt" } };
    static char def[1200];
    snprintf(def, sizeof def, "%s/card-descriptions.txt", psx_mod_player_data_dir());
    SDL_ShowSaveFileDialog(pick_cb, (void *)(intptr_t)6, s_win, filters, 1, def);
#else
    say("No file dialog in this build");
#endif
}
static void do_import_texts(void)
{
#if defined(PSX_SDL3)
    static const SDL_DialogFileFilter filters[] = { { "Text files", "txt" } };
    SDL_ShowOpenFileDialog(pick_cb, (void *)(intptr_t)7, s_win, filters, 1, psx_mod_player_data_dir(), false);
#else
    say("No file dialog in this build");
#endif
}

static void do_import(void)
{
#if defined(PSX_SDL3)
    static const SDL_DialogFileFilter filters[] = { { "Edited cards", PSX_CARD_SHARE_EXT }, { "Zip archives", "zip" } };
    SDL_ShowOpenFileDialog(pick_cb, (void *)(intptr_t)5, s_win, filters, 2, psx_mod_player_data_dir(), false);
#else
    say("No file dialog in this build");
#endif
}

/* The chosen file, once the dialog answers: export writes it, import shows
 * the preview and waits for the player. */
static void begin_export(const char *path)
{
    char p[1200]; snprintf(p, sizeof p, "%s", path);
    const char *dot = strrchr(p, '.'), *slash = strrchr(p, '/');
    if (!dot || (slash && dot < slash)) { const size_t n = strlen(p); snprintf(p + n, sizeof p - n, ".%s", PSX_CARD_SHARE_EXT); }
    char msg[160];
    psx_card_share_export(p, msg, sizeof msg);
    say(msg);
}

static void begin_import(const char *path)
{
    snprintf(s_share_path, sizeof s_share_path, "%s", path);
    if (!psx_card_share_inspect(path, &s_share)) { say(s_share.error); return; }
    if (!s_share.card_n && !s_share.has_drops) { say("That file holds no edited cards"); return; }
    s_modal = 1; s_modal_hover = -1; s_dirty = 1;
}

static void finish_activate(int go)
{
    s_modal = 0; s_dirty = 1;
    if (go) { psx_card_packs_set_dev(1); say("Card Effects on: the mod's card set is live"); }
    else    { psx_card_packs_set_dev(0); say("Card Effects stays off"); }   /* also puts the MODS row back */
}

static void finish_import(int go)
{
    if (s_modal == MODAL_ACTIVATE) { finish_activate(go); return; }
    s_modal = 0; s_dirty = 1;
    if (!go) { say("Import cancelled; nothing changed"); return; }
    char msg[200];
    psx_card_share_import(s_share_path, msg, sizeof msg);
    say(msg);
    load_editor();
    rebuild_order();
}

/* --- draw ------------------------------------------------------------------- */
static void draw_caret(const Rect *r, int dir, uint32_t col)
{
    const float cx = r->x + r->w * 0.5f, cy = r->y + r->h * 0.5f, s = (float)px(3.0f);
    const float w = (float)px(1.4f);
    if (dir < 0) {
        psx_ui_line(&s_cv, cx + s * 0.5f, cy - s, cx - s * 0.5f, cy, w, col);
        psx_ui_line(&s_cv, cx - s * 0.5f, cy, cx + s * 0.5f, cy + s, w, col);
    } else {
        psx_ui_line(&s_cv, cx - s * 0.5f, cy - s, cx + s * 0.5f, cy, w, col);
        psx_ui_line(&s_cv, cx + s * 0.5f, cy, cx - s * 0.5f, cy + s, w, col);
    }
}

static void draw_cross(const Rect *r, uint32_t col)
{
    const float cx = r->x + r->w * 0.5f, cy = r->y + r->h * 0.5f, s = (float)px(2.6f), w = (float)px(1.4f);
    psx_ui_line(&s_cv, cx - s, cy - s, cx + s, cy + s, w, col);
    psx_ui_line(&s_cv, cx - s, cy + s, cx + s, cy - s, w, col);
}

static void draw_button_face(const Rect *r, const char *label, int primary, int hover, const PsxUiFace *face)
{
    psx_ui_round_rect(&s_cv, r->x, r->y, r->w, r->h, r->h * 0.5f, primary ? COL_BTN_ON : COL_BTN);
    if (hover) psx_ui_round_rect(&s_cv, r->x, r->y, r->w, r->h, r->h * 0.5f, COL_HOVER);
    text_centered(r, label, COL_TEXT, face);
}
static void draw_button(const Rect *r, const char *label, int primary, int hover) { draw_button_face(r, label, primary, hover, face_bold()); }

static void draw_bar(void)
{
    const Layout *L = &s_L;
    psx_ui_fill(&s_cv, 0, 0, s_w, L->bar.h, COL_BAR);
    psx_ui_fill(&s_cv, 0, L->bar.h - 1, s_w, 1, 0x40FFFFFFu);
    Rect t = { px(8.0f), 0, px(110.0f), L->bar.h };
    text_in(&t, 0, "Card Manager", COL_ACCENT, face_title());
    psx_ui_round_rect(&s_cv, L->search.x, L->search.y, L->search.w, L->search.h, L->search.h * 0.5f, COL_EDIT_BG);
    char sb[48];
    snprintf(sb, sizeof sb, "%s%s", s_search, (s_focus < 0 && s_caret_on) ? "|" : "");
    if (s_search[0]) text_in(&L->search, px(8.0f), sb, COL_TEXT, face_body());
    else text_in(&L->search, px(8.0f), "Type to search\xE2\x80\xA6", COL_DIM, face_body());
    if (L->bar_free > px(70.0f)) {
        char n[48]; snprintf(n, sizeof n, "%d cards", s_order_n);
        Rect c = { L->search.x + L->search.w + px(10.0f), 0, L->bar_free - px(14.0f), L->bar.h };
        text_in(&c, 0, n, COL_DIM, face_small());
    }
}

static void draw_list(void)
{
    const Layout *L = &s_L;
    psx_ui_round_rect(&s_cv, L->list.x, L->list.y, L->list.w, L->list.h, (float)px(U_R_PANEL), COL_PANEL);
    const PsxUiFace *fs = face_small(), *fb = face_body();
    Rect hdr = { L->list.x + px(8.0f), L->list.y, L->list.w, px(U_HDR_H) };
    psx_ui_text(&s_cv, hdr.x + px(4.0f), psx_ui_baseline_in(hdr.y, hdr.h, fs), "ID", COL_DIM, fs);
    psx_ui_text(&s_cv, hdr.x + px(30.0f), psx_ui_baseline_in(hdr.y, hdr.h, fs), "Name", COL_DIM, fs);
    psx_ui_text(&s_cv, L->list.x + L->list.w - px(44.0f), psx_ui_baseline_in(hdr.y, hdr.h, fs), "Edited", COL_DIM, fs);
    int y = L->list_rows.y;
    for (int i = 0; i < L->rows; i++, y += L->row_h) {
        const int k = s_scroll + i;
        if (k >= s_order_n) break;
        const int id = s_order[k];
        const Rect row = { L->list.x + px(4.0f), y, L->list_rows.w - px(4.0f), L->row_h };
        if (id == s_sel) psx_ui_round_rect(&s_cv, row.x, row.y, row.w, row.h, row.h * 0.5f, COL_SEL_BG);
        else if (i == s_hover_row) psx_ui_round_rect(&s_cv, row.x, row.y, row.w, row.h, row.h * 0.5f, COL_HOVER);
        char b[8]; snprintf(b, sizeof b, "%d", id);
        const int base = psx_ui_baseline_in(y, L->row_h, fb);
        const int idw = psx_ui_font_text_w(fb, b);
        psx_ui_text(&s_cv, hdr.x + px(22.0f) - idw, base, b, COL_DIM, fb);
        psx_ui_text_clip(&s_cv, hdr.x + px(30.0f), base, psx_card_db_name(id), id == s_sel ? COL_ACCENT : COL_TEXT, fb,
                         L->list.w - px(30.0f) - px(54.0f));
        if (psx_card_packs_get(id, NULL)) {
            const int cx = L->list.x + L->list.w - px(30.0f);
            psx_ui_round_rect(&s_cv, cx - px(3.0f), y + L->row_h / 2 - px(3.0f), px(6.0f), px(6.0f), (float)px(3.0f), COL_EDITED);
        }
    }
    if (s_order_n > L->rows) {
        psx_ui_round_rect(&s_cv, L->sb.x, L->sb.y, L->sb.w, L->sb.h, L->sb.w * 0.5f, COL_TRACK);
        int th = L->sb.h * L->rows / s_order_n; if (th < px(12.0f)) th = px(12.0f);
        const int ty = L->sb.y + (L->sb.h - th) * s_scroll / (s_order_n - L->rows);
        psx_ui_round_rect(&s_cv, L->sb.x, ty, L->sb.w, th, L->sb.w * 0.5f, s_sb_drag ? COL_ACCENT : COL_THUMB);
    }
}

static void draw_editor(void)
{
    const Layout *L = &s_L;
    const PsxUiFace *ft = face_title(), *fb = face_body(), *fs = face_small();
    psx_ui_round_rect(&s_cv, L->ed.x, L->ed.y, L->ed.w, L->ed.h, (float)px(U_R_PANEL), COL_PANEL);
    const int ex = L->ed.x + px(U_PAD), right = L->ed.x + L->ed.w - px(U_PAD);
    /* header */
    {
        char h[96]; snprintf(h, sizeof h, "%sCard %03d", psx_card_packs_is_dev() ? "[Card Effects] " : "", s_sel);
        int x = psx_ui_text(&s_cv, ex, L->ed.y + px(U_PAD) + psx_ui_font_ascent(ft), h, COL_ACCENT, ft);
        x += px(8.0f);
        psx_ui_text_clip(&s_cv, x, L->ed.y + px(U_PAD) + psx_ui_font_ascent(ft), psx_card_db_name(s_sel), COL_TEXT, ft, right - x - px(60.0f));
        if (s_has_pack) {
            const int cw = psx_ui_font_text_w(fs, "edited") + px(12.0f), ch = px(13.0f);
            Rect chip = { right - cw, L->ed.y + px(U_PAD) + px(2.0f), cw, ch };
            psx_ui_round_rect(&s_cv, chip.x, chip.y, chip.w, chip.h, ch * 0.5f, COL_SEL_BG);
            text_centered(&chip, "edited", COL_EDITED, fs);
        }
    }
    /* tabs */
    for (int i = 0; i < 2; i++) {
        const Rect *t = &L->tab[i];
        psx_ui_round_rect(&s_cv, t->x, t->y, t->w, t->h, t->h * 0.5f, i == s_tab ? COL_BTN_ON : COL_BTN);
        text_centered(t, TAB_LABEL[i], i == s_tab ? COL_TEXT : COL_DIM, fb);
    }
    /* previews */
    if (s_tab == 0) {
    psx_ui_round_rect(&s_cv, L->art.x - 2, L->art.y - 2, L->art.w + 4, L->art.h + 4, (float)px(U_R_BOX), COL_EDIT_BG);
    if (s_art_ok) psx_ui_blit_scaled(&s_cv, L->art.x, L->art.y, L->art.w, L->art.h, (float)px(4.0f), s_art_argb, 102, 96);
    psx_ui_round_rect(&s_cv, L->thumb.x - 2, L->thumb.y - 2, L->thumb.w + 4, L->thumb.h + 4, (float)px(4.0f), COL_EDIT_BG);
    if (s_thumb_ok) psx_ui_blit_scaled(&s_cv, L->thumb.x, L->thumb.y, L->thumb.w, L->thumb.h, (float)px(3.0f), s_thumb_argb, 40, 32);
    psx_ui_text(&s_cv, L->thumb.x, L->thumb.y + L->thumb.h + px(4.0f) + psx_ui_font_ascent(fs), "duel", COL_DIM, fs);
    {
        const int lh = psx_ui_font_line_height(fs);
        int y = L->info_y;
        const int iw = right - L->info_x;
        psx_ui_text(&s_cv, L->info_x, y + psx_ui_font_ascent(fs), s_edit.has_art ? "Face art: yours" : "Face art: stock", s_edit.has_art ? COL_EDITED : COL_DIM, fs); y += lh;
        psx_ui_text(&s_cv, L->info_x, y + psx_ui_font_ascent(fs), s_edit.has_thumb ? "Duel thumbnail: yours" : "Duel thumbnail: stock", s_edit.has_thumb ? COL_EDITED : COL_DIM, fs); y += lh;
        psx_ui_text(&s_cv, L->info_x, y + psx_ui_font_ascent(fs), s_edit.has_title ? "Title strip: yours" : "Title strip: from the name", s_edit.has_title ? COL_EDITED : COL_DIM, fs); y += lh + px(4.0f);
        draw_wrapped(L->info_x, y, iw, "Any PNG works for the face; it becomes 102x96 in 256 colours. The duel thumbnail is 40x32 in 64 colours and is made from the face unless you pick one. A change shows on the next screen that draws the card.", COL_DIM, fs, 5);
    }
    }
    /* fields */
    for (int f = 0; f < F_COUNT; f++) {
        const Rect *v = &L->value[f];
        const int set = field_is_set(f);
        if (!field_applies(f)) {
            /* a spell, trap, ritual or equip card: the game never draws these */
            if (f == F_ATK && !is_monster() && s_tab == 0)
                psx_ui_text(&s_cv, L->label[f].x, psx_ui_baseline_in(L->label[f].y, L->label[f].h, fs), "ATK, DEF, stars, level and attribute are not used by a Magic, Trap, Ritual or Equip card", 0x99C9CFDDu, fs);
            continue;
        }

        if (field_label(f)[0]) psx_ui_text(&s_cv, L->label[f].x, psx_ui_baseline_in(L->label[f].y, L->label[f].h, fb), field_label(f), COL_DIM, fb);
        psx_ui_round_rect(&s_cv, v->x, v->y, v->w, v->h, (float)px(U_R_BOX), s_focus == f ? COL_EDIT_BG : COL_BTN);
        if (s_focus == f) psx_ui_round_rect_line(&s_cv, v->x, v->y, v->w, v->h, (float)px(U_R_BOX), COL_ACCENT, 1.0f);
        char t[PSX_CARD_PACK_DESC_MAX + 8];
        if (s_focus == f) {
            draw_focused_text(f);
        } else {
            field_text(f, 0, t, sizeof t);
            if (f == F_DESC) {
                /* "|" is a line break in the game; show it as one */
                char shown[PSX_CARD_PACK_DESC_MAX + 8]; snprintf(shown, sizeof shown, "%s", t);
                const int lh = psx_ui_font_line_height(fb);
                const int fitl = (v->h - px(4.0f)) / lh;
                char *p = shown; int line = 0;
                while (p && *p && line < fitl) {
                    char *bar = strchr(p, '|');
                    if (bar) *bar = 0;
                    char lines[4][256];
                    const int n = wrap_text(fb, p, v->w - px(12.0f), lines, 4);
                    for (int i = 0; i < n && line < fitl; i++, line++)
                        psx_ui_text(&s_cv, v->x + px(6.0f), v->y + px(2.0f) + line * lh + psx_ui_font_ascent(fb), lines[i], set ? COL_EDITED : COL_TEXT, fb);
                    if (!n) line++;
                    p = bar ? bar + 1 : NULL;
                }
            } else {
                if (is_trig(f) && rule_branch(f) && rule_branch(f)->fx < 0) text_in(v, px(6.0f), t, COL_ACCENT, fb);
                else if (is_rulef(f)) text_in(v, px(6.0f), t, COL_TEXT, fb);
                else text_in(v, px(6.0f), t, set ? COL_EDITED : COL_TEXT, fb);
            }
        }
        if (f == F_DESC) {
            /* how the game will lay it out */
            char cur[FTEXT]; field_text(F_DESC, 0, cur, sizeof cur);
            const char *txt = (s_focus == F_DESC) ? s_buf : cur;
            int lines = 0, longest = 0, wide = 0;
            const int ok = psx_card_packs_desc_layout(txt, &lines, &longest, &wide);
            char w[160];
            if (wide) snprintf(w, sizeof w, "Line %d is %d characters; the card shows 20 per line", wide, longest);
            else if (lines > PSX_CARD_PACK_DESC_LINES) snprintf(w, sizeof w, "%d lines; the card shows %d", lines, PSX_CARD_PACK_DESC_LINES);
            else snprintf(w, sizeof w, "%d of %d lines, longest %d of 20 characters", lines, PSX_CARD_PACK_DESC_LINES, longest);
            psx_ui_text_clip(&s_cv, v->x, v->y + v->h + px(2.0f) + psx_ui_font_ascent(fs), w, ok ? COL_DIM : COL_WARN, fs, right - v->x);
        }
        if (field_is_enum(f) && !no_steppers(f)) {
            const Rect *l = &L->step_l[f], *r = &L->step_r[f];
            psx_ui_round_rect(&s_cv, l->x, l->y, l->w, l->h, l->h * 0.5f, COL_BTN); draw_caret(l, -1, COL_TEXT);
            psx_ui_round_rect(&s_cv, r->x, r->y, r->w, r->h, r->h * 0.5f, COL_BTN); draw_caret(r, +1, COL_TEXT);
        }
        if (field_is_enum(f)) {
            /* the little "v" that says: click for the list */
            const int cx = v->x + v->w - px(7.0f), cy = v->y + v->h / 2;
            psx_ui_line(&s_cv, cx - px(3.0f), cy - px(1.5f), cx, cy + px(1.5f), 1.2f, COL_DIM);
            psx_ui_line(&s_cv, cx, cy + px(1.5f), cx + px(3.0f), cy - px(1.5f), 1.2f, COL_DIM);
        }
        if (f == F_COLOR) {
            /* three tones of the palette the card will draw with */
            unsigned char sw[9];
            const int slot = set ? s_edit.color : psx_card_colors_slot(s_sel);
            if (psx_card_colors_swatch(slot, sw)) {
                const int sx = v->x + v->w - px(3.0f) - px(10.0f) * 3, sy = v->y + px(3.0f), sh = v->h - px(6.0f);
                for (int i = 0; i < 3; i++)
                    psx_ui_round_rect(&s_cv, sx + i * px(10.0f), sy, px(9.0f), sh, (float)px(2.0f), 0xFF000000u | ((uint32_t)sw[i*3] << 16) | ((uint32_t)sw[i*3+1] << 8) | sw[i*3+2]);
            }
        }
        if (set) {
            const Rect *c = &L->clear[f];
            psx_ui_round_rect(&s_cv, c->x, c->y, c->w, c->h, c->h * 0.5f, COL_BTN); draw_cross(c, COL_TEXT);
            if (f != F_DESC && !is_rulef(f)) {
                char st[PSX_CARD_PACK_DESC_MAX + 8]; field_text(f, 1, st, sizeof st);
                char s2[PSX_CARD_PACK_DESC_MAX + 16]; snprintf(s2, sizeof s2, "stock: %s", st);
                const int sx = c->x + c->w + px(8.0f);
                const int lim = right;
                if (lim - sx > px(30.0f)) psx_ui_text_clip(&s_cv, sx, psx_ui_baseline_in(v->y, v->h, fs), s2, COL_DIM, fs, lim - sx);
            }
        }
    }
    if (L->fx_note_y) {
        const char *note = (s_tab == 1 && is_monster())
            ? (s_rule_n ? "One line per effect. \"Otherwise\" runs when the line above it fails its roll; face-up rules hold while the monster shows." : "This monster has no effects yet. Add one: a trigger, its odds and what happens, or a rule that holds while it is face-up.")
            : psx_card_effects_note(s_sel, effective_type());
        psx_ui_text_clip(&s_cv, L->fx_note_x, L->fx_note_y + psx_ui_font_ascent(fs), note, COL_ACCENT, fs, right - L->fx_note_x);
    }
    if (L->preview_y) {
        /* the card text the rules make */
        char fx[FTEXT];
        if (psx_card_effects_describe(&s_edit, fx, sizeof fx)) {
            for (char *q = fx; *q; q++) if (*q == '|') *q = ' ';
            int lines = 0, longest = 0, wide = 0;
            const int fits = psx_card_packs_desc_layout(fx, &lines, &longest, &wide);
            char head[96]; snprintf(head, sizeof head, fits ? "Card text (%d of 6 lines):" : "Card text: %d lines, the card shows 6 (trim the description or the rules):", lines);
            psx_ui_text(&s_cv, ex, L->preview_y + psx_ui_font_ascent(fs), head, fits ? COL_DIM : COL_WARN, fs);
            draw_wrapped(ex, L->preview_y + psx_ui_font_line_height(fs), right - ex, fx, COL_TEXT, fs, 3);
        } else psx_ui_text(&s_cv, ex, L->preview_y + psx_ui_font_ascent(fs), "Card text: none yet.", COL_DIM, fs);
    }
    for (int b = 0; b < B_COUNT; b++)
        if (L->btn[b].w) draw_button_face(&L->btn[b], b == B_DEV ? s_dev_label : BTN_LABEL[b], (b == B_SAVE && s_changed) || (b == B_DEV && psx_card_packs_is_dev()), s_hover_btn == b,
                         b >= B_BAR_FIRST ? face_small() : face_bold());
    /* status + help, wrapped to the panel */
    {
        int y = L->status_y;
        const int w = right - ex;
        if (s_msg[0]) y = draw_wrapped(ex, y, w, s_msg, COL_WARN, fb, 2);
        else if (s_changed) y = draw_wrapped(ex, y, w, "Unsaved changes. Save writes card.ini in the card's folder and applies it right away.", COL_DIM, fb, 2);
        else {
            char p[1200]; snprintf(p, sizeof p, "%s/%d/", psx_card_packs_dir(), s_sel);
            y = draw_wrapped(ex, y, w, p, COL_DIM, fs, 2);
        }
        y += px(3.0f);
        draw_wrapped(ex, y, w, s_tab == 0
            ? "Green is your edit; x puts a value back to stock. Click a value to type, Enter keeps it, Esc cancels; select with the mouse or Shift+arrows, Ctrl+C/V copies and pastes. In the description a | starts a new line (20 columns, six lines). Export Config writes every edited card to one .ygocards file; Import Config reads one and shows what it will replace first."
            : "Each rule is a sentence: when it happens, the odds, what it does. Lists open on a click; type to filter a long one. \"Effect text \xE2\x86\x92 description\" writes the card text onto the card.", COL_DIM, fs, 5);
    }
}

/* Where the open dropdown's list sits: under its value box, or above when
 * the window has no room below. */
/* The options the open list shows: every one, or those matching what was
 * typed. */
static int s_drop_items[CARDS];
static int drop_items(void)
{
    const int n = enum_count(s_drop);
    int m = 0;
    for (int i = 0; i < n && m < CARDS; i++)
        if (!s_drop_filter[0] || ci_contains(enum_label(s_drop, i), s_drop_filter)) s_drop_items[m++] = i;
    return m;
}
static int drop_has_header(void) { return enum_count(s_drop) > DROP_ROWS || s_drop_filter[0]; }
static void drop_open(int f)
{
    s_drop = f; s_drop_cards = 0; s_drop_filter[0] = 0; s_drop_hover = -1;
    const int c = enum_current_index(f);
    s_drop_scroll = c > DROP_ROWS / 2 ? c - DROP_ROWS / 2 : 0;
    const int mx = enum_count(f) - DROP_ROWS;
    if (s_drop_scroll > mx) s_drop_scroll = mx < 0 ? 0 : mx;
    s_dirty = 1;
}
static void drop_open_cards(int f) { drop_open(f); s_drop_cards = 1; const int c = enum_current_index(f); s_drop_scroll = c > DROP_ROWS / 2 ? c - DROP_ROWS / 2 : 0; }
static void drop_close(void) { s_drop = -1; s_drop_cards = 0; s_drop_filter[0] = 0; s_dirty = 1; }
static Rect drop_rect(void)
{
    const Layout *L = &s_L;
    const Rect *v = &L->value[s_drop];
    const PsxUiFace *fb = face_body();
    const int rh = px(U_ROW_H) + px(2.0f);
    const int n = drop_items(), shown = n < DROP_ROWS ? n : DROP_ROWS;
    /* wide enough for the longest option, so nothing is cut short */
    int w = v->w;
    for (int k = 0; k < n; k++) { const int lw = psx_ui_font_text_w(fb, enum_label(s_drop, s_drop_items[k])) + px(28.0f); if (lw > w) w = lw; }
    if (w < px(150.0f)) w = px(150.0f);
    if (w > s_w - px(8.0f)) w = s_w - px(8.0f);
    const int hdr = drop_has_header() ? rh : 0;
    Rect r = { v->x, v->y + v->h + px(2.0f), w, hdr + shown * rh + px(6.0f) };
    if (r.y + r.h > s_h - px(4.0f)) r.y = v->y - px(2.0f) - r.h;
    if (r.x + r.w > s_w - px(4.0f)) r.x = s_w - px(4.0f) - r.w;
    return r;
}
/* the list row under a point, as an index into the shown items, or -1 */
static int drop_row_at(int x, int y)
{
    const Rect r = drop_rect();
    if (!in_rect(&r, x, y)) return -1;
    const int rh = px(U_ROW_H) + px(2.0f);
    const int hdr = drop_has_header() ? rh : 0;
    const int k = (y - r.y - px(3.0f) - hdr) / rh;
    if (y - r.y - px(3.0f) < hdr) return -1;
    const int i = s_drop_scroll + k;
    return (i >= 0 && i < drop_items()) ? i : -1;
}

static void draw_dropdown(void)
{
    if (s_drop < 0) return;
    const Rect r = drop_rect();
    const int rh = px(U_ROW_H) + px(2.0f), n = drop_items();
    const int shown = n < DROP_ROWS ? n : DROP_ROWS;
    const int cur = enum_current_index(s_drop);
    psx_ui_round_rect_shadow(&s_cv, r.x, r.y, r.w, r.h, (float)px(U_R_BOX), COL_PANEL, px(5.0f));
    psx_ui_round_rect(&s_cv, r.x, r.y, r.w, r.h, (float)px(U_R_BOX), COL_EDIT_BG);
    psx_ui_round_rect_line(&s_cv, r.x, r.y, r.w, r.h, (float)px(U_R_BOX), COL_ACCENT, 1.0f);
    const PsxUiFace *fb = face_body(), *fs = face_small();
    int y0 = r.y + px(3.0f);
    if (drop_has_header()) {
        const Rect row = { r.x + px(3.0f), y0, r.w - px(6.0f), rh };
        char h[64];
        if (s_drop_filter[0]) snprintf(h, sizeof h, "%s%s  (%d)", s_drop_filter, s_caret_on ? "|" : " ", n);
        else snprintf(h, sizeof h, "type to filter\xE2\x80\xA6  (%d)", n);
        text_in(&row, px(8.0f), h, s_drop_filter[0] ? COL_TEXT : COL_DIM, fs);
        y0 += rh;
    }
    for (int k = 0; k < shown; k++) {
        const int i = s_drop_scroll + k;
        if (i >= n) break;
        const int item = s_drop_items[i];
        const Rect row = { r.x + px(3.0f), y0 + k * rh, r.w - px(6.0f), rh };
        if (item == cur) psx_ui_round_rect(&s_cv, row.x, row.y, row.w, row.h, rh * 0.5f, COL_SEL_BG);
        else if (i == s_drop_hover) psx_ui_round_rect(&s_cv, row.x, row.y, row.w, row.h, rh * 0.5f, COL_HOVER);
        text_in(&row, px(8.0f), enum_label(s_drop, item), item == cur ? COL_ACCENT : COL_TEXT, fb);
    }
    if (!n) { const Rect row = { r.x + px(3.0f), y0, r.w - px(6.0f), rh }; text_in(&row, px(8.0f), "nothing matches", COL_DIM, fb); }
    if (n > shown) {
        const int th = r.h * shown / n, ty = r.y + (r.h - th) * s_drop_scroll / (n - shown);
        psx_ui_round_rect(&s_cv, r.x + r.w - px(5.0f), ty, px(3.0f), th, px(1.5f), COL_THUMB);
    }
}

static void draw_modal(void)
{
    const Layout *L = &s_L;
    const PsxUiFace *ft = face_title(), *fb = face_body(), *fs = face_small();
    psx_ui_fill(&s_cv, 0, 0, s_w, s_h, 0xB8000000u);
    psx_ui_round_rect_shadow(&s_cv, L->modal.x, L->modal.y, L->modal.w, L->modal.h, (float)px(U_R_PANEL), COL_PANEL, px(6.0f));
    psx_ui_round_rect(&s_cv, L->modal.x, L->modal.y, L->modal.w, L->modal.h, (float)px(U_R_PANEL), COL_PANEL);
    const int ex = L->modal.x + px(U_PAD), w = L->modal.w - px(U_PAD) * 2;
    int y = L->modal.y + px(U_PAD);
    if (s_modal == MODAL_ACTIVATE) {
        psx_ui_text(&s_cv, ex, y + psx_ui_font_ascent(ft), "Card Effects", COL_ACCENT, ft); y += psx_ui_font_line_height(ft) + px(6.0f);
        y = draw_wrapped(ex, y, w, "The Card Effects mod brings the original card effects and adapts them to Forbidden Memories. Applying this will replace any settings you currently have in the Card Manager. Would you like to activate the mod?", COL_TEXT, fb, 6);
        draw_button(&L->modal_ok, "Yes", 1, s_modal_hover == 0);
        draw_button(&L->modal_cancel, "No", 0, s_modal_hover == 1);
        return;
    }
    psx_ui_text(&s_cv, ex, y + psx_ui_font_ascent(ft), "Import edited cards", COL_ACCENT, ft); y += psx_ui_font_line_height(ft) + px(4.0f);
    const char *base = strrchr(s_share_path, '/'); base = base ? base + 1 : s_share_path;
    psx_ui_text_clip(&s_cv, ex, y + psx_ui_font_ascent(fs), base, COL_DIM, fs, w); y += psx_ui_font_line_height(fs) + px(6.0f);
    char line[512];
    snprintf(line, sizeof line, "%d edited card%s in this file%s.", s_share.card_n, s_share.card_n == 1 ? "" : "s",
             s_share.has_drops ? ", plus drop table edits" : "");
    y = draw_wrapped(ex, y, w, line, COL_TEXT, fb, 2);
    if (s_share.replace_n) {
        unsigned n = (unsigned)snprintf(line, sizeof line, "%d replace%s a card you already edited: ", s_share.replace_n, s_share.replace_n == 1 ? "s" : "");
        for (int i = 0; i < s_share.replace_n && n + 48 < sizeof line; i++) {
            if (i == 12) { n += (unsigned)snprintf(line + n, sizeof line - n, " and %d more", s_share.replace_n - i); break; }
            n += (unsigned)snprintf(line + n, sizeof line - n, "%s%d %s", i ? ", " : "", s_share.replace_ids[i], psx_card_db_name(s_share.replace_ids[i]));
        }
        y = draw_wrapped(ex, y, w, line, COL_WARN, fb, 4);
    } else {
        y = draw_wrapped(ex, y, w, "None of them replaces a card you edited.", COL_DIM, fb, 1);
    }
    if (s_share.has_drops) y = draw_wrapped(ex, y, w, s_share.drops_here ? "The drop table edits in the file replace yours." : "The file's drop table edits are installed too.", COL_WARN, fb, 2);
    (void)y;
    draw_button(&L->modal_ok, "Import", 1, s_modal_hover == 0);
    draw_button(&L->modal_cancel, "Cancel", 0, s_modal_hover == 1);
}

static void draw(void)
{
    s_cv.px = s_px; s_cv.w = s_w; s_cv.h = s_h;
    layout_compute();
    psx_ui_fill(&s_cv, 0, 0, s_w, s_h, COL_BG);
    draw_bar();
    draw_list();
    draw_editor();
    draw_dropdown();
    if (s_modal) draw_modal();
}

/* --- input -------------------------------------------------------------------- */
static void set_scroll_from_thumb(int y)
{
    const Layout *L = &s_L;
    if (s_order_n <= L->rows) return;
    int th = L->sb.h * L->rows / s_order_n; if (th < px(12.0f)) th = px(12.0f);
    const int range = L->sb.h - th;
    if (range <= 0) return;
    int t = y - s_sb_grab - L->sb.y;
    if (t < 0) t = 0;
    if (t > range) t = range;
    s_scroll = (int)((long)t * (s_order_n - L->rows) / range);
    clamp_scroll();
    s_dirty = 1;
}

static void click(int x, int y, int button, int clicks)
{
    layout_compute();
    const Layout *L = &s_L;
    if (s_modal) {
        if (in_rect(&L->modal_ok, x, y)) finish_import(1);
        else if (in_rect(&L->modal_cancel, x, y)) finish_import(0);
        return;
    }
    if (s_drop >= 0) {
        const Rect r = drop_rect();
        const int i = drop_row_at(x, y);
        if (in_rect(&r, x, y) && i < 0) return;            /* the filter line, or a gap */
        const int f = s_drop, was_cards = s_drop_cards;
        if (i >= 0) {
            const int item = s_drop_items[i];
            /* keep the list mode while applying, so a card list stores a card */
            s_drop = f; s_drop_cards = was_cards;
            enum_set(f, item);
            if (s_drop == f && s_drop_cards && !was_cards) return;   /* "a card…" opened the card list */
        }
        drop_close();
        return;
    }
    if (s_focus >= 0 && in_rect(&L->value[s_focus], x, y)) {
        /* a click in the box being edited moves the caret; shift extends, double-click takes the word, triple all */
        const int i = text_index_at(x, y);
        const int shift = (SDL_GetModState() & KMOD_SHIFT) != 0;
        caret_move(i, shift);
        if (clicks >= 3) select_all();
        else if (clicks == 2) select_word();
        else s_drag_text = 1;
        return;
    }
    if (s_focus >= 0) focus_commit();
    for (int i = 0; i < 2; i++)
        if (in_rect(&L->tab[i], x, y)) { s_tab = i; s_dirty = 1; return; }
    if (in_rect(&L->list, x, y)) {
        if (s_order_n > L->rows && x >= L->sb.x - px(4.0f) && y >= L->sb.y && y < L->sb.y + L->sb.h) {
            int th = L->sb.h * L->rows / s_order_n; if (th < px(12.0f)) th = px(12.0f);
            const int ty = L->sb.y + (L->sb.h - th) * s_scroll / (s_order_n - L->rows);
            s_sb_drag = 1;
            if (y >= ty && y < ty + th) s_sb_grab = y - ty;
            else { s_sb_grab = th / 2; set_scroll_from_thumb(y); }
            s_dirty = 1;
            return;
        }
        if (in_rect(&L->list_rows, x, y)) {
            const int i = (y - L->list_rows.y) / L->row_h;
            if (i >= 0 && i < L->rows && s_scroll + i < s_order_n) select_card(s_order[s_scroll + i]);
        }
        return;
    }
    for (int f = 0; f < F_COUNT; f++) {
        if (!field_applies(f)) continue;
        if (in_rect(&L->value[f], x, y)) {
            if (field_is_enum(f)) {
                if (button == 3 && !no_steppers(f)) field_step(f, -1);
                else drop_open(f);
            } else { focus_begin(f); s_car = text_index_at(x, y); s_anchor = -1; s_drag_text = 1; }
            return;
        }
        if (field_is_enum(f) && !no_steppers(f)) {
            if (in_rect(&L->step_l[f], x, y)) { field_step(f, -1); return; }
            if (in_rect(&L->step_r[f], x, y)) { field_step(f, +1); return; }
        }
        if (field_is_set(f) && in_rect(&L->clear[f], x, y)) { field_clear(f); return; }
    }
    for (int b = 0; b < B_COUNT; b++) {
        if (!L->btn[b].w || !in_rect(&L->btn[b], x, y)) continue;
        switch (b) {
        case B_SAVE: do_save(); break;
        case B_RESTORE: do_restore(); break;
        case B_FOLDER: do_open_folder(); break;
        case B_ART: do_pick(1); break;
        case B_THUMB: do_pick(2); break;
        case B_TITLE: do_pick(3); break;
        case B_EFFECT_TEXT: do_effect_text(); break;
        case B_ADD_RULE: rule_add(); break;
        case B_EXPORT: do_export(); break;
        case B_IMPORT: do_import(); break;
        case B_EXPORT_TEXTS: do_export_texts(); break;
        case B_IMPORT_TEXTS: do_import_texts(); break;
        case B_DEV:
            if (psx_card_packs_is_dev()) { psx_card_packs_set_dev(0); say("Switching to your own cards"); }
            else { s_modal = MODAL_ACTIVATE; s_modal_hover = -1; s_dirty = 1; }
            break;
        }
        return;
    }
}

static void scroll_by(int amount)
{
    s_scroll += amount;
    clamp_scroll();
    s_dirty = 1;
}

/* Mouse events arrive in window coordinates; the canvas is the renderer's
 * output size, which differs on a scaled desktop (1480x820 window, 1480x792
 * output here). Everything geometric is in canvas pixels. */
static void to_canvas(float wx, float wy, int *cx, int *cy)
{
    int ww = 0, wh = 0;
    SDL_GetWindowSize(s_win, &ww, &wh);
    *cx = (ww > 0 && s_w > 0) ? (int)(wx * (float)s_w / (float)ww + 0.5f) : (int)wx;
    *cy = (wh > 0 && s_h > 0) ? (int)(wy * (float)s_h / (float)wh + 0.5f) : (int)wy;
}

static int on_event(const void *evp)
{
    const SDL_Event *ev = (const SDL_Event *)evp;
    if (!s_win) return 0;
    const Uint32 id = SDL_GetWindowID(s_win);
    switch (ev->type) {
    case SDL_MOUSEBUTTONDOWN:
        if (ev->button.windowID != id) return 0;
        { int cx, cy; to_canvas((float)ev->button.x, (float)ev->button.y, &cx, &cy); click(cx, cy, ev->button.button, (int)ev->button.clicks); }
        return 1;
    case SDL_MOUSEBUTTONUP:
        if (ev->button.windowID != id) return 0;
        if (s_sb_drag) { s_sb_drag = 0; s_dirty = 1; }
        s_drag_text = 0;
        return 1;
    case SDL_MOUSEMOTION: {
        if (ev->motion.windowID != id) return 0;
        int x, y; to_canvas((float)ev->motion.x, (float)ev->motion.y, &x, &y);
        if (s_sb_drag) { set_scroll_from_thumb(y); return 1; }
        if (s_drag_text && s_focus >= 0) {
            /* dragging selects */
            const int i = text_index_at(x, y);
            if (s_anchor < 0) s_anchor = s_car;
            if (i != s_car) { s_car = i; s_dirty = 1; }
            return 1;
        }
        int row = -1, btn = -1;
        if (s_drop >= 0) {
            const int h = drop_row_at(x, y);
            if (h != s_drop_hover) { s_drop_hover = h; s_dirty = 1; }
            return 1;
        }
        if (s_modal) {
            const int mh = in_rect(&s_L.modal_ok, x, y) ? 0 : in_rect(&s_L.modal_cancel, x, y) ? 1 : -1;
            if (mh != s_modal_hover) { s_modal_hover = mh; s_dirty = 1; }
            return 1;
        }
        if (in_rect(&s_L.list_rows, x, y)) {
            row = (y - s_L.list_rows.y) / s_L.row_h;
            if (row >= s_L.rows) row = -1;
        }
        for (int b = 0; b < B_COUNT; b++) if (in_rect(&s_L.btn[b], x, y)) btn = b;
        if (row != s_hover_row || btn != s_hover_btn) { s_hover_row = row; s_hover_btn = btn; s_dirty = 1; }
        return 1;
    }
    case SDL_MOUSEWHEEL: {
        if (ev->wheel.windowID != id) return 0;
#if defined(PSX_SDL3)
        const int mx = (int)ev->wheel.mouse_x;
#else
        int mx = 0, my = 0; SDL_GetMouseState(&mx, &my);
#endif
        if (s_drop >= 0) {
            const int mxs = drop_items() - DROP_ROWS;
            s_drop_scroll += ev->wheel.y > 0 ? -2 : 2;
            if (s_drop_scroll > mxs) s_drop_scroll = mxs;
            if (s_drop_scroll < 0) s_drop_scroll = 0;
            s_dirty = 1;
            return 1;
        }
        if (mx < s_L.list.x + s_L.list.w) scroll_by(ev->wheel.y > 0 ? -3 : 3);
        return 1;
    }
    case SDL_KEYUP:
        return ev->key.windowID == id;
    case SDL_KEYDOWN: {
        if (ev->key.windowID != id) return 0;
#if defined(PSX_SDL3)
        const int key = (int)ev->key.key;
#else
        const int key = (int)ev->key.keysym.sym;
#endif
        if (s_modal) {
            if (key == SDLK_ESCAPE) finish_import(0);
            else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) finish_import(1);
            return 1;
        }
        if (s_drop >= 0) {
            const int n = drop_items();
            if (key == SDLK_ESCAPE) { if (s_drop_filter[0]) { s_drop_filter[0] = 0; s_drop_scroll = 0; s_drop_hover = -1; s_dirty = 1; } else drop_close(); }
            else if (key == SDLK_BACKSPACE) { const size_t l = strlen(s_drop_filter); if (l) { s_drop_filter[l - 1] = 0; s_drop_scroll = 0; s_drop_hover = -1; s_dirty = 1; } }
            else if (key == SDLK_DOWN || key == SDLK_UP) {
                /* move the highlight through the shown options */
                int h = s_drop_hover;
                if (h < 0) { const int c = enum_current_index(s_drop); h = 0; for (int i = 0; i < n; i++) if (s_drop_items[i] == c) h = i; }
                h += key == SDLK_DOWN ? 1 : -1;
                if (h < 0) h = 0;
                if (h > n - 1) h = n - 1;
                s_drop_hover = h;
                if (h < s_drop_scroll) s_drop_scroll = h;
                if (h >= s_drop_scroll + DROP_ROWS) s_drop_scroll = h - DROP_ROWS + 1;
                s_dirty = 1;
            }
            else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
                int h = s_drop_hover;
                if (h < 0 && s_drop_filter[0] && n > 0) h = 0;           /* Enter takes the first match */
                if (h >= 0 && h < n) {
                    const int f = s_drop, was_cards = s_drop_cards, item = s_drop_items[h];
                    enum_set(f, item);
                    if (s_drop == f && s_drop_cards && !was_cards) return 1;
                }
                drop_close();
            }
            return 1;
        }
        if (s_focus >= 0) {
            if (key == SDLK_RETURN || key == SDLK_KP_ENTER) focus_commit();
            else if (key == SDLK_ESCAPE) { s_focus = -1; s_dirty = 1; }
            else if (text_key(key, (int)KEY_MOD(ev))) {}
            else if (key == SDLK_TAB) { const int f = s_focus; focus_commit(); int nf = f + 1; while (nf < F_COUNT && (field_is_enum(nf) || !field_applies(nf))) nf++; if (nf < F_COUNT) focus_begin(nf); }
            return 1;
        }
        if (key == SDLK_ESCAPE) {
            if (s_search[0]) { s_search[0] = 0; rebuild_order(); }
            else psx_card_manager_close();
        } else if (key == SDLK_BACKSPACE) {
            const size_t n = strlen(s_search);
            if (n) { s_search[n - 1] = 0; s_scroll = 0; rebuild_order(); }
        } else if (key == SDLK_PAGEUP) scroll_by(-s_L.rows);
        else if (key == SDLK_PAGEDOWN) scroll_by(s_L.rows);
        else if (key == SDLK_DOWN || key == SDLK_UP) {
            int k = 0;
            for (; k < s_order_n && s_order[k] != s_sel; k++) {}
            k += key == SDLK_DOWN ? 1 : -1;
            if (k >= 0 && k < s_order_n) {
                select_card(s_order[k]);
                if (k < s_scroll) s_scroll = k;
                if (k >= s_scroll + s_L.rows) s_scroll = k - s_L.rows + 1;
            }
        } else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && s_changed) do_save();
        s_dirty = 1;
        return 1;
    }
    case SDL_TEXTINPUT:
        if (ev->text.windowID != id) return 0;
        for (const char *p = ev->text.text; *p; p++) {
            const unsigned char ch = (unsigned char)*p;
            if (ch < 32u || ch >= 127u) continue;
            if (s_drop >= 0) {
                const size_t l = strlen(s_drop_filter);
                if (l + 1 < sizeof s_drop_filter) { s_drop_filter[l] = (char)ch; s_drop_filter[l + 1] = 0; s_drop_scroll = 0; s_drop_hover = -1; }
            } else if (s_focus >= 0) {
                const char one[2] = { (char)ch, 0 };
                text_insert(one);
            } else {
                const size_t n = strlen(s_search);
                if (n + 1 < sizeof s_search) { s_search[n] = (char)ch; s_search[n + 1] = 0; s_scroll = 0; rebuild_order(); }
            }
            s_dirty = 1;
        }
        return 1;
    case SDL_WINDOWEVENT_CLOSE:
        if (ev->window.windowID != id) return 0;
        psx_card_manager_close();
        return 1;
    case SDL_WINDOWEVENT_EXPOSED:     /* uncovered or restored: paint again */
    case SDL_WINDOWEVENT_RESIZED:
    case SDL_WINDOWEVENT_SIZE_CHANGED:
        if (ev->window.windowID != id) return 0;
        s_dirty = 1;
        return 1;
    default:
        break;
    }
    return 0;
}

/* --- window lifecycle ------------------------------------------------------------ */
static void gl_capture(void)
{
    s_gl_win = SDL_GL_GetCurrentWindow();
    s_gl_ctx = SDL_GL_GetCurrentContext();
}
static int s_ren_software;      /* the window draws through its own surface: no context to put back */
static void gl_restore(void)
{
    if (s_ren_software) return;
    if (s_gl_ctx && s_gl_win && SDL_GL_GetCurrentContext() != s_gl_ctx)
        SDL_GL_MakeCurrent(s_gl_win, s_gl_ctx);
}

static int ensure_canvas(int w, int h)
{
    if (w == s_w && h == s_h && s_px && s_tex) return 1;
    if (s_tex) { SDL_DestroyTexture(s_tex); s_tex = NULL; }
    free(s_px);
    s_px = (uint32_t *)malloc((size_t)w * (size_t)h * 4u);
    if (!s_px) { s_w = s_h = 0; gl_restore(); return 0; }
    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    gl_restore();
    if (!s_tex) { free(s_px); s_px = NULL; s_w = s_h = 0; return 0; }
    s_w = w; s_h = h;
    s_u = (float)h / 480.0f;
    if (s_u < 1.0f) s_u = 1.0f;
    if (s_u > 8.0f) s_u = 8.0f;
    s_dirty = 1;
    return 1;
}

static void present_canvas(void)
{
    if (!s_ren || !s_tex) return;
    SDL_UpdateTexture(s_tex, NULL, s_px, s_w * 4);
    SDL_RenderClear(s_ren);
    SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
    SDL_RenderPresent(s_ren);
    gl_restore();
    s_present_count++;
}

void psx_card_manager_open(void)
{
    if (s_win) { SDL_RaiseWindow(s_win); return; }
    s_win = SDL_CreateWindow("Card Manager", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             WIN_W, WIN_H, SDL_WINDOW_RESIZABLE);
    if (!s_win) { host_osd_push("Card manager: no window", 2000); return; }
    gl_capture();
    /* The software renderer first: it draws through the window's own surface
     * and touches no GL context, which is what a second window wants beside
     * the game's OpenGL. Wayland has no window surfaces, so there it fails
     * and the accelerated renderer is used with the context put back after
     * every call; on Windows a GL renderer on this window was reported to
     * leave it blank white, and the software one avoids that. */
    s_ren = NULL;
    s_ren_software = 0;
    {
        /* Which renderer draws the tool window. Default: the software one on
         * Windows (it paints through the window surface and touches no GL
         * context; the GL one there was reported to leave the window blank
         * white) and the accelerated one elsewhere (Wayland has no usable
         * window surface: the software renderer is created and then shows a
         * black box). PSX_TOOL_RENDERER=software|accelerated|<driver name>
         * overrides it for trying things without a rebuild. */
        const char *pick = getenv("PSX_TOOL_RENDERER");
#if defined(_WIN32)
        int want_software = 1;
#else
        int want_software = 0;
#endif
        if (pick && *pick) want_software = !strcmp(pick, "software");
#if defined(PSX_SDL3)
        if (pick && *pick && strcmp(pick, "software") && strcmp(pick, "accelerated")) {
            /* a named driver (opengl, direct3d11, vulkan, ...) through the
             * properties API, which the SDL2-shape shim does not cover */
            const SDL_PropertiesID props = SDL_CreateProperties();
            SDL_SetStringProperty(props, SDL_PROP_RENDERER_CREATE_NAME_STRING, pick);
            SDL_SetPointerProperty(props, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, s_win);
            s_ren = SDL_CreateRendererWithProperties(props);
            SDL_DestroyProperties(props);
        } else
#endif
        if (want_software) { s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_SOFTWARE); s_ren_software = s_ren != NULL; }
    }
    if (!s_ren) s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_ACCELERATED);
    if (!s_ren) s_ren = SDL_CreateRenderer(s_win, -1, 0);
    gl_restore();
#if defined(PSX_SDL3)
    if (s_ren) fprintf(stderr, "%s: renderer %s%s\n", SDL_GetWindowTitle(s_win), SDL_GetRendererName(s_ren), s_ren_software ? " (window surface)" : "");
#endif
    if (!s_ren) {
        SDL_DestroyWindow(s_win); s_win = NULL;
        host_osd_push("Card manager: no renderer", 2000);
        return;
    }
    if (!ensure_canvas(WIN_W, WIN_H)) { psx_card_manager_close(); return; }
    rebuild_order();
    load_editor();
    SDL_StartTextInput(s_win);
}

void psx_card_manager_close(void)
{
    if (s_tex) { SDL_DestroyTexture(s_tex); s_tex = NULL; }
    if (s_ren) { SDL_DestroyRenderer(s_ren); s_ren = NULL; }
    if (s_win) { SDL_DestroyWindow(s_win); s_win = NULL; }
    gl_restore();
    s_ren_software = 0;
    free(s_px); s_px = NULL;
    s_w = s_h = 0;
    s_hover_row = -1;
    s_hover_btn = -1;
    s_focus = -1;
    s_sb_drag = 0;
}

int psx_card_manager_is_open(void) { return s_win != NULL; }

void psx_card_manager_select(int id)
{
    if (!s_win) return;
    select_card(id);
}

static void tick(void)
{
    const int req = s_open_req;
    if (req) { s_open_req = 0; if (req > 0) psx_card_manager_open(); else psx_card_manager_close(); }
    if (!s_win) return;
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(s_ren, &w, &h);
    if (w > 0 && h > 0 && (w != s_w || h != s_h)) {
        if (!ensure_canvas(w, h)) { psx_card_manager_close(); return; }
    }
    {
        const int on = ((SDL_GetTicks() / 530u) & 1u) == 0u;
        if (on != s_caret_on) { s_caret_on = on; s_dirty = 1; }
    }
    if (s_pick_path[0]) {
        const int kind = s_pick_kind;
        char src[1024]; snprintf(src, sizeof src, "%s", s_pick_path);
        s_pick_path[0] = 0;
        if (kind == 4) begin_export(src);
        else if (kind == 5) begin_import(src);
        else if (kind == 6 || kind == 7) {
            if (s_focus >= 0) focus_commit();
            char err[512];
            const int ok = kind == 6 ? psx_card_texts_export(src, err, sizeof err) : psx_card_texts_import(src, err, sizeof err);
            say(err[0] ? err : (ok ? "Done" : "That did not work"));
            if (kind == 7) { rebuild_order(); load_editor(); }
        }
        else if (install_pick(src, kind)) {
            psx_card_packs_reload(s_sel);
            say(kind == 1 ? "Face art installed" : kind == 2 ? "Duel thumbnail installed" : "Title strip installed");
            PsxCardPack p;
            if (psx_card_packs_get(s_sel, &p)) { s_edit.has_art = p.has_art; s_edit.has_thumb = p.has_thumb; s_edit.has_title = p.has_title; s_has_pack = 1; }
            rebuild_order();
        } else say("Could not copy that file into the card's folder");
    }
    static int last_ready = -1;
    const int ready = psx_card_db_ready();
    if (ready != last_ready) { last_ready = ready; rebuild_order(); if (ready) load_editor(); }
    const unsigned gen = psx_card_packs_generation();
    if (gen != s_seen_gen) {
        s_seen_gen = gen;
        if (!s_changed && s_focus < 0) load_editor();
        s_dirty = 1;
    }
    refresh_preview();
    if (s_msg[0] && SDL_GetTicks() >= s_msg_until) { s_msg[0] = 0; s_dirty = 1; }
    if (s_dirty) {
        draw();
        s_dirty = 0;
        present_canvas();
    }
}

/* --- debug plumbing ---------------------------------------------------------------- */
void psx_card_manager_request_open(int open) { s_open_req = open ? 1 : -1; }

int psx_card_manager_state_json(char *out, unsigned cap)
{
    static char t[F_COUNT][FTEXT];
    for (int f = 0; f < F_COUNT; f++) field_text(f, 0, t[f], sizeof t[f]);
    if (s_win) layout_compute();
    unsigned n = (unsigned)snprintf(out, cap,
        "\"open\":%d,\"card\":%d,\"edited\":%d,\"changed\":%d,\"focus\":%d,\"buf\":\"%s\","
        "\"search\":\"%s\",\"rows\":%d,\"scroll\":%d,\"w\":%d,\"h\":%d,\"unit\":%.2f,\"msg\":\"%s\","
        "\"name\":\"%s\",\"desc\":\"%s\",\"atk\":\"%s\",\"def\":\"%s\",\"star1\":\"%s\",\"star2\":\"%s\",\"type\":\"%s\","
        "\"level\":\"%s\",\"attr\":\"%s\",\"price\":\"%s\",\"password\":\"%s\","
        "\"color\":\"%s\",\"effect\":\"%s\",\"amount\":\"%s\",\"target\":\"%s\",\"terrain\":\"%s\",\"ritual\":\"%s\",\"equip_bonus\":\"%s\",\"equips\":\"%.200s\",\"boost\":\"%.200s\",\"trap_max\":\"%s\","
        "\"art\":%d,\"thumb\":%d,\"title\":%d,\"presents\":%u,\"modal\":%d,\"geom\":{",
        s_win != NULL, s_sel, s_has_pack, s_changed, s_focus, s_buf, s_search, s_order_n, s_scroll,
        s_w, s_h, s_u, s_msg, t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], t[9], t[10],
        t[11], t[12], t[13], t[14], t[15], t[16], t[17], t[18], t[19], t[20],
        s_edit.has_art, s_edit.has_thumb, s_edit.has_title, s_present_count, s_modal);
    {
        char tr[6][256], bo[256];
        for (int i = 0; i < 6; i++) psx_card_packs_format_trigger(trig_at(i), tr[i], sizeof tr[i]);
        psx_card_packs_format_bonus(&s_edit, bo, sizeof bo);
        if (n < cap) n += (unsigned)snprintf(out + n, cap - n, "\"monster\":{\"battle\":\"%s\",\"on_summon\":\"%s\",\"on_flip\":\"%s\",\"on_death\":\"%s\",\"on_attack\":\"%s\",\"each_turn\":\"%s\",\"opp_turn\":\"%s\",\"bonus\":\"%s\",\"immune\":\"%s\"},"
                                             "\"drop\":%d,\"drop_cards\":%d,\"drop_n\":%d,\"drop_filter\":\"%s\",\"tab\":%d,\"rule_first\":%d,\"rule_parts\":%d,\"rules\":%d,\"caret\":%d,\"anchor\":%d",
                                             s_edit.battle > 0 ? psx_card_packs_battle_name(s_edit.battle) : "none", tr[0], tr[1], tr[2], tr[3], tr[4], tr[5], bo, t[F_IMMUNE],
                                             s_drop, s_drop_cards, s_drop >= 0 ? drop_items() : 0, s_drop_filter, s_tab, F_RULE_FIRST, RP_N, s_rule_n, s_car, s_anchor);
        if (n < cap) n += (unsigned)snprintf(out + n, cap - n, ",\"tabs\":[[%d,%d],[%d,%d]]", s_L.tab[0].x + s_L.tab[0].w / 2, s_L.tab[0].y + s_L.tab[0].h / 2, s_L.tab[1].x + s_L.tab[1].w / 2, s_L.tab[1].y + s_L.tab[1].h / 2);
    }
    if (s_win && n < cap) {
        n += (unsigned)snprintf(out + n, cap - n, ",\"rows\":[%d,%d,%d,%d],\"row_h\":%d,\"visible\":%d,\"sb\":[%d,%d,%d,%d],\"value\":[",
                                s_L.list_rows.x, s_L.list_rows.y, s_L.list_rows.w, s_L.list_rows.h, s_L.row_h, s_L.rows,
                                s_L.sb.x, s_L.sb.y, s_L.sb.w, s_L.sb.h);
        for (int f = 0; f < F_COUNT && n < cap; f++)
            n += (unsigned)snprintf(out + n, cap - n, "%s[%d,%d,%d,%d]", f ? "," : "", s_L.value[f].x, s_L.value[f].y, s_L.value[f].w, s_L.value[f].h);
        if (n < cap) n += (unsigned)snprintf(out + n, cap - n, "],\"step_r\":[");
        for (int f = 0; f < F_COUNT && n < cap; f++)
            n += (unsigned)snprintf(out + n, cap - n, "%s[%d,%d]", f ? "," : "", s_L.step_r[f].x + s_L.step_r[f].w / 2, s_L.step_r[f].y + s_L.step_r[f].h / 2);
        if (n < cap) n += (unsigned)snprintf(out + n, cap - n, "],\"btn\":[");
        for (int b = 0; b < B_COUNT && n < cap; b++)
            n += (unsigned)snprintf(out + n, cap - n, "%s[%d,%d]", b ? "," : "", s_L.btn[b].x + s_L.btn[b].w / 2, s_L.btn[b].y + s_L.btn[b].h / 2);
        if (n < cap) n += (unsigned)snprintf(out + n, cap - n, "],\"applies\":[");
        for (int f = 0; f < F_COUNT && n < cap; f++) n += (unsigned)snprintf(out + n, cap - n, "%s%d", f ? "," : "", field_applies(f));
        if (n < cap) n += (unsigned)snprintf(out + n, cap - n, "],\"modal_ok\":[%d,%d],\"modal_cancel\":[%d,%d]",
                                             s_L.modal_ok.x + s_L.modal_ok.w / 2, s_L.modal_ok.y + s_L.modal_ok.h / 2,
                                             s_L.modal_cancel.x + s_L.modal_cancel.w / 2, s_L.modal_cancel.y + s_L.modal_cancel.h / 2);
    }
    if (n < cap) n += (unsigned)snprintf(out + n, cap - n, "}");
    return n < cap;
}

/* debug helpers speak canvas pixels; events want window coordinates */
static void from_canvas(int cx, int cy, int *wx, int *wy)
{
    int ww = 0, wh = 0;
    SDL_GetWindowSize(s_win, &ww, &wh);
    *wx = (s_w > 0 && ww > 0) ? (int)((long)cx * ww / s_w) : cx;
    *wy = (s_h > 0 && wh > 0) ? (int)((long)cy * wh / s_h) : cy;
}

static int inject_button(int x, int y, int button, int down)
{
    SDL_Event ev;
    if (!s_win) return 0;
    from_canvas(x, y, &x, &y);
    SDL_zero(ev);
    ev.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    ev.button.windowID = SDL_GetWindowID(s_win);
    ev.button.button = (Uint8)button;
#if defined(PSX_SDL3)
    ev.button.down = down ? true : false;
#else
    ev.button.state = down ? SDL_PRESSED : SDL_RELEASED;
#endif
    ev.button.clicks = 1;
    ev.button.x = x;
    ev.button.y = y;
    return SDL_PushEvent(&ev) == 1;
}

int psx_card_manager_button(int x, int y, int button, int down)
{
    return inject_button(x, y, button <= 0 ? SDL_BUTTON_LEFT : button, down);
}

int psx_card_manager_move(int x, int y)
{
    SDL_Event ev;
    if (!s_win) return 0;
    SDL_zero(ev);
    ev.type = SDL_MOUSEMOTION;
    ev.motion.windowID = SDL_GetWindowID(s_win);
    from_canvas(x, y, &x, &y);
    ev.motion.x = x;
    ev.motion.y = y;
    return SDL_PushEvent(&ev) == 1;
}

int psx_card_manager_click(int x, int y, int button)
{
    if (!s_win) return 0;
    if (button <= 0) button = SDL_BUTTON_LEFT;
    if (!inject_button(x, y, button, 1)) return 0;
    return inject_button(x, y, button, 0);
}

int psx_card_manager_type(const char *text)
{
    if (!s_win || !text) return 0;
    static char keep[64];
    snprintf(keep, sizeof keep, "%s", text);
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_TEXTINPUT;
    ev.text.windowID = SDL_GetWindowID(s_win);
#if defined(PSX_SDL3)
    ev.text.text = keep;
#else
    snprintf(ev.text.text, sizeof ev.text.text, "%s", keep);
#endif
    return SDL_PushEvent(&ev) == 1;
}

int psx_card_manager_key_mod(int sdl_key, int mods)
{
    if (!s_win) return 0;
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_KEYDOWN;
    ev.key.windowID = SDL_GetWindowID(s_win);
    const int m = ((mods & 1) ? KMOD_LSHIFT : 0) | ((mods & 2) ? KMOD_LCTRL : 0);
#if defined(PSX_SDL3)
    ev.key.key = (SDL_Keycode)sdl_key;
    ev.key.down = true;
    ev.key.mod = (SDL_Keymod)m;
#else
    ev.key.keysym.sym = (SDL_Keycode)sdl_key;
    ev.key.keysym.mod = (Uint16)m;
    ev.key.state = SDL_PRESSED;
#endif
    return SDL_PushEvent(&ev) == 1;
}
int psx_card_manager_key(int sdl_key) { return psx_card_manager_key_mod(sdl_key, 0); }

void psx_card_manager_import_preview(const char *path)
{
    if (!s_win || !path || !path[0]) return;
    begin_import(path);
}

void psx_card_manager_ask_activate(void)
{
    if (!s_win) s_open_req = 1;      /* the emulation thread opens it on its next tick */
    s_modal = MODAL_ACTIVATE; s_modal_hover = -1; s_dirty = 1;
}

void psx_card_manager_search(const char *text)
{
    snprintf(s_search, sizeof s_search, "%s", text ? text : "");
    s_scroll = 0;
    rebuild_order();
}

int psx_card_manager_shot(const char *path)
{
    if (!s_win || !s_px || !path) return 0;
    if (s_dirty) { draw(); s_dirty = 0; }
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fprintf(f, "P6\n%d %d\n255\n", s_w, s_h);
    for (int i = 0; i < s_w * s_h; i++) {
        const uint32_t c = s_px[i];
        const unsigned char rgb[3] = { (unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return 1;
}

static void row_activate(void) { psx_card_manager_open(); }

PSX_MOD_CONSTRUCTOR(psx_card_manager_install)
{
    (void)psx_video_menu_add_action(PSX_VM_MENU_VIEW, "Card manager",
                                    "Change a card's name, description, art, frame colour, stats, stars, effects, price and password; export or import them",
                                    row_activate);
    (void)psx_game_add_frame_hook(tick);
    (void)psx_game_add_event_hook(on_event);
}
