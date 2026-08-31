import json, io, os, re, time, urllib.parse, urllib.request
SP=os.path.dirname(os.path.abspath(__file__))
API="https://yugipedia.com/api.php"
def get(q):
    url=API+"?action=ask&format=json&query="+urllib.parse.quote(q,safe='')
    req=urllib.request.Request(url,headers={'User-Agent':'Mozilla/5.0 (fm card add)'})
    return json.loads(urllib.request.urlopen(req,timeout=90).read().decode('utf-8'))
missing=json.load(io.open(os.path.join(SP,'missing.json'),encoding='utf-8'))
mons=[m for m in missing if m['type']=='Monster Card']
print('missing monsters:', len(mons))
out={}
for i in range(0,len(mons),15):
    chunk=mons[i:i+15]
    cond="".join("[[%s]]" % c['key'] for c in chunk)
    q="[[Card type::Monster Card]]<q>%s</q>|?English name|?Type|?ATK#|?DEF#|?Level|?Attribute|limit=50" % ("||".join(c['key'] for c in chunk))
    # SMW OR syntax: [[A||B||C]] on page name is not valid; query each page directly instead
    for c in chunk:
        d=get("[[%s]]|?English name|?Type|?ATK#|?DEF#|?Level|?Attribute|?Card type" % c['key'].replace('[','').replace(']',''))
        for k,v in d.get('query',{}).get('results',{}).items():
            p=v['printouts']
            out[c['name']]={
                'page':k,
                'type':((p.get('Type') or [{}])[0] or {}).get('fulltext') if p.get('Type') else None,
                'atk':(p.get('ATK')or[None])[0], 'def':(p.get('DEF')or[None])[0],
                'lvl':(p.get('Level')or[None])[0],
                'attr':((p.get('Attribute') or [{}])[0] or {}).get('fulltext') if p.get('Attribute') else None}
    print('  %d/%d' % (len(out),len(mons)), flush=True)
    time.sleep(0.2)
io.open(os.path.join(SP,'new_monsters.json'),'w',encoding='utf-8').write(json.dumps(out,ensure_ascii=False,indent=1))
print('fetched', len(out))
bad=[n for n,v in out.items() if v['atk'] is None or not v['type']]
print('incomplete:', bad)
