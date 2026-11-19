"""Build the Win32 Direct3D 9 probe or native view patch."""
import argparse
import ctypes as ct
from itertools import groupby
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess


ROOT = Path(__file__).resolve().parents[1]
METHOD = re.compile(
    r'STDMETHOD(?:(?:_\(\s*(?P<type>\w+)\s*,\s*(?P<typed_name>\w+)\s*\))'
    r'|(?:\(\s*(?P<name>\w+)\s*\)))\s*\(\s*THIS_?\s*(?P<args>.*?)\)\s*PURE\s*;',
    re.S,
)


def embed_keyboard_png(out):
    """Keep the original RGBA PNG in the DLL. SDL decodes it on first use."""
    data = (ROOT / 'assets/prompts/keyboard-mouse.png').read_bytes()
    if data[:8] != b'\x89PNG\r\n\x1a\n' or struct.unpack('>II', data[16:24]) != (512, 512):
        raise ValueError('The keyboard keycap sheet must be a 512 × 512 PNG.')
    lines = [', '.join(f'0x{byte:02x}' for byte in data[i:i + 16]) for i in range(0, len(data), 16)]
    (out / 'keyboard_keycap.inc').write_text(
        'constexpr unsigned char KeyboardKeycapPng[] = {\n    ' + ',\n    '.join(lines) + '\n};\n',
        encoding='utf-8')


def generate_controller_sprites(out, executable):
    """Rasterize the audited SVGs at build time and embed their lossless tiles."""
    try:
        from PIL import Image
    except ImportError:
        raise SystemExit('Install Pillow to build the SVG controller prompts.') from None
    rasterizer = executable or shutil.which('resvg')
    if not rasterizer:
        raise SystemExit('Install resvg or pass --resvg with its executable path.')

    families = ('xbox', 'dualshock-3', 'dualshock-4', 'dualsense')
    tile_size, buttons = 64, 10
    runs, offsets = [], [0]
    for family in families:
        source = ROOT / 'assets/prompts' / f'{family}.svg'
        target = out / f'{family}.png'
        subprocess.run([str(rasterizer), str(source), str(target)], check=True)
        with Image.open(target) as sheet:
            if sheet.size != (buttons * tile_size, 2 * tile_size):
                raise ValueError(f'{family} uses an unexpected SVG view size.')
            for index in range(buttons * 2):
                left = index % buttons * tile_size
                top = index // buttons * tile_size
                sprite = sheet.crop((left, top, left + tile_size, top + tile_size)).convert('RGBA')
                bounds = sprite.getchannel('A').getbbox()
                if not bounds or min(bounds[0], bounds[1], tile_size - bounds[2], tile_size - bounds[3]) < 1:
                    raise ValueError(f'{family} sprite {index + 1} has an empty or clipped mark.')
                pixels = struct.iter_unpack('<I', sprite.tobytes('raw', 'BGRA'))
                for (color,), identical in groupby(pixels):
                    runs.append(f'{{0x{color:08x}u, {sum(1 for _ in identical)}}}')
                offsets.append(len(runs))

    source = '// Generated from SVG controller art by resvg, in ControllerFamily order.\n'
    source += f'constexpr unsigned PadSpriteSize = {tile_size};\n'
    source += f'constexpr unsigned PadSpriteButtons = {buttons};\n'
    source += 'constexpr unsigned PadSpriteOffsets[] = {\n    ' + ', '.join(map(str, offsets)) + '\n};\n'
    source += 'constexpr PadSpriteRun PadSpriteRuns[] = {\n'
    for index in range(0, len(runs), 6):
        source += '    ' + ', '.join(runs[index:index + 6]) + ',\n'
    (out / 'controller_sprites.inc').write_text(source + '};\n', encoding='utf-8')


def generate(header, interface, overrides, total):
    declaration = re.search(
        rf'DECLARE_INTERFACE_IID_\(\s*{interface}\s*,[^\n]+\)\s*\{{(.*?)\n\}};',
        header, re.S)
    if not declaration:
        raise ValueError(f'Could not find {interface} in d3d9.h.')
    methods = list(METHOD.finditer(declaration[1]))
    names = {method['typed_name'] or method['name'] for method in methods}
    if len(names) != total or not overrides <= names:
        raise ValueError(f'Unrecognized {interface} method layout.')
    lines = []
    for method in methods:
        name = method['typed_name'] or method['name']
        if name in overrides:
            continue
        parameters = []
        arguments = []
        signature = ' '.join(method['args'].split())
        for index, parameter in enumerate(signature.split(',') if signature else ()):
            parameter = parameter.strip()
            match = re.search(r'[\s*](\w+)$', parameter)
            argument = match[1] if match else f'arg{index}'
            parameters.append(parameter if match else f'{parameter} {argument}')
            arguments.append(argument)
        result = method['type'] or 'HRESULT'
        lines.append(f'    {result} STDMETHODCALLTYPE {name}({", ".join(parameters)}) noexcept override {{')
        lines.append(f'        return real_->{name}({", ".join(arguments)});')
        lines.append('    }')
    return '\n'.join(lines) + '\n'


def compile_movie_shader(out):
    compiler = ct.WinDLL(str(Path(os.environ['SystemRoot']) / 'System32' / 'd3dcompiler_47.dll'))
    compiler.D3DCompile.argtypes = [ct.c_void_p, ct.c_size_t, ct.c_char_p, ct.c_void_p, ct.c_void_p,
                                    ct.c_char_p, ct.c_char_p, ct.c_uint, ct.c_uint,
                                    ct.POINTER(ct.c_void_p), ct.POINTER(ct.c_void_p)]
    compiler.D3DCompile.restype = ct.c_long
    source = (ROOT / 'native/d3d9_movie_surround.hlsl').read_bytes()
    def take_blob(blob):
        if not blob:
            return b''
        vtable = ct.cast(blob, ct.POINTER(ct.POINTER(ct.c_void_p))).contents
        call = lambda index, result_type: ct.WINFUNCTYPE(result_type, ct.c_void_p)(vtable[index])(blob)
        data = ct.string_at(call(3, ct.c_void_p), call(4, ct.c_size_t))
        call(2, ct.c_ulong)
        return data

    for name, profile in (('vertex', b'vs_3_0'), ('pixel', b'ps_3_0')):
        shader, errors = ct.c_void_p(), ct.c_void_p()
        result = compiler.D3DCompile(source, len(source), b'd3d9_movie_surround.hlsl', None, None,
                                    name.encode('ascii'), profile, 0x8000, 0, ct.byref(shader), ct.byref(errors))
        compiled, diagnostic = take_blob(shader), take_blob(errors)
        if result < 0:
            raise RuntimeError(diagnostic.rstrip(b'\0').decode('utf8', errors='replace'))
        words = [word for (word,) in struct.iter_unpack('<I', compiled)]
        lines = ['    ' + ', '.join(f'0x{word:08x}' for word in words[i:i + 8]) + ','
                for i in range(0, len(words), 8)]
        (out / f'movie_surround_{name}.inc').write_text('\n'.join(lines) + '\n', encoding='ascii')


def build(native_view, fit_video, high_refresh, lean_trace, controller_sdk, resvg=None,
          experimental_audio=False):
    compiler = shutil.which('i686-w64-mingw32-clang++')
    if not compiler:
        raise SystemExit('Install or add the Win32 LLVM-MinGW compiler to PATH.')
    toolchain = Path(compiler).resolve().parent.parent
    header = (toolchain/'include/d3d9.h').read_text(encoding='utf8')
    variant = '-'.join(name for enabled, name in (
        (native_view, 'native-view'), (fit_video, 'video-fit'), (high_refresh, 'high-refresh'),
        (lean_trace, 'lean-trace'), (controller_sdk, 'controllers'),
        (experimental_audio, 'experimental-audio')) if enabled)
    out = ROOT/'build'/(variant or 'd3d9-probe')
    out.mkdir(parents=True, exist_ok=True)
    common = {'QueryInterface', 'AddRef', 'Release'}
    (out/'factory_forward.inc').write_text(generate(header, 'IDirect3D9',
        common | {'CreateDevice'}, 17), encoding='utf8')
    (out/'device_forward.inc').write_text(generate(header, 'IDirect3DDevice9', common | {
        'GetDirect3D', 'Reset', 'Present', 'SetViewport', 'SetScissorRect', 'SetRenderState',
        'SetRenderTarget', 'Clear', 'SetTransform', 'StretchRect', 'SetVertexShaderConstantF',
        'SetTexture', 'DrawPrimitive', 'DrawIndexedPrimitive', 'DrawIndexedPrimitiveUP', 'DrawPrimitiveUP',
    }, 119), encoding='utf8')
    options = ['-DAC2_NATIVE_VIEW'] if native_view else []
    if high_refresh:
        options.append('-DAC2_HIGH_REFRESH')
    if lean_trace:
        options.append('-DAC2_LEAN_TRACE')
    controller_sources = []
    controller_options = []
    controller_libs = []
    if controller_sdk:
        embed_keyboard_png(out)
        generate_controller_sprites(out, resvg)
        runtime = controller_sdk/'bin'/'SDL3.dll'
        license_file = controller_sdk.parent/'LICENSE.txt'
        if not runtime.is_file() or not (controller_sdk/'include'/'SDL3'/'SDL.h').is_file():
            raise SystemExit('Pass the i686-w64-mingw32 folder from the SDL3 MinGW SDK.')
        shutil.copyfile(runtime, out/'SDL3.dll')
        if license_file.is_file():
            shutil.copyfile(license_file, out/'SDL3-LICENSE.txt')
        controller_sources = [str(ROOT/'native'/name) for name in
                              ('controller_bridge.cpp', 'controller_prompts.cpp', 'dualsense_feedback.cpp',
                               'keyboard_prompts.cpp')]
        controller_options = ['-I', str(controller_sdk/'include'), '-I', str(out)]
        controller_libs = [str(controller_sdk/'lib'/'libSDL3.dll.a'), '-lgdi32', '-lcfgmgr32', '-lole32', '-luuid']
        options += ['-DAC2_CONTROLLER_PROMPTS', *controller_options, *controller_sources]
    if fit_video:
        compile_movie_shader(out)
        options += ['-DAC2_VIDEO_FIT', str(ROOT/'native/d3d9_video_fit.cpp')]
    audio_libs = ['-lole32', '-lksguid'] if experimental_audio else []
    if experimental_audio:
        options += ['-DAC2_EXPERIMENTAL_AUDIO', str(ROOT/'native/audio_channel_map.cpp')]
    subprocess.run([compiler, '-std=c++20', '-O2', '-fno-omit-frame-pointer', '-Wall', '-Wextra', '-Werror',
                    '-shared', '-static', *options,
                    '-I', str(out), str(ROOT/'native/d3d9_probe.cpp'), str(ROOT/'native/d3d9_probe.def'),
                    '-ldxguid', '-luuid', *controller_libs, *audio_libs, '-o', str(out/'d3d9.dll')],
                   cwd=ROOT, check=True)
    subprocess.run([compiler, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-static',
                    str(ROOT/'native/d3d9_probe_smoke.cpp'), '-ld3d9', '-luser32',
                    '-ldxguid', '-luuid', '-o', str(out/'ac2_display_smoke.exe')], cwd=ROOT, check=True)
    if native_view:
        view_options = ['-DAC2_HIGH_REFRESH'] if high_refresh else []
        subprocess.run([compiler, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-static',
                        *view_options, '-I', str(out), str(ROOT/'native/d3d9_view_smoke.cpp'), '-ldxguid', '-luuid',
                        '-o', str(out/'ac2_view_smoke.exe')], cwd=ROOT, check=True)
    if fit_video:
        subprocess.run([compiler, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-static',
                        '-I', str(out),
                        str(ROOT/'native/d3d9_video_smoke.cpp'), str(ROOT/'native/d3d9_video_fit.cpp'),
                        '-ld3d9', '-luser32', '-o', str(out/'ac2_video_smoke.exe')], cwd=ROOT, check=True)
    if controller_sdk:
        subprocess.run([compiler, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-static',
                        *controller_options, str(ROOT/'native/controller_smoke.cpp'), *controller_sources,
                        '-ld3d9', '-luser32', *controller_libs, '-o', str(out/'ac2_controller_smoke.exe')],
                       cwd=ROOT, check=True)
    print(out/'d3d9.dll')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-view', action='store_true', help='Enable the audited Steam view correction.')
    parser.add_argument('--fit-video', action='store_true', help='Fit the movie quad and extend its background.')
    parser.add_argument('--high-refresh', action='store_true',
                        help='Build the native view patch with the experimental high-refresh frame wait change.')
    parser.add_argument('--lean-trace', action='store_true',
                        help='Log timing and applied fixes without scanning the render passes.')
    parser.add_argument('--controller-sdk', type=Path, metavar='SDL3_I686_PATH',
                        help='Build the experimental controller and prompt integration with SDL3 for Win32.')
    parser.add_argument('--resvg', type=Path, help='Path to the resvg CLI for rasterizing controller artwork.')
    parser.add_argument('--experimental-audio', action='store_true',
                        help='Include the WASAPI and XAudio2 output bridge for the supported Steam build.')
    args = parser.parse_args()
    build(bool(args.native_view or args.high_refresh or args.controller_sdk), args.fit_video,
          args.high_refresh, args.lean_trace, args.controller_sdk, args.resvg, args.experimental_audio)
