//Included inside FteImGui; keep all third-party/standard headers at global scope.
//Scalar-only diagnostic transport; counted model authority is separate.
static bool inputRegistered;
static bool Interactive(const pluguiowner_t &o)
{
	return o.owner == (o.vm == PLUGUI_VM_MENU ? InteractiveMenu : InteractiveClient) ||
		(o.vm == PLUGUI_VM_CLIENT && (o.owner == ScoresClient || o.owner == PlotsClient));
}
static Context *Find(const pluguiowner_t *o)
{
	if (!o || o->vm < 1 || o->vm > 2) return nullptr;
	Context *c = contexts[o->vm-1];
	return c && c->owner.owner == o->owner && c->owner.generation == o->generation ? c : nullptr;
}
static bool ValidEvent(const pluguiinputevent_t *e)
{
	if (!e || e->structsize != sizeof(*e) || !std::isfinite(e->a) || !std::isfinite(e->b)) return false;
	switch (e->type) {
	case PLUGUI_INPUT_MOUSEPOS: return e->a >= -32768 && e->a <= 32768 && e->b >= -32768 && e->b <= 32768;
	case PLUGUI_INPUT_BUTTON: return e->a >= 0 && e->a <= 4 && e->a == unsigned(e->a) && (e->b == 0 || e->b == 1);
	case PLUGUI_INPUT_WHEEL: return e->a >= -10 && e->a <= 10 && e->b >= -10 && e->b <= 10;
	case PLUGUI_INPUT_KEY: return e->a >= 1 && e->a <= PLUGUI_KEY_MAX && e->a == unsigned(e->a) && (e->b == 0 || e->b == 1);
	case PLUGUI_INPUT_TEXT: return e->a >= 1 && e->a <= 1114111 && e->a == unsigned(e->a) &&
		!(e->a >= 55296 && e->a <= 57343) && e->b == 0;
	case PLUGUI_INPUT_RESET: return e->a == 0 && e->b == 0;
	default: return false;
	}
}
static qboolean QDECL Input(const pluguiowner_t *o, const pluguiinputevent_t *e)
{
	Context *c = Find(o);
	if (!c || !Interactive(*o) || !ValidEvent(e) ||
		(e->type != PLUGUI_INPUT_RESET && (c->events == PLUGUI_INPUT_MAX_EVENTS ||
			unsigned(c->imgui->InputEventsQueue.Size) >= PLUGUI_INPUT_MAX_EVENTS))) return qfalse;
	static const ImGuiKey keys[] = {ImGuiKey_None,ImGuiKey_Tab,ImGuiKey_LeftArrow,ImGuiKey_RightArrow,
		ImGuiKey_UpArrow,ImGuiKey_DownArrow,ImGuiKey_Home,ImGuiKey_End,ImGuiKey_PageUp,ImGuiKey_PageDown,
		ImGuiKey_Backspace,ImGuiKey_Delete,ImGuiKey_Enter,ImGuiKey_Escape,ImGuiKey_Space,
		ImGuiKey(ImGuiMod_Ctrl),ImGuiKey(ImGuiMod_Shift),ImGuiKey(ImGuiMod_Alt),ImGuiKey(ImGuiMod_Super)};
	static_assert(sizeof(keys)/sizeof(*keys) == PLUGUI_KEY_MAX+1,"compact key mapping");
	ImGuiContext *previous = ImGui::GetCurrentContext(); ImGui::SetCurrentContext(c->imgui);
	ImGuiIO &io = ImGui::GetIO();
	switch (e->type) {
	case PLUGUI_INPUT_MOUSEPOS: io.AddMousePosEvent(e->a,e->b); break;
	case PLUGUI_INPUT_BUTTON: io.AddMouseButtonEvent(int(e->a),e->b != 0); break;
	case PLUGUI_INPUT_WHEEL: io.AddMouseWheelEvent(e->a,e->b); break;
	case PLUGUI_INPUT_KEY: io.AddKeyEvent(keys[unsigned(e->a)],e->b != 0); break;
	case PLUGUI_INPUT_TEXT: io.AddInputCharacter(unsigned(e->a)); break;
	case PLUGUI_INPUT_RESET:
		io.ClearEventsQueue(); io.ClearInputKeys(); io.ClearInputMouse();
		//Cancel the active widget as well as keys: a later release cannot activate it.
		ImGui::SetWindowFocus(nullptr);
		c->events = c->actions = c->nextaction = 0;
		break;
	}
	if (e->type != PLUGUI_INPUT_RESET) c->events++;
	ImGui::SetCurrentContext(previous);
	return qtrue;
}
static qboolean QDECL Poll(const pluguiowner_t *o, pluguiaction_t *action)
{
	Context *c = Find(o);
	if (!c || !action || !Interactive(*o) || c->model.revision || c->nextaction == c->actions) return qfalse;
	*action = c->pending[c->nextaction++]; return qtrue;
}
static void Action(Context &c, unsigned id, float value)
{
	if (c.actions == PLUGUI_INPUT_MAX_ACTIONS) { c.actionoverflow = true; return; }
	c.pending[c.actions++] = {id,c.owner.generation,value};
}
static void InteractiveGallery(Context &c)
{
	ImGui::SetNextWindowPos(ImVec2(70,70),ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(540,330),ImGuiCond_Always);
	ImGui::Begin("Interactive gallery",nullptr,ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);
	ImGui::TextUnformatted("Bounded explicit QC input / diagnostic only");
	if (ImGui::Button("Click action",ImVec2(150,28)))
	{
		if (c.clicks < 999999) c.clicks++;
		Action(c,1,float(c.clicks));
	}
	ImGui::SameLine();
	if (ImGui::Checkbox("Enabled",&c.enabled)) Action(c,2,c.enabled ? 1.0f : 0.0f);
	ImGui::SetNextItemWidth(260);
	if (ImGui::InputText("Bounded text",c.text,sizeof(c.text))) Action(c,3,float(std::strlen(c.text)));
	ImGui::Text("Clicks: %u / enabled: %d / bytes: %u",c.clicks,c.enabled,unsigned(std::strlen(c.text)));
	ImGui::TextUnformatted("Reset abandons held input; no implicit clipboard/IME hooks");
	ImGui::End();
}
