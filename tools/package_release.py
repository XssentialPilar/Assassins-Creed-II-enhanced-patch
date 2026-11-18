"""Package a tested Win32 Enhanced Patch build for GitHub Releases."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo


ROOT = Path(__file__).resolve().parents[1]
DLL_NAMES = ("d3d9.dll", "SDL3.dll")
LICENSES = (
    "licenses/SDL3.txt",
    "licenses/LLVM.txt",
    "licenses/MinGW-w64-runtime.txt",
    "licenses/winpthreads.txt",
)
PROMPT_ART = tuple(f"assets/prompts/{name}.svg" for name in
                   ("keyboard-mouse", "dualshock-3", "dualshock-4", "xbox", "dualsense"))
DOCUMENTATION = ("LICENSE", "README.md", "BUILD.md", "THIRD_PARTY.md", *PROMPT_ART, *LICENSES)


def checked_dll(path):
    data = path.read_bytes()
    pe = int.from_bytes(data[60:64], "little")
    header = data[pe:pe + 24]
    if data[:2] != b"MZ" or len(header) != 24 or header[:6] != b"PE\0\0\x4c\x01":
        raise ValueError(f"Expected an x86 PE file: {path.name}")
    if not struct.unpack_from("<H", header, 22)[0] & 0x2000:
        raise ValueError(f"Expected a DLL: {path.name}")
    return data


def package(version, build, prompt_preview=None):
    if not re.fullmatch(r"\d+\.\d+\.\d+(?:-[a-zA-Z0-9.]+)?", version):
        raise ValueError("Provide a release version such as 1.0.0.")
    files = {name: checked_dll(build / name) for name in DLL_NAMES}
    if prompt_preview is not None:
        files["input-prompts.png"] = prompt_preview.read_bytes()
        if not files["input-prompts.png"].startswith(b"\x89PNG\r\n\x1a\n"):
            raise ValueError("The prompt preview must be a PNG.")
    manifest = {"version": version, "sha256": {name: hashlib.sha256(content).hexdigest()
                                               for name, content in files.items()}}
    files["patch-manifest.json"] = (json.dumps(manifest, indent=2) + "\n").encode("utf-8")
    if (build / "SDL3-LICENSE.txt").read_text(encoding="utf-8") != (ROOT / LICENSES[0]).read_text(encoding="utf-8"):
        raise ValueError("The SDL notice in this build differs from the repository copy.")
    files.update((name, (ROOT / name).read_bytes()) for name in DOCUMENTATION)

    output = ROOT / "dist"
    output.mkdir(exist_ok=True)
    archive = output / f"AC2-Enhanced-Patch-v{version}-win32.zip"
    with ZipFile(archive, "w") as release:
        for name, content in files.items():
            release.writestr(ZipInfo(name), content, compress_type=ZIP_DEFLATED)
    with ZipFile(archive) as release:
        if release.namelist() != list(files) or any(release.read(name) != data for name, data in files.items()):
            raise ValueError("The archive contents differ from the tested build or documentation.")

    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    archive.with_suffix(".sha256").write_text(f"{digest}  {archive.name}\n", encoding="ascii", newline="\n")
    print(f"{digest}  {archive}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version")
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--prompt-preview", type=Path, help="PNG exported from this build's controller smoke test.")
    arguments = parser.parse_args()
    package(arguments.version, arguments.build_dir, arguments.prompt_preview)
