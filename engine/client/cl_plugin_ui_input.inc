//Included after the explicit NativeUI/1 bridge. No input destination/bind hooks.
static qboolean Plug_NativeUI_InputRegister(const pluguiinputservice_t *s, size_t size)
{
	if (!s || size != sizeof(*s) || !currentplug || currentplug != plugui_service ||
		plugui_busy || plugui_input.Input)
		return false;
	if (s->structsize != sizeof(*s) || s->version != PLUGUI_INPUT_VERSION ||
		s->capabilities != PLUGUI_INPUT_CAP_EVENTS || !s->Input || !s->Poll)
		return false;
	plugui_input = *s;
	return true;
}
static qboolean Plug_NativeUI_InputReady(void)
{
	return Plug_NativeUI_Ready() && plugui_input.Input && plugui_input.Poll;
}
static qboolean Plug_NativeUI_InputValid(const pluguiinputevent_t *e)
{
	if (IS_NAN(e->a) || IS_NAN(e->b))
		return false;
	switch (e->type)
	{
	case PLUGUI_INPUT_MOUSEPOS:
		return e->a >= -32768 && e->a <= 32768 && e->b >= -32768 && e->b <= 32768;
	case PLUGUI_INPUT_BUTTON:
		return e->a >= 0 && e->a <= 4 && e->a == (unsigned int)e->a && (e->b == 0 || e->b == 1);
	case PLUGUI_INPUT_WHEEL:
		return e->a >= -10 && e->a <= 10 && e->b >= -10 && e->b <= 10;
	case PLUGUI_INPUT_KEY:
		return e->a >= 1 && e->a <= PLUGUI_KEY_MAX && e->a == (unsigned int)e->a && (e->b == 0 || e->b == 1);
	case PLUGUI_INPUT_TEXT:
		return e->a >= 1 && e->a <= 1114111 && e->a == (unsigned int)e->a &&
			!(e->a >= 55296 && e->a <= 57343) && e->b == 0;
	case PLUGUI_INPUT_RESET:
		return e->a == 0 && e->b == 0;
	default:
		return false;
	}
}
void QCBUILTIN PF_ui_native_input_status(pubprogfuncs_t *prinst, struct globalvars_s *pr_globals)
{
	(void)prinst;
	G_FLOAT(OFS_RETURN) = Plug_NativeUI_InputReady();
}
void QCBUILTIN PF_ui_native_input(pubprogfuncs_t *prinst, struct globalvars_s *pr_globals)
{
	pluguivm_t *v = Plug_NativeUI_VM(prinst);
	plugin_t *old = currentplug;
	unsigned int handle = Plug_NativeUI_ID(G_FLOAT(OFS_PARM0));
	pluguiinputevent_t e;
	qboolean ok;
	e.structsize = sizeof(e);
	e.type = Plug_NativeUI_ID(G_FLOAT(OFS_PARM1));
	e.a = G_FLOAT(OFS_PARM2);
	e.b = G_FLOAT(OFS_PARM3);
	G_FLOAT(OFS_RETURN) = 0;
	if (!v || !handle || handle != v->owner.generation || !Plug_NativeUI_InputReady() ||
		!Plug_NativeUI_InputValid(&e))
		return;
	if (v->inputframe != (unsigned int)host_framecount)
	{
		v->inputframe = (unsigned int)host_framecount;
		v->inputs = 0;
	}
	if (e.type != PLUGUI_INPUT_RESET)
	{
		if (v->inputs == PLUGUI_INPUT_MAX_EVENTS)
		{
			Plug_NativeUI_CloseOwner(v, PLUGUI_CLOSE_FAILED);
			return;
		}
		v->inputs++;
	}
	if (e.type == PLUGUI_INPUT_RESET)
		v->drawok = false; //discard authority to consume pre-reset actions
	currentplug = plugui_service;
	plugui_busy = true;
	ok = plugui_input.Input(&v->owner, &e);
	plugui_busy = false;
	currentplug = old;
	if (!ok)
		Plug_NativeUI_CloseOwner(v, PLUGUI_CLOSE_FAILED);
	else
		G_FLOAT(OFS_RETURN) = 1;
}
void QCBUILTIN PF_ui_native_poll(pubprogfuncs_t *prinst, struct globalvars_s *pr_globals)
{
	pluguivm_t *v = Plug_NativeUI_VM(prinst);
	plugin_t *old = currentplug;
	unsigned int handle = Plug_NativeUI_ID(G_FLOAT(OFS_PARM0));
	pluguiaction_t action;
	qboolean ok;
	G_VECTOR(OFS_RETURN)[0] = G_VECTOR(OFS_RETURN)[1] = G_VECTOR(OFS_RETURN)[2] = 0;
	if (!v || !v->drawing || !v->drawok || !handle || handle != v->owner.generation ||
		v->lastframe != (unsigned int)host_framecount || v->polls == PLUGUI_INPUT_MAX_ACTIONS ||
		!Plug_NativeUI_InputReady())
		return;
	v->polls++;
	memset(&action, 0, sizeof(action));
	currentplug = plugui_service;
	plugui_busy = true;
	ok = plugui_input.Poll(&v->owner, &action);
	plugui_busy = false;
	currentplug = old;
	if (!ok)
		return;
	if (!action.id || action.id > PLUGUI_MAXID || action.generation != handle ||
		IS_NAN(action.value) || action.value < -1000000 || action.value > 1000000)
	{
		Plug_NativeUI_CloseOwner(v, PLUGUI_CLOSE_FAILED);
		return;
	}
	G_VECTOR(OFS_RETURN)[0] = action.id;
	G_VECTOR(OFS_RETURN)[1] = action.generation;
	G_VECTOR(OFS_RETURN)[2] = action.value;
}
