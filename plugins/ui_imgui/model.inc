//Included inside FteImGui. Snapshot/actions have stable identity, not label/index.
static qboolean QDECL SetModel2(const pluguiowner_t *o, const pluguimodel2_t *m)
{
	Context *c = Find(o);
	if (!c || !Interactive(*o) || !PlugUI_Model2Valid(m) || m->revision <= c->model.revision ||
		(ScoresOwner(*o) && !ScoresModelValid(*m))) return qfalse;
	pluguiinputevent_t reset = {sizeof(reset),PLUGUI_INPUT_RESET,0,0};
	if (!Input(o,&reset)) return qfalse;
	if (ScoresOwner(*o))
	{
		unsigned before = ScoresRowBase(c->model), after = ScoresRowBase(*m);
		unsigned rows = (m->count-after)/9;
		if (!c->model.revision) c->scoretop = true;
		else if ((c->model.count-before)/9 != rows) c->scoretop = true;
		else if (rows && c->model.widgets[before].row != m->widgets[after].row) c->scoretop = true;
		//Page/reorder replacement starts at its first row. Same identities keep
		//their vertical view through checkbox/font/geometry-only revisions.
	}
	c->model = *m;
	return qtrue;
}
static qboolean QDECL SetModel(const pluguiowner_t *o, const pluguimodel_t *m)
{
	if (!PlugUI_ModelValid(m)) return qfalse;
	pluguimodel2_t dense = {}; dense.structsize = sizeof(dense);
	dense.revision = m->revision; dense.count = m->count;
	std::memcpy(dense.widgets,m->widgets,m->count*sizeof(m->widgets[0]));
	return SetModel2(o,&dense);
}
static qboolean QDECL PollModel(const pluguiowner_t *o, pluguimodelaction_t *a)
{
	Context *c = Find(o);
	if (!c || !a || !c->model.revision || c->nextaction == c->actions) return qfalse;
	*a = c->modelpending[c->nextaction++]; return qtrue;
}
static void ModelAction(Context &c, const pluguiwidget_t &w, float value)
{
	if (c.actions == PLUGUI_INPUT_MAX_ACTIONS) { c.actionoverflow = true; return; }
	c.modelpending[c.actions++] = {w.id,w.row,c.owner.generation,c.model.revision,value};
}
static void ModelGallery(Context &c)
{
	ImGui::SetNextWindowPos(ImVec2(70,70),ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(540,330),ImGuiCond_Always);
	ImGui::Begin("Counted model",nullptr,ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);
	ImGui::Text("Counted model / revision %u / widgets %u",c.model.revision,c.model.count);
	for (unsigned i = 0; i < c.model.count; i++)
	{
		const pluguiwidget_t &w = c.model.widgets[i];
		ImGui::PushID(int(w.row)); ImGui::PushID(int(w.id));
		if (w.type == PLUGUI_WIDGET_TEXT) ImGui::TextUnformatted(w.label);
		else if (w.type == PLUGUI_WIDGET_BUTTON)
		{
			//Labels are plain text: ##/### must not alter ImGui identity or hide bytes.
			ImVec2 pos = ImGui::GetCursorScreenPos();
			if (ImGui::Button("##action",ImVec2(220,24))) ModelAction(c,w,1);
			ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x+8,pos.y+5),ImGui::GetColorU32(ImGuiCol_Text),w.label);
		}
		else
		{
			bool value = w.value != 0;
			if (ImGui::Checkbox("##value",&value)) ModelAction(c,w,value ? 1.0f : 0.0f);
			ImGui::SameLine(); ImGui::TextUnformatted(w.label);
		}
		ImGui::PopID(); ImGui::PopID();
	}
	ImGui::End();
}
