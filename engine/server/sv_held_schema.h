/* Fixed REC12 metadata fields; no native structure dumps. */
#ifndef SV_HELD_SCHEMA_H
#define SV_HELD_SCHEMA_H
typedef enum {
	HELD_RECORD_HCTX,
	HELD_RECORD_HINIT,
	HELD_RECORD_HSCOPE,
	HELD_RECORD_HPRE,
	HELD_RECORD_HMOVE,
	HELD_RECORD_HORDINARY,
	HELD_RECORD_HDONE,
	HELD_RECORD_HPOST,
	HELD_RECORD_HENTRY,
	HELD_RECORD_HMAINT,
	HELD_RECORD_HRELEASE,
	HELD_RECORD_HFREEZE,
	HELD_RECORD_HTHAW,
	HELD_RECORD_HOBS,
	HELD_RECORD_HPEND,
	HELD_RECORD_HTERMINAL,
	HELD_RECORD_HSEAL,
	HELD_RECORD_HFAIL,
	HELD_RECORD_COUNT
} held_record_family_t;
typedef struct { const char *name, *fields; unsigned int count; } held_schema_t;
static const held_schema_t held_schemas[HELD_RECORD_COUNT] = {
	{"hctx",
		"generation:G ordinal:L message:L execution:L context:U "
		"world:D zone:D engine:D plugin:D progs:D "
		"rules:U ctx_pmsrcver:U ctx_physmode:U ctx_pmtype:U ctx_gamespeed:F "
		"ctx_hullx0:F ctx_hully0:F ctx_hullx1:F ctx_hully1:F ctx_capsule:U "
		"ctx_svmaxvel:F ctx_trisoup:F ctx_rotboxes:F ctx_portalcsg:F ctx_gravity:F "
		"ctx_stopspeed:F ctx_maxspeed:F ctx_spectatormaxspeed:F ctx_accelerate:F ctx_airaccelerate:F "
		"ctx_wateraccelerate:F ctx_friction:F ctx_waterfriction:F ctx_flyfriction:F ctx_entgravity:F "
		"ctx_bunnyspeedcap:F ctx_watersinkspeed:F ctx_ktjump:F ctx_edgefriction:F ctx_walljump:U "
		"ctx_slidefix:U ctx_airstep:U ctx_pground:U ctx_stepdown:U ctx_slidyslopes:U "
		"ctx_autobunny:U ctx_bunnyfriction:U ctx_stepheight:I ctx_ticrate:F ctx_maxairspeed:F "
		"ctx_jumpvelocity:F ctx_standablenormal:F ctx_bounce:F ctx_maxvelocity:F ctx_standheight:F "
		"ctx_duckheight:F ctx_duckspeed:F ctx_viewheight:F ctx_duckviewheight:F ctx_noclipspeed:F "
		"ctx_stamina:F ctx_staminajumpcost:F ctx_staminalandcost:F ctx_staminarecovery:F ctx_normalizejump:F "
		"ctx_jumpaddrise:F ctx_jumpzoffset:F ctx_walkspeed:F ctx_groundtracedist:F ctx_bumpcount:F "
		"ctx_snaptoground:F ctx_groundquadrants:F ctx_fixslopes:F ctx_fixedges:F ctx_fixrampbugs:F "
		"ctx_rampretrace:F ctx_viewscale:F ctx_ladders:F ctx_ladderdampen:F ctx_ladderangle:F "
		"ctx_slide:F ctx_hullmins_z:F ctx_hullmaxs_z:F ctx_hullnum:I ctx_geomtype:I "
		"ctx_coordtype:U ctx_flags:U ctx_gravitydir_x:F ctx_gravitydir_y:F ctx_gravitydir_z:F "
		"ctx_numphysent:U ctx_trisoup_i:I ctx_rotboxes_i:I ctx_portalcsg_i:I ctx_excuse:F "
		"ctx_slow:F ctx_hyst:F ctx_allow:F ctx_freecredits:F ctx_gap:F",
		100},
	{"hinit",
		"generation:G ordinal:L message:L execution:L context:U "
		"origin_x:F origin_y:F origin_z:F velocity_x:F velocity_y:F "
		"velocity_z:F safeorigin_x:F safeorigin_y:F safeorigin_z:F safeorigin_known:U "
		"ground:U groundent:I hullmins_x:F hullmins_y:F hullmins_z:F "
		"hullmaxs_x:F hullmaxs_y:F hullmaxs_z:F pmtype:U jump_held:U "
		"jump_msec:I waterjumptime:F gravitydir_x:F gravitydir_y:F gravitydir_z:F "
		"basevelocity_x:F basevelocity_y:F basevelocity_z:F onladder:U capsule:U "
		"surfacefriction:F ducktime:F ducking:U ducked:U msec_carry:F "
		"oldbuttons:I stamina:F rampoff:F boardcount:F rampcontact:F "
		"srcladder:U srcladdernormal_x:F srcladdernormal_y:F srcladdernormal_z:F portaloffset_x:F "
		"portaloffset_y:F portaloffset_z:F viewheight:F waterlevel:U watertype:I "
		"raw_moveticks:L tickused:F portalcrossings:U numtouch:U clock_starttick:F "
		"clock_startsec:F clock_startwall:F clock_tickcarry:F clock_lastmt:F clock_lastsec:F "
		"clock_freeze:F clock_frzon:F clock_frzat:F clock_frzmt:F clock_frzcarry:F "
		"clock_frzticks:F clock_frzsec:F clock_state:F clock_ticks:F clock_elapsed:F "
		"clock_ckwall:F clock_ckwt:F clock_ckgaps:F clock_ckncr:F clock_ckcb:F "
		"clock_ckcr:F clock_ckd:F clock_cktr:F clock_ckpk:F clock_ckup:F "
		"clock_cku:F clock_ckskip:F clock_resumesec:F clock_lastorg_x:F clock_lastorg_y:F "
		"clock_lastorg_z:F clock_packet:F clock_counted:F clock_flags:F clock_frame:F",
		95},
	{"hscope",
		"generation:G ordinal:L message:L execution:L kind:U "
		"phase:U time:F frametime:F",
		8},
	{"hpre",
		"generation:G ordinal:L message:L execution:L phase:U "
		"time:F frametime:F",
		7},
	{"hmove",
		"generation:G ordinal:L message:L execution:L effective_msec:F "
		"gamespeed:F context:U pmtype:U ownerkind:U ownerindex:U",
		10},
	{"hordinary",
		"generation:G ordinal:L message:L execution:L ownerkind:U "
		"ownerindex:U context:U pmtype:U",
		8},
	{"hdone",
		"generation:G ordinal:L message:L execution:L ticksrun:U "
		"carry:F raw_moveticks:L",
		7},
	{"hpost",
		"generation:G ordinal:L message:L execution:L phase:U "
		"time:F frametime:F",
		7},
	{"hentry",
		"generation:G ordinal:L message:L execution:L saved_movetype:I",
		5},
	{"hmaint",
		"generation:G ordinal:L message:L execution:L site:U",
		5},
	{"hrelease",
		"generation:G ordinal:L message:L execution:L restored_movetype:I",
		5},
	{"hfreeze",
		"generation:G ordinal:L message:L execution:L clock_starttick:F "
		"clock_startsec:F clock_startwall:F clock_tickcarry:F clock_lastmt:F clock_lastsec:F "
		"clock_freeze:F clock_frzon:F clock_frzat:F clock_frzmt:F clock_frzcarry:F "
		"clock_frzticks:F clock_frzsec:F clock_state:F clock_ticks:F clock_elapsed:F "
		"clock_ckwall:F clock_ckwt:F clock_ckgaps:F clock_ckncr:F clock_ckcb:F "
		"clock_ckcr:F clock_ckd:F clock_cktr:F clock_ckpk:F clock_ckup:F "
		"clock_cku:F clock_ckskip:F clock_resumesec:F clock_lastorg_x:F clock_lastorg_y:F "
		"clock_lastorg_z:F clock_packet:F clock_counted:F clock_flags:F clock_frame:F",
		40},
	{"hthaw",
		"generation:G ordinal:L message:L execution:L clock_starttick:F "
		"clock_startsec:F clock_startwall:F clock_tickcarry:F clock_lastmt:F clock_lastsec:F "
		"clock_freeze:F clock_frzon:F clock_frzat:F clock_frzmt:F clock_frzcarry:F "
		"clock_frzticks:F clock_frzsec:F clock_state:F clock_ticks:F clock_elapsed:F "
		"clock_ckwall:F clock_ckwt:F clock_ckgaps:F clock_ckncr:F clock_ckcb:F "
		"clock_ckcr:F clock_ckd:F clock_cktr:F clock_ckpk:F clock_ckup:F "
		"clock_cku:F clock_ckskip:F clock_resumesec:F clock_lastorg_x:F clock_lastorg_y:F "
		"clock_lastorg_z:F clock_packet:F clock_counted:F clock_flags:F clock_frame:F",
		40},
	{"hobs",
		"generation:G ordinal:L message:L execution:L boundary:U "
		"origin_x:F origin_y:F origin_z:F velocity_x:F velocity_y:F "
		"velocity_z:F ground:U hullmins_x:F hullmins_y:F hullmins_z:F "
		"hullmaxs_x:F hullmaxs_y:F hullmaxs_z:F raw_moveticks:L tickused:F "
		"msec_carry:F surfacefriction:F ducktime:F ducking:U ducked:U "
		"stamina:F rampoff:F boardcount:F rampcontact:F srcladder:U "
		"srcladdernormal_x:F srcladdernormal_y:F srcladdernormal_z:F portaloffset_x:F portaloffset_y:F "
		"portaloffset_z:F",
		36},
	{"hpend",
		"generation:G ordinal:L message:L execution:L last_execution:L",
		5},
	{"hterminal",
		"generation:G ordinal:L message:L execution:L closekind:U "
		"clock_starttick:F clock_startsec:F clock_startwall:F clock_tickcarry:F clock_lastmt:F "
		"clock_lastsec:F clock_freeze:F clock_frzon:F clock_frzat:F clock_frzmt:F "
		"clock_frzcarry:F clock_frzticks:F clock_frzsec:F clock_state:F clock_ticks:F "
		"clock_elapsed:F clock_ckwall:F clock_ckwt:F clock_ckgaps:F clock_ckncr:F "
		"clock_ckcb:F clock_ckcr:F clock_ckd:F clock_cktr:F clock_ckpk:F "
		"clock_ckup:F clock_cku:F clock_ckskip:F clock_resumesec:F clock_lastorg_x:F "
		"clock_lastorg_y:F clock_lastorg_z:F clock_packet:F clock_counted:F clock_flags:F "
		"clock_frame:F",
		41},
	{"hseal",
		"generation:G ordinal:L message:L execution:L first_execution:L "
		"last_execution:L executions:L pre_enters:L pre_exits:L movers:L "
		"completions:L post_enters:L post_exits:L messages:L commands:L "
		"scene_fences:L control_events:L effect_dispatches:L ledger:D execution_ledger:D "
		"qualification:U",
		21},
	{"hfail",
		"generation:G status:U",
		2},
};
#endif
