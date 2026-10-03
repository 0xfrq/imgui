# Renaro Model Playground (Dear ImGui)

This example ports the Renaro blue Terminal Pulse playground into a native Dear ImGui + Win32 + DirectX 11 application.

It includes the complete UI loop:

- Renaro-branded sidebar and model library
- GGUF/bin/safetensors file picker with staged local-model selection
- Conversation input and staged response simulation
- Prompt/decode speed, first-token, memory, token-flow, diagnostics, and trace surfaces
- Temperature, context, threads, GPU-layer, seed, and prompt-cache controls
- Copyable run command, keyboard navigation, command palette, recent sessions, and new sessions
- Existing Renaro logo loading from `renaro/assets/logo/white-transparent.png`

The UI deliberately labels inference values as staged demo data until a llama.cpp adapter is connected. The example is the shell for that adapter; it does not execute a model yet.

## Build locally

Run `build_win64.bat` from a Visual Studio Developer Command Prompt. It writes the executable to `build/renaro_model_playground/` and expects the repository's `renaro/assets/logo/` directory to remain available beside the build output.
