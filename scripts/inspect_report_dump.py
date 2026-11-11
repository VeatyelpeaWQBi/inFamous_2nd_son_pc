# SPDX-License-Identifier: GPL-2.0-or-later
"""Read an AMD64 minidump and unwind body/prologue frames using matching local PE files.

No native DLLs, live process access, external symbols or network lookup. Unsupported unwind
operations stop explicitly. This is an offline diagnostic, not a general Windows debugger.
Layout/operations: Microsoft x64 exception handling and the installed Windows minidump ABI.
"""
import argparse
from bisect import bisect_right
import json
from pathlib import Path
import struct


def value(data, offset, fmt):
    return struct.unpack_from('<' + fmt, data, offset)[0]


class PE:
    def __init__(self, path, size, timestamp):
        self.data = Path(path).read_bytes()
        nt = value(self.data, 60, 'I')
        if self.data[nt:nt + 4] != b'PE\0\0':
            raise ValueError('Not PE')
        opt = nt + 24
        if (value(self.data, opt, 'H') != 0x20b or
                value(self.data, opt + 56, 'I') != size or
                value(self.data, nt + 8, 'I') != timestamp):
            raise ValueError('Local image does not match dump identity')
        self.headers = value(self.data, opt + 60, 'I')
        self.sections = []
        for i in range(value(self.data, nt + 6, 'H')):
            at = opt + value(self.data, nt + 20, 'H') + i * 40
            self.sections.append((value(self.data, at + 12, 'I'),
                                  value(self.data, at + 16, 'I'),
                                  value(self.data, at + 20, 'I')))
        rva, length = struct.unpack_from('<II', self.data, opt + 112 + 3 * 8)
        self.functions = list(struct.iter_unpack('<III', self.read(rva, length))) if length else []
        self.starts = [f[0] for f in self.functions]
        self.exports = {}
        export = value(self.data, opt + 112, 'I')
        if export:
            e = self.read(export, 40)
            n = value(e, 24, 'I')
            funcs, names, ordinals = struct.unpack_from('<III', e, 28)
            if n > 1_000_000:
                raise ValueError('Invalid export count')
            for i in range(n):
                ordinal = value(self.read(ordinals + 2 * i, 2), 0, 'H')
                address = value(self.read(funcs + ordinal * 4, 4), 0, 'I')
                name = value(self.read(names + i * 4, 4), 0, 'I')
                chars = bytearray()
                for j in range(2048):
                    char = self.read(name + j, 1)
                    if char == b'\0':
                        break
                    chars.extend(char)
                self.exports[address] = chars.decode('ascii', errors='replace')

    def read(self, rva, length):
        if length == 0:
            return b''
        if rva < self.headers and rva + length <= self.headers:
            return self.data[rva:rva + length]
        for start, size, offset in self.sections:
            if start <= rva and rva + length <= start + size:
                return self.data[offset + rva - start:offset + rva - start + length]
        raise ValueError(f'RVA not backed by local file: {rva:x}+{length}')

    def function(self, rva):
        i = bisect_right(self.starts, rva) - 1
        return self.functions[i] if i >= 0 and rva < self.functions[i][1] else None


def inspect(path):
    dump = path.read_bytes()
    if dump[:4] != b'MDMP':
        raise ValueError('Not a minidump')
    streams = {}
    for i in range(value(dump, 8, 'I')):
        kind, size, at = struct.unpack_from('<III', dump, value(dump, 12, 'I') + i * 12)
        streams[kind] = at, size
    at = streams[6][0]
    thread = value(dump, at, 'I')
    context_size, context_at = struct.unpack_from('<II', dump, at + 160)
    if context_size < 256 or not value(dump, context_at + 48, 'I') & 0x100000:
        raise ValueError('AMD64 exception context is required')
    registers = list(struct.unpack_from('<16Q', dump, context_at + 120))
    pc = value(dump, context_at + 248, 'Q')
    modules = []
    mod_at = streams[4][0]
    for i in range(value(dump, mod_at, 'I')):
        entry = mod_at + 4 + i * 108
        name = value(dump, entry + 20, 'I')
        name = dump[name + 4:name + 4 + value(dump, name, 'I')].decode('utf-16le')
        modules.append(dict(base=value(dump, entry, 'Q'), size=value(dump, entry + 8, 'I'),
                            timestamp=value(dump, entry + 16, 'I'), path=name))
    threads = streams[3][0]
    stack = None
    for i in range(value(dump, threads, 'I')):
        entry = threads + 4 + 48 * i
        if value(dump, entry, 'I') == thread:
            stack = struct.unpack_from('<QII', dump, entry + 24)
    if stack is None:
        raise ValueError('Faulting thread stack is absent')
    bottom, length, stack_at = stack

    def qword(address):
        if not bottom <= address <= bottom + length - 8:
            raise ValueError(f'Stack outside dump: {address:x}')
        return value(dump, stack_at + address - bottom, 'Q')

    def apply(pe, function, offset, depth=0):
        if depth > 16:
            raise ValueError('Unwind chain too deep')
        info = function[2]
        h = pe.read(info, 4)
        version, flags = h[0] & 7, h[0] >> 3
        if version not in (1, 2):
            raise ValueError(f'Unsupported unwind version {version}')
        codes = pe.read(info + 4, h[2] * 2)
        i = 0
        while i < h[2]:
            code_offset, op = codes[i * 2:i * 2 + 2]
            operation, arg = op & 15, op >> 4
            slots = {0: 1, 1: 2 if arg == 0 else 3, 2: 1, 3: 1,
                     4: 2, 5: 3, 8: 2, 9: 3}.get(operation)
            if slots is None or i + slots > h[2]:
                raise ValueError(f'Unsupported/truncated unwind op {operation}')
            extra = codes[(i + 1) * 2:(i + slots) * 2]
            if code_offset <= offset:
                if operation == 0:
                    registers[arg] = qword(registers[4])
                    registers[4] += 8
                elif operation == 1:
                    registers[4] += int.from_bytes(extra, 'little') * (8 if arg == 0 else 1)
                elif operation == 2:
                    registers[4] += arg * 8 + 8
                elif operation == 3:
                    registers[4] = registers[h[3] & 15] - (h[3] >> 4) * 16
                elif operation in (4, 5):
                    displacement = int.from_bytes(extra, 'little') * (8 if operation == 4 else 1)
                    registers[arg] = qword(registers[4] + displacement)
                # XMM saves do not affect this integer/control-register unwind.
            i += slots
        if flags & 4:
            chained = struct.unpack('<III', pe.read(info + (4 + h[2] * 2 + 3) // 4 * 4, 12))
            apply(pe, chained, 2**32, depth + 1)

    frames, seen, stopped = [], set(), None
    images = {}
    try:
        for _ in range(64):
            signature = pc, registers[4]
            if signature in seen:
                raise ValueError('Repeated frame')
            seen.add(signature)
            module = next((m for m in modules if m['base'] <= pc < m['base'] + m['size']), None)
            if module is None:
                raise ValueError(f'PC outside known modules: {pc:x}')
            if module['path'] not in images:
                images[module['path']] = PE(module['path'], module['size'], module['timestamp'])
            pe = images[module['path']]
            rva = pc - module['base']
            function = pe.function(rva)
            frames.append(dict(module=module['path'], rva=hex(rva), rsp=hex(registers[4]),
                               function_begin=hex(function[0]) if function else None,
                               exact_export=pe.exports.get(function[0] if function else rva)))
            old_rsp = registers[4]
            if function:
                apply(pe, function, rva - function[0])
            pc = qword(registers[4])
            registers[4] += 8
            if pc == 0:
                break
            if registers[4] <= old_rsp:
                raise ValueError('Nonascending stack')
    except (ValueError, OSError, struct.error) as error:
        stopped = str(error)
    return dict(dump=str(path), exception_code=hex(value(dump, at + 8, 'I')),
                fault_address=hex(value(dump, at + 24, 'Q')), thread=thread, frames=frames,
                stopped=stopped, limitation='Matching local PE unwind metadata; body/prologue '
                'frames only. Unsupported operations, missing memory and mismatched images '
                'stop explicitly. No raw-stack guesses are reported as frames.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dump', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = inspect(args.dump)
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    for i, frame in enumerate(result['frames']):
        print(i, Path(frame['module']).name, frame['rva'], frame['exact_export'])
    print('Stopped:', result['stopped'])
