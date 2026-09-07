/* psx_card_name_color.c — draws a card's NAME in the color the Card Manager
 * gave that card.
 *
 * There is no mod row and no rarity ladder any more. The color is one more
 * field on the card, beside its frame color: set it in VIEW > CARD MANAGER,
 * it is written to cards/<id>/card.ini as `name_color = red`, and it travels
 * with Export / Import Config like every other field. A card without one
 * draws the way the game draws it, so a stock install looks stock.
 *
 * WHICH TEXT IS THE NAME
 * ----------------------
 * The mechanism below is yamyi's, from PR #18; only the source of the color
 * changed.
 *
 * func_80038B4C is the text interpreter: it reads one opcode byte from the
 * live cursor at obj[obj[0x58] * 4], advances it, and calls
 * D_80090EAC[op](obj) — and CARD_NAME_CALLER is precisely that jalr's return
 * address. func_80037DA4 is the card-field handler behind several of those
 * opcodes, and its FIRST act is to read one more byte, the mode byte, which
 * says what to draw:
 *
 *     mode & 0x10   set the widget's color from D_8009B320, then return
 *     mode & 0x20   the card's NAME        (string id  gDuel_wSelectedCardID + 0x8000)
 *     mode & 0x40   the description        (string id  gDuel_wSelectedCardID + 0xD100)
 *     mode & 0x0F   0 type, 1 attribute, 2 Guardian Star — small label ids
 *                   built out of gDuel_adwCardStats[id - 1]
 *
 * tested in that order, 0x10 first. The hook runs before any of the
 * function's own instructions, so the cursor is still on that byte and
 * reading it costs nothing:
 *
 *     mode = [ obj[ obj[0x58] * 4 ] ]
 *     name = !(mode & 0x10) && (mode & 0x20)
 *
 * That is exact and needs no guesswork about the text's content.
 *
 * THE COLOR IS STICKY
 * --------------------
 * func_80036C14's a0 is the glyph record; byte +84 is its color index,
 * copied into every primitive it emits, so it stays applied to every later
 * glyph on that widget until overwritten. The name's color therefore has to
 * be taken back off once the name is done.
 *
 * A name command does not draw anything itself: it resolves the string id to
 * 0x801D0000 + u16[0x801D5800 + id*2], PUSHES that address as a new cursor
 * (obj[0x58]++), and lets the interpreter run the name as a nested stream.
 * So the name is exactly the glyphs emitted while obj[0x58] is deeper than it
 * was when the command was entered; the first glyph back at that depth is
 * past the name, and the color byte goes back to the value the game had in
 * it. Any later card-field command restores it too, as a backstop.
 *
 * The restore only fires if the byte still holds what this wrote — if the
 * game issued its own mode 0x10 color command in between, that value is
 * current and must stand.
 */

#include <stdint.h>

#include "cpu_state.h"
#include "mod_plugins.h"
#include "psx_card_packs.h"

#define PSX_TEXT_FN        0x80037DA4u
#define PSX_GLYPH_FN       0x80036C14u   /* per-decoded-character callback */
#define CARD_NAME_CALLER   0x80038B98u   /* $ra for any card-info widget's text */
#define PSX_SELECTED_CARD  0x8009B338u   /* u16, id of the card being drawn */

#define GLYPH_COLOR_OFF    84u

/* At the hook the live cursor is still sitting on the command's mode byte,
 * and that byte says which of the four fields follows. */
#define TEXT_STACK_OFF     0x58u   /* s8: which of the widget's cursors is live */
#define TEXT_SLOTS         12
#define MODE_SET_COLOR     0x10u   /* tested first by the handler, so it wins */
#define MODE_CARD_NAME     0x20u

#define CARD_ID_MAX        722

/* The address the interpreter is about to read, and how deep the cursor stack
 * is. Returns 0 when the live slot is not a real cursor. */
static uint32_t text_cursor(uint32_t obj, int *depth)
{
    const int slot = (int8_t)psx_mod_read_byte(obj + TEXT_STACK_OFF);
    *depth = slot;
    if (slot < 0 || slot >= TEXT_SLOTS)
        return 0u;
    return psx_mod_read_word(obj + (uint32_t)slot * 4u);
}

/* The card's own color, or -1 when it has none. One pack lookup per card
 * field the game draws, which is a handful per screen. */
static int card_name_color(int id)
{
    PsxCardPack p;
    if (id < 1 || id > CARD_ID_MAX) return -1;
    if (!psx_card_packs_get(id, &p)) return -1;
    if (p.name_color < 0 || p.name_color >= PSX_CARD_NAME_COLOR_COUNT) return -1;
    return p.name_color;
}

/* The widget currently carrying this layer's color, and what it displaced. */
static uint32_t s_tint_obj;      /* 0 when nothing is tinted */
static int      s_tint_depth;    /* obj[0x58] as the name command was entered */
static uint8_t  s_tint_color;    /* what was written */
static uint8_t  s_tint_prev;     /* what was there before */

/* Put the widget's color byte back, so the name's color does not carry on
 * into the type line, the Guardian Stars or the description. */
static void tint_restore(uint32_t obj)
{
    if (!s_tint_obj || obj != s_tint_obj)
        return;
    if (psx_mod_read_byte(obj + GLYPH_COLOR_OFF) == s_tint_color)
        psx_mod_write_byte(obj + GLYPH_COLOR_OFF, s_tint_prev);
    s_tint_obj = 0u;
}

static void card_name_text(CPUState *cpu, uint32_t address)
{
    if (!cpu || address != PSX_TEXT_FN || cpu->gpr[31] != CARD_NAME_CALLER)
        return;

    const uint32_t obj = cpu->gpr[4];
    int depth = -1;
    const uint32_t cursor = text_cursor(obj, &depth);
    if (!cursor)
        return;

    const unsigned mode = psx_mod_read_byte(cursor);

    /* Every card-field command that is not the name ends the previous one's
     * color — and so does the name's own command, before it re-tints. */
    tint_restore(obj);

    if ((mode & MODE_SET_COLOR) || !(mode & MODE_CARD_NAME))
        return;

    const int col = card_name_color((int)psx_mod_read_half(PSX_SELECTED_CARD));
    if (col < 0)
        return;

    s_tint_prev  = psx_mod_read_byte(obj + GLYPH_COLOR_OFF);
    s_tint_color = (uint8_t)col;
    psx_mod_write_byte(obj + GLYPH_COLOR_OFF, s_tint_color);
    s_tint_obj   = obj;
    s_tint_depth = depth;
}

static void card_name_glyph(CPUState *cpu, uint32_t address)
{
    if (!cpu || address != PSX_GLYPH_FN || !s_tint_obj)
        return;

    const uint32_t obj = cpu->gpr[4];
    if (obj != s_tint_obj)
        return;

    /* The name runs as a stream the command pushed, one level deeper than the
     * command was entered at. A glyph emitted back at that depth is already
     * past the name, so the color comes off before it is drawn. */
    if ((int)(int8_t)psx_mod_read_byte(obj + TEXT_STACK_OFF) <= s_tint_depth)
        tint_restore(obj);
}

PSX_MOD_CONSTRUCTOR(psx_card_name_color_install)
{
    (void)psx_mod_register_function_entry_plugin(
        "ygofm.card_name_color.text",
        PSX_TEXT_FN,
        card_name_text);
    (void)psx_mod_register_function_entry_plugin(
        "ygofm.card_name_color.glyph",
        PSX_GLYPH_FN,
        card_name_glyph);
}
