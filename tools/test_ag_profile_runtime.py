#!/usr/bin/env python3
"""Owned localhost dedicated/headless-client probe; no install or service actions.
Requires explicit engine/QC/compiler inputs and a fresh runtime under OWNER.md.
Synthetic Q3 brush floor uses the actual engine loader and production QC.
"""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
import re
import datetime
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import time


def floor_bsp():
    pack = struct.pack
    entities = ('{"classname" "worldspawn" "message" "AG private CPU fixture"}\n'
                '{"classname" "info_player_start" "origin" "0 0 0" "angle" "0"}\n').encode() + b'\0'
    shader = pack('<64sii', b'textures/common/caulk', 0, 1)
    planes = b''.join(pack('<4f', *p) for p in ((1,0,0,1024),(-1,0,0,1024),
                    (0,1,0,1024),(0,-1,0,1024),(0,0,1,0),(0,0,-1,64)))
    node = pack('<9i', 4, -1, -2, -1024,-1024,-64,1024,1024,2048)
    leaves = b''.join(pack('<12i', cluster,0,-1024,-1024,-64,1024,1024,2048,0,0,i,1)
                      for i,cluster in enumerate((0,-1)))
    model = pack('<6f4i',-1024,-1024,-64,1024,1024,2048,0,0,0,1)
    vertices = b''.join(pack('<10f4B', x,y,0, 0,0, 0,0, 0,0,1, 255,255,255,255)
                        for x,y in ((-1024,-1024),(1024,-1024),(0,1024)))
    face = pack('<12i12f2i', 0,-1,1,0,3,0,3,-1,0,0,0,0,
                0,0,0, 1,0,0, 0,1,0, 0,0,1, 0,0)
    lumps = [entities,shader,planes,node,leaves,pack('<i',0),pack('<2i',0,0),model,
             pack('<3i',0,6,0),b''.join(pack('<2i',i,0) for i in range(6)),
             vertices,pack('<3i',0,1,2),b'',face,b'',b'',pack('<2iB',1,1,255)]
    offset=8+17*8; directory=[]
    for lump in lumps:
        directory.append(pack('<2i',offset,len(lump))); offset+=len(lump)
    return b'IBSP'+pack('<i',46)+b''.join(directory)+b''.join(lumps)


PROBE = r'''
float ag_probe_done;
float ag_probe_next;
float ag_probe_seeded;
float ag_probe_saved;
float ag_probe_restored;
void() StartFrame =
{
    local entity p;
    local float guard, savedok;
    AGOriginalStartFrame();
    if (ag_probe_done || time < ag_probe_next) return;
    ag_probe_next = time + 1;
    p = find(world, classname, "player");
    if (p != world)
    {
        if (!ag_probe_seeded)
        {
            // Acting stale-table guard: no run fields are modified by the probe.
            run_pb_ticks[0] = 999;
            run_pb_split[1] = 888;
            run_pb_cp[1] = 777;
            ag_probe_seeded = TRUE;
        }
        if (!ag_probe_saved && p.run_t_state == TS_RUNNING && strlen(serverkey("*movementprofile")))
        {
            SV_SaveRootFor(p);
            savedok = SV_SaveWriteState(p, FS_MAXSAVES, 1);
            guard = SL_StateSnapshot(FS_MAXSAVES);
            if (!savedok || guard < 1 || !SL_StateProfileValid(guard))
                bprint(PRINT_HIGH, "AGSOCKET FAIL actual save/prefix identity\n");
            if (guard >= 1) buf_del(guard);
            guard = buf_create_ex("string", 0);
            bufstr_set(guard, 0, "FTESURF-SAVE 1");
            if (SL_StateProfileValid(guard)) bprint(PRINT_HIGH, "AGSOCKET FAIL missing save identity admitted\n");
            bufstr_set(guard, 1, "movementprofile other");
            if (SL_StateProfileValid(guard)) bprint(PRINT_HIGH, "AGSOCKET FAIL unknown save identity admitted\n");
            if (FS_MovementProfilePath("other", "data/runs/x.rec") != "") bprint(PRINT_HIGH, "AGSOCKET FAIL unknown path identity\n");
            buf_del(guard);
            ag_probe_saved = TRUE;
            bprint(PRINT_HIGH, "AGSOCKET actual profile-bound save/prefix PASS\n");
        }
        if (ag_probe_saved && !ag_probe_restored && p.run_t_state == TS_FINISHED)
        {
            SV_SaveRootFor(p);
            if (!SV_SaveApplyState(p, FS_MAXSAVES, FALSE)) bprint(PRINT_HIGH, "AGSOCKET FAIL actual save restore\n");
            else bprint(PRINT_HIGH, "AGSOCKET actual profile-bound restore PASS\n");
            ag_probe_restored = TRUE;
        }
        bprint(PRINT_HIGH, sprintf("AGSOCKET pbguard raw=%g read=%g\n", run_pb_ticks[0], SV_PBGet(0, 0)));
        bprint(PRINT_HIGH, sprintf("AGSOCKET player origin=%v velocity=%v hull=%v/%v eye=%v ticks=%g rate=%.9g\n",
            p.origin, p.velocity, p.mins, p.maxs, p.view_ofs, p.run_movetick, p.run_movetickrate));
        bprint(PRINT_HIGH, sprintf("AGSOCKET timer state=%g flags=%g identity=%s body=%g rec=%g pb=%g split=%g\n",
            p.run_t_state, p.run_t_flags, p.run_agprofile, p.run_t_ticks, p.rec_rec_bytes, p.run_t_pb, p.run_t_delta));
        bprint(PRINT_HIGH, sprintf("AGSOCKET cp=%g input=%v buttons=%g submissions=%g\n", p.run_t_cpdelta, p.movement, p.button2, lobby_run_sent));
        bprint(PRINT_HIGH, sprintf("AGSOCKET pin %s\n", infokey(p, "*pmpin")));
    }
    else if (time > 20) bprint(PRINT_HIGH, "AGSOCKET FAIL no player\n");
    bprint(PRINT_HIGH, sprintf("AGSOCKET server profile=%s mode=%g ruleset=%s\n", serverkey("*movementprofile"), cvar("pm_physicsmode"), serverkey("*ruleset")));
    if (time < 25) return;
    ag_probe_done = TRUE;
    localcmd("movement\nquit\n");
};
'''


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    for name in ('runtime','source','engine','client','compiler'):
        ap.add_argument('--'+name, type=Path, required=True)
    ap.add_argument('--profile',default='ag-100')
    ap.add_argument('--expect-refused-profile',action='store_true')
    # Headless-only CLI; rendered validation requires a separately authorized rig.
    ap.set_defaults(rendered=False)
    ap.add_argument('--netfps',type=int,default=100)
    ap.add_argument('--rec-stream',type=int,choices=(0,1),default=0)
    ap.add_argument('--held-metadata',type=int,choices=(0,1),default=0)
    ap.add_argument('--review-controls',action='store_true')
    ap.add_argument('--residual-retention',action='store_true')
    ap.add_argument('--claim-preservation',action='store_true')
    ap.add_argument('--claim-source',type=Path,help='literal prior resume module, test-only')
    ap.add_argument('--archive',type=int,choices=(0,1),default=1)
    ap.add_argument('--server-ceiling',type=int,default=0)
    ap.add_argument('--raw-ag-mode',action='store_true')
    ap.add_argument('--bad-ag-version',action='store_true')
    ap.add_argument('--missing-ag-version',action='store_true')
    ap.add_argument('--bad-advertised-model',action='store_true')
    ap.add_argument('--bad-profile-identity',action='store_true')
    ap.add_argument('--expect-context-refusal',action='store_true')
    ap.add_argument('--native-selftest',action='store_true')
    ap.add_argument('--selector-cycle',action='store_true',help='ag-100 latch, default restoration, then ag-250 map-boundary control')
    ap.add_argument('--port',type=int,required=True)
    ap.add_argument('--expect-missing-capability',action='store_true',help='client input must be an independently inspected native binary without AG capability')
    args=ap.parse_args()
    if args.review_controls and args.selector_cycle:
        ap.error('review-controls and selector-cycle require separate cases: map resets interrupt the timed review phases')
    root=args.runtime.resolve()
    assert any((p/'OWNER.md').is_file() for p in root.parents)
    assert 27520 <= args.port <= 27620
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as sock: sock.bind(('127.0.0.1',args.port))
    root.mkdir(parents=True,exist_ok=False)
    registry=[]
    def register(process,cmd,cwd,seconds):
        times=[wintypes.FILETIME() for _ in range(4)]
        gettimes=ctypes.WinDLL('kernel32',use_last_error=True).GetProcessTimes
        gettimes.argtypes=[wintypes.HANDLE]+[ctypes.POINTER(wintypes.FILETIME)]*4
        gettimes.restype=wintypes.BOOL
        assert gettimes(int(process._handle),*(ctypes.byref(t) for t in times)), ctypes.get_last_error()
        created=((times[0].dwHighDateTime<<32)|times[0].dwLowDateTime)/10000000-11644473600
        registry.append({'pid':process.pid,'birth_utc':datetime.datetime.fromtimestamp(created,datetime.timezone.utc).isoformat(),
                         'argv':cmd,'cwd':str(cwd),'deadline_epoch':time.time()+seconds,'status':'running'})
        (root/'processes.json').write_text(json.dumps(registry,indent=2))
    def stopped(process):
        for record in registry:
            if record['pid']==process.pid:
                record.update(status='stopped',exit=process.returncode)
        (root/'processes.json').write_text(json.dumps(registry,indent=2))
    game=root/'ftesurf';(game/'maps').mkdir(parents=True);(game/'cfg').mkdir()
    recordroot=game/('data/experimental/'+args.profile+'-v1/runs/ag_profile_test/main' if args.profile.startswith('ag-') else 'data/runs/ag_profile_test/main')
    canonical={}
    protected={};removed=[];staged=[];residual_staged=[];residual_evidence=[]
    if args.review_controls:
        assert args.profile in ('ag-100','ag-250')
        exp=game/'data/experimental'/(args.profile+'-v1')
        def fixture(path,data,normal=False,keep=False,remove=False):
            path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(data)
            if normal:canonical[str(path)]=data
            if keep:protected[str(path)]=(data,path.stat().st_mtime_ns)
            if remove:removed.append(path)
        for number in (1,2,999):
            for ext in ('rec','view','hid'):
                leaf=f'{number:07d}_run.{ext}'
                fixture(game/'data/runs/ag_profile_test/stage_1'/leaf,b'normal archive fixture',normal=True)
                fixture(exp/'runs/ag_profile_test/stage_1'/leaf,b'experimental archive fixture',keep=number<3,remove=number==999)
        for label,day,flags in (('old','20000101',0),('new',datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%d'),0),('protected','20000101',1)):
            for base,normal in ((game/'data',True),(exp,False)):
                directory=base/'resume/ag_profile_test'/('@'+label)/'save000'
                for leaf,data in {'ms.txt':f'FTESURF-RESUME 1\nday {day}\n'.encode(),'state.txt':f'FTESURF-SAVE 1\nflags {flags}\ntrack 0\nstartseg 0\nstagerun 0\n'.encode(),'run.rec':b'owned resume fixture'}.items():
                    fixture(directory/leaf,data,normal=normal,keep=not normal and label!='old',remove=not normal and label=='old')
        for digit in ('0','1','2'):
            for ext in ('view','hid'):
                leaf=digit*32+'.'+ext
                staged.append((leaf,digit!='0'))
    if args.profile.startswith('ag-'):
        for namespace in ('runs','parts','saves','resume','staged','evidence'):
            sentinel=game/'data'/namespace/'owned-normal-sentinel.bin'
            sentinel.parent.mkdir(parents=True,exist_ok=True);sentinel.write_bytes(b'owned normal namespace sentinel')
            canonical[str(sentinel)]=sentinel.read_bytes()
    (game/'maps/ag_profile_test.bsp').write_bytes(floor_bsp())
    shutil.copytree(args.source/'ftesurf/cfg',game/'cfg',dirs_exist_ok=True)
    # Only owned tracked config copies; redirect every embedded HTTP endpoint.
    for cfgpath in (game/'cfg').rglob('*.cfg'):
        cfgpath.write_text(re.sub(r'https?://[^\s\"]+', 'http://127.0.0.1:1', cfgpath.read_text()))
    if args.residual_retention:
        def retain_fixture(path,text,keep=True):
            path.parent.mkdir(parents=True,exist_ok=True);path.write_text(text)
            if keep:protected[str(path)]=(path.read_bytes(),path.stat().st_mtime_ns)
            else:removed.append(path)
        day=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%d')
        for profile in ('ag-100-v1','ag-250-v1'):
            for label,date,flags,typ,claimed,posts in (('expired','20000101',0,0,False,0),('new',day,0,0,False,0),('held','20000101',4194304,0,False,0),('prefix','20000101',0,1,False,0),('claimed','20000101',0,0,True,0),('local','20000101',0,0,True,0),('posted','20000101',0,0,False,1)):
                directory=game/'data/experimental'/profile/'resume/ag_profile_test'/('@'+label)/'save000'
                keep=label not in ('expired','posted')
                for leaf,text in (('ms.txt',f'FTESURF-RESUME 1\nmap ag_profile_test\nday {date}\nrecsess 0 0 {posts} 0\nrunid 20000101-owned\n'),('state.txt',f'FTESURF-SAVE 1\nflags {flags}\ntrack 0\nstartseg 0\nstagerun {typ}\n'),('run.rec','owned inactive resume prefix\n')):
                    retain_fixture(directory/leaf,text,keep)
                if claimed:
                    claim=directory/'ms.claim.txt';retain_fixture(claim,'FTESURF-RESUME 1\nmap ag_profile_test\nday 20000101\n',True)
                    if label=='local' and profile==args.profile+'-v1':protected.pop(str(claim));removed.append(claim)
            residual_evidence.append(game/'data/experimental'/profile/'evidence/ag_profile_test/20000101-owned.rec')
        retain_fixture(game/'data/experimental/ag-1000-v1/resume/ag_profile_test/@unknown/save000/ms.txt','unknown root sentinel\n')
        retain_fixture(game/'data/resume/ag_profile_test/@normal/save000/ms.txt','FTESURF-RESUME 1\nmap ag_profile_test\nday 20000101\n')
        retain_fixture(game/'data/resume/ag_profile_test/@normal/save000/state.txt','FTESURF-SAVE 1\nflags 1\ntrack 0\nstartseg 0\nstagerun 0\n')
        retain_fixture(game/'data/evidence/ag_profile_test/20000101-owned.rec','normal evidence sentinel\n')
        for profile in ('ag-100-v1','ag-250-v1'):
            for digit in ('0','1','2'):
                residual_staged.append((profile,digit*32+'_view',digit!='0'))
    if args.claim_preservation:
        claim_keep={};claim_remove=[];claim_moves=[]
        def claim_file(path,text,keep):
            path.parent.mkdir(parents=True,exist_ok=True);path.write_text(text)
            if keep:claim_keep[str(path)]=(path.read_bytes(),path.stat().st_mtime_ns)
            else:claim_remove.append(path)
        def claim_slot(base,label,keep,flags=0,claimed=False):
            directory=base/'resume/ag_profile_test'/('@'+label)/'save000'
            meta=f'FTESURF-RESUME 1\nday 20000101\nrecsess 0 0 1 0\nrunid 20000101-{label}\n'
            for leaf,text in (('ms.claim.txt' if label=='outer' else 'ms.txt',meta),('state.txt',f'FTESURF-SAVE 1\nflags {flags}\ntrack 0\nstartseg 0\nstagerun 0\n'),('run.rec','owned '+label+' prefix\n')):
                claim_file(directory/leaf,text,keep)
            if claimed:claim_file(directory/'ms.claim.txt',meta,keep)
            evidence=base/'evidence/ag_profile_test'/('20000101-'+label+'.rec')
            claim_file(evidence,'existing '+label+' evidence\n',keep)
            if not keep:
                claim_remove.remove(evidence);claim_moves.append((evidence,'owned '+label+' prefix\n'))
        for profile in ('ag-100-v1','ag-250-v1'):
            base=game/'data/experimental'/profile
            claim_slot(base,'fault',True,claimed=True)
            claim_slot(base,'outer',True)
            claim_slot(base,'neighbor',False)
        claim_slot(game/'data','normal',True,flags=1)
        claim_slot(game/'data','normal-neighbor',args.profile!='default')
    if args.raw_ag_mode:
        default=game/'cfg/default.cfg'
        text,count=re.subn(r'(?m)^(set\s+pm_physicsmode\s+)1\s*$',r'\g<1>2',default.read_text())
        assert count==1
        text,count=re.subn(r'(?m)^(set\s+pm_ticrate\s+)\S+',r'\g<1>0.001',text)
        assert count==1;default.write_text(text)
    shutil.copy2(args.source/'default.fmf',root/'default.fmf')
    zones=game/'maps/zones/local'; zones.mkdir(parents=True)
    def region(lo,hi):
        return {'points':[[lo,-100],[hi,-100],[hi,100],[lo,100]],'bottom':-1,'height':100}
    (zones/'ag_profile_test.json').write_text(json.dumps({'tracks':{'main':{'zones':{
        'segments':[{'checkpoints':[{'regions':[region(-64,64)]}, {'regions':[region(150,170)]}]},
                    {'checkpoints':[{'regions':[region(240,260)]}, {'regions':[region(350,370)]}]}],
        'end':{'regions':[region(480,600)]}}}}}))
    src=(args.source/'src').resolve()
    probe=root/'ag_startframe.qc'
    mainfile=(src/'server/sv_main.qc').read_text()
    assert mainfile.count('void() StartFrame =')==1
    probe_code=PROBE
    if args.server_ceiling:
        probe_code=probe_code.replace('    AGOriginalStartFrame();','    AGOriginalStartFrame();\n    if (time > 1) forceinfokey(world,"maxfps","'+str(args.server_ceiling)+'");',1)
    if args.bad_ag_version:
        probe_code=probe_code.replace('    AGOriginalStartFrame();','    AGOriginalStartFrame();\n    if (time > 1) forceinfokey(world,"pm_agversion","2");',1)
    if args.missing_ag_version:
        probe_code=probe_code.replace('    AGOriginalStartFrame();','    AGOriginalStartFrame();\n    if (time > 1) forceinfokey(world,"pm_agversion","");',1)
    if args.bad_advertised_model:
        probe_code=probe_code.replace('    AGOriginalStartFrame();','    AGOriginalStartFrame();\n    if (time > 1) forceinfokey(world,"pm_physicsmode","1");',1)
    if args.bad_profile_identity:
        probe_code=probe_code.replace('    AGOriginalStartFrame();','    AGOriginalStartFrame();\n    if (time > 1) forceinfokey(world,"*movementprofile","ag-250-v1");',1)
    review_code=''
    if args.review_controls:
        review_code=(Path(__file__).parent/'test_ag_review_probe.qc').read_text()
        # Eight one-second review phases follow restore; cold CSQC startup can
        # consume most of the basic 25-second world lifetime before input begins.
        assert probe_code.count('    if (time < 25) return;') == 1
        probe_code=probe_code.replace('    if (time < 25) return;', '    if (time < 40) return;',1)
        probe_code=probe_code.replace('        bprint(PRINT_HIGH, sprintf("AGSOCKET pbguard', '        AGReviewServerStep(p, ag_probe_restored);\n        bprint(PRINT_HIGH, sprintf("AGSOCKET pbguard',1)
    if args.residual_retention:
        review_code=(Path(__file__).parent/'test_ag_review_probe.qc').read_text()
        probe_code=probe_code.replace('        bprint(PRINT_HIGH, sprintf("AGSOCKET pbguard', '        AGResidualCleanupStep(p);\n        bprint(PRINT_HIGH, sprintf("AGSOCKET pbguard',1)
    if args.claim_preservation:
        review_code=(Path(__file__).parent/'test_ag_review_probe.qc').read_text()
        probe_code='float claim_done;\nvoid() StartFrame = { AGOriginalStartFrame(); if (!claim_done && time > 1) { claim_done=TRUE; AGClaimCheck(); } };\n'
        resume_path=args.claim_source or src/'server/sv_resume.qc'
        original_resume=resume_path.read_text()
        resume=original_resume
        edits=[('h = search_begin(strcat(dir, "ms*.txt"), FS_NAMESORT, TRUE);','h = AGClaimSearch(strcat(dir, "ms*.txt"), FS_NAMESORT, TRUE);'),
               ('float(string dir) SV_MsSweepProtected =','searchhandle(string pattern,float flags,float quiet) AGClaimSearch;\nvoid(string dir) AGClaimProtectionProbe;\nfloat(string dir) SV_MsSweepProtected ='),
               ('h = AGClaimSearch(strcat(dir, "ms*.txt"), FS_NAMESORT, TRUE);','AGClaimProtectionProbe(dir);\n\th = AGClaimSearch(strcat(dir, "ms*.txt"), FS_NAMESORT, TRUE);'),
               ('void(string dir, string leaf, string profile) SV_MsWipeDirProfile =','void(string dir) AGClaimWipeProbe;\nvoid(string dir, string leaf, string profile) SV_MsWipeDirProfile ='),
               ('\tlocal string dst;\n\n\tif (ms_posts > 0)','\tlocal string dst;\n\n\tAGClaimWipeProbe(dir);\n\tif (ms_posts > 0)')]
        for old,new in edits:
            assert resume.count(old)==1,old
            resume=resume.replace(old,new,1)
        claim_overlay=root/'ag_claim_resume.qc';claim_overlay.write_text(resume)
        (root/'claim-subject.json').write_text(json.dumps({'original_path':str(resume_path),'original_sha256':hashlib.sha256(resume_path.read_bytes()).hexdigest(),'decoded_original_sha256':hashlib.sha256(original_resume.encode()).hexdigest(),'overlay_sha256':hashlib.sha256(claim_overlay.read_bytes()).hexdigest(),'substitutions':edits,'native_builtin_pointer_and_body_unchanged':True,'scope':'test-only exact nested negative result; no allocator/natural-error reproduction'},indent=2))
    probe.write_text(mainfile.replace('void() StartFrame =','void() AGOriginalStartFrame =',1)+'\n'+review_code+'\n'+probe_code)
    manifest=[os.path.relpath(game/'qwprogs.dat',src).replace('\\','/')]
    for line in (src/'sv_progs.src').read_text().splitlines()[1:]:
        path=line.split('//',1)[0].strip()
        if not path: continue
        if args.claim_preservation and path=='server/sv_resume.qc':path=os.path.relpath(claim_overlay,src).replace('\\','/')
        manifest.append(os.path.relpath(probe,src).replace('\\','/') if path=='server/sv_main.qc' else path)
    source_list=src/'ag-profile-probe.src'
    with source_list.open('x') as handle:handle.write('\n'.join(manifest)+'\n')
    try:
        (root/'probe-manifest.src').write_bytes(source_list.read_bytes())
        compilecmd=[str(args.compiler.resolve()),'-max_strings','4194304','-srcfile',source_list.name]
        with (root/'compile.log').open('x') as compileout:
            compiler=subprocess.Popen(compilecmd,cwd=src,stdout=compileout,stderr=subprocess.STDOUT)
            register(compiler,compilecmd,src,120)
            try:
                compilecode=compiler.wait(timeout=120)
            finally:
                if compiler.poll() is None: compiler.terminate(); compiler.wait(timeout=10)
                stopped(compiler)
        assert compilecode==0 and 'Done. 0 warnings' in (root/'compile.log').read_text()
    finally:
        source_list.unlink()
    env=os.environ.copy();env['PATH']='C:/msys64/ucrt64/bin;C:/msys64/usr/bin;'+env.get('PATH','')
    safety = """sys_colorconsole 0
cfg_save_auto 0
set fs_automount 0
sv_public 0
forceqmenu 0
cl_run_upload 0
run_resume 0
cl_board_url ""
update_enabled 0
update_autocheck 0
set rec_terms 0
set lobby_dir_gen 999
set lobby_master ""
set lobby_master_key ""
set lobby_dir "http://127.0.0.1:1"
set cl_download_mapsrc "http://127.0.0.1:1"
set cl_download_zonesrc "http://127.0.0.1:1"
set sv_dlURL "http://127.0.0.1:1"
cl_download 0
"""
    safety += ''.join('set '+prefix+str(i)+' ""'+chr(10) for prefix in ('net_master','net_qwmaster','net_q2master','net_q3master') for i in range(1,9))
    servercfg = ('exec cfg/default.cfg'+chr(10)+safety+'log_enable 1'+chr(10)+
                 'log_name ag-server'+chr(10)+'set lobby_enable 0'+chr(10)+
                 'rec_stream '+str(args.rec_stream)+chr(10)+'rec_held_metadata '+str(args.held_metadata)+chr(10)+'sv_movementprofile "'+args.profile+'"'+chr(10)+'map ag_profile_test'+chr(10)+'movement'+chr(10))
    clientroot=root/'client';clientgame=clientroot/'ftesurf';clientgame.mkdir(parents=True)
    shutil.copy2(root/'default.fmf',clientroot/'default.fmf')
    shutil.copytree(game/'cfg',clientgame/'cfg');shutil.copytree(game/'maps',clientgame/'maps')
    # Use production CSQC/menu outputs from the most recent NoDeploy compile.
    products=sorted((args.source/'rig').glob('build-nodeploy-*'),key=lambda p:p.stat().st_mtime)
    for f in ('csprogs.dat','menu.dat'):
        shutil.copy2(products[-1]/f,clientgame/f)
        shutil.copy2(products[-1]/f,game/f) # seed dedicated CSQC before its first spawn/hash
    if args.review_controls or args.residual_retention:
        clmain=(src/'client/cl_main.qc').read_text().replace(') CSQC_Init =',') AGOriginalInit =',1).replace(') CSQC_ConsoleCommand =',') AGOriginalConsole =',1)
        for include in ('cl_tasinput.qc','cl_tasplan.qc'):
            clmain=clmain.replace('#include "'+include+'"','#include "'+Path(os.path.relpath(src/'client'/include,root)).as_posix()+'"')
        clmain+='''\nvoid(float api, string name, float version) CSQC_Init =
{ AGOriginalInit(api,name,version); registercommand("agreviewstaged"); registercommand("agresidualmap"); print("AGREVIEW init ready\\n"); };
float(string cmd) CSQC_ConsoleCommand =
{
    if (tokenize(cmd) > 0 && argv(0) == "agreviewstaged")
    {
        Rec_StagedClear("11111111111111111111111111111111", "22222222222222222222222222222222");
        print("AGREVIEW staged clear PASS\\n"); return TRUE;
    }
    if (tokenize(cmd) > 0 && argv(0) == "agresidualmap")
    {
        Rec_StagedMapClear("11111111111111111111111111111111", "22222222222222222222222222222222");
        print("AGRESIDUAL staged map PASS\\n"); return TRUE;
    }
    return AGOriginalConsole(cmd);
};\n'''
        clprobe=root/'ag_review_client.qc';clprobe.write_text(clmain)
        clmanifest=[Path(os.path.relpath(clientgame/'csprogs.dat',src)).as_posix()]
        for line in (src/'cl_progs.src').read_text().splitlines()[1:]:
            path=line.split('//',1)[0].strip()
            if path:clmanifest.append(Path(os.path.relpath(clprobe,src)).as_posix() if path=='client/cl_main.qc' else path)
        cllist=src/'ag-review-client.src'
        with cllist.open('x') as handle:handle.write('\n'.join(clmanifest)+'\n')
        try:
            cmd=[str(args.compiler.resolve()),'-max_strings','4194304','-Fomitarraydefs','-srcfile',cllist.name]
            with (root/'client-compile.log').open('x') as output:
                compiler=subprocess.Popen(cmd,cwd=src,stdout=output,stderr=subprocess.STDOUT);register(compiler,cmd,src,120)
                try:code=compiler.wait(timeout=120)
                finally:
                    if compiler.poll() is None:compiler.terminate();compiler.wait(timeout=10)
                    stopped(compiler)
            assert code==0 and 'Done. 0 warnings' in (root/'client-compile.log').read_text()
            shutil.copy2(clientgame/'csprogs.dat',game/'csprogs.dat')
        finally:cllist.unlink()
    servercfg=safety+servercfg+safety
    if args.expect_refused_profile:servercfg+='quit\n'
    servercfg += 'set lobby_master "http://127.0.0.1:1"\nset lobby_master_key "owned-fixture-not-a-real-key"\nsv_guidkey "owned-fixture-guid-salt"\n'
    servercfg += 'rec_runarchive '+str(args.archive)+'\n'
    if args.server_ceiling:servercfg+='serverinfo maxfps '+str(args.server_ceiling)+'\n'
    if args.raw_ag_mode:servercfg='pm_lockmovement 0\npm_physicsmode 2\n'+servercfg.replace('exec cfg/default.cfg','exec cfg/default.cfg\npm_lockmovement 0\npm_physicsmode 2')
    if args.native_selftest:servercfg+='pm_selftest\n'
    (game/'ag-server.cfg').write_text(servercfg)
    localpb=clientgame/'data/runs/ag_profile_test/main'
    localpb.mkdir(parents=True)
    tick=1/int(args.profile[3:]) if args.profile.startswith('ag-') else 0.015
    poison=localpb/'0000777_lobby.rec'
    poison.write_text('FTESURF-REC 5\ntickrate '+str(tick)+'\nflags 0\nsamples 0\n')
    if args.review_controls:
        for leaf,keep in staged:
            path=clientgame/'data/staged'/leaf;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(b'normal staged fixture')
            canonical[str(path)]=path.read_bytes()
    residual_initial=[]
    if args.residual_retention:
        for profile in ('ag-100-v1','ag-250-v1'):
            path=clientgame/'data/experimental'/profile/'staged'/('9'*32+'_view')
            retain_fixture(path,'pre-map optional orphan\n',False);residual_initial.append(path)
        retain_fixture(clientgame/'data/staged/normal-sentinel','normal staged sentinel\n',args.profile!='default')
    if args.residual_retention:
        for name,(content,mtime) in protected.items():
            if Path(name).is_relative_to(game/'data') and not Path(name).is_relative_to(game/'data/experimental'):canonical[name]=content
    canonical_mtime={name:Path(name).stat().st_mtime_ns for name in canonical}
    cfg=(safety+'cfg_save_auto 0\nlog_enable 1\nlog_name ag-client\nvid_renderer headless\n'
         'set lobby_enable 0\nset lobby_dir "http://127.0.0.1:1"\ncl_download 0\n'
         'cl_netfps '+str(args.netfps)+'\ncl_idlefps 0\ncl_maxfps 100\nset vid_headless_sleep 0\nsetinfo spectator 0\n'
         +
         'connect 127.0.0.1:'+str(args.port)+'\nwaitmap 60\nwaitms 2000\n')
    (clientgame/'ag-client.cfg').write_text(cfg)
    common=['-plugin','-noreset','-nobrowse','-nohome','-nosound','-ip','127.0.0.1','+set','forceqmenu','0','+set','update_enabled','0','+set','update_autocheck','0']
    servercmd=[str(args.engine.resolve()),*common,'-basedir',str(root),'-manifest',str(root/'default.fmf'),'-port',str(args.port),'+exec','ag-server.cfg']
    clientcmd=[str(args.client.resolve()),*common,'-basedir',str(clientroot),'-manifest',str(clientroot/'default.fmf'),'-port',str(args.port+1),'+set','vid_renderer','headless','+exec','ag-client.cfg']
    receipts={'server_argv':servercmd,'client_argv':clientcmd,'profile':args.profile,'netfps':args.netfps,
              'inputs':{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in (args.engine,args.client,args.compiler,game/'qwprogs.dat',game/'maps/ag_profile_test.bsp')}}
    (root/'inputs.json').write_text(json.dumps(receipts,indent=2))
    if args.claim_preservation:
        with (root/'server-console.log').open('x') as serverout:
            server=subprocess.Popen(servercmd,cwd=root,env=env,stdin=subprocess.PIPE,stdout=serverout,stderr=subprocess.STDOUT,creationflags=0x08000000)
            register(server,servercmd,root,20)
            try:
                deadline=time.monotonic()+15
                while server.poll() is None and time.monotonic()<deadline:
                    logs=(root/'server-console.log').read_text(errors='replace')
                    if 'AGCLAIM completed' in logs:break
                    time.sleep(0.1)
                if server.poll() is None:server.stdin.write(b'quit\n');server.stdin.flush()
                server.wait(timeout=5)
            finally:
                if server.poll() is None:server.terminate();server.wait(timeout=10)
                stopped(server)
        logs=(root/'server-console.log').read_text(errors='replace')
        changed=[name for name,(data,mtime) in claim_keep.items() if not Path(name).is_file() or Path(name).read_bytes()!=data or Path(name).stat().st_mtime_ns!=mtime]
        remaining=[str(path) for path in claim_remove if path.exists()]
        moves=[str(path) for path,text in claim_moves if not path.is_file() or path.read_text()!=text]
        proof={'protected_files':len(claim_keep),'changed_protected':changed,'remaining_expiry_files':remaining,'incorrect_relocations':moves,'complete_marker':'AGCLAIM completed' in logs,'singleton_claims_protected':'AGCLAIM singleton protected100=1 protected250=1 calls=2' in logs,'no_live_actor':'AGCLAIM FAIL live actor' not in logs,'injected_failure_not_natural':True,'server_exit':server.returncode}
        (root/'claim-proof.json').write_text(json.dumps(proof,indent=2))
        assert proof['complete_marker'] and proof['singleton_claims_protected'] and 'AGCLAIM FAIL' not in logs,logs[-4000:]
        assert not changed and not remaining and not moves,proof
        assert server.returncode==0
        (root/'result.json').write_text(json.dumps({'passed':True,'profile':args.profile,'claim_preservation':proof,'owned_processes_stopped':True,'prediction_acceptance':False},indent=2))
        print('PASS injected nested claim-search preservation + outer-leaf guard; real expiry/relocation neighbor; no actors')
        return
    if args.expect_refused_profile:
        with (root/'server-console.log').open('x') as serverout:
            server=subprocess.Popen(servercmd,cwd=root,env=env,stdout=serverout,stderr=subprocess.STDOUT,creationflags=0x08000000)
            register(server,servercmd,root,15)
            try:
                servercode=server.wait(timeout=15)
            finally:
                if server.poll() is None:server.terminate();server.wait(timeout=10)
                stopped(server)
        logs=(root/'server-console.log').read_text(errors='replace')
        assert servercode==0 and ('Refusing map: sv_movementprofile '+args.profile) in logs
        assert 'Experimental AG-derived' not in logs and 'AGSOCKET' not in logs
        (root/'result.json').write_text(json.dumps({'passed':True,'profile':args.profile,'refused_before_spawn':True,'owned_processes_stopped':True},indent=2))
        print('PASS explicit profile refusal before spawn: '+args.profile)
        return
    with (root/'server-console.log').open('x') as serverout,(root/'client-console.log').open('x') as clientout:
        server=subprocess.Popen(servercmd,cwd=root,env=env,stdin=subprocess.PIPE,stdout=serverout,stderr=subprocess.STDOUT,creationflags=0x08000000)
        register(server,servercmd,root,60)
        client=None
        try:
            time.sleep(2)
            client=subprocess.Popen(clientcmd,cwd=clientroot,env=env,stdin=subprocess.PIPE,stdout=clientout,stderr=subprocess.STDOUT,creationflags=0x08000000)
            register(client,clientcmd,clientroot,60)
            if args.expect_context_refusal:
                deadline=time.monotonic()+20
                while time.monotonic()<deadline and server.poll() is None:
                    text='\n'.join(p.read_text(errors='replace') for p in root.rglob('*.log'))
                    if 'Refusing movement:' in text or 'Refusing AG commands:' in text:break
                    time.sleep(0.1)
                text='\n'.join(p.read_text(errors='replace') for p in root.rglob('*.log'))
                assert 'Refusing movement:' in text or 'Refusing AG commands:' in text
                assert not list(recordroot.rglob('*.rec'))
                (root/'result.json').write_text(json.dumps({'passed':True,'context_refused':True,'scope':'headless negative authority/ceiling control'},indent=2))
                print('PASS actual unbound context or incompatible ceiling refusal')
                return
            # Windows -plugin is the existing pipe-console route, not GUI input.
            deadline=time.monotonic()+18
            clientlog=clientgame/'logs/ag-client.log'
            while time.monotonic()<deadline:
                if clientlog.exists() and 'FTESurf CSQC loaded' in clientlog.read_text(errors='replace'): break
                time.sleep(0.1)
            time.sleep(2)
            if args.review_controls:
                for leaf,keep in staged:
                    path=clientgame/'data/experimental'/(args.profile+'-v1')/'staged'/leaf
                    path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(b'experimental staged fixture')
                    if keep:protected[str(path)]=(path.read_bytes(),path.stat().st_mtime_ns)
                    else:removed.append(path)
                client.stdin.write(b'fs_cache 0\nagreviewstaged\n');client.stdin.flush()
            if args.residual_retention:
                assert all(not path.exists() for path in residual_initial),'real Rec_ViewInit did not clean inactive roots'
                for profile,leaf,keep in residual_staged:
                    retain_fixture(clientgame/'data/experimental'/profile/'staged'/leaf,'late two-run fixture\n',keep)
                client.stdin.write(b'fs_cache 0\nagreviewstaged\n');client.stdin.flush();time.sleep(0.5)
                for profile,leaf,keep in residual_staged:
                    path=clientgame/'data/experimental'/profile/'staged'/leaf
                    expected=keep or profile!=args.profile+'-v1'
                    assert path.exists()==expected,('run-end protection',path)
                client.stdin.write(b'agresidualmap\n');client.stdin.flush()
            client.stdin.write(b'vid_headless_sleep 0; vid_headless_sleep; closemenu; cl_prederror 0.01; cl_predtrace 0; lobbytime best 0 0; lobbytime keep 0 0 555 0; lobbytime best 0 0; -forward; +forward; echo AGINPUT_WALK; cl_forwardspeed; cl_idlefps; cl_maxfps; cl_netfps; cl_nopred; cl_threadedphysics\n'.replace(b'; ',b'\n'))
            client.stdin.flush()
            reader_sent=False
            cycle_state=0
            cycle_at=0
            cycle_client_offset=0;cycle_walk=False;cycle_witness=None
            walk_started=time.monotonic()
            stopped_walk=False
            deadline=time.monotonic()+35
            while server.poll() is None and time.monotonic()<deadline:
                if not stopped_walk and time.monotonic()-walk_started>2.5:
                    client.stdin.write(b'-forward; prederr_dump ag-walk.txt; cl_prederror_count; echo AGINPUT_STOP\n')
                    client.stdin.flush()
                    stopped_walk=True
                if args.review_controls:
                    savedroot=game/'data/experimental'/(args.profile+'-v1')/'saves/ag_profile_test/save999'
                    for saved in (savedroot/'state.txt',savedroot/'run.rec'):
                        if saved.is_file() and str(saved) not in protected and saved.stat().st_size:
                            protected[str(saved)]=(saved.read_bytes(),saved.stat().st_mtime_ns)
                records=list(recordroot.rglob('*.rec'))
                if args.archive==0:records=[p for p in records if p.name=='last.rec']
                records.sort(key=lambda p:(p.name!='last.rec','_shadow' in p.name,p.name))
                if records and not reader_sent:
                    relative=records[0].relative_to(game).as_posix()
                    # External test-only variants are not written through the game's
                    # FS cache invalidation path. Use documented direct lookups.
                    commands=['fs_cache 0','pm_recsim '+relative]
                    if args.profile.startswith('ag-'):
                        original=records[0].read_text()
                        agpin=next(line for line in original.splitlines() if line.startswith('pmpin '))[6:]
                        withoutpin='\n'.join(line for line in original.splitlines() if not line.startswith('pmpin '))+'\n'
                        sourceheader='\n'.join(line for line in original.splitlines() if not line.startswith('movementprofile '))+'\n'
                        sourceheader=sourceheader.replace('pmpin '+agpin, 'pmpin '+agpin.replace('physmode=2','physmode=1').replace('pmsrcver=1 ','pmsrcver=12 '))
                        variants={'optional-no-pin.rec':withoutpin,
                                  'optional-source-pin.rec':original.replace('physmode=2','physmode=1'),
                                  'source-restated-ag.rec':sourceheader.replace('begin\n','begin\npm 0 0 '+agpin+'\n',1)}
                        for filename,text in variants.items():
                            (game/filename).write_text(text)
                            commands.append('pm_recsim '+filename)
                    server.stdin.write(('\n'.join(commands)+'\n').encode())
                    server.stdin.flush()
                    (root/'reader-command.txt').write_text('\n'.join(commands))
                    reader_sent=True
                if args.selector_cycle and reader_sent:
                    now=time.monotonic()
                    if cycle_state==0:
                        server.stdin.write(b'echo AG_CYCLE_LATCHED\nsv_movementprofile ag-250\nmovement\n');server.stdin.flush()
                        cycle_state=1;cycle_at=now
                    elif cycle_state==1 and now-cycle_at>2:
                        server.stdin.write(b'echo AG_CYCLE_DEFAULT\nsv_movementprofile default\nmap ag_profile_test\nmovement\n');server.stdin.flush()
                        cycle_state=2;cycle_at=now
                    elif cycle_state==2 and now-cycle_at>4:
                        cycle_client_offset=len(clientlog.read_text(errors='replace'))
                        server.stdin.write(b'echo AG_CYCLE_FINAL\nsv_movementprofile ag-250\nmap ag_profile_test\nmovement\n');server.stdin.flush()
                        cycle_state=3;cycle_at=now
                    elif cycle_state==3:
                        final_client=clientlog.read_text(errors='replace')[cycle_client_offset:]
                        assert client.poll() is None, ('final AG250 client exited',final_client[-2000:])
                        assert 'Refusing AG commands:' not in final_client and 'Host_EndGame:' not in final_client, final_client[-2000:]
                        if not cycle_walk and 'FTESurf CSQC loaded' in final_client:
                            client.stdin.write(b'echo AG_CLIENT_FINAL\nserverinfo\n+forward\n');client.stdin.flush()
                            cycle_walk=True
                        final_server=(root/'server-console.log').read_text(errors='replace').split('AG_CYCLE_FINAL',1)[-1]
                        samples=[tuple(map(float,row)) for row in re.findall(r'AGSOCKET player origin=([^ ]+) ([^ ]+) ([^ ]+) .* ticks=([^ ]+) rate=([^\r\n]+)',final_server)]
                        inputs=[float(v) for v in re.findall(r'AGSOCKET cp=.* input=([^ ]+)',final_server)]
                        pins=[line for line in final_server.splitlines() if 'AGSOCKET pin ' in line]
                        valid_pin=any(re.search(r'\bpmsrcver=1\b',line) and re.search(r'\bphysmode=2\b',line) for line in pins)
                        if cycle_walk and len(samples)>=2:
                            ticks=[row[3] for row in samples];xs=[row[0] for row in samples]
                            if max(ticks)-min(ticks)>100 and max(xs)-min(xs)>64 and any(v>0 for v in inputs) and valid_pin:
                                assert all(abs(row[4]-.004)<1e-8 for row in samples)
                                cycle_witness={'client_live_before_stop':True,'no_context_refusal':True,'native_ag250_pin':True,'ticks':ticks,'x_positions':xs,'forward_inputs':inputs,'rate':.004}
                                client.stdin.write(b'-forward\n');client.stdin.flush()
                                server.stdin.write(b'echo AG_CYCLE_DONE\nquit\n');server.stdin.flush()
                                cycle_state=4
                        assert cycle_state==4 or now-cycle_at<12, 'final AG250 client/input/body witness missing'
                time.sleep(0.1)
            servercode=server.wait(timeout=5)
        finally:
            if client and client.poll() is None:client.terminate();client.wait(timeout=10)
            if server.poll() is None:server.terminate();server.wait(timeout=10)
            if client:stopped(client)
            stopped(server)
    logs='\n'.join(p.read_text(errors='replace') for p in root.rglob('*.log') if p.name!='compile.log')
    print('\n'.join(line for line in logs.splitlines() if 'AGSOCKET' in line or 'experimental' in line.lower() or 'requires an AG' in line))
    assert servercode==0,servercode
    if args.native_selftest:
        assert 'AG state: active zero retained' in logs and not re.search(r'(?:FAIL|failed [1-9])',logs)
    if args.review_controls:
        assert 'AGREVIEW actual waterjump active/inactive zero + positive save/load one-shot PASS' in logs
        assert 'AGREVIEW staged clear PASS' in logs
    rate_logs=logs
    if args.selector_cycle:
        serverlog=(root/'server-console.log').read_text(errors='replace')
        initial,later=serverlog.split('AG_CYCLE_LATCHED',1)
        latched,later=later.split('AG_CYCLE_DEFAULT',1)
        restored,final=later.split('AG_CYCLE_FINAL',1)
        assert 'profile=ag-100-v1 mode=2' in latched and 'ag-250-v1' not in latched
        assert 'AGSOCKET server profile= mode=1' in restored, 'default did not restore Source'
        assert 'optional ag-' not in restored
        assert 'profile=ag-250-v1 mode=2' in final and 'AG_CYCLE_DONE' in final
        assert cycle_witness is not None, 'server strings alone do not qualify the final client'
        (root/'selector-cycle.json').write_text(json.dumps({'passed':True,'midmap_identity_unchanged':True,'default_source_restored':True,'ag250_next_map':True,'final_client_acting':cycle_witness},indent=2))
        rate_logs=initial
    if args.expect_missing_capability:
        assert 'requires an AG-v1 capable client' in logs
    else:
        assert 'AGSOCKET player' in logs and 'AGSOCKET FAIL' not in logs
        if args.profile.startswith('ag-'):
            assert 'identity='+args.profile+'-v1' in logs
            expected=1/int(args.profile[3:])
            observed=list(map(float,re.findall(r'AGSOCKET player .* rate=([0-9.e-]+)',rate_logs)))
            assert observed and all(abs(rate-expected)<1e-8 for rate in observed)
            assert 'UNSUPPORTED physics mode 2' in logs, 'native reader did not explicitly refuse AG physics'
            assert 'in pm restatement: Source-only verifier, no physics verdict' in logs
            assert logs.count('UNSUPPORTED physics mode') >= 4
        assert re.search(r'AGSOCKET timer state=2 .*body=[1-9]', logs), 'timer never ran through real walking'
        recordings=list(recordroot.rglob('*.rec'))
        assert recordings, 'no retained diagnostic recording in actual run archive'
        for rec in recordings:
            text=rec.read_text()
            if args.profile.startswith('ag-'):
                assert 'movementprofile '+args.profile+'-v1' in text
                assert 'physmode=2' in text and 'pmsrcver=1' in text
                assert 'in ' in text and 'seed ' in text
        if args.profile.startswith('ag-'):
            assert 'pbguard raw=999 read=0' in logs
            assert 'actual profile-bound save/prefix PASS' in logs
            assert 'actual profile-bound restore PASS' in logs
            assert not list((game/'data').rglob('best.pb')), 'AG wrote normal PB'
            for filename,content in canonical.items():
                assert Path(filename).read_bytes()==content and Path(filename).stat().st_mtime_ns==canonical_mtime[filename]
            for filename,(content,mtime) in protected.items():assert Path(filename).read_bytes()==content and Path(filename).stat().st_mtime_ns==mtime
            for path in removed:assert not path.exists(), 'eligible experimental retention artifact remained: '+str(path)
            if args.archive==0:assert not list(recordroot.rglob('*_run.rec'))
            normal_files=[p for ns in ('runs','parts','saves','resume','staged','evidence') for p in (game/'data'/ns).rglob('*') if p.is_file()]
            assert set(map(str,normal_files))=={name for name in canonical if Path(name).is_relative_to(game)}, 'optional artifact escaped into normal namespace'
            assert all('/data/experimental/'+args.profile+'-v1/runs/' in rec.as_posix() for rec in recordings)
            assert list(localpb.iterdir())==[poison], 'AG promoted a local lobby best or receipt'
            assert 'best 0  unranked 0' in logs, 'AG local best accessor was not suppressed'
            assert not re.search(r'submissions=[1-9]', logs), 'AG reached normal submission path'
        else:
            assert 'best 777  unranked 0' in logs and 'best 555  unranked 0' in logs, 'normal local best positive control failed'
            assert re.search(r'submissions=[1-9]', logs), 'normal submission positive control failed'
        if args.residual_retention:
            assert 'AGRESIDUAL resume sweep PASS' in logs and 'AGRESIDUAL staged map PASS' in logs
            assert 'AGSOCKET FAIL' not in logs
            for filename,(content,mtime) in protected.items():
                path=Path(filename);assert path.read_bytes()==content and path.stat().st_mtime_ns==mtime,filename
            for filename in removed:assert not Path(filename).exists(),filename
            for path in residual_evidence:assert path.read_text()=='owned inactive resume prefix\n',path
        (root/'result.json').write_text(json.dumps({'passed':True,'profile':args.profile,'records':list(map(str,recordings)),'owned_processes_stopped':True,'prediction_acceptance':False,'scope':'headless socket/server/timer/identity/PB/submission only'},indent=2))
    print('PASS bounded actual dedicated/client probe; inspect inputs/logs for rendering and scope')


if __name__=='__main__':main()
