#!/usr/bin/env python3
"""Packs OpenLamina's release archives from the release workflow's build artifacts.

    scripts/ci/package.py --version VERSION --artifacts DIR --out DIR [--os windows linux] [--repository OWNER/NAME]
                          [--installer OpenLamina-Windows-Installer.exe] [--signer NAME]
    scripts/ci/package.py --version VERSION --artifacts DIR --windows-folder DIR [--repository OWNER/NAME]

DIR of --artifacts has a folder per build, named as the workflow's artifacts: windows-x64 (vrclient_x64.dll),
windows-x86 (vrclient.dll), linux-x64 and linux-x86 (vrclient.so each). Made in --out:

    OpenLamina-Windows-Portable.zip   OpenLamina/bin/vrclient_x64.dll and bin/vrclient.dll
    OpenLamina-Linux.tar.gz           OpenLamina/bin/linux64/vrclient.so and bin/vrclient.so, and install.sh
    OpenLamina-Source.tar.gz          this commit's source, submodules included
    OpenLamina-Windows-Installer.exe  with --installer: that file (scripts/ci/build-installer.ps1 builds it)
    SHA256SUMS.txt                    of the others
    RELEASE_NOTES.md                  the release's description

--windows-folder writes the Windows archive's OpenLamina folder into DIR instead, unpacked, for the installer.

Both binary archives also have an empty bin/version.txt (it keeps Steam from rewriting openvrpaths.vrpath while that
names the folder), the controller models in bin/rendermodels (scripts/rendermodels.py converts them), the licences of
OpenLamina and of what is built into it, and SOURCE.txt. Each archive's entries carry the commit's time, so the same
commit packs the same bytes.
"""

import argparse
import gzip
import hashlib
import io
import subprocess
import sys
import tarfile
import tempfile
import time
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import rendermodels  # noqa: E402

# OpenLamina's licence and those of what is built into the library or comes with it (the hand models, the controller
# models, glm, the OpenXR loader and its jsoncpp): source path -> name in the archive.
LICENCES = {
    "LICENSE.txt": "LICENSE.txt",
    "LICENSE_OPENVR.txt": "LICENSE_OPENVR.txt",
    "LICENSE_INIH.txt": "LICENSE_INIH.txt",
    "LICENCE_BUILD_UTIL.txt": "LICENCE_BUILD_UTIL.txt",
    "assets/LICENSE.txt": "LICENSE_assets.txt",
    "assets/notes.txt": "NOTES_assets.txt",
    "assets/controllers/LICENSE.md": "LICENSE_webxr-input-profiles.txt",
    "libs/glm/copying.txt": "LICENSE_glm.txt",
    "libs/openxr-sdk/LICENSE": "LICENSE_openxr-sdk.txt",
    "libs/openxr-sdk/LICENSES/LicenseRef-jsoncpp-public-domain.txt": "LICENSE_jsoncpp.txt",
}
# The Linux builds have libunwind built in (the crash backtrace).
LINUX_LICENCES = {"libs/libunwind/COPYING": "LICENSE_libunwind.txt"}
# The Linux installer, run from the unpacked folder (installer/install.sh --help).
LINUX_INSTALLER = "installer/install.sh"

# artifact folder, file in it -> path in the archive
BINARIES = {
    "windows": [
        ("windows-x64", "vrclient_x64.dll", "bin/vrclient_x64.dll"),
        ("windows-x86", "vrclient.dll", "bin/vrclient.dll"),
    ],
    "linux": [
        ("linux-x64", "vrclient.so", "bin/linux64/vrclient.so"),
        ("linux-x86", "vrclient.so", "bin/vrclient.so"),
    ],
}


def git(*args: str) -> str:
    return subprocess.run(["git", "-C", str(ROOT), *args], check=True, capture_output=True, text=True).stdout


def source_text(version: str, commit: str, repository: str) -> str:
    return (
        f"OpenLamina {version}, commit {commit}\n"
        f"From https://github.com/{repository}, a fork of OpenComposite (https://gitlab.com/znixian/OpenOVR).\n"
        "GPL-3.0-or-later (LICENSE.txt); the other licence files are those of what is built into it.\n"
        "Its source, submodules included, is OpenLamina-Source.tar.gz, which comes with this archive,\n"
        f"and https://github.com/{repository}/tree/{commit}\n"
        "Built by .github/workflows/build.yml: Release, warnings as errors; Windows with the C runtime linked in,\n"
        "Linux with the crash backtrace.\n"
    )


def binary_files(os_name: str, artifacts: Path, version: str, commit: str, repository: str) -> dict[str, tuple[bytes, int]]:
    """The binary archive's files: path -> (contents, mode)."""
    files: dict[str, tuple[bytes, int]] = {}
    for folder, name, target in BINARIES[os_name]:
        path = artifacts / folder / name
        if not path.is_file():
            sys.exit(f"{path} is missing: the {folder} build's artifact.")
        files[target] = (path.read_bytes(), 0o755)
    files["bin/version.txt"] = (b"", 0o644)
    for path, data in controller_models().items():
        files[f"bin/rendermodels/{path}"] = (data, 0o644)
    licences = LICENCES | (LINUX_LICENCES if os_name == "linux" else {})
    for source, target in licences.items():
        path = ROOT / source
        if not path.is_file():
            sys.exit(f"{path} is missing (are the submodules checked out?).")
        files[target] = (path.read_bytes(), 0o644)
    files["SOURCE.txt"] = (source_text(version, commit, repository).encode(), 0o644)
    if os_name == "linux":
        files["install.sh"] = ((ROOT / LINUX_INSTALLER).read_bytes(), 0o755)
    return files


def controller_models() -> dict[str, bytes]:
    """The controller models (scripts/rendermodels.py) from assets/controllers: path in bin/rendermodels -> contents"""
    with tempfile.TemporaryDirectory() as tmp:
        try:
            rendermodels.convert(ROOT / "assets" / "controllers", Path(tmp))
        except rendermodels.ConversionError as e:
            sys.exit(f"The controller models can't be converted: {e}")
        return {p.relative_to(tmp).as_posix(): p.read_bytes() for p in sorted(Path(tmp).rglob("*")) if p.is_file()}


def write_folder(out: Path, files: dict[str, tuple[bytes, int]]) -> None:
    """The archive's folder, unpacked: what the Windows installer installs."""
    if out.exists() and any(out.iterdir()):
        sys.exit(f"{out} isn't empty.")
    for path, (data, mode) in sorted(files.items()):
        target = out / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        if sys.platform != "win32":
            target.chmod(mode)


def directories(paths) -> list[str]:
    found = set()
    for path in paths:
        parts = path.split("/")[:-1]
        for i in range(1, len(parts) + 1):
            found.add("/".join(parts[:i]))
    return sorted(found)


def write_zip(out: Path, prefix: str, files: dict[str, tuple[bytes, int]], mtime: int) -> None:
    stamp = time.gmtime(mtime)[:6]
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in sorted(files):
            data, mode = files[path]
            info = zipfile.ZipInfo(f"{prefix}/{path}", date_time=stamp)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (0o100000 | mode) << 16
            archive.writestr(info, data)


def write_tar_gz(out: Path, add) -> None:
    """A gzipped tar without the time and file name a plain tarfile.open('w:gz') puts in the gzip header."""
    with out.open("wb") as raw, gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as gz:
        with tarfile.open(fileobj=gz, mode="w", format=tarfile.PAX_FORMAT) as archive:
            add(archive)


def tar_entry(name: str, mtime: int, mode: int, kind=tarfile.REGTYPE, size=0) -> tarfile.TarInfo:
    info = tarfile.TarInfo(name)
    info.type = kind
    info.mode = mode
    info.size = size
    info.mtime = mtime
    info.uid = info.gid = 0
    info.uname = info.gname = ""
    return info


def write_binary_tar(out: Path, prefix: str, files: dict[str, tuple[bytes, int]], mtime: int) -> None:
    def add(archive: tarfile.TarFile) -> None:
        archive.addfile(tar_entry(prefix, mtime, 0o755, tarfile.DIRTYPE))
        for folder in directories(files):
            archive.addfile(tar_entry(f"{prefix}/{folder}", mtime, 0o755, tarfile.DIRTYPE))
        for path in sorted(files):
            data, mode = files[path]
            archive.addfile(tar_entry(f"{prefix}/{path}", mtime, mode, size=len(data)), io.BytesIO(data))

    write_tar_gz(out, add)


def write_source_tar(out: Path, prefix: str, mtime: int) -> None:
    paths = sorted(p for p in git("ls-files", "--recurse-submodules", "-z").split("\0") if p)

    def add(archive: tarfile.TarFile) -> None:
        for path in paths:
            full = ROOT / path
            if full.is_symlink():
                info = tar_entry(f"{prefix}/{path}", mtime, 0o777, tarfile.SYMTYPE)
                info.linkname = str(Path(full).readlink()).replace("\\", "/")
                archive.addfile(info)
            elif full.is_file():
                mode = 0o755 if full.stat().st_mode & 0o111 and sys.platform != "win32" else 0o644
                data = full.read_bytes()
                archive.addfile(tar_entry(f"{prefix}/{path}", mtime, mode, size=len(data)), io.BytesIO(data))

    write_tar_gz(out, add)


def notes(version: str, commit: str, repository: str, sums: str, installer: bool, signer: str | None = None) -> str:
    setup_row = (
        "| **Windows** (most people) | `OpenLamina-Windows-Installer.exe` |\n"
        if installer
        else ""
    )
    signature = (
        f"If Windows says it protected your PC, choose *More info*, confirm the publisher is {signer}, then *Run anyway*."
        if signer
        else "It isn't signed yet, so if Windows says it protected your PC, choose *More info*, then *Run anyway*."
    )
    windows = (
        f"**Windows:** run `OpenLamina-Windows-Installer.exe`. No administrator needed. {signature} The OpenLamina app in "
        "the Start menu switches between OpenLamina and SteamVR, and updates OpenLamina.\n\n"
        if installer
        else ""
    )
    return f"""OpenLamina {version}, built from [{commit[:12]}](https://github.com/{repository}/commit/{commit}).

| You have | Download |
|---|---|
{setup_row}| **Linux** | `OpenLamina-Linux.tar.gz` |
| Windows, installing by hand or for one game | `OpenLamina-Windows-Portable.zip` |
| Source code | `OpenLamina-Source.tar.gz` |
| Checksums | `SHA256SUMS.txt` |

{windows}**Linux:** unpack the archive and run `OpenLamina/install.sh`. `install.sh --help` lists the rest: switching back, updating, uninstalling, and Steam launch options.

**By hand:** see the README's [Installing](https://github.com/{repository}#installing) section.

OpenLamina is a fork of [OpenComposite](https://gitlab.com/znixian/OpenOVR), GPL-3.0-or-later like it; each archive has its licences and a `SOURCE.txt`.

```
{sums}```
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--version", required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    output = parser.add_mutually_exclusive_group(required=True)
    output.add_argument("--out", type=Path)
    output.add_argument("--windows-folder", type=Path)
    parser.add_argument("--os", nargs="+", choices=["windows", "linux"], default=["windows", "linux"])
    parser.add_argument("--repository", default="feimos-dev/OpenLamina")
    parser.add_argument("--installer", type=Path, help="OpenLamina-Windows-Installer.exe, to put with the archives")
    parser.add_argument("--signer", help="who signed the installer and the DLLs, for the release notes")
    args = parser.parse_args()

    commit = git("rev-parse", "HEAD").strip()
    mtime = int(git("log", "-1", "--format=%ct").strip())
    if args.windows_folder:
        write_folder(args.windows_folder, binary_files("windows", args.artifacts, args.version, commit, args.repository))
        print(args.windows_folder)
        return
    args.out.mkdir(parents=True, exist_ok=True)

    made = []
    if "windows" in args.os:
        out = args.out / "OpenLamina-Windows-Portable.zip"
        write_zip(out, "OpenLamina", binary_files("windows", args.artifacts, args.version, commit, args.repository), mtime)
        made.append(out)
    if "linux" in args.os:
        out = args.out / "OpenLamina-Linux.tar.gz"
        write_binary_tar(out, "OpenLamina", binary_files("linux", args.artifacts, args.version, commit, args.repository), mtime)
        made.append(out)
    out = args.out / "OpenLamina-Source.tar.gz"
    write_source_tar(out, f"OpenLamina-{args.version}", mtime)
    made.append(out)
    if args.installer:
        out = args.out / "OpenLamina-Windows-Installer.exe"
        out.write_bytes(args.installer.read_bytes())
        made.append(out)

    sums = "".join(f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n" for path in made)
    (args.out / "SHA256SUMS.txt").write_text(sums, newline="\n")
    (args.out / "RELEASE_NOTES.md").write_text(notes(args.version, commit, args.repository, sums, bool(args.installer), args.signer), newline="\n")
    for path in made:
        print(f"{path} ({path.stat().st_size // 1024} KB)")


if __name__ == "__main__":
    main()
