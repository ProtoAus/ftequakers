/*
FTESurf Patch 645: the run lines' antialiased strip, natively. Included by pr_csqc.c.

FTESurf's cl_lines.qc draws a recorded run as a screen-space strip: each sample is projected,
mitred against its neighbours and emitted as two triangle fans (Line_Feed, Line_Q, Line_Emit).
In QC that is some 250 instructions and 14 builtin calls a segment, about a microsecond; eight
lines with their graph cost 3 ms a frame. These builtins are that code in C, writing the same
fans into the same 2D polygon batch R_BeginPolygon("", DRAWFLAG_TWOSIDED, TRUE) fills. QC
keeps its own loop for an engine without them, so the two must stay the same picture: a change
to one is a change to the other (FTESurf tools/test_p645lines.py compares them pixel for pixel).

  lines_width(hw, cw, pk, px)                  the half widths Line_Width computed, and a pixel
  lines_segment(a, sa, ca, la, b, sb, cb, lb)  Line_Emit: one segment, sides given
  lines_strip2d(pts, n, org, scale, rgb, a)    a strip through n (x, y, starts-a-strip) triples: a plot's curve
  lines_arrays / lines_lod                     where a VM keeps its samples and their LOD tables
  lines_draw(parm, n)                          Line_Draw's pass over one slot; 0 = not drawn
  lines_label(parm, n)                         Patch 833: where a line's name may sit, its first look

QC pointers are kept as QC pointers and resolved, with their sizes, inside each call; every
index a pass can reach is checked before it starts. The numbers are trusted to be numbers: a
NaN draws nonsense, it does not read out of bounds.
*/

enum
{
	LP_VERSION,		//1
	LP_BASE,		//the slot's first sample in the arrays
	LP_FIRST,		//first and last sample to draw, counted from the slot's first
	LP_LAST,
	LP_COUNT,		//samples the slot holds
	LP_PUBLISHED,	//samples its LOD tables cover; the rest are an unbuilt tail
	LP_FLAGS,		//LPF_*
	LP_BORNBASE,	//the slot's first entry in the birth array
	LP_CHUNKBASE,
	LP_GROUPBASE,
	LP_TPLAY,		//points after it draw at `ahead`
	LP_AHEAD,
	LP_ALPHA,
	LP_TINT,		//blend toward LP_TINTCOL
	LP_PHEAD,		//3: where the playhead is
	LP_TINTCOL = LP_PHEAD+3,	//3
	LP_WINA = LP_TINTCOL+3,		//the shown window of the run clock, and its two fade slopes
	LP_WINB,
	LP_WINKA,
	LP_WINKB,
	LP_REVEAL,
	LP_CLTIME,
	LP_EYE,			//3
	LP_FWD = LP_EYE+3,	//3
	LP_RT = LP_FWD+3,	//3
	LP_UP = LP_RT+3,	//3
	LP_TX = LP_UP+3,	//tan of the half fovs, and the frustum sides' lengths
	LP_TY,
	LP_KX,
	LP_KY,
	LP_TOLK,		//error a stride may have, per unit of distance
	LP_FAR,
	LP_FADEK,
	LP_CHUNK,		//samples a chunk, chunks a group, LOD levels a chunk
	LP_GROUP,
	LP_LEVELS,
	LP_NEAR,
	LP_GUARD,
	LP_SUBPX,
	//in and out: the frame's counters, as Line_StatsReset left them or the last pass did
	LP_NPT,
	LP_NSEG,
	LP_NWILD,
	LP_NCLIP,
	LP_BBMIN,		//2
	LP_BBMAX = LP_BBMIN+2,	//2
	LP_FIRSTPT = LP_BBMAX+2,	//2
	LP_CFADE = LP_FIRSTPT+2,
	LP_SIZE
};
#define LPF_DEMO	1	//alpha rises behind the playhead over `reveal` seconds
#define LPF_BORN	2	//... and with each sample's age
#define LPF_WINDOW	4
#define LPF_CFADE	8	//the last sphere test before this pass reached the fade distance

void *PR_PointerToNative_MoInvalidate(pubprogfuncs_t *inst, pint_t ptr, size_t offset, size_t datasize);

static struct
{
	pubprogfuncs_t *vm;
	int pos, col, t, brk, born;		//QC pointers
	unsigned int samples, bornsamples;
	int cc, cr, cerr, gc, gr;
	unsigned int chunks, groups, levels;
	float hw, cw, pk, px;
} csqc_lines;

static void CSQC_Lines_Reset(void)
{
	memset(&csqc_lines, 0, sizeof(csqc_lines));
	csqc_lines.px = 1;
}

//QC memory is read through memcpy: a QC pointer need not be aligned.
static float LinesF(const char *base, unsigned int i)
{
	float f;
	memcpy(&f, base + i*sizeof(float), sizeof(f));
	return f;
}
static void LinesV(const char *base, unsigned int i, float *out)
{
	memcpy(out, base + i*3*sizeof(float), 3*sizeof(float));
}
static float LinesBound(float lo, float v, float hi)
{
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

//what R_BeginPolygon("", DRAWFLAG_TWOSIDED, TRUE) does
static void Lines_Open(void)
{
	extern shader_t *shader_draw_fill_trans;
	const int beflags = BEF_NOSHADOWS|BEF_FORCETWOSIDED;

	if (R2D_Flush && (R2D_Flush != CSQC_PolyFlush || csqc_poly_shader != shader_draw_fill_trans || csqc_poly_flags != beflags || !csqc_poly_2d))
		R2D_Flush();
	if (!R2D_Flush)
	{
		csqc_poly_origvert = cl_numstrisvert;
		csqc_poly_origidx = cl_numstrisidx;
	}
	R2D_Flush = CSQC_PolyFlush;
	csqc_poly_shader = shader_draw_fill_trans;
	csqc_poly_flags = beflags;
	csqc_poly_2d = true;
	csqc_poly_startvert = cl_numstrisvert;
	csqc_poly_startidx = cl_numstrisidx;
}

//nv R_PolygonVertex calls and an R_EndPolygon: one fan
static void Lines_Fan(float (*xyz)[3], float (*rgba)[4], int nv)
{
	int i, first;

	Lines_Open();
	if ((cl_numstrisvert - csqc_poly_origvert) + nv >= 32768)
	{	//PF_R_PolygonEnd's limit on a batch
		CSQC_PolyFlush();
		Lines_Open();
	}
	if (cl_numstrisvert + nv > cl_maxstrisvert)
		cl_stris_ExpandVerts(cl_numstrisvert + nv + 4096);
	if (cl_numstrisidx + (nv-2)*3 > cl_maxstrisidx)
	{
		cl_maxstrisidx = cl_numstrisidx + (nv-2)*3 + 12288;
		cl_strisidx = BZ_Realloc(cl_strisidx, sizeof(*cl_strisidx)*cl_maxstrisidx);
	}
	first = cl_numstrisvert - csqc_poly_origvert;
	for (i = 0; i < nv; i++)
	{
		VectorCopy(xyz[i], cl_strisvertv[cl_numstrisvert]);
		Vector2Set(cl_strisvertt[cl_numstrisvert], 0, 0);
		Vector4Copy(rgba[i], cl_strisvertc[cl_numstrisvert]);
		cl_numstrisvert++;
	}
	for (i = 2; i < nv; i++)
	{
		cl_strisidx[cl_numstrisidx++] = first;
		cl_strisidx[cl_numstrisidx++] = first + i-1;
		cl_strisidx[cl_numstrisidx++] = first + i;
	}
	csqc_poly_startvert = cl_numstrisvert;
	csqc_poly_startidx = cl_numstrisidx;
}

#define LINES_V(i, P, S, K, C, A) \
	(xyz[i][0] = (P)[0] + (S)[0]*(K), xyz[i][1] = (P)[1] + (S)[1]*(K), xyz[i][2] = (P)[2] + (S)[2]*(K), \
	 rgba[i][0] = (C)[0], rgba[i][1] = (C)[1], rgba[i][2] = (C)[2], rgba[i][3] = (A))

//cl_lines.qc Line_Emit: a tent at one pixel, a trapezoid (two fans) wider; outer vertices carry alpha 0
static void Lines_Emit(const float *a, const float *sa, const float *ca, float la, const float *b, const float *sb, const float *cb, float lb)
{
	float xyz[6][3], rgba[6][4];
	const float hw = csqc_lines.hw, cw = csqc_lines.cw, pk = csqc_lines.pk;

	if (cw <= 0)
	{
		LINES_V(0, a, sa, 0, ca, la * pk);
		LINES_V(1, a, sa, hw, ca, 0);
		LINES_V(2, b, sb, hw, cb, 0);
		LINES_V(3, b, sb, 0, cb, lb * pk);
		LINES_V(4, b, sb, -hw, cb, 0);
		LINES_V(5, a, sa, -hw, ca, 0);
		Lines_Fan(xyz, rgba, 6);
		return;
	}
	LINES_V(0, a, sa, cw, ca, la);
	LINES_V(1, a, sa, hw, ca, 0);
	LINES_V(2, b, sb, hw, cb, 0);
	LINES_V(3, b, sb, cw, cb, lb);
	LINES_V(4, b, sb, -cw, cb, lb);
	LINES_V(5, a, sa, -cw, ca, la);
	Lines_Fan(xyz, rgba, 6);
	LINES_V(0, a, sa, -cw, ca, la);
	LINES_V(1, b, sb, -cw, cb, lb);
	LINES_V(2, b, sb, -hw, cb, 0);
	LINES_V(3, a, sa, -hw, ca, 0);
	Lines_Fan(xyz, rgba, 4);
}

typedef struct
{
	//the strip being streamed (Line_Q): the last two screen points, the pending side
	int rn;
	float rq0[3], rs0[3], rc0[3], ra0;
	float rq1[3], rc1[3], rn1[3], ra1;
	float npt, nseg, nwild, nclip;
	float bbmin[2], bbmax[2], first[2];

	//the pass over a slot (Line_Draw, Line_Feed)
	const char *pos, *col, *t, *brk, *born, *cc, *cr, *cerr, *gc, *gr;
	int b, n, published, bornbase, chunkbase, groupbase, chunk, group, levels;
	qboolean demo, bornrev, winon, cfade;
	float tplay, ahead, alpha, tint;
	vec3_t phead, tintcol;
	float wina, winb, winka, winkb, reveal, cltime;
	vec3_t eye, fwd, rt, up;
	float tx, ty, kx, ky, tolk, farz, fadek;
	float nearz, guard, subpx;
	qboolean hp;		//the world point before, for the near clip and the subdivision
	int pi;
	float pd, pa;
	vec3_t pp, pc;
} linesctx_t;

static void Lines_RunEnd(linesctx_t *L)
{
	if (L->rn >= 2)
	{
		Lines_Emit(L->rq0, L->rs0, L->rc0, L->ra0, L->rq1, L->rn1, L->rc1, L->ra1);
		L->nseg += 1;
	}
	L->rn = 0;
}

//Line_Q: a screen point onto the strip. A segment is emitted a point late, when the mitre at its
//far end is known; a point under a quarter pixel from the last is dropped.
static void Lines_Q(linesctx_t *L, const float *q, const float *c, float a)
{
	float d[2], n[3], m[3], l, k;

	L->npt += 1;
	if (q[0] < L->bbmin[0]) L->bbmin[0] = q[0];
	if (q[1] < L->bbmin[1]) L->bbmin[1] = q[1];
	if (q[0] > L->bbmax[0]) L->bbmax[0] = q[0];
	if (q[1] > L->bbmax[1]) L->bbmax[1] = q[1];
	if (L->rn > 0)
	{
		d[0] = q[0] - L->rq1[0];
		d[1] = q[1] - L->rq1[1];
		l = sqrt(d[0]*d[0] + d[1]*d[1]);
		if (l < 0.25f * csqc_lines.px)
			return;
		n[0] = d[1] * (1 / l);
		n[1] = (0 - d[0]) * (1 / l);
		n[2] = 0;
		if (L->rn == 1)
			VectorCopy(n, L->rs0);
		else
		{
			m[0] = L->rn1[0] + n[0];
			m[1] = L->rn1[1] + n[1];
			m[2] = 0;
			l = sqrt(m[0]*m[0] + m[1]*m[1]);
			if (l < 0.1f)
				VectorCopy(n, m);	//doubled back: no mitre to take
			else
			{
				m[0] *= 1 / l;
				m[1] *= 1 / l;
				k = m[0]*n[0] + m[1]*n[1];
				k = 1 / ((k > 0.5f) ? k : 0.5f);
				m[0] *= k;
				m[1] *= k;
			}
			Lines_Emit(L->rq0, L->rs0, L->rc0, L->ra0, L->rq1, m, L->rc1, L->ra1);
			L->nseg += 1;
			VectorCopy(m, L->rs0);
		}
		VectorCopy(L->rq1, L->rq0);
		VectorCopy(L->rc1, L->rc0);
		L->ra0 = L->ra1;
		VectorCopy(n, L->rn1);
		L->rn = 2;
	}
	else
	{
		L->rn = 1;
		if (L->nseg == 0)
		{
			L->first[0] = q[0];
			L->first[1] = q[1];
		}
	}
	L->rq1[0] = q[0];
	L->rq1[1] = q[1];
	L->rq1[2] = 0;
	VectorCopy(c, L->rc1);
	L->ra1 = a;
}

//PF_cs_project's arithmetic
static void Lines_Project(const float *in, float *out)
{
	float v[4], tempv[4];

	v[0] = in[0];
	v[1] = in[1];
	v[2] = in[2];
	v[3] = 1;
	Matrix4x4_CM_Transform4(csqc_proj_matrix, v, tempv);
	tempv[0] /= tempv[3];
	tempv[1] /= tempv[3];
	tempv[2] /= tempv[3];
	out[0] = (1+tempv[0])/2;
	out[1] = 1-(1+tempv[1])/2;
	out[2] = tempv[2];
	out[0] = out[0]*r_refdef.vrect.width + r_refdef.vrect.x;
	out[1] = out[1]*r_refdef.vrect.height + r_refdef.vrect.y;
	if (tempv[3] < 0)
		out[2] *= -1;
}

//Line_Sane: false for a near-plane stub beside the eye (the strip is ended there)
static qboolean Lines_Sane(linesctx_t *L, const float *q)
{
	if (q[2] < -1 || q[0] > L->guard || q[0] < -L->guard || q[1] > L->guard || q[1] < -L->guard)
	{
		L->nwild += 1;
		Lines_RunEnd(L);
		return false;
	}
	return true;
}

//Line_Pt: a world point in front of the near plane, onto the strip
static void Lines_Pt(linesctx_t *L, const float *w, const float *c, float a)
{
	float q[3];

	Lines_Project(w, q);
	if (!Lines_Sane(L, q))
		return;
	Lines_Q(L, q, c, a);
}

//Line_Sphere: -1 outside the frustum or past the far distance, else the distance to its surface
static float Lines_Sphere(linesctx_t *L, const float *cen, float rad)
{
	vec3_t r;
	float z, x;

	VectorSubtract(cen, L->eye, r);
	z = DotProduct(r, L->fwd);
	if (z < 0 - rad)
		return -1;
	x = DotProduct(r, L->rt);
	if (x < 0) x = 0 - x;
	if (x - z * L->tx > rad * L->kx)
		return -1;
	x = DotProduct(r, L->up);
	if (x < 0) x = 0 - x;
	if (x - z * L->ty > rad * L->ky)
		return -1;
	x = sqrt(DotProduct(r, r));
	L->cfade = (L->farz > 0 && x + rad > L->farz * 0.66f);
	x = x - rad;
	if (x < 0)
		return 0;
	if (L->farz > 0)
		if (x > L->farz)
			return -1;
	return x;
}

//Line_ChunkStep: 0 off screen, else the stride whose error is under the tolerance at its distance
static int Lines_ChunkStep(linesctx_t *L, int c)
{
	vec3_t cen;
	float tol;
	int k = L->chunkbase + c, l;

	LinesV(L->cc, k, cen);
	tol = Lines_Sphere(L, cen, LinesF(L->cr, k));
	if (tol < 0)
		return 0;
	tol = tol * L->tolk;
	k = k * L->levels;
	for (l = L->levels; l >= 1; l--)
		if (LinesF(L->cerr, k + l - 1) <= tol)
			return 1 << l;
	return 1;
}

//Line_Reveal
static float Lines_Reveal(linesctx_t *L, int i, float t, float tplay, float ahead)
{
	float a, f;

	a = (t <= tplay) ? 1 : ahead;
	if (L->reveal <= 0)
		return a;
	if (L->demo)
	{
		f = LinesBound(0, (tplay - t) / L->reveal, 1);
		a = ahead + (1 - ahead) * f;
	}
	if (L->bornrev)
	{
		f = LinesBound(0, (L->cltime - LinesF(L->born, L->bornbase + i)) / L->reveal, 1);
		a = a * f;
	}
	return a;
}

//Line_Feed: sample i onto the strip: the near clip against the point before, the subdivision of
//a long segment, and the playhead split
static void Lines_Feed(linesctx_t *L, int i, int st)
{
	vec3_t p, c, x, pm, pn, w, wc, r;
	float q1[3];
	float d, a, f, k, l, t, tp;
	int j, sub, v;

	LinesV(L->pos, L->b + i, p);
	VectorSubtract(p, L->eye, r);
	d = DotProduct(r, L->fwd);
	LinesV(L->col, L->b + i, c);
	if (L->tint)
		for (v = 0; v < 3; v++)
			c[v] = c[v] + (L->tintcol[v] - c[v]) * L->tint;
	t = LinesF(L->t, L->b + i);
	a = Lines_Reveal(L, i, t, L->tplay, L->ahead) * L->alpha;
	if (L->cfade)
	{
		l = sqrt(DotProduct(r, r));		//a float before the subtraction, as QC's vlen is
		a = a * LinesBound(0, (L->farz - l) * L->fadek, 1);
	}
	if (L->winon)
	{
		f = (t - L->wina) * L->winka;
		k = (L->winb - t) * L->winkb;
		a = a * LinesBound(0, (f < k) ? f : k, 1);
	}

	if (!L->hp)
	{
		if (d >= L->nearz)
			Lines_Pt(L, p, c, a);
	}
	else if (L->pd >= L->nearz && d >= L->nearz)
	{
		//the playhead between the two: the line bright to it, dim after (or ended there)
		tp = LinesF(L->t, L->b + L->pi);
		if (tp <= L->tplay && t > L->tplay)
		{
			f = t - tp;
			f = (L->tplay - tp) / ((f > 0.0001f) ? f : 0.0001f);
			k = L->alpha;
			if (L->reveal > 0)
				k = Lines_Reveal(L, i, L->tplay, L->tplay, L->ahead) * L->alpha;
			for (v = 0; v < 3; v++)
				wc[v] = L->pc[v] + (c[v] - L->pc[v]) * f;
			Lines_Pt(L, L->phead, wc, k);
			if (L->ahead <= 0)
			{
				Lines_RunEnd(L);
				L->hp = false;
				return;
			}
			st = 0;		//no curve back over the playhead
		}

		//a long segment on screen: Catmull-Rom through the neighbours (none across a break),
		//and only at full resolution
		Lines_Project(p, q1);
		if (!Lines_Sane(L, q1))
			goto keep;
		sub = 1;
		if (st == 1)
			if (L->rn > 0)
			{
				l = sqrt((q1[0]-L->rq1[0])*(q1[0]-L->rq1[0]) + (q1[1]-L->rq1[1])*(q1[1]-L->rq1[1]));
				if (l > L->subpx)
				{
					f = l / (L->subpx * 0.5f);
					f = ceil(f);
					sub = (f < 8) ? (int)f : 8;
				}
			}
		if (sub > 1)
		{
			if (L->pi > 0 && !LinesF(L->brk, L->b + L->pi))
				LinesV(L->pos, L->b + L->pi - 1, pm);
			else
				for (v = 0; v < 3; v++)
					pm[v] = 2 * L->pp[v] - p[v];
			for (v = 0; v < 3; v++)
				pn[v] = 2 * p[v] - L->pp[v];
			if (i + 1 < L->n)
				if (!LinesF(L->brk, L->b + i + 1))
					LinesV(L->pos, L->b + i + 1, pn);
			for (j = 1; j < sub; j++)
			{
				f = (float)j / sub;
				k = f * f;
				for (v = 0; v < 3; v++)
				{
					w[v] = 0.5 * ((2 * L->pp[v]) + (p[v] - pm[v]) * f
						+ (2 * pm[v] - 5 * L->pp[v] + 4 * p[v] - pn[v]) * k
						+ (3 * L->pp[v] - pm[v] - 3 * p[v] + pn[v]) * k * f);
					wc[v] = L->pc[v] + (c[v] - L->pc[v]) * f;
				}
				VectorSubtract(w, L->eye, r);
				if (DotProduct(r, L->fwd) >= L->nearz)
					Lines_Pt(L, w, wc, L->pa + (a - L->pa) * f);
			}
		}
		Lines_Q(L, q1, c, a);
	}
	else if (L->ahead <= 0 && t > L->tplay)
	{
		Lines_RunEnd(L);		//ahead 0: nothing past the playhead
		L->nclip += 1;
	}
	else if (L->pd >= L->nearz)
	{
		f = (L->pd - L->nearz) / (L->pd - d);
		for (v = 0; v < 3; v++)
			x[v] = L->pp[v] + (p[v] - L->pp[v]) * f;
		Lines_Pt(L, x, c, a);
		Lines_RunEnd(L);
		L->nclip += 1;
	}
	else if (d >= L->nearz)
	{
		f = (d - L->nearz) / (d - L->pd);
		for (v = 0; v < 3; v++)
			x[v] = p[v] + (L->pp[v] - p[v]) * f;
		Lines_Pt(L, x, L->pc, L->pa);
		Lines_Pt(L, p, c, a);
		L->nclip += 1;
	}
	else
		L->nclip += 1;

keep:
	L->hp = true;
	L->pi = i;
	VectorCopy(p, L->pp);
	L->pd = d;
	VectorCopy(c, L->pc);
	L->pa = a;
}

//void(float hw, float cw, float pk, float px) lines_width
static void QCBUILTIN PF_cs_lines_width(pubprogfuncs_t *prinst, struct globalvars_s *pr_globals)
{
	csqc_lines.hw = G_FLOAT(OFS_PARM0);
	csqc_lines.cw = G_FLOAT(OFS_PARM1);
	csqc_lines.pk = G_FLOAT(OFS_PARM2);
	csqc_lines.px = G_FLOAT(OFS_PARM3);
}

//void(vector a, vector sa, vector ca, float la, vector b, vector sb, vector cb, float lb) lines_segment
static void QCBUILTIN PF_cs_lines_segment(pubprogfuncs_t *prinst, struct globalvars_s *pr_globals)
{
	Lines_Emit(G_VECTOR(OFS_PARM0), G_VECTOR(OFS_PARM1), G_VECTOR(OFS_PARM2), G_FLOAT(OFS_PARM3),
		G_VECTOR(OFS_PARM4), G_VECTOR(OFS_PARM5), G_VECTOR(OFS_PARM6), G_FLOAT(OFS_PARM7));
}

//float(float *pts, float n, vector org, vector scale, vector rgb, float alpha) lines_strip2d
//n triples (x, y, starts-a-strip); a point is [org_x + (x - org_z) * scale_x, org_y + (y - scale_z) * scale_y],
//which is a plot's own arithmetic (cl_linegraph.qc LineGraph_Curve). Returns the segments drawn.
static void QCBUILTIN PF_cs_lines_strip2d(pubprogfuncs_t *prinst, struct globalvars_s *pr_globals)
{
	float n = G_FLOAT(OFS_PARM1);
	const float *org = G_VECTOR(OFS_PARM2), *scale = G_VECTOR(OFS_PARM3);
	const float *rgb = G_VECTOR(OFS_PARM4);
	float alpha = G_FLOAT(OFS_PARM5);
	const char *pts;
	linesctx_t L;
	unsigned int i, count;
	float q[3];

	G_FLOAT(OFS_RETURN) = 0;
	if (!(n >= 1 && n <= 1048576))
		return;
	count = (unsigned int)n;
	pts = PR_PointerToNative_MoInvalidate(prinst, G_INT(OFS_PARM0), 0, count * 3 * sizeof(float));
	if (!pts)
		return;
	memset(&L, 0, sizeof(L));
	L.nseg = 1;		//`first` is the world line's and not wanted here
	q[2] = 0;
	for (i = 0; i < count; i++)
	{
		if (LinesF(pts, i*3 + 2))
			Lines_RunEnd(&L);
		q[0] = org[0] + (LinesF(pts, i*3) - org[2]) * scale[0];
		q[1] = org[1] + (LinesF(pts, i*3 + 1) - scale[2]) * scale[1];
		Lines_Q(&L, q, rgb, alpha);
	}
	Lines_RunEnd(&L);
	G_FLOAT(OFS_RETURN) = L.nseg - 1;
}

//void(vector *pos, vector *col, float *t, float *brk, float *born, float samples, float bornsamples) lines_arrays
static void QCBUILTIN PF_cs_lines_arrays(pubprogfuncs_t *prinst, struct globalvars_s *pr_globals)
{
	float samples = G_FLOAT(OFS_PARM5), born = G_FLOAT(OFS_PARM6);

	csqc_lines.vm = NULL;
	if (!(samples >= 1 && samples <= 16777216) || !(born >= 0 && born <= 16777216))
		return;
	csqc_lines.pos = G_INT(OFS_PARM0);
	csqc_lines.col = G_INT(OFS_PARM1);
	csqc_lines.t = G_INT(OFS_PARM2);
	csqc_lines.brk = G_INT(OFS_PARM3);
	csqc_lines.born = G_INT(OFS_PARM4);
	csqc_lines.samples = (unsigned int)samples;
	csqc_lines.bornsamples = (unsigned int)born;
	csqc_lines.chunks = csqc_lines.groups = csqc_lines.levels = 0;
	csqc_lines.vm = prinst;
}

//void(vector *cc, float *cr, float *cerr, vector *gc, float *gr, float chunks, float groups, float levels) lines_lod
static void QCBUILTIN PF_cs_lines_lod(pubprogfuncs_t *prinst, struct globalvars_s *pr_globals)
{
	float chunks = G_FLOAT(OFS_PARM5), groups = G_FLOAT(OFS_PARM6), levels = G_FLOAT(OFS_PARM7);

	csqc_lines.chunks = csqc_lines.groups = csqc_lines.levels = 0;
	if (csqc_lines.vm != prinst)
		return;
	if (!(chunks >= 1 && chunks <= 16777216) || !(groups >= 1 && groups <= 16777216) || !(levels >= 1 && levels <= 8))
		return;
	csqc_lines.cc = G_INT(OFS_PARM0);
	csqc_lines.cr = G_INT(OFS_PARM1);
	csqc_lines.cerr = G_INT(OFS_PARM2);
	csqc_lines.gc = G_INT(OFS_PARM3);
	csqc_lines.gr = G_INT(OFS_PARM4);
	csqc_lines.chunks = (unsigned int)chunks;
	csqc_lines.groups = (unsigned int)groups;
	csqc_lines.levels = (unsigned int)levels;
}

static int LinesInt(const char *parm, int i, int lo, int hi, qboolean *bad)
{
	float f = LinesF(parm, i);
	if (!(f >= lo && f <= hi) || f != (int)f)
	{
		*bad = true;
		return lo;
	}
	return (int)f;
}

//float(float *parm, float count) lines_draw: Line_Draw's pass over one slot's samples.
//Returns 1 when it drew (counters written back), 0 when it refused and drew nothing.
static void QCBUILTIN PF_cs_lines_draw(pubprogfuncs_t *prinst, struct globalvars_s *pr_globals)
{
	char *parm;
	linesctx_t L;
	qboolean bad = false;
	int ia, ib, i, st, c, ce, ge, g, k, flags, turns;
	vec3_t cen;
	float f;

	G_FLOAT(OFS_RETURN) = 0;
	if (csqc_isdarkplaces || csqc_lines.vm != prinst || !csqc_lines.levels || G_FLOAT(OFS_PARM1) != LP_SIZE)
		return;
	parm = PR_PointerToNative_MoInvalidate(prinst, G_INT(OFS_PARM0), 0, LP_SIZE * sizeof(float));
	if (!parm || LinesF(parm, LP_VERSION) != 1)
		return;

	memset(&L, 0, sizeof(L));
	L.n = LinesInt(parm, LP_COUNT, 1, csqc_lines.samples, &bad);
	L.b = LinesInt(parm, LP_BASE, 0, csqc_lines.samples - 1, &bad);
	ia = LinesInt(parm, LP_FIRST, 0, 16777216, &bad);
	ib = LinesInt(parm, LP_LAST, 0, 16777216, &bad);
	L.published = LinesInt(parm, LP_PUBLISHED, 0, 16777216, &bad);
	flags = LinesInt(parm, LP_FLAGS, 0, 15, &bad);
	L.bornbase = LinesInt(parm, LP_BORNBASE, 0, 16777216, &bad);
	L.chunkbase = LinesInt(parm, LP_CHUNKBASE, 0, 16777216, &bad);
	L.groupbase = LinesInt(parm, LP_GROUPBASE, 0, 16777216, &bad);
	L.chunk = LinesInt(parm, LP_CHUNK, 1, 4096, &bad);	//4096 each: their product is an int index
	L.group = LinesInt(parm, LP_GROUP, 1, 4096, &bad);
	L.levels = LinesInt(parm, LP_LEVELS, 1, 8, &bad);
	if (bad || ia > ib || ib >= L.n || L.published > L.n || (unsigned int)L.b + L.n > csqc_lines.samples
		|| (unsigned int)L.levels != csqc_lines.levels || (1 << L.levels) > L.chunk
		|| (unsigned int)L.chunkbase + ib / L.chunk >= csqc_lines.chunks
		|| (unsigned int)L.groupbase + ib / (L.chunk * L.group) >= csqc_lines.groups)
		return;
	L.demo = !!(flags & LPF_DEMO);
	L.winon = !!(flags & LPF_WINDOW);
	L.cfade = !!(flags & LPF_CFADE);
	L.reveal = LinesF(parm, LP_REVEAL);
	//Line_Reveal reads a birth only for a slot that has them, with samples published and a reveal time
	L.bornrev = (flags & LPF_BORN) && L.published > 0 && L.reveal > 0;
	if (L.bornrev && (unsigned int)L.bornbase + ib >= csqc_lines.bornsamples)
		return;

	L.pos = PR_PointerToNative_MoInvalidate(prinst, csqc_lines.pos, 0, csqc_lines.samples * 3 * sizeof(float));
	L.col = PR_PointerToNative_MoInvalidate(prinst, csqc_lines.col, 0, csqc_lines.samples * 3 * sizeof(float));
	L.t = PR_PointerToNative_MoInvalidate(prinst, csqc_lines.t, 0, csqc_lines.samples * sizeof(float));
	L.brk = PR_PointerToNative_MoInvalidate(prinst, csqc_lines.brk, 0, csqc_lines.samples * sizeof(float));
	L.cc = PR_PointerToNative_MoInvalidate(prinst, csqc_lines.cc, 0, csqc_lines.chunks * 3 * sizeof(float));
	L.cr = PR_PointerToNative_MoInvalidate(prinst, csqc_lines.cr, 0, csqc_lines.chunks * sizeof(float));
	L.cerr = PR_PointerToNative_MoInvalidate(prinst, csqc_lines.cerr, 0, csqc_lines.chunks * csqc_lines.levels * sizeof(float));
	L.gc = PR_PointerToNative_MoInvalidate(prinst, csqc_lines.gc, 0, csqc_lines.groups * 3 * sizeof(float));
	L.gr = PR_PointerToNative_MoInvalidate(prinst, csqc_lines.gr, 0, csqc_lines.groups * sizeof(float));
	if (!L.pos || !L.col || !L.t || !L.brk || !L.cc || !L.cr || !L.cerr || !L.gc || !L.gr)
		return;
	if (L.bornrev)
	{
		L.born = PR_PointerToNative_MoInvalidate(prinst, csqc_lines.born, 0, csqc_lines.bornsamples * sizeof(float));
		if (!L.born)
			return;
	}

	L.tplay = LinesF(parm, LP_TPLAY);
	L.ahead = LinesF(parm, LP_AHEAD);
	L.alpha = LinesF(parm, LP_ALPHA);
	L.tint = LinesF(parm, LP_TINT);
	LinesV(parm + LP_PHEAD*sizeof(float), 0, L.phead);
	LinesV(parm + LP_TINTCOL*sizeof(float), 0, L.tintcol);
	L.wina = LinesF(parm, LP_WINA);
	L.winb = LinesF(parm, LP_WINB);
	L.winka = LinesF(parm, LP_WINKA);
	L.winkb = LinesF(parm, LP_WINKB);
	L.cltime = LinesF(parm, LP_CLTIME);
	LinesV(parm + LP_EYE*sizeof(float), 0, L.eye);
	LinesV(parm + LP_FWD*sizeof(float), 0, L.fwd);
	LinesV(parm + LP_RT*sizeof(float), 0, L.rt);
	LinesV(parm + LP_UP*sizeof(float), 0, L.up);
	L.tx = LinesF(parm, LP_TX);
	L.ty = LinesF(parm, LP_TY);
	L.kx = LinesF(parm, LP_KX);
	L.ky = LinesF(parm, LP_KY);
	L.tolk = LinesF(parm, LP_TOLK);
	L.farz = LinesF(parm, LP_FAR);
	L.fadek = LinesF(parm, LP_FADEK);
	L.nearz = LinesF(parm, LP_NEAR);
	L.guard = LinesF(parm, LP_GUARD);
	L.subpx = LinesF(parm, LP_SUBPX);
	L.npt = LinesF(parm, LP_NPT);
	L.nseg = LinesF(parm, LP_NSEG);
	L.nwild = LinesF(parm, LP_NWILD);
	L.nclip = LinesF(parm, LP_NCLIP);
	L.bbmin[0] = LinesF(parm, LP_BBMIN);
	L.bbmin[1] = LinesF(parm, LP_BBMIN+1);
	L.bbmax[0] = LinesF(parm, LP_BBMAX);
	L.bbmax[1] = LinesF(parm, LP_BBMAX+1);
	L.first[0] = LinesF(parm, LP_FIRSTPT);
	L.first[1] = LinesF(parm, LP_FIRSTPT+1);

	if (csqc_rebuildmatricies)
		cs_buildmatricies();

	//Line_Draw's loop
	i = ia;
	st = 1;
	c = 0;
	ce = -1;
	ge = -1;
	for (turns = ib - ia + 2; i <= ib && turns > 0; turns--)
	{
		//the unbuilt tail of a live line: no sphere or stride is its yet
		if (i >= L.published)
		{
			st = 1;
			ce = ge = ib;
			if (!L.hp && i > ia && !LinesF(L.brk, L.b + i))
				Lines_Feed(&L, i - 1, 1);
		}
		//a group of chunks off screen or past the far distance is skipped whole
		if (i > ge)
		{
			g = i / (L.chunk * L.group);
			ge = (g + 1) * L.chunk * L.group - 1;
			if (ge > L.published - 1) ge = L.published - 1;
			if (ge > ib) ge = ib;
			k = L.groupbase + g;
			LinesV(L.gc, k, cen);
			if (Lines_Sphere(&L, cen, LinesF(L.gr, k)) < 0)
			{
				Lines_RunEnd(&L);
				L.hp = false;
				i = ge + 1;
				ce = ge;
				continue;
			}
		}
		if (i > ce)
		{
			c = i / L.chunk;
			ce = (c + 1) * L.chunk - 1;
			if (ce > L.published - 1) ce = L.published - 1;
			if (ce > ib) ce = ib;
			st = Lines_ChunkStep(&L, c);
			if (!st)
			{
				Lines_RunEnd(&L);
				L.hp = false;
				i = ce + 1;
				continue;
			}
			//entering after a culled chunk: from the point before, unless a break
			if (!L.hp)
				if (i > ia)
					if (!LinesF(L.brk, L.b + i))
						Lines_Feed(&L, i - 1, st);
		}
		if (LinesF(L.brk, L.b + i))
		{
			Lines_RunEnd(&L);
			L.hp = false;
		}
		Lines_Feed(&L, i, st);
		if (L.ahead <= 0)
			if (LinesF(L.t, L.b + i) > L.tplay)
				break;
		//on the chunk's grid of st, and its last point (the error was measured so)
		if (i >= ce)
			i = ce + 1;
		else
		{
			k = c * L.chunk + ((i - c * L.chunk) / st + 1) * st;
			i = (k < ce) ? k : ce;
		}
	}
	Lines_RunEnd(&L);

	f = L.npt;		memcpy(parm + LP_NPT*sizeof(float), &f, sizeof(f));
	f = L.nseg;		memcpy(parm + LP_NSEG*sizeof(float), &f, sizeof(f));
	f = L.nwild;	memcpy(parm + LP_NWILD*sizeof(float), &f, sizeof(f));
	f = L.nclip;	memcpy(parm + LP_NCLIP*sizeof(float), &f, sizeof(f));
	memcpy(parm + LP_BBMIN*sizeof(float), L.bbmin, sizeof(L.bbmin));
	memcpy(parm + LP_BBMAX*sizeof(float), L.bbmax, sizeof(L.bbmax));
	memcpy(parm + LP_FIRSTPT*sizeof(float), L.first, sizeof(L.first));
	f = L.cfade;	memcpy(parm + LP_CFADE*sizeof(float), &f, sizeof(f));
	G_FLOAT(OFS_RETURN) = 1;
}

/*
FTESurf Patch 833: LineGraph_WorldLabels' first look along a line for where its name can sit. In
QC each of up to 256 samples a line costs a vlen and, for most, LineGraph_LabelAt -> Line_ProjPt ->
project(): with eight board lines, 0.48 ms a frame. This is that loop in the same order on the same
float arithmetic -- vlen is sqrt(DotProduct), project() is CS_ProjectPoint, and -ffp-contract=off
keeps the VM's own dot product unfused -- so it returns what QC would. QC keeps its loop for an
engine without it, and the two must change together.
*/
enum
{
	LL_VERSION,		//1
	LL_BASE,		//the slot's first sample
	LL_COUNT,		//samples the slot holds
	LL_FIRST,		//every LL_STRIDEth sample from LL_FIRST to LL_LAST is looked at
	LL_LAST,
	LL_STRIDE,
	LL_CUT,			//1: a sample whose clock is past LL_TCUT is not there (the replay's playhead)
	LL_TCUT,
	LL_FAR,			//0: no distance limit
	LL_NEAR,
	LL_GUARD,
	LL_PX,
	LL_EYE,			//3
	LL_FWD = LL_EYE+3,	//3
	LL_LO = LL_FWD+3,	//2: the rectangle a name may sit in, virtual pixels
	LL_HI = LL_LO+2,	//2
	//out
	LL_BEST = LL_HI+2,	//the nearest sample a name may sit at, and the nearest merely in front
	LL_CBEST,
	LL_BI,			//-1: none
	LL_CI,
	LL_AT,			//3: LL_BI on screen, and in the world
	LL_AW = LL_AT+3,	//3
	LL_SIZE = LL_AW+3
};

//float(float *parm, float count) lines_label: 1 with the outputs written, 0 when it refused.
static void QCBUILTIN PF_cs_lines_label(pubprogfuncs_t *prinst, struct globalvars_s *pr_globals)
{
	char *parm, *pos, *t;
	qboolean bad = false;
	int b, n, ia, ib, stride, cut, i, bi = -1, ci = -1;
	float tcut, farz, nearz, guard, px, d, best = 99999999, cbest = 99999999, f;
	vec3_t eye, fwd, p, r, q, at = {0, 0, 0}, aw = {0, 0, 0};
	float lo[2], hi[2];

	G_FLOAT(OFS_RETURN) = 0;
	if (csqc_isdarkplaces || csqc_lines.vm != prinst || G_FLOAT(OFS_PARM1) != LL_SIZE)
		return;
	parm = PR_PointerToNative_MoInvalidate(prinst, G_INT(OFS_PARM0), 0, LL_SIZE * sizeof(float));
	if (!parm || LinesF(parm, LL_VERSION) != 1)
		return;
	n = LinesInt(parm, LL_COUNT, 1, csqc_lines.samples, &bad);
	b = LinesInt(parm, LL_BASE, 0, csqc_lines.samples - 1, &bad);
	ia = LinesInt(parm, LL_FIRST, 0, 16777216, &bad);
	ib = LinesInt(parm, LL_LAST, 0, 16777216, &bad);
	stride = LinesInt(parm, LL_STRIDE, 1, 16777216, &bad);
	cut = LinesInt(parm, LL_CUT, 0, 1, &bad);
	if (bad || ia > ib || ib >= n || (unsigned int)b + n > csqc_lines.samples)
		return;
	pos = PR_PointerToNative_MoInvalidate(prinst, csqc_lines.pos, 0, csqc_lines.samples * 3 * sizeof(float));
	t = PR_PointerToNative_MoInvalidate(prinst, csqc_lines.t, 0, csqc_lines.samples * sizeof(float));
	if (!pos || !t)
		return;
	tcut = LinesF(parm, LL_TCUT);
	farz = LinesF(parm, LL_FAR);
	nearz = LinesF(parm, LL_NEAR);
	guard = LinesF(parm, LL_GUARD);
	px = LinesF(parm, LL_PX);
	LinesV(parm + LL_EYE*sizeof(float), 0, eye);
	LinesV(parm + LL_FWD*sizeof(float), 0, fwd);
	lo[0] = LinesF(parm, LL_LO);
	lo[1] = LinesF(parm, LL_LO+1);
	hi[0] = LinesF(parm, LL_HI);
	hi[1] = LinesF(parm, LL_HI+1);

	if (csqc_rebuildmatricies)
		cs_buildmatricies();
	for (i = ia; i <= ib; i += stride)
	{
		LinesV(pos, b + i, p);
		VectorSubtract(p, eye, r);
		d = sqrt(DotProduct(r, r));
		if (d >= best)
			continue;
		if (farz > 0 && d > farz)
			continue;
		//LineGraph_LabelAt at the sample itself
		if (cut && LinesF(t, b + i) > tcut)
			continue;
		//Line_ProjPt
		if (DotProduct(r, fwd) < nearz)
			continue;
		CS_ProjectPoint(p, q);
		if (q[2] < -1 || fabs(q[0]) > guard || fabs(q[1]) > guard)
			continue;
		q[0] = (float)floor(q[0] / px + 0.5f) * px;
		q[1] = (float)floor(q[1] / px + 0.5f) * px;
		q[2] = 0;
		if (!(q[0] < lo[0] || q[0] > hi[0] || q[1] < lo[1] || q[1] > hi[1]))	//QC's own test, NaN and all
		{
			best = d;
			VectorCopy(q, at);
			VectorCopy(p, aw);
			bi = i;
		}
		if (d < cbest)
		{
			cbest = d;
			ci = i;
		}
	}

	memcpy(parm + LL_BEST*sizeof(float), &best, sizeof(best));
	memcpy(parm + LL_CBEST*sizeof(float), &cbest, sizeof(cbest));
	f = bi;		memcpy(parm + LL_BI*sizeof(float), &f, sizeof(f));
	f = ci;		memcpy(parm + LL_CI*sizeof(float), &f, sizeof(f));
	memcpy(parm + LL_AT*sizeof(float), at, sizeof(at));
	memcpy(parm + LL_AW*sizeof(float), aw, sizeof(aw));
	G_FLOAT(OFS_RETURN) = 1;
}
