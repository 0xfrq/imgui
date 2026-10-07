// Renaro Model Playground: a local model comparison workbench built with Dear ImGui.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define IMGUI_DEFINE_MATH_OPERATORS

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "llama_backend.h"

#include <windows.h>
#include <commdlg.h>
#include <d3d11.h>
#include <dwmapi.h>
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
#include <functional>
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
static ImFont* g_font_semibold = nullptr;
static ImFont* g_font_bold = nullptr;
static ImFont* g_font_italic = nullptr;
static ImFont* g_font_mono = nullptr;
static bool g_icons_loaded = false;
static float g_ui_scale = 1.0f;

// Type scale (unscaled pixels; DPI scaling is applied by ImGui through style.FontScaleDpi).
static const float kFontBody = 15.0f;
static const float kFontSmall = 13.0f;
static const float kFontCaption = 12.0f;
static const float kFontMono = 13.5f;
static const float kFontTitle = 21.0f;
static const float kFontHero = 25.0f;
static const float kFontStat = 23.0f;

// Palette: graphite surfaces with a single Renaro blue accent.
static const ImVec4 kBg = ImVec4(0.039f, 0.047f, 0.067f, 1.0f);
static const ImVec4 kRail = ImVec4(0.051f, 0.061f, 0.086f, 1.0f);
static const ImVec4 kSurface = ImVec4(0.067f, 0.080f, 0.110f, 1.0f);
static const ImVec4 kSurfaceRaised = ImVec4(0.094f, 0.110f, 0.149f, 1.0f);
static const ImVec4 kSurfaceHover = ImVec4(0.122f, 0.141f, 0.188f, 1.0f);
static const ImVec4 kInset = ImVec4(0.047f, 0.057f, 0.080f, 1.0f);
static const ImVec4 kBorder = ImVec4(0.149f, 0.169f, 0.220f, 1.0f);
static const ImVec4 kBorderSoft = ImVec4(0.110f, 0.126f, 0.168f, 1.0f);
static const ImVec4 kInk = ImVec4(0.925f, 0.937f, 0.961f, 1.0f);
static const ImVec4 kInkSoft = ImVec4(0.722f, 0.753f, 0.812f, 1.0f);
static const ImVec4 kMuted = ImVec4(0.494f, 0.533f, 0.612f, 1.0f);
static const ImVec4 kFaint = ImVec4(0.322f, 0.353f, 0.420f, 1.0f);
static const ImVec4 kAccent = ImVec4(0.322f, 0.565f, 1.000f, 1.0f);
static const ImVec4 kAccentHover = ImVec4(0.451f, 0.659f, 1.000f, 1.0f);
static const ImVec4 kAccentActive = ImVec4(0.247f, 0.471f, 0.890f, 1.0f);
static const ImVec4 kAccentInk = ImVec4(0.980f, 0.988f, 1.000f, 1.0f);
static const ImVec4 kSuccess = ImVec4(0.243f, 0.824f, 0.549f, 1.0f);
static const ImVec4 kWarning = ImVec4(0.961f, 0.725f, 0.290f, 1.0f);
static const ImVec4 kError = ImVec4(1.000f, 0.392f, 0.392f, 1.0f);
static const ImVec4 kViolet = ImVec4(0.655f, 0.545f, 0.980f, 1.0f);

// Placeholder for values that were not measured (em dash).
static const char* const kNone = "\xE2\x80\x94";

// Segoe MDL2 Assets glyphs (UTF-8). They are merged into the UI fonts when the font is present;
// Icon() returns an empty string otherwise so labels still render.
#define ICON_ADD            "\xEE\x9C\x90"  // U+E710
#define ICON_CANCEL         "\xEE\x9C\x91"  // U+E711
#define ICON_SETTINGS       "\xEE\x9C\x93"  // U+E713
#define ICON_STOP           "\xEE\x9C\x9A"  // U+E71A
#define ICON_SEARCH         "\xEE\x9C\xA1"  // U+E721
#define ICON_SEND           "\xEE\x9C\xA4"  // U+E724
#define ICON_CHEVRON_DOWN   "\xEE\x9C\x8D"  // U+E70D
#define ICON_CHECK          "\xEE\x9C\xBE"  // U+E73E
#define ICON_DELETE         "\xEE\x9D\x8D"  // U+E74D
#define ICON_PLAY           "\xEE\x9D\xA8"  // U+E768
#define ICON_CHEVRON_RIGHT  "\xEE\x9D\xAC"  // U+E76C
#define ICON_WARNING        "\xEE\x9E\xBA"  // U+E7BA
#define ICON_HISTORY        "\xEE\xA0\x9C"  // U+E81C
#define ICON_BULB           "\xEE\xA0\xAF"  // U+E82F
#define ICON_SELECT_ALL     "\xEE\xA2\xB3"  // U+E8B3
#define ICON_FOLDER         "\xEE\xA2\xB7"  // U+E8B7
#define ICON_CHAT           "\xEE\xA2\xBD"  // U+E8BD
#define ICON_COPY           "\xEE\xA3\x88"  // U+E8C8
#define ICON_BOLT           "\xEE\xA5\x85"  // U+E945
#define ICON_INFO           "\xEE\xA5\x86"  // U+E946
#define ICON_DIAG           "\xEE\xA7\x99"  // U+E9D9

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

static std::filesystem::path FindLlamaServer()
{
    const std::filesystem::path relative = std::filesystem::path(L"llama-server.exe");
    const std::filesystem::path executable = ExecutableDirectory();
    const std::array<std::filesystem::path, 8> candidates = {
        relative,
        std::filesystem::path(L"tools") / relative,
        std::filesystem::path(L"build") / L"bin" / relative,
        executable / relative,
        executable / L".." / relative,
        executable / L".." / L".." / relative,
        executable / L"tools" / relative,
        executable / L".." / L"tools" / relative
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
    file_dialog.lpstrFilter = L"GGUF model files (*.gguf)\0*.gguf\0All files (*.*)\0*.*\0";
    file_dialog.lpstrFile = buffer;
    file_dialog.nMaxFile = static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0]));
    file_dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    file_dialog.lpstrTitle = L"Import local model";
    if (!::GetOpenFileNameW(&file_dialog))
        return false;
    selected_path = buffer;
    return true;
}

static bool ChooseServerFile(std::wstring& selected_path)
{
    wchar_t buffer[4096] = {};
    OPENFILENAMEW file_dialog = {};
    file_dialog.lStructSize = sizeof(file_dialog);
    file_dialog.lpstrFilter = L"llama-server executable (llama-server.exe)\0llama-server.exe\0Executable files (*.exe)\0*.exe\0All files (*.*)\0*.*\0";
    file_dialog.lpstrFile = buffer;
    file_dialog.nMaxFile = static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0]));
    file_dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    file_dialog.lpstrTitle = L"Select llama-server.exe";
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

static std::string CommandModelArgument(const std::string& model)
{
    if (model.find('\\') != std::string::npos || model.find('/') != std::string::npos || model.find(' ') != std::string::npos || model.find(':') != std::string::npos)
        return "\"" + model + "\"";
    return model;
}

enum class Pane
{
    Playground,
    Diagnostics,
    Trace,
    Settings
};

enum class MessageKind
{
    System,
    User,
    Assistant,
    Reasoning,
    Error
};

struct Message
{
    MessageKind kind = MessageKind::System;
    std::string role;
    std::string text;
    std::string annotation;
};

static const char* const kWelcomeText = "Import one or more local GGUF models, select the files you want to compare, then run the same prompt against each model.";

struct ModelEntry
{
    std::string name;
    std::string detail;
    std::wstring path;
    bool selected = false;
};

struct TraceEntry
{
    std::string model;
    std::string event;
    std::string duration;
    std::string status;
    std::string detail;
};

struct AppState
{
    Pane pane = Pane::Playground;
    std::vector<ModelEntry> models;
    std::vector<Message> messages;
    std::vector<TraceEntry> trace;
    std::vector<LlamaRunResult> comparison_results;
    LlamaRunResult latest_result;
    bool has_latest_result = false;
    std::wstring server_path;
    std::string toast;
    char prompt[4096] = {};
    char command_search[256] = {};
    char trace_filter[128] = {};
    float temperature = 0.70f;
    int context = 8192;
    int max_tokens = 512;
    int threads = 8;
    int threads_batch = 8;
    int gpu_layers = 0;
    int batch_size = 512;
    int ubatch_size = 512;
    int seed = 42;
    bool flash_attention = false;
    bool cache_prompt = true;
    bool mmap = true;
    bool mlock = false;
    bool kv_offload = true;
    bool telemetry_sampled = false;
    double prompt_tokens_per_second = 0.0;
    double decode_tokens_per_second = 0.0;
    double prompt_eval_seconds = 0.0;
    double memory_gb = 0.0;
    bool run_busy = false;
    bool stop_requested = false;
    std::string streaming_model;
    int streaming_reasoning_message = -1;
    int streaming_response_message = -1;
    float toast_remaining = 0.0f;
    bool command_palette = false;
    bool command_focus_search = false;
    int command_index = 0;
    int command_opened_frame = -1;
    bool focus_prompt = false;
    bool prompt_focused = false;
    bool scroll_to_bottom = false;
    bool show_all_signals = false;
    LlamaServerBackend backend;

    AppState()
    {
        messages = {
            { MessageKind::System, "Renaro", kWelcomeText, {} }
        };
    }
};

static std::string CurrentRunCommand(const AppState& app)
{
    const std::string server = app.server_path.empty() ? "llama-server.exe" : WideToUtf8(app.server_path);
    for (const ModelEntry& model : app.models)
    {
        if (model.selected)
        {
            const std::string model_argument = model.path.empty() ? ModelSlug(model.name) : WideToUtf8(model.path);
            std::ostringstream command;
            command << CommandModelArgument(server) << " -m " << CommandModelArgument(model_argument)
                    << " -c " << app.context
                    << " --host 127.0.0.1 --port 8080"
                    << " -t " << app.threads
                    << " -tb " << app.threads_batch
                    << " -ngl " << app.gpu_layers
                    << " -b " << app.batch_size
                    << " -ub " << app.ubatch_size
                    << " --seed " << app.seed
                    << " --temp " << std::fixed << std::setprecision(2) << app.temperature;
            if (app.flash_attention)
                command << " --flash-attn on";
            if (!app.mmap)
                command << " --no-mmap";
            if (app.mlock)
                command << " --mlock";
            if (!app.kv_offload)
                command << " --no-kv-offload";
            return command.str();
        }
    }
    return "llama-server.exe -m <select-a-model> -c " + std::to_string(app.context) + " --host 127.0.0.1 --port 8080";
}

static void ShowToast(AppState& app, const std::string& message)
{
    app.toast = message;
    app.toast_remaining = 3.0f;
}

static void AddMessage(AppState& app, MessageKind kind, const std::string& role, const std::string& text, const std::string& annotation = {})
{
    app.messages.push_back({ kind, role, text, annotation });
    app.scroll_to_bottom = true;
}

static void ResetTelemetry(AppState& app)
{
    app.telemetry_sampled = false;
    app.prompt_tokens_per_second = 0.0;
    app.decode_tokens_per_second = 0.0;
    app.prompt_eval_seconds = 0.0;
    app.memory_gb = 0.0;
    app.stop_requested = false;
    app.comparison_results.clear();
    app.latest_result = {};
    app.has_latest_result = false;
    app.trace.clear();
    app.streaming_model.clear();
    app.streaming_reasoning_message = -1;
    app.streaming_response_message = -1;
}

static size_t SelectedModelCount(const AppState& app)
{
    return static_cast<size_t>(std::count_if(app.models.begin(), app.models.end(), [](const ModelEntry& model) { return model.selected; }));
}

static std::string FormatSeconds(double seconds)
{
    if (seconds <= 0.0)
        return kNone;
    std::ostringstream value;
    value << std::fixed << std::setprecision(2) << seconds << " s";
    return value.str();
}

static std::string FormatRate(double rate)
{
    if (rate <= 0.0)
        return kNone;
    std::ostringstream value;
    value << std::fixed << std::setprecision(1) << rate << " tok/s";
    return value.str();
}

static std::string FormatMemory(double gigabytes)
{
    if (gigabytes <= 0.0)
        return kNone;
    std::ostringstream value;
    value << std::fixed << std::setprecision(2) << gigabytes << " GB";
    return value.str();
}

static std::string FormatMilliseconds(double milliseconds)
{
    if (milliseconds <= 0.0)
        return kNone;
    std::ostringstream value;
    value << std::fixed << std::setprecision(1) << milliseconds << " ms";
    return value.str();
}

static std::string FormatCount(int count)
{
    return count > 0 ? std::to_string(count) : std::string(kNone);
}

static void AppendRunTrace(AppState& app, const LlamaRunResult& result)
{
    const std::string model = result.model;
    if (result.cancelled)
    {
        const std::string detail = "user stopped generation, " + FormatCount(result.response_bytes) + " response bytes received";
        app.trace.push_back({ model, "request", FormatSeconds(result.request_seconds), "stopped", detail });
        return;
    }
    if (!result.ok)
    {
        app.trace.push_back({ model, "request", FormatSeconds(result.request_seconds), "error", result.error.empty() ? "llama-server did not return a usable result" : result.error });
        return;
    }

    const std::string token_detail = "prompt " + FormatCount(result.prompt_tokens)
        + ", generated " + FormatCount(result.predicted_tokens)
        + ", total " + FormatCount(result.total_tokens)
        + ", cached " + FormatCount(result.tokens_cached)
        + ", reasoning " + FormatCount(result.reasoning_tokens)
        + (result.prompt_cache_reused ? ", cache reused" : "");
    const std::string memory_detail = "peak " + FormatMemory(result.peak_memory_gb)
        + ", private " + FormatMemory(result.private_memory_gb);
    const std::string server_detail = (result.model_alias.empty() ? "local model" : result.model_alias)
        + ", context " + FormatCount(result.context_size)
        + ", batch " + FormatCount(result.batch_size)
        + ", ubatch " + FormatCount(result.ubatch_size)
        + ", threads " + FormatCount(result.threads)
        + ", GPU layers " + FormatCount(result.gpu_layers);
    app.trace.push_back({ model, "server load", FormatSeconds(result.server_load_seconds), "ready", "llama-server process and /health" });
    app.trace.push_back({ model, "server config", kNone, "observed", server_detail });
    app.trace.push_back({ model, "prompt eval", FormatMilliseconds(result.prompt_ms), FormatRate(result.prompt_tokens_per_second), token_detail + ", " + FormatMilliseconds(result.prompt_ms_per_token) + " per token" });
    app.trace.push_back({ model, "decode", FormatMilliseconds(result.predicted_ms), FormatRate(result.decode_tokens_per_second), "predicted " + FormatCount(result.tokens_predicted) + ", " + FormatMilliseconds(result.predicted_ms_per_token) + " per token" });
    app.trace.push_back({ model, "response", FormatSeconds(result.request_seconds), result.finish_reason, "HTTP " + std::to_string(result.http_status) + ", " + result.timings_source + ", " + FormatCount(result.response_bytes) + " bytes" });
    if (result.slot_info_available)
    {
        const std::string slot_detail = "slot " + std::to_string(result.slot_id)
            + ", state " + (result.slot_state.empty() ? "unknown" : result.slot_state)
            + ", past " + FormatCount(result.n_past)
            + ", prompt " + FormatCount(result.n_prompt_tokens)
            + ", decoded " + FormatCount(result.n_decoded)
            + ", cache " + FormatCount(result.n_cache_tokens)
            + (result.stopped_eos ? ", EOS" : "")
            + (result.stopped_limit ? ", limit" : "")
            + (result.slot_truncated ? ", truncated" : "");
        app.trace.push_back({ model, "slot state", FormatMilliseconds(result.total_ms), result.slot_state.empty() ? "observed" : result.slot_state, slot_detail });
    }
    app.trace.push_back({ model, "memory", FormatMemory(result.memory_gb), "sampled", memory_detail });
    app.trace.push_back({ model, "CPU", FormatSeconds(result.cpu_seconds), "process time", "tokenize " + FormatMilliseconds(result.tokenize_ms) + ", sample " + FormatMilliseconds(result.sample_ms) });
    if (result.metrics_available)
        app.trace.push_back({ model, "metrics", kNone, "available", result.metrics_summary });
}

static void SampleTelemetry(AppState& app, const LlamaRunResult& result)
{
    app.telemetry_sampled = result.ok;
    app.prompt_tokens_per_second = result.prompt_tokens_per_second;
    app.decode_tokens_per_second = result.decode_tokens_per_second;
    app.prompt_eval_seconds = result.prompt_eval_seconds;
    app.memory_gb = result.memory_gb;
}

static void ResetStreamingMessages(AppState& app, const std::string& model)
{
    if (app.streaming_model == model)
        return;
    app.streaming_model = model;
    app.streaming_reasoning_message = -1;
    app.streaming_response_message = -1;
}

static void ApplyStreamingDelta(AppState& app, const LlamaRunResult& result)
{
    ResetStreamingMessages(app, result.model);
    if (!result.reasoning_delta.empty())
    {
        if (app.streaming_reasoning_message < 0)
        {
            AddMessage(app, MessageKind::Reasoning, result.model, {}, "streaming reasoning from llama.cpp");
            app.streaming_reasoning_message = static_cast<int>(app.messages.size() - 1);
        }
        app.messages[static_cast<size_t>(app.streaming_reasoning_message)].text += result.reasoning_delta;
    }
    if (!result.delta.empty())
    {
        if (app.streaming_response_message < 0)
        {
            AddMessage(app, MessageKind::Assistant, result.model, {}, "streaming from llama.cpp");
            app.streaming_response_message = static_cast<int>(app.messages.size() - 1);
        }
        app.messages[static_cast<size_t>(app.streaming_response_message)].text += result.delta;
    }
}

static void FinalizeStreamingMessages(AppState& app, const LlamaRunResult& result)
{
    ResetStreamingMessages(app, result.model);
    const std::string annotation = result.cancelled
        ? "stopped by user; partial response"
        : "llama.cpp \xC2\xB7 prompt " + FormatRate(result.prompt_tokens_per_second) + " \xC2\xB7 decode " + FormatRate(result.decode_tokens_per_second);
    if (!result.reasoning.empty())
    {
        if (app.streaming_reasoning_message < 0)
        {
            AddMessage(app, MessageKind::Reasoning, result.model, result.reasoning, "llama.cpp reasoning content");
            app.streaming_reasoning_message = static_cast<int>(app.messages.size() - 1);
        }
        else
        {
            Message& reasoning = app.messages[static_cast<size_t>(app.streaming_reasoning_message)];
            reasoning.text = result.reasoning;
            reasoning.annotation = "llama.cpp reasoning content";
        }
    }

    const bool response_is_reasoning_fallback = !result.text.empty() && result.text == result.reasoning && !result.reasoning.empty();
    if (!result.text.empty() && !response_is_reasoning_fallback)
    {
        if (app.streaming_response_message < 0)
        {
            AddMessage(app, MessageKind::Assistant, result.model, result.text, annotation);
            app.streaming_response_message = static_cast<int>(app.messages.size() - 1);
        }
        else
        {
            Message& response = app.messages[static_cast<size_t>(app.streaming_response_message)];
            response.text = result.text;
            response.annotation = annotation;
        }
    }
    else if (app.streaming_response_message >= 0)
    {
        Message& response = app.messages[static_cast<size_t>(app.streaming_response_message)];
        response.text = result.text;
        response.annotation = annotation;
    }
}

static void ToggleModel(AppState& app, size_t index)
{
    if (index >= app.models.size())
        return;
    if (app.models[index].path.empty())
    {
        ShowToast(app, "Import a local model file for " + app.models[index].name);
        return;
    }
    app.models[index].selected = !app.models[index].selected;
    ResetTelemetry(app);
    const size_t count = SelectedModelCount(app);
    ShowToast(app, std::to_string(count) + " model" + (count == 1 ? "" : "s") + " selected");
}

static void ClearPlayground(AppState& app, bool announce)
{
    if (app.run_busy)
        return;
    app.messages.clear();
    AddMessage(app, MessageKind::System, "Renaro", kWelcomeText);
    std::fill_n(app.prompt, sizeof(app.prompt), '\0');
    ResetTelemetry(app);
    app.pane = Pane::Playground;
    if (announce)
        ShowToast(app, "Playground cleared");
}

static void StartRun(AppState& app, const std::string& submitted_prompt)
{
    if (app.run_busy)
        return;
    std::vector<LlamaModelJob> jobs;
    for (const ModelEntry& model : app.models)
    {
        if (model.selected)
            jobs.push_back({ model.name, model.path });
    }
    if (jobs.empty())
    {
        ShowToast(app, "Select at least one model first");
        return;
    }

    const std::string trimmed_prompt = TrimCopy(submitted_prompt);
    const std::string prompt = trimmed_prompt.empty() ? "Explain the tradeoff between context length and decode speed." : trimmed_prompt;
    ResetTelemetry(app);
    AddMessage(app, MessageKind::User, "You", prompt);
    app.pane = Pane::Playground;
    LlamaRunConfig config;
    config.server_path = app.server_path;
    config.context = app.context;
    config.temperature = app.temperature;
    config.max_tokens = app.max_tokens;
    config.threads = app.threads;
    config.threads_batch = app.threads_batch;
    config.gpu_layers = app.gpu_layers;
    config.batch_size = app.batch_size;
    config.ubatch_size = app.ubatch_size;
    config.seed = app.seed;
    config.flash_attention = app.flash_attention;
    config.cache_prompt = app.cache_prompt;
    config.mmap = app.mmap;
    config.mlock = app.mlock;
    config.kv_offload = app.kv_offload;
    std::string error;
    if (!app.backend.StartComparison(jobs, prompt, config, error))
    {
        AddMessage(app, MessageKind::Error, "Renaro", error, "llama.cpp adapter error");
        ShowToast(app, error);
        return;
    }
    app.run_busy = true;
    app.stop_requested = false;
    std::fill_n(app.prompt, sizeof(app.prompt), '\0');
}

static void StopRun(AppState& app)
{
    if (!app.run_busy || app.stop_requested)
        return;
    app.stop_requested = true;
    app.backend.RequestStop();
    ShowToast(app, "Stopping generation...");
}

static void AdvanceBackend(AppState& app, float delta_time)
{
    LlamaRunResult result;
    bool received_result = false;
    bool stopped_result = false;
    while (app.backend.PopResult(result))
    {
        received_result = true;
        if (result.partial)
        {
            app.latest_result = result;
            app.has_latest_result = true;
            ApplyStreamingDelta(app, result);
            continue;
        }

        app.latest_result = result;
        app.has_latest_result = true;
        app.comparison_results.push_back(result);
        AppendRunTrace(app, result);
        if (result.cancelled)
        {
            stopped_result = true;
            FinalizeStreamingMessages(app, result);
            if (result.text.empty() && result.reasoning.empty())
                AddMessage(app, MessageKind::Assistant, result.model, "Generation stopped before a response was produced.", "stopped by user");
        }
        else if (result.ok)
        {
            FinalizeStreamingMessages(app, result);
            SampleTelemetry(app, result);
        }
        else
        {
            ResetStreamingMessages(app, result.model);
            AddMessage(app, MessageKind::Error, result.model, result.error.empty() ? std::string("llama-server did not return a usable result.") : result.error, "llama.cpp adapter error");
        }
    }
    if (app.run_busy && !app.backend.IsBusy())
    {
        const bool was_stop_requested = app.stop_requested;
        app.run_busy = false;
        if (was_stop_requested || stopped_result)
            ShowToast(app, "Generation stopped");
        else if (received_result)
            ShowToast(app, "llama.cpp comparison complete");
        app.stop_requested = false;
    }
    if (app.toast_remaining > 0.0f)
        app.toast_remaining = std::max(0.0f, app.toast_remaining - delta_time);
}

// -------------------------------------------------------------------------------------------------
// Visual system
// -------------------------------------------------------------------------------------------------

static float Px(float value)
{
    return value * g_ui_scale;
}

static ImU32 Col(const ImVec4& color, float alpha = 1.0f)
{
    return ImGui::GetColorU32(ImVec4(color.x, color.y, color.z, color.w * alpha));
}

static ImVec4 WithAlpha(const ImVec4& color, float alpha)
{
    return ImVec4(color.x, color.y, color.z, alpha);
}

static ImVec4 Mix(const ImVec4& from, const ImVec4& to, float amount)
{
    return ImVec4(from.x + (to.x - from.x) * amount, from.y + (to.y - from.y) * amount, from.z + (to.z - from.z) * amount, from.w + (to.w - from.w) * amount);
}

static const char* Icon(const char* glyph)
{
    return g_icons_loaded ? glyph : "";
}

struct ScopedFont
{
    ScopedFont(ImFont* font, float size) { ImGui::PushFont(font, size); }
    ~ScopedFont() { ImGui::PopFont(); }
    ScopedFont(const ScopedFont&) = delete;
    ScopedFont& operator=(const ScopedFont&) = delete;
};

static ImVec4 SeriesColor(size_t index)
{
    static const ImVec4 palette[] = {
        ImVec4(0.322f, 0.565f, 1.000f, 1.0f),
        ImVec4(0.176f, 0.831f, 0.749f, 1.0f),
        ImVec4(0.655f, 0.545f, 0.980f, 1.0f),
        ImVec4(0.984f, 0.698f, 0.290f, 1.0f),
        ImVec4(0.957f, 0.447f, 0.714f, 1.0f),
        ImVec4(0.639f, 0.863f, 0.329f, 1.0f)
    };
    return palette[index % (sizeof(palette) / sizeof(palette[0]))];
}

static ImVec4 ModelColor(const AppState& app, const std::string& name)
{
    for (size_t index = 0; index < app.models.size(); ++index)
    {
        if (app.models[index].name == name)
            return SeriesColor(index);
    }
    return kMuted;
}

static const LlamaRunResult* FindResult(const AppState& app, const std::string& model)
{
    for (auto it = app.comparison_results.rbegin(); it != app.comparison_results.rend(); ++it)
    {
        if (it->model == model)
            return &*it;
    }
    return nullptr;
}

static std::string FormatNumber(double value, int precision)
{
    if (value <= 0.0)
        return kNone;
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(precision) << value;
    return stream.str();
}

static void ApplyRenaroStyle()
{
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(16.0f, 16.0f);
    style.FramePadding = ImVec2(10.0f, 7.0f);
    style.CellPadding = ImVec2(10.0f, 7.0f);
    style.ItemSpacing = ImVec2(8.0f, 8.0f);
    style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    style.TouchExtraPadding = ImVec2(0.0f, 0.0f);
    style.IndentSpacing = 18.0f;
    style.ScrollbarSize = 10.0f;
    style.GrabMinSize = 14.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.WindowRounding = 14.0f;
    style.ChildRounding = 12.0f;
    style.FrameRounding = 8.0f;
    style.PopupRounding = 10.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabRounding = 8.0f;
    style.TabRounding = 6.0f;
    style.SeparatorTextBorderSize = 1.0f;
    style.SelectableTextAlign = ImVec2(0.0f, 0.5f);
    style.DisabledAlpha = 0.45f;

    ImVec4* colors = style.Colors;
    const ImVec4 clear(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_Text] = kInk;
    colors[ImGuiCol_TextDisabled] = kMuted;
    colors[ImGuiCol_WindowBg] = kBg;
    colors[ImGuiCol_ChildBg] = clear;
    colors[ImGuiCol_PopupBg] = kSurfaceRaised;
    colors[ImGuiCol_Border] = kBorder;
    colors[ImGuiCol_BorderShadow] = clear;
    colors[ImGuiCol_FrameBg] = kInset;
    colors[ImGuiCol_FrameBgHovered] = Mix(kInset, kSurfaceRaised, 0.6f);
    colors[ImGuiCol_FrameBgActive] = kSurfaceRaised;
    colors[ImGuiCol_TitleBg] = kRail;
    colors[ImGuiCol_TitleBgActive] = kRail;
    colors[ImGuiCol_TitleBgCollapsed] = kRail;
    colors[ImGuiCol_MenuBarBg] = kRail;
    colors[ImGuiCol_ScrollbarBg] = clear;
    colors[ImGuiCol_ScrollbarGrab] = kBorder;
    colors[ImGuiCol_ScrollbarGrabHovered] = Mix(kBorder, kMuted, 0.4f);
    colors[ImGuiCol_ScrollbarGrabActive] = kMuted;
    colors[ImGuiCol_CheckMark] = kAccent;
    colors[ImGuiCol_SliderGrab] = kAccent;
    colors[ImGuiCol_SliderGrabActive] = kAccentHover;
    colors[ImGuiCol_Button] = kSurfaceRaised;
    colors[ImGuiCol_ButtonHovered] = kSurfaceHover;
    colors[ImGuiCol_ButtonActive] = kBorder;
    colors[ImGuiCol_Header] = WithAlpha(kAccent, 0.14f);
    colors[ImGuiCol_HeaderHovered] = WithAlpha(kAccent, 0.20f);
    colors[ImGuiCol_HeaderActive] = WithAlpha(kAccent, 0.28f);
    colors[ImGuiCol_Separator] = kBorderSoft;
    colors[ImGuiCol_SeparatorHovered] = kAccent;
    colors[ImGuiCol_SeparatorActive] = kAccentHover;
    colors[ImGuiCol_ResizeGrip] = clear;
    colors[ImGuiCol_ResizeGripHovered] = WithAlpha(kAccent, 0.4f);
    colors[ImGuiCol_ResizeGripActive] = kAccent;
    colors[ImGuiCol_Tab] = kSurface;
    colors[ImGuiCol_TabHovered] = kSurfaceHover;
    colors[ImGuiCol_TabSelected] = kSurfaceRaised;
    colors[ImGuiCol_TableHeaderBg] = kSurfaceRaised;
    colors[ImGuiCol_TableBorderStrong] = kBorder;
    colors[ImGuiCol_TableBorderLight] = kBorderSoft;
    colors[ImGuiCol_TableRowBg] = clear;
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.018f);
    colors[ImGuiCol_TextSelectedBg] = WithAlpha(kAccent, 0.32f);
    colors[ImGuiCol_InputTextCursor] = kAccentHover;
    colors[ImGuiCol_DragDropTarget] = kAccent;
    colors[ImGuiCol_NavCursor] = kAccent;
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.010f, 0.014f, 0.024f, 0.66f);
}

// -------------------------------------------------------------------------------------------------
// Primitive widgets
// -------------------------------------------------------------------------------------------------

static void TextColoredUnformatted(const ImVec4& color, const char* text, const char* text_end = nullptr)
{
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(text, text_end);
    ImGui::PopStyleColor();
}

static void TextWithFont(ImFont* font, float size, const ImVec4& color, const char* text)
{
    ScopedFont scoped(font, size);
    TextColoredUnformatted(color, text);
}

static void WrappedText(const ImVec4& color, const char* text, float size = kFontBody, float wrap_width = 0.0f)
{
    ScopedFont scoped(g_font_body, size);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushTextWrapPos(wrap_width > 0.0f ? ImGui::GetCursorPosX() + wrap_width : 0.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

static void SectionLabel(const char* text)
{
    TextWithFont(g_font_semibold, kFontCaption, kMuted, text);
}

static void VerticalSpace(float height)
{
    ImGui::Dummy(ImVec2(0.0f, Px(height)));
}

// Moves the cursor onto the current line so that an item of `width` ends at `right_edge` (screen space).
static void SameLineRight(float width, float right_edge)
{
    ImGui::SameLine();
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(std::max(cursor.x, right_edge - width), cursor.y));
}

static void PopUtf8Char(std::string& text)
{
    while (!text.empty())
    {
        const unsigned char last = static_cast<unsigned char>(text.back());
        text.pop_back();
        if ((last & 0xC0) != 0x80)
            break;
    }
}

static std::string Ellipsize(const std::string& text, float max_width)
{
    if (max_width <= 0.0f || ImGui::CalcTextSize(text.c_str()).x <= max_width)
        return text;
    const char* ellipsis = "\xE2\x80\xA6";
    const float ellipsis_width = ImGui::CalcTextSize(ellipsis).x;
    std::string result = text;
    while (!result.empty() && ImGui::CalcTextSize(result.c_str()).x + ellipsis_width > max_width)
        PopUtf8Char(result);
    return result + ellipsis;
}

static ImVec2 PillSize(const char* text, const char* icon = nullptr)
{
    ScopedFont scoped(g_font_semibold, kFontCaption);
    const char* glyph = icon ? Icon(icon) : "";
    float width = ImGui::CalcTextSize(text).x + Px(16.0f);
    if (glyph[0] != '\0')
        width += ImGui::CalcTextSize(glyph).x + Px(5.0f);
    return ImVec2(width, ImGui::GetFontSize() + Px(7.0f));
}

static ImVec2 DrawPill(ImDrawList* draw_list, const ImVec2& pos, const char* text, const ImVec4& color, const char* icon = nullptr)
{
    const ImVec2 size = PillSize(text, icon);
    ScopedFont scoped(g_font_semibold, kFontCaption);
    const char* glyph = icon ? Icon(icon) : "";
    draw_list->AddRectFilled(pos, pos + size, Col(color, 0.13f), size.y * 0.5f);
    draw_list->AddRect(pos, pos + size, Col(color, 0.28f), size.y * 0.5f);
    ImVec2 cursor(pos.x + Px(8.0f), pos.y + std::floor((size.y - ImGui::GetFontSize()) * 0.5f));
    if (glyph[0] != '\0')
    {
        draw_list->AddText(cursor, Col(color), glyph);
        cursor.x += ImGui::CalcTextSize(glyph).x + Px(5.0f);
    }
    draw_list->AddText(cursor, Col(color), text);
    return size;
}

static void Pill(const char* text, const ImVec4& color, const char* icon = nullptr)
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size = DrawPill(ImGui::GetWindowDrawList(), pos, text, color, icon);
    ImGui::Dummy(size);
}

static ImVec2 KeyCapSize(const char* text)
{
    ScopedFont scoped(g_font_mono, kFontCaption);
    return ImVec2(ImGui::CalcTextSize(text).x + Px(10.0f), ImGui::GetFontSize() + Px(6.0f));
}

static ImVec2 DrawKeyCap(ImDrawList* draw_list, const ImVec2& pos, const char* text)
{
    const ImVec2 size = KeyCapSize(text);
    ScopedFont scoped(g_font_mono, kFontCaption);
    draw_list->AddRectFilled(pos, pos + size, Col(kSurfaceHover), Px(5.0f));
    draw_list->AddRect(pos, pos + size, Col(kBorder), Px(5.0f));
    draw_list->AddText(ImVec2(pos.x + Px(5.0f), pos.y + Px(3.0f)), Col(kInkSoft), text);
    return size;
}

static void StatusDot(ImDrawList* draw_list, const ImVec2& center, float radius, const ImVec4& color, bool pulsing)
{
    if (pulsing)
    {
        const float phase = static_cast<float>(std::fmod(ImGui::GetTime(), 1.4) / 1.4);
        draw_list->AddCircleFilled(center, radius * (1.0f + 1.8f * phase), Col(color, 0.38f * (1.0f - phase)));
    }
    else
    {
        draw_list->AddCircleFilled(center, radius * 2.0f, Col(color, 0.14f));
    }
    draw_list->AddCircleFilled(center, radius, Col(color));
}

enum class ButtonKind
{
    Primary,
    Secondary,
    Ghost,
    Danger
};

static bool UiButton(const char* id, const char* icon, const char* label, ButtonKind kind, ImVec2 size = ImVec2(0.0f, 0.0f), float font_size = kFontSmall + 0.5f)
{
    ScopedFont scoped(g_font_semibold, font_size);
    const char* glyph = icon ? Icon(icon) : "";
    const bool has_icon = glyph[0] != '\0';
    const bool has_label = label != nullptr && label[0] != '\0';
    const float icon_width = has_icon ? ImGui::CalcTextSize(glyph).x : 0.0f;
    const float label_width = has_label ? ImGui::CalcTextSize(label).x : 0.0f;
    const float gap = has_icon && has_label ? Px(7.0f) : 0.0f;
    const float content_width = icon_width + gap + label_width;
    if (size.y <= 0.0f)
        size.y = Px(34.0f);
    if (size.x < 0.0f)
        size.x = std::max(Px(24.0f), ImGui::GetContentRegionAvail().x);
    else if (size.x == 0.0f)
        size.x = has_label ? content_width + Px(28.0f) : size.y;

    const bool pressed = ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    if (hovered)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    ImVec4 background;
    ImVec4 border(0.0f, 0.0f, 0.0f, 0.0f);
    ImVec4 foreground;
    switch (kind)
    {
    case ButtonKind::Primary:
        background = held ? kAccentActive : (hovered ? kAccentHover : kAccent);
        foreground = kAccentInk;
        break;
    case ButtonKind::Secondary:
        background = held ? kBorder : (hovered ? kSurfaceHover : kSurfaceRaised);
        border = kBorder;
        foreground = kInk;
        break;
    case ButtonKind::Danger:
        background = WithAlpha(kError, held ? 0.32f : (hovered ? 0.24f : 0.15f));
        border = WithAlpha(kError, 0.45f);
        foreground = Mix(kError, kInk, 0.25f);
        break;
    case ButtonKind::Ghost:
    default:
        background = held ? kSurfaceHover : (hovered ? kSurfaceRaised : ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        foreground = hovered ? kInk : kInkSoft;
        break;
    }

    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const float rounding = Px(8.0f);
    if (background.w > 0.0f)
        draw_list->AddRectFilled(min, max, Col(background), rounding);
    if (border.w > 0.0f)
        draw_list->AddRect(min, max, Col(border), rounding);
    if (kind == ButtonKind::Primary)
        draw_list->AddLine(ImVec2(min.x + rounding, min.y + 1.0f), ImVec2(max.x - rounding, min.y + 1.0f), Col(kAccentInk, 0.22f));

    ImVec2 cursor(min.x + std::floor((size.x - content_width) * 0.5f), min.y + std::floor((size.y - ImGui::GetFontSize()) * 0.5f));
    if (has_icon)
    {
        draw_list->AddText(cursor, Col(foreground), glyph);
        cursor.x += icon_width + gap;
    }
    if (has_label)
        draw_list->AddText(cursor, Col(foreground), label);
    return pressed;
}

static bool IconButton(const char* id, const char* icon, const char* fallback, const char* tooltip, float size = 30.0f)
{
    const bool has_icon = Icon(icon)[0] != '\0';
    const bool pressed = UiButton(id, has_icon ? icon : nullptr, has_icon ? nullptr : fallback, ButtonKind::Ghost, ImVec2(has_icon ? Px(size) : 0.0f, Px(size)));
    if (tooltip)
        ImGui::SetItemTooltip("%s", tooltip);
    return pressed;
}

static float IconButtonWidth(const char* icon, const char* fallback, float size = 30.0f)
{
    if (Icon(icon)[0] != '\0')
        return Px(size);
    ScopedFont scoped(g_font_semibold, kFontSmall + 0.5f);
    return ImGui::CalcTextSize(fallback).x + Px(28.0f);
}

static bool ToggleSwitch(const char* id, bool* value)
{
    const float height = Px(20.0f);
    const float width = Px(36.0f);
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(width, height));
    if (pressed)
        *value = !*value;
    const bool hovered = ImGui::IsItemHovered();
    if (hovered)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    const float target = *value ? 1.0f : 0.0f;
    float* animation = ImGui::GetStateStorage()->GetFloatRef(ImHashStr("toggle-animation", 0, ImGui::GetItemID()), target);
    *animation += (target - *animation) * std::min(1.0f, ImGui::GetIO().DeltaTime * 14.0f);

    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(min, max, Col(Mix(kSurfaceHover, kAccent, *animation)), height * 0.5f);
    if (*animation < 0.5f)
        draw_list->AddRect(min, max, Col(kBorder), height * 0.5f);
    const float radius = height * 0.5f - Px(3.0f) + (hovered ? Px(0.5f) : 0.0f);
    const ImVec2 knob(min.x + height * 0.5f + (width - height) * *animation, min.y + height * 0.5f);
    draw_list->AddCircleFilled(knob, radius, Col(kInk));
    return pressed;
}

static void BeginCard(const char* id, const ImVec2& size, ImGuiChildFlags child_flags = ImGuiChildFlags_None, ImGuiWindowFlags window_flags = ImGuiWindowFlags_None, ImVec2 padding = ImVec2(-1.0f, -1.0f), const ImVec4& background = kSurface)
{
    if (padding.x < 0.0f)
        padding = ImVec2(Px(16.0f), Px(16.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, background);
    ImGui::PushStyleColor(ImGuiCol_Border, kBorderSoft);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, Px(12.0f));
    ImGui::BeginChild(id, size, ImGuiChildFlags_Borders | child_flags, window_flags);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

static void EndCard()
{
    ImGui::EndChild();
}

static void CardTitle(const char* title, const char* subtitle = nullptr)
{
    TextWithFont(g_font_semibold, kFontBody, kInk, title);
    if (subtitle)
    {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Px(4.0f));
        WrappedText(kMuted, subtitle, kFontSmall);
    }
}

static void DrawLogoTile(float size)
{
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max = min + ImVec2(size, size);
    ImGui::Dummy(ImVec2(size, size));
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const float rounding = size * 0.28f;
    draw_list->AddRectFilled(min, max, Col(kAccent), rounding);
    draw_list->AddRectFilled(min, ImVec2(max.x, min.y + size * 0.5f), Col(kAccentInk, 0.06f), rounding, ImDrawFlags_RoundCornersTop);
    draw_list->AddRect(min, max, Col(kAccentInk, 0.16f), rounding);
    if (g_logo_srv)
    {
        const float inset = size * 0.16f;
        draw_list->AddImage((ImTextureID)(intptr_t)g_logo_srv, min + ImVec2(inset, inset), max - ImVec2(inset, inset));
    }
    else
    {
        ScopedFont scoped(g_font_bold, size * 0.52f / g_ui_scale);
        const ImVec2 text_size = ImGui::CalcTextSize("R");
        draw_list->AddText(min + (ImVec2(size, size) - text_size) * 0.5f, Col(kAccentInk), "R");
    }
}

static void EmptyState(const char* icon, const char* title, const char* body)
{
    const float width = ImGui::GetContentRegionAvail().x;
    const float base_x = ImGui::GetCursorPosX();
    VerticalSpace(36.0f);
    const char* glyph = Icon(icon);
    if (glyph[0] != '\0')
    {
        const float circle = Px(56.0f);
        ImGui::SetCursorPosX(base_x + (width - circle) * 0.5f);
        const ImVec2 min = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(circle, circle));
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        draw_list->AddCircleFilled(min + ImVec2(circle, circle) * 0.5f, circle * 0.5f, Col(kAccent, 0.12f));
        ScopedFont scoped(g_font_body, 22.0f);
        const ImVec2 glyph_size = ImGui::CalcTextSize(glyph);
        draw_list->AddText(min + (ImVec2(circle, circle) - glyph_size) * 0.5f, Col(kAccentHover), glyph);
        VerticalSpace(4.0f);
    }
    {
        ScopedFont scoped(g_font_bold, 17.0f);
        ImGui::SetCursorPosX(base_x + (width - ImGui::CalcTextSize(title).x) * 0.5f);
        TextColoredUnformatted(kInk, title);
    }
    {
        ScopedFont scoped(g_font_body, kFontSmall + 0.5f);
        const float wrap = std::min(width, Px(420.0f));
        const ImVec2 body_size = ImGui::CalcTextSize(body, nullptr, false, wrap);
        ImGui::SetCursorPosX(base_x + (width - body_size.x) * 0.5f);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(), pos, Col(kMuted), body, nullptr, wrap);
        ImGui::Dummy(body_size);
    }
}

// -------------------------------------------------------------------------------------------------
// Actions shared by several surfaces
// -------------------------------------------------------------------------------------------------

static std::string FormatBytes(std::uintmax_t bytes)
{
    const double gib = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    std::ostringstream value;
    if (gib >= 1.0)
        value << std::fixed << std::setprecision(2) << gib << " GB";
    else
        value << std::fixed << std::setprecision(0) << static_cast<double>(bytes) / (1024.0 * 1024.0) << " MB";
    return value.str();
}

// Builds a short description such as "4.37 GB / 8B / Q4_K_M" (dot-separated in the UI) from the file size and common GGUF naming conventions.
static std::string DescribeModelFile(const std::filesystem::path& path)
{
    std::vector<std::string> parts;
    std::error_code error;
    const std::uintmax_t bytes = std::filesystem::file_size(path, error);
    if (!error && bytes > 0)
        parts.push_back(FormatBytes(bytes));

    const std::string stem = WideToUtf8(path.stem().wstring());
    std::string params;
    std::string quant;
    std::string token;
    const auto is_digit = [](char character) { return std::isdigit(static_cast<unsigned char>(character)) != 0; };
    const auto flush = [&]()
    {
        if (token.empty())
            return;
        std::string upper = token;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char character) { return static_cast<char>(std::toupper(character)); });
        const bool quant_like = (upper.size() >= 2 && upper[0] == 'Q' && is_digit(upper[1]))
            || (upper.size() >= 3 && upper.rfind("IQ", 0) == 0 && is_digit(upper[2]))
            || upper == "F16" || upper == "BF16" || upper == "F32" || upper == "FP16";
        if (quant.empty() && quant_like)
            quant = upper;
        const bool params_like = upper.size() >= 2 && upper.back() == 'B' && is_digit(upper.front())
            && std::all_of(upper.begin(), upper.end() - 1, [&is_digit](char character) { return is_digit(character) || character == '.'; });
        if (params.empty() && params_like)
            params = upper;
        token.clear();
    };
    for (size_t index = 0; index < stem.size(); ++index)
    {
        const char character = stem[index];
        const bool decimal_point = character == '.' && index > 0 && index + 1 < stem.size() && is_digit(stem[index - 1]) && is_digit(stem[index + 1]);
        if (character == '-' || character == ' ' || (character == '.' && !decimal_point))
            flush();
        else
            token.push_back(character);
    }
    flush();

    if (!params.empty())
        parts.push_back(params);
    if (!quant.empty())
        parts.push_back(quant);
    if (parts.empty())
        return "local GGUF file";
    std::string description = parts.front();
    for (size_t index = 1; index < parts.size(); ++index)
        description += " \xC2\xB7 " + parts[index];
    return description;
}

static void ImportModel(AppState& app)
{
    if (app.run_busy)
        return;
    std::wstring selected_path;
    if (!ChooseModelFile(selected_path))
        return;
    const std::filesystem::path path(selected_path);
    const std::string name = WideToUtf8(path.stem().wstring());
    if (name.empty())
        return;
    const std::string detail = DescribeModelFile(path);
    auto existing = std::find_if(app.models.begin(), app.models.end(), [&selected_path](const ModelEntry& item) { return item.path == selected_path; });
    if (existing == app.models.end())
    {
        std::string display_name = name;
        int duplicate_index = 2;
        while (std::any_of(app.models.begin(), app.models.end(), [&display_name](const ModelEntry& item) { return item.name == display_name; }))
            display_name = name + " (" + std::to_string(duplicate_index++) + ")";
        app.models.push_back({ display_name, detail, selected_path, true });
    }
    else
    {
        existing->detail = detail;
        existing->selected = true;
    }
    ResetTelemetry(app);
    ShowToast(app, "Added " + name + " to the comparison set");
}

static void LocateServer(AppState& app)
{
    std::wstring selected_path;
    if (ChooseServerFile(selected_path))
    {
        app.server_path = selected_path;
        ShowToast(app, "llama-server.exe selected");
    }
}

static void CopyRunCommand(AppState& app)
{
    const std::string command = CurrentRunCommand(app);
    ImGui::SetClipboardText(command.c_str());
    ShowToast(app, "Launch command copied");
}

static void OpenCommandPalette(AppState& app)
{
    if (app.command_palette)
        return;
    app.command_palette = true;
    app.command_focus_search = true;
    app.command_index = 0;
    app.command_search[0] = '\0';
    app.command_opened_frame = ImGui::GetFrameCount();
}

static void FocusPrompt(AppState& app)
{
    app.pane = Pane::Playground;
    app.focus_prompt = true;
}

static const int kContextValues[] = { 2048, 4096, 8192, 16384, 32768, 65536, 131072 };
static const char* kContextLabels[] = { "2,048 tokens", "4,096 tokens", "8,192 tokens", "16,384 tokens", "32,768 tokens", "65,536 tokens", "131,072 tokens" };

static void ContextCombo(AppState& app, const char* id)
{
    int context_index = 2;
    for (int index = 0; index < IM_ARRAYSIZE(kContextValues); ++index)
    {
        if (kContextValues[index] == app.context)
            context_index = index;
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo(id, &context_index, kContextLabels, IM_ARRAYSIZE(kContextLabels)))
        app.context = kContextValues[context_index];
}

// Label on the left, current value right-aligned on the same line.
static void ControlLabel(const char* label, const char* value)
{
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    ScopedFont scoped(g_font_semibold, kFontSmall);
    TextColoredUnformatted(kInkSoft, label);
    if (value)
    {
        SameLineRight(ImGui::CalcTextSize(value).x, right);
        TextColoredUnformatted(kInk, value);
    }
}

// -------------------------------------------------------------------------------------------------
// Markdown rendering
// -------------------------------------------------------------------------------------------------

enum class MarkdownSpanStyle
{
    Normal,
    Strong,
    Emphasis,
    Code,
    Link,
    Strike
};

struct MarkdownSpan
{
    std::string text;
    MarkdownSpanStyle style = MarkdownSpanStyle::Normal;
};

static std::vector<MarkdownSpan> ParseMarkdownInline(const std::string& value)
{
    std::vector<MarkdownSpan> spans;
    size_t cursor = 0;
    size_t plain_start = 0;
    const auto flush_plain = [&spans, &value, &plain_start](size_t end)
    {
        if (end > plain_start)
            spans.push_back({ value.substr(plain_start, end - plain_start), MarkdownSpanStyle::Normal });
    };
    const auto flush_to = [&flush_plain, &cursor, &plain_start](size_t end)
    {
        flush_plain(end);
        cursor = end;
        plain_start = end;
    };

    while (cursor < value.size())
    {
        if (value[cursor] == '\\' && cursor + 1 < value.size())
        {
            cursor += 2;
            continue;
        }

        const char* delimiter = nullptr;
        MarkdownSpanStyle style = MarkdownSpanStyle::Normal;
        size_t delimiter_length = 0;
        if (value.compare(cursor, 2, "**") == 0 || value.compare(cursor, 2, "__") == 0)
        {
            delimiter = value.c_str() + cursor;
            style = MarkdownSpanStyle::Strong;
            delimiter_length = 2;
        }
        else if (value.compare(cursor, 2, "~~") == 0)
        {
            delimiter = value.c_str() + cursor;
            style = MarkdownSpanStyle::Strike;
            delimiter_length = 2;
        }
        else if (value[cursor] == '`')
        {
            delimiter = value.c_str() + cursor;
            style = MarkdownSpanStyle::Code;
            delimiter_length = 1;
        }
        else if (value[cursor] == '*')
        {
            delimiter = value.c_str() + cursor;
            style = MarkdownSpanStyle::Emphasis;
            delimiter_length = 1;
        }
        else if (value[cursor] == '_' && (cursor == 0 || value[cursor - 1] != '\\'))
        {
            delimiter = value.c_str() + cursor;
            style = MarkdownSpanStyle::Emphasis;
            delimiter_length = 1;
        }
        else if (value[cursor] == '[')
        {
            const size_t label_end = value.find("](", cursor + 1);
            const size_t link_end = label_end == std::string::npos ? std::string::npos : value.find(')', label_end + 2);
            if (label_end != std::string::npos && link_end != std::string::npos)
            {
                flush_to(cursor);
                spans.push_back({ value.substr(cursor + 1, label_end - cursor - 1), MarkdownSpanStyle::Link });
                cursor = link_end + 1;
                plain_start = cursor;
                continue;
            }
        }

        if (!delimiter)
        {
            ++cursor;
            continue;
        }

        const std::string token(delimiter, delimiter_length);
        const size_t closing = value.find(token, cursor + delimiter_length);
        if (closing == std::string::npos || closing == cursor + delimiter_length)
        {
            ++cursor;
            continue;
        }
        flush_to(cursor);
        spans.push_back({ value.substr(cursor + delimiter_length, closing - cursor - delimiter_length), style });
        cursor = closing + delimiter_length;
        plain_start = cursor;
    }
    flush_plain(value.size());
    if (spans.empty())
        spans.push_back({ value, MarkdownSpanStyle::Normal });
    return spans;
}

// Lays out styled spans word by word so mixed bold/code/link text wraps like a paragraph.
static void DrawInlineFlow(const std::string& value, float width, const ImVec4& color, ImFont* base_font, float pixel_size)
{
    const std::vector<MarkdownSpan> spans = ParseMarkdownInline(value);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float line_height = pixel_size;
    const float line_step = std::floor(pixel_size * 1.45f);
    width = std::max(width, Px(40.0f));
    float x = 0.0f;
    float y = 0.0f;

    for (const MarkdownSpan& span : spans)
    {
        if (span.text.empty())
            continue;
        ImFont* font = base_font;
        float size = pixel_size;
        ImVec4 span_color = color;
        switch (span.style)
        {
        case MarkdownSpanStyle::Strong:
            font = g_font_bold;
            span_color = kInk;
            break;
        case MarkdownSpanStyle::Emphasis:
            font = g_font_italic;
            span_color = Mix(color, kInk, 0.35f);
            break;
        case MarkdownSpanStyle::Code:
            font = g_font_mono;
            size = std::floor(pixel_size * 0.92f);
            span_color = Mix(kAccentHover, kInk, 0.25f);
            break;
        case MarkdownSpanStyle::Link:
            span_color = kAccentHover;
            break;
        case MarkdownSpanStyle::Strike:
            span_color = kMuted;
            break;
        case MarkdownSpanStyle::Normal:
        default:
            break;
        }

        const char* text = span.text.c_str();
        const char* const end = text + span.text.size();
        while (text < end)
        {
            const char* word_end = text;
            while (word_end < end && *word_end != ' ')
                ++word_end;
            const char* token_end = word_end;
            while (token_end < end && *token_end == ' ')
                ++token_end;

            float word_width = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text, word_end).x;
            if (x > 0.0f && x + word_width > width)
            {
                x = 0.0f;
                y += line_step;
            }

            const char* draw_end = token_end;
            float advance = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text, token_end).x;
            bool hard_break = false;
            if (word_width > width - x && word_end > text)
            {
                // A single token longer than the line: split it on character boundaries.
                const char* cut = text;
                float cut_width = 0.0f;
                while (cut < word_end)
                {
                    const char* next = cut + 1;
                    while (next < word_end && (static_cast<unsigned char>(*next) & 0xC0) == 0x80)
                        ++next;
                    const float next_width = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text, next).x;
                    if (next_width > width - x && cut > text)
                        break;
                    cut = next;
                    cut_width = next_width;
                }
                if (cut < word_end)
                {
                    draw_end = cut;
                    advance = cut_width;
                    word_width = cut_width;
                    hard_break = true;
                }
            }

            const ImVec2 pos(origin.x + x, origin.y + y + std::floor((line_height - size) * 0.7f));
            if (ImGui::IsRectVisible(pos, pos + ImVec2(advance, line_height)))
            {
                if (span.style == MarkdownSpanStyle::Code)
                    draw_list->AddRectFilled(pos - ImVec2(Px(3.0f), Px(1.0f)), pos + ImVec2(word_width + Px(3.0f), size + Px(2.0f)), Col(kSurfaceHover), Px(4.0f));
                draw_list->AddText(font, size, pos, Col(span_color), text, draw_end);
                if (span.style == MarkdownSpanStyle::Link)
                    draw_list->AddLine(ImVec2(pos.x, pos.y + size + 1.0f), ImVec2(pos.x + word_width, pos.y + size + 1.0f), Col(span_color, 0.6f));
                else if (span.style == MarkdownSpanStyle::Strike)
                    draw_list->AddLine(ImVec2(pos.x, pos.y + size * 0.55f), ImVec2(pos.x + word_width, pos.y + size * 0.55f), Col(span_color));
            }
            x += advance;
            text = draw_end;
            if (hard_break)
            {
                x = 0.0f;
                y += line_step;
            }
        }
    }
    ImGui::Dummy(ImVec2(width, y + line_height));
}

static bool IsMarkdownRule(const std::string& value)
{
    const std::string trimmed = TrimCopy(value);
    if (trimmed.size() < 3)
        return false;
    const char marker = trimmed.front();
    if (marker != '-' && marker != '*' && marker != '_')
        return false;
    return std::all_of(trimmed.begin(), trimmed.end(), [marker](char character) { return character == marker || character == ' '; });
}

static void DrawMarkdownCodeBlock(AppState& app, const std::string& code, const std::string& language, int block_index, float width)
{
    ImGui::PushID(block_index);
    const float right = ImGui::GetCursorScreenPos().x + width;
    {
        ScopedFont scoped(g_font_mono, kFontCaption);
        TextColoredUnformatted(kMuted, language.empty() ? "code" : language.c_str());
    }
    const float copy_width = IconButtonWidth(ICON_COPY, "Copy", 24.0f);
    SameLineRight(copy_width, right);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Px(5.0f));
    if (IconButton("##CopyCode", ICON_COPY, "Copy", "Copy code", 24.0f))
    {
        ImGui::SetClipboardText(code.c_str());
        ShowToast(app, "Code copied");
    }

    ScopedFont mono(g_font_mono, kFontMono);
    const int line_count = static_cast<int>(std::count(code.begin(), code.end(), '\n')) + 1;
    const float height = std::min(Px(340.0f), line_count * ImGui::GetFontSize() + Px(22.0f) + ImGui::GetStyle().ScrollbarSize);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kInset);
    ImGui::PushStyleColor(ImGuiCol_Border, kBorderSoft);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(14.0f), Px(10.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, Px(8.0f));
    ImGui::BeginChild("##Code", ImVec2(width, height), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoSavedSettings);
    TextColoredUnformatted(Mix(kInk, kAccentHover, 0.12f), code.c_str(), code.c_str() + code.size());
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    ImGui::PopID();
}

static void DrawMarkdown(AppState& app, const std::string& markdown, float width, const ImVec4& color, float font_size)
{
    ScopedFont base(g_font_body, font_size);
    const float pixel_size = ImGui::GetFontSize();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    std::istringstream lines(markdown);
    std::string line;
    std::string code;
    std::string language;
    bool in_code = false;
    bool first_block = true;
    int code_index = 0;
    while (std::getline(lines, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const std::string trimmed = TrimCopy(line);
        if (trimmed.rfind("```", 0) == 0)
        {
            if (in_code)
            {
                DrawMarkdownCodeBlock(app, code, language, code_index++, width);
                code.clear();
                language.clear();
                in_code = false;
            }
            else
            {
                language = TrimCopy(trimmed.substr(3));
                in_code = true;
            }
            continue;
        }
        if (in_code)
        {
            if (!code.empty())
                code.push_back('\n');
            code += line;
            continue;
        }
        if (trimmed.empty())
        {
            ImGui::Dummy(ImVec2(0.0f, Px(1.0f)));
            continue;
        }
        if (IsMarkdownRule(trimmed))
        {
            const ImVec2 start = ImGui::GetCursorScreenPos();
            draw_list->AddLine(ImVec2(start.x, start.y + Px(6.0f)), ImVec2(start.x + width, start.y + Px(6.0f)), Col(kBorder));
            ImGui::Dummy(ImVec2(width, Px(12.0f)));
            continue;
        }

        size_t heading_level = 0;
        while (heading_level < trimmed.size() && heading_level < 3 && trimmed[heading_level] == '#')
            ++heading_level;
        if (heading_level > 0 && heading_level < trimmed.size() && trimmed[heading_level] == ' ')
        {
            if (!first_block)
                ImGui::Dummy(ImVec2(0.0f, Px(4.0f)));
            const float scale = heading_level == 1 ? 1.30f : (heading_level == 2 ? 1.16f : 1.04f);
            DrawInlineFlow(trimmed.substr(heading_level + 1), width, kInk, g_font_bold, std::floor(pixel_size * scale));
            first_block = false;
            continue;
        }
        first_block = false;

        if (trimmed.front() == '>')
        {
            const ImVec2 start = ImGui::GetCursorScreenPos();
            const float indent = Px(16.0f);
            ImGui::Indent(indent);
            DrawInlineFlow(TrimCopy(trimmed.substr(1)), width - indent, Mix(color, kMuted, 0.35f), g_font_italic, pixel_size);
            ImGui::Unindent(indent);
            draw_list->AddRectFilled(ImVec2(start.x, start.y), ImVec2(start.x + Px(3.0f), ImGui::GetItemRectMax().y), Col(kAccent, 0.7f), Px(1.5f));
            continue;
        }

        size_t list_offset = std::string::npos;
        std::string list_marker;
        if (trimmed.rfind("- ", 0) == 0 || trimmed.rfind("* ", 0) == 0 || trimmed.rfind("+ ", 0) == 0)
        {
            list_offset = 2;
        }
        else
        {
            size_t digit_end = 0;
            while (digit_end < trimmed.size() && std::isdigit(static_cast<unsigned char>(trimmed[digit_end])))
                ++digit_end;
            if (digit_end > 0 && digit_end + 1 < trimmed.size() && trimmed[digit_end] == '.' && trimmed[digit_end + 1] == ' ')
            {
                list_offset = digit_end + 2;
                list_marker = trimmed.substr(0, digit_end + 1);
            }
        }
        if (list_offset != std::string::npos)
        {
            const size_t leading = std::min<size_t>(line.find_first_not_of(" \t") / 2, 4);
            const float nest = Px(18.0f) * static_cast<float>(leading);
            const float indent = nest + Px(22.0f);
            const ImVec2 start = ImGui::GetCursorScreenPos();
            if (list_marker.empty())
                draw_list->AddCircleFilled(ImVec2(start.x + nest + Px(7.0f), start.y + pixel_size * 0.55f), Px(2.6f), Col(kAccent));
            else
                draw_list->AddText(ImVec2(start.x + nest, start.y), Col(kAccentHover), list_marker.c_str());
            ImGui::Indent(indent);
            DrawInlineFlow(trimmed.substr(list_offset), width - indent, color, g_font_body, pixel_size);
            ImGui::Unindent(indent);
            continue;
        }

        DrawInlineFlow(line, width, color, g_font_body, pixel_size);
    }
    if (in_code)
        DrawMarkdownCodeBlock(app, code, language, code_index, width);
}

// -------------------------------------------------------------------------------------------------
// Navigation rail and header
// -------------------------------------------------------------------------------------------------

static bool NavItem(const char* id, const char* icon, const char* label, const char* shortcut, bool active, int badge)
{
    const ImVec2 size(std::max(Px(40.0f), ImGui::GetContentRegionAvail().x), Px(38.0f));
    const bool pressed = ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    if (hovered)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    if (active)
    {
        draw_list->AddRectFilled(min, max, Col(kAccent, 0.13f), Px(9.0f));
        draw_list->AddRectFilled(ImVec2(min.x, min.y + Px(10.0f)), ImVec2(min.x + Px(3.0f), max.y - Px(10.0f)), Col(kAccent), Px(1.5f));
    }
    else if (hovered)
    {
        draw_list->AddRectFilled(min, max, Col(kSurfaceRaised), Px(9.0f));
    }

    ScopedFont scoped(g_font_semibold, kFontBody - 0.5f);
    const float text_y = min.y + std::floor((size.y - ImGui::GetFontSize()) * 0.5f);
    float x = min.x + Px(14.0f);
    const char* glyph = Icon(icon);
    if (glyph[0] != '\0')
    {
        draw_list->AddText(ImVec2(x, text_y), Col(active ? kAccentHover : kMuted), glyph);
        x += Px(26.0f);
    }
    draw_list->AddText(ImVec2(x, text_y), Col(active || hovered ? kInk : kInkSoft), label);

    if (badge > 0)
    {
        const std::string count = std::to_string(badge);
        const ImVec2 pill = PillSize(count.c_str());
        DrawPill(draw_list, ImVec2(max.x - pill.x - Px(10.0f), min.y + (size.y - pill.y) * 0.5f), count.c_str(), active ? kAccentHover : kMuted);
    }
    else if (shortcut)
    {
        const ImVec2 cap = KeyCapSize(shortcut);
        DrawKeyCap(draw_list, ImVec2(max.x - cap.x - Px(10.0f), min.y + (size.y - cap.y) * 0.5f), shortcut);
    }
    return pressed;
}

static void DrawEngineCard(AppState& app)
{
    const bool has_server = !app.server_path.empty();
    const ImVec4 state_color = app.run_busy ? kAccent : (has_server ? kSuccess : kWarning);
    const char* state_label = app.run_busy ? "running" : (has_server ? "ready" : "missing");

    BeginCard("##EngineCard", ImVec2(0.0f, Px(150.0f)), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse, ImVec2(Px(12.0f), Px(12.0f)), kSurface);
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    SectionLabel("ENGINE");
    const ImVec2 state_size = PillSize(state_label);
    SameLineRight(state_size.x, right);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Px(3.0f));
    Pill(state_label, state_color);

    const ImVec2 row = ImGui::GetCursorScreenPos();
    StatusDot(ImGui::GetWindowDrawList(), ImVec2(row.x + Px(4.0f), row.y + Px(9.0f)), Px(3.5f), state_color, app.run_busy);
    ImGui::SetCursorScreenPos(ImVec2(row.x + Px(16.0f), row.y));
    TextWithFont(g_font_semibold, kFontBody - 0.5f, kInk, "llama.cpp server");
    {
        ScopedFont scoped(g_font_mono, kFontCaption);
        const std::string file = has_server ? WideToUtf8(std::filesystem::path(app.server_path).filename().wstring()) : "llama-server.exe not found";
        TextColoredUnformatted(has_server ? kMuted : kWarning, Ellipsize(file, ImGui::GetContentRegionAvail().x).c_str());
        if (has_server)
            ImGui::SetItemTooltip("%s", WideToUtf8(app.server_path).c_str());
    }
    VerticalSpace(2.0f);
    ImGui::BeginDisabled(app.run_busy);
    if (UiButton("##LocateServer", ICON_FOLDER, has_server ? "Change binary" : "Locate llama-server", has_server ? ButtonKind::Ghost : ButtonKind::Secondary, ImVec2(-1.0f, Px(30.0f)), kFontSmall))
        LocateServer(app);
    ImGui::EndDisabled();
    EndCard();
}

static void DrawRail(AppState& app)
{
    // Brand
    DrawLogoTile(Px(36.0f));
    ImGui::SameLine(0.0f, Px(10.0f));
    ImGui::BeginGroup();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, Px(1.0f)));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Px(1.0f));
    TextWithFont(g_font_bold, 16.5f, kInk, "Renaro");
    TextWithFont(g_font_body, kFontCaption, kMuted, "Model Playground");
    ImGui::PopStyleVar();
    ImGui::EndGroup();

    VerticalSpace(14.0f);
    SectionLabel("WORKSPACE");
    VerticalSpace(0.0f);

    struct NavEntry { Pane pane; const char* icon; const char* label; const char* shortcut; int badge; };
    const NavEntry entries[] = {
        { Pane::Playground, ICON_CHAT, "Playground", "1", 0 },
        { Pane::Diagnostics, ICON_DIAG, "Diagnostics", "2", static_cast<int>(app.comparison_results.size()) },
        { Pane::Trace, ICON_HISTORY, "Trace log", "3", 0 },
        { Pane::Settings, ICON_SETTINGS, "Settings", "4", 0 }
    };
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, Px(3.0f)));
    for (const NavEntry& entry : entries)
    {
        ImGui::PushID(static_cast<int>(entry.pane));
        if (NavItem("##Nav", entry.icon, entry.label, entry.shortcut, app.pane == entry.pane, entry.badge))
            app.pane = entry.pane;
        ImGui::PopID();
    }
    ImGui::PopStyleVar();

    VerticalSpace(10.0f);
    SectionLabel("LIBRARY");
    const size_t selected = SelectedModelCount(app);
    {
        ScopedFont scoped(g_font_body, kFontSmall);
        ImGui::TextColored(kInkSoft, "%zu model%s imported", app.models.size(), app.models.size() == 1 ? "" : "s");
        ImGui::TextColored(selected > 0 ? kAccentHover : kMuted, "%zu selected for the next run", selected);
    }

    const float footer_height = Px(150.0f);
    const float footer_y = std::max(ImGui::GetCursorPosY() + Px(8.0f), ImGui::GetWindowHeight() - footer_height - ImGui::GetStyle().WindowPadding.y);
    ImGui::SetCursorPosY(footer_y);
    DrawEngineCard(app);
}

static void StatusPillState(const AppState& app, ImVec4& color, std::string& label, bool& pulsing)
{
    pulsing = false;
    if (app.run_busy)
    {
        color = app.stop_requested ? kWarning : kAccent;
        label = app.stop_requested ? "Stopping" : app.backend.Status();
        pulsing = true;
    }
    else if (app.server_path.empty())
    {
        color = kWarning;
        label = "llama-server not located";
    }
    else if (app.has_latest_result && app.latest_result.cancelled)
    {
        color = kMuted;
        label = "Stopped";
    }
    else if (app.has_latest_result && !app.latest_result.ok)
    {
        color = kError;
        label = "Last run failed";
    }
    else if (app.telemetry_sampled)
    {
        color = kSuccess;
        label = "Run complete";
    }
    else
    {
        color = kMuted;
        label = "Ready";
    }
}

static void DrawHeader(AppState& app)
{
    struct PageInfo { const char* title; const char* subtitle; };
    PageInfo page = { "Playground", "One prompt, every selected model, measured on this machine." };
    if (app.pane == Pane::Diagnostics)
        page = { "Diagnostics", "Throughput, token accounting and resources for each model in the last run." };
    else if (app.pane == Pane::Trace)
        page = { "Trace log", "The llama.cpp event sequence recorded for every model." };
    else if (app.pane == Pane::Settings)
        page = { "Settings", "Engine and sampling controls passed to llama-server for every run." };

    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = Px(52.0f);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    ImGui::BeginGroup();
    TextWithFont(g_font_bold, kFontTitle, kInk, page.title);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Px(5.0f));
    TextWithFont(g_font_body, kFontSmall, kMuted, page.subtitle);
    ImGui::EndGroup();

    // Right cluster, laid out right-to-left.
    float right = start.x + width;
    const float control_height = Px(34.0f);
    const float control_y = start.y + (height - control_height) * 0.5f - Px(2.0f);

    if (app.pane == Pane::Playground)
    {
        ScopedFont scoped(g_font_semibold, kFontSmall + 0.5f);
        const float clear_width = (Icon(ICON_DELETE)[0] != '\0' ? Px(21.0f) : 0.0f) + ImGui::CalcTextSize("Clear").x + Px(28.0f);
        right -= clear_width;
        ImGui::SetCursorScreenPos(ImVec2(right, control_y));
        ImGui::BeginDisabled(app.run_busy);
        if (UiButton("##ClearPlayground", ICON_DELETE, "Clear", ButtonKind::Ghost, ImVec2(clear_width, control_height)))
            ClearPlayground(app, true);
        ImGui::EndDisabled();
        right -= Px(8.0f);
    }

    // Command search field.
    const float search_width = Px(212.0f);
    right -= search_width;
    ImGui::SetCursorScreenPos(ImVec2(right, control_y));
    if (ImGui::InvisibleButton("##OpenPalette", ImVec2(search_width, control_height)))
        OpenCommandPalette(app);
    {
        const bool hovered = ImGui::IsItemHovered();
        if (hovered)
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        draw_list->AddRectFilled(min, max, Col(hovered ? kSurfaceRaised : kInset), Px(9.0f));
        draw_list->AddRect(min, max, Col(hovered ? kBorder : kBorderSoft), Px(9.0f));
        ScopedFont scoped(g_font_body, kFontSmall);
        const float text_y = min.y + std::floor((control_height - ImGui::GetFontSize()) * 0.5f);
        float x = min.x + Px(12.0f);
        const char* glyph = Icon(ICON_SEARCH);
        if (glyph[0] != '\0')
        {
            draw_list->AddText(ImVec2(x, text_y), Col(kMuted), glyph);
            x += Px(22.0f);
        }
        draw_list->AddText(ImVec2(x, text_y), Col(kMuted), "Search actions");
        const ImVec2 cap = KeyCapSize("Ctrl K");
        DrawKeyCap(draw_list, ImVec2(max.x - cap.x - Px(8.0f), min.y + (control_height - cap.y) * 0.5f), "Ctrl K");
    }
    right -= Px(10.0f);

    // Run status.
    ImVec4 status_color;
    std::string status_label;
    bool pulsing = false;
    StatusPillState(app, status_color, status_label, pulsing);
    {
        ScopedFont scoped(g_font_semibold, kFontSmall);
        const float max_status_width = std::max(Px(80.0f), right - (start.x + Px(420.0f)));
        status_label = Ellipsize(status_label, max_status_width - Px(36.0f));
        const float status_width = ImGui::CalcTextSize(status_label.c_str()).x + Px(36.0f);
        if (right - status_width > start.x + Px(320.0f))
        {
            right -= status_width;
            const ImVec2 min(right, control_y);
            const ImVec2 max(right + status_width, control_y + control_height);
            draw_list->AddRectFilled(min, max, Col(status_color, 0.10f), control_height * 0.5f);
            draw_list->AddRect(min, max, Col(status_color, 0.28f), control_height * 0.5f);
            StatusDot(draw_list, ImVec2(min.x + Px(15.0f), min.y + control_height * 0.5f), Px(3.5f), status_color, pulsing);
            draw_list->AddText(ImVec2(min.x + Px(26.0f), min.y + std::floor((control_height - ImGui::GetFontSize()) * 0.5f)), Col(Mix(status_color, kInk, 0.35f)), status_label.c_str());
        }
    }

    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + height));
    ImGui::Dummy(ImVec2(width, Px(4.0f)));
}

// -------------------------------------------------------------------------------------------------
// Playground: model library
// -------------------------------------------------------------------------------------------------

static void DrawCheckBox(ImDrawList* draw_list, const ImVec2& min, float size, bool checked, bool hovered)
{
    const ImVec2 max = min + ImVec2(size, size);
    if (checked)
    {
        draw_list->AddRectFilled(min, max, Col(kAccent), Px(5.0f));
        draw_list->PathLineTo(ImVec2(min.x + size * 0.24f, min.y + size * 0.52f));
        draw_list->PathLineTo(ImVec2(min.x + size * 0.43f, min.y + size * 0.70f));
        draw_list->PathLineTo(ImVec2(min.x + size * 0.77f, min.y + size * 0.32f));
        draw_list->PathStroke(Col(kAccentInk), Px(2.0f));
    }
    else
    {
        draw_list->AddRectFilled(min, max, Col(kInset), Px(5.0f));
        draw_list->AddRect(min, max, Col(hovered ? kMuted : kBorder), Px(5.0f), Px(1.2f));
    }
}

static void DrawModelRow(AppState& app, size_t index)
{
    const ModelEntry& model = app.models[index];
    const ImVec4 series = SeriesColor(index);
    const float width = std::max(Px(80.0f), ImGui::GetContentRegionAvail().x);
    const float height = Px(58.0f);
    ImGui::PushID(static_cast<int>(index));
    const bool clicked = ImGui::InvisibleButton("##ModelRow", ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    if (hovered)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    if (model.selected)
    {
        draw_list->AddRectFilled(min, max, Col(kAccent, hovered ? 0.14f : 0.09f), Px(10.0f));
        draw_list->AddRect(min, max, Col(kAccent, 0.38f), Px(10.0f));
    }
    else
    {
        draw_list->AddRectFilled(min, max, Col(hovered ? kSurfaceRaised : kInset), Px(10.0f));
        draw_list->AddRect(min, max, Col(hovered ? kBorder : kBorderSoft), Px(10.0f));
    }

    const float box = Px(18.0f);
    DrawCheckBox(draw_list, ImVec2(min.x + Px(12.0f), min.y + (height - box) * 0.5f), box, model.selected, hovered);

    const bool running = app.run_busy && app.streaming_model == model.name;
    const LlamaRunResult* result = FindResult(app, model.name);
    std::string badge;
    ImVec4 badge_color = series;
    if (running)
    {
        badge = "live";
        badge_color = kAccent;
    }
    else if (result && result->ok && result->decode_tokens_per_second > 0.0)
    {
        badge = FormatNumber(result->decode_tokens_per_second, 1) + " t/s";
    }
    else if (result && result->cancelled)
    {
        badge = "stopped";
        badge_color = kMuted;
    }
    else if (result && !result->ok)
    {
        badge = "error";
        badge_color = kError;
    }
    const float badge_width = badge.empty() ? 0.0f : PillSize(badge.c_str()).x + Px(8.0f);

    const float text_x = min.x + Px(42.0f);
    const float text_width = max.x - text_x - Px(10.0f) - badge_width;
    {
        ScopedFont scoped(g_font_semibold, kFontBody - 0.5f);
        draw_list->AddCircleFilled(ImVec2(text_x + Px(3.0f), min.y + Px(18.5f)), Px(3.0f), Col(series));
        const std::string name = Ellipsize(model.name, text_width - Px(12.0f));
        draw_list->AddText(ImVec2(text_x + Px(12.0f), min.y + Px(10.0f)), Col(kInk), name.c_str());
    }
    {
        ScopedFont scoped(g_font_body, kFontCaption);
        const std::string detail = Ellipsize(model.detail, text_width);
        draw_list->AddText(ImVec2(text_x, min.y + Px(32.0f)), Col(kMuted), detail.c_str());
    }
    if (!badge.empty())
    {
        const ImVec2 pill = PillSize(badge.c_str());
        const ImVec2 pill_pos(max.x - pill.x - Px(10.0f), min.y + Px(10.0f));
        if (running)
            StatusDot(draw_list, ImVec2(pill_pos.x - Px(8.0f), pill_pos.y + pill.y * 0.5f), Px(3.0f), kAccent, true);
        DrawPill(draw_list, pill_pos, badge.c_str(), badge_color);
    }
    if (hovered)
        ImGui::SetItemTooltip("%s", WideToUtf8(model.path).c_str());
    if (clicked)
        ToggleModel(app, index);
    ImGui::PopID();
}

static void DrawModelsPanel(AppState& app)
{
    const size_t selected = SelectedModelCount(app);
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    TextWithFont(g_font_semibold, kFontBody + 0.5f, kInk, "Models");
    if (!app.models.empty())
    {
        ImGui::SameLine(0.0f, Px(8.0f));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Px(1.0f));
        const std::string count = std::to_string(selected) + "/" + std::to_string(app.models.size());
        Pill(count.c_str(), selected > 0 ? kAccentHover : kMuted);
    }
    const float import_width = IconButtonWidth(ICON_ADD, "Add", 28.0f);
    SameLineRight(import_width, right);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Px(4.0f));
    ImGui::BeginDisabled(app.run_busy);
    if (IconButton("##ImportHeader", ICON_ADD, "Add", "Import a local .gguf model", 28.0f))
        ImportModel(app);
    ImGui::EndDisabled();

    VerticalSpace(2.0f);
    if (app.models.empty())
    {
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float height = Px(178.0f);
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        draw_list->AddRectFilled(start, start + ImVec2(width, height), Col(kInset), Px(12.0f));
        // Dashed outline.
        const float dash = Px(6.0f);
        const ImU32 dash_color = Col(kBorder);
        for (float x = start.x + Px(12.0f); x < start.x + width - Px(12.0f); x += dash * 2.0f)
        {
            draw_list->AddLine(ImVec2(x, start.y), ImVec2(std::min(x + dash, start.x + width - Px(12.0f)), start.y), dash_color);
            draw_list->AddLine(ImVec2(x, start.y + height), ImVec2(std::min(x + dash, start.x + width - Px(12.0f)), start.y + height), dash_color);
        }
        for (float y = start.y + Px(12.0f); y < start.y + height - Px(12.0f); y += dash * 2.0f)
        {
            draw_list->AddLine(ImVec2(start.x, y), ImVec2(start.x, std::min(y + dash, start.y + height - Px(12.0f))), dash_color);
            draw_list->AddLine(ImVec2(start.x + width, y), ImVec2(start.x + width, std::min(y + dash, start.y + height - Px(12.0f))), dash_color);
        }

        ImGui::SetCursorScreenPos(start + ImVec2(Px(16.0f), Px(18.0f)));
        ImGui::BeginGroup();
        TextWithFont(g_font_semibold, kFontBody, kInk, "No models yet");
        WrappedText(kMuted, "Import one or more local .gguf files. Only files you add here are shown.", kFontSmall, width - Px(32.0f));
        VerticalSpace(4.0f);
        ImGui::BeginDisabled(app.run_busy);
        if (UiButton("##ImportEmpty", ICON_ADD, "Import model", ButtonKind::Primary, ImVec2(width - Px(32.0f), Px(34.0f))))
            ImportModel(app);
        ImGui::EndDisabled();
        ImGui::EndGroup();
        ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + height));
        ImGui::Dummy(ImVec2(width, Px(1.0f)));
    }
    else
    {
        ImGui::BeginDisabled(app.run_busy);
        if (UiButton("##SelectAll", ICON_SELECT_ALL, "Select all", ButtonKind::Ghost, ImVec2(0.0f, Px(28.0f)), kFontSmall))
        {
            for (ModelEntry& model : app.models)
                model.selected = !model.path.empty();
            ShowToast(app, "All imported models selected");
        }
        ImGui::SameLine(0.0f, Px(4.0f));
        if (UiButton("##ClearSelection", ICON_CANCEL, "Clear", ButtonKind::Ghost, ImVec2(0.0f, Px(28.0f)), kFontSmall))
        {
            for (ModelEntry& model : app.models)
                model.selected = false;
            ResetTelemetry(app);
            ShowToast(app, "Model selection cleared");
        }
        VerticalSpace(0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Px(8.0f), Px(6.0f)));
        for (size_t index = 0; index < app.models.size(); ++index)
            DrawModelRow(app, index);
        ImGui::PopStyleVar();
        VerticalSpace(2.0f);
        if (UiButton("##ImportList", ICON_ADD, "Import another model", ButtonKind::Secondary, ImVec2(-1.0f, Px(34.0f))))
            ImportModel(app);
        ImGui::EndDisabled();
    }

    VerticalSpace(10.0f);
    const ImVec2 note = ImGui::GetCursorScreenPos();
    const char* info = Icon(ICON_INFO);
    float indent = 0.0f;
    if (info[0] != '\0')
    {
        ScopedFont scoped(g_font_body, kFontSmall);
        ImGui::GetWindowDrawList()->AddText(ImVec2(note.x, note.y + Px(1.0f)), Col(kFaint), info);
        indent = Px(20.0f);
    }
    ImGui::Indent(indent);
    WrappedText(kMuted, "Selected models run one at a time on a fresh llama-server, so memory and timings stay comparable.", kFontCaption);
    ImGui::Unindent(indent);
}

// -------------------------------------------------------------------------------------------------
// Playground: conversation
// -------------------------------------------------------------------------------------------------

static void DrawUserBubble(const Message& message, float width)
{
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    float caption_height = 0.0f;
    {
        ScopedFont scoped(g_font_semibold, kFontCaption);
        const float caption_width = ImGui::CalcTextSize("You").x;
        draw_list->AddText(ImVec2(start.x + width - caption_width - Px(6.0f), start.y), Col(kMuted), "You");
        caption_height = ImGui::GetFontSize() + Px(6.0f);
    }
    ScopedFont scoped(g_font_body, kFontBody);
    const ImVec2 padding(Px(16.0f), Px(11.0f));
    const float max_text = std::max(Px(120.0f), width * 0.78f - padding.x * 2.0f);
    const char* text = message.text.c_str();
    const ImVec2 text_size = ImGui::CalcTextSize(text, nullptr, false, max_text);
    const ImVec2 bubble(text_size.x + padding.x * 2.0f, text_size.y + padding.y * 2.0f);
    const ImVec2 min(start.x + width - bubble.x, start.y + caption_height);
    draw_list->AddRectFilled(min, min + bubble, Col(Mix(kSurfaceRaised, kAccent, 0.20f)), Px(14.0f));
    draw_list->AddRect(min, min + bubble, Col(kAccent, 0.30f), Px(14.0f));
    draw_list->AddText(ImGui::GetFont(), ImGui::GetFontSize(), min + padding, Col(kInk), text, nullptr, max_text);
    ImGui::Dummy(ImVec2(width, caption_height + bubble.y));
}

static void DrawMessageCard(AppState& app, const Message& message, float width)
{
    const bool reasoning = message.kind == MessageKind::Reasoning;
    const bool error = message.kind == MessageKind::Error;
    const bool streaming = message.annotation.rfind("streaming", 0) == 0;
    const ImVec4 series = error ? kError : (reasoning ? kViolet : ModelColor(app, message.role));
    const float pad = Px(16.0f);
    const float inner_width = width - pad * 2.0f;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    // Read by value: a pointer into window storage could be invalidated by later insertions.
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID open_id = ImGui::GetID("##ReasoningOpen");
    bool open = storage->GetBool(open_id, true);

    draw_list->ChannelsSplit(2);
    draw_list->ChannelsSetCurrent(1);
    ImGui::SetCursorScreenPos(start + ImVec2(pad, pad));
    ImGui::BeginGroup();

    // Header: avatar, title, secondary label, actions.
    const float avatar = Px(28.0f);
    const ImVec2 header = ImGui::GetCursorScreenPos();
    const ImVec2 avatar_center = header + ImVec2(avatar * 0.5f, avatar * 0.5f);
    draw_list->AddCircleFilled(avatar_center, avatar * 0.5f, Col(series, 0.16f));
    draw_list->AddCircle(avatar_center, avatar * 0.5f, Col(series, 0.35f));
    {
        const char* glyph = reasoning ? Icon(ICON_BULB) : (error ? Icon(ICON_WARNING) : "");
        std::string initial;
        if (glyph[0] == '\0')
        {
            for (char character : message.role)
            {
                if (std::isalnum(static_cast<unsigned char>(character)))
                {
                    initial.assign(1, static_cast<char>(std::toupper(static_cast<unsigned char>(character))));
                    break;
                }
            }
            if (initial.empty())
                initial = "R";
            glyph = initial.c_str();
        }
        ScopedFont scoped(g_font_bold, kFontSmall);
        const ImVec2 glyph_size = ImGui::CalcTextSize(glyph);
        draw_list->AddText(avatar_center - glyph_size * 0.5f, Col(Mix(series, kInk, 0.2f)), glyph);
    }

    float actions_width = 0.0f;
    const bool can_copy = !error && !message.text.empty();
    if (can_copy)
        actions_width += IconButtonWidth(ICON_COPY, "Copy", 26.0f);
    if (reasoning)
        actions_width += IconButtonWidth(open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, open ? "Hide" : "Show", 26.0f) + Px(4.0f);

    const float title_x = header.x + avatar + Px(10.0f);
    const float title_space = header.x + inner_width - actions_width - Px(8.0f) - title_x;
    float title_end = title_x;
    {
        ScopedFont scoped(g_font_semibold, kFontBody);
        const std::string title = Ellipsize(reasoning ? std::string("Reasoning") : (error ? std::string("Run failed") : message.role), title_space);
        const float title_y = header.y + std::floor((avatar - ImGui::GetFontSize()) * 0.5f);
        draw_list->AddText(ImVec2(title_x, title_y), Col(kInk), title.c_str());
        title_end = title_x + ImGui::CalcTextSize(title.c_str()).x;
    }
    if (reasoning || error || streaming)
    {
        const float remaining = header.x + inner_width - actions_width - Px(8.0f) - (title_end + Px(10.0f));
        if (streaming)
        {
            const ImVec2 pill = PillSize("streaming");
            if (remaining > pill.x)
                DrawPill(draw_list, ImVec2(title_end + Px(10.0f), header.y + (avatar - pill.y) * 0.5f), "streaming", kAccent, ICON_BOLT);
        }
        else if (remaining > Px(30.0f))
        {
            ScopedFont scoped(g_font_body, kFontSmall);
            const std::string secondary = Ellipsize(message.role, remaining);
            draw_list->AddText(ImVec2(title_end + Px(10.0f), header.y + std::floor((avatar - ImGui::GetFontSize()) * 0.5f)), Col(kMuted), secondary.c_str());
        }
    }

    float action_x = header.x + inner_width - actions_width;
    const float action_y = header.y + (avatar - Px(26.0f)) * 0.5f;
    if (reasoning)
    {
        ImGui::SetCursorScreenPos(ImVec2(action_x, action_y));
        if (IconButton("##ToggleReasoning", open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, open ? "Hide" : "Show", open ? "Collapse reasoning" : "Expand reasoning", 26.0f))
        {
            open = !open;
            storage->SetBool(open_id, open);
        }
        action_x = ImGui::GetItemRectMax().x + Px(4.0f);
    }
    if (can_copy)
    {
        ImGui::SetCursorScreenPos(ImVec2(action_x, action_y));
        if (IconButton("##CopyMessage", ICON_COPY, "Copy", "Copy response", 26.0f))
        {
            ImGui::SetClipboardText(message.text.c_str());
            ShowToast(app, "Response copied");
        }
    }
    ImGui::SetCursorScreenPos(header);
    ImGui::Dummy(ImVec2(inner_width, avatar));
    VerticalSpace(0.0f);

    // Body.
    if (error)
    {
        WrappedText(Mix(kError, kInk, 0.45f), message.text.c_str(), kFontBody - 0.5f, inner_width);
    }
    else if (reasoning)
    {
        if (open)
        {
            DrawMarkdown(app, message.text, inner_width, Mix(kInkSoft, kMuted, 0.45f), kFontSmall + 0.5f);
        }
        else
        {
            ScopedFont scoped(g_font_italic, kFontSmall + 0.5f);
            const size_t words = static_cast<size_t>(std::count(message.text.begin(), message.text.end(), ' ')) + (message.text.empty() ? 0u : 1u);
            const std::string summary = std::to_string(words) + " words of reasoning hidden";
            TextColoredUnformatted(kFaint, summary.c_str());
        }
    }
    else if (message.text.empty())
    {
        TextWithFont(g_font_italic, kFontSmall + 0.5f, kMuted, streaming ? "Waiting for the first token..." : "No text was returned.");
    }
    else
    {
        DrawMarkdown(app, message.text, inner_width, Mix(kInk, kInkSoft, 0.35f), kFontBody);
    }

    if (!streaming && !error && !reasoning && !message.annotation.empty())
    {
        VerticalSpace(0.0f);
        const ImVec2 rule = ImGui::GetCursorScreenPos();
        draw_list->AddLine(rule, ImVec2(rule.x + inner_width, rule.y), Col(kBorderSoft));
        VerticalSpace(2.0f);
        ScopedFont scoped(g_font_body, kFontCaption);
        const char* glyph = Icon(ICON_BOLT);
        if (glyph[0] != '\0')
        {
            TextColoredUnformatted(kFaint, glyph);
            ImGui::SameLine(0.0f, Px(6.0f));
        }
        TextColoredUnformatted(kMuted, Ellipsize(message.annotation, inner_width - Px(20.0f)).c_str());
    }
    ImGui::EndGroup();

    const float height = ImGui::GetItemRectMax().y - start.y + pad;
    draw_list->ChannelsSetCurrent(0);
    const ImVec4 background = error ? Mix(kSurface, kError, 0.07f) : (reasoning ? Mix(kSurface, kViolet, 0.035f) : kSurface);
    draw_list->AddRectFilled(start, start + ImVec2(width, height), Col(background), Px(12.0f));
    draw_list->AddRect(start, start + ImVec2(width, height), Col(error ? WithAlpha(kError, 0.35f) : kBorderSoft), Px(12.0f));
    draw_list->AddRectFilled(ImVec2(start.x, start.y + Px(16.0f)), ImVec2(start.x + Px(3.0f), start.y + height - Px(16.0f)), Col(series, 0.9f), Px(1.5f));
    draw_list->ChannelsMerge();

    ImGui::SetCursorScreenPos(start);
    ImGui::Dummy(ImVec2(width, height));
}

static void DrawTypingIndicator(AppState& app, float width)
{
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float height = Px(46.0f);
    draw_list->AddRectFilled(start, start + ImVec2(width, height), Col(kSurface), Px(12.0f));
    draw_list->AddRect(start, start + ImVec2(width, height), Col(kAccent, 0.22f), Px(12.0f));

    const float time = static_cast<float>(ImGui::GetTime());
    for (int dot = 0; dot < 3; ++dot)
    {
        const float wave = 0.5f + 0.5f * std::sin(time * 6.0f - static_cast<float>(dot) * 0.8f);
        const ImVec2 center(start.x + Px(22.0f) + static_cast<float>(dot) * Px(11.0f), start.y + height * 0.5f - wave * Px(3.0f));
        draw_list->AddCircleFilled(center, Px(3.2f), Col(kAccentHover, 0.35f + 0.65f * wave));
    }

    const std::string model = app.streaming_model.empty() ? std::string("llama.cpp") : app.streaming_model;
    float x = start.x + Px(62.0f);
    {
        ScopedFont scoped(g_font_semibold, kFontSmall + 0.5f);
        const std::string label = app.stop_requested ? "Stopping " + model : "Generating with " + model;
        const std::string fitted = Ellipsize(label, width * 0.55f);
        draw_list->AddText(ImVec2(x, start.y + std::floor((height - ImGui::GetFontSize()) * 0.5f)), Col(kInk), fitted.c_str());
        x += ImGui::CalcTextSize(fitted.c_str()).x + Px(10.0f);
    }
    {
        ScopedFont scoped(g_font_body, kFontSmall);
        const std::string status = Ellipsize(app.backend.Status(), start.x + width - Px(16.0f) - x);
        draw_list->AddText(ImVec2(x, start.y + std::floor((height - ImGui::GetFontSize()) * 0.5f)), Col(kMuted), status.c_str());
    }
    ImGui::Dummy(ImVec2(width, height));
}

static void DrawWelcomeStep(AppState& app, int number, const char* title, const char* body, bool done, const char* action_id, const char* action_label, float width)
{
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float height = Px(64.0f);
    draw_list->AddRectFilled(start, start + ImVec2(width, height), Col(kSurface), Px(12.0f));
    draw_list->AddRect(start, start + ImVec2(width, height), Col(done ? WithAlpha(kSuccess, 0.30f) : kBorderSoft), Px(12.0f));

    const ImVec2 badge_center(start.x + Px(30.0f), start.y + height * 0.5f);
    if (done)
    {
        draw_list->AddCircleFilled(badge_center, Px(13.0f), Col(kSuccess, 0.18f));
        draw_list->PathLineTo(badge_center + ImVec2(-Px(5.0f), Px(0.5f)));
        draw_list->PathLineTo(badge_center + ImVec2(-Px(1.5f), Px(4.0f)));
        draw_list->PathLineTo(badge_center + ImVec2(Px(5.5f), -Px(3.5f)));
        draw_list->PathStroke(Col(kSuccess), Px(2.0f));
    }
    else
    {
        draw_list->AddCircleFilled(badge_center, Px(13.0f), Col(kAccent, 0.14f));
        ScopedFont scoped(g_font_bold, kFontSmall);
        const std::string label = std::to_string(number);
        const ImVec2 size = ImGui::CalcTextSize(label.c_str());
        draw_list->AddText(badge_center - size * 0.5f, Col(kAccentHover), label.c_str());
    }

    float action_width = 0.0f;
    if (!done && action_id)
    {
        ScopedFont scoped(g_font_semibold, kFontSmall);
        action_width = ImGui::CalcTextSize(action_label).x + Px(28.0f);
        ImGui::SetCursorScreenPos(ImVec2(start.x + width - action_width - Px(14.0f), start.y + (height - Px(30.0f)) * 0.5f));
        ImGui::PushID(number);
        if (UiButton(action_id, nullptr, action_label, ButtonKind::Secondary, ImVec2(action_width, Px(30.0f)), kFontSmall))
        {
            if (number == 1)
                ImportModel(app);
            else if (number == 3)
                LocateServer(app);
        }
        ImGui::PopID();
    }

    const float text_x = start.x + Px(56.0f);
    const float text_width = width - Px(56.0f) - action_width - Px(28.0f);
    {
        ScopedFont scoped(g_font_semibold, kFontBody - 0.5f);
        draw_list->AddText(ImVec2(text_x, start.y + Px(13.0f)), Col(done ? kInkSoft : kInk), Ellipsize(title, text_width).c_str());
    }
    {
        ScopedFont scoped(g_font_body, kFontSmall);
        draw_list->AddText(ImVec2(text_x, start.y + Px(34.0f)), Col(kMuted), Ellipsize(body, text_width).c_str());
    }
    ImGui::SetCursorScreenPos(start);
    ImGui::Dummy(ImVec2(width, height));
}

static void DrawWelcome(AppState& app, float width)
{
    const float base_x = ImGui::GetCursorPosX();
    VerticalSpace(26.0f);
    const float tile = Px(54.0f);
    ImGui::SetCursorPosX(base_x + (width - tile) * 0.5f);
    DrawLogoTile(tile);
    VerticalSpace(6.0f);
    {
        ScopedFont scoped(g_font_bold, kFontHero);
        const char* title = "Compare local models side by side";
        ImGui::SetCursorPosX(base_x + std::max(0.0f, (width - ImGui::CalcTextSize(title).x) * 0.5f));
        TextColoredUnformatted(kInk, title);
    }
    {
        ScopedFont scoped(g_font_body, kFontBody);
        const char* subtitle = "Send one prompt to every selected GGUF model and measure the difference.";
        ImGui::SetCursorPosX(base_x + std::max(0.0f, (width - ImGui::CalcTextSize(subtitle).x) * 0.5f));
        TextColoredUnformatted(kMuted, subtitle);
    }
    VerticalSpace(14.0f);

    const float steps_width = std::min(width, Px(600.0f));
    const float steps_x = base_x + (width - steps_width) * 0.5f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Px(10.0f), Px(10.0f)));
    ImGui::SetCursorPosX(steps_x);
    DrawWelcomeStep(app, 1, "Import GGUF models", "Add local .gguf files to the library on the left.", !app.models.empty(), "##StepImport", "Import", steps_width);
    ImGui::SetCursorPosX(steps_x);
    DrawWelcomeStep(app, 2, "Choose what to compare", "Tick every model that should answer the prompt.", SelectedModelCount(app) > 0, nullptr, nullptr, steps_width);
    ImGui::SetCursorPosX(steps_x);
    DrawWelcomeStep(app, 3, "Locate llama-server", "Point Renaro at the llama.cpp server binary.", !app.server_path.empty(), "##StepLocate", "Locate", steps_width);
    ImGui::PopStyleVar();

    VerticalSpace(12.0f);
    {
        ScopedFont scoped(g_font_semibold, kFontCaption);
        const char* label = "TRY A PROMPT";
        ImGui::SetCursorPosX(base_x + (width - ImGui::CalcTextSize(label).x) * 0.5f);
        TextColoredUnformatted(kMuted, label);
    }
    static const char* suggestions[] = {
        "Explain the tradeoff between context length and decode speed.",
        "Summarize what a KV cache does in three bullet points.",
        "Write a Python function that checks whether a number is prime.",
        "Draft a short, friendly release note for a bug-fix update."
    };
    const float gap = Px(10.0f);
    const float card_width = std::max(Px(60.0f), (steps_width - gap) * 0.5f);
    const float card_height = Px(58.0f);
    for (int index = 0; index < IM_ARRAYSIZE(suggestions); ++index)
    {
        if (index % 2 == 0)
            ImGui::SetCursorPosX(steps_x);
        else
            ImGui::SameLine(0.0f, gap);
        ImGui::PushID(index);
        if (ImGui::InvisibleButton("##Suggestion", ImVec2(card_width, card_height)))
        {
            std::snprintf(app.prompt, sizeof(app.prompt), "%s", suggestions[index]);
            app.focus_prompt = true;
        }
        const bool hovered = ImGui::IsItemHovered();
        if (hovered)
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        draw_list->AddRectFilled(min, max, Col(hovered ? kSurfaceRaised : kSurface), Px(10.0f));
        draw_list->AddRect(min, max, Col(hovered ? WithAlpha(kAccent, 0.4f) : kBorderSoft), Px(10.0f));
        ScopedFont scoped(g_font_body, kFontSmall);
        const float wrap = card_width - Px(28.0f);
        const ImVec2 text_size = ImGui::CalcTextSize(suggestions[index], nullptr, false, wrap);
        draw_list->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(min.x + Px(14.0f), min.y + std::max(Px(6.0f), (card_height - text_size.y) * 0.5f)), Col(hovered ? kInk : kInkSoft), suggestions[index], nullptr, wrap);
        ImGui::PopID();
    }
}

static void DrawComposer(AppState& app, float width, float height)
{
    BeginCard("##Composer", ImVec2(width, height), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse, ImVec2(Px(14.0f), Px(12.0f)));
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    const float footer_height = Px(32.0f);

    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Px(2.0f), Px(2.0f)));
    if (app.focus_prompt)
    {
        ImGui::SetKeyboardFocusHere();
        app.focus_prompt = false;
    }
    const ImVec2 input_pos = ImGui::GetCursorScreenPos();
    const float input_height = std::max(Px(30.0f), ImGui::GetContentRegionAvail().y - footer_height - ImGui::GetStyle().ItemSpacing.y);
    bool submit = false;
    {
        ScopedFont scoped(g_font_body, kFontBody);
        // Enter submits (and releases the field so it can be cleared); Shift+Enter or Ctrl+Enter add a line.
        submit = ImGui::InputTextMultiline("##Prompt", app.prompt, sizeof(app.prompt), ImVec2(-FLT_MIN, input_height), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine);
        const bool input_active = ImGui::IsItemActive();
        if (app.prompt[0] == '\0' && !input_active)
            ImGui::GetWindowDrawList()->AddText(input_pos + ImVec2(Px(2.0f), Px(2.0f)), Col(kFaint), app.run_busy ? "Generation in progress..." : "Ask something to compare across the selected models...");
        app.prompt_focused = input_active;
    }
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);

    // Footer: run parameters at a glance, then the primary action.
    const ImVec2 footer = ImGui::GetCursorScreenPos();
    const float button_width = Px(112.0f);
    const float chips_right = right - button_width - Px(10.0f);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    {
        const size_t selected = SelectedModelCount(app);
        std::ostringstream temperature;
        temperature << "temp " << std::fixed << std::setprecision(2) << app.temperature;
        const std::string chips[] = {
            std::to_string(selected) + (selected == 1 ? " model" : " models"),
            temperature.str(),
            "ctx " + std::to_string(app.context),
            std::to_string(app.max_tokens) + " max tok"
        };
        float x = footer.x;
        for (size_t index = 0; index < sizeof(chips) / sizeof(chips[0]); ++index)
        {
            const ImVec2 size = PillSize(chips[index].c_str());
            if (x + size.x > chips_right)
                break;
            DrawPill(draw_list, ImVec2(x, footer.y + (footer_height - size.y) * 0.5f), chips[index].c_str(), index == 0 && selected == 0 ? kWarning : kMuted);
            x += size.x + Px(6.0f);
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(right - button_width, footer.y));
    if (app.run_busy)
    {
        ImGui::BeginDisabled(app.stop_requested);
        if (UiButton("##StopRun", ICON_STOP, app.stop_requested ? "Stopping" : "Stop", ButtonKind::Danger, ImVec2(button_width, footer_height)))
            StopRun(app);
        ImGui::EndDisabled();
    }
    else
    {
        if (UiButton("##StartRun", ICON_SEND, "Run", ButtonKind::Primary, ImVec2(button_width, footer_height)) || submit)
        {
            StartRun(app, app.prompt);
            app.focus_prompt = true;
        }
        ImGui::SetItemTooltip("Run the prompt on every selected model (Enter)");
    }
    EndCard();

    // Focus ring around the composer while typing.
    if (app.prompt_focused)
    {
        const ImVec2 min = ImGui::GetItemRectMin() - ImVec2(1.0f, 1.0f);
        const ImVec2 max = ImGui::GetItemRectMax() + ImVec2(1.0f, 1.0f);
        ImGui::GetWindowDrawList()->AddRect(min, max, Col(kAccent, 0.55f), Px(12.0f) + 1.0f, Px(1.5f));
    }
}

static void DrawConversation(AppState& app)
{
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float composer_height = Px(124.0f);
    const float gap = Px(12.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(6.0f), Px(2.0f)));
    ImGui::BeginChild("##Thread", ImVec2(available.x, std::max(Px(120.0f), available.y - composer_height - gap)), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_None);
    ImGui::PopStyleVar();
    const float thread_width = ImGui::GetContentRegionAvail().x;
    const float content_width = std::min(thread_width, Px(860.0f));
    const float offset = std::floor((thread_width - content_width) * 0.5f);
    if (offset > 0.0f)
        ImGui::Indent(offset);

    const bool has_dialogue = std::any_of(app.messages.begin(), app.messages.end(), [](const Message& message) { return message.kind != MessageKind::System; });
    if (!has_dialogue)
    {
        DrawWelcome(app, content_width);
    }
    else
    {
        VerticalSpace(2.0f);
        for (size_t index = 0; index < app.messages.size(); ++index)
        {
            const Message& message = app.messages[index];
            if (message.kind == MessageKind::System)
                continue;
            ImGui::PushID(static_cast<int>(index));
            if (message.kind == MessageKind::User)
                DrawUserBubble(message, content_width);
            else
                DrawMessageCard(app, message, content_width);
            ImGui::PopID();
            // Breathing room between turns.
            ImGui::Dummy(ImVec2(0.0f, Px(1.0f)));
        }
    }
    if (app.run_busy)
        DrawTypingIndicator(app, content_width);
    if (offset > 0.0f)
        ImGui::Unindent(offset);
    VerticalSpace(6.0f);

    const bool near_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - Px(80.0f);
    if (app.scroll_to_bottom || (app.run_busy && near_bottom))
    {
        ImGui::SetScrollHereY(1.0f);
        app.scroll_to_bottom = false;
    }
    ImGui::EndChild();

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + gap - ImGui::GetStyle().ItemSpacing.y);
    DrawComposer(app, available.x, composer_height);
}

// -------------------------------------------------------------------------------------------------
// Playground: insights column
// -------------------------------------------------------------------------------------------------

static void StatTile(const char* label, const std::string& value, const char* unit, const ImVec4& accent, float width)
{
    const float height = Px(74.0f);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max = min + ImVec2(width, height);
    ImGui::Dummy(ImVec2(width, height));
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(min, max, Col(kInset), Px(10.0f));
    draw_list->AddRect(min, max, Col(kBorderSoft), Px(10.0f));
    draw_list->AddCircleFilled(ImVec2(min.x + Px(14.0f), min.y + Px(17.0f)), Px(3.0f), Col(accent));
    {
        ScopedFont scoped(g_font_semibold, kFontCaption);
        draw_list->AddText(ImVec2(min.x + Px(22.0f), min.y + Px(10.0f)), Col(kMuted), Ellipsize(label, width - Px(30.0f)).c_str());
    }
    const bool empty = value == kNone;
    float value_width = 0.0f;
    float value_bottom = 0.0f;
    {
        ScopedFont scoped(g_font_bold, kFontStat);
        const ImVec2 pos(min.x + Px(12.0f), min.y + Px(30.0f));
        draw_list->AddText(pos, Col(empty ? kFaint : kInk), value.c_str());
        value_width = ImGui::CalcTextSize(value.c_str()).x;
        value_bottom = pos.y + ImGui::GetFontSize();
    }
    if (!empty && unit)
    {
        ScopedFont scoped(g_font_body, kFontCaption);
        draw_list->AddText(ImVec2(min.x + Px(16.0f) + value_width, value_bottom - ImGui::GetFontSize() - Px(3.0f)), Col(kMuted), unit);
    }
}

static void DrawThroughputChart(const AppState& app)
{
    struct Bar { std::string model; double value; ImVec4 color; };
    std::vector<Bar> bars;
    for (const LlamaRunResult& result : app.comparison_results)
    {
        if (result.ok && !result.cancelled && result.decode_tokens_per_second > 0.0)
            bars.push_back({ result.model, result.decode_tokens_per_second, ModelColor(app, result.model) });
    }
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const float width = ImGui::GetContentRegionAvail().x;
    if (bars.empty())
    {
        for (int index = 0; index < 3; ++index)
        {
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const float fraction = 0.82f - 0.22f * static_cast<float>(index);
            draw_list->AddRectFilled(pos, pos + ImVec2(width * 0.35f, Px(8.0f)), Col(kSurfaceRaised), Px(4.0f));
            draw_list->AddRectFilled(pos + ImVec2(0.0f, Px(14.0f)), pos + ImVec2(width * fraction, Px(22.0f)), Col(kSurfaceRaised), Px(4.0f));
            ImGui::Dummy(ImVec2(width, Px(24.0f)));
        }
        WrappedText(kMuted, app.run_busy ? "Waiting for the first model to finish..." : "Decode speed for each model appears here after a run.", kFontSmall);
        return;
    }

    double best = 0.0;
    for (const Bar& bar : bars)
        best = std::max(best, bar.value);
    for (size_t index = 0; index < bars.size(); ++index)
    {
        const Bar& bar = bars[index];
        const bool fastest = bars.size() > 1 && bar.value >= best;
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const std::string value = FormatNumber(bar.value, 1) + " tok/s";
        float value_width = 0.0f;
        {
            ScopedFont scoped(g_font_semibold, kFontSmall);
            value_width = ImGui::CalcTextSize(value.c_str()).x;
            draw_list->AddText(ImVec2(pos.x + width - value_width, pos.y), Col(fastest ? kSuccess : kInk), value.c_str());
        }
        {
            ScopedFont scoped(g_font_body, kFontSmall);
            draw_list->AddText(pos, Col(kInkSoft), Ellipsize(bar.model, width - value_width - Px(12.0f)).c_str());
        }
        const float track_y = pos.y + Px(22.0f);
        const float fraction = best > 0.0 ? static_cast<float>(bar.value / best) : 0.0f;
        draw_list->AddRectFilled(ImVec2(pos.x, track_y), ImVec2(pos.x + width, track_y + Px(8.0f)), Col(kInset), Px(4.0f));
        draw_list->AddRectFilled(ImVec2(pos.x, track_y), ImVec2(pos.x + std::max(Px(8.0f), width * fraction), track_y + Px(8.0f)), Col(bar.color), Px(4.0f));
        ImGui::PushID(static_cast<int>(index));
        ImGui::Dummy(ImVec2(width, Px(32.0f)));
        ImGui::PopID();
    }
}

static void KeyValueRow(const char* label, const std::string& value, const ImVec4& value_color = kInkSoft)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    {
        ScopedFont scoped(g_font_body, kFontSmall);
        TextColoredUnformatted(kMuted, label);
    }
    ImGui::TableSetColumnIndex(1);
    ScopedFont scoped(g_font_body, kFontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, value_color);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(value.empty() ? kNone : value.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

static bool BeginKeyValueTable(const char* id, float label_width)
{
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit))
        return false;
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, label_width);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

static void DrawInsights(AppState& app)
{
    const float gap = Px(12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Px(8.0f), gap));

    // Throughput tiles.
    BeginCard("##ThroughputCard", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY);
    {
        const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        CardTitle("Last result");
        const bool latest_stopped = app.has_latest_result && app.latest_result.cancelled;
        const bool latest_error = app.has_latest_result && !app.latest_result.ok && !latest_stopped;
        const char* status = app.run_busy ? "sampling" : (latest_stopped ? "stopped" : (latest_error ? "error" : (app.telemetry_sampled ? "measured" : "waiting")));
        const ImVec4 status_color = app.run_busy ? kAccent : (latest_stopped ? kMuted : (latest_error ? kError : (app.telemetry_sampled ? kSuccess : kMuted)));
        SameLineRight(PillSize(status).x, right);
        Pill(status, status_color);
        if (app.telemetry_sampled && app.has_latest_result)
        {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Px(6.0f));
            ScopedFont scoped(g_font_body, kFontCaption);
            TextColoredUnformatted(kMuted, Ellipsize(app.latest_result.model, ImGui::GetContentRegionAvail().x).c_str());
        }
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Px(8.0f), Px(8.0f)));
        const float tile_width = (ImGui::GetContentRegionAvail().x - Px(8.0f)) * 0.5f;
        StatTile("Decode", FormatNumber(app.decode_tokens_per_second, 1), "tok/s", kAccent, tile_width);
        ImGui::SameLine();
        StatTile("Prompt", FormatNumber(app.prompt_tokens_per_second, 1), "tok/s", SeriesColor(1), tile_width);
        StatTile("Prompt eval", FormatNumber(app.prompt_eval_seconds, 2), "s", SeriesColor(2), tile_width);
        ImGui::SameLine();
        StatTile("Memory", FormatNumber(app.memory_gb, 2), "GB", SeriesColor(3), tile_width);
        ImGui::PopStyleVar();
    }
    EndCard();

    // Decode speed per model.
    BeginCard("##ChartCard", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY);
    CardTitle("Decode speed by model");
    VerticalSpace(0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Px(8.0f), Px(6.0f)));
    DrawThroughputChart(app);
    ImGui::PopStyleVar();
    EndCard();

    // Signals.
    BeginCard("##SignalsCard", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY);
    {
        const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        CardTitle("llama.cpp signals");
        if (app.has_latest_result)
        {
            const char* toggle = app.show_all_signals ? "Less" : "All";
            ScopedFont scoped(g_font_semibold, kFontCaption);
            const float toggle_width = ImGui::CalcTextSize(toggle).x + Px(20.0f);
            SameLineRight(toggle_width, right);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Px(4.0f));
            if (UiButton("##ToggleSignals", nullptr, toggle, ButtonKind::Ghost, ImVec2(toggle_width, Px(24.0f)), kFontCaption))
                app.show_all_signals = !app.show_all_signals;
        }
    }
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Px(8.0f), Px(4.0f)));
    if (!app.has_latest_result)
    {
        WrappedText(kMuted, "Token counts, timings and server state appear after the first response.", kFontSmall);
    }
    else if (BeginKeyValueTable("##Signals", Px(104.0f)))
    {
        const LlamaRunResult& result = app.latest_result;
        KeyValueRow("tokens", FormatCount(result.prompt_tokens) + " in \xC2\xB7 " + FormatCount(result.predicted_tokens) + " out");
        KeyValueRow("prompt timing", FormatMilliseconds(result.prompt_ms) + " \xC2\xB7 " + FormatMilliseconds(result.prompt_ms_per_token) + "/tok");
        KeyValueRow("decode timing", FormatMilliseconds(result.predicted_ms) + " \xC2\xB7 " + FormatMilliseconds(result.predicted_ms_per_token) + "/tok");
        KeyValueRow("request wall", FormatSeconds(result.request_seconds));
        KeyValueRow("server load", FormatSeconds(result.server_load_seconds));
        KeyValueRow("finish", result.finish_reason.empty() ? kNone : result.finish_reason);
        if (app.show_all_signals)
        {
            KeyValueRow("total tokens", FormatCount(result.total_tokens));
            KeyValueRow("reasoning", FormatCount(result.reasoning_tokens));
            KeyValueRow("context", FormatCount(result.context_size) + " \xC2\xB7 cached " + FormatCount(result.tokens_cached) + (result.prompt_cache_reused ? " \xC2\xB7 reused" : ""));
            KeyValueRow("server config", "batch " + FormatCount(result.batch_size) + " \xC2\xB7 ubatch " + FormatCount(result.ubatch_size) + " \xC2\xB7 threads " + FormatCount(result.threads) + " \xC2\xB7 GPU " + FormatCount(result.gpu_layers));
            KeyValueRow("slot", result.slot_info_available ? (std::to_string(result.slot_id) + " \xC2\xB7 " + (result.slot_state.empty() ? "unknown" : result.slot_state)) : "not exposed");
            KeyValueRow("process CPU", FormatSeconds(result.cpu_seconds));
            KeyValueRow("memory", FormatMemory(result.memory_gb) + " \xC2\xB7 peak " + FormatMemory(result.peak_memory_gb));
            KeyValueRow("response", FormatCount(result.response_bytes) + " bytes \xC2\xB7 HTTP " + std::to_string(result.http_status));
            KeyValueRow("metrics", result.metrics_available ? result.metrics_summary : "not exposed");
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    EndCard();

    // Quick controls.
    BeginCard("##QuickControls", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY);
    CardTitle("Quick controls");
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Px(8.0f), Px(6.0f)));
    {
        char value[32];
        std::snprintf(value, sizeof(value), "%.2f", app.temperature);
        ControlLabel("Temperature", value);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##QuickTemperature", &app.temperature, 0.0f, 1.5f, "");
        std::snprintf(value, sizeof(value), "%d", app.max_tokens);
        ControlLabel("Max output tokens", value);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderInt("##QuickMaxTokens", &app.max_tokens, 16, 8192, "");
        ControlLabel("Context window", nullptr);
        ContextCombo(app, "##QuickContext");
    }
    ImGui::PopStyleVar();
    EndCard();

    // Launch command.
    BeginCard("##CommandCard", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY);
    {
        const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        CardTitle("Launch command");
        SameLineRight(IconButtonWidth(ICON_COPY, "Copy", 26.0f), right);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Px(4.0f));
        if (IconButton("##CopyCommandInsights", ICON_COPY, "Copy", "Copy the llama-server command", 26.0f))
            CopyRunCommand(app);
        const std::string command = CurrentRunCommand(app);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, kInset);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(10.0f), Px(8.0f)));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, Px(8.0f));
        ImGui::BeginChild("##CommandText", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
        {
            ScopedFont scoped(g_font_mono, kFontCaption);
            ImGui::PushStyleColor(ImGuiCol_Text, kInkSoft);
            ImGui::TextWrapped("%s", command.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }
    EndCard();

    ImGui::PopStyleVar();
}

static void DrawPlayground(AppState& app)
{
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float gap = Px(14.0f);
    const bool show_insights = available.x >= Px(1080.0f);
    const float models_width = available.x >= Px(900.0f) ? Px(268.0f) : Px(232.0f);
    const float insights_width = show_insights ? Px(316.0f) : 0.0f;
    const float center_width = available.x - models_width - gap - (show_insights ? insights_width + gap : 0.0f);

    BeginCard("##ModelsPanel", ImVec2(models_width, available.y), ImGuiChildFlags_None, ImGuiWindowFlags_None, ImVec2(Px(14.0f), Px(14.0f)));
    DrawModelsPanel(app);
    EndCard();

    ImGui::SameLine(0.0f, gap);
    ImGui::BeginChild("##Center", ImVec2(center_width, available.y), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    DrawConversation(app);
    ImGui::EndChild();

    if (show_insights)
    {
        ImGui::SameLine(0.0f, gap);
        ImGui::BeginChild("##Insights", ImVec2(insights_width, available.y), ImGuiChildFlags_None, ImGuiWindowFlags_None);
        DrawInsights(app);
        ImGui::EndChild();
    }
}

// -------------------------------------------------------------------------------------------------
// Diagnostics
// -------------------------------------------------------------------------------------------------

static void TableTextRight(const std::string& text, const ImVec4& color = kInkSoft)
{
    const float width = ImGui::CalcTextSize(text.c_str()).x;
    const float available = ImGui::GetContentRegionAvail().x;
    if (available > width)
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + available - width);
    TextColoredUnformatted(color, text.c_str());
}

static void DrawResultCard(const AppState& app, const LlamaRunResult& result, float width, double best_decode, bool show_fastest)
{
    const float height = Px(172.0f);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max = min + ImVec2(width, height);
    ImGui::Dummy(ImVec2(width, height));
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImVec4 series = ModelColor(app, result.model);
    const bool fastest = show_fastest && result.ok && !result.cancelled && result.decode_tokens_per_second >= best_decode && best_decode > 0.0;
    draw_list->AddRectFilled(min, max, Col(kSurface), Px(12.0f));
    draw_list->AddRect(min, max, Col(fastest ? WithAlpha(kSuccess, 0.45f) : kBorderSoft), Px(12.0f));

    const float pad = Px(16.0f);
    const char* status = result.cancelled ? "stopped" : (result.ok ? (fastest ? "fastest" : "measured") : "error");
    const ImVec4 status_color = result.cancelled ? kMuted : (result.ok ? (fastest ? kSuccess : kAccentHover) : kError);
    const ImVec2 status_size = PillSize(status);
    DrawPill(draw_list, ImVec2(max.x - pad - status_size.x, min.y + pad - Px(2.0f)), status, status_color);
    draw_list->AddCircleFilled(ImVec2(min.x + pad + Px(4.0f), min.y + pad + Px(8.0f)), Px(4.0f), Col(series));
    {
        ScopedFont scoped(g_font_semibold, kFontBody);
        draw_list->AddText(ImVec2(min.x + pad + Px(16.0f), min.y + pad), Col(kInk), Ellipsize(result.model, width - pad * 2.0f - Px(24.0f) - status_size.x).c_str());
    }

    const std::string decode = result.ok ? FormatNumber(result.decode_tokens_per_second, 1) : std::string(kNone);
    float decode_width = 0.0f;
    {
        ScopedFont scoped(g_font_bold, 28.0f);
        draw_list->AddText(ImVec2(min.x + pad, min.y + Px(42.0f)), Col(result.ok ? kInk : kFaint), decode.c_str());
        decode_width = ImGui::CalcTextSize(decode.c_str()).x;
    }
    {
        ScopedFont scoped(g_font_body, kFontSmall);
        draw_list->AddText(ImVec2(min.x + pad + decode_width + Px(8.0f), min.y + Px(56.0f)), Col(kMuted), "tok/s decode");
    }

    const float column = (width - pad * 2.0f) * 0.5f;
    const auto metric = [&](int slot, const char* label, const std::string& value)
    {
        const float x = min.x + pad + column * static_cast<float>(slot % 2);
        const float y = min.y + Px(92.0f) + Px(22.0f) * static_cast<float>(slot / 2);
        ScopedFont scoped(g_font_body, kFontSmall);
        draw_list->AddText(ImVec2(x, y), Col(kMuted), label);
        const float label_width = ImGui::CalcTextSize(label).x;
        draw_list->AddText(ImVec2(x + label_width + Px(6.0f), y), Col(kInkSoft), Ellipsize(value, column - label_width - Px(12.0f)).c_str());
    };
    metric(0, "prompt", FormatNumber(result.prompt_tokens_per_second, 1) + " tok/s");
    metric(1, "tokens", FormatCount(result.prompt_tokens) + " / " + FormatCount(result.predicted_tokens));
    metric(2, "request", FormatSeconds(result.request_seconds));
    metric(3, "memory", FormatMemory(result.memory_gb));

    const float bar_y = max.y - pad - Px(4.0f);
    const float fraction = best_decode > 0.0 && result.ok ? static_cast<float>(result.decode_tokens_per_second / best_decode) : 0.0f;
    draw_list->AddRectFilled(ImVec2(min.x + pad, bar_y), ImVec2(max.x - pad, bar_y + Px(4.0f)), Col(kInset), Px(2.0f));
    if (fraction > 0.0f)
        draw_list->AddRectFilled(ImVec2(min.x + pad, bar_y), ImVec2(min.x + pad + (width - pad * 2.0f) * fraction, bar_y + Px(4.0f)), Col(series), Px(2.0f));
}

static void DetailGroupTitle(const char* title)
{
    TextWithFont(g_font_semibold, kFontSmall + 0.5f, kInk, title);
}

static void DrawDiagnostics(AppState& app)
{
    if (app.comparison_results.empty())
    {
        BeginCard("##DiagnosticsEmpty", ImVec2(0.0f, Px(320.0f)));
        EmptyState(ICON_DIAG, app.run_busy ? "Run in progress" : "No runs yet", app.run_busy ? "Measurements appear here as soon as each model finishes." : "Run a prompt in the playground. Every model's timing, token counts, memory and server state are collected here.");
        if (!app.run_busy)
        {
            VerticalSpace(8.0f);
            const float button_width = Px(170.0f);
            ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - button_width) * 0.5f + ImGui::GetCursorPosX());
            if (UiButton("##GoPlayground", ICON_CHAT, "Open playground", ButtonKind::Primary, ImVec2(button_width, Px(34.0f))))
                FocusPrompt(app);
        }
        EndCard();
        return;
    }

    for (const LlamaRunResult& result : app.comparison_results)
    {
        if (!result.ok && !result.cancelled && !result.error.empty())
        {
            const std::string text = result.model + ": " + result.error;
            BeginCard(("##Error" + result.model).c_str(), ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_None, ImVec2(Px(14.0f), Px(10.0f)), Mix(kSurface, kError, 0.08f));
            WrappedText(Mix(kError, kInk, 0.35f), text.c_str(), kFontSmall + 0.5f);
            EndCard();
        }
    }

    // Leaderboard cards.
    double best_decode = 0.0;
    size_t ok_count = 0;
    for (const LlamaRunResult& result : app.comparison_results)
    {
        if (result.ok && !result.cancelled)
        {
            best_decode = std::max(best_decode, result.decode_tokens_per_second);
            ++ok_count;
        }
    }
    {
        const float available = ImGui::GetContentRegionAvail().x;
        const float gap = Px(12.0f);
        const int count = static_cast<int>(app.comparison_results.size());
        const int columns = std::max(1, std::min(count, static_cast<int>((available + gap) / (Px(236.0f) + gap))));
        const float card_width = std::floor((available - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
        for (int index = 0; index < count; ++index)
        {
            if (index % columns != 0)
                ImGui::SameLine();
            ImGui::PushID(index);
            DrawResultCard(app, app.comparison_results[static_cast<size_t>(index)], card_width, best_decode, ok_count > 1);
            ImGui::PopID();
        }
        ImGui::PopStyleVar();
    }

    // Side-by-side table.
    BeginCard("##ComparisonCard", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY);
    CardTitle("Side by side");
    VerticalSpace(0.0f);
    {
        ScopedFont scoped(g_font_body, kFontSmall + 0.5f);
        if (ImGui::BeginTable("##ComparisonTable", 11, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX))
        {
            ImGui::TableSetupColumn("Model", ImGuiTableColumnFlags_WidthStretch, 2.2f);
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Prompt tok/s", ImGuiTableColumnFlags_WidthStretch, 1.1f);
            ImGui::TableSetupColumn("Decode tok/s", ImGuiTableColumnFlags_WidthStretch, 1.1f);
            ImGui::TableSetupColumn("Prompt tok", ImGuiTableColumnFlags_WidthStretch, 0.9f);
            ImGui::TableSetupColumn("Output tok", ImGuiTableColumnFlags_WidthStretch, 0.9f);
            ImGui::TableSetupColumn("Prompt", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Decode", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Request", ImGuiTableColumnFlags_WidthStretch, 0.9f);
            ImGui::TableSetupColumn("Memory", ImGuiTableColumnFlags_WidthStretch, 0.9f);
            ImGui::TableSetupColumn("Finish", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
            ImGui::TableHeadersRow();
            ImGui::PopStyleColor();
            for (size_t index = 0; index < app.comparison_results.size(); ++index)
            {
                const LlamaRunResult& result = app.comparison_results[index];
                const bool fastest = ok_count > 1 && result.ok && !result.cancelled && result.decode_tokens_per_second >= best_decode;
                ImGui::TableNextRow(ImGuiTableRowFlags_None, Px(30.0f));
                ImGui::TableSetColumnIndex(0);
                const ImVec2 dot = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(dot.x + Px(4.0f), dot.y + ImGui::GetFontSize() * 0.55f), Px(3.5f), Col(ModelColor(app, result.model)));
                ImGui::SetCursorScreenPos(ImVec2(dot.x + Px(14.0f), dot.y));
                TextColoredUnformatted(kInk, Ellipsize(result.model, ImGui::GetContentRegionAvail().x).c_str());
                ImGui::TableSetColumnIndex(1);
                TextColoredUnformatted(result.cancelled ? kMuted : (result.ok ? kSuccess : kError), result.cancelled ? "stopped" : (result.ok ? "measured" : "error"));
                ImGui::TableSetColumnIndex(2);
                TableTextRight(FormatNumber(result.prompt_tokens_per_second, 1));
                ImGui::TableSetColumnIndex(3);
                TableTextRight(FormatNumber(result.decode_tokens_per_second, 1), fastest ? kSuccess : kInk);
                ImGui::TableSetColumnIndex(4);
                TableTextRight(FormatCount(result.prompt_tokens));
                ImGui::TableSetColumnIndex(5);
                TableTextRight(FormatCount(result.predicted_tokens));
                ImGui::TableSetColumnIndex(6);
                TableTextRight(FormatMilliseconds(result.prompt_ms));
                ImGui::TableSetColumnIndex(7);
                TableTextRight(FormatMilliseconds(result.predicted_ms));
                ImGui::TableSetColumnIndex(8);
                TableTextRight(FormatSeconds(result.request_seconds));
                ImGui::TableSetColumnIndex(9);
                TableTextRight(FormatMemory(result.memory_gb));
                ImGui::TableSetColumnIndex(10);
                TextColoredUnformatted(kInkSoft, result.cancelled ? "stopped" : (result.ok ? (result.finish_reason.empty() ? kNone : result.finish_reason.c_str()) : "error"));
            }
            ImGui::EndTable();
        }
    }
    EndCard();

    if (!app.has_latest_result)
        return;

    // Latest response, grouped.
    const LlamaRunResult& result = app.latest_result;
    VerticalSpace(4.0f);
    SectionLabel("LATEST RESPONSE");
    {
        ScopedFont scoped(g_font_bold, 17.0f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Px(4.0f));
        TextColoredUnformatted(kInk, result.model.c_str());
    }

    const float label_width = Px(132.0f);
    const bool two_columns = ImGui::GetContentRegionAvail().x >= Px(820.0f);
    const auto group = [&](const char* id, const char* title, const std::function<void()>& rows)
    {
        BeginCard(id, ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY);
        DetailGroupTitle(title);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Px(8.0f), Px(4.0f)));
        if (BeginKeyValueTable("##Rows", label_width))
        {
            rows();
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();
        EndCard();
    };

    const std::function<void()> identity = [&]()
    {
        KeyValueRow("run status", result.cancelled ? "stopped by user" : (result.ok ? "completed" : "error"), result.cancelled ? kMuted : (result.ok ? kSuccess : kError));
        KeyValueRow("response id", result.response_id);
        KeyValueRow("response type", result.response_object);
        KeyValueRow("server model", result.response_model);
        KeyValueRow("model alias", result.model_alias);
        KeyValueRow("model path", result.model_path);
        KeyValueRow("build info", result.build_info);
        KeyValueRow("chat template", result.chat_template);
        KeyValueRow("fingerprint", result.system_fingerprint);
        KeyValueRow("HTTP status", std::to_string(result.http_status));
    };
    const std::function<void()> timing = [&]()
    {
        KeyValueRow("timing source", result.timings_source);
        KeyValueRow("server load", FormatSeconds(result.server_load_seconds));
        KeyValueRow("request wall", FormatSeconds(result.request_seconds));
        KeyValueRow("timing totals", FormatMilliseconds(result.total_ms) + " \xC2\xB7 load " + FormatMilliseconds(result.load_ms) + " \xC2\xB7 start " + FormatMilliseconds(result.start_ms));
        KeyValueRow("prompt eval", FormatMilliseconds(result.prompt_ms) + " \xC2\xB7 " + FormatRate(result.prompt_tokens_per_second) + " \xC2\xB7 " + FormatMilliseconds(result.prompt_ms_per_token) + "/tok");
        KeyValueRow("decode", FormatMilliseconds(result.predicted_ms) + " \xC2\xB7 " + FormatRate(result.decode_tokens_per_second) + " \xC2\xB7 " + FormatMilliseconds(result.predicted_ms_per_token) + "/tok");
        KeyValueRow("sampling", "tokenize " + FormatMilliseconds(result.tokenize_ms) + " \xC2\xB7 sample " + FormatMilliseconds(result.sample_ms));
        KeyValueRow("metric totals", "prompt " + FormatSeconds(result.metrics_prompt_seconds_total) + " \xC2\xB7 predicted " + FormatSeconds(result.metrics_predicted_seconds_total));
    };
    const std::function<void()> tokens = [&]()
    {
        KeyValueRow("prompt", FormatCount(result.prompt_tokens));
        KeyValueRow("generated", FormatCount(result.predicted_tokens));
        KeyValueRow("total", FormatCount(result.total_tokens));
        KeyValueRow("reasoning", FormatCount(result.reasoning_tokens));
        KeyValueRow("predictions", "accepted " + FormatCount(result.accepted_prediction_tokens) + " \xC2\xB7 rejected " + FormatCount(result.rejected_prediction_tokens));
        KeyValueRow("response", FormatCount(result.response_bytes) + " bytes \xC2\xB7 finish " + (result.finish_reason.empty() ? std::string(kNone) : result.finish_reason));
    };
    const std::function<void()> server = [&]()
    {
        KeyValueRow("server config", "context " + FormatCount(result.context_size) + " \xC2\xB7 batch " + FormatCount(result.batch_size) + " \xC2\xB7 ubatch " + FormatCount(result.ubatch_size));
        KeyValueRow("threads", FormatCount(result.threads) + " \xC2\xB7 batch " + FormatCount(result.threads_batch) + " \xC2\xB7 GPU layers " + FormatCount(result.gpu_layers));
        KeyValueRow("context & cache", FormatCount(result.context_size) + " \xC2\xB7 cached " + FormatCount(result.tokens_cached) + " \xC2\xB7 reused " + (result.prompt_cache_reused ? "yes" : "no"));
        KeyValueRow("slot state", result.slot_info_available ? ("id " + std::to_string(result.slot_id) + " \xC2\xB7 " + (result.slot_state.empty() ? "unknown" : result.slot_state)) : "not exposed by server");
        KeyValueRow("slot counters", "past " + FormatCount(result.n_past) + " \xC2\xB7 prompt " + FormatCount(result.n_prompt_tokens) + " \xC2\xB7 decoded " + FormatCount(result.n_decoded) + " \xC2\xB7 cache " + FormatCount(result.n_cache_tokens) + " \xC2\xB7 limit " + FormatCount(result.n_predict_limit));
        const std::string flags = std::string(result.stopped_eos ? "EOS " : "") + (result.stopped_word ? "word " : "") + (result.stopped_limit ? "limit " : "") + (result.slot_truncated ? "truncated" : "");
        KeyValueRow("stop flags", flags.empty() ? "none" : flags);
    };
    const std::function<void()> resources = [&]()
    {
        KeyValueRow("process CPU", FormatSeconds(result.cpu_seconds));
        KeyValueRow("working set", FormatMemory(result.memory_gb));
        KeyValueRow("peak", FormatMemory(result.peak_memory_gb));
        KeyValueRow("private", FormatMemory(result.private_memory_gb));
        KeyValueRow("metrics", result.metrics_available ? result.metrics_summary : "not exposed by server");
    };

    if (two_columns && ImGui::BeginTable("##DetailGrid", 2, ImGuiTableFlags_SizingStretchSame))
    {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        group("##Identity", "Identity", identity);
        group("##Tokens", "Tokens", tokens);
        ImGui::TableSetColumnIndex(1);
        group("##Timing", "Timing", timing);
        group("##Server", "Server & slot", server);
        group("##Resources", "Resources", resources);
        ImGui::EndTable();
    }
    else
    {
        group("##Identity", "Identity", identity);
        group("##Timing", "Timing", timing);
        group("##Tokens", "Tokens", tokens);
        group("##Server", "Server & slot", server);
        group("##Resources", "Resources", resources);
    }
}

// -------------------------------------------------------------------------------------------------
// Trace
// -------------------------------------------------------------------------------------------------

static ImVec4 TraceStatusColor(const std::string& status)
{
    if (status == "error")
        return kError;
    if (status == "stopped")
        return kMuted;
    if (status.find("tok/s") != std::string::npos)
        return kAccentHover;
    if (status == "ready" || status == "available" || status == "stop")
        return kSuccess;
    return kInkSoft;
}

static void DrawTrace(AppState& app)
{
    // Toolbar.
    {
        const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        const float search_width = std::min(Px(320.0f), ImGui::GetContentRegionAvail().x * 0.5f);
        const ImVec2 field = ImGui::GetCursorScreenPos();
        const char* glyph = Icon(ICON_SEARCH);
        const bool has_glyph = glyph[0] != '\0';
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(has_glyph ? Px(34.0f) : Px(12.0f), Px(8.0f)));
        ImGui::PushStyleColor(ImGuiCol_Border, kBorderSoft);
        ImGui::SetNextItemWidth(search_width);
        ImGui::InputTextWithHint("##TraceFilter", "Filter by model, event or detail", app.trace_filter, sizeof(app.trace_filter));
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        if (has_glyph)
            ImGui::GetWindowDrawList()->AddText(ImVec2(field.x + Px(12.0f), field.y + Px(8.0f)), Col(kMuted), glyph);

        const float copy_width = Px(128.0f);
        SameLineRight(copy_width, right);
        ImGui::BeginDisabled(app.trace.empty());
        if (UiButton("##CopyTrace", ICON_COPY, "Copy trace", ButtonKind::Secondary, ImVec2(copy_width, Px(34.0f))))
        {
            std::string text = "model\tevent\tduration\tstatus\tdetail\n";
            for (const TraceEntry& entry : app.trace)
                text += entry.model + "\t" + entry.event + "\t" + entry.duration + "\t" + entry.status + "\t" + entry.detail + "\n";
            ImGui::SetClipboardText(text.c_str());
            ShowToast(app, "Trace copied as tab-separated text");
        }
        ImGui::EndDisabled();

        const std::string count = std::to_string(app.trace.size()) + (app.trace.size() == 1 ? " event" : " events");
        ScopedFont scoped(g_font_body, kFontSmall);
        const float count_width = ImGui::CalcTextSize(count.c_str()).x;
        ImGui::SameLine();
        ImGui::SetCursorScreenPos(ImVec2(right - copy_width - Px(14.0f) - count_width, ImGui::GetCursorScreenPos().y + Px(8.0f)));
        TextColoredUnformatted(kMuted, count.c_str());
    }
    VerticalSpace(2.0f);

    if (app.trace.empty())
    {
        BeginCard("##TraceEmpty", ImVec2(0.0f, Px(300.0f)));
        EmptyState(ICON_HISTORY, "No events recorded", "Server start-up, slot timing, token accounting, response status and resource snapshots are logged here for every model in a run.");
        EndCard();
        return;
    }

    const std::string query = LowerCopy(app.trace_filter);
    BeginCard("##TraceCard", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_None, ImVec2(Px(6.0f), Px(6.0f)));
    ImGui::PushFont(g_font_body, kFontSmall + 0.5f);
    if (ImGui::BeginTable("##TraceTable", 5, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX))
    {
        ImGui::TableSetupColumn("Model", ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableSetupColumn("Event", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Duration", ImGuiTableColumnFlags_WidthStretch, 0.9f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthStretch, 4.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
        ImGui::TableHeadersRow();
        ImGui::PopStyleColor();

        std::string previous_model;
        size_t shown = 0;
        for (size_t index = 0; index < app.trace.size(); ++index)
        {
            const TraceEntry& entry = app.trace[index];
            if (!query.empty())
            {
                const std::string haystack = LowerCopy(entry.model + " " + entry.event + " " + entry.status + " " + entry.detail);
                if (haystack.find(query) == std::string::npos)
                    continue;
            }
            ++shown;
            const bool new_group = entry.model != previous_model;
            previous_model = entry.model;
            ImGui::TableNextRow(ImGuiTableRowFlags_None, Px(30.0f));
            if (new_group && shown > 1)
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, Col(kSurfaceRaised, 0.45f));
            ImGui::PushID(static_cast<int>(index));
            ImGui::TableSetColumnIndex(0);
            if (new_group)
            {
                const ImVec2 dot = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(dot.x + Px(4.0f), dot.y + ImGui::GetFontSize() * 0.55f), Px(3.5f), Col(ModelColor(app, entry.model)));
                ImGui::SetCursorScreenPos(ImVec2(dot.x + Px(14.0f), dot.y));
                ScopedFont bold(g_font_semibold, kFontSmall + 0.5f);
                TextColoredUnformatted(kInk, Ellipsize(entry.model, ImGui::GetContentRegionAvail().x).c_str());
            }
            ImGui::TableSetColumnIndex(1);
            {
                ScopedFont bold(g_font_semibold, kFontSmall + 0.5f);
                TextColoredUnformatted(kInkSoft, entry.event.c_str());
            }
            ImGui::TableSetColumnIndex(2);
            {
                ScopedFont mono(g_font_mono, kFontCaption + 0.5f);
                TableTextRight(entry.duration, entry.duration == kNone ? kFaint : kInk);
            }
            ImGui::TableSetColumnIndex(3);
            if (!entry.status.empty())
                Pill(Ellipsize(entry.status, ImGui::GetContentRegionAvail().x - Px(20.0f)).c_str(), TraceStatusColor(entry.status));
            ImGui::TableSetColumnIndex(4);
            ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(entry.detail.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (shown == 0)
        {
            VerticalSpace(4.0f);
            TextColoredUnformatted(kMuted, "  No events match the filter.");
        }
    }
    ImGui::PopFont();
    EndCard();
}

// -------------------------------------------------------------------------------------------------
// Settings
// -------------------------------------------------------------------------------------------------

static bool BeginSettingsCard(const char* id, const char* title, const char* subtitle, float width)
{
    BeginCard(id, ImVec2(width, 0.0f), ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_None, ImVec2(Px(20.0f), Px(18.0f)));
    CardTitle(title, subtitle);
    VerticalSpace(2.0f);
    if (!ImGui::BeginTable("##SettingRows", 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp))
        return false;
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 1.25f);
    ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    return true;
}

static void EndSettingsCard(bool table_open)
{
    if (table_open)
        ImGui::EndTable();
    EndCard();
}

static void SettingLabel(const char* label, const char* help)
{
    ImGui::TableNextRow(ImGuiTableRowFlags_None, Px(52.0f));
    ImGui::TableSetColumnIndex(0);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Px(4.0f));
    TextWithFont(g_font_semibold, kFontBody - 0.5f, kInk, label);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Px(5.0f));
    WrappedText(kMuted, help, kFontCaption + 0.5f);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Px(8.0f));
    ImGui::SetNextItemWidth(-FLT_MIN);
}

static void SettingSwitch(const char* id, const char* label, const char* help, bool* value)
{
    SettingLabel(label, help);
    const float switch_width = Px(36.0f);
    const float available = ImGui::GetContentRegionAvail().x;
    if (available > switch_width)
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + available - switch_width);
    ToggleSwitch(id, value);
}

static void DrawSettings(AppState& app)
{
    const float available = ImGui::GetContentRegionAvail().x;
    const float width = std::min(available, Px(820.0f));
    const float offset = std::floor((available - width) * 0.5f);
    if (offset > 0.0f)
        ImGui::Indent(offset);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Px(8.0f), Px(14.0f)));

    // Engine binary.
    {
        BeginCard("##BinaryCard", ImVec2(width, 0.0f), ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_None, ImVec2(Px(20.0f), Px(18.0f)));
        const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        const bool has_server = !app.server_path.empty();
        const char* state = has_server ? "found" : "missing";
        TextWithFont(g_font_semibold, kFontBody, kInk, "llama-server binary");
        SameLineRight(PillSize(state).x, right);
        Pill(state, has_server ? kSuccess : kWarning);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Px(4.0f));
        WrappedText(kMuted, "Renaro starts one llama-server process per selected model. llama.cpp is never bundled.", kFontSmall);
        VerticalSpace(0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Px(8.0f), Px(8.0f)));
        const float button_width = Px(118.0f);
        const ImVec2 box = ImGui::GetCursorScreenPos();
        const float box_width = ImGui::GetContentRegionAvail().x - button_width - Px(10.0f);
        const float box_height = Px(36.0f);
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        draw_list->AddRectFilled(box, box + ImVec2(box_width, box_height), Col(kInset), Px(8.0f));
        draw_list->AddRect(box, box + ImVec2(box_width, box_height), Col(kBorderSoft), Px(8.0f));
        {
            ScopedFont scoped(g_font_mono, kFontSmall);
            const std::string path = has_server ? WideToUtf8(app.server_path) : "Select llama-server.exe to enable runs";
            const std::string fitted = Ellipsize(path, box_width - Px(24.0f));
            draw_list->AddText(ImVec2(box.x + Px(12.0f), box.y + std::floor((box_height - ImGui::GetFontSize()) * 0.5f)), Col(has_server ? kInkSoft : kMuted), fitted.c_str());
        }
        ImGui::Dummy(ImVec2(box_width, box_height));
        if (has_server && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", WideToUtf8(app.server_path).c_str());
        ImGui::SameLine(0.0f, Px(10.0f));
        ImGui::BeginDisabled(app.run_busy);
        if (UiButton("##BrowseServer", ICON_FOLDER, "Browse", ButtonKind::Secondary, ImVec2(button_width, box_height)))
            LocateServer(app);
        ImGui::EndDisabled();
        ImGui::PopStyleVar();
        EndCard();
    }

    // Generation.
    {
        const bool open = BeginSettingsCard("##GenerationCard", "Generation", "Sampling options sent with each chat completion request.", width);
        if (open)
        {
            SettingLabel("Temperature", "Higher values make output more varied.");
            ImGui::SliderFloat("##SettingsTemperature", &app.temperature, 0.0f, 1.5f, "%.2f");
            SettingLabel("Max output tokens", "Upper bound on generated tokens per model.");
            ImGui::SliderInt("##SettingsMaxTokens", &app.max_tokens, 16, 8192);
            SettingLabel("Seed", "Fixed seed for reproducible comparisons.");
            ImGui::SliderInt("##SettingsSeed", &app.seed, 0, 9999);
            SettingSwitch("##SettingsCachePrompt", "Reuse prompt cache", "Let llama-server reuse the KV cache for a repeated prompt prefix.", &app.cache_prompt);
        }
        EndSettingsCard(open);
    }

    // Context and batching.
    {
        const bool open = BeginSettingsCard("##ContextCard", "Context & batching", "How much text the server keeps in memory and how it schedules prompt processing.", width);
        if (open)
        {
            SettingLabel("Context window", "Tokens of context allocated per model.");
            ContextCombo(app, "##SettingsContext");
            SettingLabel("Batch size", "Logical batch for prompt processing (-b).");
            ImGui::SliderInt("##SettingsBatch", &app.batch_size, 32, 4096);
            SettingLabel("Micro batch size", "Physical batch submitted to the backend (-ub).");
            ImGui::SliderInt("##SettingsMicroBatch", &app.ubatch_size, 32, 4096);
        }
        EndSettingsCard(open);
    }

    // Compute.
    {
        const bool open = BeginSettingsCard("##ComputeCard", "CPU & GPU", "Thread counts and offload settings for every llama-server launch.", width);
        if (open)
        {
            SettingLabel("Threads", "Threads used during generation (-t).");
            ImGui::SliderInt("##SettingsThreads", &app.threads, 1, 64);
            SettingLabel("Batch threads", "Threads used for prompt processing (-tb).");
            ImGui::SliderInt("##SettingsBatchThreads", &app.threads_batch, 1, 64);
            SettingLabel("GPU layers", "Layers offloaded to the GPU (-ngl). 0 keeps the model on the CPU.");
            ImGui::SliderInt("##SettingsGpuLayers", &app.gpu_layers, 0, 128);
            SettingSwitch("##SettingsFlashAttention", "Flash attention", "Faster attention kernels where the backend supports them.", &app.flash_attention);
            SettingSwitch("##SettingsKvOffload", "Keep KV cache on GPU", "Turn off to keep the KV cache in system memory.", &app.kv_offload);
        }
        EndSettingsCard(open);
    }

    // Memory.
    {
        const bool open = BeginSettingsCard("##MemoryCard", "Memory", "How model weights are loaded from disk.", width);
        if (open)
        {
            SettingSwitch("##SettingsMmap", "Memory-map weights", "Map the GGUF file instead of reading it into RAM up front.", &app.mmap);
            SettingSwitch("##SettingsMlock", "Lock weights in memory", "Prevent the OS from paging the model out.", &app.mlock);
        }
        EndSettingsCard(open);
    }

    // Command preview.
    {
        BeginCard("##SettingsCommandCard", ImVec2(width, 0.0f), ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_None, ImVec2(Px(20.0f), Px(18.0f)));
        const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        TextWithFont(g_font_semibold, kFontBody, kInk, "Launch command");
        const float copy_width = Px(96.0f);
        SameLineRight(copy_width, right);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Px(6.0f));
        if (UiButton("##SettingsCopyCommand", ICON_COPY, "Copy", ButtonKind::Secondary, ImVec2(copy_width, Px(30.0f))))
            CopyRunCommand(app);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Px(10.0f));
        WrappedText(kMuted, "The equivalent command for the first selected model.", kFontSmall);
        VerticalSpace(0.0f);
        const std::string command = CurrentRunCommand(app);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, kInset);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(12.0f), Px(10.0f)));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, Px(8.0f));
        ImGui::BeginChild("##SettingsCommandText", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
        {
            ScopedFont scoped(g_font_mono, kFontSmall);
            ImGui::PushStyleColor(ImGuiCol_Text, kInkSoft);
            ImGui::TextWrapped("%s", command.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        EndCard();
    }

    ImGui::PopStyleVar();
    if (offset > 0.0f)
        ImGui::Unindent(offset);
}

// -------------------------------------------------------------------------------------------------
// Command palette and toast
// -------------------------------------------------------------------------------------------------

struct PaletteAction
{
    const char* icon;
    const char* label;
    const char* detail;
    const char* id;
    const char* shortcut;
};

static const PaletteAction kPaletteActions[] = {
    { ICON_SEND, "Focus prompt", "Jump to the prompt box", "focus", nullptr },
    { ICON_PLAY, "Run selected models", "Send the current prompt to every selected model", "run-selected", "Enter" },
    { ICON_ADD, "Import model", "Add a local .gguf file to the library", "import", nullptr },
    { ICON_CHAT, "Open playground", "Conversation and model library", "playground", "1" },
    { ICON_DIAG, "Open diagnostics", "Compare timing, tokens and memory", "diagnostics", "2" },
    { ICON_HISTORY, "Open trace log", "Inspect the llama.cpp event sequence", "trace", "3" },
    { ICON_SETTINGS, "Open settings", "Engine and sampling controls", "settings", "4" },
    { ICON_FOLDER, "Locate llama-server", "Choose the llama-server.exe binary", "locate", nullptr },
    { ICON_COPY, "Copy launch command", "Copy the equivalent llama-server command", "copy", nullptr },
    { ICON_DELETE, "Clear playground", "Remove messages and measured results", "clear-playground", nullptr }
};

static void RunCommandAction(AppState& app, const char* action)
{
    app.command_palette = false;
    app.command_search[0] = '\0';
    if (std::strcmp(action, "focus") == 0)
        FocusPrompt(app);
    else if (std::strcmp(action, "playground") == 0)
        app.pane = Pane::Playground;
    else if (std::strcmp(action, "diagnostics") == 0)
        app.pane = Pane::Diagnostics;
    else if (std::strcmp(action, "trace") == 0)
        app.pane = Pane::Trace;
    else if (std::strcmp(action, "settings") == 0)
        app.pane = Pane::Settings;
    else if (std::strcmp(action, "run-selected") == 0)
        StartRun(app, app.prompt);
    else if (std::strcmp(action, "import") == 0)
        ImportModel(app);
    else if (std::strcmp(action, "locate") == 0)
        LocateServer(app);
    else if (std::strcmp(action, "copy") == 0)
        CopyRunCommand(app);
    else if (std::strcmp(action, "clear-playground") == 0)
        ClearPlayground(app, true);
}

static void DrawCommandPalette(AppState& app)
{
    const char* popup_id = "##RenaroCommandPalette";
    if (app.command_palette && !ImGui::IsPopupOpen(popup_id))
        ImGui::OpenPopup(popup_id);

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float width = std::min(Px(580.0f), viewport->WorkSize.x - Px(48.0f));
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.16f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, kSurfaceRaised);
    ImGui::PushStyleColor(ImGuiCol_Border, kBorder);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Px(14.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, Px(14.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    // NoNavInputs keeps arrow keys for the action list instead of moving keyboard focus out of the search field.
    const bool open = ImGui::BeginPopupModal(popup_id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNavInputs);
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(2);
    if (!open)
    {
        app.command_palette = false;
        return;
    }

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const float inner_width = ImGui::GetContentRegionAvail().x;

    // Search row.
    {
        const ImVec2 row = ImGui::GetCursorScreenPos();
        const float row_height = Px(54.0f);
        const char* glyph = Icon(ICON_SEARCH);
        float text_x = Px(18.0f);
        if (glyph[0] != '\0')
        {
            ScopedFont scoped(g_font_body, kFontBody + 1.0f);
            draw_list->AddText(ImVec2(row.x + Px(20.0f), row.y + std::floor((row_height - ImGui::GetFontSize()) * 0.5f)), Col(kMuted), glyph);
            text_x = Px(48.0f);
        }
        ImGui::SetCursorScreenPos(ImVec2(row.x + text_x, row.y + Px(12.0f)));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, Px(6.0f)));
        ScopedFont scoped(g_font_body, kFontBody + 1.5f);
        if (app.command_focus_search)
        {
            ImGui::SetKeyboardFocusHere();
            app.command_focus_search = false;
        }
        ImGui::SetNextItemWidth(inner_width - text_x - Px(18.0f));
        if (ImGui::InputTextWithHint("##CommandSearch", "Type a command or search...", app.command_search, sizeof(app.command_search)))
            app.command_index = 0;
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + row_height));
        draw_list->AddLine(ImVec2(row.x, row.y + row_height), ImVec2(row.x + inner_width, row.y + row_height), Col(kBorder));
        ImGui::Dummy(ImVec2(inner_width, Px(4.0f)));
    }

    // Filtered actions.
    std::vector<const PaletteAction*> visible;
    const std::string query = LowerCopy(app.command_search);
    for (const PaletteAction& action : kPaletteActions)
    {
        if (query.empty() || LowerCopy(action.label).find(query) != std::string::npos || LowerCopy(action.detail).find(query) != std::string::npos)
            visible.push_back(&action);
    }
    if (!visible.empty())
    {
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
            app.command_index = (app.command_index + 1) % static_cast<int>(visible.size());
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
            app.command_index = (app.command_index + static_cast<int>(visible.size()) - 1) % static_cast<int>(visible.size());
        app.command_index = std::clamp(app.command_index, 0, static_cast<int>(visible.size()) - 1);
    }

    const char* run_action = nullptr;
    const float list_pad = Px(8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, Px(2.0f)));
    for (size_t index = 0; index < visible.size(); ++index)
    {
        const PaletteAction& action = *visible[index];
        const ImVec2 row_start = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(row_start.x + list_pad, row_start.y));
        ImGui::PushID(action.id);
        const ImVec2 size(inner_width - list_pad * 2.0f, Px(46.0f));
        if (ImGui::InvisibleButton("##PaletteRow", size))
            run_action = action.id;
        const bool hovered = ImGui::IsItemHovered();
        if (hovered && (ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f))
            app.command_index = static_cast<int>(index);
        const bool highlighted = app.command_index == static_cast<int>(index);
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        if (highlighted)
            draw_list->AddRectFilled(min, max, Col(kAccent, 0.14f), Px(9.0f));

        const float icon_box = Px(30.0f);
        const ImVec2 icon_min(min.x + Px(8.0f), min.y + (size.y - icon_box) * 0.5f);
        draw_list->AddRectFilled(icon_min, icon_min + ImVec2(icon_box, icon_box), Col(highlighted ? WithAlpha(kAccent, 0.22f) : kInset), Px(7.0f));
        const char* glyph = Icon(action.icon);
        if (glyph[0] != '\0')
        {
            ScopedFont scoped(g_font_body, kFontBody - 1.0f);
            const ImVec2 glyph_size = ImGui::CalcTextSize(glyph);
            draw_list->AddText(icon_min + (ImVec2(icon_box, icon_box) - glyph_size) * 0.5f, Col(highlighted ? kAccentHover : kInkSoft), glyph);
        }

        float right = max.x - Px(10.0f);
        if (action.shortcut)
        {
            const ImVec2 cap = KeyCapSize(action.shortcut);
            right -= cap.x;
            DrawKeyCap(draw_list, ImVec2(right, min.y + (size.y - cap.y) * 0.5f), action.shortcut);
            right -= Px(10.0f);
        }
        const float text_x = icon_min.x + icon_box + Px(12.0f);
        float label_width = 0.0f;
        {
            ScopedFont scoped(g_font_semibold, kFontBody - 0.5f);
            draw_list->AddText(ImVec2(text_x, min.y + std::floor((size.y - ImGui::GetFontSize()) * 0.5f)), Col(kInk), action.label);
            label_width = ImGui::CalcTextSize(action.label).x;
        }
        {
            ScopedFont scoped(g_font_body, kFontSmall);
            const float detail_x = text_x + label_width + Px(10.0f);
            const std::string detail = Ellipsize(action.detail, right - detail_x);
            draw_list->AddText(ImVec2(detail_x, min.y + std::floor((size.y - ImGui::GetFontSize()) * 0.5f) + Px(1.0f)), Col(kMuted), detail.c_str());
        }
        ImGui::PopID();
    }
    ImGui::PopStyleVar();
    if (visible.empty())
    {
        VerticalSpace(6.0f);
        ImGui::SetCursorPosX(Px(20.0f));
        TextColoredUnformatted(kMuted, "No matching actions");
        VerticalSpace(6.0f);
    }

    // Footer hints.
    {
        VerticalSpace(2.0f);
        const ImVec2 footer = ImGui::GetCursorScreenPos();
        const float footer_height = Px(38.0f);
        draw_list->AddLine(footer, ImVec2(footer.x + inner_width, footer.y), Col(kBorderSoft));
        float x = footer.x + Px(18.0f);
        const float y = footer.y + Px(9.0f);
        const auto hint = [&](const char* key, const char* label)
        {
            const ImVec2 cap = DrawKeyCap(draw_list, ImVec2(x, y), key);
            x += cap.x + Px(6.0f);
            ScopedFont scoped(g_font_body, kFontCaption);
            draw_list->AddText(ImVec2(x, y + std::floor((cap.y - ImGui::GetFontSize()) * 0.5f)), Col(kMuted), label);
            x += ImGui::CalcTextSize(label).x + Px(16.0f);
        };
        hint("\xE2\x86\x91\xE2\x86\x93", "navigate");
        hint("Enter", "run");
        hint("Esc", "close");
        ImGui::Dummy(ImVec2(inner_width, footer_height));
    }

    if (!visible.empty() && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)))
        run_action = visible[static_cast<size_t>(app.command_index)]->id;

    const bool clicked_outside = ImGui::IsMouseClicked(ImGuiMouseButton_Left)
        && !ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)
        && ImGui::GetFrameCount() != app.command_opened_frame;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || clicked_outside)
        app.command_palette = false;

    if (run_action || !app.command_palette)
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();

    // Run after the popup is closed so a file dialog or new pane does not draw under the modal.
    if (run_action)
        RunCommandAction(app, run_action);
}

static void DrawToast(const AppState& app)
{
    if (app.toast_remaining <= 0.0f || app.toast.empty())
        return;
    const float elapsed = 3.0f - app.toast_remaining;
    const float appear = std::clamp(elapsed / 0.18f, 0.0f, 1.0f);
    const float alpha = std::clamp(std::min(appear, app.toast_remaining / 0.35f), 0.0f, 1.0f);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 position(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y - Px(28.0f) + (1.0f - appear) * Px(12.0f));
    ImGui::SetNextWindowPos(position, ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(16.0f), Px(10.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Px(20.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kSurfaceRaised);
    ImGui::PushStyleColor(ImGuiCol_Border, kBorder);
    ImGui::Begin("##RenaroToast", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs);
    const char* glyph = Icon(ICON_CHECK);
    if (glyph[0] != '\0')
    {
        TextColoredUnformatted(kAccentHover, glyph);
        ImGui::SameLine(0.0f, Px(8.0f));
    }
    TextWithFont(g_font_semibold, kFontSmall + 0.5f, kInk, app.toast.c_str());
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(4);
}

// -------------------------------------------------------------------------------------------------
// Frame
// -------------------------------------------------------------------------------------------------

static void DrawRenaroApp(AppState& app)
{
    ImGuiIO& io = ImGui::GetIO();
    AdvanceBackend(app, io.DeltaTime);

    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_K, false))
        OpenCommandPalette(app);
    if (!io.WantTextInput && !io.KeyCtrl && !app.command_palette)
    {
        if (ImGui::IsKeyPressed(ImGuiKey_1, false)) app.pane = Pane::Playground;
        if (ImGui::IsKeyPressed(ImGuiKey_2, false)) app.pane = Pane::Diagnostics;
        if (ImGui::IsKeyPressed(ImGuiKey_3, false)) app.pane = Pane::Trace;
        if (ImGui::IsKeyPressed(ImGuiKey_4, false)) app.pane = Pane::Settings;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("##RenaroRoot", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);

    const ImVec2 root_min = ImGui::GetWindowPos();
    const ImVec2 root_max = root_min + ImGui::GetWindowSize();
    const float rail_width = Px(232.0f);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(root_min, root_max, Col(kBg));
    // Soft accent glow behind the header.
    draw_list->AddRectFilledMultiColor(ImVec2(root_min.x + rail_width, root_min.y), ImVec2(root_max.x, root_min.y + Px(220.0f)), Col(kAccent, 0.07f), Col(kViolet, 0.035f), Col(kBg, 0.0f), Col(kBg, 0.0f));

    ImGui::PushStyleColor(ImGuiCol_ChildBg, kRail);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(14.0f), Px(18.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::BeginChild("##Rail", ImVec2(rail_width, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    DrawRail(app);
    ImGui::EndChild();
    // The rail child is opaque, so its edge line goes on the main side.
    draw_list->AddLine(ImVec2(root_min.x + rail_width + 0.5f, root_min.y), ImVec2(root_min.x + rail_width + 0.5f, root_max.y), Col(kBorderSoft));

    ImGui::SameLine(0.0f, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(24.0f), Px(18.0f)));
    ImGui::BeginChild("##Main", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    DrawHeader(app);
    VerticalSpace(2.0f);
    if (app.pane == Pane::Playground)
    {
        DrawPlayground(app);
    }
    else
    {
        ImGui::BeginChild("##Page", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_None);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Px(8.0f), Px(12.0f)));
        if (app.pane == Pane::Diagnostics)
            DrawDiagnostics(app);
        else if (app.pane == Pane::Trace)
            DrawTrace(app);
        else
            DrawSettings(app);
        ImGui::PopStyleVar();
        VerticalSpace(12.0f);
        ImGui::EndChild();
    }
    ImGui::EndChild();

    ImGui::End();
    DrawCommandPalette(app);
    DrawToast(app);
}

static bool FontFileExists(const char* path)
{
    return ::GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

// Merges Segoe MDL2 Assets (Windows 10) or Segoe Fluent Icons (Windows 11) into the font added last.
static void MergeIconFont(float size)
{
    static const ImWchar icon_ranges[] = { 0xE700, 0xF8FF, 0 };
    const char* candidates[] = { "C:\\Windows\\Fonts\\segmdl2.ttf", "C:\\Windows\\Fonts\\SegoeIcons.ttf" };
    for (const char* path : candidates)
    {
        if (!FontFileExists(path))
            continue;
        ImFontConfig config;
        config.MergeMode = true;
        config.GlyphRanges = icon_ranges;
        // Icon glyphs fill the whole em box on the baseline; shrink them and drop them onto the text's cap height.
        config.ExtraSizeScale = 0.8f;
        config.GlyphOffset = ImVec2(0.0f, 2.0f * size / 15.0f);
        if (ImGui::GetIO().Fonts->AddFontFromFileTTF(path, size, &config))
            g_icons_loaded = true;
        return;
    }
}

static ImFont* LoadUiFont(const char* path, float size, bool merge_icons)
{
    if (!FontFileExists(path))
        return nullptr;
    ImFont* font = ImGui::GetIO().Fonts->AddFontFromFileTTF(path, size);
    if (font && merge_icons)
        MergeIconFont(size);
    return font;
}

int main(int, char**)
{
    ImGui_ImplWin32_EnableDpiAwareness();
    const float main_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(::MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));
    g_ui_scale = main_scale;

    WNDCLASSEXW window_class = { sizeof(window_class), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"Renaro Model Playground", nullptr };
    ::RegisterClassExW(&window_class);
    HWND hwnd = ::CreateWindowW(window_class.lpszClassName, L"Renaro Model Playground", WS_OVERLAPPEDWINDOW, 100, 100, (int)(1500 * main_scale), (int)(900 * main_scale), nullptr, nullptr, window_class.hInstance, nullptr);
    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
        return 1;
    }

    // Dark title bar to match the UI. Both attributes are ignored by Windows versions that do not support them.
    const BOOL dark_title_bar = TRUE;
    ::DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark_title_bar, sizeof(dark_title_bar));
    const COLORREF caption_color = RGB(13, 16, 22);
    ::DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption_color, sizeof(caption_color));

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
    style.FontSizeBase = kFontBody;

    g_font_body = LoadUiFont("C:\\Windows\\Fonts\\segoeui.ttf", kFontBody, true);
    g_font_semibold = LoadUiFont("C:\\Windows\\Fonts\\seguisb.ttf", kFontBody, true);
    g_font_bold = LoadUiFont("C:\\Windows\\Fonts\\segoeuib.ttf", kFontBody, true);
    g_font_italic = LoadUiFont("C:\\Windows\\Fonts\\segoeuii.ttf", kFontBody, false);
    g_font_mono = LoadUiFont("C:\\Windows\\Fonts\\CascadiaMono.ttf", kFontMono, false);
    if (!g_font_mono)
        g_font_mono = LoadUiFont("C:\\Windows\\Fonts\\consola.ttf", kFontMono, false);
    if (!g_font_body)
        g_font_body = io.Fonts->AddFontDefault();
    if (!g_font_semibold)
        g_font_semibold = g_font_bold ? g_font_bold : g_font_body;
    if (!g_font_bold)
        g_font_bold = g_font_semibold;
    if (!g_font_italic)
        g_font_italic = g_font_body;
    if (!g_font_mono)
        g_font_mono = g_font_body;
    io.FontDefault = g_font_body;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
    LoadRenaroLogo();

    AppState app;
    app.server_path = FindLlamaServer().wstring();
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

    app.backend.Stop();
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
