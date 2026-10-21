# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify bounded auxiliary-copy/submission witnesses in the pinned read-only game.

Uses the installed LLVM and Python standard library. This is developer analysis;
it neither patches the game nor certifies safe removal of a runtime dependency.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

from analyze_game import elf_for_window, llvm_path
from game_image import Image, read_identity
from project_paths import ROOT, project_child, relative_path, write_json, write_text

GAME_SHA256 = '2d1ca79630d7bbe6fa29575f59aa7996d43071d74ac8e4f7d40137967d9039ab'

WINDOWS = [
    ('copy-enqueue', 0x11fc0, 0xba),
    ('auxiliary-submit', 0x1691e0, 0x2fe),
    ('auxiliary-reservation', 0x16dd50, 0x48),
    ('auxiliary-view-a', 0x16dda0, 0x5f),
    ('auxiliary-view-b', 0x16de00, 0x5f),
    ('dma-builder', 0x63c2c0, 0xee),
    ('event-write-builder', 0x63d370, 0xdb),
    ('command-embedded-reservation', 0x63cc00, 0xce),
    ('wait-memory-builder', 0x63d9c0, 0xab),
    ('acquire-builder', 0x63db30, 0x83),
    ('indirect-buffer-builder', 0x63d8c0, 0x77),
    ('queue-submit-wrapper', 0x63b8d0, 0x5),
    ('queue-submit-import', 0x9858a0, 0x10),
    ('auxiliary-range-view', 0x61e260, 0xce),
    ('auxiliary-arena-users', 0x16df30, 0xef),
    ('auxiliary-cpu-copy', 0x16dcc0, 0x84),
    ('command-manager-reset', 0x169660, 0x44),
    ('feedback-consumer', 0x3f3ed0, 0x189),
    ('feedback-list', 0x3edef0, 0x267),
]

# Each entry is one reviewed instruction or adjacent instruction sequence. Meanings
# stay local to these witnesses; no caller/lifetime completeness is inferred.
WITNESSES = [
    ('dma_destination_argument', 0x11fda, '4889f2'),
    ('dma_source_argument', 0x11fd7, '4989d0'),
    ('dma_chunk_limit', 0x12024, '41b9fcff1f00'),
    ('dma_nonfinal_sync_zero', 0x1202d, 'c7042400000000'),
    ('dma_nonfinal_builder', 0x12034, 'e887a26200', 0x63c2c0),
    ('dma_final_sync_argument', 0x12063, '890424'),
    ('dma_final_builder', 0x12066, 'e855a26200', 0x63c2c0),
    ('dma_packet_opcode', 0x63c336, 'c700005005c0'),
    ('dma_count_mask', 0x63c32f, '4181e5ffff1f00'),
    ('dma_sync_bit', 0x63c366, 'c1e11f'),
    ('dma_base_control', 0x63c36c, '448d8c3100400004'),
    ('dma_source_store', 0x63c389, '4489400889480c'),
    ('dma_destination_store', 0x63c390, '895010897014'),
    ('dma_count_store', 0x63c396, '44896018'),
    ('dma_cursor_advance', 0x63c39a, '498346101c'),
    ('reservation_full_request_advance', 0x16dd56, 'f0440fc18754010000'),
    ('reservation_signed_capacity', 0x16dd5f, '8b87580100004429c04d63c0410f48c1'),
    ('reservation_clamp', 0x16dd6f, '39f00f4fc6'),
    ('reservation_a_stride', 0x16dd74, '4969f0b00000004803b730030000'),
    ('reservation_b_stride', 0x16dd85, '4b8d348048c1e6044803b738030000'),
    ('consumer_positive_reserved_count', 0x3f3fe7, '89c385db7e4e'),
    ('consumer_copy_a_sync_zero', 0x3f4005, '4531c0'),
    ('consumer_copy_b_sync_one', 0x3f4022, '41b801000000'),
    ('consumer_submit', 0x3f4036, 'e8a551d7ff', 0x1691e0),
    ('conditional_preamble_flag', 0x16921c, '4180bc24841b000000'),
    ('conditional_embedded_dword', 0x169241, 'e8ba394d00', 0x63cc00),
    ('conditional_release_one', 0x169278, 'e8f3404d00', 0x63d370),
    ('conditional_wait_memory', 0x169293, 'e828474d00', 0x63d9c0),
    ('conditional_acquire', 0x1692a5, 'e886484d00', 0x63db30),
    ('completion_slot_index', 0x1692d6, '49638c248c1b0000'),
    ('completion_sequence_load', 0x1692de, '458b8c24881b0000'),
    ('completion_sequence_increment_store', 0x1692fd, '41ffc145898c24881b0000'),
    ('completion_release', 0x169317, 'e854404d00', 0x63d370),
    ('reclaim_slot_index', 0x169321, '49638c24801b0000'),
    ('reclaim_marker_rounding', 0x169357, '49c1e9024183c1164183e1f0'),
    ('reclaim_release', 0x169363, 'e808404d00', 0x63d370),
    ('previous_interval_offset', 0x169368, '458bb4247c1b0000'),
    ('previous_interval_base', 0x16938f, '4d03b424701b0000'),
    ('indirect_append', 0x16944c, 'e86f444d00', 0x63d8c0),
    ('doorbell_submit', 0x16949e, 'e82d244d00', 0x63b8d0),
    ('next_interval_align', 0x1694a3, '498b4424204883c03f4883e0c04989442420'),
    ('next_interval_offset_store', 0x1694c1, '418984247c1b0000'),
    ('release_opcode', 0x63d3d8, 'c700024905c0'),
    ('release_data_select', 0x63d3ea, '41c1e41d'),
    ('release_interrupt_base', 0x63d417, '428d8c2300000003'),
    ('release_address_and_data', 0x63d429, '4489780c89501044894814897818'),
    ('indirect_opcode', 0x63d908, 'c700023f02c0'),
    ('indirect_valid_and_mode', 0x63d916, '4181ce00008001'),
    ('submit_import_tail', 0x63b8d0, 'e9cb9f3400', 0x9858a0),
    ('poll_expected_sequence', 0x1695f4, '448bb3881b0000'),
    ('poll_completion_compare', 0x169624, '443934c172e6'),
    ('reset_cursor', 0x169660, '488b87701b0000'),
    ('reset_offset', 0x169680, 'c7877c1b000000000000'),
    ('reset_reclaim_slot', 0x169691, '488b0a486387801b000048c704c100000000'),
    ('cpu_copy_a', 0x16dd13, 'e888668100', 0x9843a0),
    ('cpu_copy_b', 0x16dd33, 'e868668100', 0x9843a0),
    ('cpu_copy_returns_old_index', 0x16dd38, '4489f8'),
    ('view_base_low', 0x61e28d, '8933'),
    ('view_stride_field', 0x61e293, '81e20000ff3f'),
    ('view_count_field', 0x61e31f, '44897308'),
    ('arena_detach_pointer_pair', 0x16e019, 'c4c1781100'),
    ('manager_prior_wait', 0x3edf1a, 'e8c1b6d7ff', 0x1695e0),
]


def verify_witnesses(image):
    checks = []
    for name, address, expected, *target in WITNESSES:
        code = image.read(address, len(bytes.fromhex(expected)))
        if code.hex() != expected:
            raise ValueError(f'Changed instruction: {name} at {address:#x}')
        item = {'name': name, 'rva': hex(address), 'bytes': expected, 'matches': True}
        if target:
            if code[0] not in (0xe8, 0xe9) or len(code) != 5:
                raise ValueError('Expected rel32 instruction')
            actual = address + 5 + struct.unpack_from('<i', code, 1)[0]
            if actual != target[0]:
                raise ValueError(f'Changed call or tail target: {name}')
            item['target'] = hex(actual)
        checks.append(item)
    return checks


def headers(data):
    start = 0
    if data[:4] == bytes.fromhex('4f153d1d'):
        start = 32 + struct.unpack_from('<H', data, 24)[0] * 32
    elf = struct.unpack_from('<16sHHIQQQIHHHHHH', data, start)
    return [list(struct.unpack_from('<IIQQQQQQ', data, start + elf[5] + i * elf[9]))
            for i in range(elf[10])]


def segment_file_data(image, file_offset, size):
    data = image.data
    if data[:4] != bytes.fromhex('4f153d1d'):
        if file_offset < 0 or file_offset + size > len(data):
            raise ValueError('File segment out of bounds')
        return data[file_offset:file_offset + size]
    table = headers(data)
    for i in range(struct.unpack_from('<H', data, 24)[0]):
        flags, offset, stored, _ = struct.unpack_from('<QQQQ', data, 32 + i * 32)
        if not flags & 0x800:
            continue
        ph = table[(flags >> 20) & 0xfff]
        if ph[2] <= file_offset and file_offset + size <= ph[2] + ph[5]:
            if flags & (2 | 8):
                raise ValueError('Compressed or encrypted segment unsupported')
            relative = file_offset - ph[2]
            if relative + size > stored or offset + relative + size > len(data):
                raise ValueError('SELF segment truncated')
            return data[offset + relative:offset + relative + size]
    raise ValueError('SELF segment not mapped')


def imported_symbol(image, stub):
    code = image.read(stub, 6)
    if code[:2] != b'\xff\x25':
        raise ValueError('Expected RIP-relative import stub')
    got = stub + 6 + struct.unpack_from('<i', code, 2)[0]
    table = headers(image.data)
    ph_dyn, = [ph for ph in table if ph[0] == 2]
    ph_lib, = [ph for ph in table if ph[0] == 0x61000000]
    dynamic = segment_file_data(image, ph_dyn[2], ph_dyn[5])
    library = segment_file_data(image, ph_lib[2], ph_lib[5])
    entries = {}
    for offset in range(0, len(dynamic), 16):
        tag, value = struct.unpack_from('<qQ', dynamic, offset)
        if not tag:
            break
        entries[tag] = value
    relocations = library[entries[0x61000029]:entries[0x61000029] + entries[0x6100002d]]
    matches = []
    for offset in range(0, len(relocations), 24):
        address, info, addend = struct.unpack_from('<QQq', relocations, offset)
        if address != got:
            continue
        symbol_index = info >> 32
        name_offset = struct.unpack_from('<I', library, entries[0x61000039] + symbol_index * 24)[0]
        start = entries[0x61000035] + name_offset
        end = library.index(b'\0', start, entries[0x61000035] + entries[0x61000037])
        matches.append({'stub_rva': hex(stub), 'got_rva': hex(got), 'relocation_type': info & 0xffffffff,
                        'addend': addend, 'symbol_index': symbol_index,
                        'encoded_symbol': library[start:end].decode('ascii')})
    if len(matches) != 1:
        raise ValueError('Import GOT must have exactly one relocation')
    return matches[0]


def recovered_contract():
    # Static interpretation of the byte witnesses above and the project's PM4
    # definitions. These are expected templates, never actual guest values.
    return {
        'evidence': 'static instructions and resolved imports; no runtime packet capture',
        'copy_helper': {
            'rva': '0x11fc0', 'destination_argument': 'rsi', 'source_argument': 'rdx',
            'byte_count_argument': 'ecx', 'final_cp_sync_argument': 'r8d',
            'chunk_limit': 0x1ffffc, 'zero_byte_input_supported': False,
            'zero_byte_note': 'The reviewed caller skips nonpositive reservation results. '
                              'No behavior for arbitrary zero/negative/wrapped sizes is certified.',
            'dma_packet_header': '0xc0055000',
            'caller_a': {'source': 'emitter+0x110', 'destination': 'reserved arena A',
                         'bytes': 'positive reserved count * 0xb0 modulo 2^32', 'final_cp_sync': 0},
            'caller_b': {'source': 'emitter+0x118', 'destination': 'reserved arena B',
                         'bytes': 'positive reserved count * 0x50 modulo 2^32', 'final_cp_sync': 1},
            'memory_dma_control_without_sync': '0x04004000',
            'memory_dma_control_with_sync': '0x84004000',
            'guest_cpu_memcpy': False,
        },
        'reservation': {
            'rva': '0x16dd50', 'atomic_cursor_field': 'manager+0x154',
            'capacity_field': 'manager+0x158', 'arena_a_field': 'manager+0x330',
            'arena_b_field': 'manager+0x338', 'arena_strides': [0xb0, 0x50],
            'cursor_advance': 'full requested uint32 count before signed clamp',
            'return': 'signed clamped count; destination arithmetic uses sign-extended old cursor',
            'nonnegative_no_wrap_range_proven': False,
        },
        'submit': {
            'rva': '0x1691e0', 'manager_static_rva': '0x7d3cf10',
            'completion_table_pointer_rva': '0xe17e80',
            'completion_index_field': 'manager+0x1b8c',
            'expected_sequence_field': 'manager+0x1b88',
            'reclaim_index_field': 'manager+0x1b80',
            'command_base_field': 'manager+0x1b70',
            'next_interval_offset_field': 'manager+0x1b7c',
            'completion': 'increment uint32 sequence, append RELEASE_MEM writing that sequence',
            'reclaim': 'append separate RELEASE_MEM writing next aligned command-buffer dword offset',
            'release_template': ['0xc0054902', '0x0000062f', '0x23000000',
                                 'address_lo', 'address_hi', 'value32', '0x00000000'],
            'release_data_select': 'Data32Low', 'release_interrupt_select': 'IrqUndocumented (3)',
            'marker_formula': '((cursor_after_first_release - command_base) / 4 + 22) & ~15 '
                              'under the valid aligned nonwrapping range assumption',
            'marker_meaning': 'includes the second seven-dword release and alignment to 16 dwords',
            'indirect_buffer_header': '0xc0023f02',
            'indirect_buffer_mode': '0x01800000 | (interval_dwords & 0xfffff)',
            'doorbell': 'sceGnmDingDong(queue_id, ring_write_offset_dw)',
            'conditional_preamble': 'manager+0x1b84 enables embedded dword, RELEASE_MEM(1), '
                                    'WAIT_REG_MEM and ACQUIRE_MEM before the two normal releases',
            'sequence_is_rendered_frame': False, 'sequence_proves_object_generation': False,
        },
        'other_pool_users': {
            'cpu_copy_rva': '0x16dcc0', 'copy_import': 'internal_memcpy',
            'cpu_copy_count': 2, 'cpu_copy_strides': [0xb0, 0x50],
            'cpu_copy_return': 'old reservation index; unlike the count-returning 0x16dd50 helper',
            'view_helpers': ['0x16dda0', '0x16de00'],
            'view_builder': '0x61e260: constructs a 16-byte buffer descriptor; '
                            'base, stride and count stores observed, not an allocation',
            'advertised_view_count': 2000, 'actual_capacity_equals_view_count_proven': False,
            'detach_rva': '0x16e019',
            'detach': 'conditional CPU writes followed by clearing both arena pointer fields',
            'reset_rva': '0x169660',
            'reset': 'rewinds command cursor, clears interval offset, preamble flag and reclaim slot; '
                     'does not store expected sequence+0x1b88 in this reviewed helper',
        },
        'unresolved': [
            'Actual nonfaulting auxiliary count/source/destination values and active task generations.',
            'Pool allocation/capacity initialization, reset callers and all mixed CPU/GPU accesses.',
            'Per-queue command-buffer instance and submission boundaries under concurrency and reuse.',
            'Whether completion and reclaim slots are distinct for each live instance; fields differ '
            'but runtime index values are not captured.',
            'Descriptor construction and declared copy ranges do not prove actual shader stores.',
            'Conditional preamble, callback growth and other callers prevent a universal two-release '
            'or one-pair-per-submission assumption.',
        ],
        'next_evidence': 'Observe bounded command-side DMA/RELEASE_MEM/IB metadata with queue, '
                         'submission and buffer-instance boundaries, then bind to CPU consumption. '
                         'Pattern similarity alone must not certify a generation or remove a fence.',
    }


def analyze(output):
    output = project_child(relative_path(output))
    if not output.is_relative_to(project_child('doc')):
        raise ValueError('Analysis output must stay below doc')
    project = json.loads((ROOT / 'project.json').read_text(encoding='utf-8'))
    game = project_child(project['game_relative_path'])
    image = Image(game / 'eboot.bin')
    digest = hashlib.sha256(image.data).hexdigest()
    identity = read_identity(game / 'sce_sys/param.sfo')
    if not (digest == project['game_sha256'] == GAME_SHA256 and
            identity['TITLE_ID'] == project['title'] == 'CUSA00309' and
            identity['APP_VER'] == project['version'] == '01.07'):
        raise ValueError('Pinned original game identity mismatch')
    tool = llvm_path(None)
    if not tool:
        raise ValueError('Existing LLVM is required; no substitute toolchain is created')
    windows = []
    assemblies = {}
    for name, address, size in WINDOWS:
        raw = image.read(address, size)
        result = subprocess.run([tool, '-d', '--x86-asm-syntax=intel', '-'],
                                input=elf_for_window(address, raw), capture_output=True, check=True)
        assembly = result.stdout.decode('utf-8', errors='strict')
        if '<unknown>' in assembly:
            raise ValueError(f'Undecodable instruction in selected window: {name}')
        filename = name + '.asm.txt'
        assemblies[filename] = assembly
        windows.append({'name': name, 'rva': hex(address), 'size': size,
                        'sha256': hashlib.sha256(raw).hexdigest(),
                        'disassembly': filename,
                        'direct_calls': re.findall(r'\bcall\s+(0x[0-9a-f]+)', assembly),
                        'scope': 'bounded window; completeness requires explicit control-flow review'})
    report = {'schema': 1, 'identity': identity, 'game_sha256': digest,
              'llvm': str(tool), 'windows': windows, 'source_game_written': False,
              'runtime_measured': False, 'generation_proven': False,
              'function_recovery_complete': False,
              'safe_to_replace_cpu_consumption_with_gpu': False,
              'safe_to_remove_dependencies': False}
    report['program_headers'] = headers(image.data)
    report['submission_import'] = imported_symbol(image, 0x9858a0)
    report['copy_import'] = imported_symbol(image, 0x9843a0)
    if report['submission_import']['encoded_symbol'] != 'bX5IbRvECXk#M#N':
        raise ValueError('Submission import differs from pinned sceGnmDingDong NID')
    if report['copy_import']['encoded_symbol'] != 'Q3VBxCXhUHs#c#c':
        raise ValueError('CPU copy import differs from pinned internal_memcpy NID')
    report['recovered_contract'] = recovered_contract()
    report['engine_sources'] = []
    for source in ('src/core/libraries/gnmdriver/gnmdriver.cpp',
                   'src/core/libraries/libc_internal/libc_internal_memory.cpp',
                   'src/video_core/amdgpu/pm4_cmds.h',
                   'src/video_core/amdgpu/liverpool.cpp'):
        report['engine_sources'].append({'path': source,
                                        'sha256': hashlib.sha256((ROOT / source).read_bytes()).hexdigest()})
    report['instruction_witnesses'] = verify_witnesses(image)
    # Validate every input/witness before publishing any new output. The JSON
    # manifest is replaced last, after its corresponding disassemblies.
    for filename, assembly in assemblies.items():
        write_text(output / filename, assembly)
    write_json(output / 'helpers.json', report)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path('doc/binary-analysis/auxiliary'))
    args = parser.parse_args()
    report = analyze(project_child(args.output))
    windows = report['windows']
    print(json.dumps({'windows': len(windows), 'bytes_decoded': sum(x['size'] for x in windows),
                      'instruction_witnesses': len(report['instruction_witnesses']),
                      'game_sha256': report['game_sha256'], 'llvm': report['llvm']}))


if __name__ == '__main__':
    main()
