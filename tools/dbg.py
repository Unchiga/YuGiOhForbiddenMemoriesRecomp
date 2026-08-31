"""Thin wrapper over psxrecomp's debug_client for the card-library probes.

Resolves debug_client relative to this file so the tools work from any cwd
and from any session -- an earlier copy of this lived in a per-session
scratchpad and broke the moment that session ended.
"""
import os, sys

_REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(_REPO, 'psxrecomp', 'tools'))
import debug_client as dc

H, P = '127.0.0.1', 4370

def q(c):
    try:
        return dc.query(H, P, c)
    except Exception as e:
        return {'err': str(e)}

def rd(addr, n=4):
    r = q({'cmd': 'read_ram', 'addr': '%08X' % addr, 'len': n})
    return r.get('data') or r.get('hex') or ''

def u32(addr):
    h = rd(addr, 4)
    if len(h) < 8:
        return None
    return int.from_bytes(bytes.fromhex(h[:8]), 'little')

def frame():
    r = q({'cmd': 'frame'})
    return r.get('frame', r.get('value'))
