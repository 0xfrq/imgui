# Renaro Model Playground (Dear ImGui)

Renaro Model Playground is a native Dear ImGui workbench for testing local open-weight models with llama.cpp. It uses one comparison surface: select one or more local model files, send the same prompt to each model, and compare their responses and runtime measurements.

## What it does

- Imports local `.gguf` model files, the native llama.cpp format.
- Selects one or several models for a comparison run.
- Starts one `llama-server.exe` process per selected model, sequentially.
- Sends prompts through llama.cpp's `/v1/chat/completions` endpoint.
- Displays the real assistant response, prompt throughput, decode throughput, prompt evaluation time, and server working-set memory.
- Keeps diagnostics and a run trace beside the conversation.
- Exposes context, temperature, threads, GPU layers, and seed controls.
- Lets the user select `llama-server.exe` from Settings and copy the equivalent launch command.

Models run sequentially so a later model does not compete with an earlier model for RAM or VRAM. This produces more useful performance comparisons on typical research workstations.

## llama.cpp requirement

The app does not bundle llama.cpp. Build or download a compatible `llama-server.exe` from llama.cpp and select it in Settings, or place it in one of the paths the app checks at startup:

- beside the executable
- `tools/llama-server.exe`
- `build/bin/llama-server.exe`

The selected server must expose the standard llama.cpp `/health` and OpenAI-compatible `/v1/chat/completions` endpoints.

## Build locally

Run `build_win64.bat` from a Visual Studio Developer Command Prompt. It writes the executable to `build/renaro_model_playground/` and links WinHTTP and PSAPI for the llama.cpp adapter.

The executable expects the Renaro logo at `renaro/assets/logo/white-transparent.png` beside the repository or packaged under the executable's directory.
