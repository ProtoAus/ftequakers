#!/usr/bin/env python3
"""Read the actual Makefile's revision without compiling; old .git-dir mutant must fail."""
import argparse
import os
from pathlib import Path
import subprocess


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--work-root', type=Path, required=True)
    ap.add_argument('--make', default='C:/msys64/usr/bin/make.exe')
    a = ap.parse_args()
    root = Path(__file__).resolve().parents[1]
    assert (root / '.git').is_file(), 'this control requires an actual git worktree'
    a.work_root.mkdir(parents=True, exist_ok=False)
    (a.work_root / 'OWNER.md').write_text('Owned metadata-only Makefile subject/mutant; no compiler, game or deployment.\n')
    git = lambda *args: subprocess.check_output(['git', '-C', str(root), *args], text=True).strip()
    expected = f'git-{int(git("rev-list", "--count", "HEAD")) + 29}-{git("describe", "--long", "--always", "--dirty")}'
    actual = (root / 'engine/Makefile').read_text()
    mutant = actual.replace('cd $(BASE_DIR) && git rev-parse --git-dir >/dev/null 2>&1 &&',
                            'test -d $(BASE_DIR)/../.git && cd $(BASE_DIR) &&')
    assert mutant != actual
    env = dict(os.environ)
    for key in ('SVN_VERSION', 'SVNREVISION', 'MAKEFLAGS'):
        env.pop(key, None)
    env['PATH'] = 'C:/msys64/ucrt64/bin;C:/msys64/usr/bin;' + env.get('PATH', '')
    for name, source, wanted in [('subject', actual, expected), ('old-git-dir-mutant', mutant, '')]:
        f = a.work_root / (name + '.mk')
        f.write_text(source + '\n.PHONY: release-stamp-control\nrelease-stamp-control:\n\t@echo STAMP=$(SVN_VERSION)\n')
        result = subprocess.run([a.make, '--no-print-directory', '-s', '-C', str(root / 'engine'),
                                 '-f', str(f), 'release-stamp-control', 'FTE_TARGET=win64'],
                                env=env, capture_output=True, text=True)
        (a.work_root / (name + '.log')).write_text(result.stdout + result.stderr)
        assert result.returncode == 0, result.stderr
        stamps = [line.removeprefix('STAMP=') for line in result.stdout.splitlines() if line.startswith('STAMP=')]
        assert stamps == [wanted], (name, stamps, wanted)
        print(name, 'PASS', stamps)
    print('PASS: actual worktree revision; old directory-only check demonstrably loses it')


if __name__ == '__main__':
    main()
