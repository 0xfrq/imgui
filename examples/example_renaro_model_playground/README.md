# Renaro Model Playground (Dear ImGui)

Renaro Model Playground is a native Dear ImGui workbench for testing local open-weight models with llama.cpp. It uses one comparison surface: select one or more local model files, send the same prompt to each model, and compare their responses and runtime measurements.

## What it does

- Lets the user build the model library by importing one or several local `.gguf` files.
- Starts one `llama-server.exe` process per selected model, sequentially.
- Sends prompts through llama.cpp's `/v1/chat/completions` endpoint with server-sent event streaming; assistant text grows live while the model is decoding.
- Renders common Markdown in model responses: headings, lists, quotes, code fences, inline code, emphasis, links, and rules.
- Displays prompt/decode throughput, token counts, prompt/decode milliseconds, per-token timing, request time, server load time, context/cache counts, CPU time, working/peak/private memory, finish reason, HTTP status, response size, slot counters, stop flags, model metadata, and Prometheus counters when the server exposes them.
- Keeps per-model diagnostics and a detailed run trace beside the conversation.
- Exposes context, temperature, max output tokens, prompt-cache reuse, threads, batch threads, GPU layers, batch and micro-batch sizes, flash attention, memory mapping, memory locking, KV offload, and seed controls.
- Lets the user select `llama-server.exe` from Settings and copy the equivalent launch command.

Models run sequentially so a later model does not compete with an earlier model for RAM or VRAM. This produces more useful performance comparisons on typical research workstations.

The model library starts empty by design. No model names or placeholder entries are bundled into the playground.

## llama.cpp requirement

The app does not bundle llama.cpp. Build or download a compatible `llama-server.exe` from llama.cpp and select it in Settings, or place it in one of the paths the app checks at startup:

- beside the executable
- `tools/llama-server.exe`
- `build/bin/llama-server.exe`

The selected server must expose the standard llama.cpp `/health` and OpenAI-compatible `/v1/chat/completions` endpoints. `/props`, `/slots`, and `/metrics` are optional enrichments; older builds continue to work without them.

## Build locally

Run `build_win64.bat` from a Visual Studio Developer Command Prompt. It writes the executable to `build/renaro_model_playground/` and links WinHTTP and PSAPI for the llama.cpp adapter.

The executable expects the Renaro logo at `renaro/assets/logo/white-transparent.png` beside the repository or packaged under the executable's directory.

## Build with GitHub Actions

Open the repository's Actions tab and run `Build Renaro Model Playground`, or push a change under `examples/example_renaro_model_playground/`. Download the `renaro-model-playground-windows` artifact from the completed run. Add your own `llama-server.exe` and `.gguf` files locally; model binaries are intentionally not committed or bundled.
