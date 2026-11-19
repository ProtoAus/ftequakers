#!/usr/bin/env python3
"""Read-only bytecode controls for two actual FTEQCC playtest regressions.

The retained failing programs are positive fault controls, not passing subjects.
No native VM, keys, recordings or player data are read by this tool.
"""
import argparse
import json
from pathlib import Path
import struct


class Progs:
    def __init__(self, path):
        data = Path(path).read_bytes()
        h = struct.unpack_from('<15i', data)
        if h[0] != 7:
            raise ValueError('This actual-output control expects full32 v7 progs')
        self.data, self.header = data, h
        self.strings = data[h[10]:h[10]+h[11]]
        self.globals = struct.unpack_from('<'+str(h[13])+'i', data, h[12])
        self.statements = [struct.unpack_from('<4i', data, h[2]+i*16) for i in range(h[3])]
        self.functions = [struct.unpack_from('<7i8B', data, h[8]+i*36) for i in range(h[9])]

    def text(self, offset):
        return self.strings[offset:self.strings.index(b'\0', offset)].decode(errors='replace')

    def function(self, name):
        return next((i, f) for i, f in enumerate(self.functions) if self.text(f[4]) == name)

    def body(self, name):
        _, f = self.function(name)
        start = f[0]
        end = min((g[0] for g in self.functions if g[0] > start), default=len(self.statements))
        return list(enumerate(self.statements[start:end], start))

    def kc_bounds(self):
        # All eight checked accesses in this actual KC_Refresh index KC_N40
        # arrays. The corrupted store-side bound is the FIFTH, not the first.
        return [(i,s[2],s[3]) for i,s in self.body('KC_Refresh') if s[0] == 211]

    def entropy_calls(self):
        index, builtin = self.function('fs_randomhex')
        if builtin[0] != 0:
            raise ValueError('Not the expected named #0 builtin declaration')
        result = []
        for i, s in self.body('SV_RandHex'):
            # CALL0..CALL8, CALL1H..CALL8H, including the actual CALL1H105.
            if s[0] in range(51,60) or s[0] in range(105,113):
                if 0 <= s[1] < len(self.globals) and self.globals[s[1]] == index:
                    result.append({'statement': i, 'callee_global': s[1], 'builtin_index': index})
        return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--bad-client', type=Path, required=True)
    ap.add_argument('--bad-server', type=Path, required=True)
    ap.add_argument('--fixed-client', type=Path, required=True)
    ap.add_argument('--fixed-server', type=Path, required=True)
    args = ap.parse_args()
    bc, bs, fc, fs = [Progs(p) for p in (args.bad_client,args.bad_server,args.fixed_client,args.fixed_server)]
    result = {
        'bad_kc_bounds': bc.kc_bounds(), 'fixed_kc_bounds': fc.kc_bounds(),
        'bad_entropy_calls': bs.entropy_calls(), 'fixed_entropy_calls': fs.entropy_calls(),
    }
    checks = {
        'original_array_fault_discriminates': [r[1] for r in result['bad_kc_bounds']] == [40,40,40,40,11,40,40,40],
        'fixed_array_bounds_remain_40': [r[1] for r in result['fixed_kc_bounds']] == [40]*8,
        'original_builtin_alias_fault_discriminates': not result['bad_entropy_calls'],
        'fixed_entropy_invokes_genuine_builtin': len(result['fixed_entropy_calls']) == 1,
    }
    result.update(ok=all(checks.values()), checks=checks)
    print(json.dumps(result, indent=2))
    return 0 if result['ok'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
