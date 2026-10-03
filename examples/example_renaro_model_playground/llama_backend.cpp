#include "llama_backend.h"

#include <windows.h>
#include <psapi.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
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

static std::string Truncate(const std::string& value, size_t limit)
{
    if (value.size() <= limit)
        return value;
    return value.substr(0, limit) + "...";
}

static bool HttpRequest(int port, const wchar_t* method, const wchar_t* path, const std::string& body, std::string& response, DWORD& status_code, std::string& error)
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

    const wchar_t* headers = body.empty() ? nullptr : L"Content-Type: application/json\r\n";
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

static double ProcessMemoryGb(HANDLE process)
{
    if (!process)
        return 0.0;
    PROCESS_MEMORY_COUNTERS_EX counters = {};
    if (!::GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
        return 0.0;
    return static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0 * 1024.0);
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

    SetStatus("running " + model.name);
    std::ostringstream body;
    body << "{\"model\":\"local\",\"messages\":[{\"role\":\"user\",\"content\":\"" << JsonEscape(prompt)
         << "\"}],\"temperature\":" << std::fixed << std::setprecision(3) << config.temperature
         << ",\"max_tokens\":" << config.max_tokens << ",\"seed\":" << config.seed << ",\"stream\":false}";
    std::string response;
    DWORD status_code = 0;
    const auto request_started = std::chrono::steady_clock::now();
    if (!HttpRequest(config.port, L"POST", L"/v1/chat/completions", body.str(), response, status_code, error))
    {
        result.error = error;
        StopServer();
        return false;
    }

    if (!ExtractJsonString(response, "\"content\"", result.text))
    {
        ExtractJsonString(response, "\"error\"", result.error);
        if (result.error.empty())
            result.error = "llama-server returned no assistant content";
        StopServer();
        return false;
    }

    double prompt_ms = 0.0;
    double predicted_ms = 0.0;
    double prompt_tokens = 0.0;
    double predicted_tokens = 0.0;
    double prompt_per_second = 0.0;
    double decode_per_second = 0.0;
    ExtractJsonNumber(response, "\"prompt_ms\"", prompt_ms);
    ExtractJsonNumber(response, "\"predicted_ms\"", predicted_ms);
    if (!ExtractJsonNumber(response, "\"prompt_n\"", prompt_tokens))
        ExtractJsonNumber(response, "\"prompt_tokens\"", prompt_tokens);
    if (!ExtractJsonNumber(response, "\"predicted_n\"", predicted_tokens))
        ExtractJsonNumber(response, "\"completion_tokens\"", predicted_tokens);
    ExtractJsonNumber(response, "\"prompt_per_second\"", prompt_per_second);
    ExtractJsonNumber(response, "\"predicted_per_second\"", decode_per_second);
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
    result.prompt_tokens_per_second = prompt_per_second;
    result.decode_tokens_per_second = decode_per_second;
    result.prompt_eval_seconds = prompt_ms > 0.0 ? prompt_ms / 1000.0 : 0.0;
    result.memory_gb = ProcessMemoryGb(process_.hProcess);
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
    command += L" -ngl " + std::to_wstring(config.gpu_layers);
    command += L" --seed " + std::to_wstring(config.seed);
    command += L" --temp " + std::to_wstring(config.temperature);

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
