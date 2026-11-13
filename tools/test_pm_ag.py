#!/usr/bin/env python3
"""Compile actual AG dry primitives with real engine headers and acting falsifiers.

No SDK/OpenAG source, game process, world shim or installed binary is used.
Requires an owned artifact directory; retains outputs including rejected controls.
Default-path byte controls are source preservation, not full trajectory parity.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--work-dir', type=Path, required=True)
    parser.add_argument('--baseline', required=True, help='Exact engine commit to preserve')
    args = parser.parse_args()
    out = args.work_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    cc = Path(args.cc)
    if cc.is_absolute():
        env['PATH'] = str(cc.parent) + os.pathsep + env.get('PATH', '')
    results = []

    def run(name, command, expected=0):
        process = subprocess.run(command, capture_output=True, text=True, env=env)
        (out / (name + '.stdout.txt')).write_text(process.stdout, encoding='utf-8')
        (out / (name + '.stderr.txt')).write_text(process.stderr, encoding='utf-8')
        results.append({'name': name, 'command': [str(x) for x in command],
                        'exit': process.returncode, 'expected': expected})
        (out / 'commands.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
        if expected == 'nonzero':
            assert process.returncode != 0, name + ' failed to expose the counterexample'
            assert 'FAIL line' in process.stderr, name + ' did not reach an acting assertion'
        else:
            assert process.returncode == expected, name + ': ' + process.stderr
        return process

    preserved = {}
    # Unchanged source is stronger here than an uncalled alternate-mode assertion:
    # no selector, shared layout, native pin, dispatch, or build registration changed.
    for file in ('engine/common/pmove.c', 'engine/common/pmove.h',
                 'engine/common/pm_source.c', 'engine/common/protocol.h',
                 'engine/server/sv_phys.c', 'engine/server/sv_user.c',
                 'engine/server/sv_ccmds.c', 'engine/client/cl_main.c',
                 'engine/client/cl_pred.c', 'engine/Makefile', 'CMakeLists.txt'):
        original = subprocess.run(['git', '-C', str(ROOT), 'show',
                                   args.baseline + ':' + file], capture_output=True, check=True).stdout
        # Checkouts may use CRLF; retain the raw hash but compare source text.
        assert original.replace(b'\r\n', b'\n') == (ROOT / file).read_bytes().replace(b'\r\n', b'\n'), file + ' changed'
        preserved[file] = sha(ROOT / file)

    flags = [args.cc, '-std=gnu99', '-Wall', '-Wextra', '-Werror',
             '-Wno-unused-parameter', '-ffp-contract=off',
             '-I', str(ROOT / 'engine/client'), '-I', str(ROOT / 'engine/common')]
    source = ROOT / 'engine/common/pm_ag.c'
    header = ROOT / 'engine/common/pm_ag.h'
    harness = ROOT / 'tools/test_pm_ag.c'
    obj = out / 'pm_ag.o'
    testobj = out / 'test_pm_ag.o'
    exe = out / 'test_pm_ag.exe'
    run('compile-ag', flags + ['-MMD', '-MF', str(out / 'pm_ag.d'), '-c', str(source), '-o', str(obj)])
    run('compile-harness', flags + ['-MMD', '-MF', str(out / 'test_pm_ag.d'), '-c', str(harness), '-o', str(testobj)])
    run('link', [args.cc, str(obj), str(testobj), '-lm', '-o', str(exe)])
    acting = run('acting', [str(exe)])
    print(acting.stdout, end='')

    text = source.read_text(encoding='utf-8')
    controls = (
        ('wrong-capped-product', '* wishspeed * dt * surfacefriction;', '* target * dt * surfacefriction;'),
        ('wrong-autobhop', 'return fresh && move->onground', 'return pressed && move->onground'),
        ('wrong-airborne-latch', 'move->jump_held = pressed;', 'move->jump_held = pressed && move->onground;'),
        ('wrong-clock', 'move->cmd.msec * 0.001', '0.01f'),
        ('wrong-jump-fixup', 'move->velocity[2] = sqrtf(2.0f * 800.0f * 45.0f);\n\t\tPMAg_HalfGravity(move, vars);',
         'move->velocity[2] = sqrtf(2.0f * 800.0f * 45.0f);'),
        ('wrong-takeoff-cap', 'move->onground = false;',
         'move->onground = false;\n\t\tif (move->velocity[0] > 544) move->velocity[0] *= 0.65f;'),
    )
    for name, old, new in controls:
        assert text.count(old) == (3 if name == 'wrong-clock' else 1), name + ' mutation anchor count changed'
        changed = out / (name + '.c')
        changed.write_text(text.replace(old, new), encoding='utf-8')
        control_exe = out / (name + '.exe')
        # The held-jump mutation intentionally leaves the edge local unused.
        control_flags = flags + (['-Wno-unused-variable'] if name == 'wrong-autobhop' else [])
        run(name + '-compile', control_flags + [str(changed), str(testobj), '-lm', '-o', str(control_exe)])
        run(name, [str(control_exe)], expected='nonzero')
        print('PASS acting falsifier rejected ' + name)

    receipt = {
        'baseline': args.baseline, 'source': sha(source), 'header': sha(header),
        'harness': sha(harness), 'runner': sha(Path(__file__)), 'executable': sha(exe),
        'preserved_default_sources': preserved, 'commands': results,
        'scope': 'Actual native primitives and analytic controls; no world, engine dispatch, oracle or installed-server parity',
    }
    (out / 'receipt.json').write_text(json.dumps(receipt, indent=2), encoding='utf-8')
    print('PASS unchanged default/pin/dispatch/build source controls; receipt: ' + str(out / 'receipt.json'))


if __name__ == '__main__':
    main()
