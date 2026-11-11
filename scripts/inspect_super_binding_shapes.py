"""Inspect existing cached SPIR-V without modifying cache files or invoking a compiler."""
import hashlib
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]


def inspect(entry):
    file = ROOT / entry["source"]
    data = file.read_bytes()
    offset = data.find(b"\x03\x02\x23\x07", 0, 64)
    if offset not in (0, 24):
        raise ValueError(f"Unknown SPIR-V envelope: {file}")
    if offset == 24 and (data[:8] != b"SHADCA01" or
                         struct.unpack_from("<Q", data, 8)[0] != len(data) - 24):
        raise ValueError(f"Invalid cache envelope length: {file}")
    raw = data[offset:]
    words = struct.unpack("<" + "I" * (len(raw) // 4), raw)
    index = 5
    names, bindings = {}, {}
    images = samplers = 0
    while index < len(words):
        count, opcode = words[index] >> 16, words[index] & 65535
        if not count or index + count > len(words):
            raise ValueError(f"Invalid instruction: {file}")
        args = words[index+1:index+count]
        if opcode == 5:  # OpName
            names[args[0]] = struct.pack("<" + "I" * len(args[1:]), *args[1:]).split(b"\0")[0].decode("utf8")
        elif opcode == 71 and len(args) > 2 and args[1] == 33:  # OpDecorate Binding
            bindings[args[0]] = args[2]
        elif opcode == 25:  # OpTypeImage
            images += 1
        elif opcode == 26:  # OpTypeSampler
            samplers += 1
        index += count
    digest = hashlib.sha256(data).hexdigest()
    return dict(shader=entry["hash"], source=entry["source"], sha256=digest,
                matches_prior_disassembly=digest == entry["source_sha256"], cache_header_bytes=offset,
                bindings=[dict(binding=value, name=names.get(key, ""))
                          for key, value in sorted(bindings.items(), key=lambda item: item[1])],
                image_types=images, sampler_types=samplers)


if __name__ == "__main__":
    entries = json.loads((ROOT / "doc/super-shader-provenance.json").read_text(encoding="utf-8-sig"))
    results = [inspect(entry) for entry in entries]
    target = ROOT / "doc/super-deep-20261031/buffer-plan-feasibility.json"
    target.write_text(json.dumps(results, indent=2)+"\n", encoding="utf8")
    for result in results:
        print(result["shader"], "decorated variables:", len(result["bindings"]),
              "image types:", result["image_types"], "sampler types:", result["sampler_types"],
              "matches prior disassembly:", result["matches_prior_disassembly"])
