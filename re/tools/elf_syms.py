import sys
from elftools.elf.elffile import ELFFile
f = ELFFile(open(sys.argv[1], 'rb'))
for seg in f.iter_segments():
    if seg.header.p_type == 'PT_DYNAMIC':
        print('NEEDED:', [t.needed for t in seg.iter_tags() if t.entry.d_tag == 'DT_NEEDED'])
ds = f.get_section_by_name('.dynsym')
und = sorted(s.name for s in ds.iter_symbols() if s['st_shndx'] == 'SHN_UNDEF' and s.name.startswith('SDL_'))
print(len(und), 'undefined SDL syms'); print(' '.join(und))
