// Renaro Model Playground: a blue terminal-style local model workbench built with Dear ImGui.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define IMGUI_DEFINE_MATH_OPERATORS

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <windows.h>
#include <commdlg.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

// Dear ImGui DirectX 11 example state.
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static bool                     g_SwapChainOccluded = false;
static UINT                     g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;
static ID3D11ShaderResourceView* g_logo_srv = nullptr;

static ImFont* g_font_body = nullptr;
static ImFont* g_font_mono = nullptr;

static const ImVec4 kBg = ImVec4(0.045f, 0.060f, 0.095f, 1.0f);
static const ImVec4 kPanel = ImVec4(0.075f, 0.095f, 0.145f, 1.0f);
static const ImVec4 kPanelRaised = ImVec4(0.105f, 0.135f, 0.195f, 1.0f);
static const ImVec4 kPanelDeep = ImVec4(0.030f, 0.042f, 0.075f, 1.0f);
static const ImVec4 kRule = ImVec4(0.160f, 0.205f, 0.300f, 1.0f);
static const ImVec4 kInk = ImVec4(0.900f, 0.930f, 0.985f, 1.0f);
static const ImVec4 kInkSoft = ImVec4(0.690f, 0.750f, 0.850f, 1.0f);
static const ImVec4 kMuted = ImVec4(0.410f, 0.490f, 0.620f, 1.0f);
static const ImVec4 kBlue = ImVec4(0.230f, 0.610f, 1.000f, 1.0f);
static const ImVec4 kBlueBright = ImVec4(0.440f, 0.760f, 1.000f, 1.0f);
static const ImVec4 kBlueInk = ImVec4(0.025f, 0.065f, 0.125f, 1.0f);
static const ImVec4 kError = ImVec4(1.000f, 0.380f, 0.340f, 1.0f);

static bool CreateDeviceD3D(HWND hWnd);
static void CleanupDeviceD3D();
static void CreateRenderTarget();
static void CleanupRenderTarget();
static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty())
        return {};
    const int length = ::WideCharToMultiByte(CP_UTF8, 0, value.c_str(), (int)value.size(), nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return {};
    std::string result(length, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, value.c_str(), (int)value.size(), result.data(), length, nullptr, nullptr);
    return result;
}

static std::filesystem::path ExecutableDirectory()
{
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return {};
    return std::filesystem::path(buffer).parent_path();
}

static std::filesystem::path FindRenaroLogo()
{
    const std::filesystem::path relative = std::filesystem::path(L"renaro") / L"assets" / L"logo" / L"white-transparent.png";
    const std::filesystem::path executable = ExecutableDirectory();
    const std::array<std::filesystem::path, 7> candidates = {
        relative,
        std::filesystem::path(L"..") / relative,
        std::filesystem::path(L"..") / L".." / relative,
        executable / relative,
        executable / L".." / relative,
        executable / L".." / L".." / relative,
        executable / L".." / L".." / L".." / relative
    };

    for (const std::filesystem::path& candidate : candidates)
    {
        std::error_code error;
        if (std::filesystem::exists(candidate, error))
            return std::filesystem::absolute(candidate, error);
    }
    return {};
}

static bool LoadTextureFromFile(const std::filesystem::path& path, ID3D11ShaderResourceView** out_srv, int* out_width, int* out_height)
{
    if (!out_srv || !g_pd3dDevice || path.empty())
        return false;
    *out_srv = nullptr;

    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    ComPtr<ID3D11Texture2D> texture;

    HRESULT hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr))
        return false;
    hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr))
        return false;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr))
        return false;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr))
        return false;
    hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr))
        return false;

    UINT width = 0;
    UINT height = 0;
    if (FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0)
        return false;

    std::vector<unsigned char> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
    if (FAILED(converter->CopyPixels(nullptr, width * 4u, static_cast<UINT>(pixels.size()), pixels.data())))
        return false;

    D3D11_TEXTURE2D_DESC texture_desc = {};
    texture_desc.Width = width;
    texture_desc.Height = height;
    texture_desc.MipLevels = 1;
    texture_desc.ArraySize = 1;
    texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Usage = D3D11_USAGE_DEFAULT;
    texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA texture_data = {};
    texture_data.pSysMem = pixels.data();
    texture_data.SysMemPitch = width * 4u;
    if (FAILED(g_pd3dDevice->CreateTexture2D(&texture_desc, &texture_data, &texture)))
        return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC view_desc = {};
    view_desc.Format = texture_desc.Format;
    view_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view_desc.Texture2D.MipLevels = 1;
    if (FAILED(g_pd3dDevice->CreateShaderResourceView(texture.Get(), &view_desc, out_srv)))
        return false;

    if (out_width)
        *out_width = static_cast<int>(width);
    if (out_height)
        *out_height = static_cast<int>(height);
    return true;
}

static bool LoadRenaroLogo()
{
    const std::filesystem::path logo_path = FindRenaroLogo();
    if (logo_path.empty())
        return false;
    int width = 0;
    int height = 0;
    return LoadTextureFromFile(logo_path, &g_logo_srv, &width, &height);
}

static bool ChooseModelFile(std::wstring& selected_path)
{
    wchar_t buffer[4096] = {};
    OPENFILENAMEW file_dialog = {};
    file_dialog.lStructSize = sizeof(file_dialog);
    file_dialog.hwndOwner = nullptr;
    file_dialog.lpstrFilter = L"Model files (*.gguf;*.bin;*.safetensors)\0*.gguf;*.bin;*.safetensors\0All files (*.*)\0*.*\0";
    file_dialog.lpstrFile = buffer;
    file_dialog.nMaxFile = static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0]));
    file_dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    file_dialog.lpstrTitle = L"Import local model";
    if (!::GetOpenFileNameW(&file_dialog))
        return false;
    selected_path = buffer;
    return true;
}

static std::string TrimCopy(const std::string& value)
{
    const size_t start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
        return {};
    const size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

static std::string LowerCopy(const std::string& value)
{
    std::string result = value;
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return result;
}

static std::string ModelSlug(const std::string& value)
{
    std::string result;
    result.reserve(value.size());
    bool pending_dash = false;
    for (unsigned char character : value)
    {
        if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9'))
        {
            if (pending_dash && !result.empty())
                result.push_back('-');
            result.push_back(static_cast<char>(std::tolower(character)));
            pending_dash = false;
        }
        else
        {
            pending_dash = true;
        }
    }
    return result;
}

static std::string BuildRunCommand(const std::string& model, int context, float temperature, int threads = 8, int gpu_layers = 0, int seed = 42, bool use_prompt_cache = false)
{
    std::ostringstream command;
    command << "renaro run --model " << ModelSlug(model) << " --ctx " << context << " --temp " << std::fixed << std::setprecision(2) << temperature;
    command << " --threads " << threads << " --gpu-layers " << gpu_layers << " --seed " << seed;
    if (use_prompt_cache)
        command << " --prompt-cache";
    return command.str();
}

enum class Pane
{
    Playground,
    Diagnostics,
    Trace,
    Settings
};

struct Message
{
    std::string role;
    std::string text;
    bool assistant = false;
    bool system = false;
    std::string annotation;
};

struct ModelEntry
{
    std::string name;
    std::string detail;
};

struct SessionEntry
{
    std::string name;
    std::string model;
    std::string when;
};

struct TraceEntry
{
    const char* event;
    const char* duration;
    const char* status;
};

struct AppState
{
    Pane pane = Pane::Playground;
    std::vector<ModelEntry> models;
    std::vector<SessionEntry> sessions;
    std::vector<Message> messages;
    std::vector<TraceEntry> trace;
    std::string current_model;
    std::string session_name;
    std::string imported_path;
    std::string toast;
    char prompt[4096] = {};
    char command_search[256] = {};
    float temperature = 0.70f;
    int context = 8192;
    int threads = 8;
    int gpu_layers = 0;
    int seed = 42;
    bool use_prompt_cache = false;
    bool telemetry_sampled = false;
    bool simulation_busy = false;
    float simulation_remaining = 0.0f;
    float toast_remaining = 0.0f;
    bool command_palette = false;
    bool command_focus_search = false;
    std::array<float, 12> bars = { 0.26f, 0.42f, 0.34f, 0.58f, 0.49f, 0.72f, 0.62f, 0.84f, 0.74f, 0.94f, 0.79f, 0.68f };

    AppState()
    {
        models = {
            { "Qwen3 8B", "8.2B  /  GGUF  /  ready" },
            { "Llama 3.1 8B", "8.0B  /  GGUF  /  staged" },
            { "Mistral Nemo", "12.2B  /  GGUF  /  staged" },
            { "Gemma 3 12B", "12.2B  /  GGUF  /  staged" }
        };
        sessions = {
            { "Untitled study", "Qwen3 8B", "just now" },
            { "Repetition sweep", "Llama 3.1 8B", "yesterday" },
            { "Context drift", "Mistral Nemo", "last week" }
        };
        current_model = models.front().name;
        session_name = sessions.front().name;
        messages = {
            { "System", "You are a local research assistant. State assumptions, show uncertainty, and keep the answer close to the evidence.", false, true, {} },
            { "You", "Give me three hypotheses for why this prompt might increase repetition.", false, false, {} },
            { "Qwen3 8B", "Start with the context window, sampling temperature, and the stop sequence. Hold the model constant, change one variable at a time, and record the decode trace.", true, false, "sample stream  /  llama.cpp adapter pending" }
        };
        trace = {
            { "load context", "-", "pending" },
            { "prefill", "-", "pending" },
            { "decode loop", "-", "pending" },
            { "stream tokens", "-", "pending" }
        };
    }
};

static std::string CurrentRunCommand(const AppState& app)
{
    return BuildRunCommand(app.current_model, app.context, app.temperature, app.threads, app.gpu_layers, app.seed, app.use_prompt_cache);
}

static void ShowToast(AppState& app, const std::string& message)
{
    app.toast = message;
    app.toast_remaining = 3.0f;
}

static void AddMessage(AppState& app, const std::string& role, const std::string& text, bool assistant, bool system, const std::string& annotation = {})
{
    app.messages.push_back({ role, text, assistant, system, annotation });
}

static void ResetTelemetry(AppState& app)
{
    app.telemetry_sampled = false;
    app.bars = { 0.26f, 0.42f, 0.34f, 0.58f, 0.49f, 0.72f, 0.62f, 0.84f, 0.74f, 0.94f, 0.79f, 0.68f };
    for (TraceEntry& entry : app.trace)
    {
        entry.duration = "-";
        entry.status = "pending";
    }
}

static void SampleTelemetry(AppState& app)
{
    app.telemetry_sampled = true;
    app.bars = { 0.36f, 0.48f, 0.43f, 0.68f, 0.57f, 0.78f, 0.67f, 0.90f, 0.80f, 0.96f, 0.86f, 0.76f };
    app.trace = {
        { "load context", "0.14 s", "sampled" },
        { "prefill", "0.31 s", "sampled" },
        { "decode loop", "0.82 s", "sampled" },
        { "stream tokens", "1.42 s", "sampled" }
    };
}

static void SelectModel(AppState& app, size_t index, bool announce)
{
    if (index >= app.models.size())
        return;
    app.current_model = app.models[index].name;
    ResetTelemetry(app);
    for (SessionEntry& session : app.sessions)
    {
        if (session.name == app.session_name)
        {
            session.model = app.current_model;
            session.when = "just now";
        }
    }
    if (announce)
        ShowToast(app, app.current_model + " selected for this session");
}

static void StartNewSession(AppState& app, bool announce)
{
    app.simulation_busy = false;
    app.simulation_remaining = 0.0f;
    app.session_name = "Untitled study";
    app.messages.clear();
    AddMessage(app, "System", "You are a local research assistant. State assumptions, show uncertainty, and keep the answer close to the evidence.", false, true);
    app.sessions.front() = { "Untitled study", app.current_model, "just now" };
    ResetTelemetry(app);
    app.pane = Pane::Playground;
    if (announce)
        ShowToast(app, "New session ready");
}

static void ActivateSession(AppState& app, size_t index)
{
    if (index >= app.sessions.size())
        return;
    const SessionEntry& session = app.sessions[index];
    app.session_name = session.name;
    for (size_t model_index = 0; model_index < app.models.size(); ++model_index)
    {
        if (app.models[model_index].name == session.model)
        {
            app.current_model = session.model;
            break;
        }
    }
    ResetTelemetry(app);
    app.pane = Pane::Playground;
    ShowToast(app, "Opened " + app.session_name);
}

static void StartSimulation(AppState& app, const std::string& submitted_prompt)
{
    if (app.simulation_busy)
        return;
    const std::string prompt = TrimCopy(submitted_prompt);
    const bool has_prompt = !prompt.empty();
    if (has_prompt)
        AddMessage(app, "You", prompt, false, false);
    app.pane = Pane::Playground;
    app.simulation_busy = true;
    app.simulation_remaining = 0.78f;
    app.telemetry_sampled = false;
    if (has_prompt)
        std::fill_n(app.prompt, sizeof(app.prompt), '\0');
}

static void AdvanceSimulation(AppState& app, float delta_time)
{
    if (app.simulation_busy)
    {
        app.simulation_remaining -= delta_time;
        if (app.simulation_remaining <= 0.0f)
        {
            AddMessage(app, app.current_model, "Staged response for the current prompt. Connect the llama.cpp adapter here to replace this sample with a live stream.", true, false, "staged sample  /  llama.cpp adapter pending");
            app.simulation_busy = false;
            SampleTelemetry(app);
            ShowToast(app, "Staged telemetry sample ready");
        }
    }
    if (app.toast_remaining > 0.0f)
        app.toast_remaining = std::max(0.0f, app.toast_remaining - delta_time);
}

static void ApplyRenaroStyle()
{
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(16.0f, 16.0f);
    style.FramePadding = ImVec2(10.0f, 7.0f);
    style.CellPadding = ImVec2(8.0f, 6.0f);
    style.ItemSpacing = ImVec2(8.0f, 8.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.TouchExtraPadding = ImVec2(2.0f, 2.0f);
    style.ScrollbarSize = 12.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.WindowRounding = 0.0f;
    style.ChildRounding = 7.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 7.0f;
    style.ScrollbarRounding = 5.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 4.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = kInk;
    colors[ImGuiCol_TextDisabled] = kMuted;
    colors[ImGuiCol_WindowBg] = kBg;
    colors[ImGuiCol_ChildBg] = kPanel;
    colors[ImGuiCol_PopupBg] = kPanelRaised;
    colors[ImGuiCol_Border] = kRule;
    colors[ImGuiCol_FrameBg] = kPanelDeep;
    colors[ImGuiCol_FrameBgHovered] = kPanelRaised;
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.135f, 0.185f, 0.270f, 1.0f);
    colors[ImGuiCol_Button] = kPanelRaised;
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.160f, 0.230f, 0.350f, 1.0f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.190f, 0.290f, 0.440f, 1.0f);
    colors[ImGuiCol_Header] = ImVec4(0.130f, 0.190f, 0.300f, 1.0f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.170f, 0.270f, 0.420f, 1.0f);
    colors[ImGuiCol_HeaderActive] = kBlue;
    colors[ImGuiCol_Separator] = kRule;
    colors[ImGuiCol_SeparatorHovered] = kBlue;
    colors[ImGuiCol_SeparatorActive] = kBlueBright;
    colors[ImGuiCol_CheckMark] = kBlueBright;
    colors[ImGuiCol_SliderGrab] = kBlue;
    colors[ImGuiCol_SliderGrabActive] = kBlueBright;
    colors[ImGuiCol_Tab] = kPanel;
    colors[ImGuiCol_TabHovered] = kPanelRaised;
    colors[ImGuiCol_TabActive] = ImVec4(0.120f, 0.205f, 0.330f, 1.0f);
    colors[ImGuiCol_TableHeaderBg] = kPanelRaised;
    colors[ImGuiCol_TableBorderStrong] = kRule;
    colors[ImGuiCol_TableBorderLight] = ImVec4(0.105f, 0.145f, 0.220f, 1.0f);
    colors[ImGuiCol_NavHighlight] = kBlue;
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.015f, 0.025f, 0.055f, 0.78f);
}

static void DrawPulseDot(const ImVec2& center, float radius, float time, bool active)
{
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const float pulse = active ? (0.72f + 0.28f * std::sin(time * 4.0f)) : 0.65f;
    draw_list->AddCircleFilled(center, radius * 2.2f, ImGui::GetColorU32(ImVec4(kBlue.x, kBlue.y, kBlue.z, 0.10f * pulse)));
    draw_list->AddCircleFilled(center, radius, ImGui::GetColorU32(ImVec4(kBlueBright.x, kBlueBright.y, kBlueBright.z, pulse)));
}

static bool PrimaryButton(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f))
{
    ImGui::PushStyleColor(ImGuiCol_Button, kBlue);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kBlueBright);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.160f, 0.440f, 0.780f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, kBlueInk);
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return clicked;
}

static bool QuietButton(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f))
{
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kPanelRaised);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.150f, 0.220f, 0.340f, 1.0f));
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return clicked;
}

static void PanelHeading(const char* label, const char* title)
{
    ImGui::TextColored(kMuted, "%s", label);
    ImGui::PushFont(g_font_body);
    ImGui::TextColored(kInk, "%s", title);
    ImGui::PopFont();
}

static void DrawSidebar(AppState& app, float time)
{
    ImGui::BeginGroup();
    if (g_logo_srv)
        ImGui::Image((ImTextureID)(intptr_t)g_logo_srv, ImVec2(30.0f, 30.0f));
    else
    {
        ImGui::Dummy(ImVec2(30.0f, 30.0f));
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        const ImVec2 min = ImGui::GetItemRectMin();
        draw_list->AddRectFilled(min, min + ImVec2(30.0f, 30.0f), ImGui::GetColorU32(kBlue), 5.0f);
        draw_list->AddText(min + ImVec2(9.0f, 5.0f), ImGui::GetColorU32(kBlueInk), "R");
    }
    ImGui::SameLine(0.0f, 9.0f);
    ImGui::BeginGroup();
    ImGui::TextColored(kInk, "RENARO");
    ImGui::TextColored(kBlueBright, "PLAYGROUND");
    ImGui::EndGroup();
    ImGui::EndGroup();

    ImGui::Dummy(ImVec2(0.0f, 22.0f));
    ImGui::TextColored(kMuted, "WORKSPACE");

    struct NavItem { Pane pane; const char* mark; const char* label; const char* key; };
    const NavItem nav_items[] = {
        { Pane::Playground, ">", "Playground", "1" },
        { Pane::Diagnostics, "#", "Diagnostics", "2" },
        { Pane::Trace, "=", "Trace log", "3" },
        { Pane::Settings, "/", "Settings", "4" }
    };

    for (const NavItem& item : nav_items)
    {
        const bool active = app.pane == item.pane;
        ImGui::PushID(item.label);
        ImGui::PushStyleColor(ImGuiCol_Button, active ? ImVec4(0.120f, 0.230f, 0.380f, 1.0f) : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.120f, 0.185f, 0.290f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, active ? kBlueBright : kInkSoft);
        const float shortcut_width = 30.0f;
        if (ImGui::Button((std::string(item.mark) + "  " + item.label).c_str(), ImVec2(std::max(80.0f, ImGui::GetContentRegionAvail().x - shortcut_width), 34.0f)))
            app.pane = item.pane;
        ImGui::PopStyleColor(3);
        ImGui::SameLine(0.0f, 3.0f);
        ImGui::TextColored(active ? kBlue : kMuted, "[%s]", item.key);
        ImGui::PopID();
    }

    ImGui::Dummy(ImVec2(0.0f, 18.0f));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, 12.0f));
    ImGui::TextColored(kMuted, "RECENT SESSIONS");
    for (size_t index = 0; index < app.sessions.size(); ++index)
    {
        const SessionEntry& session = app.sessions[index];
        const bool active = session.name == app.session_name;
        ImGui::PushID(static_cast<int>(index));
        ImGui::PushStyleColor(ImGuiCol_Header, active ? ImVec4(0.105f, 0.180f, 0.290f, 1.0f) : ImVec4(0, 0, 0, 0));
        if (ImGui::Selectable(session.name.c_str(), active, ImGuiSelectableFlags_DontClosePopups, ImVec2(-1.0f, 30.0f)))
            ActivateSession(app, index);
        ImGui::PopStyleColor();
        ImGui::Indent(17.0f);
        ImGui::TextColored(kMuted, "%s  /  %s", session.model.c_str(), session.when.c_str());
        ImGui::Unindent(17.0f);
        ImGui::PopID();
    }

    ImGui::Dummy(ImVec2(0.0f, 12.0f));
    if (QuietButton("+  New session", ImVec2(-1.0f, 32.0f)))
        StartNewSession(app, true);

    const float footer_height = 68.0f;
    const float footer_y = std::max(ImGui::GetCursorPosY(), ImGui::GetWindowHeight() - footer_height);
    ImGui::SetCursorPosY(footer_y);
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    ImGui::BeginGroup();
    DrawPulseDot(ImGui::GetCursorScreenPos() + ImVec2(5.0f, 8.0f), 3.0f, time, false);
    ImGui::Dummy(ImVec2(14.0f, 16.0f));
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::BeginGroup();
    ImGui::TextColored(kInkSoft, "Local engine");
    ImGui::TextColored(kMuted, "adapter not connected");
    ImGui::EndGroup();
    ImGui::EndGroup();
    ImGui::TextColored(kMuted, "v0.1 playground");
}

static void DrawTopBar(AppState& app, float time)
{
    ImGui::BeginChild("##TopBar", ImVec2(0.0f, 74.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    ImGui::TextColored(kMuted, "LOCAL INFERENCE  /  WORKSPACE");
    ImGui::TextColored(kInk, "Playground");

    const float action_width = 332.0f;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - action_width));
    DrawPulseDot(ImGui::GetCursorScreenPos() + ImVec2(5.0f, 10.0f), 3.0f, time, app.simulation_busy);
    ImGui::Dummy(ImVec2(14.0f, 18.0f));
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::TextColored(app.simulation_busy ? kBlueBright : kMuted, "%s", app.simulation_busy ? "SIMULATING" : (app.telemetry_sampled ? "DEMO SAMPLE" : "READY FOR A RUN"));
    ImGui::SameLine(0.0f, 16.0f);
    if (QuietButton("Find  Ctrl+K", ImVec2(92.0f, 30.0f)))
    {
        app.command_palette = true;
        app.command_focus_search = true;
    }
    ImGui::SameLine(0.0f, 6.0f);
    if (PrimaryButton("New session", ImVec2(112.0f, 30.0f)))
        StartNewSession(app, true);
    ImGui::EndChild();
}

static void DrawModelShelf(AppState& app)
{
    PanelHeading("MODELS", "Library");
    ImGui::Dummy(ImVec2(0.0f, 10.0f));

    for (size_t index = 0; index < app.models.size(); ++index)
    {
        const ModelEntry& model = app.models[index];
        const bool active = model.name == app.current_model;
        ImGui::PushID(static_cast<int>(index));
        ImGui::PushStyleColor(ImGuiCol_Button, active ? ImVec4(0.120f, 0.230f, 0.380f, 1.0f) : kPanelDeep);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.145f, 0.250f, 0.390f, 1.0f));
        const float status_width = 52.0f;
        if (ImGui::Button((std::string(1, model.name.front()) + "  " + model.name).c_str(), ImVec2(std::max(90.0f, ImGui::GetContentRegionAvail().x - status_width), 34.0f)))
            SelectModel(app, index, true);
        ImGui::PopStyleColor(2);
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::TextColored(active ? kBlueBright : kMuted, "%s", active ? "active" : "staged");
        ImGui::TextColored(kMuted, "    %s", model.detail.c_str());
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        ImGui::PopID();
    }

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    if (QuietButton("+  Import local model", ImVec2(-1.0f, 36.0f)))
    {
        std::wstring selected_path;
        if (ChooseModelFile(selected_path))
        {
            const std::filesystem::path path(selected_path);
            const std::string name = WideToUtf8(path.stem().wstring());
            app.imported_path = WideToUtf8(path.wstring());
            if (!name.empty())
            {
                auto existing = std::find_if(app.models.begin(), app.models.end(), [&name](const ModelEntry& item) { return item.name == name; });
                if (existing == app.models.end())
                    app.models.push_back({ name, "local file  /  staged import" });
                const size_t index = static_cast<size_t>(std::distance(app.models.begin(), std::find_if(app.models.begin(), app.models.end(), [&name](const ModelEntry& item) { return item.name == name; })));
                SelectModel(app, index, false);
            }
            ShowToast(app, "Selected " + name + " for the next import flow");
        }
    }
    if (!app.imported_path.empty())
    {
        ImGui::TextColored(kBlueBright, "Ready to import");
        ImGui::TextWrapped("%s", app.imported_path.c_str());
    }
    ImGui::Dummy(ImVec2(0.0f, 12.0f));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    ImGui::TextColored(kMuted, "MODEL DIRECTORY");
    ImGui::TextColored(kInkSoft, "-  adapter path pending");
    ImGui::TextWrapped("Choose a directory when the local engine adapter is connected.");
}

static void DrawMessage(const Message& message)
{
    const ImVec4 role_color = message.system ? kBlueBright : (message.assistant ? kBlue : kInk);
    if (message.system)
        ImGui::SeparatorText("SYSTEM");
    else
        ImGui::TextColored(role_color, "%s", message.role.c_str());
    ImGui::SameLine(0.0f, 10.0f);
    ImGui::TextColored(kMuted, "%s", message.system ? "ready" : "now");
    ImGui::TextWrapped("%s", message.text.c_str());
    if (!message.annotation.empty())
    {
        ImGui::TextColored(kMuted, "----  %s", message.annotation.c_str());
    }
    ImGui::Dummy(ImVec2(0.0f, 7.0f));
}

static bool WorkspaceTab(const char* label, bool active)
{
    ImGui::PushStyleColor(ImGuiCol_Button, active ? ImVec4(0.110f, 0.200f, 0.330f, 1.0f) : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.130f, 0.230f, 0.370f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, active ? kInk : kMuted);
    const bool clicked = ImGui::Button(label, ImVec2(0.0f, 29.0f));
    ImGui::PopStyleColor(3);
    return clicked;
}

static void DrawConversation(AppState& app)
{
    const float prompt_height = 137.0f;
    ImGui::BeginChild("##ConversationScroll", ImVec2(0.0f, -prompt_height), ImGuiChildFlags_None, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    for (const Message& message : app.messages)
        DrawMessage(message);
    if (app.simulation_busy)
    {
        ImGui::TextColored(kBlueBright, "%s", app.current_model.c_str());
        ImGui::SameLine(0.0f, 10.0f);
        ImGui::TextColored(kMuted, "sampling...");
        ImGui::TextColored(kBlue, "|  |  |  |  |  |  |  |  |  |  |  |");
    }
    ImGui::EndChild();

    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_FrameBg, kPanelDeep);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.055f, 0.080f, 0.135f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.070f, 0.105f, 0.180f, 1.0f));
    ImGui::InputTextMultiline("##Prompt", app.prompt, sizeof(app.prompt), ImVec2(-1.0f, 70.0f), ImGuiInputTextFlags_AllowTabInput);
    const bool submit = ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Enter) && !ImGui::GetIO().KeyShift;
    ImGui::PopStyleColor(3);
    ImGui::TextColored(kMuted, "Enter sends  /  Shift+Enter adds a line");
    ImGui::SameLine(ImGui::GetWindowWidth() - 135.0f);
    ImGui::BeginDisabled(app.simulation_busy);
    if (PrimaryButton("Send prompt", ImVec2(118.0f, 30.0f)) || submit)
        StartSimulation(app, app.prompt);
    ImGui::EndDisabled();
}

static void DrawDiagnostics(const AppState& app)
{
    ImGui::TextColored(kBlueBright, "DIAGNOSTICS");
    ImGui::TextColored(kInk, "Run evidence");
    ImGui::TextWrapped("Tokenization, sampling, prompt-cache hits, and memory pressure stay beside the conversation so each run can be compared without leaving the workbench.");
    ImGui::Dummy(ImVec2(0.0f, 14.0f));
    if (!app.telemetry_sampled)
    {
        ImGui::TextColored(kMuted, "Diagnostics begin with the first staged or real run.");
        return;
    }
    if (ImGui::BeginTable("##DiagnosticTable", 3, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("signal");
        ImGui::TableSetupColumn("value");
        ImGui::TableSetupColumn("source");
        ImGui::TableHeadersRow();
        const char* rows[][3] = {
            { "prompt tokens / sec", "42.6", "staged demo" },
            { "decode tokens / sec", "18.4", "staged demo" },
            { "time to first token", "0.82 s", "staged demo" },
            { "memory footprint", "6.1 GB", "staged demo" },
            { "prompt cache", "not connected", "adapter pending" }
        };
        for (const auto& row : rows)
        {
            ImGui::TableNextRow();
            for (int column = 0; column < 3; ++column)
            {
                ImGui::TableSetColumnIndex(column);
                ImGui::TextColored(column == 1 ? kInk : kMuted, "%s", row[column]);
            }
        }
        ImGui::EndTable();
    }
}

static void DrawTrace(const AppState& app)
{
    ImGui::TextColored(kBlueBright, "TRACE");
    ImGui::TextColored(kInk, "Run sequence");
    ImGui::Dummy(ImVec2(0.0f, 12.0f));
    if (ImGui::BeginTable("##TraceTable", 3, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("event");
        ImGui::TableSetupColumn("duration");
        ImGui::TableSetupColumn("status");
        ImGui::TableHeadersRow();
        for (const TraceEntry& entry : app.trace)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(kInkSoft, "%s", entry.event);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(kInk, "%s", entry.duration);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextColored(app.telemetry_sampled ? kBlueBright : kMuted, "%s", entry.status);
        }
        ImGui::EndTable();
    }
}

static void DrawSettings(AppState& app)
{
    ImGui::TextColored(kBlueBright, "SETTINGS");
    ImGui::TextColored(kInk, "Engine controls");
    ImGui::TextWrapped("These controls are wired to the run command now. The llama.cpp adapter can consume the same values when the backend is connected.");
    ImGui::Dummy(ImVec2(0.0f, 14.0f));
    ImGui::SliderInt("Threads", &app.threads, 1, 32);
    ImGui::SliderInt("GPU layers", &app.gpu_layers, 0, 64);
    ImGui::SliderInt("Seed", &app.seed, 0, 9999);
    ImGui::Checkbox("Use prompt cache", &app.use_prompt_cache);
    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    ImGui::TextColored(kMuted, "Current command");
    ImGui::PushFont(g_font_mono);
    ImGui::TextWrapped("%s", CurrentRunCommand(app).c_str());
    ImGui::PopFont();
}

static void DrawWorkspace(AppState& app)
{
    PanelHeading("ACTIVE SESSION", app.current_model.c_str());
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::TextColored(kMuted, "/  %s", app.session_name.c_str());
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - 235.0f));
    if (QuietButton("Copy run command", ImVec2(112.0f, 30.0f)))
    {
        const std::string command = CurrentRunCommand(app);
        ImGui::SetClipboardText(command.c_str());
        ShowToast(app, "Run command copied");
    }
    ImGui::SameLine(0.0f, 6.0f);
    ImGui::BeginDisabled(app.simulation_busy);
    if (PrimaryButton("Simulate", ImVec2(94.0f, 30.0f)))
        StartSimulation(app, {});
    ImGui::EndDisabled();

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    if (app.pane != Pane::Settings)
    {
        if (WorkspaceTab("Conversation", app.pane == Pane::Playground))
            app.pane = Pane::Playground;
        ImGui::SameLine(0.0f, 4.0f);
        if (WorkspaceTab("Diagnostics", app.pane == Pane::Diagnostics))
            app.pane = Pane::Diagnostics;
        ImGui::SameLine(0.0f, 4.0f);
        if (WorkspaceTab("Trace", app.pane == Pane::Trace))
            app.pane = Pane::Trace;
    }
    ImGui::Separator();

    if (app.pane == Pane::Playground)
        DrawConversation(app);
    else if (app.pane == Pane::Diagnostics)
        DrawDiagnostics(app);
    else if (app.pane == Pane::Trace)
        DrawTrace(app);
    else
        DrawSettings(app);
}

static void DrawMetric(const char* label, const char* value, const char* note)
{
    ImGui::TextColored(kMuted, "%s", label);
    ImGui::TextColored(kInk, "%s", value);
    ImGui::TextColored(kMuted, "%s", note);
}

static void DrawTelemetryPlot(const AppState& app)
{
    ImGui::TextColored(kInkSoft, "Token flow");
    ImGui::SameLine(ImGui::GetWindowWidth() - 92.0f);
    ImGui::TextColored(kMuted, "%s", app.telemetry_sampled ? "sample only" : "awaiting run");
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    const ImVec2 plot_size(ImGui::GetContentRegionAvail().x, 102.0f);
    ImGui::InvisibleButton("##TokenPlot", plot_size);
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(min, max, ImGui::GetColorU32(kPanelDeep), 4.0f);
    const float gap = 4.0f;
    const float bar_width = (plot_size.x - gap * 13.0f) / 12.0f;
    for (size_t index = 0; index < app.bars.size(); ++index)
    {
        const float height = (plot_size.y - 18.0f) * app.bars[index];
        const float x = min.x + gap + static_cast<float>(index) * (bar_width + gap);
        const float y = max.y - 8.0f - height;
        const ImU32 color = ImGui::GetColorU32(ImVec4(kBlue.x, kBlue.y, kBlue.z, index % 3 == 2 ? 0.46f : 0.82f));
        draw_list->AddRectFilled(ImVec2(x, y), ImVec2(x + bar_width, max.y - 8.0f), color, 1.0f);
    }
}

static void DrawTelemetry(AppState& app)
{
    PanelHeading("RUN TELEMETRY", "Signal");
    ImGui::SameLine(ImGui::GetWindowWidth() - 88.0f);
    ImGui::TextColored(app.telemetry_sampled ? kBlueBright : kMuted, "%s", app.simulation_busy ? "sampling" : (app.telemetry_sampled ? "sampled" : "waiting"));
    ImGui::Dummy(ImVec2(0.0f, 10.0f));

    if (ImGui::BeginTable("##MetricGrid", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        DrawMetric("Prompt tok/s", app.telemetry_sampled ? "42.6" : "-", app.telemetry_sampled ? "staged demo" : "run pending");
        ImGui::TableSetColumnIndex(1);
        DrawMetric("Decode tok/s", app.telemetry_sampled ? "18.4" : "-", app.telemetry_sampled ? "staged demo" : "run pending");
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        DrawMetric("First token", app.telemetry_sampled ? "0.82 s" : "-", app.telemetry_sampled ? "staged demo" : "not measured");
        ImGui::TableSetColumnIndex(1);
        DrawMetric("Memory", app.telemetry_sampled ? "6.1 GB" : "-", app.telemetry_sampled ? "staged demo" : "adapter pending");
        ImGui::EndTable();
    }

    ImGui::Dummy(ImVec2(0.0f, 12.0f));
    DrawTelemetryPlot(app);
    ImGui::Dummy(ImVec2(0.0f, 12.0f));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    ImGui::TextColored(kInkSoft, "Temperature");
    ImGui::SameLine(ImGui::GetWindowWidth() - 62.0f);
    ImGui::TextColored(kBlueBright, "%.2f", app.temperature);
    ImGui::SliderFloat("##Temperature", &app.temperature, 0.0f, 1.5f, "");

    static const char* context_items[] = { "4,096 tokens", "8,192 tokens", "16,384 tokens", "32,768 tokens" };
    int context_index = app.context == 4096 ? 0 : (app.context == 16384 ? 2 : (app.context == 32768 ? 3 : 1));
    if (ImGui::Combo("Context window", &context_index, context_items, IM_ARRAYSIZE(context_items)))
    {
        const int context_values[] = { 4096, 8192, 16384, 32768 };
        app.context = context_values[context_index];
    }

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    ImGui::TextColored(kMuted, "RUN COMMAND");
    ImGui::PushFont(g_font_mono);
    ImGui::TextWrapped("%s", CurrentRunCommand(app).c_str());
    ImGui::PopFont();
    if (QuietButton("Copy command", ImVec2(112.0f, 29.0f)))
    {
        const std::string command = CurrentRunCommand(app);
        ImGui::SetClipboardText(command.c_str());
        ShowToast(app, "Run command copied");
    }
}

static void DrawWorkbench(AppState& app)
{
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float gutter = 10.0f;
    const bool wide = available.x >= 1020.0f;

    if (wide)
    {
        const float model_width = 226.0f;
        const float telemetry_width = 284.0f;
        const float workspace_width = available.x - model_width - telemetry_width - gutter * 2.0f;

        ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
        ImGui::BeginChild("##ModelShelf", ImVec2(model_width, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        DrawModelShelf(app);
        ImGui::EndChild();
        ImGui::SameLine(0.0f, gutter);
        ImGui::BeginChild("##Workspace", ImVec2(workspace_width, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        DrawWorkspace(app);
        ImGui::EndChild();
        ImGui::SameLine(0.0f, gutter);
        ImGui::BeginChild("##Telemetry", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        DrawTelemetry(app);
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    else
    {
        const float auxiliary_height = 235.0f;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
        ImGui::BeginChild("##Workspace", ImVec2(0.0f, -auxiliary_height), ImGuiChildFlags_Borders, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        DrawWorkspace(app);
        ImGui::EndChild();
        ImGui::BeginChild("##Auxiliary", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        const float half = (ImGui::GetContentRegionAvail().x - gutter) * 0.5f;
        ImGui::BeginChild("##CompactModels", ImVec2(half, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        DrawModelShelf(app);
        ImGui::EndChild();
        ImGui::SameLine(0.0f, gutter);
        ImGui::BeginChild("##CompactTelemetry", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        DrawTelemetry(app);
        ImGui::EndChild();
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
}

static void RunCommandAction(AppState& app, const char* action)
{
    app.command_palette = false;
    app.command_search[0] = '\0';
    if (std::strcmp(action, "focus") == 0)
        app.pane = Pane::Playground;
    else if (std::strcmp(action, "diagnostics") == 0)
        app.pane = Pane::Diagnostics;
    else if (std::strcmp(action, "trace") == 0)
        app.pane = Pane::Trace;
    else if (std::strcmp(action, "simulate") == 0)
        StartSimulation(app, {});
    else if (std::strcmp(action, "copy") == 0)
    {
        const std::string command = CurrentRunCommand(app);
        ImGui::SetClipboardText(command.c_str());
        ShowToast(app, "Run command copied");
    }
    else if (std::strcmp(action, "new-session") == 0)
        StartNewSession(app, true);
}

static void DrawCommandPalette(AppState& app)
{
    if (!app.command_palette)
        return;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::GetForegroundDrawList()->AddRectFilled(viewport->WorkPos, viewport->WorkPos + viewport->WorkSize, ImGui::GetColorU32(ImVec4(0.01f, 0.02f, 0.05f, 0.48f)));
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.22f));
    ImGui::SetNextWindowSize(ImVec2(470.0f, 0.0f), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kPanelRaised);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 18.0f));
    ImGui::Begin("##RenaroCommandPalette", &app.command_palette, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextColored(kMuted, "RENARO COMMAND PALETTE");
    ImGui::TextColored(kInk, "Find an action");
    ImGui::Dummy(ImVec2(0.0f, 7.0f));
    if (app.command_focus_search)
    {
        ImGui::SetKeyboardFocusHere();
        app.command_focus_search = false;
    }
    ImGui::InputTextWithHint("##CommandSearch", "Search actions...", app.command_search, sizeof(app.command_search));
    ImGui::Dummy(ImVec2(0.0f, 9.0f));

    struct Action { const char* label; const char* detail; const char* id; };
    const Action actions[] = {
        { "Focus prompt", "Jump to the conversation input", "focus" },
        { "Open diagnostics", "Inspect the run surface", "diagnostics" },
        { "Open trace log", "Inspect the staged event sequence", "trace" },
        { "Simulate response", "Fill the UI with staged telemetry", "simulate" },
        { "Copy run command", "Take the local command with you", "copy" },
        { "New session", "Clear the current run state", "new-session" }
    };
    const std::string query = LowerCopy(app.command_search);
    for (const Action& action : actions)
    {
        if (!query.empty() && LowerCopy(action.label).find(query) == std::string::npos && LowerCopy(action.detail).find(query) == std::string::npos)
            continue;
        ImGui::PushID(action.id);
        if (ImGui::Selectable(action.label, false, ImGuiSelectableFlags_DontClosePopups, ImVec2(-1.0f, 32.0f)))
            RunCommandAction(app, action.id);
        ImGui::SameLine();
        ImGui::TextColored(kMuted, "%s", action.detail);
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    ImGui::TextColored(kMuted, "Ctrl+K open  /  Esc close  /  1-4 switch views");
    if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        app.command_palette = false;
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

static void DrawToast(const AppState& app)
{
    if (app.toast_remaining <= 0.0f || app.toast.empty())
        return;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos + ImVec2(viewport->WorkSize.x - 24.0f, viewport->WorkSize.y - 24.0f), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kPanelRaised);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 10.0f));
    ImGui::Begin("##RenaroToast", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::TextColored(kBlueBright, "%s", app.toast.c_str());
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

static void DrawRenaroApp(AppState& app)
{
    ImGuiIO& io = ImGui::GetIO();
    AdvanceSimulation(app, io.DeltaTime);

    if (!io.WantTextInput)
    {
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_K))
        {
            app.command_palette = true;
            app.command_focus_search = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_1)) app.pane = Pane::Playground;
        if (ImGui::IsKeyPressed(ImGuiKey_2)) app.pane = Pane::Diagnostics;
        if (ImGui::IsKeyPressed(ImGuiKey_3)) app.pane = Pane::Trace;
        if (ImGui::IsKeyPressed(ImGuiKey_4)) app.pane = Pane::Settings;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##RenaroRoot", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar();

    const ImVec2 root_min = ImGui::GetWindowPos();
    const ImVec2 root_max = root_min + ImGui::GetWindowSize();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(root_min, root_max, ImGui::GetColorU32(kBg));
    for (float x = root_min.x; x < root_max.x; x += 32.0f)
        draw_list->AddLine(ImVec2(x, root_min.y), ImVec2(x, root_max.y), ImGui::GetColorU32(ImVec4(0.12f, 0.20f, 0.32f, 0.08f)));
    for (float y = root_min.y; y < root_max.y; y += 32.0f)
        draw_list->AddLine(ImVec2(root_min.x, y), ImVec2(root_max.x, y), ImGui::GetColorU32(ImVec4(0.12f, 0.20f, 0.32f, 0.08f)));

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.055f, 0.075f, 0.120f, 0.98f));
    ImGui::BeginChild("##Sidebar", ImVec2(232.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    DrawSidebar(app, static_cast<float>(ImGui::GetTime()));
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::SameLine(0.0f, 0.0f);
    ImGui::BeginChild("##Main", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    DrawTopBar(app, static_cast<float>(ImGui::GetTime()));
    ImGui::Separator();
    DrawWorkbench(app);
    ImGui::EndChild();

    ImGui::End();
    DrawCommandPalette(app);
    DrawToast(app);
}

int main(int, char**)
{
    ImGui_ImplWin32_EnableDpiAwareness();
    const float main_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(::MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXW window_class = { sizeof(window_class), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"Renaro Model Playground", nullptr };
    ::RegisterClassExW(&window_class);
    HWND hwnd = ::CreateWindowW(window_class.lpszClassName, L"Renaro Model Playground", WS_OVERLAPPEDWINDOW, 100, 100, (int)(1500 * main_scale), (int)(900 * main_scale), nullptr, nullptr, window_class.hInstance, nullptr);
    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ApplyRenaroStyle();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;

    if (::GetFileAttributesA("C:\\Windows\\Fonts\\segoeui.ttf") != INVALID_FILE_ATTRIBUTES)
        g_font_body = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 16.0f);
    if (::GetFileAttributesA("C:\\Windows\\Fonts\\consola.ttf") != INVALID_FILE_ATTRIBUTES)
        g_font_mono = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", 13.0f);
    if (!g_font_body)
        g_font_body = io.Fonts->AddFontDefault();
    if (!g_font_mono)
        g_font_mono = g_font_body;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
    LoadRenaroLogo();

    AppState app;
    bool done = false;
    while (!done)
    {
        MSG message;
        while (::PeekMessage(&message, nullptr, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&message);
            ::DispatchMessage(&message);
            if (message.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;
        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
        {
            ::Sleep(10);
            continue;
        }
        g_SwapChainOccluded = false;
        if (g_ResizeWidth != 0 && g_ResizeHeight != 0)
        {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawRenaroApp(app);
        ImGui::Render();

        const float clear_color[4] = { kBg.x, kBg.y, kBg.z, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        const HRESULT present_result = g_pSwapChain->Present(1, 0);
        g_SwapChainOccluded = (present_result == DXGI_STATUS_OCCLUDED);
    }

    if (g_logo_srv)
    {
        g_logo_srv->Release();
        g_logo_srv = nullptr;
    }
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    ::CoUninitialize();
    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
    return 0;
}

static bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC swap_chain_desc = {};
    swap_chain_desc.BufferCount = 2;
    swap_chain_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_chain_desc.BufferDesc.RefreshRate.Numerator = 60;
    swap_chain_desc.BufferDesc.RefreshRate.Denominator = 1;
    swap_chain_desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc.OutputWindow = hWnd;
    swap_chain_desc.SampleDesc.Count = 1;
    swap_chain_desc.Windowed = TRUE;
    swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT device_flags = 0;
    D3D_FEATURE_LEVEL feature_level;
    const D3D_FEATURE_LEVEL feature_levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, device_flags, feature_levels, 2, D3D11_SDK_VERSION, &swap_chain_desc, &g_pSwapChain, &g_pd3dDevice, &feature_level, &g_pd3dDeviceContext);
    if (result == DXGI_ERROR_UNSUPPORTED)
        result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, device_flags, feature_levels, 2, D3D11_SDK_VERSION, &swap_chain_desc, &g_pSwapChain, &g_pd3dDevice, &feature_level, &g_pd3dDeviceContext);
    if (result != S_OK)
        return false;
    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

static void CreateRenderTarget()
{
    ID3D11Texture2D* back_buffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    g_pd3dDevice->CreateRenderTargetView(back_buffer, nullptr, &g_mainRenderTargetView);
    back_buffer->Release();
}

static void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;
    switch (msg)
    {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
        {
            g_ResizeWidth = static_cast<UINT>(LOWORD(lParam));
            g_ResizeHeight = static_cast<UINT>(HIWORD(lParam));
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
