"""Print printable ASCII (and UTF-16LE) strings of a binary, like `strings`,
optionally filtered by a case-insensitive regular expression.

    python dll_strings.py <file> [regex] [--min N]

Used to see which option names, environment variables and libraries a
Houdini plug-in refers to. The output stays in the scratch directory; the
report only describes what was found.
"""
import re
import sys


def main():
    args = [a for a in sys.argv[1:]]
    minlen = 5
    if '--min' in args:
        i = args.index('--min')
        minlen = int(args[i + 1])
        del args[i:i + 2]
    path = args[0]
    pattern = re.compile(args[1], re.I) if len(args) > 1 else None
    data = open(path, 'rb').read()
    seen = set()
    ascii_re = re.compile(rb'[\x20-\x7e]{%d,}' % minlen)
    utf16_re = re.compile(rb'(?:[\x20-\x7e]\x00){%d,}' % minlen)
    for m in ascii_re.finditer(data):
        s = m.group().decode('ascii')
        if s not in seen and (pattern is None or pattern.search(s)):
            seen.add(s)
            print(s)
    for m in utf16_re.finditer(data):
        s = m.group().decode('utf-16le')
        if s not in seen and (pattern is None or pattern.search(s)):
            seen.add(s)
            print('[u16] ' + s)


if __name__ == '__main__':
    main()
