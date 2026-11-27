#!/usr/bin/env python3
# Which SPIR-V modules hold an 8-bit value as more than a scalar loaded and converted.
#
# Qualcomm's Adreno 730 compiler (Vulkan 1.1 driver; docs/s22-compatibility.md) refused 957 of the
# 1966 pipelines ggml-vulkan registers. Over every module whose outcome the phone reported, the
# refused ones are exactly those with one of these, and no accepted one has any:
#   vec8  an 8-bit vector (u8vec4/i8vec4: unpack8, OpBitcast to v4u8, shuffles of them)
#   arr8  an array of 8-bit values (int8_t lookup tables such as kvalues_iq4nl)
#   phi8  an 8-bit value merged across branches (OpPhi of u8/i8)
# Scalar 8-bit loads from a storage buffer and their conversions compile.
#
# Usage: scripts/spirv-8bit-audit.py <dir with .spv> [name-substring ...]
# Needs the SPIR-V grammar (spirv-headers: /usr/include/spirv/unified1/spirv.core.grammar.json).
import json, os, re, struct, sys

GRAMMAR = '/usr/include/spirv/unified1/spirv.core.grammar.json'
g = json.load(open(GRAMMAR))
INS = {i['opcode']: i for i in g['instructions']}
STORAGE = {0: 'UniformConstant', 2: 'Uniform', 4: 'Workgroup', 6: 'Private', 7: 'Function', 9: 'PushConstant',
           12: 'StorageBuffer'}


def features(path):
    b = open(path, 'rb').read()
    w = struct.unpack('<%dI' % (len(b) // 4), b)
    if w[0] != 0x07230203:
        return None
    names, out, i = {}, set(), 5
    while i < len(w):
        n, op = w[i] >> 16, w[i] & 0xffff
        if n == 0:
            break
        ins = INS.get(op)
        name = ins['opname'] if ins else str(op)
        a = w[i + 1:i + n]
        if name == 'OpTypeInt':
            names[a[0]] = '%s%d' % ('i' if a[2] else 'u', a[1])
        elif name == 'OpTypeVector':
            names[a[0]] = 'v%d%s' % (a[2], names.get(a[1], '?'))
        elif name in ('OpTypeArray', 'OpTypeRuntimeArray'):
            names[a[0]] = 'arr<%s>' % names.get(a[1], '?')
        elif name == 'OpTypePointer':
            names[a[0]] = 'ptr%s<%s>' % (STORAGE.get(a[1], a[1]), names.get(a[2], '?'))
        elif ins and ins.get('operands') and ins['operands'][0]['kind'] == 'IdResultType':
            t = names.get(a[0], '?')
            if re.search(r'v\d[ui]8$', t):
                out.add('vec8')
            if name == 'OpPhi' and re.fullmatch(r'[ui]8', t):
                out.add('phi8')
            if name == 'OpVariable' and re.search(r'arr<[ui]8>', t) and 'StorageBuffer' not in t:
                out.add('arr8')
            if name == 'OpConstantComposite' and re.fullmatch(r'arr<[ui]8>', t):
                out.add('arr8')
        i += n
    return out


def main():
    root, subs = sys.argv[1], sys.argv[2:]
    for f in sorted(os.listdir(root)):
        if not f.endswith('.spv') or (subs and not any(s in f for s in subs)):
            continue
        feats = features(os.path.join(root, f))
        if feats is not None:
            print('%-60s %s' % (f[:-4], ' '.join(sorted(feats)) or '-'))


if __name__ == '__main__':
    main()
