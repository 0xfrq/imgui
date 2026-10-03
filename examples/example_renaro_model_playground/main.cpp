// Renaro Model Playground: a blue terminal-style local model workbench built with Dear ImGui.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define IMGUI_DEFINE_MATH_OPERATORS

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "llama_backend.h"

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
static ImFont* g_font_bold = nullptr;
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
    std::string imported_path;
    std::wstring server_path;
    std::string toast;
    char prompt[4096] = {};
    char command_search[256] = {};
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
    LlamaServerBackend backend;
    std::array<float, 12> bars = { 0.26f, 0.42f, 0.34f, 0.58f, 0.49f, 0.72f, 0.62f, 0.84f, 0.74f, 0.94f, 0.79f, 0.68f };

    AppState()
    {
        messages = {
            { "Renaro", "Import one or more local GGUF models, select the files you want to compare, then run the same prompt against each model.", false, true, {} }
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

static void AddMessage(AppState& app, const std::string& role, const std::string& text, bool assistant, bool system, const std::string& annotation = {})
{
    app.messages.push_back({ role, text, assistant, system, annotation });
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
    app.bars = { 0.26f, 0.42f, 0.34f, 0.58f, 0.49f, 0.72f, 0.62f, 0.84f, 0.74f, 0.94f, 0.79f, 0.68f };
}

static size_t SelectedModelCount(const AppState& app)
{
    return static_cast<size_t>(std::count_if(app.models.begin(), app.models.end(), [](const ModelEntry& model) { return model.selected; }));
}

static std::string FormatSeconds(double seconds)
{
    if (seconds <= 0.0)
        return "-";
    std::ostringstream value;
    value << std::fixed << std::setprecision(2) << seconds << " s";
    return value.str();
}

static std::string FormatRate(double rate)
{
    if (rate <= 0.0)
        return "-";
    std::ostringstream value;
    value << std::fixed << std::setprecision(1) << rate << " tokens per second";
    return value.str();
}

static std::string FormatMemory(double gigabytes)
{
    if (gigabytes <= 0.0)
        return "-";
    std::ostringstream value;
    value << std::fixed << std::setprecision(2) << gigabytes << " GB";
    return value.str();
}

static std::string FormatMilliseconds(double milliseconds)
{
    if (milliseconds <= 0.0)
        return "-";
    std::ostringstream value;
    value << std::fixed << std::setprecision(1) << milliseconds << " ms";
    return value.str();
}

static std::string FormatCount(int count)
{
    return count > 0 ? std::to_string(count) : "-";
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
    app.trace.push_back({ model, "server config", "-", "observed", server_detail });
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
        app.trace.push_back({ model, "metrics", "-", "available", result.metrics_summary });
}

static void SampleTelemetry(AppState& app, const LlamaRunResult& result)
{
    app.telemetry_sampled = result.ok;
    app.prompt_tokens_per_second = result.prompt_tokens_per_second;
    app.decode_tokens_per_second = result.decode_tokens_per_second;
    app.prompt_eval_seconds = result.prompt_eval_seconds;
    app.memory_gb = result.memory_gb;
    const float prompt_scale = std::clamp(static_cast<float>(result.prompt_tokens_per_second / 100.0), 0.12f, 0.98f);
    const float decode_scale = std::clamp(static_cast<float>(result.decode_tokens_per_second / 50.0), 0.12f, 0.98f);
    app.bars = { prompt_scale * 0.72f, prompt_scale * 0.88f, prompt_scale * 0.66f, decode_scale * 0.72f, decode_scale * 0.54f, decode_scale * 0.86f, decode_scale * 0.70f, decode_scale * 0.92f, decode_scale * 0.78f, decode_scale, decode_scale * 0.88f, decode_scale * 0.74f };
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
            AddMessage(app, result.model + " reasoning", {}, true, false, "streaming reasoning from llama.cpp");
            app.streaming_reasoning_message = static_cast<int>(app.messages.size() - 1);
        }
        app.messages[static_cast<size_t>(app.streaming_reasoning_message)].text += result.reasoning_delta;
    }
    if (!result.delta.empty())
    {
        if (app.streaming_response_message < 0)
        {
            AddMessage(app, result.model, {}, true, false, "streaming from llama.cpp");
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
        : "llama.cpp; prompt " + FormatRate(result.prompt_tokens_per_second) + "; decode " + FormatRate(result.decode_tokens_per_second);
    if (!result.reasoning.empty())
    {
        if (app.streaming_reasoning_message < 0)
        {
            AddMessage(app, result.model + " reasoning", result.reasoning, true, false, "llama.cpp reasoning content");
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
            AddMessage(app, result.model, result.text, true, false, annotation);
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
    AddMessage(app, "Renaro", "Import one or more local GGUF models, select the files you want to compare, then run the same prompt against each model.", false, true);
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
    AddMessage(app, "You", prompt, false, false);
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
        AddMessage(app, "Renaro", error, true, false, "llama.cpp adapter error");
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
                AddMessage(app, result.model, "Generation stopped before a response was produced.", true, false, "stopped by user");
        }
        else if (result.ok)
        {
            FinalizeStreamingMessages(app, result);
            SampleTelemetry(app, result);
        }
        else
        {
            ResetStreamingMessages(app, result.model);
            AddMessage(app, result.model, "Run failed: " + result.error, true, false, "llama.cpp adapter error");
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

static bool DangerButton(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f))
{
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.640f, 0.145f, 0.125f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.900f, 0.235f, 0.190f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.500f, 0.090f, 0.080f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.000f, 0.930f, 0.915f, 1.0f));
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
    const float logo_size = 96.0f;
    ImGui::SetCursorPosX(std::max(0.0f, (ImGui::GetContentRegionAvail().x - logo_size) * 0.5f));
    if (g_logo_srv)
        ImGui::Image((ImTextureID)(intptr_t)g_logo_srv, ImVec2(logo_size, logo_size));
    else
    {
        ImGui::Dummy(ImVec2(logo_size, logo_size));
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        const ImVec2 min = ImGui::GetItemRectMin();
        draw_list->AddRectFilled(min, min + ImVec2(logo_size, logo_size), ImGui::GetColorU32(kBlue), 12.0f);
        draw_list->AddText(min + ImVec2(42.0f, 32.0f), ImGui::GetColorU32(kBlueInk), "R");
    }
    ImGui::Dummy(ImVec2(0.0f, 26.0f));
    ImGui::TextColored(kMuted, "WORKSPACE");

    struct NavItem { Pane pane; const char* label; };
    const NavItem nav_items[] = {
        { Pane::Playground, "Playground" },
        { Pane::Diagnostics, "Diagnostics" },
        { Pane::Trace, "Trace log" },
        { Pane::Settings, "Settings" }
    };

    for (size_t index = 0; index < IM_ARRAYSIZE(nav_items); ++index)
    {
        const NavItem& item = nav_items[index];
        const bool active = app.pane == item.pane;
        ImGui::PushID(static_cast<int>(index));
        ImGui::PushStyleColor(ImGuiCol_Button, active ? ImVec4(0.120f, 0.230f, 0.380f, 1.0f) : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.120f, 0.185f, 0.290f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, active ? kBlueBright : kInkSoft);
        if (ImGui::Button(item.label, ImVec2(-1.0f, 34.0f)))
            app.pane = item.pane;
        ImGui::PopStyleColor(3);
        ImGui::PopID();
    }

    const float footer_height = 86.0f;
    const float footer_y = std::max(ImGui::GetCursorPosY(), ImGui::GetWindowHeight() - footer_height);
    ImGui::SetCursorPosY(footer_y);
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    ImGui::BeginGroup();
    DrawPulseDot(ImGui::GetCursorScreenPos() + ImVec2(5.0f, 8.0f), 3.0f, time, false);
    ImGui::Dummy(ImVec2(14.0f, 16.0f));
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::BeginGroup();
    ImGui::TextColored(kInkSoft, "llama.cpp bridge");
    ImGui::TextColored(kMuted, "%s", app.server_path.empty() ? "server path not set" : app.backend.Status().c_str());
    ImGui::EndGroup();
    ImGui::EndGroup();
    ImGui::TextColored(kMuted, "%zu model%s selected", SelectedModelCount(app), SelectedModelCount(app) == 1 ? "" : "s");
}

static void DrawTopBar(AppState& app, float time)
{
    ImGui::BeginChild("##TopBar", ImVec2(0.0f, 74.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    ImGui::TextColored(kMuted, "LOCAL INFERENCE");
    ImGui::TextColored(kInk, "Model bench");

    const float action_width = 322.0f;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - action_width));
    DrawPulseDot(ImGui::GetCursorScreenPos() + ImVec2(5.0f, 10.0f), 3.0f, time, app.run_busy);
    ImGui::Dummy(ImVec2(14.0f, 18.0f));
    ImGui::SameLine(0.0f, 4.0f);
    const std::string status = app.run_busy ? app.backend.Status() : (app.telemetry_sampled ? "live sample" : "ready for a run");
    ImGui::TextColored(app.run_busy ? kBlueBright : kMuted, "%s", status.c_str());
    ImGui::SameLine(0.0f, 16.0f);
    if (QuietButton("Find  Ctrl+K", ImVec2(92.0f, 30.0f)))
    {
        app.command_palette = true;
        app.command_focus_search = true;
    }
    ImGui::SameLine(0.0f, 6.0f);
    ImGui::BeginDisabled(app.run_busy);
    if (QuietButton("Clear run", ImVec2(90.0f, 30.0f)))
        ClearPlayground(app, true);
    ImGui::EndDisabled();
    ImGui::EndChild();
}

static void DrawModelShelf(AppState& app)
{
    PanelHeading("MODELS", "Library");
    ImGui::SameLine(ImGui::GetWindowWidth() - 82.0f);
    ImGui::TextColored(kBlueBright, "%zu selected", SelectedModelCount(app));
    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    if (app.models.empty())
    {
        ImGui::TextColored(kInkSoft, "No models imported");
        ImGui::TextWrapped("Add one or more GGUF files below. The library only shows files you attach to this playground.");
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
    }

    ImGui::BeginDisabled(app.run_busy);
    if (QuietButton("Select all", ImVec2(82.0f, 28.0f)))
    {
        size_t attached_count = 0;
        for (ModelEntry& model : app.models)
        {
            if (!model.path.empty())
            {
                model.selected = true;
                ++attached_count;
            }
        }
        ShowToast(app, attached_count == 0 ? "Import local model files first" : "All attached models selected");
    }
    ImGui::SameLine(0.0f, 6.0f);
    if (QuietButton("Clear", ImVec2(70.0f, 28.0f)))
    {
        for (ModelEntry& model : app.models)
            model.selected = false;
        ResetTelemetry(app);
        ShowToast(app, "Model selection cleared");
    }
    ImGui::Dummy(ImVec2(0.0f, 10.0f));

    for (size_t index = 0; index < app.models.size(); ++index)
    {
        const ModelEntry& model = app.models[index];
        const bool active = model.selected;
        ImGui::PushID(static_cast<int>(index));
        ImGui::PushStyleColor(ImGuiCol_Button, active ? ImVec4(0.120f, 0.230f, 0.380f, 1.0f) : kPanelDeep);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.145f, 0.250f, 0.390f, 1.0f));
        const float status_width = 52.0f;
        if (ImGui::Button(model.name.c_str(), ImVec2(std::max(90.0f, ImGui::GetContentRegionAvail().x - status_width), 34.0f)))
            ToggleModel(app, index);
        ImGui::PopStyleColor(2);
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::TextColored(active ? kBlueBright : kMuted, "%s", active ? "selected" : "staged");
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
                auto existing = std::find_if(app.models.begin(), app.models.end(), [&selected_path](const ModelEntry& item) { return item.path == selected_path; });
                if (existing == app.models.end())
                {
                    std::string display_name = name;
                    int duplicate_index = 2;
                    while (std::any_of(app.models.begin(), app.models.end(), [&display_name](const ModelEntry& item) { return item.name == display_name; }))
                        display_name = name + " (" + std::to_string(duplicate_index++) + ")";
                    app.models.push_back({ display_name, "local file, ready", selected_path, true });
                }
                else
                {
                    existing->path = selected_path;
                    existing->detail = "local file, ready";
                    existing->selected = true;
                }
                ResetTelemetry(app);
            }
            ShowToast(app, "Added " + name + " to the comparison set");
        }
    }
    if (!app.imported_path.empty())
    {
        ImGui::TextColored(kBlueBright, "Latest model file");
        ImGui::TextWrapped("%s", app.imported_path.c_str());
    }
    ImGui::EndDisabled();
    ImGui::Dummy(ImVec2(0.0f, 12.0f));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    ImGui::TextColored(kMuted, "COMPARISON MODE");
    ImGui::TextColored(kInkSoft, "Selected models run sequentially");
    ImGui::TextWrapped("One llama-server process is started per model so memory and token timings stay comparable.");
}

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

static void DrawMarkdownInline(const std::string& value)
{
    const std::vector<MarkdownSpan> spans = ParseMarkdownInline(value);
    if (spans.size() == 1 && spans.front().style == MarkdownSpanStyle::Normal)
    {
        ImGui::TextWrapped("%s", spans.front().text.c_str());
        return;
    }

    bool first = true;
    for (const MarkdownSpan& span : spans)
    {
        if (span.text.empty())
            continue;
        if (!first)
            ImGui::SameLine(0.0f, 0.0f);
        first = false;
        switch (span.style)
        {
        case MarkdownSpanStyle::Strong:
            ImGui::PushFont(g_font_bold);
            ImGui::TextColored(kInk, "%s", span.text.c_str());
            ImGui::PopFont();
            break;
        case MarkdownSpanStyle::Emphasis:
            ImGui::TextColored(kInkSoft, "%s", span.text.c_str());
            break;
        case MarkdownSpanStyle::Code:
            ImGui::PushFont(g_font_mono);
            ImGui::TextColored(kBlueBright, "%s", span.text.c_str());
            ImGui::PopFont();
            break;
        case MarkdownSpanStyle::Link:
            ImGui::TextColored(kBlueBright, "%s", span.text.c_str());
            ImGui::GetWindowDrawList()->AddLine(ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y - 1.0f), ImGui::GetItemRectMax(), ImGui::GetColorU32(kBlueBright));
            break;
        case MarkdownSpanStyle::Strike:
            ImGui::TextColored(kMuted, "%s", span.text.c_str());
            ImGui::GetWindowDrawList()->AddLine(ImVec2(ImGui::GetItemRectMin().x, (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f), ImVec2(ImGui::GetItemRectMax().x, (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f), ImGui::GetColorU32(kMuted));
            break;
        case MarkdownSpanStyle::Normal:
        default:
            ImGui::TextColored(kInkSoft, "%s", span.text.c_str());
            break;
        }
    }
    if (first)
        ImGui::TextUnformatted("");
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

static void DrawMarkdownCodeBlock(const std::string& code, const std::string& language, int block_index)
{
    const int line_count = static_cast<int>(std::count(code.begin(), code.end(), '\n')) + 1;
    const float height = std::min(280.0f, std::max(54.0f, 24.0f + line_count * ImGui::GetTextLineHeightWithSpacing()));
    const std::string child_id = "##MarkdownCode" + std::to_string(block_index);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanelDeep);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
    ImGui::BeginChild(child_id.c_str(), ImVec2(-1.0f, height), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoSavedSettings);
    if (!language.empty())
        ImGui::TextColored(kMuted, "%s", language.c_str());
    ImGui::PushFont(g_font_mono);
    ImGui::TextUnformatted(code.c_str());
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

static void DrawMarkdown(const std::string& markdown)
{
    std::istringstream lines(markdown);
    std::string line;
    std::string code;
    std::string language;
    bool in_code = false;
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
                DrawMarkdownCodeBlock(code, language, code_index++);
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
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
            continue;
        }
        if (IsMarkdownRule(trimmed))
        {
            ImGui::Separator();
            continue;
        }

        size_t heading_level = 0;
        while (heading_level < trimmed.size() && heading_level < 3 && trimmed[heading_level] == '#')
            ++heading_level;
        if (heading_level > 0 && heading_level < trimmed.size() && trimmed[heading_level] == ' ')
        {
            ImGui::PushFont(g_font_bold);
            ImGui::TextColored(heading_level == 1 ? kBlueBright : kInk, "%s", trimmed.substr(heading_level + 1).c_str());
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(0.0f, heading_level == 1 ? 4.0f : 2.0f));
            continue;
        }

        if (trimmed.front() == '>')
        {
            ImGui::TextColored(kBlue, "|");
            ImGui::SameLine(0.0f, 8.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, kInkSoft);
            DrawMarkdownInline(TrimCopy(trimmed.substr(1)));
            ImGui::PopStyleColor();
            continue;
        }

        size_t list_offset = std::string::npos;
        std::string list_marker;
        if (trimmed.rfind("- ", 0) == 0 || trimmed.rfind("* ", 0) == 0 || trimmed.rfind("+ ", 0) == 0)
        {
            list_offset = 2;
            list_marker = "-";
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
            ImGui::TextColored(kBlueBright, "%s", list_marker.c_str());
            ImGui::SameLine(0.0f, 8.0f);
            DrawMarkdownInline(trimmed.substr(list_offset));
            continue;
        }

        DrawMarkdownInline(line);
    }
    if (in_code)
        DrawMarkdownCodeBlock(code, language, code_index);
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
    DrawMarkdown(message.text);
    if (!message.annotation.empty())
    {
        ImGui::TextColored(kMuted, "%s", message.annotation.c_str());
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
    for (size_t index = 0; index < app.messages.size(); ++index)
    {
        ImGui::PushID(static_cast<int>(index));
        DrawMessage(app.messages[index]);
        ImGui::PopID();
    }
    if (app.run_busy)
    {
        ImGui::TextColored(kBlueBright, "%zu selected models", SelectedModelCount(app));
        ImGui::SameLine(0.0f, 10.0f);
        ImGui::TextColored(kMuted, "%s", app.backend.Status().c_str());
        ImGui::TextColored(kBlue, "|  |  |  |  |  |  |  |  |  |  |  |");
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 48.0f)
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_FrameBg, kPanelDeep);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.055f, 0.080f, 0.135f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.070f, 0.105f, 0.180f, 1.0f));
    ImGui::InputTextMultiline("##Prompt", app.prompt, sizeof(app.prompt), ImVec2(-1.0f, 70.0f), ImGuiInputTextFlags_AllowTabInput);
    const bool submit = ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Enter) && !ImGui::GetIO().KeyShift;
    ImGui::PopStyleColor(3);
    ImGui::TextColored(kMuted, "Enter sends; Shift+Enter adds a line");
    const float prompt_action_width = app.run_busy ? 145.0f : 118.0f;
    ImGui::SameLine(ImGui::GetWindowWidth() - prompt_action_width - 12.0f);
    if (app.run_busy)
    {
        ImGui::BeginDisabled(app.stop_requested);
        if (DangerButton("Stop generating##prompt", ImVec2(prompt_action_width, 30.0f)))
            StopRun(app);
        ImGui::EndDisabled();
    }
    else
    {
        if (PrimaryButton("Run selected##prompt", ImVec2(prompt_action_width, 30.0f)) || submit)
            StartRun(app, app.prompt);
    }
}

static void DrawDiagnostics(const AppState& app)
{
    ImGui::TextColored(kBlueBright, "DIAGNOSTICS");
    ImGui::TextColored(kInk, "Run evidence");
    ImGui::TextWrapped("Each row is collected from llama.cpp, its optional slot endpoint, and the server process. Use this surface to compare token counts, timing, memory, CPU, cache, and response metadata per model.");
    ImGui::Dummy(ImVec2(0.0f, 14.0f));
    if (app.comparison_results.empty())
    {
        ImGui::TextColored(kMuted, "Diagnostics begin with the first llama.cpp run.");
        return;
    }
    if (ImGui::BeginTable("##ComparisonTable", 11, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollX))
    {
        ImGui::TableSetupColumn("model");
        ImGui::TableSetupColumn("status");
        ImGui::TableSetupColumn("prompt tokens per second");
        ImGui::TableSetupColumn("decode tokens per second");
        ImGui::TableSetupColumn("prompt tok");
        ImGui::TableSetupColumn("generated tok");
        ImGui::TableSetupColumn("prompt ms");
        ImGui::TableSetupColumn("decode ms");
        ImGui::TableSetupColumn("request");
        ImGui::TableSetupColumn("memory");
        ImGui::TableSetupColumn("finish");
        ImGui::TableHeadersRow();
        for (const LlamaRunResult& result : app.comparison_results)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(kInk, "%s", result.model.c_str());
            ImGui::TableSetColumnIndex(1);
            const char* status_label = result.cancelled ? "stopped" : (result.ok ? "measured" : "error");
            const ImVec4 status_color = result.cancelled ? kMuted : (result.ok ? kBlueBright : kError);
            ImGui::TextColored(status_color, "%s", status_label);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextColored(kInkSoft, "%s", FormatRate(result.prompt_tokens_per_second).c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextColored(kInkSoft, "%s", FormatRate(result.decode_tokens_per_second).c_str());
            ImGui::TableSetColumnIndex(4);
            ImGui::TextColored(kInkSoft, "%s", FormatCount(result.prompt_tokens).c_str());
            ImGui::TableSetColumnIndex(5);
            ImGui::TextColored(kInkSoft, "%s", FormatCount(result.predicted_tokens).c_str());
            ImGui::TableSetColumnIndex(6);
            ImGui::TextColored(kInkSoft, "%s", FormatMilliseconds(result.prompt_ms).c_str());
            ImGui::TableSetColumnIndex(7);
            ImGui::TextColored(kInkSoft, "%s", FormatMilliseconds(result.predicted_ms).c_str());
            ImGui::TableSetColumnIndex(8);
            ImGui::TextColored(kInkSoft, "%s", FormatSeconds(result.request_seconds).c_str());
            ImGui::TableSetColumnIndex(9);
            ImGui::TextColored(kInkSoft, "%s", FormatMemory(result.memory_gb).c_str());
            ImGui::TableSetColumnIndex(10);
            ImGui::TextColored(kInkSoft, "%s", result.cancelled ? "stopped by user" : (result.ok ? result.finish_reason.c_str() : "error"));
        }
        ImGui::EndTable();
    }

    if (app.has_latest_result)
    {
        const LlamaRunResult& result = app.latest_result;
        ImGui::Dummy(ImVec2(0.0f, 14.0f));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        ImGui::TextColored(kMuted, "LATEST RESPONSE DETAIL");
        ImGui::TextColored(kInk, "%s", result.model.c_str());
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        if (ImGui::BeginTable("##LatestDetailTable", 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("signal");
            ImGui::TableSetupColumn("value");
            ImGui::TableHeadersRow();
            const auto detail = [](const char* label, const std::string& value)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(kMuted, "%s", label);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextWrapped("%s", value.c_str());
            };
            detail("run status", result.cancelled ? "stopped by user" : (result.ok ? "completed" : "error"));
            detail("response id", result.response_id.empty() ? "-" : result.response_id);
            detail("response type", result.response_object.empty() ? "-" : result.response_object);
            detail("server model", result.response_model);
            detail("model alias", result.model_alias.empty() ? "-" : result.model_alias);
            detail("model path", result.model_path.empty() ? "-" : result.model_path);
            detail("build info", result.build_info.empty() ? "-" : result.build_info);
            detail("chat template", result.chat_template.empty() ? "-" : result.chat_template);
            detail("fingerprint", result.system_fingerprint.empty() ? "-" : result.system_fingerprint);
            detail("HTTP status", std::to_string(result.http_status));
            detail("timing source", result.timings_source);
            detail("server load", FormatSeconds(result.server_load_seconds));
            detail("request wall time", FormatSeconds(result.request_seconds));
            detail("timing totals", FormatMilliseconds(result.total_ms) + ", load " + FormatMilliseconds(result.load_ms) + ", start " + FormatMilliseconds(result.start_ms));
            detail("prompt eval", FormatMilliseconds(result.prompt_ms) + ", " + FormatRate(result.prompt_tokens_per_second) + ", " + FormatMilliseconds(result.prompt_ms_per_token) + " per token");
            detail("decode", FormatMilliseconds(result.predicted_ms) + ", " + FormatRate(result.decode_tokens_per_second) + ", " + FormatMilliseconds(result.predicted_ms_per_token) + " per token");
            detail("sampling", "tokenize " + FormatMilliseconds(result.tokenize_ms) + ", sample " + FormatMilliseconds(result.sample_ms));
            detail("tokens", "prompt " + FormatCount(result.prompt_tokens) + ", generated " + FormatCount(result.predicted_tokens) + ", total " + FormatCount(result.total_tokens) + ", reasoning " + FormatCount(result.reasoning_tokens));
            detail("prediction accounting", "accepted " + FormatCount(result.accepted_prediction_tokens) + ", rejected " + FormatCount(result.rejected_prediction_tokens));
            detail("server config", "context " + FormatCount(result.context_size) + ", batch " + FormatCount(result.batch_size) + ", ubatch " + FormatCount(result.ubatch_size) + ", threads " + FormatCount(result.threads) + ", batch threads " + FormatCount(result.threads_batch) + ", GPU layers " + FormatCount(result.gpu_layers));
            detail("context and cache", FormatCount(result.context_size) + ", cached " + FormatCount(result.tokens_cached) + ", reused " + (result.prompt_cache_reused ? "yes" : "no"));
            detail("slot state", result.slot_info_available ? ("id " + std::to_string(result.slot_id) + ", " + (result.slot_state.empty() ? "unknown" : result.slot_state)) : "not exposed by server");
            detail("slot counters", "past " + FormatCount(result.n_past) + ", prompt " + FormatCount(result.n_prompt_tokens) + ", decoded " + FormatCount(result.n_decoded) + ", cache " + FormatCount(result.n_cache_tokens) + ", limit " + FormatCount(result.n_predict_limit));
            detail("stop flags", std::string(result.stopped_eos ? "EOS " : "") + (result.stopped_word ? "word " : "") + (result.stopped_limit ? "limit " : "") + (result.slot_truncated ? "truncated" : "none"));
            detail("process CPU", FormatSeconds(result.cpu_seconds));
            detail("memory", FormatMemory(result.memory_gb) + ", peak " + FormatMemory(result.peak_memory_gb) + ", private " + FormatMemory(result.private_memory_gb));
            detail("response", FormatCount(result.response_bytes) + " bytes, finish " + result.finish_reason);
            detail("metrics", result.metrics_available ? result.metrics_summary : "not exposed by server");
            detail("metric time totals", "prompt " + FormatSeconds(result.metrics_prompt_seconds_total) + ", predicted " + FormatSeconds(result.metrics_predicted_seconds_total));
            ImGui::EndTable();
        }
    }
    for (const LlamaRunResult& result : app.comparison_results)
    {
        if (!result.ok && !result.cancelled && !result.error.empty())
            ImGui::TextColored(kError, "%s: %s", result.model.c_str(), result.error.c_str());
    }
}

static void DrawTrace(const AppState& app)
{
    ImGui::TextColored(kBlueBright, "TRACE");
    ImGui::TextColored(kInk, "Per-model run sequence");
    ImGui::TextWrapped("Server startup, slot timing, token accounting, response status, and process resource snapshots are appended here for every model.");
    ImGui::Dummy(ImVec2(0.0f, 12.0f));
    if (app.trace.empty())
    {
        ImGui::TextColored(kMuted, "Trace entries appear after a llama.cpp run.");
        return;
    }
    if (ImGui::BeginTable("##TraceTable", 5, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollX))
    {
        ImGui::TableSetupColumn("model");
        ImGui::TableSetupColumn("event");
        ImGui::TableSetupColumn("duration");
        ImGui::TableSetupColumn("status");
        ImGui::TableSetupColumn("detail");
        ImGui::TableHeadersRow();
        for (const TraceEntry& entry : app.trace)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(kInk, "%s", entry.model.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(kInkSoft, "%s", entry.event.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextColored(kInk, "%s", entry.duration.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextColored(entry.status == "error" ? kError : (entry.status == "stopped" ? kMuted : kBlueBright), "%s", entry.status.c_str());
            ImGui::TableSetColumnIndex(4);
            ImGui::TextWrapped("%s", entry.detail.c_str());
        }
        ImGui::EndTable();
    }
}

static void DrawSettings(AppState& app)
{
    ImGui::TextColored(kBlueBright, "SETTINGS");
    ImGui::TextColored(kInk, "Engine controls");
    ImGui::TextWrapped("These controls are passed to llama-server for every selected model. Models are run one at a time so the measurements stay comparable.");
    ImGui::Dummy(ImVec2(0.0f, 14.0f));
    ImGui::TextColored(kMuted, "REQUEST");
    ImGui::SliderFloat("Temperature##settings", &app.temperature, 0.0f, 1.5f, "%.2f");
    ImGui::SliderInt("Max output tokens##settings", &app.max_tokens, 16, 8192);
    ImGui::Checkbox("Reuse prompt cache##settings", &app.cache_prompt);
    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    ImGui::TextColored(kMuted, "SERVER RUNTIME");
    ImGui::SliderInt("Threads##settings", &app.threads, 1, 64);
    ImGui::SliderInt("Batch threads##settings", &app.threads_batch, 1, 64);
    ImGui::SliderInt("GPU layers##settings", &app.gpu_layers, 0, 128);
    ImGui::SliderInt("Batch size##settings", &app.batch_size, 32, 4096);
    ImGui::SliderInt("Micro batch size##settings", &app.ubatch_size, 32, 4096);
    ImGui::SliderInt("Seed##settings", &app.seed, 0, 9999);
    ImGui::Checkbox("Flash attention##settings", &app.flash_attention);
    ImGui::Checkbox("Memory map weights##settings", &app.mmap);
    ImGui::Checkbox("Lock weights in memory##settings", &app.mlock);
    ImGui::Checkbox("Keep KV cache on GPU##settings", &app.kv_offload);
    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    ImGui::TextColored(kMuted, "LLAMA-SERVER BINARY");
    const std::string server_path = app.server_path.empty() ? "Not selected" : WideToUtf8(app.server_path);
    ImGui::PushFont(g_font_mono);
    ImGui::TextWrapped("%s", server_path.c_str());
    ImGui::PopFont();
    if (QuietButton("Select llama-server.exe", ImVec2(-1.0f, 30.0f)))
    {
        std::wstring selected_path;
        if (ChooseServerFile(selected_path))
        {
            app.server_path = selected_path;
            ShowToast(app, "llama-server.exe selected");
        }
    }
    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    ImGui::TextColored(kMuted, "Current command");
    ImGui::PushFont(g_font_mono);
    ImGui::TextWrapped("%s", CurrentRunCommand(app).c_str());
    ImGui::PopFont();
}

static void DrawWorkspace(AppState& app)
{
    const size_t selected_count = SelectedModelCount(app);
    const std::string selected_label = selected_count == 0
        ? "No models selected"
        : std::to_string(selected_count) + (selected_count == 1 ? " model selected" : " models selected");
    PanelHeading("MODEL COMPARISON", selected_label.c_str());
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::TextColored(kMuted, "same prompt, sequential runs");
    const float action_width = app.run_busy ? 280.0f : 235.0f;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - action_width));
    if (QuietButton("Copy run command", ImVec2(112.0f, 30.0f)))
    {
        const std::string command = CurrentRunCommand(app);
        ImGui::SetClipboardText(command.c_str());
        ShowToast(app, "Run command copied");
    }
    ImGui::SameLine(0.0f, 6.0f);
    if (app.run_busy)
    {
        ImGui::BeginDisabled(app.stop_requested);
        if (DangerButton("Stop generating##header", ImVec2(138.0f, 30.0f)))
            StopRun(app);
        ImGui::EndDisabled();
    }
    else if (PrimaryButton("Run selected##header", ImVec2(112.0f, 30.0f)))
    {
        StartRun(app, {});
    }

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
    ImGui::TextColored(kMuted, "%s", app.run_busy ? "collecting" : (app.telemetry_sampled ? "llama.cpp" : "awaiting run"));
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
    const bool latest_stopped = app.has_latest_result && app.latest_result.cancelled;
    const bool latest_error = app.has_latest_result && !app.latest_result.ok && !latest_stopped;
    const ImVec4 telemetry_status_color = app.run_busy ? kBlueBright : (latest_stopped ? kMuted : (latest_error ? kError : (app.telemetry_sampled ? kBlueBright : kMuted)));
    const char* telemetry_status = app.run_busy ? "sampling" : (latest_stopped ? "stopped" : (latest_error ? "error" : (app.telemetry_sampled ? "measured" : "waiting")));
    ImGui::TextColored(telemetry_status_color, "%s", telemetry_status);
    ImGui::Dummy(ImVec2(0.0f, 10.0f));

    const std::string prompt_rate = FormatRate(app.prompt_tokens_per_second);
    const std::string decode_rate = FormatRate(app.decode_tokens_per_second);
    const std::string prompt_time = FormatSeconds(app.prompt_eval_seconds);
    const std::string memory = FormatMemory(app.memory_gb);
    if (ImGui::BeginTable("##MetricGrid", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        DrawMetric("Prompt tokens per second", prompt_rate.c_str(), app.telemetry_sampled ? "llama.cpp timing" : "run pending");
        ImGui::TableSetColumnIndex(1);
        DrawMetric("Decode tokens per second", decode_rate.c_str(), app.telemetry_sampled ? "llama.cpp timing" : "run pending");
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        DrawMetric("Prompt eval", prompt_time.c_str(), app.telemetry_sampled ? "time before decode" : "not measured");
        ImGui::TableSetColumnIndex(1);
        DrawMetric("Memory", memory.c_str(), app.telemetry_sampled ? "server working set" : "not measured");
        ImGui::EndTable();
    }

    ImGui::Dummy(ImVec2(0.0f, 12.0f));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    ImGui::TextColored(kMuted, "LLAMA.CPP SIGNALS");
    if (!app.has_latest_result)
    {
        ImGui::TextColored(kMuted, "Waiting for the first response.");
    }
    else if (ImGui::BeginTable("##LlamaSignalTable", 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("signal");
        ImGui::TableSetupColumn("value");
        ImGui::TableHeadersRow();
        const LlamaRunResult& result = app.latest_result;
        const auto signal = [](const char* label, const std::string& value)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(kMuted, "%s", label);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextWrapped("%s", value.c_str());
        };
        signal("prompt tokens", FormatCount(result.prompt_tokens));
        signal("generated tokens", FormatCount(result.predicted_tokens));
        signal("total tokens", FormatCount(result.total_tokens));
        signal("reasoning tokens", FormatCount(result.reasoning_tokens));
        signal("prompt timing", FormatMilliseconds(result.prompt_ms) + ", " + FormatMilliseconds(result.prompt_ms_per_token) + " per token");
        signal("decode timing", FormatMilliseconds(result.predicted_ms) + ", " + FormatMilliseconds(result.predicted_ms_per_token) + " per token");
        signal("request wall", FormatSeconds(result.request_seconds));
        signal("server load", FormatSeconds(result.server_load_seconds));
        signal("context and cache", FormatCount(result.context_size) + ", " + FormatCount(result.tokens_cached) + (result.prompt_cache_reused ? ", reused" : ""));
        signal("server config", "batch " + FormatCount(result.batch_size) + ", ubatch " + FormatCount(result.ubatch_size) + ", threads " + FormatCount(result.threads) + ", GPU " + FormatCount(result.gpu_layers));
        signal("slot", result.slot_info_available ? (std::to_string(result.slot_id) + ", " + (result.slot_state.empty() ? "unknown" : result.slot_state)) : "not exposed");
        signal("process CPU", FormatSeconds(result.cpu_seconds));
        signal("working and peak", FormatMemory(result.memory_gb) + ", " + FormatMemory(result.peak_memory_gb));
        signal("response", FormatCount(result.response_bytes) + " bytes, HTTP " + std::to_string(result.http_status));
        signal("finish", result.finish_reason.empty() ? "-" : result.finish_reason);
        signal("metrics", result.metrics_available ? result.metrics_summary : "not exposed");
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
    else if (std::strcmp(action, "run-selected") == 0)
        StartRun(app, {});
    else if (std::strcmp(action, "copy") == 0)
    {
        const std::string command = CurrentRunCommand(app);
        ImGui::SetClipboardText(command.c_str());
        ShowToast(app, "Run command copied");
    }
    else if (std::strcmp(action, "clear-playground") == 0)
        ClearPlayground(app, true);
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
        { "Open trace log", "Inspect the llama.cpp event sequence", "trace" },
        { "Run selected models", "Send the prompt through the comparison set", "run-selected" },
        { "Copy run command", "Take the local command with you", "copy" },
        { "Clear playground", "Clear messages and measured results", "clear-playground" }
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
    ImGui::TextColored(kMuted, "Ctrl+K opens; Esc closes; 1-4 switch views");
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
    AdvanceBackend(app, io.DeltaTime);

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
    if (::GetFileAttributesA("C:\\Windows\\Fonts\\segoeuib.ttf") != INVALID_FILE_ATTRIBUTES)
        g_font_bold = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 16.0f);
    if (::GetFileAttributesA("C:\\Windows\\Fonts\\consola.ttf") != INVALID_FILE_ATTRIBUTES)
        g_font_mono = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", 13.0f);
    if (!g_font_body)
        g_font_body = io.Fonts->AddFontDefault();
    if (!g_font_bold)
        g_font_bold = g_font_body;
    if (!g_font_mono)
        g_font_mono = g_font_body;

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
