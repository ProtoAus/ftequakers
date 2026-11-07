#!/usr/bin/env python3
"""Extract actual C grid traversal/insertion/unlink; verify empty-call elision.

The adapter records occupied-cell and raw entity-node order, not native trace_t
merging/publication. Empty World_ClipToLinks has no effects outside its loop.
Run outside native timing, with a fresh --out directory; preserves failed arms.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

HARNESS = r'''
#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define HAVE_CLIENT 1
#define CSQC_DAT 1
#define countof(a) (sizeof(a)/sizeof((a)[0]))
#define bound(lo,v,hi) ((v)<(lo)?(lo):((v)>(hi)?(hi):(v)))
typedef struct link_s { struct link_s *prev, *next; } link_t;
typedef struct wedict_s wedict_t;
typedef struct { link_t l; wedict_t *ed; } areagridlink_t;
typedef struct { int solid; float absmin[2], absmax[2]; } vars_t;
struct wedict_s { int entnum; vars_t *v; areagridlink_t gridareas[16]; };
typedef struct { int gridsize[2]; float gridbias[2], gridscale[2];
 areagridlink_t *gridareas, jumboarea, portallist; } world_t;
typedef struct { float boxmins[2], boxmaxs[2]; unsigned int type; } moveclip_t;
static struct { int ival; } csqc_emptygrid;
static world_t csqc_world;
static size_t areagridsequence;
#define SOLID_PORTAL 9
static void ClearLink(link_t *l) { l->next=l->prev=l; }
static void InsertLinkBefore(link_t *l, link_t *head) {
 l->next=head; l->prev=head->prev; head->prev->next=l; head->prev=l;
}
static void RemoveLink(link_t *l) { l->prev->next=l->next; l->next->prev=l->prev; }
/*MACRO*/
static int nvisits, busy[300], nbusy, payload[300], npayload;
static void World_ClipToLinks(world_t *w, areagridlink_t *node, moveclip_t *clip) {
 link_t *l; (void)clip; nvisits++;
 if (node->l.next!=&node->l)
  busy[nbusy++]=node==&w->jumboarea ? -1 : (int)(node-w->gridareas);
 for (l=node->l.next;l!=&node->l;l=l->next)
  payload[npayload++]=((areagridlink_t*)l)->ed->entnum;
}
/*FUNCTIONS*/
static void LinkInsertion(world_t *w, wedict_t *ent) {
/*INSERTION*/
}
static wedict_t ents[4];
static vars_t vars[4];
static areagridlink_t cells[256];
static void reset(void) {
 unsigned int i;
 memset(&csqc_world,0,sizeof(csqc_world)); memset(ents,0,sizeof(ents));
 memset(vars,0,sizeof(vars)); memset(cells,0,sizeof(cells));
 for(i=0;i<4;i++) { ents[i].entnum=(int)i; ents[i].v=&vars[i]; }
 csqc_world.gridareas=cells; csqc_world.gridsize[0]=csqc_world.gridsize[1]=16;
 csqc_world.gridscale[0]=csqc_world.gridscale[1]=1;
 for(i=0;i<256;i++) ClearLink(&cells[i].l);
 ClearLink(&csqc_world.jumboarea.l); ClearLink(&csqc_world.portallist.l);
 csqc_emptygrid.ival=1; nvisits=nbusy=npayload=0;
}
static void call(world_t *w, moveclip_t *clip) {
 size_t seq=areagridsequence;
 nvisits=nbusy=npayload=0; World_ClipToAllLinks(w,clip);
 assert(areagridsequence==seq+1);
}
static void compare(moveclip_t *clip) {
 int expected_busy[300], expected_payload[300], nb, np, nv;
 csqc_emptygrid.ival=0; call(&csqc_world,clip);
 nb=nbusy;np=npayload;nv=nvisits;
 memcpy(expected_busy,busy,sizeof(busy)); memcpy(expected_payload,payload,sizeof(payload));
 csqc_emptygrid.ival=1; call(&csqc_world,clip);
 assert(nb==nbusy && np==npayload && nvisits<=nv);
 for(int i=0;i<nb;i++) assert(expected_busy[i]==busy[i]);
 for(int i=0;i<np;i++) assert(expected_payload[i]==payload[i]);
 assert(nvisits==1+nb-(csqc_world.jumboarea.l.next!=&csqc_world.jumboarea.l));
}
int main(void) {
 moveclip_t clip={{0,0},{15,15},0};world_t other;
 reset(); compare(&clip); assert(nvisits==1);
 csqc_emptygrid.ival=0;call(&csqc_world,&clip);assert(nvisits==257);
 other=csqc_world;ClearLink(&other.jumboarea.l);
 csqc_emptygrid.ival=1;call(&other,&clip);assert(nvisits==257);
 /* Actual native insertion: shared cells, insertion order, first/later rows. */
 reset(); vars[1].solid=1;vars[1].absmin[0]=vars[1].absmin[1]=2;
 vars[1].absmax[0]=vars[1].absmax[1]=3;LinkInsertion(&csqc_world,&ents[1]);
 vars[3]=vars[1];LinkInsertion(&csqc_world,&ents[3]);
 for(unsigned int i=0;i<6;i++) {unsigned int flags[]={0,1,32,128,512,0x80000001u};clip.type=flags[i];compare(&clip);}
 assert(npayload==8 && payload[0]==1 && payload[1]==3);
 World_UnlinkEdict(&ents[1]);compare(&clip);assert(npayload==4);
 World_UnlinkEdict(&ents[3]);compare(&clip);assert(nvisits==1);
 /* Stationary query: activation, unlink, rebuild, jumbo and portal lists. */
 vars[1].absmin[0]=vars[1].absmin[1]=0;vars[1].absmax[0]=vars[1].absmax[1]=0;
 vars[3].absmin[0]=vars[3].absmin[1]=15;vars[3].absmax[0]=vars[3].absmax[1]=15;
 LinkInsertion(&csqc_world,&ents[1]);LinkInsertion(&csqc_world,&ents[3]);
 /* Native insertion sends the upper boundary to jumbo, before cell zero. */
 compare(&clip);assert(npayload==2 && busy[0]==-1 && busy[1]==0);
 /* A synthetic occupied final head independently exercises the loop bound. */
 World_UnlinkEdict(&ents[3]);InsertLinkBefore(&ents[3].gridareas[0].l,&cells[255].l);
 compare(&clip);assert(npayload==2 && busy[0]==0 && busy[1]==255);
 World_UnlinkEdict(&ents[1]);World_UnlinkEdict(&ents[3]);
 vars[3].solid=SOLID_PORTAL;LinkInsertion(&csqc_world,&ents[3]);
 assert(csqc_world.portallist.l.next==&ents[3].gridareas[0].l);compare(&clip);assert(nvisits==1);
 World_UnlinkEdict(&ents[3]);vars[3].solid=1;
 vars[3].absmin[0]=vars[3].absmin[1]=0;LinkInsertion(&csqc_world,&ents[3]);
 assert(csqc_world.jumboarea.l.next==&ents[3].gridareas[0].l);
 compare(&clip);assert(npayload==1 && busy[0]==-1);
 World_UnlinkEdict(&ents[3]);compare(&clip);assert(nvisits==1);
 /* Use the exact forward-head predicate, not a stronger backlink invariant. */
 reset();ents[1].gridareas[0].ed=&ents[1];ents[1].gridareas[0].l.next=&cells[0].l;
 cells[0].l.next=&ents[1].gridareas[0].l;compare(&clip);assert(npayload==1);
 reset();clip.boxmaxs[0]=clip.boxmaxs[1]=0;compare(&clip);assert(nvisits==1);
 puts("PASS acted empty-call elision, occupied-cell/node order, native link/unlink/portal/jumbo transitions");return 0;
}
'''


def function(text, signature):
    start = text.index(signature)
    return text[start:text.index('\n}', start) + 2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--cc', default='C:/msys64/ucrt64/bin/gcc.exe')
    args = parser.parse_args()
    if args.out.exists():
        parser.error('preserve existing evidence: ' + str(args.out))
    root = Path(__file__).resolve().parent.parent
    world = (root / 'engine/server/world.c').read_text()
    header = (root / 'engine/common/world.h').read_text()
    macro = re.search(r'#define CALCAREAGRIDBOUNDS.*?(?=\n#else)', header, re.S).group()
    traversal = function(world, 'static void World_ClipToAllLinks (')
    unlink = function(world[world.index('void World_UnlinkEdict (wedict_t *ent)\n{\n\tsize_t i;'):], 'void World_UnlinkEdict (')
    insertion = '\t// find the first node' + world.split('#ifdef USEAREAGRID\n\t// find the first node', 1)[1].split('\n#else', 1)[0]
    # Verify that the unchanged native callee has declarations only before its
    # forward-node loop: an empty call has no gameplay/state side effects.
    callee = function(world[world.index('#define SOLID_ISPHYSPROP'):], 'static void World_ClipToLinks (')
    prefix = callee.split('// touch linked edicts', 1)[0].split('{', 1)[1]
    assert re.fullmatch(r'\s*link_t\s*\*l,\s*\*next;\s*wedict_t\s*\*touch;\s*trace_t\s+trace;\s*', prefix), prefix
    assert 'for (l = node->l.next ; l != &node->l ; l = next)' in callee
    exact = unlink + '\n' + traversal
    diagnostic = ''
    if 'world_traceprofile_active' in exact:
        diagnostic = ('static int world_traceprofile_active;\n'
                      'static unsigned long long world_traceprofile_grid_skips, world_traceprofile_grid_cells;\n'
                      'static unsigned int world_traceprofile_grid_maxcells, world_traceprofile_grid_checks;\n')
    source = HARNESS.replace('/*MACRO*/', macro).replace('/*FUNCTIONS*/', diagnostic + exact).replace('/*INSERTION*/', insertion)
    mutations = {
        'inverted-empty-test': ('node->l.next == &node->l', 'node->l.next != &node->l'),
        'backlink-test': ('node->l.next == &node->l', 'node->l.prev == &node->l'),
        'other-world': ('w == &csqc_world', '(w == &csqc_world || 1)'),
        'disabled-guard': ('csqc_emptygrid.ival && w == &csqc_world', 'w == &csqc_world'),
        'missing-jumbo': ('World_ClipToLinks(w, &w->jumboarea, clip);', '(void)clip;'),
        'missing-sequence': ('areagridsequence++;', '(void)0;'),
        'reverse-fast-row': ('\t\t\t\tfor (g[1] = ming[1]; g[1] < maxg[1]; g[1]++)', '\t\t\t\tfor (g[1] = maxg[1]-1; g[1] >= ming[1]; g[1]--)'),
        'omit-last-fast-row': ('\t\t\t\tfor (g[1] = ming[1]; g[1] < maxg[1]; g[1]++)', '\t\t\t\tfor (g[1] = ming[1]; g[1] < maxg[1]-1; g[1]++)'),
    }
    args.out.mkdir(parents=True)
    env = dict(os.environ, PATH=str(Path(args.cc).parent) + os.pathsep + os.environ.get('PATH', ''))
    report = {'source_sha256': hashlib.sha256(exact.encode()).hexdigest(), 'callee_sha256': hashlib.sha256(callee.encode()).hexdigest(), 'arms': {}}
    for name in ['subject', *mutations]:
        arm = source
        if name != 'subject':
            old, new = mutations[name]
            assert arm.count(old) == 1, (name, arm.count(old))
            arm = arm.replace(old, new)
        src, exe = args.out / (name + '.c'), args.out / (name + '.exe')
        src.write_text(arm)
        subprocess.run([args.cc, '-std=c99', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare', '-O2', str(src), '-o', str(exe)], check=True, env=env)
        run = subprocess.run([str(exe)], capture_output=True, text=True, env=env)
        report['arms'][name] = {'returncode': run.returncode, 'stdout': run.stdout, 'stderr': run.stderr}
        (args.out / 'result.json').write_text(json.dumps(report, indent=2))
        assert (run.returncode == 0) == (name == 'subject'), report
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
