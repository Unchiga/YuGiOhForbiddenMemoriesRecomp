import json, io, os, urllib.parse, urllib.request
SP = os.path.dirname(os.path.abspath(__file__))
PRINT = "|?Card number|?English name (linked)|?Card type|?Type|?Level|?ATK|?DEF|?Guardian Star"
def ask(cond, offset):
    q = cond + PRINT + "|limit=500|offset=%d|sort=Card number|order=asc" % offset
    url = "https://yugipedia.com/api.php?action=ask&format=json&query=" + urllib.parse.quote(q, safe='')
    req = urllib.request.Request(url, headers={'User-Agent':'Mozilla/5.0 (card-list diff)'})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.loads(r.read().decode('utf-8'))
rows = {}
for off in (0, 500):
    d = ask("[[Release::Yu-Gi-Oh! Forbidden Memories]]", off)
    res = d.get('query', {}).get('results', {})
    print('offset %d -> %d rows' % (off, len(res)))
    rows.update(res)
    if len(res) < 500: break
io.open(os.path.join(SP,'fmr_cards.json'),'w',encoding='utf-8').write(json.dumps(rows, ensure_ascii=False))
print('total', len(rows))
