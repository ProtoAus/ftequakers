//Shared bounds/grammar for NativeUIModel/1 and NativeUIPlot/1. Include after plugin.h/ABI types.
#ifndef FTE_UI_MODEL_H
#define FTE_UI_MODEL_H
static int PlugUI_LabelValid(const char *text)
{
	unsigned int i = 0, c, n, mincode, b;
	while (i < PLUGUI_MODEL_LABEL_BYTES)
	{
		c = (unsigned char)text[i++];
		if (!c) return 1;
		if (c < 32 || c == 127) return 0;
		if (c < 128) continue;
		if (c >= 0xc2 && c <= 0xdf) { n = 1; mincode = 0x80; c &= 31; }
		else if (c >= 0xe0 && c <= 0xef) { n = 2; mincode = 0x800; c &= 15; }
		else if (c >= 0xf0 && c <= 0xf4) { n = 3; mincode = 0x10000; c &= 7; }
		else return 0;
		while (n--)
		{
			if (i >= PLUGUI_MODEL_LABEL_BYTES) return 0;
			b = (unsigned char)text[i++];
			if (b < 0x80 || b > 0xbf) return 0;
			c = (c << 6) | (b & 63);
		}
		if (c < mincode || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return 0;
	}
	return 0;
}
static int PlugUI_WidgetValid(const pluguiwidget_t *w)
{
	unsigned int bits;
	memcpy(&bits, &w->value, sizeof(bits)); //also reliable in fast-math engine TUs
	if ((bits & (255u << 23)) == (255u << 23)) return 0;
	if (!w->id || w->id > PLUGUI_MODEL_MAX_ID || !w->row || w->row > PLUGUI_MODEL_MAX_ID ||
		w->type < PLUGUI_WIDGET_TEXT || w->type > PLUGUI_WIDGET_CHECKBOX ||
		!PlugUI_LabelValid(w->label)) return 0;
	return w->value == 0 || (w->type == PLUGUI_WIDGET_CHECKBOX && w->value == 1);
}
static int PlugUI_WidgetsValid(unsigned int revision, unsigned int count, const pluguiwidget_t *widgets)
{
	unsigned int i, j;
	if (!revision || revision > PLUGUI_MODEL_MAX_ID) return 0;
	for (i = 0; i < count; i++)
	{
		if (!PlugUI_WidgetValid(&widgets[i])) return 0;
		for (j = 0; j < i; j++)
			if (widgets[i].id == widgets[j].id) return 0;
	}
	return 1;
}
static int PlugUI_ModelValid(const pluguimodel_t *m)
{
	return m && m->structsize == sizeof(*m) && m->count <= PLUGUI_MODEL_MAX_WIDGETS &&
		PlugUI_WidgetsValid(m->revision, m->count, m->widgets);
}
static int PlugUI_Finite(float value)
{
	unsigned int bits;
	memcpy(&bits, &value, sizeof(bits)); //also holds in a fast-math translation unit
	return (bits & (255u << 23)) != (255u << 23);
}
//A number that can be plotted: finite, and small enough to do arithmetic on.
static int PlugUI_PlotNumber(float value)
{
	return PlugUI_Finite(value) && value >= -PLUGUI_PLOT_MAX_VALUE && value <= PLUGUI_PLOT_MAX_VALUE;
}
//Everything a provider indexes or draws from: counts, ranges, order, magnitudes.
//The host runs it on its copy of QC's rows; a provider runs it again on arrival.
static int PlugUI_PlotValid(const pluguiplot_t *p)
{
	unsigned int i, j, k, at = 0;
	const pluguiplotseries_t *s;
	if (!p || p->structsize != sizeof(*p) || !p->revision || p->revision > PLUGUI_MODEL_MAX_ID ||
		p->count > PLUGUI_PLOT_MAX_SERIES || p->points > PLUGUI_PLOT_MAX_TOTAL ||
		(p->points && (!p->x || !p->a || !p->b || !p->brk))) return 0;
	for (i = 0; i < p->count; i++)
	{
		s = &p->series[i];
		if (s->flags > PLUGUI_PLOT_FLAGS || !s->count || s->count > PLUGUI_PLOT_MAX_POINTS ||
			s->first != at || s->count > p->points - at ||
			!PlugUI_PlotNumber(s->gap) || !(s->gap > 0)) return 0;
		for (j = 0; j < 3; j++)
			if (!PlugUI_Finite(s->rgb[j]) || s->rgb[j] < 0 || s->rgb[j] > 1) return 0;
		for (k = at; k < at + s->count; k++)
		{
			if (!PlugUI_PlotNumber(p->x[k]) || !PlugUI_PlotNumber(p->a[k]) || !PlugUI_PlotNumber(p->b[k]) ||
				p->brk[k] > 1) return 0;
			if (k > at && p->x[k] < p->x[k-1]) return 0;
		}
		at += s->count;
	}
	return at == p->points;
}
static int PlugUI_PlotViewValid(const pluguiplotview_t *v)
{
	return v && v->structsize == sizeof(*v) && v->hidden < (1u << PLUGUI_PLOT_MAX_SERIES) &&
		v->emphasis < (1u << PLUGUI_PLOT_MAX_SERIES) && v->marked <= 1 && PlugUI_PlotNumber(v->mark) &&
		PlugUI_Finite(v->textpx) && v->textpx >= 1 && v->textpx <= 256 &&
		v->rangeserial <= PLUGUI_MODEL_MAX_ID && PlugUI_PlotNumber(v->x0) && PlugUI_PlotNumber(v->x1);
}
static int PlugUI_Model2Valid(const pluguimodel2_t *m)
{
	return m && m->structsize == sizeof(*m) && m->count <= PLUGUI_MODEL2_MAX_WIDGETS &&
		PlugUI_WidgetsValid(m->revision, m->count, m->widgets);
}
#endif
