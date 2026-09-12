# Free Duel completion border

`src/psx_free_duel_completion_art.c` is a lossless ARGB bake of the project
owner's `portraitBorder1.png` through `portraitBorder8.png` (48x48 each). The
runtime cycles them in that order over completed CPU portraits. The bake keeps
the authored transparency and does not require a PNG decoder at runtime.

Original SHA-256 fingerprints:

```
portraitBorder1.png  6bc5e56afa05e8071bee113796ca350a3493ed203e9cdd7e67813a03e3b02e1d
portraitBorder2.png  598fb71777164a1c2d0f729e5f9f5bbd92e6fa832e69d5c4c00c0dac8d220ceb
portraitBorder3.png  f492e2d4b5356bb5afae6f2f8c0f55191f7b9bf610255e52171b03dedd973aba
portraitBorder4.png  eb0f2fb2df5a84369a0dbd5231ceda248c2d00a7373cbcbb972fb7df4128d26c
portraitBorder5.png  d16688db94f033e8544ec5a775f055c4b38f5df30d7c1722f1ac0f50cfba75bd
portraitBorder6.png  84f9a335d2e1c2a80142320570800ddca931f840eb855bc73781f00bb441fed9
portraitBorder7.png  8ba00278b04cb3a7164c32ac6df0b16ed26dadf50ee4ee75c19cb71193c95d4d
portraitBorder8.png  1b0f7411d5dbb27ced74a1d838d52c51de4ccc6cf6dfd47f10cae934b7a8e66e
```

With the originals still in their source directory, verify every decoded C
pixel and all file fingerprints with:

```sh
python3 tools/verify_portrait_borders.py /home/codyj/Documents/Art
```

To replace the animation, put the eight source PNGs in one directory and run:

```sh
python3 tools/gen_portrait_borders.py /path/to/pngs src/psx_free_duel_completion_art.c
```
