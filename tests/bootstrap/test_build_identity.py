# SPDX-License-Identifier: GPL-2.0-or-later
"""Build selection safety using tiny PE-header fixtures; no compiler or game runs."""
from pathlib import Path
import json
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import build_identity
import project_paths


class BuildSelectionSafety(unittest.TestCase):
    def setUp(self):
        parent = ROOT / 'state/offline-tests'
        parent.mkdir(parents=True, exist_ok=True)
        temporary = tempfile.TemporaryDirectory(dir=parent)
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        for module in (build_identity, project_paths):
            replacement = patch.object(module, 'ROOT', self.root)
            replacement.start()
            self.addCleanup(replacement.stop)
        (self.root / 'src').mkdir()
        (self.root / 'cmake').mkdir()
        (self.root / 'src/main.cpp').write_text('int main() {}', encoding='utf-8')
        (self.root / 'CMakeLists.txt').write_text('# fixture', encoding='utf-8')
        (self.root / 'inFAMOUS Second Son.ico').write_bytes(b'icon fixture')
        (self.root / 'scripts').mkdir()
        for name in ('build.bat', 'scripts/build.ps1', 'scripts/build_tools.ps1', 'scripts/build_identity.py', 'scripts/tools.ps1'):
            (self.root / name).write_text('# build fixture', encoding='utf-8')
        self.manifest = self.root / 'project.json'
        self.manifest.write_text(json.dumps({
            'phase': 'project-engine-pending',
            'runtime_path': 'Build/RelWithDebInfo/inFAMOUS Second Son.exe',
            'new_executable_built': False,
            'dependencies_root': '../existing/externals',
            'dependency_build_root': '../existing/Build',
        }), encoding='utf-8')
        self.build = self.root / 'Build/RelWithDebInfo'
        self.build.mkdir(parents=True)
        # This validates header inspection only, never execution or real PDB compatibility.
        image = bytearray(70)
        image[:2] = b'MZ'
        struct.pack_into('<I', image, 60, 64)
        image[64:] = b'PE\0\0\x64\x86'
        self.executable = self.build / 'inFAMOUS Second Son.exe'
        self.executable.write_bytes(image)
        (self.build / 'inFAMOUS Second Son.pdb').write_bytes(b'symbol fixture')

    def activate(self):
        source = build_identity.source_snapshot()['source_sha256']
        return build_identity.activate('RelWithDebInfo', source)

    def test_success_selects_only_project_build_and_checks_future_source_edits(self):
        receipt = self.activate()
        project = project_paths.read_object(self.manifest)
        self.assertEqual(project['runtime_path'], 'Build/RelWithDebInfo/inFAMOUS Second Son.exe')
        self.assertEqual(project['runtime_sha256'], receipt['runtime_sha256'])
        self.assertFalse(project['game_acceptance_verified'])
        self.assertTrue(build_identity.verify_project_build(project, self.executable)['source_checked'])
        (self.root / 'src/main.cpp').write_text('int main() {return 1;}', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'changed'):
            build_identity.verify_project_build(project, self.executable)

    def test_pending_build_cannot_use_a_present_but_unverified_executable(self):
        project = project_paths.read_object(self.manifest)
        with self.assertRaisesRegex(ValueError, 'has not been built'):
            build_identity.verify_project_build(project, self.executable)

    def test_legacy_phase_does_not_allow_an_external_engine(self):
        project = project_paths.read_object(self.manifest)
        project.update(phase='bat-baseline', runtime_path='../../legacy.exe')
        with self.assertRaisesRegex(ValueError, 'escaped'):
            build_identity.verify_project_build(project, self.executable)

    def test_build_recipe_change_requires_a_rebuild(self):
        self.activate()
        project = project_paths.read_object(self.manifest)
        (self.root / 'scripts/build.ps1').write_text('# changed compiler options', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'changed'):
            build_identity.verify_project_build(project, self.executable)

    def test_source_changed_during_build_keeps_previous_selection(self):
        before = self.manifest.read_bytes()
        source = build_identity.source_snapshot()['source_sha256']
        (self.root / 'src/main.cpp').write_text('changed', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'changed during'):
            build_identity.activate('RelWithDebInfo', source)
        self.assertEqual(self.manifest.read_bytes(), before)

    def test_invalid_executable_keeps_previous_selection(self):
        before = self.manifest.read_bytes()
        self.executable.write_bytes(b'not an exe')
        with self.assertRaisesRegex(ValueError, 'Windows executable'):
            self.activate()
        self.assertEqual(self.manifest.read_bytes(), before)

    def test_missing_symbols_keeps_previous_selection(self):
        before = self.manifest.read_bytes()
        (self.build / 'inFAMOUS Second Son.pdb').unlink()
        with self.assertRaisesRegex(ValueError, 'debug symbols'):
            self.activate()
        self.assertEqual(self.manifest.read_bytes(), before)

    def test_project_build_cannot_point_outside_project(self):
        self.activate()
        project = project_paths.read_object(self.manifest)
        project['runtime_path'] = '../outside.exe'
        with self.assertRaisesRegex(ValueError, 'escaped'):
            build_identity.verify_project_build(project, self.executable)


if __name__ == '__main__':
    unittest.main()
