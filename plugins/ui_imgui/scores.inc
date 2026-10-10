//Owner 205: real scoreboard table; QC owns data, paging and action meaning.
static bool ScoresOwner(const pluguiowner_t &o)
{
	return o.vm == PLUGUI_VM_CLIENT && o.owner == ScoresClient;
}
static constexpr float ScoresFontPixels[] = {13,16,20,24};
static unsigned ScoresRowBase(const pluguimodel2_t &m)
{
	return m.count >= 6 && m.widgets[5].id == 6 ? 6 : 5;
}
static int ScoresFontIndex(const pluguimodel2_t &m)
{
	if (ScoresRowBase(m) == 5) return 0;
	const char *labels[] = {"13","16","20","24"};
	for (unsigned i = 0; i < 4; i++)
		if (!std::strcmp(m.widgets[5].label,labels[i])) return int(i);
	return -1;
}
static bool ScoresModelValid(const pluguimodel2_t &m)
{
	unsigned base = ScoresRowBase(m);
	//A dense page has at most 24 rows (222 widgets with font metadata).
	if (m.count < base || (m.count-base)%9 || (m.count-base)/9 > 24) return false;
	//Optional physical-font metadata; old five-widget snapshots stay at 13px.
	if (base == 6 && (m.widgets[5].row != 1 || m.widgets[5].type != PLUGUI_WIDGET_TEXT ||
		ScoresFontIndex(m) < 0)) return false;
	for (unsigned i = 0; i < 4; i++)
	{
		const pluguiwidget_t &w = m.widgets[i];
		if (w.id != i+1 || w.row != 1) return false;
		if (i == 0 && w.type != PLUGUI_WIDGET_CHECKBOX) return false;
		if (i == 1 && w.type != PLUGUI_WIDGET_TEXT) return false;
		if (i >= 2 && w.type != PLUGUI_WIDGET_TEXT && w.type != PLUGUI_WIDGET_BUTTON) return false;
	}
	const pluguiwidget_t &standing = m.widgets[4];
	if (standing.id != 5 || standing.type != PLUGUI_WIDGET_CHECKBOX ||
		(!standing.value && standing.row != 1)) return false;
	bool highlighted = !standing.value;
	for (unsigned i = base; i < m.count; i++)
	{
		const pluguiwidget_t &w = m.widgets[i];
		unsigned role = (i-base)%9+1;
		if (w.row < 16 || w.row > (PLUGUI_MODEL_MAX_ID-9)/16 || w.id != w.row*16+role ||
			w.row != m.widgets[i-role+1].row) return false;
		if (role == 1 && standing.value && standing.row == w.row) highlighted = true;
		if (role == 4)
		{
			if (w.type != PLUGUI_WIDGET_TEXT && w.type != PLUGUI_WIDGET_BUTTON) return false;
		}
		else if (role == 9)
		{
			if (w.type != PLUGUI_WIDGET_TEXT && w.type != PLUGUI_WIDGET_CHECKBOX) return false;
		}
		else if (w.type != PLUGUI_WIDGET_TEXT) return false;
	}
	return highlighted;
}
//FTESurf's `Dusk` palette, the values of src/shared/sh_ui.qc's SUI_THEME_* (Patch
//606); change them together. Colours and rounding only: the gates click at fixed
//offsets inside this table, so no padding, spacing or border size may move.
static void ScoresTheme(ImGuiStyle &s)
{
	const ImVec4 well(0.075f,0.078f,0.110f,1), panel(0.102f,0.110f,0.149f,1), chip(0.141f,0.153f,0.208f,1),
		hover(0.184f,0.200f,0.275f,1), pressed(0.231f,0.251f,0.345f,1), selected(0.200f,0.262f,0.361f,1),
		edge(0.204f,0.220f,0.286f,1), edgehi(0.314f,0.337f,0.435f,1), text(0.882f,0.894f,0.949f,1),
		muted(0.435f,0.459f,0.565f,1), accent(0.549f,0.769f,0.973f,1), clear(0,0,0,0);
	ImVec4 *c = s.Colors;
	c[ImGuiCol_Text] = text; c[ImGuiCol_TextDisabled] = muted;
	c[ImGuiCol_WindowBg] = well; c[ImGuiCol_ChildBg] = clear; c[ImGuiCol_Border] = edge;
	c[ImGuiCol_FrameBg] = chip; c[ImGuiCol_FrameBgHovered] = hover; c[ImGuiCol_FrameBgActive] = pressed;
	c[ImGuiCol_Button] = chip; c[ImGuiCol_ButtonHovered] = hover; c[ImGuiCol_ButtonActive] = pressed;
	c[ImGuiCol_Header] = selected; c[ImGuiCol_HeaderHovered] = hover; c[ImGuiCol_HeaderActive] = pressed;
	c[ImGuiCol_CheckMark] = accent; c[ImGuiCol_NavCursor] = accent;
	c[ImGuiCol_ScrollbarBg] = clear; c[ImGuiCol_ScrollbarGrab] = edgehi;
	c[ImGuiCol_ScrollbarGrabHovered] = muted; c[ImGuiCol_ScrollbarGrabActive] = accent;
	c[ImGuiCol_TableHeaderBg] = panel; c[ImGuiCol_TableBorderStrong] = edge;
	c[ImGuiCol_TableBorderLight] = ImVec4(edge.x,edge.y,edge.z,0.55f);
	c[ImGuiCol_TableRowBg] = clear; c[ImGuiCol_TableRowBgAlt] = ImVec4(chip.x,chip.y,chip.z,0.45f);
	s.ScrollbarRounding = 6; s.GrabRounding = 4;
	//A held peek is drawn disabled; the default 0.6 read as a washed-out board.
	s.DisabledAlpha = 0.85f;
}
static void ScoresFont(Context &c)
{
	int index = ScoresFontIndex(c.model);
	if (index < 0 || !c.scoresfonts[index]) return;
	ImGui::GetIO().FontDefault = c.scoresfonts[index];
	//Scale from the immutable base, never repeatedly scale a rounded style.
	ImGui::GetStyle() = c.scoresstyle;
	ImGui::GetStyle().ScaleAllSizes(ScoresFontPixels[index]/13);
}
static void ScoresWidget(Context &c, const pluguiwidget_t &w)
{
	ImGui::PushID(int(w.row)); ImGui::PushID(int(w.id));
	if (w.type == PLUGUI_WIDGET_TEXT) ImGui::TextUnformatted(w.label);
	else if (w.type == PLUGUI_WIDGET_BUTTON)
	{
		//Neither identity nor visible bytes come from ImGui's ##/### label grammar.
		ImVec2 pos = ImGui::GetCursorScreenPos();
		if (ImGui::Button("##action",ImVec2(w.row == 1 ? 90*ImGui::GetFontSize()/13 : ImGui::GetContentRegionAvail().x,0))) ModelAction(c,w,1);
		ImDrawList *list = ImGui::GetWindowDrawList();
		list->PushClipRect(ImGui::GetItemRectMin(),ImGui::GetItemRectMax(),true);
		list->AddText(ImVec2(pos.x+ImGui::GetStyle().FramePadding.x,pos.y+ImGui::GetStyle().FramePadding.y),ImGui::GetColorU32(ImGuiCol_Text),w.label);
		list->PopClipRect();
	}
	else
	{
		bool value = w.value != 0;
		if (ImGui::Checkbox("##line",&value)) ModelAction(c,w,value ? 1.0f : 0.0f);
		if (w.label[0]) { ImGui::SameLine(); ImGui::TextUnformatted(w.label); }
	}
	ImGui::PopID(); ImGui::PopID();
}
static bool ScoresGallery(Context &c, const pluguiframe_t &f)
{
	if (!c.model.revision || !ScoresModelValid(c.model)) return false;
	float width = f.clip[2]-f.clip[0], height = f.clip[3]-f.clip[1];
	//Written so Inf/NaN extents (finite edges can still overflow) are refused too.
	if (!(width >= 150 && height >= 80 && width <= f.pixelwidth && height <= f.pixelheight)) return false;
	bool active = c.model.widgets[0].value != 0;
	ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
		ImGuiWindowFlags_NoNav;
	if (!active) flags |= ImGuiWindowFlags_NoInputs;
	ImGui::SetNextWindowPos(ImVec2(f.clip[0],f.clip[1]),ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(width,height),ImGuiCond_Always);
	ImGui::Begin("Scores table",nullptr,flags);
	//Table scrolling creates child windows; NoInputs on the parent alone does
	//not disable their widgets. Passive snapshots must be inert throughout.
	ImGui::BeginDisabled(!active);
	ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(c.model.widgets[1].label); ImGui::PopTextWrapPos();
	unsigned base = ScoresRowBase(c.model);
	float scale = ImGui::GetFontSize()/13;
	float contentwidth = ImGui::GetContentRegionAvail().x;
	bool stacked = contentwidth < 180*scale + ImGui::GetStyle().ItemSpacing.x;
	float footer = ImGui::GetFrameHeightWithSpacing() * (stacked ? 2 : 1);
	if (c.model.widgets[4].label[0])
		footer += ImGui::CalcTextSize(c.model.widgets[4].label,nullptr,false,contentwidth).y + ImGui::GetStyle().ItemSpacing.y;
	float tableheight = ImGui::GetContentRegionAvail().y-footer;
	//Non-positive BeginTable height means fill remaining space, not zero space.
	if (tableheight < ImGui::GetTextLineHeightWithSpacing()*2 + ImGui::GetStyle().ScrollbarSize)
	{
		ImGui::EndDisabled(); ImGui::End(); return false;
	}
	if (c.scoretop) ImGui::SetNextWindowScroll(ImVec2(-1,0));
	if (ImGui::BeginTable("runs",9,ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
		ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
		ImVec2(0,tableheight)))
	{
		c.scoretop = false;
		const char *heads[] = {"#","time","delta","player","replay","age/date","run","why/tag","line"};
		float widths[] = {24,78,82,130,140,78,72,78,78};
		//Verification is part of the replay claim, not an expendable suffix.
		for (float &columnwidth : widths) columnwidth *= scale;
		for (unsigned i = base+4; i < c.model.count; i += 9)
			widths[4] = std::max(widths[4],ImGui::CalcTextSize(c.model.widgets[i].label).x);
		for (unsigned col = 0; col < 9; col++) ImGui::TableSetupColumn(heads[col],ImGuiTableColumnFlags_WidthFixed,widths[col]);
		ImGui::TableSetupScrollFreeze(0,1);
		ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(0.651f,0.671f,0.761f,1)); ImGui::TableHeadersRow(); ImGui::PopStyleColor();
		ImGuiListClipper rows; rows.Begin(int((c.model.count-base)/9));
		while (rows.Step())
			for (int row = rows.DisplayStart; row < rows.DisplayEnd; row++)
			{
				ImGui::TableNextRow();
				if (c.model.widgets[4].value && c.model.widgets[4].row == c.model.widgets[base+row*9].row)
					ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,ImGui::GetColorU32(ImGuiCol_Header));
				for (unsigned col = 0; col < 9; col++)
				{
					ImGui::TableNextColumn(); ScoresWidget(c,c.model.widgets[base+row*9+col]);
				}
			}
		ImGui::EndTable();
	}
	//Standing outside the fetched rows is plain text, never a synthetic target.
	if (c.model.widgets[4].label[0])
	{
		ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(c.model.widgets[4].label); ImGui::PopTextWrapPos();
	}
	ScoresWidget(c,c.model.widgets[2]); if (!stacked) ImGui::SameLine(); ScoresWidget(c,c.model.widgets[3]);
	ImGui::EndDisabled(); ImGui::End();
	return true;
}
