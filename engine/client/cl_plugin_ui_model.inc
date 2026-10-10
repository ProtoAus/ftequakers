//Counted atomic QC snapshots; no borrowed string or row-index action authority.
#include "../../plugins/ui_model.h"
static qboolean Plug_NativeUI_ModelRegister(const pluguimodelservice_t *s, size_t size)
{
	if (!s || size != sizeof(*s) || !currentplug || currentplug != plugui_service ||
		plugui_busy || !plugui_input.Input || plugui_model.SetModel) return false;
	if (s->structsize != sizeof(*s) || s->version != PLUGUI_MODEL_VERSION ||
		s->capabilities != PLUGUI_MODEL_CAP_WIDGETS || !s->SetModel || !s->PollModel) return false;
	plugui_model = *s;
	return true;
}
static qboolean Plug_NativeUI_ModelRegister2(const pluguimodelservice2_t *s, size_t size)
{
	if (!s || size != sizeof(*s) || !currentplug || currentplug != plugui_service ||
		plugui_busy || !plugui_input.Input || !plugui_model.SetModel || plugui_model2.SetModel) return false;
	if (s->structsize != sizeof(*s) || s->version != PLUGUI_MODEL2_VERSION ||
		s->capabilities != PLUGUI_MODEL_CAP_WIDGETS || !s->SetModel || !s->PollModel) return false;
	plugui_model2 = *s;
	return true;
}
static qboolean Plug_NativeUI_ModelReady(void)
{
	return Plug_NativeUI_InputReady() && plugui_model.SetModel && plugui_model.PollModel;
}
static pluguivm_t *Plug_NativeUI_ModelOwner(pubprogfuncs_t *vm, float arg)
{
	pluguivm_t *v = Plug_NativeUI_VM(vm);
	unsigned int handle = Plug_NativeUI_ID(arg);
	return v && v->drawing && handle && handle == v->owner.generation &&
		Plug_NativeUI_ModelReady() ? v : NULL;
}
void QCBUILTIN PF_ui_native_model_status(pubprogfuncs_t *prinst, struct globalvars_s *pr_globals)
{
	(void)prinst;
	G_FLOAT(OFS_RETURN) = Plug_NativeUI_ModelReady();
}
void QCBUILTIN PF_ui_native_model_limit(pubprogfuncs_t *prinst, struct globalvars_s *pr_globals)
{
	(void)prinst;
	G_FLOAT(OFS_RETURN) = !Plug_NativeUI_ModelReady() ? 0 :
		plugui_model2.SetModel ? PLUGUI_MODEL2_MAX_WIDGETS : PLUGUI_MODEL_MAX_WIDGETS;
}
void QCBUILTIN PF_ui_native_model_begin(pubprogfuncs_t *prinst, struct globalvars_s *pr_globals)
{
	pluguivm_t *v = Plug_NativeUI_ModelOwner(prinst, G_FLOAT(OFS_PARM0));
	unsigned int revision = Plug_NativeUI_ID(G_FLOAT(OFS_PARM1));
	float count = G_FLOAT(OFS_PARM2);
	G_FLOAT(OFS_RETURN) = 0;
	if (!v) return;
	//Even a rejected begin abandons an older staging transaction, not its live model.
	v->stageactive = false;
	if (!revision || revision <= v->model.revision || IS_NAN(count) || count < 0 ||
		count > (plugui_model2.SetModel ? PLUGUI_MODEL2_MAX_WIDGETS : PLUGUI_MODEL_MAX_WIDGETS) ||
		count != (unsigned int)count) return;
	memset(&v->staging, 0, sizeof(v->staging));
	v->staging.structsize = sizeof(v->staging);
	v->staging.revision = revision; v->staging.count = (unsigned int)count;
	v->staged = 0; v->stagebad = false; v->stageactive = true;
	G_FLOAT(OFS_RETURN) = 1;
}
void QCBUILTIN PF_ui_native_model_widget(pubprogfuncs_t *prinst, struct globalvars_s *pr_globals)
{
	pluguivm_t *v = Plug_NativeUI_ModelOwner(prinst, G_FLOAT(OFS_PARM0));
	float index = G_FLOAT(OFS_PARM1);
	pluguiwidget_t w;
	const char *label;
	unsigned int len, i;
	G_FLOAT(OFS_RETURN) = 0;
	if (!v || !v->stageactive || v->stagebad) return;
	v->stagebad = true; //failure cannot leave a seemingly complete transaction
	if (IS_NAN(index) || index != v->staged || v->staged >= v->staging.count) return;
	memset(&w, 0, sizeof(w));
	w.id = Plug_NativeUI_ID(G_VECTOR(OFS_PARM2)[0]);
	w.row = Plug_NativeUI_ID(G_VECTOR(OFS_PARM2)[1]);
	w.type = Plug_NativeUI_ID(G_VECTOR(OFS_PARM2)[2]);
	w.value = G_FLOAT(OFS_PARM3);
	label = PR_GetStringOfs(prinst, OFS_PARM4);
	if (!label) return;
	for (len = 0; len < sizeof(w.label); len++)
	{
		w.label[len] = label[len];
		if (!label[len]) break;
	}
	if (!PlugUI_WidgetValid(&w)) return;
	for (i = 0; i < v->staged; i++)
		if (w.id == v->staging.widgets[i].id) return;
	v->staging.widgets[v->staged++] = w; v->stagebad = false;
	G_FLOAT(OFS_RETURN) = 1;
}
void QCBUILTIN PF_ui_native_model_commit(pubprogfuncs_t *prinst, struct globalvars_s *pr_globals)
{
	pluguivm_t *v = Plug_NativeUI_ModelOwner(prinst, G_FLOAT(OFS_PARM0));
	plugin_t *old = currentplug;
	qboolean ok;
	pluguimodel_t prior;
	G_FLOAT(OFS_RETURN) = 0;
	if (!v || !v->stageactive) return;
	v->stageactive = false;
	if (v->stagebad || v->staged != v->staging.count || !PlugUI_Model2Valid(&v->staging)) return;
	v->drawok = false;
	currentplug = plugui_service; plugui_busy = true;
	if (plugui_model2.SetModel) ok = plugui_model2.SetModel(&v->owner, &v->staging);
	else
	{
		//Exact /1 layout, never cast a larger struct through the old callback.
		memset(&prior, 0, sizeof(prior)); prior.structsize = sizeof(prior);
		prior.revision = v->staging.revision; prior.count = v->staging.count;
		if (prior.count <= PLUGUI_MODEL_MAX_WIDGETS)
			memcpy(prior.widgets, v->staging.widgets, prior.count * sizeof(prior.widgets[0]));
		ok = PlugUI_ModelValid(&prior) && plugui_model.SetModel(&v->owner, &prior);
	}
	plugui_busy = false; currentplug = old;
	if (!ok) Plug_NativeUI_CloseOwner(v, PLUGUI_CLOSE_FAILED);
	else { v->model = v->staging; G_FLOAT(OFS_RETURN) = 1; }
}
void QCBUILTIN PF_ui_native_model_poll(pubprogfuncs_t *prinst, struct globalvars_s *pr_globals)
{
	pluguivm_t *v = Plug_NativeUI_ModelOwner(prinst, G_FLOAT(OFS_PARM0));
	unsigned int revision = Plug_NativeUI_ID(G_FLOAT(OFS_PARM1)), i;
	plugin_t *old = currentplug;
	pluguimodelaction_t a;
	const pluguiwidget_t *w;
	qboolean ok;
	G_VECTOR(OFS_RETURN)[0] = G_VECTOR(OFS_RETURN)[1] = G_VECTOR(OFS_RETURN)[2] = 0;
	if (!v || !revision || revision != v->model.revision || !v->drawok ||
		v->lastframe != (unsigned int)host_framecount || v->polls == PLUGUI_INPUT_MAX_ACTIONS) return;
	v->polls++;
	memset(&a, 0, sizeof(a));
	currentplug = plugui_service; plugui_busy = true;
	ok = plugui_model2.PollModel ? plugui_model2.PollModel(&v->owner, &a) : plugui_model.PollModel(&v->owner, &a);
	plugui_busy = false; currentplug = old;
	if (!ok) return;
	for (i = 0; i < v->model.count; i++)
		if (v->model.widgets[i].id == a.id) break;
	if (i == v->model.count || a.generation != v->owner.generation ||
		a.revision != revision || IS_NAN(a.value)) goto malformed;
	w = &v->model.widgets[i];
	if (a.row != w->row || w->type == PLUGUI_WIDGET_TEXT ||
		(w->type == PLUGUI_WIDGET_BUTTON && a.value != 1) ||
		(w->type == PLUGUI_WIDGET_CHECKBOX && a.value != 0 && a.value != 1)) goto malformed;
	G_VECTOR(OFS_RETURN)[0] = a.id;
	G_VECTOR(OFS_RETURN)[1] = a.row;
	G_VECTOR(OFS_RETURN)[2] = a.value;
	return;
malformed:
	Plug_NativeUI_CloseOwner(v, PLUGUI_CLOSE_FAILED);
}
