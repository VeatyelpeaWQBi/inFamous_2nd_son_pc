# SPDX-License-Identifier: GPL-2.0-or-later
"""Read-only GDS footprint proof on retained SPIR-V disassemblies; no game/tool launch."""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import re


class Unsupported(ValueError):
    pass


def inspect(path):
    instructions, defs, uses = [], {}, defaultdict(list)
    for number, raw in enumerate(path.read_text(encoding='utf-8-sig').splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith(';'):
            continue
        match = re.fullmatch(r'(?:(%\S+)\s*=\s*)?(Op\w+)\s*(.*)', line)
        if not match:
            raise ValueError(f'{path}:{number}: unrecognized disassembly')
        result, op, rest = match.groups()
        args = rest.split()
        ins = (result, op, args, number)
        instructions.append(ins)
        if result:
            defs[result] = ins
        for arg in args:
            if arg.startswith('%'):
                uses[arg].append(ins)
    names = {args[0]: ' '.join(args[1:]).strip('"')
             for _, op, args, _ in instructions if op == 'OpName'}
    roots = [ident for ident, name in names.items() if name == 'gds_buffer']
    flat_roots = {ident for ident, name in names.items() if name == 'srt_flatbuf'}
    decorations, member_decorations = defaultdict(dict), defaultdict(dict)
    for _, op, args, _ in instructions:
        if op == 'OpDecorate' and len(args) >= 3:
            decorations[args[0]][args[1]] = args[2:]
        if op == 'OpMemberDecorate' and len(args) >= 4:
            member_decorations[args[0]][tuple(args[1:3])] = args[3:]

    def definition(ident, op=None):
        ins = defs.get(ident)
        if not ins or (op and ins[1] != op):
            raise Unsupported(f'expected {op or "definition"} for {ident}')
        return ins

    def is_u32(ident):
        ins = defs.get(ident)
        return bool(ins and ins[1] == 'OpTypeInt' and ins[2] == ['32', '0'])

    def u32_pointer(ident):
        ins = definition(ident, 'OpTypePointer')
        return ins[2][0] == 'StorageBuffer' and is_u32(ins[2][1])

    def u32_array_root(root):
        var = definition(root, 'OpVariable')
        ptr = definition(var[2][0], 'OpTypePointer')
        if var[2][1:] != ['StorageBuffer'] or ptr[2][0] != 'StorageBuffer':
            raise Unsupported('non-storage GDS/table')
        struct = definition(ptr[2][1], 'OpTypeStruct')
        if len(struct[2]) != 1:
            raise Unsupported('GDS/table is not a one-member struct')
        array = definition(struct[2][0], 'OpTypeRuntimeArray')
        if (not is_u32(array[2][0]) or
                decorations[array[0]].get('ArrayStride') != ['4'] or
                member_decorations[struct[0]].get(('0','Offset')) != ['0']):
            raise Unsupported('GDS/table is not an offset-zero u32 array with stride 4')

    def constant(ident):
        ins = definition(ident, 'OpConstant')
        if not is_u32(ins[2][0]) or len(ins[2]) != 2:
            raise Unsupported('non-u32 constant')
        return int(ins[2][1], 0) & 0xffffffff

    def expr(ident, visiting=None):
        visiting = set() if visiting is None else visiting
        if ident in visiting or len(visiting) >= 24:
            raise Unsupported('cyclic/large address expression')
        result, op, args, _ = definition(ident)
        if not is_u32(args[0]):
            raise Unsupported(f'non-u32 address expression: {ident}')
        visiting = visiting | {ident}
        if op == 'OpConstant':
            return ('constant', constant(ident))
        if op == 'OpLoad':
            _, ptr_op, ptr_args, _ = definition(args[1])
            if not u32_pointer(ptr_args[0]) or ptr_args[1] not in flat_roots:
                raise Unsupported('address loads mutable data or an unrecognized table')
            if ptr_op in ('OpAccessChain','OpInBoundsAccessChain'):
                if len(ptr_args) != 4 or constant(ptr_args[2]) != 0:
                    raise Unsupported('nontrivial table access chain')
                word = constant(ptr_args[3])
            elif ptr_op == 'OpRawAccessChainNV':
                if len(ptr_args) != 6 or constant(ptr_args[2]) != 4 or constant(ptr_args[4]) != 0:
                    raise Unsupported('nontrivial raw table access chain')
                word = constant(ptr_args[3])
            else:
                raise Unsupported('table pointer arithmetic')
            u32_array_root(ptr_args[1])
            return ('flat_word', word)
        arity = {'OpIAdd':2,'OpISub':2,'OpBitwiseAnd':2,'OpShiftRightLogical':2,
                 'OpShiftLeftLogical':2,'OpBitFieldUExtract':3}
        if op not in arity or len(args) != arity[op]+1:
            raise Unsupported(f'unsupported/dynamic address instruction {op}')
        return (op, *(expr(arg, visiting) for arg in args[1:]))

    output = {'source':str(path),'bytes':path.stat().st_size,
              'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),
              'gds_roots':len(roots),'qualified':False,'accesses':[]}
    if not roots:
        output['reason'] = 'no GDS binding'
        return output
    try:
        models = [args for _,op,args,_ in instructions if op == 'OpMemoryModel']
        if len(models) != 1 or models[0][0] != 'Logical':
            raise Unsupported('physical/unrecognized addressing permits unproven aliases')
        for flat in flat_roots:
            u32_array_root(flat)
            for ident,op,args,line in uses[flat]:
                if op in ('OpName','OpDecorate','OpEntryPoint'):
                    continue
                if (op not in ('OpAccessChain','OpInBoundsAccessChain','OpRawAccessChainNV') or
                        len(args) not in (4,6) or args[1] != flat or not u32_pointer(args[0])):
                    raise Unsupported(f'table pointer escapes at line {line}')
                for _,use_op,use_args,use_line in uses[ident]:
                    if use_op in ('OpName','OpDecorate'):
                        continue
                    if (use_op != 'OpLoad' or use_args[1] != ident or not is_u32(use_args[0]) or
                            use_args.count(ident) != 1):
                        raise Unsupported(f'table can change or pointer escapes at line {use_line}')
        for root in roots:
            u32_array_root(root)
            for ident, op, args, line in uses[root]:
                if op in ('OpName','OpDecorate','OpEntryPoint'):
                    continue
                if (op not in ('OpAccessChain','OpInBoundsAccessChain') or
                        len(args) != 4 or args[1] != root or constant(args[2]) != 0 or
                        not u32_pointer(args[0])):
                    raise Unsupported(f'GDS pointer escapes or non-u32 indexing at line {line}')
                index = expr(args[3])
                consumers = []
                for _, use_op, use_args, use_line in uses[ident]:
                    if use_op in ('OpName','OpDecorate'):
                        continue
                    # A deliberately narrow supported memory-operation set. Any indirect
                    # pointer flow or unsupported operation makes the entire module ineligible.
                    if (use_op not in ('OpAtomicIAdd','OpAtomicISub','OpAtomicUMin','OpAtomicUMax') or
                            use_args[1] != ident or not is_u32(use_args[0]) or
                            use_args.count(ident) != 1):
                        raise Unsupported(f'unsupported GDS pointer consumer {use_op} at line {use_line}')
                    consumers.append({'line':use_line,'operation':use_op})
                if consumers:
                    output['accesses'].append({'pointer':ident,'line':line,'dword_index':index,
                                               'bytes':4,'consumers':consumers})
        if not output['accesses'] or len(output['accesses']) > 8:
            raise Unsupported('no accesses or footprint exceeds bounded plan')
        output['qualified'] = True
        output['reason'] = 'all GDS pointers have uniform table/constant u32 expressions and bounded consumers'
    except Unsupported as error:
        output['reason'] = str(error)
        output['accesses'] = []
    return output


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory',type=Path)
    p.add_argument('--output',type=Path,required=True)
    args = p.parse_args()
    rows = [inspect(path) for path in sorted(args.directory.glob('*.spvasm'))]
    report = {'scope':'retained SPIR-V static analysis only; no timing or live performance claim',
              'runtime_requirements':['Match roots to actual GDS/Flatbuf descriptor metadata, not debug names.',
                  'Check every address evaluation and final byte bounds; otherwise full-range fallback.',
                  'Preserve full descriptor, GDS cross-queue waits and future graphics/transfer hazards.',
                  'Reconcile all shader permutations; no persistent cache schema/binary changes.',
                  'Measure barriers actually avoided after all other bindings, not just GDS hits.'],
              'programs':rows}
    args.output.write_text(json.dumps(report,indent=2)+'\n')
    for row in rows:
        print(Path(row['source']).name,row['qualified'],row['reason'])
