// Headless (command-line) mode for the Renaro Model Playground.
//
// Runs a JSONL file of prompts (or a single prompt) through llama-server one call at a time,
// in file order, and writes three files:
//   results.jsonl  one JSON line per call, sorted keys, no timestamps or timings, so two runs
//                  with the same settings can be compared byte for byte
//   perf.json      server token counts, timings and peak memory per call, plus totals
//   manifest.json  every setting and the exact llama-server command line

#include "headless.h"
#include "llama_backend.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
const char* const kManifestFormat = "renaro-headless/1";

// -------------------------------------------------------------------------------------------------
// Text helpers
// -------------------------------------------------------------------------------------------------

std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty())
        return {};
    const int length = ::WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return {};
    std::string result(static_cast<size_t>(length), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), &result[0], length, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string& value)
{
    if (value.empty())
        return {};
    const int length = ::MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0)
        return {};
    std::wstring result(static_cast<size_t>(length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), &result[0], length);
    return result;
}

std::string PathToUtf8(const std::filesystem::path& path)
{
    return WideToUtf8(path.wstring());
}

std::string Trim(const std::string& value)
{
    const size_t start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
        return {};
    const size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

std::string Lower(const std::string& value)
{
    std::string result = value;
    for (char& character : result)
    {
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character - 'A' + 'a');
    }
    return result;
}

bool IsDigit(char character)
{
    return character >= '0' && character <= '9';
}

void AppendUtf8(std::string& output, unsigned int codepoint)
{
    if (codepoint <= 0x7f)
    {
        output.push_back(static_cast<char>(codepoint));
    }
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

// Replaces invalid UTF-8 with U+FFFD so every string written to disk is valid UTF-8.
std::string SanitizeUtf8(const std::string& value)
{
    std::string result;
    result.reserve(value.size());
    size_t index = 0;
    while (index < value.size())
    {
        const unsigned char lead = static_cast<unsigned char>(value[index]);
        if (lead < 0x80)
        {
            result.push_back(static_cast<char>(lead));
            ++index;
            continue;
        }
        size_t length = 0;
        unsigned int codepoint = 0;
        unsigned int minimum = 0;
        if (lead >= 0xc2 && lead <= 0xdf) { length = 2; codepoint = lead & 0x1fu; minimum = 0x80; }
        else if (lead >= 0xe0 && lead <= 0xef) { length = 3; codepoint = lead & 0x0fu; minimum = 0x800; }
        else if (lead >= 0xf0 && lead <= 0xf4) { length = 4; codepoint = lead & 0x07u; minimum = 0x10000; }
        bool valid = length > 0 && index + length <= value.size();
        for (size_t offset = 1; valid && offset < length; ++offset)
        {
            const unsigned char next = static_cast<unsigned char>(value[index + offset]);
            if ((next & 0xc0) != 0x80)
                valid = false;
            else
                codepoint = (codepoint << 6) | (next & 0x3fu);
        }
        if (valid && (codepoint < minimum || codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)))
            valid = false;
        if (valid)
        {
            result.append(value, index, length);
            index += length;
        }
        else
        {
            result += "\xEF\xBF\xBD";
            ++index;
        }
    }
    return result;
}

std::string Fnv1a64Hex(const std::string& data)
{
    std::uint64_t hash = 1469598103934665603ull;
    for (const char character : data)
    {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ull;
    }
    char text[17] = {};
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
    return text;
}

std::vector<std::string> SplitLines(const std::string& text)
{
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size())
    {
        const size_t end = text.find('\n', start);
        std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(std::move(line));
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return lines;
}

void StripUtf8Bom(std::string& text)
{
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xef && static_cast<unsigned char>(text[1]) == 0xbb && static_cast<unsigned char>(text[2]) == 0xbf)
        text.erase(0, 3);
}

// -------------------------------------------------------------------------------------------------
// JSON output. The format matches Python's json.dumps(value, sort_keys=True, ensure_ascii=False,
// separators=(",", ":")), so a line can be checked or regenerated with standard tools.
// -------------------------------------------------------------------------------------------------

void AppendJsonString(std::string& output, const std::string& raw)
{
    const std::string value = SanitizeUtf8(raw);
    output += '"';
    for (const char character : value)
    {
        const unsigned char code = static_cast<unsigned char>(character);
        switch (code)
        {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (code < 0x20)
            {
                char escaped[8] = {};
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned int>(code));
                output += escaped;
            }
            else
            {
                output += character;
            }
            break;
        }
    }
    output += '"';
}

std::string JsonString(const std::string& value)
{
    std::string output;
    AppendJsonString(output, value);
    return output;
}

std::string JsonBool(bool value)
{
    return value ? "true" : "false";
}

std::string JsonInt(long long value)
{
    return std::to_string(value);
}

// Fixed decimals, for measured values.
std::string JsonFixed(double value, int decimals)
{
    if (!std::isfinite(value))
        return "null";
    char text[64] = {};
    std::snprintf(text, sizeof(text), "%.*f", decimals, value);
    return text;
}

// Shortest readable form, for settings such as 0.7 or 0.05.
std::string JsonReal(double value)
{
    if (!std::isfinite(value))
        return "null";
    char text[64] = {};
    std::snprintf(text, sizeof(text), "%.6g", value);
    return text;
}

std::string JsonStringArray(const std::vector<std::string>& values)
{
    std::string output = "[";
    for (size_t index = 0; index < values.size(); ++index)
    {
        if (index > 0)
            output += ',';
        AppendJsonString(output, values[index]);
    }
    output += ']';
    return output;
}

// A JSON object whose keys are always written in sorted order.
class JsonObject
{
public:
    JsonObject& Set(const std::string& key, const std::string& json)
    {
        fields_[key] = json;
        return *this;
    }

    std::string Dump() const
    {
        std::string output = "{";
        bool first = true;
        for (const auto& field : fields_)
        {
            if (!first)
                output += ',';
            first = false;
            AppendJsonString(output, field.first);
            output += ':';
            output += field.second;
        }
        output += '}';
        return output;
    }

    // Same content, one top-level field per line for files people read.
    std::string DumpLines() const
    {
        std::string output = "{";
        bool first = true;
        for (const auto& field : fields_)
        {
            output += first ? "\n  " : ",\n  ";
            first = false;
            AppendJsonString(output, field.first);
            output += ':';
            output += field.second;
        }
        output += first ? "}\n" : "\n}\n";
        return output;
    }

private:
    std::map<std::string, std::string> fields_;
};

// -------------------------------------------------------------------------------------------------
// JSON input (one value per JSONL line)
// -------------------------------------------------------------------------------------------------

struct JsonValue
{
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    std::string text;              // string value, or a number exactly as written
    std::vector<JsonValue> items;  // array items, or object values (parallel to keys)
    std::vector<std::string> keys; // object keys

    // Like Python's json module, a repeated key resolves to its last value.
    const JsonValue* Find(const std::string& key) const
    {
        if (type != Type::Object)
            return nullptr;
        const JsonValue* found = nullptr;
        for (size_t index = 0; index < keys.size() && index < items.size(); ++index)
        {
            if (keys[index] == key)
                found = &items[index];
        }
        return found;
    }
};

class JsonParser
{
public:
    explicit JsonParser(const std::string& text) : text_(text) {}

    bool Parse(JsonValue& value, std::string& error)
    {
        SkipSpace();
        bool ok = ParseValue(value, 0);
        if (ok)
        {
            SkipSpace();
            if (pos_ != text_.size())
                ok = Fail("unexpected text after the JSON value");
        }
        if (!ok)
            error = error_ + " (column " + std::to_string(pos_ + 1) + ")";
        return ok;
    }

private:
    bool Fail(const char* message)
    {
        if (error_.empty())
            error_ = message;
        return false;
    }

    void SkipSpace()
    {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\r' || text_[pos_] == '\n'))
            ++pos_;
    }

    bool Literal(const char* word)
    {
        const size_t length = std::strlen(word);
        if (text_.compare(pos_, length, word) != 0)
            return false;
        pos_ += length;
        return true;
    }

    bool ParseValue(JsonValue& value, int depth)
    {
        if (depth > 64)
            return Fail("JSON is nested too deeply");
        if (pos_ >= text_.size())
            return Fail("unexpected end of JSON");
        const char character = text_[pos_];
        if (character == '{')
            return ParseObject(value, depth);
        if (character == '[')
            return ParseArray(value, depth);
        if (character == '"')
        {
            value.type = JsonValue::Type::String;
            return ParseString(value.text);
        }
        if (character == 't' || character == 'f')
        {
            value.type = JsonValue::Type::Bool;
            value.boolean = character == 't';
            return Literal(value.boolean ? "true" : "false") || Fail("invalid literal");
        }
        if (character == 'n')
        {
            value.type = JsonValue::Type::Null;
            return Literal("null") || Fail("invalid literal");
        }
        if (character == '-' || IsDigit(character))
            return ParseNumber(value);
        return Fail("unexpected character");
    }

    bool ParseObject(JsonValue& value, int depth)
    {
        value.type = JsonValue::Type::Object;
        ++pos_;
        SkipSpace();
        if (pos_ < text_.size() && text_[pos_] == '}')
        {
            ++pos_;
            return true;
        }
        while (true)
        {
            SkipSpace();
            if (pos_ >= text_.size() || text_[pos_] != '"')
                return Fail("expected a quoted object key");
            std::string key;
            if (!ParseString(key))
                return false;
            SkipSpace();
            if (pos_ >= text_.size() || text_[pos_] != ':')
                return Fail("expected ':' after an object key");
            ++pos_;
            SkipSpace();
            JsonValue member;
            if (!ParseValue(member, depth + 1))
                return false;
            value.keys.push_back(std::move(key));
            value.items.push_back(std::move(member));
            SkipSpace();
            if (pos_ < text_.size() && text_[pos_] == ',')
            {
                ++pos_;
                continue;
            }
            if (pos_ < text_.size() && text_[pos_] == '}')
            {
                ++pos_;
                return true;
            }
            return Fail("expected ',' or '}' in an object");
        }
    }

    bool ParseArray(JsonValue& value, int depth)
    {
        value.type = JsonValue::Type::Array;
        ++pos_;
        SkipSpace();
        if (pos_ < text_.size() && text_[pos_] == ']')
        {
            ++pos_;
            return true;
        }
        while (true)
        {
            SkipSpace();
            JsonValue item;
            if (!ParseValue(item, depth + 1))
                return false;
            value.items.push_back(std::move(item));
            SkipSpace();
            if (pos_ < text_.size() && text_[pos_] == ',')
            {
                ++pos_;
                continue;
            }
            if (pos_ < text_.size() && text_[pos_] == ']')
            {
                ++pos_;
                return true;
            }
            return Fail("expected ',' or ']' in an array");
        }
    }

    bool ReadHex4(unsigned int& result)
    {
        if (pos_ + 4 > text_.size())
            return false;
        unsigned int value = 0;
        for (size_t offset = 0; offset < 4; ++offset)
        {
            const char character = text_[pos_ + offset];
            value <<= 4;
            if (character >= '0' && character <= '9') value |= static_cast<unsigned int>(character - '0');
            else if (character >= 'a' && character <= 'f') value |= static_cast<unsigned int>(character - 'a' + 10);
            else if (character >= 'A' && character <= 'F') value |= static_cast<unsigned int>(character - 'A' + 10);
            else return false;
        }
        pos_ += 4;
        result = value;
        return true;
    }

    bool ParseString(std::string& output)
    {
        ++pos_;
        output.clear();
        while (pos_ < text_.size())
        {
            const unsigned char character = static_cast<unsigned char>(text_[pos_++]);
            if (character == '"')
                return true;
            if (character < 0x20)
                return Fail("control character inside a string");
            if (character != '\\')
            {
                output.push_back(static_cast<char>(character));
                continue;
            }
            if (pos_ >= text_.size())
                break;
            const char escaped = text_[pos_++];
            switch (escaped)
            {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u':
            {
                unsigned int codepoint = 0;
                if (!ReadHex4(codepoint))
                    return Fail("invalid \\u escape");
                if (codepoint >= 0xd800 && codepoint <= 0xdbff)
                {
                    const size_t saved = pos_;
                    unsigned int low = 0;
                    if (pos_ + 1 < text_.size() && text_[pos_] == '\\' && text_[pos_ + 1] == 'u')
                    {
                        pos_ += 2;
                        if (ReadHex4(low) && low >= 0xdc00 && low <= 0xdfff)
                        {
                            codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
                        }
                        else
                        {
                            pos_ = saved;
                            codepoint = 0xfffd;
                        }
                    }
                    else
                    {
                        codepoint = 0xfffd;
                    }
                }
                else if (codepoint >= 0xdc00 && codepoint <= 0xdfff)
                {
                    codepoint = 0xfffd;
                }
                AppendUtf8(output, codepoint);
                break;
            }
            default:
                return Fail("invalid escape sequence");
            }
        }
        return Fail("unterminated string");
    }

    bool ParseNumber(JsonValue& value)
    {
        const size_t start = pos_;
        if (text_[pos_] == '-')
            ++pos_;
        if (pos_ >= text_.size() || !IsDigit(text_[pos_]))
            return Fail("invalid number");
        if (text_[pos_] == '0')
        {
            ++pos_;
        }
        else
        {
            while (pos_ < text_.size() && IsDigit(text_[pos_]))
                ++pos_;
        }
        if (pos_ < text_.size() && text_[pos_] == '.')
        {
            ++pos_;
            if (pos_ >= text_.size() || !IsDigit(text_[pos_]))
                return Fail("invalid number");
            while (pos_ < text_.size() && IsDigit(text_[pos_]))
                ++pos_;
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E'))
        {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-'))
                ++pos_;
            if (pos_ >= text_.size() || !IsDigit(text_[pos_]))
                return Fail("invalid number");
            while (pos_ < text_.size() && IsDigit(text_[pos_]))
                ++pos_;
        }
        value.type = JsonValue::Type::Number;
        value.text = text_.substr(start, pos_ - start);
        return true;
    }

    const std::string& text_;
    size_t pos_ = 0;
    std::string error_;
};

// Canonical form used to echo input ids: sorted object keys, numbers exactly as written.
std::string SerializeJson(const JsonValue& value)
{
    switch (value.type)
    {
    case JsonValue::Type::Null:
        return "null";
    case JsonValue::Type::Bool:
        return JsonBool(value.boolean);
    case JsonValue::Type::Number:
        return value.text;
    case JsonValue::Type::String:
        return JsonString(value.text);
    case JsonValue::Type::Array:
    {
        std::string output = "[";
        for (size_t index = 0; index < value.items.size(); ++index)
        {
            if (index > 0)
                output += ',';
            output += SerializeJson(value.items[index]);
        }
        output += ']';
        return output;
    }
    case JsonValue::Type::Object:
    {
        JsonObject object;
        for (size_t index = 0; index < value.keys.size() && index < value.items.size(); ++index)
            object.Set(value.keys[index], SerializeJson(value.items[index]));
        return object.Dump();
    }
    }
    return "null";
}

// -------------------------------------------------------------------------------------------------
// Console output
// -------------------------------------------------------------------------------------------------

bool g_quiet = false;
std::atomic<bool> g_interrupted(false);
std::atomic<LlamaServerBackend*> g_active_backend(nullptr);

void PrintLine(FILE* stream, const std::string& text)
{
    std::fputs(text.c_str(), stream);
    std::fputc('\n', stream);
    std::fflush(stream);
}

void Info(const std::string& text)
{
    if (!g_quiet)
        PrintLine(stdout, text);
}

void Warn(const std::string& text)
{
    PrintLine(stderr, "warning: " + text);
}

void ErrorLine(const std::string& text)
{
    PrintLine(stderr, "error: " + text);
}

BOOL WINAPI ConsoleControlHandler(DWORD type)
{
    switch (type)
    {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        g_interrupted.store(true);
        if (LlamaServerBackend* backend = g_active_backend.load())
            backend->RequestStop();
        return TRUE;
    default:
        return FALSE;
    }
}

void PrintUsage()
{
    static const char* const kUsage =
        "Renaro Model Playground - headless mode\n"
        "\n"
        "Runs prompts through llama-server without opening a window. Any option that starts\n"
        "with - selects this mode; --headless can be given to make it explicit.\n"
        "\n"
        "Usage:\n"
        "  renaro_model_playground.exe --headless --model <model.gguf> --input <prompts.jsonl> [options]\n"
        "  renaro_model_playground.exe --headless --model <model.gguf> --prompt \"<text>\" [options]\n"
        "\n"
        "Input\n"
        "  --input <file>            JSONL prompts, run one at a time in file order. Each line is an\n"
        "                            object such as {\"id\": \"p1\", \"prompt\": \"...\", \"system\": \"...\"},\n"
        "                            a chat {\"id\": ..., \"messages\": [{\"role\": ..., \"content\": ...}]}\n"
        "                            or a bare JSON string. Blank lines are skipped.\n"
        "  --prompt <text>           Run one prompt instead of --input.\n"
        "  --prompt-field <name>     Field that holds the prompt (default: prompt).\n"
        "  --id-field <name>         Field echoed as \"id\" in the results (default: id).\n"
        "  --system <text>           System message for prompts that do not bring their own.\n"
        "  --config <file>           Read settings from a YAML file such as slm.yaml (key: value\n"
        "                            pairs; nesting is ignored, relative paths are relative to the\n"
        "                            file). Command-line options override it.\n"
        "\n"
        "Model and server (fixed for the whole run; the server always has one slot, -np 1)\n"
        "  --model <file.gguf>       Model file. Repeat to run several models one after another.\n"
        "  --server <exe>            llama-server.exe (default: found the same way as the GUI).\n"
        "  --port <n>                Port for llama-server (default: 8080).\n"
        "  --ctx <n>                 Context size, -c (default: 8192).\n"
        "  --threads <n>             Generation threads, -t (default: 8).\n"
        "  --threads-batch <n>       Prompt threads, -tb (default: same as --threads).\n"
        "  --gpu-layers <n>          Layers offloaded to the GPU, -ngl (default: 0).\n"
        "  --batch <n>               Logical batch, -b (default: 512).\n"
        "  --ubatch <n>              Physical batch, -ub (default: 512).\n"
        "  --flash-attn  --no-mmap  --mlock  --no-kv-offload\n"
        "  --server-arg <arg>        Extra llama-server argument. Repeat for more.\n"
        "  --load-timeout <s>        Seconds to wait for the model to load (default: 600).\n"
        "  --attach                  Use a llama-server already running on --port instead of\n"
        "                            starting one (no command line or memory in the output).\n"
        "\n"
        "Sampling (sent with every request)\n"
        "  --temperature <f>         default 0.7\n"
        "  --top-p <f>               default 0.95\n"
        "  --top-k <n>               default 40\n"
        "  --min-p <f>               default 0.05\n"
        "  --max-tokens <n>          default 512 (-1: until the context is full)\n"
        "  --seed <n>                default 42\n"
        "  --repeat-penalty <f>      default 1.0\n"
        "  --presence-penalty <f>    default 0.0\n"
        "  --frequency-penalty <f>   default 0.0\n"
        "  --thinking on|off|default Sets chat_template_kwargs.enable_thinking. on and off also\n"
        "                            start llama-server with --jinja (default: template default).\n"
        "  --cache-prompt            Reuse the KV cache between prompts. Off by default because\n"
        "                            reusing it can change results.\n"
        "\n"
        "Output\n"
        "  --out <dir>               Output directory (default: current directory).\n"
        "  --results <file>          One JSON line per call: answer, reasoning, finish_reason,\n"
        "                            truncated, id, index, model, ok, error. Sorted keys, no\n"
        "                            timestamps (default: <out>/results.jsonl).\n"
        "  --perf <file>             Token counts, timings and peak memory per call and in total\n"
        "                            (default: <out>/perf.json).\n"
        "  --manifest <file>         Every setting and the exact llama-server command line\n"
        "                            (default: <out>/manifest.json).\n"
        "  --dry-run                 Check the options and input and write the manifest only.\n"
        "  --quiet                   Print only warnings and errors.\n"
        "  --version                 Print the headless format version.\n"
        "\n"
        "Exit codes: 0 all calls succeeded, 1 some calls failed, 2 bad options or input,\n"
        "3 the run could not start or write its output, 130 interrupted.\n";
    std::fputs(kUsage, stdout);
    std::fflush(stdout);
}

// -------------------------------------------------------------------------------------------------
// Options
// -------------------------------------------------------------------------------------------------

struct Options
{
    std::vector<std::wstring> models;
    bool models_from_cli = false;
    std::wstring input_path;
    bool input_from_cli = false;
    std::string prompt;
    bool has_prompt = false;
    std::string prompt_field = "prompt";
    std::string id_field = "id";
    std::string system_prompt;
    std::wstring out_dir;
    std::wstring results_path;
    std::wstring perf_path;
    std::wstring manifest_path;
    std::wstring server_path;
    std::vector<std::wstring> server_args;
    bool server_args_from_cli = false;

    double temperature = 0.7;
    double top_p = 0.95;
    int top_k = 40;
    double min_p = 0.05;
    int max_tokens = 512;
    int seed = 42;
    double repeat_penalty = 1.0;
    double presence_penalty = 0.0;
    double frequency_penalty = 0.0;
    int thinking = -1;

    int port = 8080;
    int ctx = 8192;
    int threads = 8;
    int threads_batch = 0;
    int gpu_layers = 0;
    int batch = 512;
    int ubatch = 512;
    bool flash_attn = false;
    bool mmap = true;
    bool mlock = false;
    bool kv_offload = true;
    bool cache_prompt = false;
    int load_timeout = 600;
    bool attach = false;
    bool dry_run = false;
    bool quiet = false;
};

struct SettingAlias
{
    const char* alias;
    const char* key;
};

// Alternative spellings accepted on the command line and in YAML files (llama.cpp, OpenAI and
// Hugging Face names for the same setting).
const SettingAlias kSettingAliases[] = {
    { "temp", "temperature" },
    { "n_predict", "max_tokens" },
    { "max_new_tokens", "max_tokens" },
    { "max_output_tokens", "max_tokens" },
    { "num_predict", "max_tokens" },
    { "repetition_penalty", "repeat_penalty" },
    { "enable_thinking", "thinking" },
    { "context", "ctx" },
    { "ctx_size", "ctx" },
    { "n_ctx", "ctx" },
    { "context_length", "ctx" },
    { "context_size", "ctx" },
    { "num_ctx", "ctx" },
    { "n_threads", "threads" },
    { "n_threads_batch", "threads_batch" },
    { "n_gpu_layers", "gpu_layers" },
    { "ngl", "gpu_layers" },
    { "batch_size", "batch" },
    { "n_batch", "batch" },
    { "ubatch_size", "ubatch" },
    { "n_ubatch", "ubatch" },
    { "server_path", "server" },
    { "llama_server", "server" },
    { "model_path", "model" },
    { "model_file", "model" },
    { "gguf", "model" },
    { "system_prompt", "system" },
    { "prompts", "input" },
    { "input_file", "input" },
    { "output_dir", "out" },
    { "flash_attention", "flash_attn" },
    { "load_timeout_seconds", "load_timeout" },
    // Nested YAML keys, written as section.key.
    { "model.path", "model" },
    { "model.file", "model" },
    { "model.gguf", "model" },
    { "server.path", "server" },
    { "server.binary", "server" },
    { "server.port", "port" },
    { "llama_server.path", "server" },
    { "llama_server.port", "port" },
};

std::string NormalizeKey(const std::string& raw)
{
    std::string key = Lower(Trim(raw));
    std::replace(key.begin(), key.end(), '-', '_');
    return key;
}

std::string CanonicalKey(const std::string& raw)
{
    const std::string key = NormalizeKey(raw);
    for (const SettingAlias& alias : kSettingAliases)
    {
        if (key == alias.alias)
            return alias.key;
    }
    return key;
}

// Options that take no value on the command line ("--attach"); "--attach=false" also works.
bool IsFlagKey(const std::string& key)
{
    static const char* const kFlags[] = {
        "headless", "help", "version", "flash_attn", "mmap", "no_mmap", "mlock", "kv_offload",
        "no_kv_offload", "cache_prompt", "no_cache_prompt", "attach", "dry_run", "quiet"
    };
    for (const char* flag : kFlags)
    {
        if (key == flag)
            return true;
    }
    return false;
}

bool ParseIntValue(const std::string& text, long long minimum, long long maximum, int& output)
{
    const std::string value = Trim(text);
    if (value.empty())
        return false;
    errno = 0;
    char* end = nullptr;
    const long long parsed = std::strtoll(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0' || parsed < minimum || parsed > maximum)
        return false;
    output = static_cast<int>(parsed);
    return true;
}

bool ParseRealValue(const std::string& text, double minimum, double maximum, double& output)
{
    const std::string value = Trim(text);
    if (value.empty())
        return false;
    errno = 0;
    char* end = nullptr;
    const double parsed = std::strtod(value.c_str(), &end);
    if (errno != 0 || end == value.c_str() || *end != '\0' || !std::isfinite(parsed) || parsed < minimum || parsed > maximum)
        return false;
    output = parsed;
    return true;
}

bool ParseBoolValue(const std::string& text, bool& output)
{
    const std::string value = Lower(Trim(text));
    if (value == "true" || value == "yes" || value == "on" || value == "1")
    {
        output = true;
        return true;
    }
    if (value == "false" || value == "no" || value == "off" || value == "0")
    {
        output = false;
        return true;
    }
    return false;
}

bool ParseThinkingValue(const std::string& text, int& output)
{
    const std::string value = Lower(Trim(text));
    bool flag = false;
    if (ParseBoolValue(value, flag))
    {
        output = flag ? 1 : 0;
        return true;
    }
    if (value == "enabled" || value == "enable")
    {
        output = 1;
        return true;
    }
    if (value == "disabled" || value == "disable")
    {
        output = 0;
        return true;
    }
    if (value == "default" || value == "auto" || value == "template")
    {
        output = -1;
        return true;
    }
    return false;
}

std::wstring ResolvePath(const std::string& value, const std::filesystem::path& base_dir)
{
    std::filesystem::path path(Utf8ToWide(Trim(value)));
    if (!base_dir.empty() && path.is_relative())
        path = base_dir / path;
    return path.wstring();
}

enum class SettingResult
{
    Applied,
    Unknown,
    Invalid
};

// Shared by command-line options (from_cli) and YAML keys. key must already be canonical.
SettingResult ApplySetting(Options& options, const std::string& key, const std::string& value, bool from_cli, const std::filesystem::path& base_dir, std::string& error)
{
    const auto invalid = [&](const char* expected)
    {
        error = "invalid value '" + value + "' for " + key + " (expected " + expected + ")";
        return SettingResult::Invalid;
    };
    const auto path_value = [&]() { return ResolvePath(value, base_dir); };
    bool flag = false;

    if (key == "model")
    {
        if (Trim(value).empty())
            return invalid("a file path");
        // Models named on the command line replace the ones from the config file.
        if (from_cli && !options.models_from_cli)
        {
            options.models.clear();
            options.models_from_cli = true;
        }
        options.models.push_back(path_value());
    }
    else if (key == "server_arg")
    {
        if (from_cli && !options.server_args_from_cli)
        {
            options.server_args.clear();
            options.server_args_from_cli = true;
        }
        options.server_args.push_back(Utf8ToWide(value));
    }
    else if (key == "input")
    {
        options.input_path = path_value();
        options.input_from_cli = options.input_from_cli || from_cli;
    }
    else if (key == "prompt" && from_cli)
    {
        // --prompt replaces an input file named in the config file.
        if (!options.input_from_cli)
            options.input_path.clear();
        options.prompt = value;
        options.has_prompt = true;
    }
    else if (key == "prompt_field") { if (Trim(value).empty()) return invalid("a field name"); options.prompt_field = Trim(value); }
    else if (key == "id_field") { if (Trim(value).empty()) return invalid("a field name"); options.id_field = Trim(value); }
    else if (key == "system") options.system_prompt = value;
    else if (key == "out") options.out_dir = path_value();
    else if (key == "results") options.results_path = path_value();
    else if (key == "perf") options.perf_path = path_value();
    else if (key == "manifest") options.manifest_path = path_value();
    else if (key == "server") options.server_path = path_value();
    else if (key == "temperature") { if (!ParseRealValue(value, 0.0, 100.0, options.temperature)) return invalid("a number >= 0"); }
    else if (key == "top_p") { if (!ParseRealValue(value, 0.0, 1.0, options.top_p)) return invalid("a number from 0 to 1"); }
    else if (key == "top_k") { if (!ParseIntValue(value, 0, 1000000, options.top_k)) return invalid("an integer >= 0"); }
    else if (key == "min_p") { if (!ParseRealValue(value, 0.0, 1.0, options.min_p)) return invalid("a number from 0 to 1"); }
    else if (key == "max_tokens") { if (!ParseIntValue(value, -1, 10000000, options.max_tokens) || options.max_tokens == 0) return invalid("a positive integer or -1"); }
    else if (key == "seed") { if (!ParseIntValue(value, -1, 2147483647, options.seed)) return invalid("an integer from -1 to 2147483647"); }
    else if (key == "repeat_penalty") { if (!ParseRealValue(value, 0.0, 100.0, options.repeat_penalty)) return invalid("a number >= 0"); }
    else if (key == "presence_penalty") { if (!ParseRealValue(value, -100.0, 100.0, options.presence_penalty)) return invalid("a number"); }
    else if (key == "frequency_penalty") { if (!ParseRealValue(value, -100.0, 100.0, options.frequency_penalty)) return invalid("a number"); }
    else if (key == "thinking") { if (!ParseThinkingValue(value, options.thinking)) return invalid("on, off or default"); }
    else if (key == "port") { if (!ParseIntValue(value, 1, 65535, options.port)) return invalid("a port from 1 to 65535"); }
    else if (key == "ctx") { if (!ParseIntValue(value, 0, 16777216, options.ctx)) return invalid("an integer >= 0"); }
    else if (key == "threads") { if (!ParseIntValue(value, 1, 4096, options.threads)) return invalid("an integer >= 1"); }
    else if (key == "threads_batch") { if (!ParseIntValue(value, 1, 4096, options.threads_batch)) return invalid("an integer >= 1"); }
    else if (key == "gpu_layers") { if (!ParseIntValue(value, 0, 100000, options.gpu_layers)) return invalid("an integer >= 0"); }
    else if (key == "batch") { if (!ParseIntValue(value, 1, 1048576, options.batch)) return invalid("an integer >= 1"); }
    else if (key == "ubatch") { if (!ParseIntValue(value, 1, 1048576, options.ubatch)) return invalid("an integer >= 1"); }
    else if (key == "load_timeout") { if (!ParseIntValue(value, 1, 86400, options.load_timeout)) return invalid("seconds from 1 to 86400"); }
    else if (key == "flash_attn") { if (!ParseBoolValue(value, options.flash_attn)) return invalid("true or false"); }
    else if (key == "mmap") { if (!ParseBoolValue(value, options.mmap)) return invalid("true or false"); }
    else if (key == "no_mmap") { if (!ParseBoolValue(value, flag)) return invalid("true or false"); options.mmap = !flag; }
    else if (key == "mlock") { if (!ParseBoolValue(value, options.mlock)) return invalid("true or false"); }
    else if (key == "kv_offload") { if (!ParseBoolValue(value, options.kv_offload)) return invalid("true or false"); }
    else if (key == "no_kv_offload") { if (!ParseBoolValue(value, flag)) return invalid("true or false"); options.kv_offload = !flag; }
    else if (key == "cache_prompt") { if (!ParseBoolValue(value, options.cache_prompt)) return invalid("true or false"); }
    else if (key == "no_cache_prompt") { if (!ParseBoolValue(value, flag)) return invalid("true or false"); options.cache_prompt = !flag; }
    else if (key == "attach") { if (!ParseBoolValue(value, options.attach)) return invalid("true or false"); }
    else if (key == "dry_run") { if (!ParseBoolValue(value, options.dry_run)) return invalid("true or false"); }
    else if (key == "quiet") { if (!ParseBoolValue(value, options.quiet)) return invalid("true or false"); }
    else return SettingResult::Unknown;
    return SettingResult::Applied;
}

// -------------------------------------------------------------------------------------------------
// YAML config (the subset settings files use: key: value scalars, comments, sections, and
// | or > block scalars; list items are skipped)
// -------------------------------------------------------------------------------------------------

bool ReadFileBytes(const std::wstring& path, std::string& data)
{
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    if (!file)
        return false;
    std::ostringstream buffer;
    buffer << file.rdbuf();
    data = buffer.str();
    return !file.bad();
}

// Position of the ':' that ends a key: followed by a space, a tab or the end of the line.
size_t FindYamlKeyColon(const std::string& line)
{
    size_t start = 0;
    if (!line.empty() && (line[0] == '"' || line[0] == '\''))
    {
        const size_t close = line.find(line[0], 1);
        if (close == std::string::npos)
            return std::string::npos;
        start = close + 1;
    }
    for (size_t index = start; index < line.size(); ++index)
    {
        if (line[index] == ':' && (index + 1 == line.size() || line[index + 1] == ' ' || line[index + 1] == '\t'))
            return index;
    }
    return std::string::npos;
}

std::string UnquoteYamlKey(const std::string& key)
{
    if (key.size() >= 2 && (key[0] == '"' || key[0] == '\'') && key.back() == key[0])
        return key.substr(1, key.size() - 2);
    return key;
}

// Parses the value after "key:". Returns false for an unterminated quoted value.
bool ParseYamlScalar(const std::string& raw, std::string& value)
{
    const std::string text = Trim(raw);
    value.clear();
    if (text.empty() || text[0] == '#')
        return true;
    if (text[0] == '"' || text[0] == '\'')
    {
        const char quote = text[0];
        bool closed = false;
        size_t index = 1;
        for (; index < text.size(); ++index)
        {
            const char character = text[index];
            if (quote == '\'' && character == '\'')
            {
                if (index + 1 < text.size() && text[index + 1] == '\'')
                {
                    value.push_back('\'');
                    ++index;
                    continue;
                }
                closed = true;
                ++index;
                break;
            }
            if (quote == '"' && character == '"')
            {
                closed = true;
                ++index;
                break;
            }
            if (quote == '"' && character == '\\' && index + 1 < text.size())
            {
                const char escaped = text[++index];
                switch (escaped)
                {
                case 'n': value.push_back('\n'); break;
                case 't': value.push_back('\t'); break;
                case 'r': value.push_back('\r'); break;
                case '"': value.push_back('"'); break;
                case '\\': value.push_back('\\'); break;
                case '/': value.push_back('/'); break;
                default:
                    value.push_back('\\');
                    value.push_back(escaped);
                    break;
                }
                continue;
            }
            value.push_back(character);
        }
        if (!closed)
            return false;
        const std::string rest = Trim(text.substr(index));
        return rest.empty() || rest[0] == '#';
    }
    size_t end = text.size();
    for (size_t index = 1; index < text.size(); ++index)
    {
        if (text[index] == '#' && (text[index - 1] == ' ' || text[index - 1] == '\t'))
        {
            end = index;
            break;
        }
    }
    value = Trim(text.substr(0, end));
    return true;
}

// Reads the lines of a | (literal) or > (folded) block scalar that follows lines[index].
std::string ReadYamlBlock(const std::vector<std::string>& lines, size_t& index, size_t parent_indent, bool folded, char chomp)
{
    std::vector<std::string> block;
    size_t block_indent = std::string::npos;
    size_t next = index + 1;
    for (; next < lines.size(); ++next)
    {
        const std::string& line = lines[next];
        const size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos)
        {
            block.push_back(std::string());
            continue;
        }
        if (first <= parent_indent)
            break;
        if (block_indent == std::string::npos)
            block_indent = first;
        if (first < block_indent)
            break;
        block.push_back(line.substr(block_indent));
    }
    index = next - 1;

    size_t trailing_blank = 0;
    while (!block.empty() && block.back().empty())
    {
        block.pop_back();
        ++trailing_blank;
    }
    std::string value;
    bool previous_text = false;
    for (size_t line = 0; line < block.size(); ++line)
    {
        if (!folded)
        {
            if (line > 0)
                value += '\n';
            value += block[line];
            continue;
        }
        if (block[line].empty())
        {
            value += '\n';
            previous_text = false;
            continue;
        }
        if (previous_text)
            value += ' ';
        value += block[line];
        previous_text = true;
    }
    if (chomp != '-' && !value.empty())
        value += '\n';
    if (chomp == '+')
        value.append(trailing_blank, '\n');
    return value;
}

bool LoadConfigFile(const std::wstring& path, Options& options, std::string& error)
{
    std::string text;
    if (!ReadFileBytes(path, text))
    {
        error = "cannot read config file " + WideToUtf8(path);
        return false;
    }
    StripUtf8Bom(text);
    const std::string file_name = WideToUtf8(path);
    const std::filesystem::path base_dir = std::filesystem::path(path).parent_path();
    const std::vector<std::string> lines = SplitLines(text);
    std::vector<std::pair<size_t, std::string>> sections; // indentation and key of open sections
    for (size_t index = 0; index < lines.size(); ++index)
    {
        const std::string& line = lines[index];
        const std::string trimmed = Trim(line);
        const std::string location = file_name + " line " + std::to_string(index + 1);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed == "---" || trimmed == "...")
            continue;
        const size_t indent = line.find_first_not_of(" \t");
        while (!sections.empty() && sections.back().first >= indent)
            sections.pop_back();
        if (trimmed[0] == '-')
            continue;
        const size_t colon = FindYamlKeyColon(trimmed);
        if (colon == std::string::npos)
        {
            Warn(location + ": not a 'key: value' line, ignored");
            continue;
        }
        const std::string key_text = UnquoteYamlKey(Trim(trimmed.substr(0, colon)));
        std::string value;
        if (!ParseYamlScalar(trimmed.substr(colon + 1), value))
        {
            error = location + ": unterminated quoted value";
            return false;
        }
        if (value == "|" || value == "|-" || value == "|+" || value == ">" || value == ">-" || value == ">+")
        {
            value = ReadYamlBlock(lines, index, indent, value[0] == '>', value.size() > 1 ? value[1] : '\0');
        }
        else if (value.empty())
        {
            sections.push_back({ indent, NormalizeKey(key_text) }); // a section such as "sampling:"
            continue;
        }
        if (value == "~" || Lower(value) == "null")
            continue;

        // "server:\n  port: 8081" is tried as server.port first, then as port.
        std::string dotted;
        for (const auto& section : sections)
            dotted += section.second + ".";
        dotted += NormalizeKey(key_text);
        std::string key = CanonicalKey(dotted);
        if (key == dotted)
            key = CanonicalKey(key_text);
        std::string setting_error;
        const SettingResult result = ApplySetting(options, key, value, false, base_dir, setting_error);
        if (result == SettingResult::Unknown)
            Warn(location + ": ignoring unknown key '" + dotted + "'");
        else if (result == SettingResult::Invalid)
        {
            error = location + ": " + setting_error;
            return false;
        }
    }
    return true;
}

// -------------------------------------------------------------------------------------------------
// Command line
// -------------------------------------------------------------------------------------------------

struct CliToken
{
    std::string name; // as typed, for messages
    std::string key;  // canonical
    std::string value;
};

bool TokenizeArguments(const std::vector<std::string>& args, std::vector<CliToken>& tokens, std::string& error)
{
    for (size_t index = 0; index < args.size(); ++index)
    {
        const std::string& arg = args[index];
        if (arg == "-h" || arg == "/?")
        {
            tokens.push_back({ arg, "help", "true" });
            continue;
        }
        if (arg.size() < 3 || arg.compare(0, 2, "--") != 0)
        {
            error = "unexpected argument '" + arg + "' (options start with --; see --help)";
            return false;
        }
        std::string name = arg.substr(2);
        std::string value;
        bool inline_value = false;
        const size_t equals = name.find('=');
        if (equals != std::string::npos)
        {
            value = name.substr(equals + 1);
            name = name.substr(0, equals);
            inline_value = true;
        }
        const std::string key = CanonicalKey(name);
        if (IsFlagKey(key))
        {
            if (!inline_value)
                value = "true";
        }
        else if (!inline_value)
        {
            if (index + 1 >= args.size())
            {
                error = "--" + name + " needs a value";
                return false;
            }
            value = args[++index];
        }
        tokens.push_back({ "--" + name, key, value });
    }
    return true;
}

// -------------------------------------------------------------------------------------------------
// Prompts
// -------------------------------------------------------------------------------------------------

struct PromptItem
{
    size_t line = 0;
    std::string id_json = "null";
    std::string prompt;
    std::string system;
    bool has_system = false;
    std::vector<LlamaChatMessage> history;
};

// Message content as a string, or OpenAI-style content parts [{"type":"text","text":"..."}].
bool MessageContentText(const JsonValue& content, std::string& text)
{
    if (content.type == JsonValue::Type::String)
    {
        text = content.text;
        return true;
    }
    if (content.type != JsonValue::Type::Array)
        return false;
    text.clear();
    for (const JsonValue& part : content.items)
    {
        if (part.type == JsonValue::Type::String)
        {
            text += part.text;
            continue;
        }
        const JsonValue* part_text = part.Find("text");
        if (!part_text || part_text->type != JsonValue::Type::String)
            return false;
        text += part_text->text;
    }
    return true;
}

bool PromptFromJson(const JsonValue& value, const Options& options, PromptItem& item, std::string& error)
{
    if (value.type == JsonValue::Type::String)
    {
        item.prompt = value.text;
        return true;
    }
    if (value.type != JsonValue::Type::Object)
    {
        error = "expected a JSON object or string";
        return false;
    }
    if (const JsonValue* id = value.Find(options.id_field))
        item.id_json = SerializeJson(*id);
    if (const JsonValue* system = value.Find("system"))
    {
        if (system->type == JsonValue::Type::String)
        {
            item.system = system->text;
            item.has_system = true;
        }
        else if (system->type != JsonValue::Type::Null)
        {
            error = "\"system\" must be a string";
            return false;
        }
    }
    if (const JsonValue* prompt = value.Find(options.prompt_field))
    {
        if (prompt->type != JsonValue::Type::String)
        {
            error = "\"" + options.prompt_field + "\" must be a string";
            return false;
        }
        item.prompt = prompt->text;
        return true;
    }
    const JsonValue* messages = value.Find("messages");
    if (!messages)
    {
        error = "missing \"" + options.prompt_field + "\" (or a \"messages\" array)";
        return false;
    }
    if (messages->type != JsonValue::Type::Array || messages->items.empty())
    {
        error = "\"messages\" must be a non-empty array";
        return false;
    }
    std::vector<LlamaChatMessage> chat;
    for (const JsonValue& message : messages->items)
    {
        const JsonValue* role = message.Find("role");
        const JsonValue* content = message.Find("content");
        std::string text;
        if (!role || role->type != JsonValue::Type::String || !content || !MessageContentText(*content, text))
        {
            error = "every message needs a string \"role\" and text \"content\"";
            return false;
        }
        if (role->text == "system")
        {
            item.system = item.system.empty() ? text : item.system + "\n\n" + text;
            item.has_system = true;
            continue;
        }
        chat.push_back({ role->text, text });
    }
    if (chat.empty() || chat.back().role != "user")
    {
        error = "the last message must have role \"user\"";
        return false;
    }
    item.prompt = chat.back().content;
    chat.pop_back();
    item.history = std::move(chat);
    return true;
}

bool LoadPrompts(const std::wstring& path, const Options& options, std::vector<PromptItem>& items, std::string& fingerprint, std::string& error)
{
    std::string text;
    if (!ReadFileBytes(path, text))
    {
        error = "cannot read input file " + WideToUtf8(path);
        return false;
    }
    fingerprint = Fnv1a64Hex(text);
    StripUtf8Bom(text);
    const std::vector<std::string> lines = SplitLines(text);
    std::set<std::string> seen_ids;
    for (size_t index = 0; index < lines.size(); ++index)
    {
        if (Trim(lines[index]).empty())
            continue;
        const std::string location = "input line " + std::to_string(index + 1);
        JsonValue value;
        std::string parse_error;
        JsonParser parser(lines[index]);
        if (!parser.Parse(value, parse_error))
        {
            error = location + ": invalid JSON: " + parse_error;
            return false;
        }
        PromptItem item;
        item.line = index + 1;
        if (!PromptFromJson(value, options, item, parse_error))
        {
            error = location + ": " + parse_error;
            return false;
        }
        if (Trim(item.prompt).empty())
        {
            error = location + ": the prompt is empty";
            return false;
        }
        if (item.id_json != "null" && !seen_ids.insert(item.id_json).second)
            Warn(location + ": id " + item.id_json + " was already used");
        items.push_back(std::move(item));
    }
    if (items.empty())
    {
        error = "the input file has no prompts";
        return false;
    }
    return true;
}

// -------------------------------------------------------------------------------------------------
// Results and measurements
// -------------------------------------------------------------------------------------------------

// Splits "<think>...</think>answer" when the server returned the reasoning inside the content.
void SplitThinkTags(std::string& answer, std::string& reasoning, int thinking)
{
    if (!reasoning.empty())
        return;
    static const std::string kOpen = "<think>";
    static const std::string kClose = "</think>";
    const size_t first_text = answer.find_first_not_of(" \t\r\n");
    const bool starts_with_open = first_text != std::string::npos && answer.compare(first_text, kOpen.size(), kOpen) == 0;
    const size_t close = answer.rfind(kClose);
    // Some templates open <think> in the prompt, so the output only has the closing tag.
    if (close != std::string::npos && (starts_with_open || thinking != 0))
    {
        std::string thought = answer.substr(0, close);
        if (starts_with_open)
            thought = thought.substr(first_text + kOpen.size());
        reasoning = thought;
        answer = answer.substr(close + kClose.size());
        return;
    }
    if (starts_with_open)
    {
        // Cut off while thinking: no answer yet.
        reasoning = answer.substr(first_text + kOpen.size());
        answer.clear();
    }
}

struct CallRecord
{
    size_t index = 0;
    size_t model_index = 0;
    std::string id_json = "null";
    bool ok = false;
    std::string finish_reason;
    bool truncated = false;
    int prompt_tokens = 0;
    int output_tokens = 0;
    int cached_tokens = 0;
    int reasoning_tokens = 0;
    double prompt_ms = 0.0;
    double decode_ms = 0.0;
    double prompt_tps = 0.0;
    double decode_tps = 0.0;
    double request_seconds = 0.0;
    double server_load_seconds = 0.0;
    double peak_memory_gb = 0.0;
    double peak_commit_gb = 0.0;
    std::string timings_source;
};

struct Totals
{
    int calls = 0;
    int ok = 0;
    int errors = 0;
    int cutoffs = 0;
    long long prompt_tokens = 0;
    long long output_tokens = 0;
    long long cached_tokens = 0;
    long long timed_prompt_tokens = 0;
    long long timed_output_tokens = 0;
    double prompt_ms = 0.0;
    double decode_ms = 0.0;
    double request_seconds = 0.0;
    double server_load_seconds = 0.0;
    double decode_tps_sum = 0.0;
    int decode_tps_count = 0;
    double peak_memory_gb = 0.0;
    double peak_commit_gb = 0.0;

    void Add(const CallRecord& call)
    {
        ++calls;
        if (!call.ok)
        {
            ++errors;
            return;
        }
        ++ok;
        if (call.truncated)
            ++cutoffs;
        prompt_tokens += call.prompt_tokens;
        output_tokens += call.output_tokens;
        cached_tokens += call.cached_tokens;
        if (call.prompt_ms > 0.0)
        {
            prompt_ms += call.prompt_ms;
            timed_prompt_tokens += call.prompt_tokens;
        }
        if (call.decode_ms > 0.0)
        {
            decode_ms += call.decode_ms;
            timed_output_tokens += call.output_tokens;
        }
        request_seconds += call.request_seconds;
        server_load_seconds += call.server_load_seconds;
        if (call.decode_tps > 0.0)
        {
            decode_tps_sum += call.decode_tps;
            ++decode_tps_count;
        }
        peak_memory_gb = std::max(peak_memory_gb, call.peak_memory_gb);
        peak_commit_gb = std::max(peak_commit_gb, call.peak_commit_gb);
    }

    JsonObject Object(bool memory_known) const
    {
        JsonObject object;
        object.Set("calls", JsonInt(calls))
            .Set("ok", JsonInt(ok))
            .Set("errors", JsonInt(errors))
            .Set("cutoffs", JsonInt(cutoffs))
            .Set("prompt_tokens", JsonInt(prompt_tokens))
            .Set("output_tokens", JsonInt(output_tokens))
            .Set("cached_tokens", JsonInt(cached_tokens))
            .Set("prompt_ms", JsonFixed(prompt_ms, 3))
            .Set("decode_ms", JsonFixed(decode_ms, 3))
            .Set("prompt_tps", JsonFixed(prompt_ms > 0.0 ? timed_prompt_tokens / (prompt_ms / 1000.0) : 0.0, 3))
            .Set("decode_tps", JsonFixed(decode_ms > 0.0 ? timed_output_tokens / (decode_ms / 1000.0) : 0.0, 3))
            .Set("mean_decode_tps", JsonFixed(decode_tps_count > 0 ? decode_tps_sum / decode_tps_count : 0.0, 3))
            .Set("request_seconds", JsonFixed(request_seconds, 3))
            .Set("server_load_seconds", JsonFixed(server_load_seconds, 3))
            .Set("peak_memory_gb", memory_known ? JsonFixed(peak_memory_gb, 3) : "null")
            .Set("peak_commit_gb", memory_known ? JsonFixed(peak_commit_gb, 3) : "null");
        return object;
    }
};

struct ModelPlan
{
    std::wstring path;
    std::string name;
    long long size_bytes = -1;
    std::wstring command;
    bool observed = false;
    std::string build_info;
    std::string observed_model_path;
    std::string chat_template_fnv;
    std::string system_fingerprint;
    int total_slots = 0;
    int n_ctx = 0;
};

std::string CallJson(const CallRecord& call, const std::vector<ModelPlan>& models, bool memory_known)
{
    JsonObject object;
    object.Set("index", JsonInt(static_cast<long long>(call.index)))
        .Set("id", call.id_json)
        .Set("model", JsonString(models[call.model_index].name))
        .Set("ok", JsonBool(call.ok))
        .Set("finish_reason", JsonString(call.finish_reason))
        .Set("truncated", JsonBool(call.truncated))
        .Set("prompt_tokens", JsonInt(call.prompt_tokens))
        .Set("output_tokens", JsonInt(call.output_tokens))
        .Set("cached_tokens", JsonInt(call.cached_tokens))
        .Set("reasoning_tokens", JsonInt(call.reasoning_tokens))
        .Set("prompt_ms", JsonFixed(call.prompt_ms, 3))
        .Set("decode_ms", JsonFixed(call.decode_ms, 3))
        .Set("prompt_tps", JsonFixed(call.prompt_tps, 3))
        .Set("decode_tps", JsonFixed(call.decode_tps, 3))
        .Set("request_seconds", JsonFixed(call.request_seconds, 3))
        .Set("server_load_seconds", JsonFixed(call.server_load_seconds, 3))
        .Set("peak_memory_gb", memory_known ? JsonFixed(call.peak_memory_gb, 3) : "null")
        .Set("peak_commit_gb", memory_known ? JsonFixed(call.peak_commit_gb, 3) : "null")
        .Set("timings_source", JsonString(call.timings_source));
    return object.Dump();
}

std::string PerfJson(const std::vector<CallRecord>& calls, const std::vector<ModelPlan>& models, bool memory_known)
{
    std::string output = "{\"calls\":[";
    for (size_t index = 0; index < calls.size(); ++index)
    {
        output += index > 0 ? ",\n  " : "\n  ";
        output += CallJson(calls[index], models, memory_known);
    }
    output += calls.empty() ? "]" : "\n]";

    output += ",\"models\":[";
    Totals overall;
    for (size_t model_index = 0; model_index < models.size(); ++model_index)
    {
        Totals totals;
        for (const CallRecord& call : calls)
        {
            if (call.model_index == model_index)
                totals.Add(call);
        }
        JsonObject object = totals.Object(memory_known);
        object.Set("model", JsonString(models[model_index].name));
        output += model_index > 0 ? ",\n  " : "\n  ";
        output += object.Dump();
    }
    output += models.empty() ? "]" : "\n]";
    for (const CallRecord& call : calls)
        overall.Add(call);
    output += ",\"totals\":" + overall.Object(memory_known).Dump() + "}\n";
    return output;
}

struct RunContext
{
    const Options* options = nullptr;
    std::vector<std::string> invocation;
    std::wstring config_path;
    std::string input_fingerprint;
    size_t prompt_count = 0;
    std::vector<ModelPlan>* models = nullptr;
    std::string state;
    size_t calls_planned = 0;
    size_t calls_done = 0;
    Totals totals;
};

std::string ManifestJson(const RunContext& context)
{
    const Options& options = *context.options;

    JsonObject request;
    request.Set("cache_prompt", JsonBool(options.cache_prompt))
        .Set("chat_template_kwargs", options.thinking >= 0 ? std::string("{\"enable_thinking\":") + JsonBool(options.thinking > 0) + "}" : std::string("null"))
        .Set("frequency_penalty", JsonReal(options.frequency_penalty))
        .Set("max_tokens", JsonInt(options.max_tokens))
        .Set("min_p", JsonReal(options.min_p))
        .Set("presence_penalty", JsonReal(options.presence_penalty))
        .Set("repeat_penalty", JsonReal(options.repeat_penalty))
        .Set("seed", JsonInt(options.seed))
        .Set("stream", "true")
        .Set("system", options.system_prompt.empty() ? std::string("null") : JsonString(options.system_prompt))
        .Set("temperature", JsonReal(options.temperature))
        .Set("top_k", JsonInt(options.top_k))
        .Set("top_p", JsonReal(options.top_p));

    std::vector<std::string> extra_args;
    for (const std::wstring& argument : options.server_args)
        extra_args.push_back(WideToUtf8(argument));
    JsonObject server;
    server.Set("attach", JsonBool(options.attach))
        .Set("batch", JsonInt(options.batch))
        .Set("ctx", JsonInt(options.ctx))
        .Set("extra_args", JsonStringArray(extra_args))
        .Set("flash_attn", JsonBool(options.flash_attn))
        .Set("gpu_layers", JsonInt(options.gpu_layers))
        .Set("jinja", JsonBool(options.thinking >= 0))
        .Set("kv_offload", JsonBool(options.kv_offload))
        .Set("load_timeout_seconds", JsonInt(options.load_timeout))
        .Set("mlock", JsonBool(options.mlock))
        .Set("mmap", JsonBool(options.mmap))
        .Set("parallel", "1")
        .Set("path", options.attach ? std::string("null") : JsonString(WideToUtf8(options.server_path)))
        .Set("port", JsonInt(options.port))
        .Set("threads", JsonInt(options.threads))
        .Set("threads_batch", JsonInt(options.threads_batch))
        .Set("ubatch", JsonInt(options.ubatch));

    std::string models = "[";
    for (size_t index = 0; index < context.models->size(); ++index)
    {
        const ModelPlan& model = (*context.models)[index];
        std::string observed = "null";
        if (model.observed)
        {
            JsonObject values;
            values.Set("build_info", JsonString(model.build_info))
                .Set("chat_template_fnv1a64", model.chat_template_fnv.empty() ? std::string("null") : JsonString(model.chat_template_fnv))
                .Set("model_path", JsonString(model.observed_model_path))
                .Set("n_ctx", JsonInt(model.n_ctx))
                .Set("system_fingerprint", JsonString(model.system_fingerprint))
                .Set("total_slots", JsonInt(model.total_slots));
            observed = values.Dump();
        }
        std::error_code error;
        const std::filesystem::path absolute = model.path.empty() ? std::filesystem::path() : std::filesystem::absolute(std::filesystem::path(model.path), error);
        JsonObject entry;
        entry.Set("name", JsonString(model.name))
            .Set("path", model.path.empty() ? std::string("null") : JsonString(WideToUtf8(model.path)))
            .Set("absolute_path", model.path.empty() || error ? std::string("null") : JsonString(PathToUtf8(absolute)))
            .Set("size_bytes", model.size_bytes >= 0 ? JsonInt(model.size_bytes) : std::string("null"))
            .Set("server_command", model.command.empty() ? std::string("null") : JsonString(WideToUtf8(model.command)))
            .Set("observed", observed);
        models += index > 0 ? ",\n    " : "\n    ";
        models += entry.Dump();
    }
    models += context.models->empty() ? "]" : "\n  ]";

    JsonObject input;
    input.Set("kind", options.has_prompt ? "\"prompt\"" : "\"jsonl\"")
        .Set("path", options.has_prompt ? std::string("null") : JsonString(WideToUtf8(options.input_path)))
        .Set("fnv1a64", JsonString(context.input_fingerprint))
        .Set("prompts", JsonInt(static_cast<long long>(context.prompt_count)))
        .Set("prompt_field", JsonString(options.prompt_field))
        .Set("id_field", JsonString(options.id_field));

    JsonObject outputs;
    outputs.Set("results", JsonString(WideToUtf8(options.results_path)))
        .Set("perf", JsonString(WideToUtf8(options.perf_path)))
        .Set("manifest", JsonString(WideToUtf8(options.manifest_path)));

    JsonObject status;
    status.Set("state", JsonString(context.state))
        .Set("calls_planned", JsonInt(static_cast<long long>(context.calls_planned)))
        .Set("calls_done", JsonInt(static_cast<long long>(context.calls_done)))
        .Set("ok", JsonInt(context.totals.ok))
        .Set("errors", JsonInt(context.totals.errors))
        .Set("cutoffs", JsonInt(context.totals.cutoffs));

    JsonObject manifest;
    manifest.Set("format", JsonString(kManifestFormat))
        .Set("invocation", JsonStringArray(context.invocation))
        .Set("config_file", context.config_path.empty() ? std::string("null") : JsonString(WideToUtf8(context.config_path)))
        .Set("dry_run", JsonBool(options.dry_run))
        .Set("input", input.Dump())
        .Set("models", models)
        .Set("outputs", outputs.Dump())
        .Set("request", request.Dump())
        .Set("server", server.Dump())
        .Set("status", status.Dump());
    return manifest.DumpLines();
}

bool EnsureParentDirectory(const std::wstring& path, std::string& error)
{
    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (parent.empty())
        return true;
    std::error_code code;
    std::filesystem::create_directories(parent, code);
    if (code)
    {
        error = "cannot create directory " + PathToUtf8(parent) + ": " + code.message();
        return false;
    }
    return true;
}

// Writes next to the target and renames, so readers never see a half-written file.
bool WriteFileReplacing(const std::wstring& path, const std::string& data)
{
    const std::wstring temp = path + L".tmp";
    {
        std::ofstream file(std::filesystem::path(temp), std::ios::binary | std::ios::trunc);
        if (file)
        {
            file.write(data.data(), static_cast<std::streamsize>(data.size()));
            file.close();
            if (file && ::MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                return true;
        }
    }
    ::DeleteFileW(temp.c_str());
    std::ofstream file(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    if (!file)
        return false;
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
    file.close();
    return static_cast<bool>(file);
}

// Waits for the single final (non-streaming) result of a StartComparison call.
bool WaitForFinalResult(LlamaServerBackend& backend, LlamaRunResult& final_result)
{
    bool have_result = false;
    LlamaRunResult result;
    while (true)
    {
        const bool busy = backend.IsBusy();
        while (backend.PopResult(result))
        {
            if (!result.partial)
            {
                final_result = std::move(result);
                have_result = true;
            }
        }
        if (!busy)
            return have_result;
        ::Sleep(5);
    }
}

std::string ShortId(const std::string& id_json)
{
    return id_json.size() > 40 ? id_json.substr(0, 37) + "..." : id_json;
}

int RunHeadlessMain(int argc, wchar_t** argv, const std::wstring& detected_server_path)
{
    std::vector<std::string> args;
    std::vector<std::string> invocation;
    invocation.push_back(argc > 0 && argv[0] ? WideToUtf8(argv[0]) : std::string("renaro_model_playground.exe"));
    for (int index = 1; index < argc; ++index)
    {
        args.push_back(WideToUtf8(argv[index] ? argv[index] : L""));
        invocation.push_back(args.back());
    }

    std::vector<CliToken> tokens;
    std::string error;
    if (!TokenizeArguments(args, tokens, error))
    {
        ErrorLine(error);
        return 2;
    }

    bool help = false;
    bool version = false;
    std::wstring config_path;
    for (const CliToken& token : tokens)
    {
        if (token.key == "help")
            help = true;
        else if (token.key == "version")
            version = true;
        else if (token.key == "config")
            config_path = Utf8ToWide(Trim(token.value));
    }
    if (help)
    {
        PrintUsage();
        return 0;
    }
    if (version)
    {
        std::printf("%s\n", kManifestFormat);
        return 0;
    }

    Options options;
    if (!config_path.empty() && !LoadConfigFile(config_path, options, error))
    {
        ErrorLine(error);
        return 2;
    }
    for (const CliToken& token : tokens)
    {
        if (token.key == "help" || token.key == "version" || token.key == "config" || token.key == "headless")
            continue;
        std::string setting_error;
        const SettingResult result = ApplySetting(options, token.key, token.value, true, std::filesystem::path(), setting_error);
        if (result == SettingResult::Unknown)
        {
            ErrorLine("unknown option " + token.name + " (see --help)");
            return 2;
        }
        if (result == SettingResult::Invalid)
        {
            ErrorLine(token.name + ": " + setting_error);
            return 2;
        }
    }
    g_quiet = options.quiet;

    // Validate.
    if (options.has_prompt == !options.input_path.empty())
    {
        ErrorLine(options.has_prompt ? "use either --input or --prompt, not both" : "give the prompts with --input <file.jsonl> or --prompt <text>");
        return 2;
    }
    if (options.models.empty() && !options.attach)
    {
        ErrorLine("--model <file.gguf> is required");
        return 2;
    }
    if (!options.attach)
    {
        if (options.server_path.empty())
            options.server_path = detected_server_path;
        std::error_code server_error;
        if (options.server_path.empty())
        {
            if (!options.dry_run)
            {
                ErrorLine("llama-server.exe was not found; pass --server <path>");
                return 2;
            }
            Warn("llama-server.exe was not found; pass --server <path>");
        }
        else if (!std::filesystem::is_regular_file(std::filesystem::path(options.server_path), server_error))
        {
            if (!options.dry_run)
            {
                ErrorLine("llama-server not found: " + WideToUtf8(options.server_path));
                return 2;
            }
            Warn("llama-server not found: " + WideToUtf8(options.server_path));
        }
    }
    if (options.threads_batch <= 0)
        options.threads_batch = options.threads;
    if (options.seed < 0)
        Warn("--seed -1 lets llama-server pick a random seed, so results will not repeat");
    if (options.cache_prompt)
        Warn("--cache-prompt reuses the KV cache between prompts, which can change results between runs");
    if (options.out_dir.empty())
        options.out_dir = L".";
    const std::filesystem::path out_dir(options.out_dir);
    if (options.results_path.empty())
        options.results_path = (out_dir / L"results.jsonl").wstring();
    if (options.perf_path.empty())
        options.perf_path = (out_dir / L"perf.json").wstring();
    if (options.manifest_path.empty())
        options.manifest_path = (out_dir / L"manifest.json").wstring();

    std::vector<ModelPlan> models;
    if (options.models.empty())
    {
        ModelPlan model;
        model.name = "attached";
        models.push_back(model);
    }
    for (const std::wstring& path : options.models)
    {
        ModelPlan model;
        model.path = path;
        const std::filesystem::path file(path);
        model.name = PathToUtf8(file.stem());
        if (model.name.empty())
            model.name = PathToUtf8(file.filename());
        if (model.name.empty())
            model.name = WideToUtf8(path);
        std::error_code code;
        const bool exists = std::filesystem::is_regular_file(file, code);
        if (exists)
        {
            const std::uintmax_t size = std::filesystem::file_size(file, code);
            if (!code)
                model.size_bytes = static_cast<long long>(size);
        }
        else if (!options.attach)
        {
            if (!options.dry_run)
            {
                ErrorLine("model file not found: " + WideToUtf8(path));
                return 2;
            }
            Warn("model file not found: " + WideToUtf8(path));
        }
        models.push_back(model);
    }

    std::vector<PromptItem> prompts;
    std::string fingerprint;
    if (options.has_prompt)
    {
        if (Trim(options.prompt).empty())
        {
            ErrorLine("--prompt is empty");
            return 2;
        }
        PromptItem item;
        item.prompt = options.prompt;
        prompts.push_back(item);
        fingerprint = Fnv1a64Hex(options.prompt);
    }
    else if (!LoadPrompts(options.input_path, options, prompts, fingerprint, error))
    {
        ErrorLine(error);
        return 2;
    }

    // Everything that stays fixed for the run.
    LlamaRunConfig base;
    base.server_path = options.server_path;
    base.port = options.port;
    base.context = options.ctx;
    base.temperature = static_cast<float>(options.temperature);
    base.threads = options.threads;
    base.threads_batch = options.threads_batch;
    base.gpu_layers = options.gpu_layers;
    base.batch_size = options.batch;
    base.ubatch_size = options.ubatch;
    base.seed = options.seed;
    base.max_tokens = options.max_tokens;
    base.flash_attention = options.flash_attn;
    base.cache_prompt = options.cache_prompt;
    base.mmap = options.mmap;
    base.mlock = options.mlock;
    base.kv_offload = options.kv_offload;
    base.top_p = static_cast<float>(options.top_p);
    base.top_k = options.top_k;
    base.min_p = static_cast<float>(options.min_p);
    base.repeat_penalty = static_cast<float>(options.repeat_penalty);
    base.presence_penalty = static_cast<float>(options.presence_penalty);
    base.frequency_penalty = static_cast<float>(options.frequency_penalty);
    base.explicit_sampling = true;
    base.enable_thinking = options.thinking;
    base.parallel = 1;
    base.extra_server_args = options.server_args;
    base.attach_existing = options.attach;
    base.health_timeout_ms = options.load_timeout * 1000;
    base.allow_empty_content = true;
    base.keep_server_alive = true;
    if (!options.attach)
    {
        for (ModelPlan& model : models)
            model.command = LlamaServerCommandLine(LlamaModelJob{ model.name, model.path }, base);
    }

    RunContext context;
    context.options = &options;
    context.invocation = invocation;
    context.config_path = config_path;
    context.input_fingerprint = fingerprint;
    context.prompt_count = prompts.size();
    context.models = &models;
    context.calls_planned = prompts.size() * models.size();
    context.state = options.dry_run ? "dry-run" : "running";

    if (!EnsureParentDirectory(options.manifest_path, error))
    {
        ErrorLine(error);
        return 3;
    }
    if (!WriteFileReplacing(options.manifest_path, ManifestJson(context)))
    {
        ErrorLine("cannot write " + WideToUtf8(options.manifest_path));
        return 3;
    }

    if (options.dry_run)
    {
        Info("dry run: " + std::to_string(prompts.size()) + " prompt(s) x " + std::to_string(models.size()) + " model(s) = " + std::to_string(context.calls_planned) + " call(s)");
        for (const ModelPlan& model : models)
            Info("  " + model.name + ": " + (model.command.empty() ? std::string("attach to port ") + std::to_string(options.port) : WideToUtf8(model.command)));
        Info("manifest: " + WideToUtf8(options.manifest_path));
        return 0;
    }

    if (!EnsureParentDirectory(options.results_path, error) || !EnsureParentDirectory(options.perf_path, error))
    {
        ErrorLine(error);
        return 3;
    }

    if (options.attach)
    {
        const auto deadline = ::GetTickCount64() + static_cast<ULONGLONG>(options.load_timeout) * 1000ull;
        bool healthy = false;
        while (!healthy && ::GetTickCount64() < deadline)
        {
            healthy = LlamaServerAnswers(options.port, true);
            if (!healthy)
                ::Sleep(250);
        }
        if (!healthy)
        {
            ErrorLine("no healthy llama-server answered on port " + std::to_string(options.port));
            return 3;
        }
    }
    else if (LlamaServerAnswers(options.port, false))
    {
        // Another server on the port would answer our health checks and requests.
        ErrorLine("port " + std::to_string(options.port) + " is already in use; stop that server or pick another --port");
        return 3;
    }

    std::ofstream results_file(std::filesystem::path(options.results_path), std::ios::binary | std::ios::trunc);
    if (!results_file)
    {
        ErrorLine("cannot write " + WideToUtf8(options.results_path));
        return 3;
    }

    const bool memory_known = !options.attach;
    std::vector<CallRecord> calls;
    bool interrupted = false;
    bool write_failed = false;
    g_interrupted.store(false);
    LlamaServerBackend backend;
    g_active_backend.store(&backend);
    ::SetConsoleCtrlHandler(ConsoleControlHandler, TRUE);

    for (size_t model_index = 0; model_index < models.size() && !interrupted && !write_failed; ++model_index)
    {
        ModelPlan& model = models[model_index];
        Info("model " + model.name + (options.attach ? " (attached server on port " + std::to_string(options.port) + ")" : std::string()));
        if (!model.command.empty())
            Info("  " + WideToUtf8(model.command));
        for (size_t prompt_index = 0; prompt_index < prompts.size(); ++prompt_index)
        {
            if (g_interrupted.load())
            {
                interrupted = true;
                break;
            }
            const PromptItem& item = prompts[prompt_index];
            LlamaRunConfig config = base;
            config.system_prompt = item.has_system ? item.system : options.system_prompt;
            config.history = item.history;

            LlamaRunResult result;
            std::string start_error;
            bool have_result = false;
            if (backend.StartComparison({ LlamaModelJob{ model.name, model.path } }, item.prompt, config, start_error))
            {
                if (g_interrupted.load())
                    backend.RequestStop();
                have_result = WaitForFinalResult(backend, result);
            }
            else
            {
                result.ok = false;
                result.error = start_error;
                have_result = true;
            }
            if (!have_result || result.cancelled)
            {
                interrupted = true;
                break;
            }

            std::string answer;
            std::string reasoning;
            std::string finish_reason = "error";
            if (result.ok)
            {
                answer = result.content;
                reasoning = result.reasoning;
                SplitThinkTags(answer, reasoning, options.thinking);
                answer = Trim(answer);
                reasoning = Trim(reasoning);
                finish_reason = result.finish_reason.empty() ? std::string("unknown") : result.finish_reason;
            }
            const bool truncated = result.ok && finish_reason == "length";
            const std::string error_text = result.error.empty() ? std::string("llama-server did not return a usable result") : result.error;

            JsonObject line;
            line.Set("answer", JsonString(answer))
                .Set("error", result.ok ? std::string("null") : JsonString(error_text))
                .Set("finish_reason", JsonString(finish_reason))
                .Set("id", item.id_json)
                .Set("index", JsonInt(static_cast<long long>(prompt_index)))
                .Set("model", JsonString(model.name))
                .Set("ok", JsonBool(result.ok))
                .Set("reasoning", JsonString(reasoning))
                .Set("truncated", JsonBool(truncated));
            const std::string serialized = line.Dump() + "\n";
            results_file.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
            results_file.flush();
            if (!results_file)
            {
                ErrorLine("cannot write " + WideToUtf8(options.results_path));
                write_failed = true;
                break;
            }

            CallRecord call;
            call.index = prompt_index;
            call.model_index = model_index;
            call.id_json = item.id_json;
            call.ok = result.ok;
            call.finish_reason = finish_reason;
            call.truncated = truncated;
            if (result.ok)
            {
                call.prompt_tokens = result.prompt_tokens;
                call.output_tokens = result.predicted_tokens;
                call.cached_tokens = result.tokens_cached;
                call.reasoning_tokens = result.reasoning_tokens;
                call.prompt_ms = result.prompt_ms;
                call.decode_ms = result.predicted_ms;
                call.prompt_tps = result.prompt_tokens_per_second;
                call.decode_tps = result.decode_tokens_per_second;
                call.peak_memory_gb = result.peak_memory_gb;
                call.peak_commit_gb = result.peak_private_memory_gb;
                call.timings_source = result.timings_source;
            }
            call.request_seconds = result.request_seconds;
            call.server_load_seconds = result.server_load_seconds;
            calls.push_back(call);
            context.totals.Add(call);
            context.calls_done = calls.size();
            if (!WriteFileReplacing(options.perf_path, PerfJson(calls, models, memory_known)))
                Warn("cannot write " + WideToUtf8(options.perf_path));

            if (result.ok && !model.observed)
            {
                model.observed = true;
                model.build_info = result.build_info;
                model.observed_model_path = result.model_path;
                model.chat_template_fnv = result.chat_template.empty() ? std::string() : Fnv1a64Hex(result.chat_template);
                model.system_fingerprint = result.system_fingerprint;
                model.total_slots = result.total_slots;
                model.n_ctx = result.context_size;
                if (!options.attach && model.total_slots > 1)
                    Warn("llama-server reports " + std::to_string(model.total_slots) + " slots although it was started with -np 1");
                WriteFileReplacing(options.manifest_path, ManifestJson(context));
            }

            char progress[512] = {};
            if (result.ok)
            {
                std::snprintf(progress, sizeof(progress), "[%llu/%llu] %s id=%s %s  prompt %d tok  output %d tok  %.1f / %.1f tok/s",
                    static_cast<unsigned long long>(context.calls_done), static_cast<unsigned long long>(context.calls_planned),
                    model.name.c_str(), ShortId(item.id_json).c_str(), finish_reason.c_str(),
                    call.prompt_tokens, call.output_tokens, call.prompt_tps, call.decode_tps);
                Info(progress);
            }
            else
            {
                std::snprintf(progress, sizeof(progress), "[%llu/%llu] %s id=%s ",
                    static_cast<unsigned long long>(context.calls_done), static_cast<unsigned long long>(context.calls_planned),
                    model.name.c_str(), ShortId(item.id_json).c_str());
                PrintLine(stderr, std::string(progress) + "error: " + error_text);
            }
            if (g_interrupted.load())
            {
                interrupted = true;
                break;
            }
        }
        // A fresh server for the next model, so its load time and memory peak are its own.
        backend.Stop();
    }
    backend.Stop();
    ::SetConsoleCtrlHandler(ConsoleControlHandler, FALSE);
    g_active_backend.store(nullptr);
    results_file.close();

    context.state = write_failed ? "failed" : (interrupted ? "interrupted" : "complete");
    if (!WriteFileReplacing(options.perf_path, PerfJson(calls, models, memory_known)))
        Warn("cannot write " + WideToUtf8(options.perf_path));
    if (!WriteFileReplacing(options.manifest_path, ManifestJson(context)))
        Warn("cannot write " + WideToUtf8(options.manifest_path));

    char summary[256] = {};
    std::snprintf(summary, sizeof(summary), "%s: %d call(s), %d ok, %d error(s), %d cut off (finish_reason length)",
        context.state.c_str(), context.totals.calls, context.totals.ok, context.totals.errors, context.totals.cutoffs);
    Info(summary);
    Info("results:  " + WideToUtf8(options.results_path));
    Info("perf:     " + WideToUtf8(options.perf_path));
    Info("manifest: " + WideToUtf8(options.manifest_path));

    if (write_failed)
        return 3;
    if (interrupted)
    {
        PrintLine(stderr, "interrupted");
        return 130;
    }
    return context.totals.errors > 0 ? 1 : 0;
}
}

bool HeadlessRequested(int argc, wchar_t** argv)
{
    for (int index = 1; index < argc; ++index)
    {
        if (argv[index] && (argv[index][0] == L'-' || std::wcscmp(argv[index], L"/?") == 0))
            return true;
    }
    return false;
}

int RunHeadless(int argc, wchar_t** argv, const std::wstring& detected_server_path)
{
    const UINT previous_output_cp = ::GetConsoleOutputCP();
    ::SetConsoleOutputCP(CP_UTF8);
    const int exit_code = RunHeadlessMain(argc, argv, detected_server_path);
    if (previous_output_cp != 0)
        ::SetConsoleOutputCP(previous_output_cp);
    return exit_code;
}
