#include "llama_backend.h"

#include <windows.h>
#include <psapi.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iomanip>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace
{
static std::wstring QuoteArg(const std::wstring& value)
{
    std::wstring result = L"\"";
    for (wchar_t character : value)
    {
        if (character == L'\"')
            result += L"\\\"";
        else
            result += character;
    }
    result += L"\"";
    return result;
}

static std::string JsonEscape(const std::string& value)
{
    std::string result;
    result.reserve(value.size() + 16);
    for (unsigned char character : value)
    {
        switch (character)
        {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (character < 0x20)
                result += ' ';
            else
                result += static_cast<char>(character);
            break;
        }
    }
    return result;
}

static void AppendUtf8(std::string& output, unsigned int codepoint)
{
    if (codepoint <= 0x7f)
        output.push_back(static_cast<char>(codepoint));
    else if (codepoint <= 0x7ff)
    {
        output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
    else if (codepoint <= 0xffff)
    {
        output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
    else
    {
        output.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
}

static bool ParseHex4(const std::string& value, size_t offset, unsigned int& result)
{
    if (offset + 4 > value.size())
        return false;
    result = 0;
    for (size_t index = 0; index < 4; ++index)
    {
        const char character = value[offset + index];
        result <<= 4;
        if (character >= '0' && character <= '9') result |= static_cast<unsigned int>(character - '0');
        else if (character >= 'a' && character <= 'f') result |= static_cast<unsigned int>(character - 'a' + 10);
        else if (character >= 'A' && character <= 'F') result |= static_cast<unsigned int>(character - 'A' + 10);
        else return false;
    }
    return true;
}

static bool ExtractJsonString(const std::string& json, const char* key, std::string& value)
{
    const size_t key_position = json.find(key);
    if (key_position == std::string::npos)
        return false;
    size_t cursor = json.find(':', key_position + std::strlen(key));
    if (cursor == std::string::npos)
        return false;
    ++cursor;
    while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == '\t' || json[cursor] == '\r' || json[cursor] == '\n'))
        ++cursor;
    if (cursor >= json.size() || json[cursor] != '"')
        return false;
    ++cursor;

    value.clear();
    while (cursor < json.size())
    {
        const char character = json[cursor++];
        if (character == '"')
            return true;
        if (character != '\\')
        {
            value.push_back(character);
            continue;
        }
        if (cursor >= json.size())
            return false;
        const char escaped = json[cursor++];
        switch (escaped)
        {
        case '"': value.push_back('"'); break;
        case '\\': value.push_back('\\'); break;
        case '/': value.push_back('/'); break;
        case 'b': value.push_back('\b'); break;
        case 'f': value.push_back('\f'); break;
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        case 'u':
        {
            unsigned int codepoint = 0;
            if (!ParseHex4(json, cursor, codepoint))
                return false;
            cursor += 4;
            AppendUtf8(value, codepoint);
            break;
        }
        default:
            value.push_back(escaped);
            break;
        }
    }
    return false;
}

static bool ExtractJsonStringLast(const std::string& json, const char* key, std::string& value)
{
    size_t search_from = 0;
    bool found = false;
    while (true)
    {
        const size_t position = json.find(key, search_from);
        if (position == std::string::npos)
            break;
        std::string candidate;
        if (ExtractJsonString(json.substr(position), key, candidate))
        {
            value = std::move(candidate);
            found = true;
        }
        search_from = position + std::strlen(key);
    }
    return found;
}

static bool ExtractJsonNumber(const std::string& json, const char* key, double& value)
{
    const size_t key_position = json.find(key);
    if (key_position == std::string::npos)
        return false;
    size_t cursor = json.find(':', key_position + std::strlen(key));
    if (cursor == std::string::npos)
        return false;
    ++cursor;
    while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == '\t' || json[cursor] == '\r' || json[cursor] == '\n'))
        ++cursor;
    char* end = nullptr;
    value = std::strtod(json.c_str() + cursor, &end);
    return end != json.c_str() + cursor;
}

static bool ExtractJsonBool(const std::string& json, const char* key, bool& value)
{
    const size_t key_position = json.find(key);
    if (key_position == std::string::npos)
        return false;
    size_t cursor = json.find(':', key_position + std::strlen(key));
    if (cursor == std::string::npos)
        return false;
    ++cursor;
    while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == '\t' || json[cursor] == '\r' || json[cursor] == '\n'))
        ++cursor;
    if (json.compare(cursor, 4, "true") == 0)
    {
        value = true;
        return true;
    }
    if (json.compare(cursor, 5, "false") == 0)
    {
        value = false;
        return true;
    }
    return false;
}

static bool ExtractPrometheusNumber(const std::string& metrics, const char* metric, double& value)
{
    const size_t metric_length = std::strlen(metric);
    size_t line_start = 0;
    while (line_start < metrics.size())
    {
        const size_t line_end = metrics.find('\n', line_start);
        const size_t length = line_end == std::string::npos ? metrics.size() - line_start : line_end - line_start;
        const std::string line = metrics.substr(line_start, length);
        const bool metric_name_matches = line.compare(0, metric_length, metric) == 0
            && (line.size() == metric_length || line[metric_length] == ' ' || line[metric_length] == '\t' || line[metric_length] == '{');
        if (metric_name_matches)
        {
            size_t cursor = metric_length;
            while (cursor < line.size() && line[cursor] != ' ' && line[cursor] != '\t')
                ++cursor;
            while (cursor < line.size() && (line[cursor] == ' ' || line[cursor] == '\t'))
                ++cursor;
            char* end = nullptr;
            value = std::strtod(line.c_str() + cursor, &end);
            if (end != line.c_str() + cursor)
                return true;
        }
        if (line_end == std::string::npos)
            break;
        line_start = line_end + 1;
    }
    return false;
}

static std::string Truncate(const std::string& value, size_t limit)
{
    if (value.size() <= limit)
        return value;
    return value.substr(0, limit) + "...";
}

static std::string TrimWhitespaceCopy(const std::string& value)
{
    const size_t start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
        return {};
    const size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

static bool HttpRequest(int port, const wchar_t* method, const wchar_t* path, const std::string& body, std::string& response, DWORD& status_code, std::string& error, const std::function<void(const std::string&)>& on_chunk = {})
{
    response.clear();
    status_code = 0;
    HINTERNET session = ::WinHttpOpen(L"Renaro Model Playground/0.1", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
    {
        error = "WinHTTP could not open a session";
        return false;
    }
    // Generation can take longer than connection setup, especially while a large GGUF warms up.
    ::WinHttpSetTimeouts(session, 1500, 1500, 1500, 300000);
    HINTERNET connection = ::WinHttpConnect(session, L"127.0.0.1", static_cast<INTERNET_PORT>(port), 0);
    if (!connection)
    {
        ::WinHttpCloseHandle(session);
        error = "Could not connect to llama-server";
        return false;
    }
    HINTERNET request = ::WinHttpOpenRequest(connection, method, path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!request)
    {
        ::WinHttpCloseHandle(connection);
        ::WinHttpCloseHandle(session);
        error = "Could not create the llama-server request";
        return false;
    }

    const wchar_t* headers = body.empty() ? nullptr : (on_chunk ? L"Content-Type: application/json\r\nAccept: text/event-stream\r\n" : L"Content-Type: application/json\r\n");
    const LPVOID data = body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data());
    const DWORD data_length = static_cast<DWORD>(body.size());
    bool ok = ::WinHttpSendRequest(request, headers, headers ? static_cast<DWORD>(-1L) : 0, data, data_length, data_length, 0) != FALSE;
    if (ok)
        ok = ::WinHttpReceiveResponse(request, nullptr) != FALSE;
    if (!ok)
    {
        error = "llama-server did not answer the request";
        ::WinHttpCloseHandle(request);
        ::WinHttpCloseHandle(connection);
        ::WinHttpCloseHandle(session);
        return false;
    }

    DWORD status_size = sizeof(status_code);
    ::WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_size, WINHTTP_NO_HEADER_INDEX);
    DWORD available = 0;
    while (::WinHttpQueryDataAvailable(request, &available) && available > 0)
    {
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!::WinHttpReadData(request, chunk.data(), available, &read))
            break;
        chunk.resize(read);
        response += chunk;
        if (on_chunk)
            on_chunk(chunk);
    }

    ::WinHttpCloseHandle(request);
    ::WinHttpCloseHandle(connection);
    ::WinHttpCloseHandle(session);
    if (status_code < 200 || status_code >= 300)
    {
        std::ostringstream message;
        message << "llama-server returned HTTP " << status_code;
        if (!response.empty())
            message << ": " << Truncate(response, 240);
        error = message.str();
        return false;
    }
    return true;
}

struct ProcessSnapshot
{
    double working_gb = 0.0;
    double peak_working_gb = 0.0;
    double private_gb = 0.0;
};

static ProcessSnapshot ReadProcessSnapshot(HANDLE process)
{
    ProcessSnapshot snapshot;
    if (!process)
        return snapshot;
    PROCESS_MEMORY_COUNTERS_EX counters = {};
    if (!::GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
        return snapshot;
    snapshot.working_gb = static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0 * 1024.0);
    snapshot.peak_working_gb = static_cast<double>(counters.PeakWorkingSetSize) / (1024.0 * 1024.0 * 1024.0);
    snapshot.private_gb = static_cast<double>(counters.PrivateUsage) / (1024.0 * 1024.0 * 1024.0);
    return snapshot;
}

static double ReadProcessCpuSeconds(HANDLE process)
{
    if (!process)
        return 0.0;
    FILETIME creation = {};
    FILETIME exit = {};
    FILETIME kernel = {};
    FILETIME user = {};
    if (!::GetProcessTimes(process, &creation, &exit, &kernel, &user))
        return 0.0;
    ULARGE_INTEGER kernel_ticks = {};
    ULARGE_INTEGER user_ticks = {};
    kernel_ticks.LowPart = kernel.dwLowDateTime;
    kernel_ticks.HighPart = kernel.dwHighDateTime;
    user_ticks.LowPart = user.dwLowDateTime;
    user_ticks.HighPart = user.dwHighDateTime;
    return static_cast<double>(kernel_ticks.QuadPart + user_ticks.QuadPart) / 10000000.0;
}
}

LlamaServerBackend::~LlamaServerBackend()
{
    Stop();
}

bool LlamaServerBackend::StartComparison(const std::vector<LlamaModelJob>& models, const std::string& prompt, const LlamaRunConfig& config, std::string& error)
{
    error.clear();
    if (models.empty())
    {
        error = "Select at least one model first";
        return false;
    }
    if (config.server_path.empty())
    {
        error = "Select llama-server.exe in Settings first";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (busy_)
        {
            error = "A model run is already in progress";
            return false;
        }
    }
    if (worker_.joinable())
        worker_.join();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        results_.clear();
        stop_requested_ = false;
        busy_ = true;
        status_ = "starting";
    }
    worker_ = std::thread(&LlamaServerBackend::RunComparison, this, models, prompt, config);
    return true;
}

void LlamaServerBackend::Stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_requested_ = true;
    }
    if (worker_.joinable())
        worker_.join();
    StopServer();
    std::lock_guard<std::mutex> lock(mutex_);
    busy_ = false;
    status_ = "ready";
}

bool LlamaServerBackend::PopResult(LlamaRunResult& result)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (results_.empty())
        return false;
    result = std::move(results_.front());
    results_.pop_front();
    return true;
}

bool LlamaServerBackend::IsBusy() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return busy_;
}

std::string LlamaServerBackend::Status() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void LlamaServerBackend::RunComparison(std::vector<LlamaModelJob> models, std::string prompt, LlamaRunConfig config)
{
    for (size_t index = 0; index < models.size(); ++index)
    {
        if (IsStopRequested())
            break;
        LlamaRunResult result;
        result.model = models[index].name;
        result.last = index + 1 == models.size();
        RunModel(models[index], prompt, config, result);
        PushResult(std::move(result));
    }
    StopServer();
    std::lock_guard<std::mutex> lock(mutex_);
    busy_ = false;
    status_ = "ready";
}

bool LlamaServerBackend::RunModel(const LlamaModelJob& model, const std::string& prompt, const LlamaRunConfig& config, LlamaRunResult& result)
{
    if (model.path.empty())
    {
        result.error = "No local model file is attached to this entry";
        return false;
    }

    SetStatus("loading " + model.name);
    std::string error;
    const auto server_started = std::chrono::steady_clock::now();
    if (!StartServer(model, config, error))
    {
        result.error = error;
        return false;
    }
    if (!WaitForHealth(config, error))
    {
        result.error = error;
        StopServer();
        return false;
    }
    result.server_load_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - server_started).count();

    SetStatus("streaming " + model.name);
    std::ostringstream body;
    body << "{\"model\":\"local\",\"messages\":[{\"role\":\"user\",\"content\":\"" << JsonEscape(prompt)
         << "\"}],\"temperature\":" << std::fixed << std::setprecision(3) << config.temperature
         << ",\"max_tokens\":" << config.max_tokens << ",\"seed\":" << config.seed
         << ",\"cache_prompt\":" << (config.cache_prompt ? "true" : "false")
         << ",\"stream\":true}";
    std::string response;
    DWORD status_code = 0;
    std::string stream_buffer;
    std::string stream_text;
    std::string stream_reasoning;
    std::string pending_delta;
    std::string pending_reasoning_delta;
    bool stream_emitted = false;
    auto last_stream_emit = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    const auto flush_stream_update = [&]()
    {
        if (pending_delta.empty() && pending_reasoning_delta.empty())
            return;
        LlamaRunResult update = result;
        update.partial = true;
        update.streaming = true;
        update.ok = true;
        update.delta = std::move(pending_delta);
        update.reasoning_delta = std::move(pending_reasoning_delta);
        pending_delta.clear();
        pending_reasoning_delta.clear();
        update.response_bytes = static_cast<int>(response.size());
        PushResult(std::move(update));
        stream_emitted = true;
        last_stream_emit = std::chrono::steady_clock::now();
    };
    const auto on_stream_chunk = [&](const std::string& chunk)
    {
        stream_buffer += chunk;
        size_t newline = std::string::npos;
        while ((newline = stream_buffer.find('\n')) != std::string::npos)
        {
            std::string line = stream_buffer.substr(0, newline);
            stream_buffer.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            line = TrimWhitespaceCopy(line);
            if (line.rfind("data:", 0) == 0)
                line = TrimWhitespaceCopy(line.substr(5));
            if (line.empty() || line == "[DONE]")
                continue;

            std::string delta;
            std::string reasoning_delta;
            ExtractJsonString(line, "\"content\"", delta);
            ExtractJsonString(line, "\"reasoning_content\"", reasoning_delta);
            if (delta.empty() && reasoning_delta.empty())
                continue;
            stream_text += delta;
            stream_reasoning += reasoning_delta;
            pending_delta += delta;
            pending_reasoning_delta += reasoning_delta;
            const double since_last_emit = std::chrono::duration<double>(std::chrono::steady_clock::now() - last_stream_emit).count();
            if (!stream_emitted || since_last_emit >= 0.032)
                flush_stream_update();
        }
    };
    const double cpu_before = ReadProcessCpuSeconds(process_.hProcess);
    const auto request_started = std::chrono::steady_clock::now();
    const bool request_ok = HttpRequest(config.port, L"POST", L"/v1/chat/completions", body.str(), response, status_code, error, on_stream_chunk);
    on_stream_chunk("\n");
    flush_stream_update();
    result.request_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - request_started).count();
    result.http_status = static_cast<int>(status_code);
    result.response_bytes = static_cast<int>(response.size());
    if (!request_ok)
    {
        result.error = error;
        StopServer();
        return false;
    }

    bool has_content = false;
    if (!stream_text.empty() || !stream_reasoning.empty())
    {
        result.text = stream_text;
        result.reasoning = stream_reasoning;
        has_content = !result.text.empty();
    }
    else
    {
        has_content = ExtractJsonString(response, "\"content\"", result.text);
        ExtractJsonString(response, "\"reasoning_content\"", result.reasoning);
    }
    if (!has_content && result.reasoning.empty())
    {
        ExtractJsonString(response, "\"error\"", result.error);
        if (result.error.empty())
            result.error = "llama-server returned no assistant content";
        StopServer();
        return false;
    }
    if (!has_content)
        result.text = result.reasoning;
    ExtractJsonString(response, "\"id\"", result.response_id);
    ExtractJsonString(response, "\"object\"", result.response_object);
    ExtractJsonString(response, "\"system_fingerprint\"", result.system_fingerprint);
    ExtractJsonStringLast(response, "\"finish_reason\"", result.finish_reason);
    ExtractJsonString(response, "\"model\"", result.response_model);

    double prompt_ms = 0.0;
    double predicted_ms = 0.0;
    double prompt_tokens = 0.0;
    double predicted_tokens = 0.0;
    double prompt_per_second = 0.0;
    double decode_per_second = 0.0;
    double prompt_per_token_ms = 0.0;
    double predicted_per_token_ms = 0.0;
    double total_tokens = 0.0;
    double context_size = 0.0;
    double tokens_cached = 0.0;
    double cache_n = 0.0;
    double cache_reuse = 0.0;
    bool cache_reused_flag = false;
    double tokens_evaluated = 0.0;
    double tokens_predicted = 0.0;
    double reasoning_tokens = 0.0;
    double accepted_prediction_tokens = 0.0;
    double rejected_prediction_tokens = 0.0;
    ExtractJsonNumber(response, "\"prompt_ms\"", prompt_ms);
    ExtractJsonNumber(response, "\"predicted_ms\"", predicted_ms);
    if (!ExtractJsonNumber(response, "\"prompt_n\"", prompt_tokens))
        ExtractJsonNumber(response, "\"prompt_tokens\"", prompt_tokens);
    if (!ExtractJsonNumber(response, "\"predicted_n\"", predicted_tokens))
        ExtractJsonNumber(response, "\"completion_tokens\"", predicted_tokens);
    ExtractJsonNumber(response, "\"total_tokens\"", total_tokens);
    ExtractJsonNumber(response, "\"tokens_evaluated\"", tokens_evaluated);
    ExtractJsonNumber(response, "\"tokens_predicted\"", tokens_predicted);
    ExtractJsonNumber(response, "\"cache_n\"", cache_n);
    ExtractJsonNumber(response, "\"cached_tokens\"", tokens_cached);
    ExtractJsonNumber(response, "\"reasoning_tokens\"", reasoning_tokens);
    ExtractJsonNumber(response, "\"accepted_prediction_tokens\"", accepted_prediction_tokens);
    ExtractJsonNumber(response, "\"rejected_prediction_tokens\"", rejected_prediction_tokens);
    ExtractJsonNumber(response, "\"prompt_per_second\"", prompt_per_second);
    ExtractJsonNumber(response, "\"predicted_per_second\"", decode_per_second);
    ExtractJsonNumber(response, "\"prompt_per_token_ms\"", prompt_per_token_ms);
    ExtractJsonNumber(response, "\"predicted_per_token_ms\"", predicted_per_token_ms);

    // These endpoints are optional, but expose the server configuration and build
    // metadata that the OpenAI-compatible response intentionally omits.
    std::string props_response;
    DWORD props_status = 0;
    std::string props_error;
    if (HttpRequest(config.port, L"GET", L"/props", {}, props_response, props_status, props_error))
    {
        ExtractJsonString(props_response, "\"model_path\"", result.model_path);
        ExtractJsonString(props_response, "\"model_alias\"", result.model_alias);
        ExtractJsonString(props_response, "\"build_info\"", result.build_info);
        ExtractJsonString(props_response, "\"chat_template\"", result.chat_template);
        ExtractJsonNumber(props_response, "\"n_ctx\"", context_size);
        double value = 0.0;
        if (ExtractJsonNumber(props_response, "\"n_batch\"", value))
            result.batch_size = static_cast<int>(value);
        if (ExtractJsonNumber(props_response, "\"n_ubatch\"", value))
            result.ubatch_size = static_cast<int>(value);
        if (ExtractJsonNumber(props_response, "\"n_threads\"", value))
            result.threads = static_cast<int>(value);
        if (ExtractJsonNumber(props_response, "\"n_threads_batch\"", value))
            result.threads_batch = static_cast<int>(value);
        if (ExtractJsonNumber(props_response, "\"n_gpu_layers\"", value))
            result.gpu_layers = static_cast<int>(value);
    }

    // /slots is optional across llama.cpp builds, so enrich the run when available.
    std::string slots_response;
    DWORD slots_status = 0;
    std::string slots_error;
    if (HttpRequest(config.port, L"GET", L"/slots", {}, slots_response, slots_status, slots_error))
    {
        result.slot_info_available = true;
        double value = 0.0;
        if (ExtractJsonNumber(slots_response, "\"id\"", value))
            result.slot_id = static_cast<int>(value);
        ExtractJsonString(slots_response, "\"state\"", result.slot_state);
        ExtractJsonBool(slots_response, "\"truncated\"", result.slot_truncated);
        ExtractJsonBool(slots_response, "\"stopped_eos\"", result.stopped_eos);
        ExtractJsonBool(slots_response, "\"stopped_word\"", result.stopped_word);
        ExtractJsonBool(slots_response, "\"stopped_limit\"", result.stopped_limit);
        if (prompt_ms <= 0.0)
            ExtractJsonNumber(slots_response, "\"prompt_ms\"", prompt_ms);
        if (prompt_ms <= 0.0)
            ExtractJsonNumber(slots_response, "\"t_prompt_processing\"", prompt_ms);
        if (predicted_ms <= 0.0)
            ExtractJsonNumber(slots_response, "\"predicted_ms\"", predicted_ms);
        if (predicted_ms <= 0.0)
            ExtractJsonNumber(slots_response, "\"t_token_generation\"", predicted_ms);
        ExtractJsonNumber(slots_response, "\"t_load_ms\"", result.load_ms);
        if (result.load_ms <= 0.0)
            ExtractJsonNumber(slots_response, "\"t_load\"", result.load_ms);
        ExtractJsonNumber(slots_response, "\"t_start_generation_ms\"", result.start_ms);
        if (result.start_ms <= 0.0)
            ExtractJsonNumber(slots_response, "\"t_start_ms\"", result.start_ms);
        ExtractJsonNumber(slots_response, "\"t_tokenize_ms\"", result.tokenize_ms);
        ExtractJsonNumber(slots_response, "\"t_sample_ms\"", result.sample_ms);
        ExtractJsonNumber(slots_response, "\"t_total_ms\"", result.total_ms);
        if (prompt_tokens <= 0.0)
        {
            if (!ExtractJsonNumber(slots_response, "\"prompt_n\"", prompt_tokens))
            {
                if (!ExtractJsonNumber(slots_response, "\"n_prompt_tokens\"", prompt_tokens))
                    ExtractJsonNumber(slots_response, "\"tokens_evaluated\"", prompt_tokens);
            }
        }
        if (predicted_tokens <= 0.0)
        {
            if (!ExtractJsonNumber(slots_response, "\"predicted_n\"", predicted_tokens))
            {
                if (!ExtractJsonNumber(slots_response, "\"n_decoded\"", predicted_tokens))
                    ExtractJsonNumber(slots_response, "\"tokens_predicted\"", predicted_tokens);
            }
        }
        ExtractJsonNumber(slots_response, "\"n_ctx\"", context_size);
        if (ExtractJsonNumber(slots_response, "\"n_past\"", value))
            result.n_past = static_cast<int>(value);
        if (ExtractJsonNumber(slots_response, "\"n_prompt_tokens\"", value))
            result.n_prompt_tokens = static_cast<int>(value);
        if (ExtractJsonNumber(slots_response, "\"n_decoded\"", value))
            result.n_decoded = static_cast<int>(value);
        if (ExtractJsonNumber(slots_response, "\"n_cache_tokens\"", value))
            result.n_cache_tokens = static_cast<int>(value);
        if (ExtractJsonNumber(slots_response, "\"n_predict\"", value))
            result.n_predict_limit = static_cast<int>(value);
        if (!ExtractJsonNumber(slots_response, "\"n_cache\"", tokens_cached))
        {
            if (!ExtractJsonNumber(slots_response, "\"tokens_cached\"", tokens_cached))
            {
                if (!ExtractJsonNumber(slots_response, "\"cache_tokens\"", tokens_cached))
                    ExtractJsonNumber(slots_response, "\"n_cache_tokens\"", tokens_cached);
            }
        }
        ExtractJsonNumber(slots_response, "\"cache_reuse\"", cache_reuse);
        ExtractJsonBool(slots_response, "\"cache_reuse\"", cache_reused_flag);
        if (tokens_evaluated <= 0.0)
            ExtractJsonNumber(slots_response, "\"tokens_evaluated\"", tokens_evaluated);
        if (tokens_predicted <= 0.0)
            ExtractJsonNumber(slots_response, "\"tokens_predicted\"", tokens_predicted);
    }

    if (tokens_cached <= 0.0 && cache_n > 0.0)
        tokens_cached = cache_n;

    // Prometheus metrics are cumulative for the current server process. They are
    // useful as a second source of truth when response timing fields are missing.
    std::string metrics_response;
    DWORD metrics_status = 0;
    std::string metrics_error;
    if (HttpRequest(config.port, L"GET", L"/metrics", {}, metrics_response, metrics_status, metrics_error))
    {
        double metric = 0.0;
        result.metrics_available = true;
        if (ExtractPrometheusNumber(metrics_response, "llamacpp:requests_total", metric))
            result.metrics_requests_total = static_cast<int>(metric);
        if (ExtractPrometheusNumber(metrics_response, "llamacpp:requests_processing", metric))
            result.metrics_requests_processing = static_cast<int>(metric);
        if (ExtractPrometheusNumber(metrics_response, "llamacpp:requests_deferred", metric))
            result.metrics_requests_deferred = static_cast<int>(metric);
        if (ExtractPrometheusNumber(metrics_response, "llamacpp:prompt_tokens_total", metric))
            result.metrics_prompt_tokens_total = static_cast<int>(metric);
        if (ExtractPrometheusNumber(metrics_response, "llamacpp:tokens_predicted_total", metric))
            result.metrics_predicted_tokens_total = static_cast<int>(metric);
        ExtractPrometheusNumber(metrics_response, "llamacpp:prompt_tokens_seconds_total", result.metrics_prompt_seconds_total);
        ExtractPrometheusNumber(metrics_response, "llamacpp:tokens_predicted_seconds_total", result.metrics_predicted_seconds_total);
        std::ostringstream metrics_summary;
        metrics_summary << "requests " << result.metrics_requests_total
                        << ", prompt tokens " << result.metrics_prompt_tokens_total
                        << ", predicted tokens " << result.metrics_predicted_tokens_total
                        << ", processing " << result.metrics_requests_processing
                        << ", deferred " << result.metrics_requests_deferred;
        result.metrics_summary = metrics_summary.str();
    }

    if (prompt_per_second <= 0.0 && prompt_ms > 0.0 && prompt_tokens > 0.0)
        prompt_per_second = prompt_tokens / (prompt_ms / 1000.0);
    if (decode_per_second <= 0.0 && predicted_ms > 0.0 && predicted_tokens > 0.0)
        decode_per_second = predicted_tokens / (predicted_ms / 1000.0);
    if (decode_per_second <= 0.0 && predicted_tokens > 0.0)
    {
        const double elapsed_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - request_started).count();
        if (elapsed_seconds > 0.0)
            decode_per_second = predicted_tokens / elapsed_seconds;
    }
    if (total_tokens <= 0.0 && prompt_tokens > 0.0 && predicted_tokens > 0.0)
        total_tokens = prompt_tokens + predicted_tokens;
    if (tokens_evaluated <= 0.0)
        tokens_evaluated = prompt_tokens;
    if (tokens_predicted <= 0.0)
        tokens_predicted = predicted_tokens;

    if (prompt_per_token_ms <= 0.0 && prompt_ms > 0.0 && prompt_tokens > 0.0)
        prompt_per_token_ms = prompt_ms / prompt_tokens;
    if (predicted_per_token_ms <= 0.0 && predicted_ms > 0.0 && predicted_tokens > 0.0)
        predicted_per_token_ms = predicted_ms / predicted_tokens;
    if (result.total_ms <= 0.0 && result.request_seconds > 0.0)
        result.total_ms = result.request_seconds * 1000.0;
    if (result.batch_size <= 0)
        result.batch_size = config.batch_size;
    if (result.ubatch_size <= 0)
        result.ubatch_size = config.ubatch_size;
    if (result.threads <= 0)
        result.threads = config.threads;
    if (result.threads_batch <= 0)
        result.threads_batch = config.threads_batch;
    if (result.gpu_layers <= 0 && config.gpu_layers > 0)
        result.gpu_layers = config.gpu_layers;
    if (result.context_size <= 0)
        result.context_size = config.context;

    const ProcessSnapshot memory = ReadProcessSnapshot(process_.hProcess);
    const double cpu_after = ReadProcessCpuSeconds(process_.hProcess);
    result.prompt_tokens = static_cast<int>(prompt_tokens);
    result.predicted_tokens = static_cast<int>(predicted_tokens);
    result.total_tokens = static_cast<int>(total_tokens);
    result.context_size = static_cast<int>(context_size);
    result.tokens_cached = static_cast<int>(tokens_cached);
    result.prompt_cache_reused = cache_reuse > 0.0 || cache_reused_flag;
    result.tokens_evaluated = static_cast<int>(tokens_evaluated);
    result.tokens_predicted = static_cast<int>(tokens_predicted);
    result.reasoning_tokens = static_cast<int>(reasoning_tokens);
    result.accepted_prediction_tokens = static_cast<int>(accepted_prediction_tokens);
    result.rejected_prediction_tokens = static_cast<int>(rejected_prediction_tokens);
    result.prompt_tokens_per_second = prompt_per_second;
    result.decode_tokens_per_second = decode_per_second;
    result.prompt_ms = prompt_ms;
    result.predicted_ms = predicted_ms;
    result.prompt_ms_per_token = prompt_per_token_ms;
    result.predicted_ms_per_token = predicted_per_token_ms;
    result.prompt_eval_seconds = prompt_ms > 0.0 ? prompt_ms / 1000.0 : 0.0;
    result.cpu_seconds = std::max(0.0, cpu_after - cpu_before);
    result.memory_gb = memory.working_gb;
    result.peak_memory_gb = memory.peak_working_gb;
    result.private_memory_gb = memory.private_gb;
    if (prompt_ms > 0.0 || predicted_ms > 0.0 || prompt_per_second > 0.0 || decode_per_second > 0.0)
        result.timings_source = "llama.cpp timings";
    else if (predicted_tokens > 0.0)
        result.timings_source = "wall-clock fallback";
    else
        result.timings_source = "response metadata";
    if (result.finish_reason.empty())
        result.finish_reason = "unknown";
    if (result.response_model.empty())
        result.response_model = "local";
    result.ok = true;
    StopServer();
    return true;
}

bool LlamaServerBackend::StartServer(const LlamaModelJob& model, const LlamaRunConfig& config, std::string& error)
{
    const DWORD attributes = ::GetFileAttributesW(config.server_path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
    {
        error = "llama-server.exe was not found at the selected path";
        return false;
    }

    std::wstring command = QuoteArg(config.server_path);
    command += L" -m " + QuoteArg(model.path);
    command += L" -c " + std::to_wstring(config.context);
    command += L" --host 127.0.0.1 --port " + std::to_wstring(config.port);
    command += L" -t " + std::to_wstring(config.threads);
    command += L" -tb " + std::to_wstring(config.threads_batch);
    command += L" -ngl " + std::to_wstring(config.gpu_layers);
    command += L" -b " + std::to_wstring(config.batch_size);
    command += L" -ub " + std::to_wstring(config.ubatch_size);
    command += L" --seed " + std::to_wstring(config.seed);
    command += L" --temp " + std::to_wstring(config.temperature);
    if (config.flash_attention)
        command += L" --flash-attn on";
    if (!config.mmap)
        command += L" --no-mmap";
    if (config.mlock)
        command += L" --mlock";
    if (!config.kv_offload)
        command += L" --no-kv-offload";

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    if (!::CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
    {
        error = "Could not start llama-server.exe";
        return false;
    }
    process_ = process;
    return true;
}

void LlamaServerBackend::StopServer()
{
    if (process_.hProcess)
    {
        ::TerminateProcess(process_.hProcess, 0);
        ::WaitForSingleObject(process_.hProcess, 3000);
        ::CloseHandle(process_.hThread);
        ::CloseHandle(process_.hProcess);
        process_ = {};
    }
}

bool LlamaServerBackend::WaitForHealth(const LlamaRunConfig& config, std::string& error)
{
    for (int attempt = 0; attempt < 240; ++attempt)
    {
        if (IsStopRequested())
        {
            error = "Run cancelled";
            return false;
        }
        if (process_.hProcess && ::WaitForSingleObject(process_.hProcess, 0) == WAIT_OBJECT_0)
        {
            error = "llama-server exited before becoming healthy";
            return false;
        }
        std::string response;
        DWORD status_code = 0;
        std::string request_error;
        if (HttpRequest(config.port, L"GET", L"/health", {}, response, status_code, request_error))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    error = "llama-server did not become healthy before the timeout";
    return false;
}

bool LlamaServerBackend::IsStopRequested() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return stop_requested_;
}

void LlamaServerBackend::SetStatus(const std::string& status)
{
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = status;
}

void LlamaServerBackend::PushResult(LlamaRunResult result)
{
    std::lock_guard<std::mutex> lock(mutex_);
    results_.push_back(std::move(result));
}
