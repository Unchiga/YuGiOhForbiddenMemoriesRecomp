import json,os,collections
import numpy as np
from PIL import Image, ImageDraw, ImageFont
titles={int(k):v for k,v in json.load(open('titles.json')).items()}
names={int(k):v for k,v in json.load(open('card_names.json')).items()}; names[5]='Ryu-kishin'
def extent(rows):
    cols=[x for x in range(96) if any(rows[y][x] for y in range(14))]; return (min(cols),max(cols)+1) if cols else (0,0)
U=[i for i in sorted(titles) if extent(titles[i])[1]-extent(titles[i])[0] < 80]
F='/home/codyj/Documents/My Games/Yu-Gi-Oh Forbidden Memories Recompiled/card_skins/timesbd.ttf'
font=ImageFont.truetype(F, 12, layout_engine=ImageFont.Layout.BASIC)
def render(text,base_y=11,x0=3):
    im=Image.new('L',(96,14),0); d=ImageDraw.Draw(im); d.text((x0,base_y), text, font=font, fill=255, anchor='ls'); return np.asarray(im,dtype=int)
R={id:render(names[id]) for id in U}
# histogram-match LUT on first half, test on second half
half=len(U)//2
co=np.zeros((256,8),dtype=int)
for id in U[:half]:
    o=np.array(titles[id]); r=R[id]
    for rv,ov in zip(r.ravel(),o.ravel()): co[rv,ov]+=1
lut=co.argmax(1)
def stats(ids):
    ag=[];ex=0
    for id in ids:
        o=np.array(titles[id]); q=lut[R[id]]; ag.append((q==o).mean()); ex+=int((q==o).all())
    return np.mean(ag),ex,len(ids)
print('env',os.environ.get('FREETYPE_PROPERTIES'),'LUT nonzero breakpoints:',[i for i in range(1,256) if lut[i]!=lut[i-1]][:12], 'lut max',lut.max())
print('train agree %.3f exact %d/%d'%stats(U[:half])); print('test  agree %.3f exact %d/%d'%stats(U[half:]))
# ink-only agreement (pixels where either is nonzero)
ag=[]
for id in U[half:]:
    o=np.array(titles[id]); q=lut[R[id]]; m=(o>0)|(q>0); ag.append((q[m]==o[m]).mean())
print('ink-pixel agreement %.3f'%np.mean(ag))
# sheet
pal=[(255,255,255)]+[tuple(((c&31)<<3,((c>>5)&31)<<3,((c>>10)&31)<<3)) for c in (0xDEF7,0xCE73,0xC210,0xB5AD,0xAD6B,0xA108,0x9084)]
def img(arr):
    im=Image.new('RGB',(96,14),(255,255,255))
    for y in range(14):
        for x in range(96):
            v=int(arr[y][x])
            if v: im.putpixel((x,y),pal[v])
    return im
rows=[]
for id in (4,2,100,5,300,650):
    rows.append(('orig %d'%id,img(np.array(titles[id])))); rows.append(('render',img(lut[R[id]])))
W=96*3; sheet=Image.new('RGB',(W+120,42*len(rows)),(255,255,255)); d=ImageDraw.Draw(sheet)
for i,(lab,im) in enumerate(rows):
    sheet.paste(im.resize((W,42),Image.NEAREST),(0,i*42)); d.text((W+6,i*42+14),lab,fill=(0,0,0))
sheet.save('font_compare4_%s.png'%(os.environ.get('FREETYPE_PROPERTIES','default').split('=')[-1])); print('sheet saved')
np.save('lut_%s.npy'%(os.environ.get('FREETYPE_PROPERTIES','default').split('=')[-1]), lut)
