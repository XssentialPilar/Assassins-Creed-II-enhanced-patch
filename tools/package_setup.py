"""Package the tested Windows setup and its private patch DLL."""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile

from package_release import LICENSES

ROOT = Path(__file__).resolve().parents[1]


def package(version: str, executable: Path, patch: Path) -> None:
    if not re.fullmatch(r"\d+\.\d+\.\d+(?:-[a-zA-Z0-9.]+)?", version):
        raise ValueError("Provide a release version such as 1.1.0-preview.4.")
    output = ROOT / "dist"
    output.mkdir(exist_ok=True)
    archive = output / f"AC2-Enhanced-Patch-v{version}-Setup-win-x64.zip"
    setup_name = f"AC2-Enhanced-Patch-v{version}-Setup.exe"
    private_dll = executable.with_name("AC2EnhancedPatch.Payload.dll")
    config = executable.with_suffix(".exe.config")
    binaries = {setup_name: executable, private_dll.name: private_dll, setup_name + ".config": config}
    with ZipFile(patch) as payload:
        manifest = json.loads(payload.read("patch-manifest.json"))
        if manifest["version"] != version:
            raise ValueError("Setup and patch versions differ.")
        preview = payload.read("input-prompts.png")
        if hashlib.sha256(preview).hexdigest() != manifest["sha256"]["input-prompts.png"]:
            raise ValueError("Setup preview checksum does not match the patch.")
    subprocess.run([str(executable.resolve()), "--verify-package", str(patch.resolve())],
                   check=True, timeout=60, creationflags=subprocess.CREATE_NO_WINDOW)
    with ZipFile(archive, "w", compression=ZIP_DEFLATED) as release:
        for name, path in binaries.items():
            release.write(path, name)
        for name in ("README.md", "THIRD_PARTY.md", "docs/audio-outputs.md", *LICENSES):
            release.write(ROOT / name, name)
        release.write(ROOT / "LICENSE", "licenses/Enhanced-Patch-MIT.txt")
    with ZipFile(archive) as release:
        if release.testzip() is not None:
            raise ValueError("Setup archive validation failed.")
        for name, path in binaries.items():
            if release.read(name) != path.read_bytes():
                raise ValueError(f"The archive does not contain the verified {name}.")
    with archive.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    archive.with_suffix(".sha256").write_text(f"{digest}  {archive.name}\n", encoding="ascii", newline="\n")
    print(f"{digest}  {archive}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version")
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--patch", required=True, type=Path)
    args = parser.parse_args()
    package(args.version, args.exe, args.patch)
