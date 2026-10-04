import pathlib, sys

root = pathlib.Path(sys.argv[1])
out = pathlib.Path(sys.argv[2])
files = [root / 'lua_runtime.js'] + sorted((root / 'presets').glob('*.lua')) + sorted((root / 'maps').glob('level*/layout.json'))
with out.open('w', encoding='ascii') as f:
    f.write('#pragma once\n#include <cstddef>\n#include <string_view>\nnamespace doomsday_embedded {\nstruct Resource { const char* path; const unsigned char* data; std::size_t size; };\n')
    names=[]
    for i,p in enumerate(files):
        name = 'r%d' % i; names.append((p.relative_to(root).as_posix(),name))
        b=p.read_bytes(); f.write('static const unsigned char %s[] = {'%name)
        for j,x in enumerate(b):
            if j%20==0: f.write('\n ')
            f.write('0x%02x,'%x)
        f.write('\n};\n')
    f.write('static const Resource resources[] = {\n')
    for path,name in names: f.write('{"%s",%s,sizeof(%s)},\n'%(path,name,name))
    f.write('};\ninline std::string_view Find(std::string_view path) { for (const auto& r: resources) if (path==r.path) return {reinterpret_cast<const char*>(r.data),r.size}; return {}; }\n}\n')
