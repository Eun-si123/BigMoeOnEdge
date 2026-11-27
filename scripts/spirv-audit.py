#!/usr/bin/env python3
# The SPIR-V a Vulkan build ships, audited: version and declared OpCapability/OpExtension per module.
# Usage: scripts/spirv-audit.py <build>/third_party/llama.cpp/ggml/src/ggml-vulkan/vulkan-shaders.spv
# Needs the SPIR-V grammar (spirv-headers: /usr/include/spirv/unified1/spirv.core.grammar.json).
import json, os, struct, sys, collections
grammar = json.load(open('/usr/include/spirv/unified1/spirv.core.grammar.json'))
cap_names = {}
for k in grammar['operand_kinds']:
    if k['kind'] == 'Capability':
        for e in k['enumerants']:
            cap_names.setdefault(e['value'], e['enumerant'])
root = sys.argv[1]
versions = collections.Counter(); caps = collections.defaultdict(list); exts = collections.defaultdict(list)
for dp, _, fs in os.walk(root):
    for f in fs:
        if not f.endswith('.spv'): continue
        b = open(os.path.join(dp, f), 'rb').read()
        w = struct.unpack('<%dI' % (len(b)//4), b)
        if w[0] != 0x07230203: continue
        ver = '%d.%d' % ((w[1] >> 16) & 0xff, (w[1] >> 8) & 0xff); versions[ver] += 1
        i = 5
        while i < len(w):
            n, op = w[i] >> 16, w[i] & 0xffff
            if n == 0: break
            if op == 17: caps[cap_names.get(w[i+1], str(w[i+1]))].append(f)
            elif op == 10:
                s = b[(i+1)*4:(i+n)*4].split(b'\0')[0].decode(); exts[s].append(f)
            i += n
print('modules by SPIR-V version:', dict(versions))
print('\ncapabilities (modules using it):')
for c, l in sorted(caps.items(), key=lambda x: -len(x[1])): print('  %-40s %4d  e.g. %s' % (c, len(l), l[0]))
print('\nSPIR-V extensions:')
for e, l in sorted(exts.items(), key=lambda x: -len(x[1])): print('  %-40s %4d  e.g. %s' % (e, len(l), l[0]))
