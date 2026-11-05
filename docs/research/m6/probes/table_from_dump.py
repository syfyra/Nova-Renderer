"""Turns a render_prims_dump.txt (probe_karma_aovs.py) into a Markdown table of
the RenderVar prims: prim, husk:name, sourceType, sourceName, dataType,
husk:format, other driver:parameters:aov:* attributes.

    python table_from_dump.py render_prims_dump.txt
"""
import re
import sys

COMMON = {'dataType', 'sourceName', 'sourceType', 'driver:parameters:aov:husk:name',
          'driver:parameters:aov:husk:format'}
SHORT = {
    'driver:parameters:aov:husk:clearValue': 'clear',
    'driver:parameters:aov:husk:multiSampled': 'ms',
    'driver:parameters:aov:karma:filter': 'filter',
    'driver:parameters:aov:husk:channel_prefix': 'prefix',
    'driver:parameters:aov:karma:cryptomatte': 'crypto',
    'driver:parameters:aov:karma:cryptomatterank': 'rank',
    'driver:parameters:aov:karma:cryptomattesidecar': 'sidecar',
}


def parse(path):
    prims = []
    cur = None
    for line in open(path, encoding='utf-8'):
        m = re.match(r'^(/\S+)\s+\((\w*)\)\s+schemas=(.*)$', line.strip())
        if m:
            cur = {'path': m.group(1), 'type': m.group(2), 'schemas': m.group(3), 'attrs': {}}
            prims.append(cur)
            continue
        m = re.match(r'^\s+(?:rel\s+)?(\S+(?:\[\])?)\s+(\S+)\s+=\s+(.*)$', line)
        if m and cur is not None:
            cur['attrs'][m.group(2)] = m.group(3).strip()
    return prims


def clean(v):
    v = v.strip()
    if len(v) >= 2 and v[0] == v[-1] and v[0] in '\'"':
        v = v[1:-1]
    return v.replace('|', '\\|')


def main():
    rows = []
    for p in parse(sys.argv[1]):
        if p['type'] != 'RenderVar':
            continue
        a = p['attrs']
        extra = []
        for k, v in sorted(a.items()):
            if k in COMMON:
                continue
            extra.append('%s=%s' % (SHORT.get(k, k.replace('driver:parameters:aov:', '')), clean(v)))
        rows.append('| %s | %s | %s | `%s` | %s | %s | %s |' % (
            p['path'].rsplit('/', 1)[-1], clean(a.get('driver:parameters:aov:husk:name', '')),
            clean(a.get('sourceType', '')), clean(a.get('sourceName', '')), clean(a.get('dataType', '')),
            clean(a.get('driver:parameters:aov:husk:format', '')), ', '.join(extra)))
    print('| prim | husk:name | sourceType | sourceName | dataType | husk:format | other aov attributes |')
    print('|---|---|---|---|---|---|---|')
    print('\n'.join(rows))


main()
