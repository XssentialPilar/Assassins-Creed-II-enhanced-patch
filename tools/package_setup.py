"""Package the tested Windows setup with its runtime notices."""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile

from package_release import LICENSES, PROMPT_ART

ROOT = Path(__file__).resolve().parents[1]


def package(version: str, executable: Path, patch: Path) -> None:
    if not re.fullmatch(r"\d+\.\d+\.\d+(?:-[a-zA-Z0-9.]+)?", version):
        raise ValueError("Provide a release version such as 1.1.0-preview.4.")
    assets = json.loads((ROOT / "installer/obj/project.assets.json").read_text(encoding="utf-8-sig"))
    framework, = assets["project"]["frameworks"].values()
    versions = {entry["name"].lower(): entry["version"].strip("[]").split(",")[0]
                for entry in framework["downloadDependencies"]}
    package_folders = [Path(path) for path in assets["packageFolders"]]
    output = ROOT / "dist"
    output.mkdir(exist_ok=True)
    archive = output / f"AC2-Enhanced-Patch-v{version}-Setup-win-x64.zip"
    setup_name = f"AC2-Enhanced-Patch-v{version}-Setup.exe"
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
        release.write(executable, setup_name)
        release.writestr(f"input-prompts-v{version}.png", preview)
        for name in ("README.md", "THIRD_PARTY.md", *PROMPT_ART, *LICENSES):
            release.write(ROOT / name, name)
        release.write(ROOT / "LICENSE", "licenses/Enhanced-Patch-MIT.txt")
        for package_name, prefix in (
            ("microsoft.netcore.app.runtime.win-x64", "NET-Runtime"),
            ("microsoft.windowsdesktop.app.runtime.win-x64", "Windows-Desktop"),
        ):
            version_root = Path(package_name) / versions[package_name]
            source = next(folder / version_root for folder in package_folders if (folder / version_root).is_dir())
            for notice in sorted(source.iterdir()):
                if notice.is_file() and notice.name.upper().startswith(("LICENSE", "THIRD-PARTY-NOTICES")):
                    release.write(notice, f"licenses/{prefix}-{notice.name}")
    with ZipFile(archive) as release:
        if release.testzip() is not None:
            raise ValueError("Setup archive validation failed.")
        with release.open(setup_name) as build, executable.open("rb") as original:
            if hashlib.file_digest(build, "sha256").digest() != hashlib.file_digest(original, "sha256").digest():
                raise ValueError("The archive does not contain the tested installer.")
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
