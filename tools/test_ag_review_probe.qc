// Test-only phase control; first real walk/finish/restore is completed before holds.
float(string dir) SV_MsSweepProtected;
float ag_claim_faults, ag_claim_neighbors, ag_claim_outer, ag_claim_wipes, ag_claim_normal;
searchhandle(string pattern, float flags, float quiet) AGClaimSearch =
{
    if (pattern == "data/experimental/ag-100-v1/resume/ag_profile_test/@fault/save000/ms*.txt" ||
        pattern == "data/experimental/ag-250-v1/resume/ag_profile_test/@fault/save000/ms*.txt")
    {
        ag_claim_faults=ag_claim_faults+1;
        return -1;
    }
    if (strstrofs(pattern,"/@neighbor/save000/ms*.txt",0) >= 0) ag_claim_neighbors=ag_claim_neighbors+1;
    return search_begin(pattern,flags,quiet);
};
void(string dir) AGClaimProtectionProbe =
{
    if (substring(dir,0,12) == "data/resume/") ag_claim_normal=ag_claim_normal+1;
    if (dir == "data/experimental/ag-100-v1/resume/ag_profile_test/@outer/save000/" ||
        dir == "data/experimental/ag-250-v1/resume/ag_profile_test/@outer/save000/")
    {
        ag_claim_outer=ag_claim_outer+1;
    }
};
void(string dir) AGClaimWipeProbe =
{
    if (strstrofs(dir,"/@fault/save000/",0) >= 0 || strstrofs(dir,"/@outer/save000/",0) >= 0)
        ag_claim_wipes=ag_claim_wipes+1;
};
void() AGClaimCheck =
{
	local float outer_before, protected100, protected250;

	if (find(world,classname,"player") != world) bprint(PRINT_HIGH,"AGCLAIM FAIL live actor\n");
	if (ag_claim_faults < 2 || ag_claim_neighbors < 2) bprint(PRINT_HIGH,"AGCLAIM FAIL guard not reached\n");
	if (ag_claim_outer || ag_claim_wipes || ag_claim_normal) bprint(PRINT_HIGH,"AGCLAIM FAIL preservation or optional scope\n");
	bprint(PRINT_HIGH,sprintf("AGCLAIM completed faults=%g neighbors=%g outer=%g wipes=%g normal=%g\n",ag_claim_faults,ag_claim_neighbors,ag_claim_outer,ag_claim_wipes,ag_claim_normal));
	// Separate direct-helper calls must not mask the caller's outer=0 proof.
	outer_before=ag_claim_outer;
	protected100=SV_MsSweepProtected("data/experimental/ag-100-v1/resume/ag_profile_test/@outer/save000/");
	protected250=SV_MsSweepProtected("data/experimental/ag-250-v1/resume/ag_profile_test/@outer/save000/");
	if (!protected100 || !protected250) bprint(PRINT_HIGH,"AGCLAIM FAIL singleton claim unprotected\n");
	if (ag_claim_outer != outer_before+2) bprint(PRINT_HIGH,"AGCLAIM FAIL singleton helper not reached\n");
	bprint(PRINT_HIGH,sprintf("AGCLAIM singleton protected100=%g protected250=%g calls=%g\n",protected100,protected250,ag_claim_outer-outer_before));
};
float ag_residual_swept;
void(entity p) AGResidualCleanupStep =
{
    local float offer;
    if (time < 17 || ag_residual_swept) return;
    ag_residual_swept=TRUE;
    offer=0;
    if (serverkey("*movementprofile") != "")
    {
        // Bootstrap claim protected this real @local slot during WorldSpawn.
        // Remove it now so the actual active-offer exclusion must act instead.
        fremove(strcat(SV_MsRoot(p),"/save000/ms.claim.txt"));
        offer=p.run_ms_offer; p.run_ms_offer=1;
    }
    SV_MsSweep();
    if (serverkey("*movementprofile") != "") p.run_ms_offer=offer;
    bprint(PRINT_HIGH,"AGRESIDUAL resume sweep PASS\n");
};
float ag_review_phase;
void(entity p, float restored) AGReviewServerStep =
{
    local string text;
    local float rows;
    if (!restored || time < 17) return;
    text = SV_AGWaterSave(p);
    if (ag_review_phase == 0)
    {
        if (SV_RewindWriteState(p,FS_MAXSAVES,123,1) || SV_SaveWriteAt(p,FS_MAXSAVES,123,1))
            bprint(PRINT_HIGH,"AGSOCKET FAIL unsupported historic disk export admitted\n");
        cvar_set("rec_runs_keep", "2");
        SV_RecPrune(p, 1, "run", "", "0000002_run.rec");
        cvar_set("run_resume_days", "30"); SV_MsSweep();
        p.run_pmhold=1; p.run_agwateractive=1; p.run_agwatertime=0;
        p.run_agwaterpush='50 -5 0'; p.run_forceagwater=1;
    }
    else if (ag_review_phase == 1)
    {
        if (strstrofs(text,"agwaterjump 1 0.000000000 50.000",0) != 0) bprint(PRINT_HIGH,"AGSOCKET FAIL active-zero native diagnostic\n");
        if (!SV_SaveWriteState(p,FS_MAXSAVES-2,77)) bprint(PRINT_HIGH,"AGSOCKET FAIL active-zero save\n");
        p.run_agwateractive=0; p.run_agwatertime=0; p.run_forceagwater=1;
    }
    else if (ag_review_phase == 2)
    {
        if (strstrofs(text,"agwaterjump 0",0) != 0) bprint(PRINT_HIGH,"AGSOCKET FAIL inactive-zero native diagnostic\n");
        if (!SV_SaveApplyState(p,FS_MAXSAVES-2,FALSE)) bprint(PRINT_HIGH,"AGSOCKET FAIL active-zero load\n");
        p.run_pmhold=1;
    }
    else if (ag_review_phase == 3)
    {
        if (strstrofs(text,"agwaterjump 1 0.000000000",0) != 0) bprint(PRINT_HIGH,"AGSOCKET FAIL active-zero restored\n");
        if (p.run_forceagwater) bprint(PRINT_HIGH,"AGSOCKET FAIL one-shot uncleared\n");
        rec_sl_seq[0]=SV_SaveReadLight(FS_MAXSAVES-2,0);
        SV_SaveDemote(0);
        rows=SL_StateSnapshot(FS_MAXSAVES-2);
        if (rows < 1 || rec_sl_tick[0] != SL_DEMOTED)
            bprint(PRINT_HIGH,"AGSOCKET FAIL demotion did not act\n");
        else if (!SL_StateProfileValid(rows)) bprint(PRINT_HIGH,"AGSOCKET FAIL demotion lost waterjump tuple\n");
        if (rows >= 1) buf_del(rows);
        p.run_agwateractive=0; p.run_agwatertime=1.5;
    }
    else if (ag_review_phase == 4)
    {
        if (strstrofs(text,"agwaterjump 1 0.000000000",0) != 0) bprint(PRINT_HIGH,"AGSOCKET FAIL one-shot replayed fields\n");
        p.run_agwateractive=1; p.run_agwatertime=1.5; p.run_forceagwater=1;
    }
    else if (ag_review_phase == 5)
    {
        if (strstrofs(text,"agwaterjump 1 1.500000000",0) != 0) bprint(PRINT_HIGH,"AGSOCKET FAIL positive timer\n");
        if (!SV_SaveWriteState(p,FS_MAXSAVES-3,78)) bprint(PRINT_HIGH,"AGSOCKET FAIL positive save\n");
        p.run_agwateractive=0; p.run_agwatertime=0; p.run_forceagwater=1;
    }
    else if (ag_review_phase == 6)
    {
        if (!SV_SaveApplyState(p,FS_MAXSAVES-3,FALSE)) bprint(PRINT_HIGH,"AGSOCKET FAIL positive load\n");
        p.run_pmhold=1;
    }
    else if (ag_review_phase == 7)
    {
        if (strstrofs(text,"agwaterjump 1 1.500000000",0) != 0) bprint(PRINT_HIGH,"AGSOCKET FAIL positive restored\n");
        p.run_pmhold=0; p.run_agwateractive=0; p.run_agwatertime=0; p.run_forceagwater=1;
        bprint(PRINT_HIGH,"AGREVIEW actual waterjump active/inactive zero + positive save/load one-shot PASS\n");
    }
    ag_review_phase=ag_review_phase+1;
};
