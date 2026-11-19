#!/usr/bin/env python3
"""Compile controlled checkbuiltin deprecation scopes; no native game/data access."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

PREAMBLE = '''#pragma target FTE
float(__variant builtin) checkbuiltin = #0:checkbuiltin;
__deprecated("query before calling") string(float n) probe = #0:fs_randomhex;
'''
BODIES = {
    'checked': 'void() checked = { local string s; if (checkbuiltin(probe)) s = probe(16); };\n',
    'unchecked': 'void() unchecked = { local string s; s = probe(16); };\n',
    'separate_scope': ('void() checked = { local string s; if (checkbuiltin(probe)) s = probe(16); };\n'
                       'void() unchecked = { local string s; s = probe(16); };\n'),
    'expired_scope': ('void() expired = { local string s; __using(probe) { '
                      'if (checkbuiltin(probe)) s = probe(16); } s = probe(16); };\n'),
}


def arm(qcc, path, body, werror=False):
    path.mkdir()
    (path/'probe.qc').write_text(('#pragma warning error F326\n' if werror else '')+PREAMBLE+body)
    (path/'progs.src').write_text('probe.dat\nprobe.qc\n')
    result = subprocess.run([str(qcc),'-O2','-Wall','-srcfile','progs.src'],cwd=path,
                            text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=20)
    (path/'compile.log').write_text(result.stdout)
    if werror:
        if result.returncode and 'Variable "probe" is deprecated' not in result.stdout:
            raise RuntimeError('Unexpected warnings-as-errors failure: '+str(path/'compile.log'))
        return result.returncode
    if result.returncode:
        raise RuntimeError('Control compile failed: '+str(path/'compile.log'))
    return len(re.findall(r'warning[^\n]*Variable "probe" is deprecated',result.stdout))


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--qcc',type=Path,required=True)
    p.add_argument('--regressed-qcc',type=Path,required=True,help='first alias repair, before scoped-warning repair')
    p.add_argument('--work-root',type=Path,required=True)
    args=p.parse_args()
    root=args.work_root.resolve()
    if not any((x/'OWNER.md').is_file() for x in (root,*root.parents)):
        p.error('--work-root needs existing ancestor OWNER.md')
    root.mkdir(parents=True,exist_ok=True)
    out=Path(tempfile.mkdtemp(prefix='qcc-warning-',dir=root))
    counts={}
    for label,qcc in [('regressed',args.regressed_qcc.resolve()),('fixed',args.qcc.resolve())]:
        counts[label]={name:arm(qcc,out/(label+'-'+name),body) for name,body in BODIES.items()}
    expected={'regressed':{'checked':1,'unchecked':1,'separate_scope':2,'expired_scope':1},
              'fixed':{'checked':0,'unchecked':1,'separate_scope':1,'expired_scope':1}}
    errors={
        'regressed_checked':arm(args.regressed_qcc.resolve(),out/'werror-regressed-checked',BODIES['checked'],True),
        'fixed_checked':arm(args.qcc.resolve(),out/'werror-fixed-checked',BODIES['checked'],True),
        'fixed_unchecked':arm(args.qcc.resolve(),out/'werror-fixed-unchecked',BODIES['unchecked'],True),
        'fixed_expired_scope':arm(args.qcc.resolve(),out/'werror-fixed-expired',BODIES['expired_scope'],True),
    }
    result={'ok':counts==expected and errors=={'regressed_checked':1,'fixed_checked':0,'fixed_unchecked':1,'fixed_expired_scope':1},
            'actual':counts,'expected':expected,'werror_exit_codes':errors,'artifacts':str(out),
            'qcc_sha256':hashlib.sha256(args.qcc.read_bytes()).hexdigest(),
            'regressed_qcc_sha256':hashlib.sha256(args.regressed_qcc.read_bytes()).hexdigest()}
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))
    return 0 if result['ok'] else 1


if __name__=='__main__':
    raise SystemExit(main())
