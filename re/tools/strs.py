import re, sys
data = open(sys.argv[1], 'rb').read()
out = open(sys.argv[2], 'w', encoding='latin-1')
for m in re.finditer(rb'[\x20-\x7e]{6,}', data):
    out.write('%08x %s\n' % (m.start(), m.group().decode('latin-1')))
