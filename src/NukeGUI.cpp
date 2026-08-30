// NukeGUI — runtime immediate-mode GUI backend: implements the engine's nuke::iGUI with a
// STATICALLY vendored imgui (own context, so no clash with the editor's NukeImGui) and
// composites into the camera/viewport RT via the neutral NukeUIDrawData / iRender 2D seam.
#include <interface/NUKEEInteface.h>   // NUKEModule + AppInstance
#include <interface/iGUI.h>            // engine GUI facade (game codes against this)
#include <API/iGUI.h>                  // retained Ui layer, emitted per frame
#include <render/irender.h>            // iRender + NukeUIDrawData (neutral 2D seam)
#include <API/Model/Atom.h>
#include <API/Model/Component.h>
#include <API/Model/Texture.h>
#include <API/Model/DevConsole.h>     // the engine dev console draws inside our frame
#include <API/Model/resdb.h>
#include <imgui.h>
#include <map>
#include <string>
#include <vector>
#include <chrono>

using namespace nuke;

// The input seam's key codes are GLFW numbering.
static ImGuiKey KeyToImGuiKey(int key)
{
	switch (key)
	{
	case 258: return ImGuiKey_Tab;          // GLFW_KEY_TAB
	case 263: return ImGuiKey_LeftArrow;
	case 262: return ImGuiKey_RightArrow;
	case 265: return ImGuiKey_UpArrow;
	case 264: return ImGuiKey_DownArrow;
	case 266: return ImGuiKey_PageUp;
	case 267: return ImGuiKey_PageDown;
	case 268: return ImGuiKey_Home;
	case 269: return ImGuiKey_End;
	case 260: return ImGuiKey_Insert;
	case 261: return ImGuiKey_Delete;
	case 259: return ImGuiKey_Backspace;
	case  32: return ImGuiKey_Space;
	case 257: return ImGuiKey_Enter;
	case 256: return ImGuiKey_Escape;
	case 335: return ImGuiKey_KeypadEnter;
	case 340: return ImGuiKey_LeftShift;
	case 341: return ImGuiKey_LeftCtrl;
	case 342: return ImGuiKey_LeftAlt;
	case 343: return ImGuiKey_LeftSuper;
	case 344: return ImGuiKey_RightShift;
	case 345: return ImGuiKey_RightCtrl;
	case 346: return ImGuiKey_RightAlt;
	case 347: return ImGuiKey_RightSuper;
	default: break;
	}
	if (key >= 48 && key <= 57)  return (ImGuiKey)(ImGuiKey_0 + (key - 48));    // 0..9
	if (key >= 65 && key <= 90)  return (ImGuiKey)(ImGuiKey_A + (key - 65));    // A..Z
	if (key >= 290 && key <= 301) return (ImGuiKey)(ImGuiKey_F1 + (key - 290)); // F1..F12
	return ImGuiKey_None;
}

// Clipboard bridge: imgui platform callbacks -> the render seam (GLFW clipboard).
static iRender* g_clipRender = nullptr;
static const char* ClipGet(ImGuiContext*) { return g_clipRender ? g_clipRender->getClipboardText() : ""; }
static void ClipSet(ImGuiContext*, const char* t) { if (g_clipRender) g_clipRender->setClipboardText(t); }

// imgui-backed implementation of the engine's iGUI, reached from Component::OnGUI via nuke::GUI().
struct GUIBackend : iGUI
{
	iRender* render = nullptr;                 // for the image cache (owned textures)
	ImGuiStyle defaultStyle;                   // snapshot for ResetStyle()
	bool haveDefault = false;

	// texture-asset images: guid -> (renderer texture handle, native size)
	struct CachedImage { uint64_t tex = 0; float w = 0, h = 0; };
	std::map<std::string, CachedImage> images;

	bool Begin(const char* n) override { return ImGui::Begin(n); }
	void End() override { ImGui::End(); }
	void Text(const char* s) override { ImGui::TextUnformatted(s); }
	bool Button(const char* s) override { return ImGui::Button(s); }
	void SameLine() override { ImGui::SameLine(); }
	void Separator() override { ImGui::Separator(); }
	bool Checkbox(const char* l, bool* v) override { return ImGui::Checkbox(l, v); }
	bool SliderFloat(const char* l, float* v, float lo, float hi) override { return ImGui::SliderFloat(l, v, lo, hi); }

	bool InputText(const char* l, char* buf, int cap) override { return ImGui::InputText(l, buf, (size_t)cap); }
	bool Combo(const char* l, int* cur, const char* const* items, int n) override
	{ return ImGui::Combo(l, cur, items, n); }
	void ProgressBar(float f, const char* overlay) override
	{ ImGui::ProgressBar(f, ImVec2(0, 0), (overlay && overlay[0]) ? overlay : nullptr); }

	void Image(const char* texGuid, float w, float h) override
	{
		if (!texGuid || !texGuid[0]) return;
		auto it = images.find(texGuid);
		if (it == images.end())
		{
			CachedImage ci;
			if (Texture* t = ResDB::getSingleton()->GetTexture(texGuid))
			{
				std::vector<unsigned char> rgba = t->DecodeRGBA();
				if (!rgba.empty() && render)
				{
					ci.tex = render->createTexture2D(rgba.data(), t->width, t->height);
					ci.w = (float)t->width; ci.h = (float)t->height;
				}
			}
			it = images.emplace(texGuid, ci).first;   // failures cache too: no per-frame retry
		}
		if (!it->second.tex) { ImGui::TextDisabled("[image: %s]", texGuid); return; }
		const float iw = w > 0 ? w : it->second.w;
		const float ih = h > 0 ? h : it->second.h;
		ImGui::Image((ImTextureID)it->second.tex, ImVec2(iw, ih));
	}

	// Neutral NUKEUI_COL_* -> imgui; ImGuiCol_COUNT = unmapped.
	static ImGuiCol MapColor(int what)
	{
		switch (what)
		{
		case NUKEUI_COL_TEXT:             return ImGuiCol_Text;
		case NUKEUI_COL_WINDOW_BG:        return ImGuiCol_WindowBg;
		case NUKEUI_COL_FRAME_BG:         return ImGuiCol_FrameBg;
		case NUKEUI_COL_FRAME_BG_HOVERED: return ImGuiCol_FrameBgHovered;
		case NUKEUI_COL_FRAME_BG_ACTIVE:  return ImGuiCol_FrameBgActive;
		case NUKEUI_COL_TITLE_BG:         return ImGuiCol_TitleBg;
		case NUKEUI_COL_TITLE_BG_ACTIVE:  return ImGuiCol_TitleBgActive;
		case NUKEUI_COL_BUTTON:           return ImGuiCol_Button;
		case NUKEUI_COL_BUTTON_HOVERED:   return ImGuiCol_ButtonHovered;
		case NUKEUI_COL_BUTTON_ACTIVE:    return ImGuiCol_ButtonActive;
		case NUKEUI_COL_CHECK_MARK:       return ImGuiCol_CheckMark;
		case NUKEUI_COL_SLIDER_GRAB:      return ImGuiCol_SliderGrab;
		case NUKEUI_COL_BORDER:           return ImGuiCol_Border;
		case NUKEUI_COL_SEPARATOR:        return ImGuiCol_Separator;
		case NUKEUI_COL_PROGRESS:         return ImGuiCol_PlotHistogram;   // progress fill
		default:                          return ImGuiCol_COUNT;
		}
	}

	void SnapshotDefault()
	{
		if (haveDefault) return;
		defaultStyle = ImGui::GetStyle();
		haveDefault = true;
	}

	void StyleColor(int what, float r, float g, float b, float a) override
	{
		SnapshotDefault();
		const ImGuiCol c = MapColor(what);
		if (c != ImGuiCol_COUNT) ImGui::GetStyle().Colors[c] = ImVec4(r, g, b, a);
	}

	void StyleVar(int what, float x, float y) override
	{
		SnapshotDefault();
		ImGuiStyle& s = ImGui::GetStyle();
		switch (what)
		{
		case NUKEUI_VAR_ALPHA:           s.Alpha = x; break;
		case NUKEUI_VAR_WINDOW_ROUNDING: s.WindowRounding = x; break;
		case NUKEUI_VAR_FRAME_ROUNDING:  s.FrameRounding = x; break;
		case NUKEUI_VAR_GRAB_ROUNDING:   s.GrabRounding = x; break;
		case NUKEUI_VAR_BORDER_SIZE:     s.WindowBorderSize = x; s.FrameBorderSize = x; break;
		case NUKEUI_VAR_WINDOW_PADDING:  s.WindowPadding = ImVec2(x, y); break;
		case NUKEUI_VAR_FRAME_PADDING:   s.FramePadding = ImVec2(x, y); break;
		case NUKEUI_VAR_ITEM_SPACING:    s.ItemSpacing = ImVec2(x, y); break;
		default: break;
		}
	}

	void FontScale(float s) override
	{
		SnapshotDefault();
		if (s > 0.01f) ImGui::GetStyle().FontScaleMain = s;
	}

	void ResetStyle() override
	{
		if (haveDefault) ImGui::GetStyle() = defaultStyle;
	}

	void SetNextWindowRect(float x, float y, float w, float h) override
	{
		ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
		if (w > 0 && h > 0) ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
	}

	// ---- dev-console widgets (iGUI tail) ----
	void TextColored(float r, float g, float b, float a, const char* s) override
	{ ImGui::TextColored(ImVec4(r, g, b, a), "%s", s ? s : ""); }

	// Up/Down replace the edit buffer with history lines; ` and ~ are filtered (console toggle).
	struct HistCtx { const char* const* items; int count; int* pos; };
	static int InputCb(ImGuiInputTextCallbackData* d)
	{
		HistCtx* c = (HistCtx*)d->UserData;
		if (d->EventFlag == ImGuiInputTextFlags_CallbackCharFilter)
			return (d->EventChar == '`' || d->EventChar == '~') ? 1 : 0;
		if (d->EventFlag == ImGuiInputTextFlags_CallbackHistory && c && c->count > 0)
		{
			int& p = *c->pos;
			if (d->EventKey == ImGuiKey_UpArrow)        p = p < 0 ? c->count - 1 : (p > 0 ? p - 1 : 0);
			else if (d->EventKey == ImGuiKey_DownArrow) { if (p >= 0 && ++p >= c->count) p = -1; }
			d->DeleteChars(0, d->BufTextLen);
			if (p >= 0 && p < c->count) d->InsertChars(0, c->items[p]);
		}
		return 0;
	}
	std::map<unsigned int, int> histPos;   // per-widget history cursor (-1 = fresh line)
	bool InputTextHistory(const char* label, char* buf, int cap,
	                      const char* const* history, int histCount) override
	{
		auto it = histPos.find(ImGui::GetID(label));
		if (it == histPos.end()) it = histPos.emplace(ImGui::GetID(label), -1).first;
		HistCtx ctx{ history, histCount, &it->second };
		ImGui::SetNextItemWidth(-1);
		const bool submit = ImGui::InputText(label, buf, (size_t)cap,
			ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory |
			ImGuiInputTextFlags_CallbackCharFilter, &InputCb, &ctx);
		if (submit) it->second = -1;
		return submit;
	}
	void FocusNextWidget() override { ImGui::SetKeyboardFocusHere(); }
	void BeginScrollRegion(const char* id, float h) override
	{ ImGui::BeginChild(id, ImVec2(0, h <= 0 ? -ImGui::GetFrameHeightWithSpacing() : h)); }
	void EndScrollRegion() override { ImGui::EndChild(); }
	void ScrollToBottom() override { ImGui::SetScrollHereY(1.0f); }

	void DropImageCache(iRender* r)
	{
		for (auto& kv : images)
			if (kv.second.tex && r) r->destroyTexture2D(kv.second.tex);
		images.clear();
	}
};

static void DispatchOnGUI(Atom* a)
{
	if (!a || !a->enabled) return;   // disabled atom = whole subtree off
	for (Component* c : a->components) if (c && c->enabled) c->OnGUI();
	for (Atom* ch : a->children) DispatchOnGUI(ch);
}

struct NukeGUIModule : public NUKEModule
{
	ImGuiContext* ctx = nullptr;
	GUIBackend    backend;
	bool          inited = false;
	std::vector<NukeUIDrawList>         lists;
	std::vector<std::vector<NukeUICmd>> cmds;
	std::chrono::steady_clock::time_point last;
	bool haveLast = false;

	NukeGUIModule()
	{
		strcpy(title, "NukeGUI");
		strcpy(author, "Luastris");
		strcpy(description, "Runtime GUI (static imgui; immediate nuke::GUI + retained nuke::Ui).");
		strcpy(version, "0.2.0.0");
		strcpy(site, "https://luastris.com");
		tags = { "gui", "imgui", "runtime-ui" };
	}

	const char* provides() override { return "gui"; }   // only one live GUI backend at a time

	void OnLoad() override { IMGUI_CHECKVERSION(); ctx = ImGui::CreateContext(); }

	void Run(AppInstance* inst) override
	{
		instance = inst; stopped = false;
		SetGUIBackend(&backend);                                  // nuke::GUI() now forwards here
		if (instance && instance->render)
			instance->render->setOnRender([this] { Frame(); });
	}

	bool HasSettings() override { return false; }
	void Settings() override {}
	void Shutdown() override
	{
		stopped = true;
		SetGUIBackend(nullptr);
		g_clipRender = nullptr;
		if (instance && instance->render) backend.DropImageCache(instance->render);
		if (ctx) { ImGui::DestroyContext(ctx); ctx = nullptr; }
	}

	void UpdateTextures(ImDrawData* dd, iRender* r)
	{
		if (!dd->Textures) return;
		for (ImTextureData* tex : *dd->Textures)
		{
			if (tex->Status == ImTextureStatus_WantCreate)
			{
				tex->SetTexID((ImTextureID)r->createTexture2D(tex->GetPixels(), tex->Width, tex->Height));
				tex->SetStatus(ImTextureStatus_OK);
			}
			else if (tex->Status == ImTextureStatus_WantUpdates)
			{
				if (tex->GetTexID() != ImTextureID_Invalid) r->destroyTexture2D((uint64_t)tex->GetTexID());
				tex->SetTexID((ImTextureID)r->createTexture2D(tex->GetPixels(), tex->Width, tex->Height));
				tex->SetStatus(ImTextureStatus_OK);
			}
			else if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0)
			{
				if (tex->GetTexID() != ImTextureID_Invalid) r->destroyTexture2D((uint64_t)tex->GetTexID());
				tex->SetTexID(ImTextureID_Invalid);
				tex->SetStatus(ImTextureStatus_Destroyed);
			}
		}
	}

	void Setup()
	{
		ImGui::SetCurrentContext(ctx);
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
		io.Fonts->AddFontDefault();
		ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
		pio.Platform_GetClipboardTextFn = &ClipGet;
		pio.Platform_SetClipboardTextFn = &ClipSet;
		inited = true;
	}

	void FeedInput(iRender* r, bool inside)
	{
		ImGuiIO& io = ImGui::GetIO();

		// The seam queues are drained even when the cursor is outside, or stale input bursts in later.
		unsigned int chars[64];
		int n = r->fetchUIChars(chars, 64);
		for (int i = 0; i < n; ++i) if (inside) io.AddInputCharacter(chars[i]);

		int keys[64], actions[64], mods[64];
		n = r->fetchUIKeys(keys, actions, mods, 64);
		for (int i = 0; i < n; ++i)
		{
			if (actions[i] != 0 && actions[i] != 1) continue;   // press/release only (no repeat: imgui does it)
			const bool down = actions[i] == 1;
			// Modifier state must be sent BEFORE the key that uses it.
			io.AddKeyEvent(ImGuiMod_Ctrl,  (mods[i] & 0x2) != 0);
			io.AddKeyEvent(ImGuiMod_Shift, (mods[i] & 0x1) != 0);
			io.AddKeyEvent(ImGuiMod_Alt,   (mods[i] & 0x4) != 0);
			io.AddKeyEvent(ImGuiMod_Super, (mods[i] & 0x8) != 0);
			const ImGuiKey k = KeyToImGuiKey(keys[i]);
			if (k != ImGuiKey_None && inside) io.AddKeyEvent(k, down);
		}

		double sx = 0, sy = 0;
		r->getScrollDelta(sx, sy);
		if (inside && (sx != 0 || sy != 0)) io.AddMouseWheelEvent((float)sx, (float)sy);
	}

	void Frame()
	{
		if (!instance || !instance->render) return;
		if (instance->playState == 0) return;        // only while playing (PIE / Player)
		iRender* r = instance->render;
		ImGui::SetCurrentContext(ctx);
		if (!inited) Setup();
		g_clipRender = r;
		ImGuiIO& io = ImGui::GetIO();
		int tw = instance->uiW > 0 ? instance->uiW : r->width;
		int th = instance->uiH > 0 ? instance->uiH : r->height;
		io.DisplaySize = ImVec2((float)tw, (float)th);
		auto now = std::chrono::steady_clock::now();
		io.DeltaTime = haveLast ? std::chrono::duration<float>(now - last).count() : (1.0f / 60.0f);
		if (io.DeltaTime <= 0.0f) io.DeltaTime = 1.0f / 60.0f;
		last = now; haveLast = true;

		// Clicks are only delivered when the cursor is INSIDE the viewport rect.
		double mx = 0, my = 0; r->getCursorPos(mx, my);
		float lx = (float)(mx - instance->uiX), ly = (float)(my - instance->uiY);
		bool inside = lx >= 0 && ly >= 0 && lx < (float)tw && ly < (float)th;
		io.AddMousePosEvent(lx, ly);
		io.AddMouseButtonEvent(0, inside && r->isMouseButtonDown(0));
		io.AddMouseButtonEvent(1, inside && r->isMouseButtonDown(1));
		io.AddMouseButtonEvent(2, inside && r->isMouseButtonDown(2));
		backend.render = r;
		FeedInput(r, inside);

		ImGui::NewFrame();
		if (instance->currentWorld)
		{
			// OnGUI enters the script VM on the render thread while the fixed thread may also
			// be inside Lua — both sweeps must run under the game lock.
			instance->currentWorld->LockGame();
			for (Atom* a : instance->currentWorld->GetHierarchy()) DispatchOnGUI(a);
			nuke::Ui::Emit();                                // retained tree, same lock
			nuke::Console::Emit();                           // dev console on top, same lock
			instance->currentWorld->UnlockGame();
		}
		ImGui::Render();

		ImDrawData* dd = ImGui::GetDrawData();
		if (!dd) return;
		UpdateTextures(dd, r);

		cmds.clear(); cmds.resize(dd->CmdListsCount); lists.clear();
		const ImVec2 pos = dd->DisplayPos, scale = dd->FramebufferScale;
		for (int i = 0; i < dd->CmdListsCount; ++i)
		{
			const ImDrawList* dl = dd->CmdLists[i];
			std::vector<NukeUICmd>& cl = cmds[i];
			for (int c = 0; c < dl->CmdBuffer.Size; ++c)
			{
				const ImDrawCmd& dc = dl->CmdBuffer[c];
				if (dc.UserCallback || dc.ElemCount == 0) continue;
				NukeUICmd nc{};
				nc.clipRect[0] = (dc.ClipRect.x - pos.x) * scale.x;
				nc.clipRect[1] = (dc.ClipRect.y - pos.y) * scale.y;
				nc.clipRect[2] = (dc.ClipRect.z - pos.x) * scale.x;
				nc.clipRect[3] = (dc.ClipRect.w - pos.y) * scale.y;
				nc.texId = (uint64_t)dc.GetTexID();
				nc.elemCount = dc.ElemCount; nc.idxOffset = dc.IdxOffset; nc.vtxOffset = dc.VtxOffset;
				cl.push_back(nc);
			}
			NukeUIDrawList nl{};
			nl.vtx = reinterpret_cast<const NukeUIVert*>(dl->VtxBuffer.Data);  // ImDrawVert == NukeUIVert layout
			nl.vtxCount = dl->VtxBuffer.Size;
			nl.idx = reinterpret_cast<const uint16_t*>(dl->IdxBuffer.Data);
			nl.idxCount = dl->IdxBuffer.Size;
			nl.cmds = cl.data(); nl.cmdCount = (int)cl.size();
			lists.push_back(nl);
		}
		NukeUIDrawData nd{};
		nd.lists = lists.data(); nd.listCount = (int)lists.size();
		nd.dispPos[0] = pos.x; nd.dispPos[1] = pos.y;
		nd.dispSize[0] = dd->DisplaySize.x; nd.dispSize[1] = dd->DisplaySize.y;
		r->bindRenderTarget(instance->uiTarget);   // composite into the viewport/camera RT (0 = backbuffer)
		r->renderDrawLists(nd);
		r->bindRenderTarget(0);
	}
};

extern "C" BOOST_SYMBOL_EXPORT NukeGUIModule plugin;
NukeGUIModule plugin;
