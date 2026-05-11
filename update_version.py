#!/usr/bin/env python3
"""Version management script. Single entry point for updating pak.json and derived files."""

import io
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).parent
PAK = ROOT / "pak.json"
APP_VERSION = ROOT / "state/app_version.txt"
SELFUPDATE_H = ROOT / "src/selfupdate.h"
QR_CODE_H = ROOT / "src/qr_code_data.h"
QR_CODE_PNG = ROOT / "src/qr-code.png"


def load_pak():
    with open(PAK) as f:
        return json.load(f)


def save_pak(pak):
    with open(PAK, "w") as f:
        json.dump(pak, f, indent=2)
        f.write("\n")


def latest_version(pak):
    return next(iter(pak["changelog"]))


def normalize_version(v):
    return v if v.startswith("v") else f"v{v}"


def cmd_latest(pak):
    v = latest_version(pak)
    print(f"{v}: {pak['changelog'][v]}")


def cmd_list(pak):
    for v, note in pak["changelog"].items():
        print(f"{v}: {note}")


def cmd_update(pak, args):
    if not args:
        print("usage: update [<version>] <text>", file=sys.stderr)
        sys.exit(1)

    # If first arg looks like a version key present in changelog, use it
    if args[0] in pak["changelog"]:
        version, text = args[0], " ".join(args[1:])
        if not text:
            print("usage: update [<version>] <text>", file=sys.stderr)
            sys.exit(1)
    else:
        version, text = latest_version(pak), " ".join(args)

    pak["changelog"][version] = text
    save_pak(pak)
    print(f"Updated {version}: {text}")


def cmd_create(pak, args):
    if len(args) < 2:
        print("usage: create <version> <text>", file=sys.stderr)
        sys.exit(1)

    version = normalize_version(args[0])
    text = " ".join(args[1:])

    if version in pak["changelog"]:
        print(f"Version {version} already exists. Use 'update' to change its note.", file=sys.stderr)
        sys.exit(1)

    pak["version"] = version
    pak["changelog"] = {version: text, **pak["changelog"]}
    save_pak(pak)
    print(f"pak.json -> {version}: {text}")

    APP_VERSION.write_text(version)
    print(f"state/app_version.txt -> {version}")

    _update_selfupdate_h(pak["repo_url"])
    _update_qr_code_h(pak["repo_url"])


def cmd_remove(pak, args):
    if not args:
        print("usage: remove <version>", file=sys.stderr)
        sys.exit(1)

    version = args[0]
    if version not in pak["changelog"]:
        print(f"Version {version} not found in changelog.", file=sys.stderr)
        sys.exit(1)

    note = pak["changelog"][version]
    confirm = input(f'Remove {version}: "{note}". Type "Remove" to confirm: ')
    if confirm != "Remove":
        print("Aborted.")
        return

    was_current = pak["version"] == version
    del pak["changelog"][version]

    if was_current:
        if not pak["changelog"]:
            print("No versions left after removal.", file=sys.stderr)
            sys.exit(1)
        new_version = next(iter(pak["changelog"]))
        pak["version"] = new_version
        APP_VERSION.write_text(new_version)
        print(f"Current version updated to {new_version}")

    save_pak(pak)
    print(f"Removed {version}.")


def _update_selfupdate_h(repo_url):
    match = re.search(r"github\.com/(.+?)(?:\.git)?$", repo_url.rstrip("/"))
    if not match:
        print(f"ERROR: cannot parse GitHub repo from {repo_url}", file=sys.stderr)
        sys.exit(1)
    owner_repo = match.group(1)

    content = SELFUPDATE_H.read_text()
    content = re.sub(
        r'(#define APP_GITHUB_REPO\s+)"[^"]+"',
        f'\\1"{owner_repo}"',
        content,
    )
    SELFUPDATE_H.write_text(content)
    print(f"src/selfupdate.h -> {owner_repo}")


def _update_qr_code_h(repo_url):
    if QR_CODE_H.exists():
        existing = QR_CODE_H.read_text()
        if f"// Embedded QR code PNG for {repo_url}" in existing:
            print(f"src/qr_code_data.h already encodes {repo_url}, skipping")
            return

    try:
        import qrcode
    except ImportError:
        print("ERROR: qrcode not installed (pip install qrcode[pil])", file=sys.stderr)
        sys.exit(1)

    qr = qrcode.QRCode(
        error_correction=qrcode.constants.ERROR_CORRECT_M,
        box_size=7,
        border=2,
    )
    qr.add_data(repo_url)
    qr.make(fit=True)
    img = qr.make_image().convert("RGB")
    w, h = img.size

    img.save(QR_CODE_PNG)

    if shutil.which("pngcrush"):
        with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as tmp:
            tmp_path = Path(tmp.name)
        subprocess.run(["pngcrush", "-brute", "-noreduce", str(QR_CODE_PNG), str(tmp_path)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if tmp_path.exists() and tmp_path.stat().st_size > 0:
            before = QR_CODE_PNG.stat().st_size
            shutil.move(str(tmp_path), QR_CODE_PNG)
            after = QR_CODE_PNG.stat().st_size
            print(f"pngcrush: {before} -> {after} bytes ({before - after} saved)")
        else:
            tmp_path.unlink(missing_ok=True)

    data = QR_CODE_PNG.read_bytes()

    lines = []
    for i in range(0, len(data), 12):
        lines.append("  " + ", ".join(f"0x{b:02x}" for b in data[i:i+12]))

    header = (
        "#ifndef QR_CODE_DATA_H\n"
        "#define QR_CODE_DATA_H\n"
        "\n"
        f"// Embedded QR code PNG for {repo_url}\n"
        f"// Generated from qr-code.png ({w}x{h} pixels)\n"
        "static const unsigned char qr_code_png[] = {\n"
        + ",\n".join(lines) + "\n"
        "};\n"
        f"static const unsigned int qr_code_png_len = {len(data)};\n"
        "\n"
        "#endif // QR_CODE_DATA_H\n"
    )
    QR_CODE_H.write_text(header)
    print(f"src/qr_code_data.h -> {w}x{h}, {len(data)} bytes")


USAGE = """\
usage: update_version.py [command] [args]

Commands:
  (none)               Show the current version and its changelog note
  latest               Same as above
  list                 List all versions and changelog notes
  create <ver> <text>  Add a new version entry and update derived files
  update [<ver>] <text>  Edit a changelog note (defaults to latest version)
  remove <ver>         Remove a version entry
  help, -h, --help     Show this help message
"""


def cmd_help():
    print(USAGE, end="")


COMMANDS = {
    "latest": lambda pak, args: cmd_latest(pak),
    "list":   lambda pak, args: cmd_list(pak),
    "update": cmd_update,
    "create": cmd_create,
    "remove": cmd_remove,
    "help":   lambda pak, args: cmd_help(),
}


def main():
    pak = load_pak()
    args = sys.argv[1:]

    if args and args[0] in ("-h", "--help"):
        cmd_help()
        return

    if not args or args[0] not in COMMANDS:
        cmd_latest(pak)
        return

    COMMANDS[args[0]](pak, args[1:])


if __name__ == "__main__":
    main()
