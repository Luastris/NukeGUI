// NukeGUI — runtime immediate-mode GUI (gameplay plugin).
//
// imgui vendored STATICALLY (own context; internal symbols) — ships in the Player, no extra dll, no
// clash with the editor's NukeImGui. Renderer-INDEPENDENT: draw lists -> neutral nuke::NukeUIDrawData
// -> iRender 2D seam. The GAME draws via the engine's nuke::iGUI (Component::OnGUI); this plugin is just
// the backend that implements iGUI with imgui and composites into the camera/viewport RT.
#include <interface/NUKEEInteface.h>   // NUKEModule + AppInstance
#include <interface/iGUI.h>            // engine GUI facade (game codes against this)
#include <render/irender.h>            // iRender + NukeUIDrawData (neutral 2D seam)
#include <API/Model/Atom.h>
#include <API/Model/Component.h>
#include <imgui.h>
#include <vector>
#include <chrono>

using namespace nuke;

// imgui-backed implementation of the engine's iGUI (called inside Component::OnGUI via nuke::GUI()).
struct GUIBackend : iGUI
{
    bool Begin(const char* n) override { return ImGui::Begin(n); }
    void End() override { ImGui::End(); }
    void Text(const char* s) override { ImGui::TextUnformatted(s); }
    bool Button(const char* s) override { return ImGui::Button(s); }
    void SameLine() override { ImGui::SameLine(); }
    void Separator() override { ImGui::Separator(); }
    bool Checkbox(const char* l, bool* v) override { return ImGui::Checkbox(l, v); }
    bool SliderFloat(const char* l, float* v, float lo, float hi) override { return ImGui::SliderFloat(l, v, lo, hi); }
};

static void DispatchOnGUI(Atom* a)
{
    if (!a) return;
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
        strcpy(description, "Runtime immediate-mode GUI (static imgui; game draws via nuke::GUI / Component::OnGUI).");
        strcpy(version, "0.1.0.0");
        strcpy(site, "https://luastris.com");
        tags = { "gui", "imgui", "runtime-ui" };
    }

    // Service metadata: the active runtime-GUI backend (SetGUIBackend already enforces a
    // single live backend; provides() makes that exclusivity visible to the loader/window).
    const char* provides() override { return "gui"; }

    void OnLoad() override { IMGUI_CHECKVERSION(); ctx = ImGui::CreateContext(); }

    void Run(AppInstance* inst) override
    {
        instance = inst; stopped = false;
        SetGUIBackend(&backend);                                  // game's nuke::GUI() now forwards here
        if (instance && instance->render)
            instance->render->setOnRender([this] { Frame(); });  // composite into the camera/viewport RT
    }

    bool HasSettings() override { return false; }
    void Settings() override {}
    void Shutdown() override
    {
        stopped = true;
        SetGUIBackend(nullptr);
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
        inited = true;
    }

    void Frame()
    {
        if (!instance || !instance->render) return;
        if (instance->playState == 0) return;        // only while playing (PIE / Player)
        iRender* r = instance->render;
        ImGui::SetCurrentContext(ctx);
        if (!inited) Setup();
        ImGuiIO& io = ImGui::GetIO();
        int tw = instance->uiW > 0 ? instance->uiW : r->width;
        int th = instance->uiH > 0 ? instance->uiH : r->height;
        io.DisplaySize = ImVec2((float)tw, (float)th);
        auto now = std::chrono::steady_clock::now();
        io.DeltaTime = haveLast ? std::chrono::duration<float>(now - last).count() : (1.0f / 60.0f);
        if (io.DeltaTime <= 0.0f) io.DeltaTime = 1.0f / 60.0f;
        last = now; haveLast = true;

        // Input: map to the target rect; only deliver clicks when the cursor is INSIDE the viewport
        // (so the game UI owns input there, and clicks elsewhere don't hit it).
        double mx = 0, my = 0; r->getCursorPos(mx, my);
        float lx = (float)(mx - instance->uiX), ly = (float)(my - instance->uiY);
        bool inside = lx >= 0 && ly >= 0 && lx < (float)tw && ly < (float)th;
        io.AddMousePosEvent(lx, ly);
        io.AddMouseButtonEvent(0, inside && r->isMouseButtonDown(0));
        io.AddMouseButtonEvent(1, inside && r->isMouseButtonDown(1));
        io.AddMouseButtonEvent(2, inside && r->isMouseButtonDown(2));

        ImGui::NewFrame();
        if (instance->currentScene)                          // the game draws its UI via Component::OnGUI
            for (Atom* a : instance->currentScene->GetHierarchy()) DispatchOnGUI(a);
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
