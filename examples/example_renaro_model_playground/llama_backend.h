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
    int threads_batch = 8;
    int gpu_layers = 0;
    int batch_size = 512;
    int ubatch_size = 512;
    int seed = 42;
    int max_tokens = 512;
    bool flash_attention = false;
    bool cache_prompt = true;
    bool mmap = true;
    bool mlock = false;
    bool kv_offload = true;
};

struct LlamaRunResult
{
    std::string model;
    bool ok = false;
    bool last = false;
    bool partial = false;
    bool streaming = false;
    bool slot_info_available = false;
    bool metrics_available = false;
    bool prompt_cache_reused = false;
    bool slot_truncated = false;
    bool stopped_eos = false;
    bool stopped_word = false;
    bool stopped_limit = false;
    std::string text;
    std::string delta;
    std::string reasoning;
    std::string reasoning_delta;
    std::string error;
    std::string finish_reason;
    std::string response_model;
    std::string response_id;
    std::string response_object;
    std::string system_fingerprint;
    std::string model_path;
    std::string model_alias;
    std::string build_info;
    std::string chat_template;
    std::string slot_state;
    std::string metrics_summary;
    std::string timings_source;
    int http_status = 0;
    int slot_id = -1;
    int prompt_tokens = 0;
    int predicted_tokens = 0;
    int total_tokens = 0;
    int context_size = 0;
    int batch_size = 0;
    int ubatch_size = 0;
    int threads = 0;
    int threads_batch = 0;
    int gpu_layers = 0;
    int n_past = 0;
    int n_prompt_tokens = 0;
    int n_decoded = 0;
    int n_cache_tokens = 0;
    int n_predict_limit = 0;
    int tokens_cached = 0;
    int tokens_evaluated = 0;
    int tokens_predicted = 0;
    int reasoning_tokens = 0;
    int accepted_prediction_tokens = 0;
    int rejected_prediction_tokens = 0;
    int response_bytes = 0;
    int metrics_requests_total = 0;
    int metrics_requests_processing = 0;
    int metrics_requests_deferred = 0;
    int metrics_prompt_tokens_total = 0;
    int metrics_predicted_tokens_total = 0;
    double prompt_tokens_per_second = 0.0;
    double decode_tokens_per_second = 0.0;
    double prompt_ms = 0.0;
    double predicted_ms = 0.0;
    double prompt_ms_per_token = 0.0;
    double predicted_ms_per_token = 0.0;
    double tokenize_ms = 0.0;
    double sample_ms = 0.0;
    double total_ms = 0.0;
    double load_ms = 0.0;
    double start_ms = 0.0;
    double metrics_prompt_seconds_total = 0.0;
    double metrics_predicted_seconds_total = 0.0;
    double prompt_eval_seconds = 0.0;
    double request_seconds = 0.0;
    double server_load_seconds = 0.0;
    double cpu_seconds = 0.0;
    double memory_gb = 0.0;
    double peak_memory_gb = 0.0;
    double private_memory_gb = 0.0;
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
