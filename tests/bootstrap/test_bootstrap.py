from pathlib import Path
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import preflight
import metrics
import game_image
import project_paths
import launcher_settings
from unittest.mock import Mock, patch

class FakeImage:
    def read(self, offset, size):
        return bytes(size)

class PatchSafety(unittest.TestCase):
    def xml(self):
        return ET.fromstring('<Patch><TitleID><ID>CUSA00309</ID></TitleID></Patch>')
    def meta(self, root, value="01"):
        meta=ET.SubElement(root,"Metadata",Name="capacity",isEnabled="true",AppVer="01.07",AppElf="eboot.bin")
        lines=ET.SubElement(meta,"PatchList")
        ET.SubElement(lines,"Line",Type="bytes",Address="0x400010",Value=value,
                      OriginalHash=preflight.fnv1a(bytes.fromhex("00")))
    def test_conflicting_enabled_patches_rejected(self):
        root=self.xml()
        self.meta(root,"01")
        self.meta(root,"02")
        with self.assertRaisesRegex(ValueError,"Conflicting"):
            preflight.audit_xml(FakeImage(),root,{"TITLE_ID":"CUSA00309","APP_VER":"01.07"},"1080p")
    def test_version_mismatch_rejected(self):
        root=self.xml()
        self.meta(root)
        with self.assertRaisesRegex(ValueError,"incompatible"):
            preflight.audit_xml(FakeImage(),root,{"TITLE_ID":"CUSA00309","APP_VER":"01.00"},"1080p")

class MeasurementSafety(unittest.TestCase):
    def report(self, text, confirmations=None, mode="1080p"):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/"frames.csv"
            path.write_text(text)
            return metrics.summarize(path,0,1,mode,confirmations or {"scene":False})
    def test_high_fps_without_scene_confirmation_not_accepted(self):
        result=self.report("frame,elapsed_ns,interval_ns\n0,0,0\n1,10000000,10000000\n2,20000000,10000000\n")
        self.assertEqual(result["fps"],100)
        self.assertEqual(result["baseline_acceptance"],"not-verified")
        self.assertFalse(result["hardware_present_verified"])
    def test_partial_visual_confirmation_not_accepted(self):
        result=self.report("frame,elapsed_ns,interval_ns\n0,0,0\n1,10000000,10000000\n2,20000000,10000000\n", {"scene":True})
        self.assertEqual(result["baseline_acceptance"],"not-verified")
    def test_missing_frames_rejected(self):
        with self.assertRaisesRegex(ValueError,"contiguous"):
            self.report("frame,elapsed_ns,interval_ns\n0,0,0\n2,20000000,20000000\n")
    def test_timestamps_inconsistent_rejected(self):
        with self.assertRaisesRegex(ValueError,"disagree"):
            self.report("frame,elapsed_ns,interval_ns\n0,0,0\n1,20000000,10000000\n")

    def test_other_resolutions_do_not_pass_an_existing_baseline(self):
        result = self.report(
            "frame,elapsed_ns,interval_ns\n0,0,0\n1,10000000,10000000\n2,20000000,10000000\n",
            {key: True for key in ('scene', 'brightness', 'particles', 'resolution')}, '1440p')
        self.assertEqual(result['fps'], 100)
        self.assertIsNone(result['numeric_screen_pass'])
        self.assertFalse(result['baseline_target_applicable'])
        self.assertEqual(result['baseline_acceptance'], 'not-verified')

class InputSafety(unittest.TestCase):
    def test_truncated_binary_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "Truncated"):
            game_image.unpack("<Q", b"abc")

    def test_empty_patch_is_rejected(self):
        safety = PatchSafety()
        root = safety.xml()
        safety.meta(root, "")
        with self.assertRaisesRegex(ValueError, "Empty"):
            preflight.audit_xml(FakeImage(), root, {"TITLE_ID":"CUSA00309","APP_VER":"01.07"}, "1080p")

    def test_below_base_patch_is_rejected(self):
        safety = PatchSafety()
        root = safety.xml()
        safety.meta(root)
        root.find("./Metadata/PatchList/Line").set("Address", "0x3fffff")
        with self.assertRaisesRegex(ValueError, "below"):
            preflight.audit_xml(FakeImage(), root, {"TITLE_ID":"CUSA00309","APP_VER":"01.07"}, "1080p")

    def test_nonfinite_measurement_is_rejected(self):
        for start, end in ((float("nan"), 1), (0, float("inf"))):
            with self.assertRaisesRegex(ValueError, "Invalid"):
                metrics.summarize(Path("unused.csv"), start, end, "1080p", {})

    def test_project_escape_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "escaped"):
            project_paths.project_child("../outside")

    def test_failed_atomic_write_preserves_previous_config(self):
        with tempfile.TemporaryDirectory() as directory:
            with patch.object(project_paths, "ROOT", Path(directory).resolve()):
                target = Path(directory) / "state/config.json"
                project_paths.write_json(target, {"schema": 1})
                with patch.object(Path, "replace", side_effect=OSError("interrupted")):
                    with self.assertRaises(OSError):
                        project_paths.write_json(target, {"schema": 2})
                self.assertEqual(project_paths.read_object(target), {"schema": 1})
                self.assertEqual(list(target.parent.glob("*.pending")), [])


class DisplaySettings(unittest.TestCase):
    def test_legacy_preferences_default_to_fullscreen(self):
        with patch.object(launcher_settings, 'available_languages', return_value=[('English', 1)]):
            settings = launcher_settings.validate({'schema': 1, 'console_language': 1,
                                                   'is_circle_enter': False, 'show_fps': True})
        self.assertEqual(settings['schema'], 3)
        self.assertEqual(settings['display_mode'], 'fullscreen')
        self.assertEqual(settings['resolution'], '1080p')

    def test_display_modes_preserve_rendering_baseline(self):
        for mode, expected in launcher_settings.DISPLAY_CONFIG.items():
            config = {'GPU': {'window_width': 3840, 'window_height': 2160,
                              'readbacks_mode': 2, 'hdr_allowed': False}}
            launcher_settings.apply_settings(config, {'console_language': 1, 'is_circle_enter': False,
                                                      'display_mode': mode})
            self.assertEqual((config['GPU']['full_screen'], config['GPU']['full_screen_mode']), expected)
            self.assertEqual((config['GPU']['window_width'], config['GPU']['window_height']), (3840, 2160))
            self.assertEqual(config['GPU']['readbacks_mode'], 2)
            self.assertIs(config['GPU']['hdr_allowed'], False)

class ResolutionSettings(unittest.TestCase):
    def preferences(self):
        return {'schema': 2, 'console_language': 1, 'is_circle_enter': False,
                'show_fps': True, 'display_mode': 'borderless'}

    def test_old_preferences_inherit_the_previous_resolution(self):
        with patch.object(launcher_settings, 'SETTINGS_PATH') as path, \
                patch.object(launcher_settings, 'read_object', return_value=self.preferences()), \
                patch.object(launcher_settings, 'previous_resolution', return_value='pro4k'), \
                patch.object(launcher_settings, 'available_languages', return_value=[('English', 1)]):
            path.is_file.return_value = True
            settings = launcher_settings.load_settings()
        self.assertEqual(settings['resolution'], 'pro4k')
        self.assertEqual(settings['display_mode'], 'borderless')
        self.assertTrue(settings['show_fps'])

    def test_explicit_resolution_overrides_saved_preference_without_saving(self):
        preferences = dict(self.preferences(), schema=3, resolution='pro4k')
        with patch.object(launcher_settings, 'SETTINGS_PATH') as path, \
                patch.object(launcher_settings, 'read_object', return_value=preferences), \
                patch.object(launcher_settings, 'write_json') as write, \
                patch.object(launcher_settings, 'available_languages', return_value=[('English', 1)]):
            path.is_file.return_value = True
            settings = launcher_settings.load_settings('1080p')
        self.assertEqual(settings['resolution'], '1080p')
        self.assertEqual(preferences['resolution'], 'pro4k')
        write.assert_not_called()

    def test_unknown_resolution_is_rejected(self):
        with self.assertRaises(ValueError):
            launcher_settings.validate(dict(self.preferences(), resolution='3440x1440'))

    def test_resolution_selection_preserves_precision_and_sdr(self):
        for mode, size, pro in (('720p', (1280, 720), False), ('900p', (1600, 900), False),
                                ('1080p', (1920, 1080), False), ('1440p', (2560, 1440), True),
                                ('1800p', (3200, 1800), True), ('pro4k', (3840, 2160), True)):
            with self.subTest(mode=mode):
                config = {'GPU': {'readbacks_mode': 2, 'hdr_allowed': False}}
                launcher_settings.apply_resolution(config, mode)
                gpu = config['GPU']
                self.assertEqual((gpu['window_width'], gpu['window_height']), size)
                self.assertIs(config['General']['neo_mode'], pro)
                self.assertEqual(gpu['readbacks_mode'], 2)
                self.assertIs(gpu['hdr_allowed'], False)

    def resolution_xml(self):
        safety = PatchSafety()
        root = safety.xml()
        safety.meta(root)
        for _, _, _, name in launcher_settings.RESOLUTION_CONFIG.values():
            if name:
                safety.meta(root)
                root.findall('Metadata')[-1].set('Name', name)
        return root

    def test_only_selected_resolution_enabled_and_common_patch_preserved(self):
        for mode in launcher_settings.RESOLUTION_CONFIG:
            with self.subTest(mode=mode):
                root = self.resolution_xml()
                template = Mock()
                template.stat.return_value.st_size = 512
                with patch.object(preflight.ET, 'parse', return_value=ET.ElementTree(root)):
                    prepared = preflight.prepare_xml(template, mode)
                audited = preflight.audit_xml(FakeImage(), prepared,
                    {'TITLE_ID': 'CUSA00309', 'APP_VER': '01.07'}, mode)
                names = [entry['name'] for entry in audited]
                selected = launcher_settings.resolution_config(mode)[3]
                self.assertEqual(names, ['capacity'] + ([selected] if selected else []))

    def test_wrong_or_duplicate_resolution_patch_is_rejected(self):
        for duplicate in (False, True):
            with self.subTest(duplicate=duplicate):
                safety = PatchSafety()
                root = safety.xml()
                for _ in range(2 if duplicate else 1):
                    safety.meta(root)
                    root.findall('Metadata')[-1].set('Name',
                        launcher_settings.resolution_config('pro4k' if duplicate else '1440p')[3])
                with self.assertRaisesRegex(ValueError, 'selection'):
                    preflight.audit_xml(FakeImage(), root,
                        {'TITLE_ID': 'CUSA00309', 'APP_VER': '01.07'}, 'pro4k')

    def test_frozen_benchmark_rejects_other_resolutions(self):
        with self.assertRaisesRegex(ValueError, 'explicit'):
            preflight.inspect('720p', frozen=True)

if __name__=="__main__":
    unittest.main()
