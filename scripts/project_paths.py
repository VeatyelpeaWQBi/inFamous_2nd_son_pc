# SPDX-License-Identifier: GPL-2.0-or-later
"""Project-relative inputs and guarded, atomic user-data writes."""
import json
import os
import stat
from pathlib import Path
from uuid import uuid4

ROOT = Path(__file__).resolve().parents[1]


def resolve_project_path(value):
    path = Path(value)
    if path.is_absolute() or path.drive or path.root:
        raise ValueError('Stored paths must be relative to the project directory')
    return (ROOT / path).resolve()


def project_child(value):
    """Reject escaping paths and links before reading or writing project state."""
    path = Path(os.path.abspath(ROOT / Path(value)))
    resolved = resolve_project_path(value)
    if path == ROOT or not path.is_relative_to(ROOT) or not resolved.is_relative_to(ROOT):
        raise ValueError('Path escaped the project directory')
    for item in (path, *path.parents):
        if item == ROOT:
            break
        try:
            info = item.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & getattr(stat, 'FILE_ATTRIBUTE_REPARSE_POINT', 0):
            raise ValueError('Project data path contains a link')
    return path


def relative_path(value):
    return Path(os.path.relpath(Path(value).absolute(), ROOT)).as_posix()


def read_object(path):
    value = json.loads(path.read_text(encoding='utf-8-sig'))
    if not isinstance(value, dict):
        raise ValueError(f'Expected a JSON object: {path.name}')
    return value


def write_text(path, text):
    path = project_child(relative_path(path))
    path.parent.mkdir(parents=True, exist_ok=True)
    pending = path.with_name(path.name + '.' + uuid4().hex + '.pending')
    try:
        pending.write_text(text, encoding='utf-8')
        pending.replace(path)
    finally:
        pending.unlink(missing_ok=True)


def write_json(path, value):
    write_text(path, json.dumps(value, ensure_ascii=False, indent=2)+'\n')
