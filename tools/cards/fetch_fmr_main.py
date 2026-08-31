import json, io, os, re, time, urllib.parse, urllib.request
SP = os.path.dirname(os.path.abspath(__file__))
API = "https://yugipedia.com/api.php"
def post(params):
    data = urllib.parse.urlencode(params).encode()
    req = urllib.request.Request(API, data=data,
        headers={'User-Agent':'Mozilla/5.0 (fm card diff)'})
    with urllib.request.urlopen(req, timeout=90) as r:
        return json.loads(r.read().decode('utf-8'))
titles = list(json.load(io.open(os.path.join(SP,'fmr_cards.json'),encoding='utf-8')).keys())
print('titles:', len(titles))
wt = {}
for i in range(0, len(titles), 40):
    chunk = titles[i:i+40]
    d = post({'action':'query','format':'json','titles':'|'.join(chunk),
              'prop':'revisions','rvprop':'content','rvslots':'main'})
    for pg in d.get('query',{}).get('pages',{}).values():
        try: wt[pg['title']] = pg['revisions'][0]['slots']['main']['*']
        except Exception: pass
    print('  %d/%d' % (len(wt), len(titles)), flush=True)
    time.sleep(0.2)
out={}
for title, text in wt.items():
    fm = title[:-6].strip() if title.endswith('(FMR)') else title
    m = re.search(r'^\s*\|\s*main\s*=\s*(.+?)\s*$', text, re.M)
    n = re.search(r'^\s*\|\s*number\s*=\s*(\d+)\s*$', text, re.M)
    out[fm] = {'canonical': m.group(1) if m else fm, 'number': int(n.group(1)) if n else None}
io.open(os.path.join(SP,'fmr_main.json'),'w',encoding='utf-8').write(json.dumps(out,ensure_ascii=False,indent=0))
ren=[(k,v['canonical']) for k,v in out.items() if v['canonical'].lower()!=k.lower()]
print('mapped:', len(out), ' renamed:', len(ren))
