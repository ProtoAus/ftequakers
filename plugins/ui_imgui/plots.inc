//Owner 206: run graphs on ImPlot (Patch 614). Included inside FteImGui.
//QC owns the samples, what joins them, the energy sum, the names and every reading.
//This draws the curves it was handed and lets the player move the view; the view and
//the cursor's x go back as scalar actions (PlotsReport*). Nothing else returns.
static bool plotRegistered;
static bool PlotsOwner(const pluguiowner_t &o)
{
	return o.vm == PLUGUI_VM_CLIENT && o.owner == PlotsClient;
}
constexpr unsigned PlotsReportX0 = 1, PlotsReportX1 = 2, PlotsReportHover = 3, PlotsReportPoints = 4,
	PlotsReportRebuilt = 5;
//What one plot may draw, every series together. A point is 4 vertices and 18 indices
//as an anti-aliased strip, and the backend makes each index a vertex again: about a
//microsecond a point on the desktop it was measured on (11,752 points a frame, two
//to a pixel column, took the frame from 1.6 to 9-16 ms). Hence the cutting down.
constexpr unsigned PlotsPoolPoints = 6000;
constexpr unsigned PlotsPoolStrips = 1024;
constexpr float PlotsTolerance = 0.25f; //pixels a drawn curve may stand off a row it skipped
struct PlotStrip { unsigned first, count, series; };
//One plot's curves as drawn: strips of pixel positions, kept until what they were cut
//from changes (the revision, the series shown, either axis, the plot's rectangle).
//A view that is only being read costs its strips and nothing else.
struct PlotPool {
	bool valid = false;
	unsigned revision = 0, shown = 0, npts = 0, nstrips = 0;
	double x0 = 0, x1 = 0, y0 = 0, y1 = 0;
	ImVec2 pos, size;
	ImVec2 pts[PlotsPoolPoints];
	PlotStrip strips[PlotsPoolStrips];
};
constexpr double PlotsMinSpan = 0.25; //seconds: the QC panel's LG_MINSPAN
constexpr float PlotsFontPixels[] = {12,14,16,18,20,24,28,32};
struct PlotStore {
	unsigned revision = 0, count = 0, points = 0;
	pluguiplotseries_t series[PLUGUI_PLOT_MAX_SERIES] = {};
	float *x = nullptr, *a = nullptr, *b = nullptr;
	unsigned char *brk = nullptr;
	float xmax = 0;                 //where the longest series ends
	pluguiplotview_t view = {};
	unsigned rangeseen = 0;         //the range request already honoured
	bool fit = true, range = false; //asked of the next frame: everything, or want0..want1
	double want0 = 0, want1 = 0;
	double v0 = 0, v1 = 0, span = 0; //what the last frame showed
	PlotPool pool[2];
	unsigned drawn = 0, rebuilt = 0; //this frame: points drawn, pools cut afresh
	bool hover = false; double hoverx = 0;
	~PlotStore() { delete[] x; delete[] a; delete[] b; delete[] brk; }
};
//The board's palette (scores.inc); the panel behind this window is QC's.
static void PlotsTheme(ImGuiStyle &s, ImPlotStyle &p)
{
	const ImVec4 well(0.075f,0.078f,0.110f,1), hover(0.184f,0.200f,0.275f,1), pressed(0.231f,0.251f,0.345f,1),
		edge(0.204f,0.220f,0.286f,1), edgehi(0.314f,0.337f,0.435f,1), text(0.882f,0.894f,0.949f,1),
		dim(0.651f,0.671f,0.761f,1), accent(0.549f,0.769f,0.973f,1), clear(0,0,0,0);
	s.Colors[ImGuiCol_Text] = text; s.Colors[ImGuiCol_WindowBg] = clear; s.Colors[ImGuiCol_Border] = edge;
	s.Colors[ImGuiCol_Separator] = edge; s.Colors[ImGuiCol_SeparatorHovered] = edgehi;
	s.Colors[ImGuiCol_SeparatorActive] = accent;
	s.WindowPadding = ImVec2(0,0); s.WindowBorderSize = 0; s.WindowRounding = 0;
	ImVec4 *q = p.Colors;
	q[ImPlotCol_FrameBg] = clear; q[ImPlotCol_PlotBg] = well; q[ImPlotCol_PlotBorder] = edge;
	q[ImPlotCol_InlayText] = dim; q[ImPlotCol_AxisText] = dim; q[ImPlotCol_TitleText] = dim;
	q[ImPlotCol_AxisGrid] = ImVec4(edgehi.x,edgehi.y,edgehi.z,0.55f); q[ImPlotCol_AxisTick] = edge;
	q[ImPlotCol_AxisBg] = clear; q[ImPlotCol_AxisBgHovered] = ImVec4(hover.x,hover.y,hover.z,0.6f);
	q[ImPlotCol_AxisBgActive] = pressed; q[ImPlotCol_Selection] = accent; q[ImPlotCol_Crosshairs] = text;
	p.PlotPadding = ImVec2(2,4); p.LabelPadding = ImVec2(6,4); p.PlotBorderSize = 1; p.MinorAlpha = 0.35f;
	p.FitPadding = ImVec2(0,0.06f);
	p.PlotMinSize = ImVec2(64,40);  //the default 200x150 is more than a small window's panel has
}
//One atlas of the embedded face at eight sizes; the frame picks the largest that
//fits the caller's caption size. No file IO and no rebake.
static bool PlotsFonts(Context &c, ImGuiIO &io)
{
	static const ImWchar ascii[] = {0x20,0x7e,0};
	for (unsigned i = 0; i < sizeof(PlotsFontPixels)/sizeof(*PlotsFontPixels); i++)
	{
		ImFontConfig config;
		config.FontDataOwnedByAtlas = false;
		c.plotfonts[i] = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char *>(PlotFontData),
			int(PlotFontSize),PlotsFontPixels[i],&config,ascii);
		if (!c.plotfonts[i]) return false;
	}
	io.FontDefault = c.plotfonts[0];
	return true;
}
static void PlotsFont(Context &c)
{
	const float want = c.plots ? c.plots->view.textpx : 0;
	unsigned pick = 0;
	for (unsigned i = 1; i < sizeof(PlotsFontPixels)/sizeof(*PlotsFontPixels); i++)
		if (PlotsFontPixels[i] <= want+0.5f) pick = i;
	if (c.plotfonts[pick]) ImGui::GetIO().FontDefault = c.plotfonts[pick];
	//A time label is centred on its tick: half of "0:00.00" either side, or the
	//one on the plot's last pixel is cut by the window.
	ImPlot::GetStyle().PlotPadding.x = std::round(PlotsFontPixels[pick]*1.7f);
}
//m:ss, with the decimals the visible span needs: a clock, as the board prints one.
static int PlotsClock(double value, char *buff, int size, void *user)
{
	const PlotStore *st = static_cast<const PlotStore *>(user);
	const int decimals = st->span >= 20 ? 0 : st->span >= 2 ? 1 : 2;
	const double scale = decimals == 0 ? 1 : decimals == 1 ? 10 : 100;
	//ImPlot may ask for any double; the minutes are an int.
	const double v = std::min(std::round(std::fabs(value)*scale)/scale,1e11);
	const int m = int(v/60);
	return std::snprintf(buff,size_t(size),"%s%d:%0*.*f",(value < 0 && v > 0) ? "-" : "",m,
		decimals ? decimals+3 : 2,decimals,v-m*60.0);
}
static qboolean QDECL SetPlot(const pluguiowner_t *o, const pluguiplot_t *p)
{
	Context *c = Find(o);
	if (!c || !PlotsOwner(*o) || !PlugUI_PlotValid(p) || (c->plots && p->revision <= c->plots->revision))
		return qfalse;
	PlotStore *next = new (std::nothrow) PlotStore;
	if (!next) return qfalse;
	const size_t n = p->points ? p->points : 1;
	next->x = new (std::nothrow) float[n]; next->a = new (std::nothrow) float[n];
	next->b = new (std::nothrow) float[n]; next->brk = new (std::nothrow) unsigned char[n];
	if (!next->x || !next->a || !next->b || !next->brk) { delete next; return qfalse; }
	if (p->points)
	{
		std::memcpy(next->x,p->x,p->points*sizeof(float)); std::memcpy(next->a,p->a,p->points*sizeof(float));
		std::memcpy(next->b,p->b,p->points*sizeof(float)); std::memcpy(next->brk,p->brk,p->points);
	}
	next->revision = p->revision; next->count = p->count; next->points = p->points;
	for (unsigned i = 0; i < p->count; i++)
	{
		next->series[i] = p->series[i];
		next->xmax = std::max(next->xmax,next->x[p->series[i].first+p->series[i].count-1]);
	}
	if (PlotStore *old = c->plots)
	{
		//The view is the player's and outlives a revision. A longer or shorter extent
		//is shown whole only if the whole one was what they were looking at (a line
		//still loading sends a revision every quarter second).
		const bool whole = old->span <= 0 || (old->v0 <= 0.0005 && old->v1 >= double(old->xmax)-0.0005);
		next->view = old->view; next->rangeseen = old->rangeseen;
		next->fit = old->fit || (whole && next->xmax != old->xmax);
		next->range = old->range; next->want0 = old->want0; next->want1 = old->want1;
		next->v0 = old->v0; next->v1 = old->v1; next->span = old->span;
		delete old;
	}
	c->plots = next;
	return qtrue;
}
static qboolean QDECL SetView(const pluguiowner_t *o, const pluguiplotview_t *v)
{
	Context *c = Find(o);
	if (!c || !PlotsOwner(*o) || !c->plots || !PlugUI_PlotViewValid(v)) return qfalse;
	PlotStore &st = *c->plots;
	st.view = *v;
	if (v->rangeserial != st.rangeseen)
	{
		//Kept inside the run: ImPlot's own constraint took two frames to pull a far request
		//back, and what was shown meanwhile was reported to a host that drops an owner
		//for a number past a million.
		const double xmax = std::max(double(st.xmax),PlotsMinSpan);
		st.rangeseen = v->rangeserial;
		st.want0 = std::min(std::max(double(v->x0),0.0),xmax);
		st.want1 = std::min(std::max(double(v->x1),0.0),xmax);
		st.fit = !(st.want1 > st.want0);
		st.range = !st.fit;
	}
	return qtrue;
}
//A series' value at x, between the rows either side. False outside its rows and
//across a break or a gap: nothing is drawn where nothing was recorded.
static bool PlotsAt(const PlotStore &st, const pluguiplotseries_t &s, bool second, double x, double &y)
{
	const float *xs = st.x+s.first, *ys = (second ? st.b : st.a)+s.first;
	const unsigned n = s.count, i = unsigned(std::lower_bound(xs,xs+n,float(x))-xs);
	if (i == n) return false;
	if (double(xs[i]) == x) { y = ys[i]; return true; }
	if (!i || st.brk[s.first+i] || xs[i]-xs[i-1] > s.gap) return false;
	const double f = (x-xs[i-1])/(double(xs[i])-xs[i-1]);
	y = ys[i-1]+(double(ys[i])-ys[i-1])*f;
	return true;
}
static ImU32 PlotsColour(const pluguiplotseries_t &s, float alpha)
{
	return ImGui::ColorConvertFloat4ToU32(ImVec4(s.rgb[0],s.rgb[1],s.rgb[2],alpha));
}
//The swing filter, on pixels: a vertex is added only when no straight line from the
//last one passes within PlotsTolerance (vertically) of every point since. Points
//arrive left to right; one that does not advance (a vertical step) is always kept.
struct PlotSwing {
	PlotPool &pool;
	unsigned series, start = 0;
	ImVec2 anchor, last;
	float lo = 0, hi = 0;
	bool open = false, pending = false, full = false;
	PlotSwing(PlotPool &p, unsigned s) : pool(p), series(s) {}
	void Emit(ImVec2 p)
	{
		if (pool.npts == PlotsPoolPoints) { full = true; return; }
		pool.pts[pool.npts++] = p; anchor = p; pending = false;
	}
	void Cone(ImVec2 p)
	{
		const float dx = p.x-anchor.x;
		lo = (p.y-PlotsTolerance-anchor.y)/dx; hi = (p.y+PlotsTolerance-anchor.y)/dx;
		last = p; pending = true;
	}
	void Add(ImVec2 p)
	{
		if (!open) { start = pool.npts; open = true; Emit(p); return; }
		if (p.x-anchor.x < 0.01f) { if (pending) Emit(last); Emit(p); return; }
		if (!pending) { Cone(p); return; }
		const float dx = p.x-anchor.x, slope = (p.y-anchor.y)/dx;
		if (slope >= lo && slope <= hi)
		{
			lo = std::max(lo,(p.y-PlotsTolerance-anchor.y)/dx); hi = std::min(hi,(p.y+PlotsTolerance-anchor.y)/dx);
			last = p;
			return;
		}
		Emit(last);
		if (p.x-anchor.x < 0.01f) Emit(p); else Cone(p);
	}
	//The strip ends: at a break, a gap, or the last row.
	void End()
	{
		if (!open) return;
		if (pending) Emit(last);
		const unsigned count = pool.npts-start;
		if (count < 2) pool.npts = start; //a lone point draws nothing
		else if (pool.nstrips == PlotsPoolStrips) full = true;
		else pool.strips[pool.nstrips++] = {start,count,series};
		open = pending = false;
	}
};
//One series' rows in view into the pool, as strips. `columns` 0 is every row; past
//that the rows are first cut to each column's low and high, in the order they came,
//which bounds the work whatever the series holds. False: the pool ran out.
static bool PlotsSimplify(PlotPool &pool, const PlotStore &st, unsigned index, bool second, const ImPlotRect &lim,
	double ax, double bx, double ay, double by, unsigned columns, bool fit)
{
	const pluguiplotseries_t &s = st.series[index];
	const float *xs = st.x+s.first, *ys = (second ? st.b : st.a)+s.first;
	const unsigned char *bk = st.brk+s.first;
	const unsigned n = s.count;
	unsigned i0 = unsigned(std::lower_bound(xs,xs+n,float(lim.X.Min))-xs);
	unsigned i1 = unsigned(std::upper_bound(xs,xs+n,float(lim.X.Max))-xs);
	//The y axis is fitted to the rows in view, not to the vertices that survive: the
	//same answer every frame, so the strips cut for it can be kept.
	if (fit && i1 > i0)
	{
		unsigned low = i0, high = i0;
		for (unsigned i = i0+1; i < i1; i++)
		{
			if (ys[i] < ys[low]) low = i;
			if (ys[i] > ys[high]) high = i;
		}
		ImPlot::FitPoint(ImPlotPoint(xs[low],ys[low])); ImPlot::FitPoint(ImPlotPoint(xs[high],ys[high]));
	}
	if (i0) i0--;           //one row either side: a curve runs in from the plot's edge
	if (i1 < n) i1++;
	if (i1 <= i0) return true;
	PlotSwing swing(pool,index);
	//The row either side of the view can be a billion seconds away. Bounded before it is a
	//float: the host refuses a mesh with a vertex past 1e7 (cl_plugin_mesh.inc).
	auto pixel = [&](unsigned i)
	{
		return ImVec2(float(std::min(std::max(ax*xs[i]+bx,-1e6),1e6)),float(std::min(std::max(ay*ys[i]+by,-1e6),1e6)));
	};
	if (!columns)
	{
		for (unsigned i = i0; i < i1; i++)
		{
			if (i > i0 && (bk[i] || xs[i]-xs[i-1] > s.gap)) swing.End();
			swing.Add(pixel(i));
		}
		swing.End();
		return !swing.full;
	}
	const double left = lim.X.Min, percolumn = (lim.X.Max-lim.X.Min)/columns;
	if (!(percolumn > 0)) return true;
	long long column = 0;
	unsigned lo = i0, hi = i0;
	bool open = false;
	for (unsigned i = i0; i <= i1; i++)
	{
		const bool end = (i == i1), gap = !end && i > i0 && (bk[i] || xs[i]-xs[i-1] > s.gap);
		//ImPlot makes a degenerate range DBL_EPSILON wide (implot_internal.h:835), and a row
		//over that is no integer: bounded first.
		const long long at = end ? 0 :
			(long long)std::floor(std::min(std::max((double(xs[i])-left)/percolumn,-2.0),double(columns)+2));
		if (!end && !gap && open && at == column)
		{
			if (ys[i] < ys[lo]) lo = i;
			if (ys[i] > ys[hi]) hi = i;
			continue;
		}
		if (open)
		{
			swing.Add(pixel(std::min(lo,hi)));
			if (lo != hi) swing.Add(pixel(std::max(lo,hi)));
		}
		if (gap || end) swing.End();
		column = at; lo = hi = i; open = !end;
	}
	return !swing.full;
}
//The current plot's pool, cut afresh only when what it was cut from has changed.
static void PlotsBuild(PlotStore &st, PlotPool &pool, bool second, unsigned shown)
{
	const ImPlotRect lim = ImPlot::GetPlotLimits();
	const ImVec2 pos = ImPlot::GetPlotPos(), size = ImPlot::GetPlotSize();
	if (pool.valid && pool.revision == st.revision && pool.shown == shown && pool.x0 == lim.X.Min &&
		pool.x1 == lim.X.Max && pool.y0 == lim.Y.Min && pool.y1 == lim.Y.Max && pool.pos.x == pos.x &&
		pool.pos.y == pos.y && pool.size.x == size.x && pool.size.y == size.y) return;
	pool.valid = false;
	pool.npts = pool.nstrips = 0;
	st.rebuilt++;
	const double dx = lim.X.Max-lim.X.Min, dy = lim.Y.Max-lim.Y.Min;
	if (!(dx > 0) || !(dy > 0)) return;
	//Both axes are linear (no menu here can change a scale): two corners are the mapping.
	const ImVec2 p0 = ImPlot::PlotToPixels(lim.X.Min,lim.Y.Min), p1 = ImPlot::PlotToPixels(lim.X.Max,lim.Y.Max);
	const double ax = (double(p1.x)-p0.x)/dx, bx = p0.x-ax*lim.X.Min, ay = (double(p1.y)-p0.y)/dy, by = p0.y-ay*lim.Y.Min;
	unsigned curves = 0;
	for (unsigned i = 0; i < st.count; i++)
		if (((shown >> i) & 1) && !(second && !(st.series[i].flags & PLUGUI_PLOT_HAS_B))) curves++;
	const unsigned share = PlotsPoolPoints/std::max(curves,1u);
	const bool fit = ImPlot::FitThisFrame();
	for (unsigned i = 0; i < st.count; i++)
	{
		if (!((shown >> i) & 1) || (second && !(st.series[i].flags & PLUGUI_PLOT_HAS_B))) continue;
		const unsigned points = pool.npts, strips = pool.nstrips;
		//Every row first. A series that then holds more than its share of the pool
		//(noise, or far more rows than pixels) is done again by pixel column.
		if (!PlotsSimplify(pool,st,i,second,lim,ax,bx,ay,by,0,fit) || pool.npts-points > share)
		{
			pool.npts = points; pool.nstrips = strips;
			PlotsSimplify(pool,st,i,second,lim,ax,bx,ay,by,std::max(share/2,8u),false);
		}
	}
	pool.revision = st.revision; pool.shown = shown;
	pool.x0 = lim.X.Min; pool.x1 = lim.X.Max; pool.y0 = lim.Y.Min; pool.y1 = lim.Y.Max;
	pool.pos = pos; pool.size = size;
	pool.valid = true;
}
//A dot on a curve, ringed in the well's colour so it reads over other curves.
static void PlotsDot(ImDrawList *list, ImVec2 at, ImU32 col, float r)
{
	list->AddCircleFilled(at,r+1.5f,IM_COL32(19,20,28,255),20);
	list->AddCircleFilled(at,r,col,20);
}
//One of the two plots, between BeginPlot and EndPlot: every shown series, the
//marker, and the cursor's line with a dot on each curve.
static void PlotsPlot(PlotStore &st, bool second, const pluguiframe_t &f, bool fit, bool range)
{
	const double xmax = std::max(double(st.xmax),PlotsMinSpan);
	const ImPlotAxisFlags quiet = ImPlotAxisFlags_NoHighlight | ImPlotAxisFlags_NoMenus | ImPlotAxisFlags_NoSideSwitch;
	ImPlot::SetupAxis(ImAxis_X1,nullptr,quiet | (second ? 0 : ImPlotAxisFlags_NoTickLabels));
	ImPlot::SetupAxis(ImAxis_Y1,second ? "energy" : "speed, u/s",quiet | ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_RangeFit);
	ImPlot::SetupAxisFormat(ImAxis_X1,PlotsClock,&st);
	ImPlot::SetupAxisLimitsConstraints(ImAxis_X1,0,xmax);
	ImPlot::SetupAxisZoomConstraints(ImAxis_X1,PlotsMinSpan,xmax);
	if (fit) ImPlot::SetupAxisLimits(ImAxis_X1,0,xmax,ImPlotCond_Always);
	else if (range) ImPlot::SetupAxisLimits(ImAxis_X1,st.want0,st.want1,ImPlotCond_Always);
	ImPlot::SetupFinish();
	//ImPlot's own double click fits to what the curves report, and they report only
	//the rows in view: name the whole extent so it shows the whole run.
	ImPlot::FitPointX(0); ImPlot::FitPointX(xmax);

	const unsigned live = st.count >= 32 ? ~0u : (1u << st.count)-1;
	const unsigned shown = live & ~st.view.hidden, loud = shown & st.view.emphasis;
	const float weight = st.view.textpx >= 20 ? 2.5f : 2.0f;
	ImDrawList *list = ImPlot::GetPlotDrawList();
	const ImVec2 pos = ImPlot::GetPlotPos(), size = ImPlot::GetPlotSize();
	ImPlot::PushPlotClipRect();
	PlotPool &pool = st.pool[second ? 1 : 0];
	PlotsBuild(st,pool,second,shown);
	//Emphasised curves last, over the ones they are picked out from.
	for (int pass = 0; pool.valid && pass < 2; pass++)
		for (unsigned k = 0; k < pool.nstrips; k++)
		{
			const PlotStrip &strip = pool.strips[k];
			const bool up = (loud >> strip.series) & 1;
			if (up != (pass == 1)) continue;
			list->AddPolyline(pool.pts+strip.first,int(strip.count),
				PlotsColour(st.series[strip.series],(loud && !up) ? 0.30f : 1.0f),ImDrawFlags_None,up ? weight+1 : weight);
			st.drawn += strip.count;
		}
	if (st.view.marked)
		for (unsigned i = 0; i < st.count; i++)
		{
			const pluguiplotseries_t &s = st.series[i];
			double y;
			if (!(s.flags & PLUGUI_PLOT_MARKED) || !((shown >> i) & 1)) continue;
			//Only while it is in view, as the QC panel's playhead: a marker off to one
			//side is a line a very long way outside the plot.
			if (!(double(st.view.mark) >= ImPlot::GetPlotLimits().X.Min && double(st.view.mark) <= ImPlot::GetPlotLimits().X.Max)) continue;
			const float x = std::round(ImPlot::PlotToPixels(double(st.view.mark),0).x);
			list->AddLine(ImVec2(x,pos.y),ImVec2(x,pos.y+size.y),IM_COL32(225,228,242,115));
			if (!(second && !(s.flags & PLUGUI_PLOT_HAS_B)) && PlotsAt(st,s,second,st.view.mark,y))
				PlotsDot(list,ImPlot::PlotToPixels(double(st.view.mark),y),PlotsColour(s,1),weight+1.5f);
		}
	//The cursor's time is read off this plot's own mapping, so its line stands at
	//the mouse in both plots; the rows are the window's, the gap between them too.
	const ImVec2 mouse = ImGui::GetIO().MousePos;
	if (ImGui::IsMousePosValid(&mouse) && mouse.x >= pos.x && mouse.x <= pos.x+size.x &&
		mouse.y >= f.clip[1] && mouse.y < f.clip[3])
	{
		const double t = ImPlot::PixelsToPlot(ImVec2(mouse.x,pos.y)).x;
		const float x = std::floor(mouse.x);
		list->AddLine(ImVec2(x,pos.y),ImVec2(x,pos.y+size.y),IM_COL32(225,228,242,180));
		for (unsigned i = 0; i < st.count; i++)
		{
			const pluguiplotseries_t &s = st.series[i];
			double y;
			if (!((shown >> i) & 1) || (second && !(s.flags & PLUGUI_PLOT_HAS_B)) || !PlotsAt(st,s,second,t,y)) continue;
			PlotsDot(list,ImVec2(x,ImPlot::PlotToPixels(t,y).y),PlotsColour(s,1),weight+1.5f);
		}
		if (!second || !st.hover) { st.hover = true; st.hoverx = t; }
	}
	ImPlot::PopPlotClipRect();
	//The right button without a drag: the whole run, as in the QC panel. With a
	//drag it is ImPlot's box selection.
	if (ImPlot::IsPlotHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
		ImGui::GetIO().MouseDragMaxDistanceSqr[ImGuiMouseButton_Right] < 16.0f) st.fit = true;
	const ImPlotRect lim = ImPlot::GetPlotLimits();
	if (second) { st.v0 = lim.X.Min; st.v1 = lim.X.Max; st.span = lim.X.Max-lim.X.Min; }
}
//ImGui and ImPlot report a usage error (an unbalanced Begin, a Setup call out of place) through
//here once its assert is switched off. No such error is known in this owner; if one appears
//the frame is refused and QC draws its own plots, where the default would abort the game.
static void PlotsError(ImGuiContext *, void *user, const char *)
{
	static_cast<Context *>(user)->plotserror = true;
}
static bool PlotsGallery(Context &c, const pluguiframe_t &f)
{
	if (!c.plots) return false;
	PlotStore &st = *c.plots;
	const float width = f.clip[2]-f.clip[0], height = f.clip[3]-f.clip[1];
	//Written so Inf/NaN extents (finite edges can still overflow) are refused too.
	if (!(width >= 160 && height >= 120 && width <= f.pixelwidth && height <= f.pixelheight)) return false;
	ImGui::SetNextWindowPos(ImVec2(f.clip[0],f.clip[1]),ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(width,height),ImGuiCond_Always);
	ImGui::Begin("Run graphs",nullptr,ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav |
		ImGuiWindowFlags_NoBackground);
	st.drawn = st.rebuilt = 0; st.hover = false;
	//A request is one frame's: both plots take it, and the right button may make the next.
	const bool fit = st.fit, range = st.range && !st.fit;
	st.fit = st.range = false;
	bool ok = false;
	if (ImPlot::BeginSubplots("##runs",2,1,ImVec2(width,height),ImPlotSubplotFlags_LinkAllX))
	{
		ok = true;
		for (int plot = 0; plot < 2; plot++)
			if (ImPlot::BeginPlot(plot ? "##energy" : "##speed",ImVec2(0,0),ImPlotFlags_NoTitle |
				ImPlotFlags_NoLegend | ImPlotFlags_NoMenus | ImPlotFlags_NoMouseText))
			{
				PlotsPlot(st,plot != 0,f,fit,range);
				ImPlot::EndPlot();
			}
			else ok = false;
		ImPlot::EndSubplots();
	}
	ImGui::End();
	if (!ok) return false;
	//The host closes an owner over an action outside +-1e6, and a series may run past that.
	auto bounded = [](double value) { return float(std::min(std::max(value,-1e6),1e6)); };
	Action(c,PlotsReportX0,bounded(st.v0)); Action(c,PlotsReportX1,bounded(st.v1));
	if (st.hover) Action(c,PlotsReportHover,bounded(st.hoverx));
	Action(c,PlotsReportPoints,float(st.drawn)); Action(c,PlotsReportRebuilt,float(st.rebuilt));
	return true;
}
