# SPDX-License-Identifier: GPL-2.0-or-later
"""Record and check project source provenance; never compile or start an engine."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import struct
from project_paths import ROOT, project_child, read_object, write_json

CONFIGURATIONS = ('RelWithDebInfo', 'Release', 'Debug')


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def source_snapshot():
    files = [ROOT / name for name in (
        'CMakeLists.txt', 'inFAMOUS Second Son.ico', 'build.bat',
        'scripts/build.ps1', 'scripts/build_tools.ps1', 'scripts/build_identity.py', 'scripts/tools.ps1')]
    for directory in ('src', 'cmake'):
        files.extend(p for p in (ROOT / directory).rglob('*') if p.is_file())
    h = hashlib.sha256()
    for path in sorted(files, key=lambda p: p.relative_to(ROOT).as_posix()):
        relative = path.relative_to(ROOT).as_posix()
        safe = project_child(relative)
        h.update(relative.encode('utf-8') + b'\0')
        h.update(bytes.fromhex(digest(safe)))
    project = read_object(ROOT / 'project.json')
    for key in ('dependencies_root', 'dependency_build_root'):
        h.update(key.encode('ascii') + b'\0' + project[key].encode('utf-8') + b'\0')
    return {'source_sha256': h.hexdigest(), 'source_files': len(files)}


def activate(configuration, expected_source):
    if configuration not in CONFIGURATIONS:
        raise ValueError('Unsupported build configuration')
    snapshot = source_snapshot()
    if snapshot['source_sha256'] != expected_source:
        raise ValueError('Project source changed during the build. Run build.bat again; runtime was not switched.')
    relative = f'Build/{configuration}/inFAMOUS Second Son.exe'
    executable = project_child(relative)
    # Verify a Windows x64 PE image rather than accepting an arbitrary nonempty file.
    with executable.open('rb') as stream:
        header = stream.read(64)
        if len(header) != 64 or header[:2] != b'MZ':
            raise ValueError('Build output is not a Windows executable')
        offset = struct.unpack_from('<I', header, 60)[0]
        stream.seek(offset)
        if stream.read(6) != b'PE\0\0\x64\x86':
            raise ValueError('Build output is not a Windows x64 executable')
    pdb_relative = f'Build/{configuration}/inFAMOUS Second Son.pdb'
    pdb = project_child(pdb_relative)
    if configuration != 'Release' and (not pdb.is_file() or not pdb.stat().st_size):
        raise ValueError('The requested debug symbols were not generated; runtime was not switched')
    receipt_relative = f'Build/{configuration}/build-receipt.json'
    receipt = {
        'schema': 1, 'runtime': relative, 'runtime_sha256': digest(executable),
        'configuration': configuration, **snapshot,
        'pdb': pdb_relative if pdb.is_file() else None,
        'pdb_sha256': digest(pdb) if pdb.is_file() else None,
        'built_at_utc': datetime.now(timezone.utc).isoformat(),
        'external_dependency_sources': read_object(ROOT / 'project.json')['dependencies_root'],
        'external_dependency_libraries': read_object(ROOT / 'project.json')['dependency_build_root'],
        'game_started': False, 'game_acceptance_verified': False,
    }
    project = read_object(ROOT / 'project.json')
    project.update({
        'phase': 'project-engine', 'runtime_path': relative,
        'runtime_sha256': receipt['runtime_sha256'],
        'runtime_source_equivalence': 'project-source-snapshot',
        'runtime_revision': 'project-snapshot:' + snapshot['source_sha256'],
        'runtime_build_receipt': receipt_relative, 'new_executable_built': True,
        'game_acceptance_verified': False,
    })
    # Store provenance first; an interrupted manifest replacement keeps the old selection.
    write_json(project_child(receipt_relative), receipt)
    write_json(ROOT / 'project.json', project)
    return receipt


def verify_project_build(project, executable):
    expected = project_child(project['runtime_path'])
    if executable != expected or not expected.is_relative_to(project_child('Build')):
        raise ValueError('Project engine must be inside this project Build directory')
    if project.get('phase') != 'project-engine' or project.get('new_executable_built') is not True:
        raise ValueError('Project engine has not been built. Run build.bat first; external engines are not used.')
    if not expected.is_file():
        raise ValueError('Project engine is missing. Run build.bat to restore it.')
    receipt = read_object(project_child(project['runtime_build_receipt']))
    configuration = receipt.get('configuration')
    if (configuration not in CONFIGURATIONS
            or project['runtime_path'] != f'Build/{configuration}/inFAMOUS Second Son.exe'
            or project['runtime_build_receipt'] != f'Build/{configuration}/build-receipt.json'):
        raise ValueError('Invalid project build output or receipt path')
    if (receipt.get('schema') != 1 or receipt.get('runtime') != project['runtime_path']
            or receipt.get('runtime_sha256') != project['runtime_sha256']):
        raise ValueError('Build receipt does not match the selected engine')
    if source_snapshot()['source_sha256'] != receipt.get('source_sha256'):
        raise ValueError('Project C++ or build inputs changed. Run build.bat before starting the game.')
    return {
        'kind': 'project-build', 'source_checked': True,
        'source_sha256': receipt['source_sha256'], 'configuration': receipt['configuration'],
        'receipt': project['runtime_build_receipt'],
    }


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('snapshot', 'activate'))
    parser.add_argument('--configuration', choices=CONFIGURATIONS, default='RelWithDebInfo')
    parser.add_argument('--source-sha256')
    args = parser.parse_args()
    try:
        if args.action == 'snapshot':
            print(json.dumps(source_snapshot()))
        else:
            if not args.source_sha256:
                raise ValueError('The pre-build source fingerprint is required')
            receipt = activate(args.configuration, args.source_sha256)
            print('Activated project engine: ' + receipt['runtime'])
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, str(error) + '\n')
