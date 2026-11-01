#!/usr/bin/env python3
"""Convert a SingleStepTests 65x02 json file to the line format read by nesref --sst."""
import json, sys
d = json.load(open(sys.argv[1]))
out = open(sys.argv[2], 'w')
for t in d:
    i, f = t['initial'], t['final']
    l = ['T %s|' % t['name'].replace('|', '_'), '%d %d %d %d %d %d %d' % (i['pc'], i['s'], i['a'], i['x'], i['y'], i['p'], len(i['ram']))]
    l += ['%d %d' % (a, v) for a, v in i['ram']]
    l.append('%d %d %d %d %d %d %d' % (f['pc'], f['s'], f['a'], f['x'], f['y'], f['p'], len(f['ram'])))
    l += ['%d %d' % (a, v) for a, v in f['ram']]
    l.append('%d' % len(t['cycles']))
    l += ['%d %d %d' % (a, v, 0 if rw == 'read' else 1) for a, v, rw in t['cycles']]
    out.write(' '.join(l) + '\n')
