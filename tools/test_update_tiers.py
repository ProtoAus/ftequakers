#!/usr/bin/env python3
"""Compile the actual native disk policy; exact public tiers path only."""
import argparse
import os
from pathlib import Path
import subprocess


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--work-root', type=Path, required=True)
    ap.add_argument('--cc', default='gcc')
    a = ap.parse_args()
    a.work_root.mkdir(parents=True, exist_ok=False)
    (a.work_root / 'OWNER.md').write_text('Owned native tier-policy compile/mutation fixtures. No game processes or player data.\n')
    source = Path(__file__).resolve().parents[1] / 'engine/common/qkupdate.c'
    text = source.read_text()
    policy = text[text.index('static qboolean QKU_ValidPath('):text.index('static qboolean QKU_FSInGame(')]
    header = '#include <stdio.h>\n#include <string.h>\n#include <strings.h>\n#include <stdbool.h>\ntypedef bool qboolean;\n#define MAX_OSPATH 1024\n#define Q_strncasecmp strncasecmp\n'
    test = r'''
int main(void) {
    const char *bad[] = {
        "ftesurf/maps/tiers-other.txt", "ftesurf/maps/tiers.txt/extra",
        "ftesurf/maps/TIERS.txt", "ftesurf/maps/a.bsp", "ftesurf/data/consent.txt",
        "ftesurf/ftesurf.cfg", "ftesurf/maps/../tiers.txt", "ftesurf/maps/tiers.txt.",
        "ftesurf/maps/tiers.txt:stream", "ftesurf/maps/tiers.txt ",
        "ftesurf/maps/zones/curated/surf_a.json", "hl2/pak01_dir.vpk"
    };
    unsigned int i;
    if (!QKU_FSManaged("ftesurf/maps/tiers.txt")) return 10;
    if (!QKU_FSManaged("ftesurf/menu.dat")) return 11;
    if (!QKU_FSManaged("ftesurf/maps/zones/local/surf_a.json")) return 12;
    for (i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
        if (QKU_FSManaged(bad[i])) { fprintf(stderr, "unexpected managed path: %s\n", bad[i]); return 20; }
    }
    puts("native policy: exact tier catalogue and existing assets accepted; 12 neighboring/player paths refused");
    return 0;
}
'''
    needle = '"ftesurf/maps/tiers.txt", '
    assert policy.count(needle) == 1
    env = dict(os.environ)
    if Path(a.cc).is_absolute():
        # MSYS toolchains need their sibling DLLs in the compiler child's PATH.
        env['PATH'] = str(Path(a.cc).parent) + os.pathsep + env.get('PATH', '')
    for name, code, expected in [('subject', policy, 0), ('prechange-mutant', policy.replace(needle, ''), 10)]:
        c = a.work_root / (name + '.c')
        binary = a.work_root / (name + '.exe')
        c.write_text(header + code + test)
        subprocess.run([a.cc, '-std=c99', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(binary)], check=True, env=env)
        result = subprocess.run([str(binary)], capture_output=True, text=True, env=env)
        print(name, 'exit', result.returncode, result.stdout.strip())
        assert result.returncode == expected, result.stderr
    print('PASS: subject accepts the new asset; restoring old policy acts and rejects it')


if __name__ == '__main__':
    main()
