#!/usr/bin/env python3
"""Compile actual model-wrapper/legacy parser functions against native state types.

Literal prior-source inputs expose each residual independently. This does not
qualify AG replay/held models or replace the native pm_selftest.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile
from test_sourcepractice_ground import function
ROOT=Path(__file__).resolve().parents[1]
MAIN=r'''
#undef snprintf /* Native headers redirect it; only fixture string construction uses libc. */
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void) {
 pmsourcestate_t s;
 const char *base="surfacefriction=1 ducktime=0 ducking=0 ducked=0 msec_carry=0 oldbuttons=0 stamina=0 rampoff=0 boardcount=0 rampcontact=0 srcladder=0 ladnx=0 ladny=0 ladnz=0";
 char full[1024],text[2048];
 snprintf(full,sizeof(full),"%s wjtime=0 wjx=50 wjy=-5 agwaterjump=1",base);
 CHECK(SV_PMStateParseForModel(base,PHYSMODE_SOURCE,&s));
 CHECK(SV_PMStateParse(full,&s)); /* legacy compatibility is independently retained */
 if(__SOURCE__) {
  CHECK(!SV_PMStateParseForModel(full,PHYSMODE_SOURCE,&s));
  snprintf(text,sizeof(text),"%s xagwaterjump=1",base);
  CHECK(SV_PMStateParseForModel(text,PHYSMODE_SOURCE,&s));
  snprintf(text,sizeof(text),"xagwaterjump=1 %s",full);
  CHECK(!SV_PMStateParseForModel(text,PHYSMODE_SOURCE,&s));
 }
 if(__TUPLE__) {
  snprintf(text,sizeof(text),"%s agwaterjump=1",base);
  CHECK(!SV_PMStateParseForModel(text,PHYSMODE_AG,&s));
  snprintf(text,sizeof(text),"%s wjtime=0 agwaterjump=1",base);
  CHECK(!SV_PMStateParseForModel(text,PHYSMODE_AG,&s));
  snprintf(text,sizeof(text),"%s wjtime=bad wjx=0 wjy=0 agwaterjump=1",base);
  CHECK(!SV_PMStateParseForModel(text,PHYSMODE_AG,&s));
  CHECK(SV_PMStateParseForModel(full,PHYSMODE_AG,&s)&&s.agwaterjump&&s.waterjumpremaining==0&&s.waterjumpdir[0]==50);
  snprintf(text,sizeof(text),"%s wjtime=0 wjx=0 wjy=0 agwaterjump=0",base);
  CHECK(SV_PMStateParseForModel(text,PHYSMODE_AG,&s)&&!s.agwaterjump);
  snprintf(text,sizeof(text),"%s wjtime=1.5 wjx=50 wjy=-5 agwaterjump=1",base);
  CHECK(SV_PMStateParseForModel(text,PHYSMODE_AG,&s)&&s.agwaterjump&&s.waterjumpremaining==1.5);
 }
 puts("PASS actual model wrapper: recognized AG marker rejected as Source; legacy parser unchanged; complete water tuple required");return 0;
}
'''
def main():
 p=argparse.ArgumentParser();p.add_argument('--cc',default='gcc');p.add_argument('--work-dir',type=Path,required=True);p.add_argument('--source-file',type=Path);p.add_argument('--case',choices=('all','source-marker','complete-tuple'),default='all');a=p.parse_args()
 source=(a.source_file or ROOT/'engine/server/sv_user.c').read_text()
 code='#include "quakedef.h"\n'+ '\n'.join(function(source,s) for s in ('qboolean SV_PMStateHasWater(', 'qboolean SV_PMStateParse(', 'qboolean SV_PMStateParseForModel('))
 code+=MAIN.replace('__SOURCE__',str(int(a.case!='complete-tuple'))).replace('__TUPLE__',str(int(a.case!='source-marker')))
 a.work_dir.mkdir(parents=True,exist_ok=True)
 with tempfile.TemporaryDirectory(prefix='model-state-',dir=a.work_dir) as d:
  c=Path(d)/'check.c';exe=Path(d)/'check.exe';c.write_text(code)
  cmd=[a.cc,'-std=gnu99','-Wall','-Wextra','-Werror','-Wno-sign-compare','-I'+str(ROOT/'engine/client'),'-I'+str(ROOT/'engine/common'),str(c),'-lm','-o',str(exe)]
  print(cmd,flush=True);subprocess.run(cmd,check=True,timeout=30);subprocess.run([str(exe)],check=True,timeout=10)
if __name__=='__main__':main()
