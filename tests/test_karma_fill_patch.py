"""Exercise the XML's actual SysV guest wrapper against bounded synthetic callees.

Windows x64, installed Python only; no game process, save, dependency or disk cache.
This does not establish in-game unlock/HUD correctness.
"""
import ctypes
from pathlib import Path
import struct
import xml.etree.ElementTree as ET


def main():
    root = Path(__file__).resolve().parents[1]
    patch = ET.parse(root / 'profiles/CUSA00309.xml').getroot().find('RuntimeCheats')
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.VirtualAlloc.restype = ctypes.c_void_p
    kernel.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_uint32]
    kernel.VirtualFree.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32]
    base = kernel.VirtualAlloc(None, 0x2000000, 0x3000, 0x40)
    if not base:
        raise ctypes.WinError(ctypes.get_last_error())
    arena, hero = base + 0x1f00000, base + 0x1000000
    try:
        for code in patch.findall('Code'):
            data = bytes.fromhex(code.attrib['Value'])
            ctypes.memmove(arena + int(code.attrib['Offset'], 16), data, len(data))
        for entry in patch.find('Relocations'):
            a = entry.attrib
            offset, target = int(a['Offset'], 16), int(a['Target'], 16)
            target = arena + target if a['Space'] == 'arena' else base + target - 0x400000
            data = (struct.pack('<Q', target) if a['Type'] == 'abs64' else
                    struct.pack('<i', target - (arena + offset + 4)))
            ctypes.memmove(arena + offset, data, len(data))
        # Replacement callees leave the wrapper's player/request guards intact.
        ctypes.memmove(arena + 0x50, b'\xc3', 1)
        ctypes.memmove(base + 0x266310, bytes.fromhex('C747282A000000C3'), 8)
        ctypes.c_uint64.from_address(base + 0x1d0cfe0).value = hero
        owner = ctypes.c_uint64.from_address(hero + 0x1ef48)
        owner.value = hero
        faction = ctypes.c_uint32.from_address(base + 0xefacf8 + 0xc4)
        faction.value = 0
        # Windows -> SysV: preserve RDI/RSI and keep the call stack aligned.
        shim = (bytes.fromhex('57564883EC084889CF48B8') + struct.pack('<Q', arena + 0x320) +
                bytes.fromhex('FFD04883C4085E5FC3'))
        ctypes.memmove(base, shim, len(shim))
        run = ctypes.CFUNCTYPE(None, ctypes.c_void_p)(base)
        request = ctypes.c_uint32.from_address(arena + 0x44)
        complete = ctypes.c_uint32.from_address(arena + 0x48)
        result = ctypes.c_uint32.from_address(arena + 0x4c)
        meter = ctypes.c_uint32.from_address(hero + 0x1ef58)
        run(hero)
        assert complete.value == 0
        request.value = 1
        run(hero)
        assert (complete.value, result.value) == (1, 42)
        meter.value = 0
        run(hero)
        assert meter.value == 0, 'a consumed one-shot must not refill'
        assert result.value == 0, 'live telemetry must reflect consumption without a new fill'
        request.value = 2
        run(hero + 0x40000)
        assert complete.value == 1, 'non-player cannot consume a request'
        owner.value = 0
        run(hero)
        assert complete.value == 1, 'uninitialized streak cannot consume a request'
        owner.value = hero
        faction.value = 2
        run(hero)
        assert complete.value == 1, 'invalid faction cannot invoke the setter'
        faction.value = 1
        run(hero)
        assert complete.value == 2
        print('PASS: XML native one-shot execution, consumption, player/owner/faction guards')
    finally:
        kernel.VirtualFree(base, 0, 0x8000)


if __name__ == '__main__':
    main()
