import sys, collections
def fnv(b):
    h = 2166136261
    for x in b: h = ((h ^ x) * 16777619) & 0xFFFFFFFF
    return h
ref = collections.defaultdict(dict)
for l in open(sys.argv[1]):
    p = l.split()
    if p[0] in ('RAM', 'OAMRAM', 'NT', 'PAL'):
        ref[int(p[1])][p[0]] = bytes.fromhex(p[2])
mes = {}
for l in open(sys.argv[2]):
    p = l.split()
    if len(p) == 5: mes[int(p[0])] = [int(x, 16) for x in p[1:]]
def hr(r): return fnv(r[:256] + r[512:2048])
def hp(p):
    q = bytearray(p[:32])
    for i in (16, 20, 24, 28): q[i] = q[i - 16]
    return fnv(bytes(q))
H = {f: [hr(r['RAM']), 0, fnv(r['NT'][:2048]), hp(r['PAL'])] for f, r in ref.items()}
for off in (0,):
    ok = [0, 0, 0]
    for f in ref:
        m = mes.get(f + off)
        if not m: continue
        for i, (a, b) in enumerate(((H[f][0], m[0]), (H[f][2], m[2]), (H[f][3], m[3]))): ok[i] += a == b
        if H[f][0] != m[0] and 'first' not in globals(): globals()['first'] = f
    print('frames equal: RAM %d NT %d PAL %d of %d; first RAM mismatch frame: %s' % (ok[0], ok[1], ok[2], len(ref), globals().get('first')))
