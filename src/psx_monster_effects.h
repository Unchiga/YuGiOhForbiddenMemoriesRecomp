/* psx_monster_effects.h -- effects on monsters, which the stock game has none of.
 *
 * card.ini keys, all optional:
 *   battle    = indestructible | mutual | slayer     (what happens when it fights)
 *   on_summon = <effect> [number] [type] [field]     (a magic effect, cast when it lands face-up)
 *               a trigger holds up to four branches, "50%: raigeki; else: destroy_own":
 *               each branch rolls its own chance, "else:" fires when the one before it did not
 *   on_death  = ...                                  (cast when it is destroyed)
 *   on_attack = ...                                  (cast when it declares an attack)
 *   each_turn = ...                                  (cast at the start of its owner's turn)
 *   bonus     = 500, 200 per ally, 100 per enemy     (ATK and DEF while on the field)
 *   immune    = traps, magic                         (traps never fire on it; destruction magic skips it)
 * See psx_monster_effects.c for how each is made. */
#ifndef PSX_MONSTER_EFFECTS_H
#define PSX_MONSTER_EFFECTS_H

#ifdef __cplusplus
extern "C" {
#endif

int psx_monster_effects_state_json(char *out, unsigned cap);

#ifdef __cplusplus
}
#endif

#endif /* PSX_MONSTER_EFFECTS_H */
