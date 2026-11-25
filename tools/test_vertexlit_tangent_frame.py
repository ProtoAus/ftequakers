#!/usr/bin/env python3
"""Compile the literal unbumped GLSL frame helper as float C++; no GL/assets.

The adapter changes GLSL's inout parameters to references and spells its float
literals with C++'s f suffix. Native GLSL compilation/rendering are separate. --legacy-control models the old no-guard
path and must fail these numerical checks; it is not a legacy qualification gate.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SHADER = ROOT / 'plugins/hl2/glsl/vmt/vertexlit.glsl'


def helper(text):
    start = text.index('void VMT_UnbumpedFrame(')
    brace = text.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


CPP = r'''
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
using std::abs;
struct vec3 {
    float x, y, z;
    vec3(float a=0, float b=0, float c=0): x(a), y(b), z(c) {}
};
static float dot(vec3 a, vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
static vec3 cross(vec3 a, vec3 b) {
    return vec3(a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x);
}
static vec3 scale(vec3 a, float s) { return vec3(a.x*s,a.y*s,a.z*s); }
static vec3 add(vec3 a, vec3 b) { return vec3(a.x+b.x,a.y+b.y,a.z+b.z); }
static vec3 normalize(vec3 a) { return scale(a,1.0f/std::sqrt(dot(a,a))); }
static bool finite(vec3 a) { return std::isfinite(a.x)&&std::isfinite(a.y)&&std::isfinite(a.z); }
static unsigned checks=0, failures=0;
static void expect(bool ok, const char *what) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL %s\n",what); }
}
// LITERAL_HELPER
static void repaired(vec3 n, vec3 s, vec3 t, bool preserve) {
    const vec3 before_s=s, before_t=t;
    VMT_UnbumpedFrame(n,s,t);
    if (preserve) {
        expect(std::memcmp(&s,&before_s,sizeof(s))==0,"valid authored tangent changed");
        expect(std::memcmp(&t,&before_t,sizeof(t))==0,"valid handed binormal changed");
    }
    // These are the shader's unchanged operations immediately after the helper.
    s=normalize(s); t=normalize(t);
    expect(finite(s)&&finite(t),"nonfinite normalized frame");
    expect(abs(dot(n,s))<2e-5f && abs(dot(n,t))<2e-5f && abs(dot(s,t))<2e-5f,"nonorthogonal frame");
    const vec3 sun=normalize(vec3(.6f,.2f,.5f));
    const vec3 projected(dot(sun,s),dot(sun,t),dot(sun,n));
    const float lambert=projected.z/std::sqrt(dot(projected,projected));
    expect(std::isfinite(lambert)&&abs(lambert-dot(sun,n))<2e-5f,"sun differs from geometric-normal cosine");
    const vec3 eye=normalize(vec3(.2f,.8f,.5f));
    const vec3 ev(dot(eye,s),dot(eye,t),dot(eye,n));
    const vec3 reflected=add(add(scale(s,-ev.x),scale(t,-ev.y)),scale(n,ev.z));
    const vec3 direct=add(scale(eye,-1),scale(n,2*dot(eye,n)));
    expect(finite(reflected)&&abs(reflected.x-direct.x)<2e-5f&&abs(reflected.y-direct.y)<2e-5f&&abs(reflected.z-direct.z)<2e-5f,"reflection differs from geometric reflection");
}
int main() {
    // Identity and rotated normals; one rigid bone/instance rotation preserves
    // the same normal/frame identities. No skeletal runtime is simulated here.
    const vec3 normals[]={vec3(0,0,1),vec3(0,0,-1),vec3(1,0,0),normalize(vec3(1,2,3)),normalize(vec3(-3,1,2))};
    const float inf=std::numeric_limits<float>::infinity();
    const float nan=std::numeric_limits<float>::quiet_NaN();
    for (vec3 n:normals) {
        const vec3 axis=abs(n.z)<.999f?vec3(0,0,1):vec3(0,1,0);
        const vec3 s=normalize(cross(axis,n));
        const vec3 t=cross(n,s);
        repaired(n,s,t,true); repaired(n,s,scale(t,-1),true);
        repaired(n,vec3(),vec3(),false);
        repaired(n,n,vec3(),false);
        repaired(n,scale(n,-1),vec3(),false);
        const vec3 near=add(n,scale(s,1e-8f));
        repaired(n,near,cross(n,near),false);
        repaired(n,s,vec3(),false);
        repaired(n,scale(s,1e-25f),scale(t,1e-25f),false);
        repaired(n,vec3(inf,0,0),t,false);
        repaired(n,s,vec3(0,inf,0),false);
        repaired(n,vec3(nan,0,0),t,false);
    }
    std::printf("frame checks %u failures %u\n",checks,failures);
    return failures?1:0;
}
'''


def run(out, legacy):
    text = SHADER.read_text()
    body = helper(text)
    assert text.count('VMT_UnbumpedFrame(') == 2
    assert re.search(r'#ifndef BUMP\s+VMT_UnbumpedFrame\(n, s, t\);\s+#endif\s+s = normalize\(s\);\s+t = normalize\(t\);', text)
    assert re.search(r'#ifndef BUMP\s+(?://[^\n]*\n\s*)*void VMT_UnbumpedFrame', text)
    assert 'p1843_basis_repair' not in text, 'private diagnostic control must not ship'
    body = body.replace('inout vec3 s', 'vec3& s').replace('inout vec3 t', 'vec3& t')
    body = re.sub(r'(?<![\w.])(?:\d+\.\d*(?:[eE][+-]?\d+)?|\d+[eE][+-]?\d+)(?![\w.])', lambda m: m.group() + 'f', body)
    if legacy:
        body = 'void VMT_UnbumpedFrame(vec3, vec3&, vec3&) {}'
    cpp = CPP.replace('// LITERAL_HELPER', body)
    out.mkdir(parents=True, exist_ok=False)
    source = out / 'frame.cpp'
    source.write_text(cpp)
    compiler = os.environ.get('CXX', 'g++')
    results = []
    for opt in ('-O0', '-O2'):
        binary = out / ('frame' + opt)
        argv = [compiler, '-std=c++11', opt, '-Wall', '-Wextra', '-Werror', str(source), '-o', str(binary)]
        built = subprocess.run(argv, capture_output=True, text=True)
        (out / ('compile' + opt + '.stdout')).write_text(built.stdout)
        (out / ('compile' + opt + '.stderr')).write_text(built.stderr)
        assert built.returncode == 0, (argv, built.returncode, built.stderr)
        acted = subprocess.run([str(binary)], capture_output=True, text=True)
        (out / ('run' + opt + '.stdout')).write_text(acted.stdout)
        (out / ('run' + opt + '.stderr')).write_text(acted.stderr)
        print(opt, acted.stdout.strip())
        results.append({'opt': opt, 'exit': acted.returncode, 'argv': argv})
    report = {'kind': 'modelled old no-guard CONTROL' if legacy else 'literal production helper float controls', 'shader_sha256': hashlib.sha256(SHADER.read_bytes()).hexdigest(), 'helper_sha256': hashlib.sha256(helper(text).encode()).hexdigest(), 'results': results, 'native_glsl_or_visual_claim': False}
    (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    return int(any(x['exit'] != 0 for x in results))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, help='new owned directory retaining compiler/source/results')
    p.add_argument('--legacy-control', action='store_true')
    args = p.parse_args()
    if args.out is not None:
        return run(args.out, args.legacy_control)
    with tempfile.TemporaryDirectory(prefix='vmt-frame-') as tmp:
        return run(Path(tmp) / 'controls', args.legacy_control)


if __name__ == '__main__':
    raise SystemExit(main())
