"""Project-local launch preferences and supported rendering resolutions."""
from project_paths import ROOT, project_child, read_object, write_json

SETTINGS_PATH = project_child('state/play/launcher.json')
LANGUAGES = (('繁體中文（香港）', 10, 'chinese'),
             ('English', 1, 'english'), ('한국어（韩语）', 9, 'korean'))
BUTTONS = (('○ 确认 / Xbox B', True), ('× 确认 / Xbox A', False))
DISPLAY_MODES = (('全屏', 'fullscreen'), ('窗口', 'windowed'), ('无边框窗口', 'borderless'))
DISPLAY_CONFIG = {
    'fullscreen': (True, 'Fullscreen'),
    'windowed': (False, 'Windowed'),
    'borderless': (True, 'Fullscreen (Borderless)'),
}
# Width, height, existing correctness profile, mutually exclusive game patch.
RESOLUTION_CONFIG = {
    '720p': (1280, 720, '1080p', 'Native 1280x720 (render and output)'),
    '900p': (1600, 900, '1080p', 'Native 1600x900 (render and output)'),
    '1080p': (1920, 1080, '1080p', None),
    '1440p': (2560, 1440, 'pro4k', 'PS4 Pro mode: native 2560x1440 without checkerboard'),
    '1800p': (3200, 1800, 'pro4k', 'PS4 Pro mode: native 3200x1800 without checkerboard'),
    'pro4k': (3840, 2160, 'pro4k', 'PS4 Pro mode: native 3840x2160 without checkerboard'),
}
RESOLUTIONS = tuple((f'{width} × {height}（' +
                     ('4K' if key == 'pro4k' else key) +
                     (' / PS4 Pro）' if profile == 'pro4k' else '）'), key)
                    for key, (width, height, profile, _) in RESOLUTION_CONFIG.items())


def resolution_config(mode):
    if type(mode) is not str or mode not in RESOLUTION_CONFIG:
        raise ValueError('不支持的渲染分辨率。')
    return RESOLUTION_CONFIG[mode]


def available_languages():
    cache = ROOT / 'game/CUSA00309/art/cache'
    return [(name, code) for name, code, asset in LANGUAGES
            if (cache / f'lang_{asset}_text.xpps').is_file()]


def validate(settings):
    display_mode = settings.get('display_mode', 'fullscreen')
    resolution = settings.get('resolution', '1080p')
    resolution_config(resolution)
    if (settings.get('schema') not in (1, 2, 3)
            or type(display_mode) is not str or display_mode not in DISPLAY_CONFIG
            or type(settings.get('console_language')) is not int
            or settings['console_language'] not in {code for _, code in available_languages()}
            or type(settings.get('is_circle_enter')) is not bool
            or type(settings.get('show_fps')) is not bool):
        raise ValueError('启动设置无效，或所选语言资源不存在。')
    return {'schema': 3, 'display_mode': display_mode, 'resolution': resolution,
            **{key: settings[key] for key in ('console_language', 'is_circle_enter', 'show_fps')}}


def previous_resolution():
    # Migrate old preferences using the last persisted game configuration.
    path = project_child('state/play/user/custom_configs/CUSA00309.json')
    if path.is_file():
        gpu = read_object(path).get('GPU', {})
        for key, (width, height, _, _) in RESOLUTION_CONFIG.items():
            if (gpu.get('window_width'), gpu.get('window_height')) == (width, height):
                return key
    return '1080p'


def load_settings(mode='auto'):
    if SETTINGS_PATH.is_file():
        settings = read_object(SETTINGS_PATH)
        if 'resolution' not in settings:
            settings['resolution'] = previous_resolution()
    else:
        options = available_languages()
        if not options:
            raise ValueError('未找到游戏语言资源，请检查 game/CUSA00309。')
        base = project_child('state/baseline/user')
        current = project_child('state/play/user')
        global_path = current / 'config.json'
        if not global_path.is_file():
            global_path = base / 'config.json'
        game_path = current / 'custom_configs/CUSA00309.json'
        if not game_path.is_file():
            game_path = base / 'custom_configs/CUSA00309.json'
        global_config, game_config = read_object(global_path), read_object(game_path)
        general = dict(global_config.get('General', {}))
        general.update(game_config.get('General', {}))
        inputs = dict(global_config.get('Input', {}))
        inputs.update(game_config.get('Input', {}))
        language = general.get('console_language', options[0][1])
        if language not in {code for _, code in options}:
            language = options[0][1]
        settings = {'schema': 3, 'display_mode': 'fullscreen', 'resolution': previous_resolution(),
                    'console_language': language, 'is_circle_enter': inputs.get('is_circle_enter', False),
                    'show_fps': global_config.get('General', {}).get('show_fps_counter', False)}
    settings = validate(settings)
    if mode != 'auto':
        resolution_config(mode)
        settings['resolution'] = mode
    return settings


def save_selection(settings):
    settings = validate(settings)
    write_json(SETTINGS_PATH, settings)
    return settings


def apply_settings(config, settings):
    config.setdefault('General', {})['console_language'] = settings['console_language']
    config.setdefault('Input', {})['is_circle_enter'] = settings['is_circle_enter']
    fullscreen, mode = DISPLAY_CONFIG[settings.get('display_mode', 'fullscreen')]
    config.setdefault('GPU', {}).update(full_screen=fullscreen, full_screen_mode=mode)


def apply_resolution(config, mode):
    width, height, profile, _ = resolution_config(mode)
    config.setdefault('General', {})['neo_mode'] = profile == 'pro4k'
    config.setdefault('GPU', {}).update(window_width=width, window_height=height)
