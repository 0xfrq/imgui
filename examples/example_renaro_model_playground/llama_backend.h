#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct LlamaModelJob
{
    std::string name;
    std::wstring path;
};

struct LlamaRunConfig
{
    std::wstring server_path;
    int port = 8080;
    int context = 8192;
    float temperature = 0.70f;
    int threads = 8;
    int gpu_layers = 0;
    int seed = 42;
    int max_tokens = 512;
};

struct LlamaRunResult
{
    std::string model;
    bool ok = false;
    bool last = false;
    std::string text;
    std::string error;
    double prompt_tokens_per_second = 0.0;
    double decode_tokens_per_second = 0.0;
    double prompt_eval_seconds = 0.0;
    double memory_gb = 0.0;
};

class LlamaServerBackend
{
public:
    LlamaServerBackend() = default;
    ~LlamaServerBackend();

    LlamaServerBackend(const LlamaServerBackend&) = delete;
    LlamaServerBackend& operator=(const LlamaServerBackend&) = delete;

    bool StartComparison(const std::vector<LlamaModelJob>& models, const std::string& prompt, const LlamaRunConfig& config, std::string& error);
    void Stop();
    bool PopResult(LlamaRunResult& result);
    bool IsBusy() const;
    std::string Status() const;

private:
    void RunComparison(std::vector<LlamaModelJob> models, std::string prompt, LlamaRunConfig config);
    bool RunModel(const LlamaModelJob& model, const std::string& prompt, const LlamaRunConfig& config, LlamaRunResult& result);
    bool StartServer(const LlamaModelJob& model, const LlamaRunConfig& config, std::string& error);
    void StopServer();
    bool WaitForHealth(const LlamaRunConfig& config, std::string& error);
    bool IsStopRequested() const;
    void SetStatus(const std::string& status);
    void PushResult(LlamaRunResult result);

    mutable std::mutex mutex_;
    std::thread worker_;
    std::deque<LlamaRunResult> results_;
    PROCESS_INFORMATION process_ = {};
    bool busy_ = false;
    bool stop_requested_ = false;
    std::string status_ = "ready";
};
