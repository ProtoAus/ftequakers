"""Patch 1467: fail if any engine object reaches a thread-local symbol through its plain address (.refptr).

GCC 16's native TLS on mingw took the address of a thread-local `pmove` through `.refptr.pmove` -- the
symbol's plain address, i.e. the TLS template in the image -- for indexed writes, so a worker thread's
velocity changes landed in a copy nobody reads. The engine keeps only thread-local POINTERS (pm_thread.h,
com_bih_ts.h); this checks the built objects stay that way.

usage: check_tls_refptr.py <release dir> [<release dir> ...]   (e.g. eng/engine/release)
exit 0 clean, 1 a .refptr to a thread-local symbol, 2 no objects found.
"""
import os, re, subprocess, sys

OBJDUMP = os.environ.get('OBJDUMP', r'C:\msys64\ucrt64\bin\objdump.exe')


def objdump(args, path):
    return subprocess.run([OBJDUMP] + args + [path], capture_output=True, text=True, errors='replace').stdout


def main(dirs):
    objs = []
    for d in dirs:
        for root, _, files in os.walk(d):
            objs += [os.path.join(root, f) for f in files if f.endswith('.o')]
    if not objs:
        print('no objects under', dirs)
        return 2
    tls = set()
    for o in objs:   # symbols defined in a .tls section (COFF: `objdump -h` index + 1 is the symbol's `sec`)
        secs = {int(m.group(1)) + 1 for m in re.finditer(r'^\s*(\d+)\s+\.tls\S*\s', objdump(['-h'], o), re.M)}
        if not secs:
            continue
        for m in re.finditer(r'^\[\s*\d+\]\(sec\s+(\d+)\).*\s(\S+)$', objdump(['-t'], o), re.M):
            if int(m.group(1)) in secs:
                tls.add(m.group(2))
    bad = 0
    for o in objs:
        for line in objdump(['-r'], o).splitlines():
            m = re.search(r'\.refptr\.(\S+)', line)
            if m and m.group(1) in tls:
                print(f'{o}: {line.strip()}')
                bad += 1
    print(f'{len(objs)} objects, {len(tls)} thread-local symbols ({", ".join(sorted(tls))}), {bad} plain-address uses')
    if not tls:
        print('no thread-local symbols found: the check could not measure (objdump format?)')
        return 2
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:] or ['.']))
