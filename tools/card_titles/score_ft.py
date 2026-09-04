import json,sys,os
import numpy as np
from PIL import Image, ImageDraw, ImageFont
titles={int(k):v for k,v in json.load(open('titles.json')).items()}
names={int(k):v for k,v in json.load(open('card_names.json')).items()}; names[5]='Ryu-kishin'
def extent(rows):
    cols=[x for x in range(96) if any(rows[y][x] for y in range(14))]; return (min(cols),max(cols)+1) if cols else (0,0)
U=[i for i in sorted(titles) if extent(titles[i])[1]-extent(titles[i])[0] < 80][:120]
F='/home/codyj/Documents/My Games/Yu-Gi-Oh Forbidden Memories Recompiled/card_skins/timesbd.ttf'
def render(text,px=12,base_y=11,x0=3):
    im=Image.new('L',(96,14),0); d=ImageDraw.Draw(im); f=ImageFont.truetype(F, px, layout_engine=ImageFont.Layout.BASIC)
    d.text((x0,base_y), text, font=f, fill=255, anchor='ls'); return np.asarray(im,dtype=float)/255.0
maps={'round7':lambda r:np.rint(r*7),'ceil7':lambda r:np.ceil(r*7-1e-9),'floor8':lambda r:np.floor(r*8).clip(0,7),
      'gamma1.5':lambda r:np.rint((r**1.5)*7),'gamma0.7':lambda r:np.rint((r**0.7)*7)}
best=None
for mname,fn in maps.items():
    for base_y in (10,11):
        for x0 in (2,3):
            agree=[];exact=0
            for id in U:
                o=np.array(titles[id]); q=np.clip(fn(render(names[id],12,base_y,x0)),0,7).astype(int)
                agree.append((q==o).mean()); exact+=int((q==o).all())
            r=(np.mean(agree),exact,mname,base_y,x0)
            if best is None or r>best: best=r
            print('%s base %d x0 %d: agree %.3f exact %d/%d'%(mname,base_y,x0,np.mean(agree),exact,len(U)))
print('BEST',best, 'env', os.environ.get('FREETYPE_PROPERTIES'))
_,_,mname,base_y,x0=best
o=np.array(titles[4]); q=np.clip(maps[mname](render(names[4],12,base_y,x0)),0,7).astype(int)
for y in range(1,13): print(''.join('.' if v==0 else str(v) for v in o[y][:44]), '  ', ''.join('.' if v==0 else str(v) for v in q[y][:44]))
