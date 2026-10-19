"""Verify a baseline without starting the runtime or changing game files."""
import argparse
import hashlib
import json
import sys
import xml.etree.ElementTree as ET
from game_image import Image, read_identity, fnv1a
from launcher_settings import (RESOLUTION_CONFIG, load_settings, apply_settings,
                               apply_resolution, resolution_config)
from project_paths import ROOT, resolve_project_path, project_child, relative_path, read_object, write_json, write_text

def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()

def resolution_patch(name):
    return name.startswith("Native ") or name.startswith("PS4 Pro mode:")

def prepare_xml(template, mode):
    if template.stat().st_size > 1024 * 1024:
        raise ValueError("Patch XML exceeds the supported size")
    root = ET.parse(template).getroot()
    if root.tag != "Patch":
        raise ValueError("Expected a Patch XML document")
    selected_patch = resolution_config(mode)[3]
    for meta in root.findall("Metadata"):
        name = meta.get("Name", "")
        if resolution_patch(name):
            enabled = name == selected_patch
            meta.set("isEnabled", str(enabled).lower())
    return root

def audit_xml(image, root, identity, mode):
    titles = [n.text for n in root.findall("./TitleID/ID")]
    if identity["TITLE_ID"] not in titles:
        raise ValueError("Patch title does not match the game")
    patches, used = [], {}
    active_resolutions = []
    for meta in root.findall("Metadata"):
        if meta.get("isEnabled") != "true":
            continue
        if meta.get("AppVer") != identity["APP_VER"] or meta.get("AppElf") != "eboot.bin":
            raise ValueError("Enabled patch has an incompatible version or executable")
        name = meta.get("Name", "")
        if resolution_patch(name):
            active_resolutions.append(name)
        lines = []
        for node in meta.findall("./PatchList/Line"):
            if node.get("Type") != "bytes":
                raise ValueError("This baseline only accepts verified literal byte patches")
            offset = int(node.get("Address", ""), 16) - 0x400000
            if offset < 0:
                raise ValueError("Patch address lies below the executable base")
            value = bytes.fromhex(node.get("Value", ""))
            if not value or len(value) > 65536:
                raise ValueError("Empty or oversized patch payload")
            expected = node.get("OriginalHash", "").lower()
            actual = fnv1a(image.read(offset, len(value)))
            if not expected or actual != expected:
                raise ValueError(f"OriginalHash mismatch: {name} at RVA {offset:#x}")
            for i, byte in enumerate(value):
                if offset + i in used and used[offset + i] != byte:
                    raise ValueError(f"Conflicting enabled patches at RVA {offset+i:#x}")
                used[offset+i] = byte
            lines.append({"rva": hex(offset), "size": len(value), "original_fnv1a": actual})
        patches.append({"name": name, "lines": lines})
    selected_patch = resolution_config(mode)[3]
    expected = [selected_patch] if selected_patch else []
    if active_resolutions != expected:
        raise ValueError("Resolution patch selection is inconsistent")
    return patches

def inspect(mode, runtime=None, session=None, frozen=False):
    if frozen and mode not in ("1080p", "pro4k"):
        raise ValueError("Frozen benchmarks require an explicit 1080p or pro4k baseline")
    launcher_settings = None if frozen else load_settings(mode)
    if launcher_settings is not None:
        mode = launcher_settings["resolution"]
    width, height, baseline_mode, _ = resolution_config(mode)
    project = read_object(ROOT / "project.json")
    game = project_child(project["game_relative_path"])
    executable = resolve_project_path(runtime or project["runtime_path"])
    if digest(executable) != project["runtime_sha256"]:
        raise ValueError("Runtime fingerprint changed; review and pin it before use")
    image = Image(game / "eboot.bin")
    if hashlib.sha256(image.data).hexdigest() != project["game_sha256"]:
        raise ValueError("Game executable fingerprint does not match the baseline")
    identity = read_identity(game / "sce_sys/param.sfo")
    if identity["TITLE_ID"] != project["title"] or identity["APP_VER"] != project["version"]:
        raise ValueError("Game identity does not match the baseline")
    profile = read_object(project_child("profiles/" + baseline_mode + ".json"))
    config_path = project_child("state/play/user/custom_configs/CUSA00309.json")
    if frozen or not config_path.is_file():
        config_path = project_child("state/baseline/user/custom_configs/CUSA00309.json")
    config = read_object(config_path)
    for group, values in profile["overrides"].items():
        config.setdefault(group, {}).update(values)
    if launcher_settings is not None:
        apply_settings(config, launcher_settings)
    apply_resolution(config, mode)
    gpu = config["GPU"]
    if type(gpu["readbacks_mode"]) is not int or gpu["readbacks_mode"] != 2 or gpu["hdr_allowed"] is not False:
        raise ValueError("The correctness baseline requires precise readbacks and SDR output")
    if config["General"]["neo_mode"] is not (baseline_mode == "pro4k"):
        raise ValueError("The selected console mode is inconsistent")
    expected_size = (width, height)
    if (gpu["window_width"], gpu["window_height"]) != expected_size:
        raise ValueError("Window dimensions do not match the selected mode")
    xml = prepare_xml(project_child("profiles/CUSA00309.xml"), mode)
    patches = audit_xml(image, xml, identity, mode)
    report = {
        "schema": 3, "path_base": "project", "mode": mode, "runtime": relative_path(executable),
        "resolution": {"width": width, "height": height}, "baseline_profile": baseline_mode,
        "runtime_sha256": project["runtime_sha256"], "identity": identity,
        "game_sha256": project["game_sha256"], "patches": patches,
        "gpu_config": config["GPU"], "neo_mode": config["General"]["neo_mode"],
        "launcher_settings": launcher_settings,
        "runtime_capabilities": {"benchmark_dir": True, "wait_for_debugger": True,
                                 "evidence": "runtime log revision 3c3a06a282203045994ec48afd9c307e00e473ba and matching src/main.cpp",
                                 "dynamic_probe_executed": False},
        "source_game_written": False, "game_launched": False,
        "brightness_verified": False, "particles_verified": False, "fps_verified": False,
    }
    if session:
        target = project_child(session)
        allowed = project_child("state/sessions")
        if not target.is_relative_to(allowed) or target == allowed:
            raise ValueError("Session must be below this project's state/sessions directory")
        write_json(target / "custom_configs/CUSA00309.json", config)
        ET.indent(xml, space="    ")
        patch_path = target / "patches/CUSA00309.xml"
        write_text(patch_path, ET.tostring(xml, encoding="unicode", xml_declaration=True))
        report["patch_xml"] = relative_path(patch_path)
        write_json(target / "preflight.json", report)
    return report

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("auto", *RESOLUTION_CONFIG), default="auto")
    parser.add_argument("--runtime")
    parser.add_argument("--session")
    parser.add_argument("--frozen", action="store_true")
    args = parser.parse_args()
    try:
        report = inspect(args.mode, args.runtime, args.session, args.frozen)
    except (OSError, ValueError, KeyError, ET.ParseError) as error:
        print(f"Preflight failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0

if __name__ == "__main__":
    sys.exit(main())
