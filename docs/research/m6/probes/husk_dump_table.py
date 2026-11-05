"""Extracts the render settings JSON that `husk -V 9` prints (one
XUSD_RenderProduct/XUSD_RenderVar dump per render) from a husk log and prints
a Markdown table of what each render var becomes: AOV name, HdFormat,
multiSampled, clear value, pixel format, and the aovSettings keys.

    python -I husk_dump_table.py husk.log [--json out.json]
"""
import json
import re
import sys


def extract(log):
    text = open(log, encoding='utf-8', errors='replace').read()
    start = text.find('{\n    "camera"')
    if start < 0:
        start = re.search(r'^\[[\d:]+\] \{\s*$', text, re.M).end() - 1
    depth = 0
    for i in range(start, len(text)):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                return json.loads(text[start:i + 1].replace('\t', ' '))
    raise ValueError('no JSON block found')


def short(v):
    s = json.dumps(v)
    return s if len(s) < 60 else s[:57] + '...'


def main():
    data = extract(sys.argv[1])
    if '--json' in sys.argv:
        out = sys.argv[sys.argv.index('--json') + 1]
        data['RenderSettings'].pop('batchCommandLine', None)
        data['RenderSettings'].pop('usdFilename', None)
        with open(out, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(data, f, indent=1)
    for prod in data['RenderProducts']:
        print('product %s: %s' % (prod['settings'].get('sourcePrim'), short(prod['settings'])))
        print()
        print('| AOVName | HdFormat | multiSampled | clear | pixel format x channels | aovSettings (besides sourceName/sourceType/dataType/sourcePrim) |')
        print('|---|---|---|---|---|---|')
        for v in prod['RenderVariables']:
            s = dict(v['settings'])
            for k in ('sourceName', 'sourceType', 'dataType', 'sourcePrim'):
                s.pop(k, None)
            rest = ', '.join('%s=%s' % (k.replace('driver:parameters:aov:', ''), short(val)) for k, val in sorted(s.items()))
            print('| %s | %s | %s | %s | %s x%s | %s |' % (v['AOVName'], v['HdFormat'], v['HdMultiSampled'],
                                                         short(v['HdClearValue']), v['AOVPixelFormat'],
                                                         v['AOVChannelSize'], rest.replace('|', '\\|')))
        print()


main()
