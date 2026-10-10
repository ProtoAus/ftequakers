//Included after the model bridge. NativeUIPlot/1: counted sample series for a plot
//owner. QC names its arrays by pointer; every range is checked against the VM's own
//memory, copied here and validated before a provider sees it. No QC pointer, and no
//pointer into QC memory, crosses the ABI.
void *PR_PointerToNative_MoInvalidate(pubprogfuncs_t *inst, pint_t ptr, size_t offset, size_t datasize);

#define PLUGUI_PLOT_BEGINS 2u //transactions a VM may start in one frame

static qboolean Plug_NativeUI_PlotRegister(const pluguiplotservice_t *s, size_t size)
{
	if (!s || size != sizeof(*s) || !currentplug || currentplug != plugui_service ||
		plugui_busy || !plugui_input.Input || plugui_plot.SetPlot) return false;
	if (s->structsize != sizeof(*s) || s->version != PLUGUI_PLOT_VERSION ||
		s->capabilities != PLUGUI_PLOT_CAP_SERIES || !s->SetPlot || !s->SetView) return false;
	plugui_plot = *s;
	return true;
}
//With the owner: a revision, a transaction and the rows are all its own.
static void Plug_NativeUI_PlotFree(pluguivm_t *v)
{
	BZ_Free(v->plotx); BZ_Free(v->plota); BZ_Free(v->plotb); BZ_Free(v->plotbrk);
	v->plotx = v->plota = v->plotb = NULL; v->plotbrk = NULL;
	v->plotcap = v->plotrevision = 0;
	v->plotactive = v->plotbad = false;
}
static qboolean Plug_NativeUI_PlotReady(void)
{
	return Plug_NativeUI_InputReady() && plugui_plot.SetPlot && plugui_plot.SetView;
}
static pluguivm_t *Plug_NativeUI_PlotOwner(pubprogfuncs_t *vm, float arg)
{
	pluguivm_t *v = Plug_NativeUI_VM(vm);
	unsigned int handle = Plug_NativeUI_ID(arg);
	return v && v->drawing && handle && handle == v->owner.generation &&
		Plug_NativeUI_PlotReady() ? v : NULL;
}
//A whole number in 0..limit, else -1.
static int Plug_NativeUI_Count(float f, unsigned int limit)
{
	if (IS_NAN(f) || f < 0 || f > limit || f != (unsigned int)f)
		return -1;
	return (int)f;
}
//A QC pointer need not be aligned: never read one through a float lvalue.
static float Plug_NativeUI_Float(const void *base, unsigned int i)
{
	float f;
	memcpy(&f, (const qbyte *)base + i * sizeof(f), sizeof(f));
	return f;
}
//Room for `rows` more. Doubling, capped: a full snapshot is 589824 rows, 7.7 MB here.
static qboolean Plug_NativeUI_PlotRoom(pluguivm_t *v, unsigned int rows)
{
	unsigned int want = v->plot.points + rows, cap = v->plotcap;
	float *x, *a, *b;
	qbyte *brk;
	if (rows > PLUGUI_PLOT_MAX_TOTAL - v->plot.points) return false;
	if (want <= cap) return true;
	if (!cap) cap = 4096;
	while (cap < want) cap *= 2;
	if (cap > PLUGUI_PLOT_MAX_TOTAL) cap = PLUGUI_PLOT_MAX_TOTAL;
	//Four blocks, and plotcap last: a failure part-way leaves each one valid, owned
	//and at least its old size.
	x = BZF_Realloc(v->plotx, cap * sizeof(float)); if (!x) return false; v->plotx = x;
	a = BZF_Realloc(v->plota, cap * sizeof(float)); if (!a) return false; v->plota = a;
	b = BZF_Realloc(v->plotb, cap * sizeof(float)); if (!b) return false; v->plotb = b;
	brk = BZF_Realloc(v->plotbrk, cap); if (!brk) return false; v->plotbrk = brk;
	v->plotcap = cap;
	return true;
}
void QCBUILTIN PF_ui_native_plot_status(pubprogfuncs_t *prinst, struct globalvars_s *pr_globals)
{
	(void)prinst;
	G_FLOAT(OFS_RETURN) = Plug_NativeUI_PlotReady();
}
//float(float handle, float revision, float series)
void QCBUILTIN PF_ui_native_plot_begin(pubprogfuncs_t *prinst, struct globalvars_s *pr_globals)
{
	pluguivm_t *v = Plug_NativeUI_PlotOwner(prinst, G_FLOAT(OFS_PARM0));
	unsigned int revision = Plug_NativeUI_ID(G_FLOAT(OFS_PARM1));
	int count = Plug_NativeUI_Count(G_FLOAT(OFS_PARM2), PLUGUI_PLOT_MAX_SERIES);
	G_FLOAT(OFS_RETURN) = 0;
	if (!v) return;
	//Even a refused begin abandons an older transaction, not the published plot.
	v->plotactive = false;
	if (v->plotframe != (unsigned int)host_framecount)
	{
		v->plotframe = (unsigned int)host_framecount;
		v->plotbegins = 0;
	}
	//Budgeted like open/input/poll: a transaction copies and checks every row twice.
	if (v->plotbegins == PLUGUI_PLOT_BEGINS || !revision || revision <= v->plotrevision || count < 0) return;
	v->plotbegins++;
	memset(&v->plot, 0, sizeof(v->plot));
	v->plot.structsize = sizeof(v->plot);
	v->plot.revision = revision; v->plot.count = (unsigned int)count;
	v->plotseries = 0; v->plotbad = false; v->plotrows = true; v->plotactive = true;
	G_FLOAT(OFS_RETURN) = 1;
}
//float(float handle, float index, vector spec, vector rgb)
//spec is [PLUGUI_PLOT_* flags, gap, 0]. The series' rows follow, before the next series.
void QCBUILTIN PF_ui_native_plot_series(pubprogfuncs_t *prinst, struct globalvars_s *pr_globals)
{
	pluguivm_t *v = Plug_NativeUI_PlotOwner(prinst, G_FLOAT(OFS_PARM0));
	float index = G_FLOAT(OFS_PARM1);
	int flags = Plug_NativeUI_Count(G_VECTOR(OFS_PARM2)[0], PLUGUI_PLOT_FLAGS);
	pluguiplotseries_t s;
	unsigned int i;
	G_FLOAT(OFS_RETURN) = 0;
	if (!v || !v->plotactive || v->plotbad) return;
	v->plotbad = true; //a failure cannot leave a seemingly complete transaction
	if (IS_NAN(index) || index != v->plotseries || v->plotseries >= v->plot.count || !v->plotrows || flags < 0) return;
	memset(&s, 0, sizeof(s));
	s.flags = (unsigned int)flags;
	s.gap = G_VECTOR(OFS_PARM2)[1];
	if (!PlugUI_PlotNumber(s.gap) || !(s.gap > 0)) return;
	for (i = 0; i < 3; i++)
	{
		s.rgb[i] = G_VECTOR(OFS_PARM3)[i];
		if (!PlugUI_Finite(s.rgb[i]) || s.rgb[i] < 0 || s.rgb[i] > 1) return;
	}
	s.first = v->plot.points;
	v->plot.series[v->plotseries++] = s;
	v->plotrows = false; v->plotbad = false;
	G_FLOAT(OFS_RETURN) = 1;
}
//float(float handle, float index, float *x, float *a, float *b, float *brk, float count, float xoffset)
//`count` floats at each pointer. b is null exactly when the series has no second
//value; brk may be null (nothing breaks). x is stored less xoffset.
void QCBUILTIN PF_ui_native_plot_rows(pubprogfuncs_t *prinst, struct globalvars_s *pr_globals)
{
	pluguivm_t *v = Plug_NativeUI_PlotOwner(prinst, G_FLOAT(OFS_PARM0));
	float index = G_FLOAT(OFS_PARM1), xoffset = G_FLOAT(OFS_PARM7);
	int count = Plug_NativeUI_Count(G_FLOAT(OFS_PARM6), PLUGUI_PLOT_MAX_POINTS);
	const void *x, *a, *b = NULL, *brk = NULL;
	pluguiplotseries_t *s;
	unsigned int i, n, at;
	size_t bytes;
	float xv, av, bv, kv, last = 0;
	G_FLOAT(OFS_RETURN) = 0;
	if (!v || !v->plotactive || v->plotbad) return;
	v->plotbad = true;
	if (IS_NAN(index) || !v->plotseries || index != v->plotseries - 1 || v->plotrows ||
		count < 1 || !PlugUI_Finite(xoffset)) return;
	n = (unsigned int)count;
	bytes = n * sizeof(float);
	s = &v->plot.series[v->plotseries - 1];
	//Offset 0: the helper's bounds test is on ptr..ptr+bytes, and a null fails it.
	x = PR_PointerToNative_MoInvalidate(prinst, G_INT(OFS_PARM2), 0, bytes);
	a = PR_PointerToNative_MoInvalidate(prinst, G_INT(OFS_PARM3), 0, bytes);
	if (!x || !a) return;
	if (s->flags & PLUGUI_PLOT_HAS_B)
	{
		b = PR_PointerToNative_MoInvalidate(prinst, G_INT(OFS_PARM4), 0, bytes);
		if (!b) return;
	}
	else if (G_INT(OFS_PARM4)) return;
	if (G_INT(OFS_PARM5))
	{
		brk = PR_PointerToNative_MoInvalidate(prinst, G_INT(OFS_PARM5), 0, bytes);
		if (!brk) return;
	}
	if (!Plug_NativeUI_PlotRoom(v, n)) return;
	at = v->plot.points;
	for (i = 0; i < n; i++)
	{
		xv = Plug_NativeUI_Float(x, i) - xoffset;
		av = Plug_NativeUI_Float(a, i);
		bv = b ? Plug_NativeUI_Float(b, i) : 0;
		kv = brk ? Plug_NativeUI_Float(brk, i) : 0;
		//x is judged less its offset: a run's clock may be large where its length is not.
		if (!PlugUI_PlotNumber(xv) || !PlugUI_PlotNumber(av) || !PlugUI_PlotNumber(bv) || !PlugUI_Finite(kv) ||
			(i && xv < last)) return;
		last = xv;
		v->plotx[at+i] = xv;
		v->plota[at+i] = av;
		v->plotb[at+i] = bv;
		v->plotbrk[at+i] = (kv != 0);
	}
	s->count = n;
	v->plot.points += n;
	v->plotrows = true; v->plotbad = false;
	G_FLOAT(OFS_RETURN) = 1;
}
//float(float handle)
void QCBUILTIN PF_ui_native_plot_commit(pubprogfuncs_t *prinst, struct globalvars_s *pr_globals)
{
	pluguivm_t *v = Plug_NativeUI_PlotOwner(prinst, G_FLOAT(OFS_PARM0));
	plugin_t *old = currentplug;
	qboolean ok;
	G_FLOAT(OFS_RETURN) = 0;
	if (!v || !v->plotactive) return;
	v->plotactive = false;
	if (v->plotbad || v->plotseries != v->plot.count || !v->plotrows) return;
	v->plot.x = v->plotx; v->plot.a = v->plota; v->plot.b = v->plotb; v->plot.brk = v->plotbrk;
	ok = PlugUI_PlotValid(&v->plot);
	if (ok)
	{
		v->drawok = false;
		currentplug = plugui_service; plugui_busy = true;
		ok = plugui_plot.SetPlot(&v->owner, &v->plot);
		plugui_busy = false; currentplug = old;
		if (!ok) Plug_NativeUI_CloseOwner(v, PLUGUI_CLOSE_FAILED);
		else { v->plotrevision = v->plot.revision; G_FLOAT(OFS_RETURN) = 1; }
	}
	v->plot.x = v->plot.a = v->plot.b = NULL; v->plot.brk = NULL; //lent for the call only
}
//float(float handle, vector mark, vector masks, vector range)
//mark is [x, shown, caption px], masks [hidden, emphasis, 0], range [serial, x0, x1].
void QCBUILTIN PF_ui_native_plot_view(pubprogfuncs_t *prinst, struct globalvars_s *pr_globals)
{
	pluguivm_t *v = Plug_NativeUI_PlotOwner(prinst, G_FLOAT(OFS_PARM0));
	plugin_t *old = currentplug;
	const float *mark = G_VECTOR(OFS_PARM1), *masks = G_VECTOR(OFS_PARM2), *range = G_VECTOR(OFS_PARM3);
	int marked = Plug_NativeUI_Count(mark[1], 1);
	int hidden = Plug_NativeUI_Count(masks[0], (1u << PLUGUI_PLOT_MAX_SERIES) - 1);
	int emphasis = Plug_NativeUI_Count(masks[1], (1u << PLUGUI_PLOT_MAX_SERIES) - 1);
	int serial = Plug_NativeUI_Count(range[0], PLUGUI_MAXID);
	pluguiplotview_t view;
	qboolean ok;
	G_FLOAT(OFS_RETURN) = 0;
	if (!v || !v->plotrevision || marked < 0 || hidden < 0 || emphasis < 0 || serial < 0) return;
	memset(&view, 0, sizeof(view));
	view.structsize = sizeof(view);
	view.hidden = (unsigned int)hidden; view.emphasis = (unsigned int)emphasis;
	view.marked = (unsigned int)marked; view.mark = mark[0]; view.textpx = mark[2];
	view.rangeserial = (unsigned int)serial; view.x0 = range[1]; view.x1 = range[2];
	if (!PlugUI_PlotViewValid(&view)) return;
	currentplug = plugui_service; plugui_busy = true;
	ok = plugui_plot.SetView(&v->owner, &view);
	plugui_busy = false; currentplug = old;
	if (!ok) Plug_NativeUI_CloseOwner(v, PLUGUI_CLOSE_FAILED);
	else G_FLOAT(OFS_RETURN) = 1;
}
