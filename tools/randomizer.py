#!/usr/bin/env python3
"""randomizer.py -- build a full-game randomizer as one .ygomods MOD package.

    python3 tools/randomizer.py [--seed N] [--difficulty D] [--out FILE.ygomods]

Needs the DEBUG build running (./Play.sh -dbg --no-launcher) past the boot
screen once: the disc's own card stats, AI profiles, fusion table and equip
groups come out of it through one debug export (whatever mods are loaded at
the time, the STOCK values are what it asks for), so the package is built
from THIS disc's own data. The export is cached in build/randomizer/ and
later runs work with the game closed. The drop tables and deck pools come from the
baked drop database (build/game-assets/psx_drop_db.c).

What the package does, all from stock and all seeded:

  cards        every monster's ATK and DEF are rolled outright, 0 to 4500 in
               steps of 10 (a card the game can deal into a NEW GAME's first
               deck stays at 2900 or under, so no start opens with a 3000+
               card), its level follows the new numbers, and its type (any
               of the twenty monster types), attribute and both guardian
               stars are rolled; every card
               gets a random frame color, name color, price and password; a
               set of monsters gets battle rules, immunities and cast
               effects; a set of Magic cards gets a different effect. Every
               card whose effect changed gets its description replaced by a
               short text saying what it now does (six lines of twenty).
  drop tables  every duelist's three bands keep their drop count; monster
               slots get random monsters (every monster in the game is
               dropped by somebody), magic/trap/equip/ritual slots keep their
               card, every slot gets a fresh weight, each band totals 2048.
               That needs release 0.5.7 or later. --drops compat instead
               pins up to 128 random monsters over each stock table, taking
               most of every band, which 0.5.5 and 0.5.6 load.
  CPU decks    every duelist's pool keeps its size and its monster/spell mix,
               with random cards and random weights. The later a duelist sits
               in the campaign, the stronger the monsters they draw from and
               the heavier their strongest cards weigh, so the randomized
               game still ramps: the pool is a window on the ATK ladder, the
               weakest 65 % for the first duelist and the strongest 25 % for
               the last; --difficulty scales how fast that window climbs (1
               is the default, 2 reaches the top by mid-campaign, 0 keeps
               everyone on the weak end).
  CPU AI       hand size (5 early to 20 late), combo width and fusion depth
               (1 to 3) follow the campaign with a little jitter; the fusion
               deck gate is rolled; the other bytes stay stock.
  fusions      the results of the monster + monster recipes are shuffled
               among themselves.
  equips       every equip card fits a random set of monsters, as many as
               it fits in stock; Megamorph and the other fits-everything
               equip keep fitting everything.
  settings     CARD DROPS 15, DROP MISSING CARDS on, LIBRARY PLACEHOLDERS on.

Import it in the game with MODS > IMPORT MOD PACKAGE. Export your own package
first: an import replaces the parts this file carries.
"""
import argparse, io, json, os, random, re, struct, sys, time, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

NCARDS, TOTAL, NDUEL = 722, 2048, 39
TYPE_MAGIC, TYPE_TRAP, TYPE_RITUAL, TYPE_EQUIP = 20, 21, 22, 23
COLORS = ['yellow', 'green', 'pink', 'blue', 'purple', 'orange']
ATTRS = ['Light', 'Dark', 'Earth', 'Water', 'Fire', 'Wind']
TYPES = ['Dragon', 'Spellcaster', 'Zombie', 'Warrior', 'Beast-Warrior', 'Beast', 'Winged Beast', 'Fiend',
         'Fairy', 'Insect', 'Dinosaur', 'Reptile', 'Fish', 'Sea Serpent', 'Machine', 'Thunder', 'Aqua',
         'Pyro', 'Rock', 'Plant']
STARS = ['Mars', 'Jupiter', 'Saturn', 'Uranus', 'Pluto', 'Neptune', 'Mercury', 'Sun', 'Moon', 'Venus']
NAME_COLORS = ['white', 'yellow', 'blue', 'green', 'grey', 'orange', 'red']
# Every card the game can deal into a NEW GAME's first deck: the seven starter
# sets in the name-entry module (WA_MRG.MRG at 0xF92BD4, 7 x {u16 draws, u16
# weight[722]}), any card with a weight in any set. Their ATK and DEF are
# capped so a fresh game never opens with a card over 3000.
STARTER_POOL = set([3, 5, 8, 9, 10, 23, 24, 25, 29, 30, 34, 40, 47, 48, 50, 53, 58, 59, 61, 65, 75, 76, 80, 100, 101, 102, 104, 105, 107, 108, 109, 110, 112, 113, 114, 115, 116, 118, 119, 120, 121, 122, 123, 129, 130, 132, 133, 134, 135, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 148, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 164, 165, 167, 169, 171, 172, 173, 174, 175, 176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 187, 188, 189, 190, 191, 192, 195, 196, 197, 198, 199, 200, 201, 202, 203, 205, 206, 207, 208, 209, 210, 211, 212, 214, 215, 218, 219, 220, 221, 222, 224, 225, 226, 227, 228, 229, 231, 232, 233, 234, 236, 237, 238, 239, 240, 241, 242, 243, 244, 245, 246, 247, 248, 250, 251, 253, 254, 256, 257, 258, 259, 260, 261, 262, 263, 264, 265, 266, 267, 268, 269, 270, 271, 272, 273, 274, 276, 277, 279, 280, 282, 283, 285, 289, 290, 291, 292, 293, 294, 295, 296, 298, 300, 301, 302, 303, 304, 305, 306, 307, 308, 309, 310, 311, 312, 313, 314, 315, 316, 317, 319, 321, 322, 323, 324, 326, 327, 328, 330, 331, 332, 333, 334, 335, 336, 337, 381, 387, 393, 394, 395, 397, 398, 399, 402, 406, 410, 411, 414, 417, 420, 421, 422, 430, 431, 432, 435, 436, 444, 445, 446, 450, 451, 452, 455, 457, 461, 463, 469, 474, 475, 476, 477, 478, 480, 481, 484, 485, 486, 488, 489, 490, 492, 496, 501, 502, 503, 504, 505, 506, 510, 514, 516, 524, 527, 530, 534, 536, 537, 538, 539, 540, 543, 544, 546, 547, 548, 549, 550, 552, 553, 556, 558, 559, 560, 561, 563, 566, 567, 568, 569, 570, 573, 574, 576, 579, 580, 581, 583, 584, 585, 586, 588, 589, 590, 591, 592, 598, 599, 600, 601, 602, 604, 605, 606, 608, 609, 610, 611, 612, 615, 616, 620, 629, 634, 635, 642, 643, 644, 646, 647, 649, 652, 654, 659])
STARTER_CAP, ATK_CAP = 2900, 4500


# --- stock data -------------------------------------------------------------

def read_stock_from_game(tmp):
    """The DISC's own values, whatever edits are loaded: card stats and
    level/attribute (psx_card_packs_stock), the 40 AI profiles as first seen,
    the stock fusion pairs and the equip groups, all through one debug
    export (`randomizer_stock`)."""
    import dbg
    r = dbg.q({'cmd': 'randomizer_stock', 'path': tmp})
    if not r.get('ok'):
        raise RuntimeError('stock export failed: %s' % r)
    j = json.load(open(tmp))
    cards = {int(k): v for k, v in j['cards'].items()}
    if all(v['atk'] == 0 for v in cards.values()):
        raise RuntimeError('the card table is not resident yet: let the game boot first')
    ai = {int(k): v for k, v in j['ai'].items()}
    fusions = [tuple(f) for f in j['fusions']]
    equips = {int(k): v for k, v in j['equips'].items()}
    return cards, ai, fusions, equips


def read_drop_db():
    """Names, the three drop tiers and the deck pool per duelist, baked."""
    path = None
    for b in ('build', 'build-dbg'):
        p = os.path.join(ROOT, b, 'game-assets', 'psx_drop_db.c')
        if os.path.exists(p):
            path = p
            break
    if not path:
        raise SystemExit('no baked drop database: build the game first')
    src = open(path).read()
    arrs = {m.group(1): [(int(c), int(w)) for c, w in re.findall(r'\{\s*(\d+)\s*,\s*(\d+)\s*\}', m.group(2))]
            for m in re.finditer(r'static const PsxDropWeight (\w+)\[\]\s*=\s*\{(.*?)\};', src, re.S)}
    recs = re.findall(r'\{\s*"([^"]+)"\s*,\s*\{\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*\}\s*,\s*\{[^}]*\}\s*,\s*(\w+)', src)
    if len(recs) != NDUEL:
        raise SystemExit('drop database: expected %d duelists, found %d' % (NDUEL, len(recs)))
    return [dict(name=n, tiers=[arrs[a], arrs[b], arrs[c]], deck=arrs[d]) for n, a, b, c, d in recs]


def read_fusions_from_game(tmp):
    import dbg
    r = dbg.q({'cmd': 'fusion_manager', 'export': tmp})
    if not r.get('ok'):
        raise SystemExit('fusion table export failed: %s' % r)
    out = []
    for ln in open(tmp):
        m = re.match(r'\s*(\d+)\s+(\d+)\s+(\d+)', ln)
        if m:
            out.append(tuple(int(x) for x in m.groups()))
    return out


def read_equips_from_game(tmp):
    """The disc's equip groups: {equip card: [monsters it fits]}."""
    import dbg
    r = dbg.q({'cmd': 'fusion_manager', 'equips_export': tmp})
    if not r.get('ok'):
        raise SystemExit('equip table export failed: %s' % r)
    out = {}
    for ln in open(tmp):
        m = re.match(r'\s*(\d+)\s+(\d+)', ln)
        if m:
            out.setdefault(int(m.group(1)), []).append(int(m.group(2)))
    return out


# --- the rolls --------------------------------------------------------------

def random_weights(rng, n):
    """n weights that total 2048, each at least 1: a square draw, so a band
    has a few heavy entries and a long light tail, the way stock reads."""
    raw = [4 + k * k for k in (rng.randrange(1, 13) for _ in range(n))]
    s = sum(raw)
    w = [max(1, r * TOTAL // s) for r in raw]
    got = sum(w)
    while got < TOTAL:
        i = rng.randrange(n); w[i] += 1; got += 1
    while got > TOTAL:
        i = rng.randrange(n)
        if w[i] > 1:
            w[i] -= 1; got -= 1
    return w


def is_monster(cards, c):
    return cards[c]['type'] < TYPE_MAGIC


def roll_drops(rng, db, cards):
    """Per duelist: {card: [pow, bcd, tec]} covering every stock card (0 when
    it fell out) and every new one. Every monster lands in at least one band,
    and a non-monster nobody drops in stock is given one extra slot."""
    monsters = [c for c in range(1, NCARDS + 1) if is_monster(cards, c)]
    others = [c for c in range(1, NCARDS + 1) if not is_monster(cards, c)]
    # the monster slots, as (duelist, tier, slot index)
    slots = []
    for d, D in enumerate(db):
        for t, tier in enumerate(D['tiers']):
            for i, (c, w) in enumerate(tier):
                if is_monster(cards, c):
                    slots.append((d, t, i))
    assign = {}
    picks = rng.sample(slots, len(monsters))
    for m, s in zip(monsters, picks):
        assign[s] = m
    dropped_other = {c for D in db for tier in D['tiers'] for c, w in tier if not is_monster(cards, c)}
    extra = {}
    for c in others:
        if c not in dropped_other:
            d = rng.randrange(NDUEL); t = rng.randrange(3)
            extra.setdefault((d, t), []).append(c)
    out = []
    for d, D in enumerate(db):
        vec = {}
        for t, tier in enumerate(D['tiers']):
            used = set()
            new = []
            for c, w in tier:
                vec.setdefault(c, [0, 0, 0])
                if not is_monster(cards, c):
                    new.append(c); used.add(c)
            # the guaranteed monsters first, so a random fill never takes one
            for i, (c, w) in enumerate(tier):
                m = assign.get((d, t, i))
                if m is not None:
                    used.add(m); new.append(m)
            for i, (c, w) in enumerate(tier):
                if not is_monster(cards, c) or (d, t, i) in assign:
                    continue
                m = rng.choice(monsters)
                while m in used:
                    m = rng.choice(monsters)
                used.add(m); new.append(m)
            for c in extra.get((d, t), []):
                if c not in used:
                    used.add(c); new.append(c)
            ws = random_weights(rng, len(new))
            for c, w in zip(new, ws):
                vec.setdefault(c, [0, 0, 0])[t] = w
        out.append(vec)
    return out


def ramp(d, difficulty):
    """0 for the first duelist .. 1 for the last, scaled and clamped."""
    return max(0.0, min(1.0, d / float(NDUEL - 1) * difficulty))


def pin_share(n_stock):
    """How much of a band the pins may take on the release's rescale: it
    caps pins at 1984 and gives every surviving stock card at least 1, so a
    band with many stock drops must leave them room (twice their count)."""
    return min(1900, TOTAL - max(80, 2 * n_stock + 16))


def roll_drops_compat(rng, db, cards, per_duelist=128):
    """Per duelist: {card: [pow, bcd, tec]} with at most per_duelist cards,
    none of them a stock drop, each pinned in one or two bands, the pins in
    every band totalling pin_share(). The stock table keeps the remainder.
    Every monster lands in at least one duelist's pins."""
    monsters = [c for c in range(1, NCARDS + 1) if is_monster(cards, c)]
    out = []
    # every card somewhere first: spread the monsters over the duelists, and
    # give the spells nobody drops in stock a home too
    stock_all = {c for D in db for tier in D['tiers'] for c, w in tier}
    must = {d: [] for d in range(NDUEL)}
    order = monsters[:] + [c for c in range(1, NCARDS + 1) if not is_monster(cards, c) and c not in stock_all]
    rng.shuffle(order)
    for i, m in enumerate(order):
        must[i % NDUEL].append(m)
    for d, D in enumerate(db):
        stock = {c for tier in D['tiers'] for c, w in tier}
        picks = [m for m in must[d] if m not in stock]
        seen = set(picks)
        while len(picks) < per_duelist:
            m = rng.choice(monsters)
            if m in seen or m in stock:
                continue
            seen.add(m); picks.append(m)
        # each card sits in one band, a third of them in a second one too
        bands = {c: {rng.randrange(3)} for c in picks}
        for c in picks:
            if rng.random() < 0.34:
                bands[c].add(rng.randrange(3))
        vec = {c: [0, 0, 0] for c in picks}
        for t in range(3):
            members = [c for c in picks if t in bands[c]]
            if not members:
                members = [rng.choice(picks)]
            share = pin_share(len(D['tiers'][t]))
            ws = random_weights(rng, len(members))
            # scale 2048 down to the share, keeping every pin at least 1
            ws = [max(1, w * share // TOTAL) for w in ws]
            got = sum(ws); i = 0
            while got < share:
                ws[i % len(ws)] += 1; got += 1; i += 1
            while got > share:
                j = max(range(len(ws)), key=lambda k: ws[k]); ws[j] -= 1; got -= 1
            for c, w in zip(members, ws):
                vec[c][t] = w
        out.append(vec)
    return out


def old_rescale(w, cards, weights):
    """The release's psx_drop_pins_rescale, ported so a compat package can
    be checked against it here. Returns the tier or None when refused."""
    t = list(w)
    added = sum(weights)
    if added > TOTAL - 64:
        return None
    for c in cards:
        t[c - 1] = 0
    old_sum = sum(t)
    if not old_sum:
        return None
    target = TOTAL - added
    got = 0
    for i in range(NCARDS):
        if t[i]:
            v = t[i] * target // old_sum
            t[i] = max(1, v); got += t[i]
    while got < target:
        i = max(range(NCARDS), key=lambda k: t[k]); t[i] += 1; got += 1
    while got > target:
        i = max(range(NCARDS), key=lambda k: t[k])
        if t[i] <= 1:
            break
        t[i] -= 1; got -= 1
    for c, wt in zip(cards, weights):
        t[c - 1] = wt
    return t if sum(t) == TOTAL else None


def roll_decks(rng, db, cards, new_atk, difficulty):
    """Per duelist: {card: weight}, the pool the same size and the same
    monster/spell split as stock, with random cards and weights. The monster
    picks come from the top slice of the ATK ladder, a slice that narrows
    with the campaign (all 100 % for the first duelist, the top 25 % for the
    last at difficulty 1), and the weights lean on the strong cards the same
    way, so a late duelist both owns and draws their heavy hitters."""
    ladder = sorted((c for c in range(1, NCARDS + 1) if is_monster(cards, c)),
                    key=lambda c: new_atk[c])            # weakest first
    spells = [c for c in range(1, NCARDS + 1) if cards[c]['type'] in (TYPE_MAGIC, TYPE_EQUIP, TYPE_TRAP)]
    out = []
    for d, D in enumerate(db):
        r = ramp(d, difficulty)
        n_mon = sum(1 for c, w in D['deck'] if is_monster(cards, c))
        # a window on the ladder that slides up with the campaign: the first
        # duelist draws from the weakest 65 %, the last from the strongest 25 %
        lo = int(len(ladder) * 0.75 * r)
        hi = max(lo + n_mon + 5, int(len(ladder) * (0.65 + 0.35 * r)))
        if hi > len(ladder):                    # a big pool near the top: slide the window down
            hi = len(ladder); lo = max(0, hi - n_mon - 5)
        pool = ladder[lo:hi]
        used = set(); new = []
        for c, w in D['deck']:
            src = pool if is_monster(cards, c) else spells
            m = rng.choice(src)
            while m in used:
                m = rng.choice(src)
            used.add(m); new.append(m)
        ws = random_weights(rng, len(new))
        if r > 0:
            # lean the weights on strength: the heaviest weights go to the
            # strongest monsters with probability r, else stay where they fell
            mons = [c for c in new if is_monster(cards, c)]
            mon_w = sorted((ws[new.index(c)] for c in mons), reverse=True)
            by_atk = sorted(mons, key=lambda c: -new_atk[c])
            for c, w in zip(by_atk, mon_w):
                if rng.random() < r:
                    ws[new.index(c)] = w
            # the swap above can leave duplicates of a weight and lose the
            # 2048 total; renormalise the way the game's own importer does
            tot = sum(ws)
            ws = [max(1, w * TOTAL // tot) for w in ws]
            ws[ws.index(max(ws))] += TOTAL - sum(ws)
        out.append(dict(zip(new, ws)))
    return out


def roll_ai(rng, db, ai, difficulty):
    """Opponent id = drop database index + 1. Hand size, combo width and
    fusion depth rise with the campaign and are rolled around that; the
    fusion deck gate is rolled; the rest stays stock."""
    out = []
    for d in range(NDUEL):
        r = ramp(d, difficulty)
        b = list(ai[d + 1])
        b[0] = max(5, min(20, 5 + int(round(15 * r)) + rng.randrange(-2, 3)))
        b[2] = rng.randrange(3, 16)
        b[3] = max(1, min(3, 1 + int(round(2 * r)) + rng.choice((-1, 0, 0, 1))))
        b[4] = max(1, min(3, 1 + int(round(2 * r)) + rng.choice((-1, 0, 0, 1))))
        out.append(b)
    return out


def level_for(atk, dfn, rng):
    best = max(atk, dfn)
    lv = 1 if best <= 500 else 1 + (best - 200) // 350
    lv += rng.choice((-1, 0, 0, 1))
    return max(1, min(12, lv))


def price_roll(rng):
    """10 .. 999999, most cards cheap, a few absurd: a log-uniform draw
    rounded to two significant digits."""
    v = 10 ** rng.uniform(1, 6)
    mag = 10 ** max(0, int(len(str(int(v))) - 2))
    return max(10, min(999999, int(v / mag) * mag))


# (rule, what it says on the card)
CASTS = [('heal 500', 'gain 500 LP'), ('heal 1000', 'gain 1000 LP'),
         ('damage 500', 'the foe loses 500 LP'), ('damage 1000', 'the foe loses 1000 LP'),
         ('raigeki', 'destroy every foe monster'), ('dark_hole', 'destroy every monster'),
         ('stop_defense', 'foe monsters go to attack mode'),
         ('flip', 'foe monsters flip face down'), ('weaken 500', 'foe monsters lose 500 ATK'),
         ('swords', 'Swords of Revealing Light'), ('harpie', 'destroy foe magic and traps'),
         ('destroy_strongest', 'destroy the strongest foe monster'),
         ('destroy_atk 1500', 'destroy foe monsters of 1500+ ATK'),
         ('gamble', 'Time Wizard coin flip'),
         ('50%: raigeki; else: destroy_own_lp',
          'coin flip: heads destroys every foe monster, tails your own and half their ATK in LP'),
         ('50%: heal 1000; else: damage 1000', 'coin flip: heads you gain 1000 LP, tails the foe loses 1000 LP')]
TRIGGERS = [('on_summon', 'When summoned'), ('on_flip', 'When flipped up'),
            ('on_attack', 'When it attacks'), ('on_death', 'When destroyed')]
BATTLES = [('indestructible', 'Cannot be destroyed in battle'), ('mutual', 'Both monsters die when it battles'),
           ('slayer', 'Destroys any monster it battles')]
IMMUNES = [('traps', 'Immune to traps'), ('magic', 'Immune to magic'), ('traps, magic', 'Immune to traps and magic')]
MAGIC_FX = [('heal', 1000, 'Gain 1000 LP.'), ('heal', 2000, 'Gain 2000 LP.'),
            ('damage', 1000, 'The foe loses 1000 LP.'), ('damage', 2000, 'The foe loses 2000 LP.'),
            ('raigeki', None, 'Destroy every foe monster.'), ('dark_hole', None, 'Destroy every monster on the field.'),
            ('stop_defense', None, 'Foe monsters go to attack mode.'), ('flip', None, 'Foe monsters flip face down.'),
            ('weaken', 500, 'Foe monsters lose 500 ATK and DEF.'), ('swords', None, 'Swords of Revealing Light.'),
            ('harpie', None, 'Destroy foe magic and trap cards.'), ('destroy_strongest', None, 'Destroy the strongest foe monster.'),
            ('destroy_atk', 1500, 'Destroy foe monsters of 1500 ATK or more.'),
            ('lose_lp', 1000, 'You lose 1000 LP.'), ('coin_lp', None, 'Coin flip: tails, lose half your LP.')]


def card_text(sentences, width=20, lines=6):
    """The card shows six lines of twenty characters; | is a line break."""
    words = ' '.join(sentences).split()
    out, cur = [], ''
    for w in words:
        if len(w) > width:
            w = w[:width]
        if cur and len(cur) + 1 + len(w) > width:
            out.append(cur); cur = w
        else:
            cur = (cur + ' ' + w).strip()
    if cur:
        out.append(cur)
    return '|'.join(out[:lines])


def roll_cards(rng, cards, n_fx=160, n_battle=60, n_magic=40):
    """Per card: the card.ini lines. Also returns the rolled ATK per monster,
    which the deck ramp reads."""
    out = {}
    new_atk = {}
    monsters = [c for c in range(1, NCARDS + 1) if is_monster(cards, c)]
    magics = [c for c in range(1, NCARDS + 1) if cards[c]['type'] == TYPE_MAGIC]
    fx_cards = set(rng.sample(monsters, min(n_fx, len(monsters))))
    battle_cards = set(rng.sample(monsters, min(n_battle, len(monsters))))
    magic_cards = set(rng.sample(magics, min(n_magic, len(magics))))   # the game has 36 Magic cards
    passwords = rng.sample(range(10000000, 100000000), NCARDS)   # eight digits, no two alike
    for c in range(1, NCARDS + 1):
        lines = ['color = %s' % rng.choice(COLORS), 'name_color = %s' % rng.choice(NAME_COLORS),
                 'price = %d' % price_roll(rng), 'password = %08d' % passwords[c - 1]]
        s = cards[c]
        if is_monster(cards, c):
            # anything goes: Blue-eyes may come out at 200, Kuriboh at 4000.
            # Only the cards a new game can deal stay under 3000.
            cap = STARTER_CAP if c in STARTER_POOL else ATK_CAP
            atk = rng.randrange(0, cap // 10 + 1) * 10
            dfn = rng.randrange(0, cap // 10 + 1) * 10
            s1, s2 = rng.sample(STARS, 2)
            new_atk[c] = atk
            lines += ['attack = %d' % atk, 'defense = %d' % dfn, 'level = %d' % level_for(atk, dfn, rng),
                      'type = %s' % rng.choice(TYPES), 'attribute = %s' % rng.choice(ATTRS),
                      'star1 = %s' % s1, 'star2 = %s' % s2]
            said = []
            if c in battle_cards:
                rule, text = rng.choice(BATTLES)
                lines.append('battle = %s' % rule); said.append(text + '.')
                if rng.random() < 0.5:
                    rule, text = rng.choice(IMMUNES)
                    lines.append('immune = %s' % rule); said.append(text + '.')
            if c in fx_cards:
                trig, when = rng.choice(TRIGGERS)
                rule, text = rng.choice(CASTS)
                lines.append('%s = %s' % (trig, rule)); said.append('%s: %s.' % (when, text))
            if said:
                lines.append('description = %s' % card_text(said))
        elif c in magic_cards:
            fx, amount, text = rng.choice(MAGIC_FX)
            lines.append('effect = %s' % fx)
            if amount is not None:
                lines.append('amount = %d' % amount)
            lines.append('description = %s' % card_text([text]))
        out[c] = lines
    return out, fx_cards, battle_cards, magic_cards, new_atk


def roll_equips(rng, equips, cards):
    """Per equip card: the monsters it fits, the same number as stock but
    drawn at random; an equip that fits every monster in stock keeps that."""
    monsters = [c for c in range(1, NCARDS + 1) if is_monster(cards, c)]
    out = {}
    for e, mons in equips.items():
        if len(mons) >= len(monsters):
            out[e] = 'all'
        else:
            out[e] = sorted(rng.sample(monsters, len(mons)))
    return out


def roll_fusions(rng, fusions, cards):
    """Shuffle the results among the monster + monster recipes."""
    idx = [i for i, (a, b, r) in enumerate(fusions)
           if is_monster(cards, a) and is_monster(cards, b) and is_monster(cards, r)]
    results = [fusions[i][2] for i in idx]
    rng.shuffle(results)
    out = list(fusions)
    for i, r in zip(idx, results):
        a, b, _ = out[i]
        out[i] = (a, b, r)
    changed = sum(1 for i in idx if out[i][2] != fusions[i][2])
    return out, changed


# --- the files --------------------------------------------------------------

def text_drops(db, drops):
    s = ['; Yu-Gi-Oh! Forbidden Memories - Recompiled : drop table edits',
         '; Written by tools/randomizer.py. <card id> = <POW>, <BCD>, <TEC>, out of 2048.', '']
    for D, vec in zip(db, drops):
        s.append('[%s]' % D['name'])
        for c in sorted(vec):
            s.append('%-3d = %4d, %4d, %4d' % (c, *vec[c]))
        s.append('')
    return '\n'.join(s) + '\n'


def text_cpu(db, decks, ai):
    s = ['; Yu-Gi-Oh! Forbidden Memories - Recompiled : CPU duelists',
         '; Written by tools/randomizer.py. ai = the nine profile bytes; <card id> = <weight> is the deck pool.', '']
    for D, deck, b in zip(db, decks, ai):
        s.append('[%s]' % D['name'])
        s.append('ai = ' + ', '.join(str(x) for x in b))
        # the importer lays the listed cards over the STOCK pool, so a stock
        # card that left the pool has to be written as 0 to really leave
        full = dict(deck)
        for c, w in D['deck']:
            full.setdefault(c, 0)
        for c in sorted(full):
            s.append('%-3d = %4d' % (c, full[c]))
        s.append('')
    return '\n'.join(s) + '\n'


def text_fusions(fusions):
    s = ['# Yu-Gi-Oh! Forbidden Memories - fusion table, randomized by tools/randomizer.py',
         '# card1\tcard2\tresult', '']
    s += ['%d\t%d\t%d' % f for f in fusions]
    return '\n'.join(s) + '\n'


def build(seed, out_path, difficulty=1.0, stock_cache=None, drops_mode='full'):
    rng = random.Random(seed)
    db = read_drop_db()
    tmpdir = os.path.join(ROOT, 'build', 'randomizer')
    os.makedirs(tmpdir, exist_ok=True)
    try:
        cards, ai, fusions, equips = read_stock_from_game(os.path.join(tmpdir, 'stock-export.json'))
        json.dump(dict(cards=cards, ai=ai, fusions=fusions, equips=equips), open(os.path.join(tmpdir, 'stock.json'), 'w'))
    except Exception as e:
        cache = stock_cache or os.path.join(tmpdir, 'stock.json')
        if not os.path.exists(cache):
            raise SystemExit('the game is not up (%s) and there is no cached stock data at %s' % (e, cache))
        j = json.load(open(cache))
        cards = {int(k): v for k, v in j['cards'].items()}
        ai = {int(k): v for k, v in j['ai'].items()}
        fusions = [tuple(f) for f in j['fusions']]
        if 'equips' not in j:
            raise SystemExit('the cached stock data predates the equip export: run once with the game up')
        equips = {int(k): v for k, v in j['equips'].items()}
        print('game not reachable (%s): using cached stock data from %s' % (e, cache))

    drops = roll_drops(rng, db, cards) if drops_mode == 'full' else roll_drops_compat(rng, db, cards)
    card_ini, fx_cards, battle_cards, magic_cards, new_atk = roll_cards(rng, cards)
    decks = roll_decks(rng, db, cards, new_atk, difficulty)
    ai_new = roll_ai(rng, db, ai, difficulty)
    fus_new, fus_changed = roll_fusions(rng, fusions, cards)
    eq_new = roll_equips(rng, equips, cards)
    for e, mons in eq_new.items():
        card_ini[e].append('equips = %s' % (mons if mons == 'all' else ', '.join(str(m) for m in mons)))

    # sanity: every band balances the way the runtime will balance it, and
    # every monster is dropped somewhere
    dropped = set()
    for D, vec in zip(db, drops):
        assert len(vec) <= 722 if drops_mode == 'full' else len(vec) <= 128
        for t in range(3):
            if drops_mode == 'full':
                assert sum(v[t] for v in vec.values()) == TOTAL
            else:
                stock = [0] * NCARDS
                for c, w in D['tiers'][t]:
                    stock[c - 1] = w
                pins = [(c, v[t]) for c, v in vec.items()]
                res = old_rescale(stock, [c for c, w in pins], [w for c, w in pins])
                assert res is not None, ('band would be refused', D['name'], t)
                dropped |= {i + 1 for i in range(NCARDS) if res[i]}
        dropped |= {c for c, v in vec.items() if any(v)}
    assert all(c in dropped for c in range(1, NCARDS + 1)), 'a card is dropped by nobody'
    for deck in decks:
        assert sum(deck.values()) == TOTAL and min(deck.values()) >= 1
    for c, lines in card_ini.items():
        for ln in lines:
            if ln.startswith('description = '):
                parts = ln[len('description = '):].split('|')
                assert len(parts) <= 6 and all(len(p) <= 20 for p in parts), (c, ln)

    stamp = time.strftime('%Y-%m-%d %H:%M')
    z = zipfile.ZipFile(out_path, 'w', zipfile.ZIP_DEFLATED)
    ids = sorted(card_ini)
    z.writestr('cards-manifest.ini',
               '; Yu-Gi-Oh! Forbidden Memories Recompiled -- edited cards\n'
               'format = YGOFM-EDITED-CARDS\nversion = 1\ngame = SLUS-01411\ncards = %s\ndrop_table_edits = 1\n'
               % ', '.join(str(i) for i in ids))
    for c in ids:
        z.writestr('cards/%d/card.ini' % c, '\n'.join(card_ini[c]) + '\n')
    z.writestr('drop_table_edits.ini', text_drops(db, drops))
    z.writestr('cpu-duelists.ini', text_cpu(db, decks, ai_new))
    z.writestr('fusion-edits.txt', text_fusions(fus_new))
    z.writestr('mod_settings.ini',
               '; Yu-Gi-Oh! Forbidden Memories - Recompiled : MODS and CHEATS rows\n'
               'card_drops = 15\ndrop_missing_cards = 1\nfill_library = 1\n')
    z.writestr('manifest.ini',
               '; Yu-Gi-Oh! Forbidden Memories Recompiled -- MOD package\n'
               'format = YGOFM-MOD-PACKAGE\nversion = 1\ngame = SLUS-01411\ncreated = %s\n'
               'title = Randomizer (seed %d, difficulty %g)\n'
               'cards = %d\ndrop_tables = 1\ncpu = 1\nportraits = 0\nfusion = 1\n'
               'dialogue = 0\ndrop_missing_cards = 0\ncard_shop = 0\nsettings = 1\n'
               % (stamp, seed, difficulty, len(ids)))
    z.close()
    print('wrote %s (seed %d, difficulty %g)' % (out_path, seed, difficulty))
    print('  %d cards (%d monsters with cast effects, %d with battle rules, %d magic cards re-done)'
          % (len(ids), len(fx_cards), len(battle_cards), len(magic_cards)))
    print('  %d duelists: drops, decks and AI; %d of %d fusion results moved; %d equip cards re-paired'
          % (NDUEL, fus_changed, len(fusions), sum(1 for v in eq_new.values() if v != 'all')))
    for d in (0, 9, 19, 29, NDUEL - 1):
        mons = [(c, w) for c, w in decks[d].items() if c in new_atk]
        wm = sum(new_atk[c] * w for c, w in mons) // max(1, sum(w for c, w in mons))
        print('  %-16s pool ATK %4d (weighted), best %4d, hand %2d, combo %d, depth %d'
              % (db[d]['name'], wm, max(new_atk[c] for c, w in mons), ai_new[d][0], ai_new[d][3], ai_new[d][4]))


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--seed', type=int, default=None, help='the seed; a random one when left out')
    ap.add_argument('--difficulty', type=float, default=1.0, help='how steeply the CPUs ramp (0 flat, 1 default, 2 harsh)')
    ap.add_argument('--drops', choices=('compat', 'full'), default='full',
                    help='full (default) rewrites the bands and needs 0.5.7 or later; compat pins over stock and loads on 0.5.5 and 0.5.6')
    ap.add_argument('--out', default=None, help='output file (default randomizer-<seed>.ygomods in the working directory)')
    ap.add_argument('--stock', default=None, help='cached stock.json to use when the game is not running')
    a = ap.parse_args()
    seed = a.seed if a.seed is not None else random.randrange(1, 1 << 31)
    out = a.out or 'randomizer-%d.ygomods' % seed
    build(seed, out, a.difficulty, a.stock, a.drops)
