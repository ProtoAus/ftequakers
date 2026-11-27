#!/usr/bin/env python3
"""Run the headless client with small, disposable startup fixtures.

Usage: python3 tools/test_startup_files.py <client> [--logs <directory>]
Run inside a network namespace if the host's network must be unavailable.
No installed game or assets are used.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import zipfile

READY = "STARTUP_PROBE_READY"
MARKER = "STARTUP_PROBE_"


def marker(name):
    return f"cfg_save_auto 0\necho {MARKER}{name}\n"


CONFIG = marker("SETTINGS")
ROOT = marker("ROOT_DEFAULT")
PREFERRED = marker("PREFERRED_DEFAULT")
FOREIGN = marker("FOREIGN")
AUTOEXEC = marker("AUTOEXEC")
CASES = (
    ("ordinary", {"default.cfg": ROOT, "cfg/default.cfg": PREFERRED,
                  "config.cfg": CONFIG}, ["PREFERRED_DEFAULT", "SETTINGS"]),
    ("addon-quake", {"cfg/default.cfg": PREFERRED, "config.cfg": CONFIG,
                     "downloads/quake.rc": FOREIGN}, ["PREFERRED_DEFAULT", "SETTINGS"]),
    ("addon-hexen", {"default.cfg": ROOT, "config.cfg": CONFIG,
                     "downloads/hexen.rc": FOREIGN}, ["ROOT_DEFAULT", "SETTINGS"]),
    ("addon-q3", {"cfg/default.cfg": PREFERRED, "config.cfg": CONFIG,
                  "downloads/q3config.cfg": FOREIGN}, ["PREFERRED_DEFAULT", "SETTINGS"]),
    ("addon-cfg-default", {"default.cfg": ROOT, "config.cfg": CONFIG,
                           "downloads/cfg/default.cfg": FOREIGN}, ["ROOT_DEFAULT", "SETTINGS"]),
    ("local-quake", {"quake.rc": marker("QUAKE"), "hexen.rc": marker("HEXEN"),
                     "default.cfg": ROOT}, ["QUAKE"]),
    ("local-hexen", {"hexen.rc": marker("HEXEN"), "default.cfg": ROOT}, ["HEXEN"]),
    ("packaged-quake", {"local.pk3": {"quake.rc": marker("QUAKE")}, "default.cfg": ROOT}, ["QUAKE"]),
    ("named-settings", {"cfg/default.cfg": PREFERRED, "startup_custom.cfg": CONFIG,
                        "downloads/quake.rc": FOREIGN}, ["PREFERRED_DEFAULT", "SETTINGS"]),
    ("named-both-addon", {"cfg/default.cfg": PREFERRED, "startup_custom.cfg": CONFIG,
                          "downloads/config.cfg": FOREIGN, "downloads/q3config.cfg": FOREIGN},
     ["PREFERRED_DEFAULT", "SETTINGS"]),
    ("local-q3", {"q3config.cfg": marker("Q3"), "config.cfg": CONFIG,
                  "default.cfg": ROOT}, ["ROOT_DEFAULT", "Q3"]),
    ("autoexec-addon-root", {"cfg/default.cfg": PREFERRED, "autoexec.cfg": AUTOEXEC,
                            "downloads/default.cfg": FOREIGN}, ["PREFERRED_DEFAULT", "AUTOEXEC"]),
    ("autoexec-preferred", {"cfg/default.cfg": PREFERRED, "autoexec.cfg": AUTOEXEC},
     ["PREFERRED_DEFAULT", "AUTOEXEC"]),
    ("autoexec-addon-only", {"cfg/default.cfg": PREFERRED, "downloads/autoexec.cfg": FOREIGN},
     ["PREFERRED_DEFAULT"]),
)


def run(binary, root, files, mainconfig=None):
    manifest = root / "probe.fmf"
    manifest.write_text('FTEMANIFEST 1\nGAME "StartupProbe"\nBASEGAME "probe"\nDISABLEHOMEDIR 1\n'
                        '-set vid_renderer headless\n-set cfg_save_auto 0\n'
                        + (f'MAINCONFIG {mainconfig}\n' if mainconfig else ''), encoding="utf-8")
    for name, data in files.items():
        path = root / "probe" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(data, dict):
            with zipfile.ZipFile(path, "w") as pack:
                for entry, script in data.items():
                    pack.writestr(entry, script)
        else:
            path.write_text(data, encoding="utf-8")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({k: str(root / "home") for k in ("HOME", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME")})
    result = subprocess.run(
        [str(binary), "-basedir", str(root), "-manifest", str(manifest), "-nohome", "-nosound", "-nojoy",
         "+set", "vid_renderer", "headless", "+set", "cfg_save_auto", "0", "+echo", READY, "+quit"],
        cwd=root, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        encoding="utf-8", errors="replace", timeout=15, check=False,
    )
    lines = [line.strip() for line in result.stdout.splitlines()]
    marks = [line.removeprefix(MARKER) for line in lines if line.startswith(MARKER) and line != READY]
    return result, READY in lines, marks


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("client", type=Path)
    ap.add_argument("--logs", type=Path)
    args = ap.parse_args()
    binary = args.client.resolve(strict=True)
    if args.logs:
        args.logs.mkdir(parents=True, exist_ok=True)
    failed = False
    with tempfile.TemporaryDirectory(prefix="startup-files-") as tmp:
        for label, files, expected in CASES:
            root = Path(tmp) / label
            root.mkdir()
            result, ready, marks = run(binary, root, files, "startup_custom" if label.startswith("named-") else None)
            if args.logs:
                (args.logs / f"{label}.log").write_text(result.stdout, encoding="utf-8")
            ok = result.returncode == 0 and ready and marks == expected
            print(f"{label}: {'PASS' if ok else 'FAIL'} (exit {result.returncode}, ready={ready}, "
                  f"markers={marks}, expected={expected})", flush=True)
            failed |= not ok
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
