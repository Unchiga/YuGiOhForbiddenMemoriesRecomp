# Card title font research (parked 2026-09-03)

The 722 baked card titles (96x14 4bpp) live in each card's 7-sector art record
at LBA 10817 + 7*id (decomp findings F125/F127): +0 art 102x96 8bpp, +9792 CLUT,
+10304 title, +10976 a 16x88 strip. Extract with tools/disc_image.py
(`open_disc(cue).read(lba*2048, 7*2048)`); the extracted art is Konami's and
must not be committed.

Result of fitting the titles: Times New Roman Bold, 12px em, baseline row 11,
pen x=3, coverage -> CLUT level thresholds in title_model.json, names wider
than ~90px squeezed to columns 3..93. Konami's rasteriser gives one dark + one
light column per stem, which neither FreeType nor stb_truetype reproduces
(ink-pixel agreement ~36% within one level). score_ft.py / lut_fit.py are the
scoring scripts (expect titles.json / card_names.json beside them; regenerate
from the disc). src/psx_card_skins.c carries the stb_truetype renderer built
from this model; it needs the player's own card_skins/timesbd.ttf.
