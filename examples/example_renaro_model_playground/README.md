# Renaro Model Playground (Dear ImGui)

Renaro Model Playground is a native Dear ImGui workbench for testing local open-weight models with llama.cpp. It uses one comparison surface: select one or more local model files, send the same prompt to each model, and compare their responses and runtime measurements.

## What it does

- Lets the user build the model library by importing one or several local `.gguf` files.
- Starts one `llama-server.exe` process per selected model, sequentially.
- Sends prompts through llama.cpp's `/v1/chat/completions` endpoint with server-sent event streaming; assistant text grows live while the model is decoding.
- Renders common Markdown in model responses: headings, lists, quotes, code fences, inline code, emphasis, links, and rules.
- Displays prompt/decode throughput, token counts, prompt/decode milliseconds, per-token timing, request time, server load time, context/cache counts, CPU time, working/peak/private memory, finish reason, HTTP status, response size, slot counters, stop flags, model metadata, and Prometheus counters when the server exposes them.
- Keeps per-model diagnostics and a detailed run trace beside the conversation.
- Exposes context, temperature, top-p, top-k, thinking on/off (`enable_thinking`), max output tokens, prompt-cache reuse, threads, batch threads, GPU layers, batch and micro-batch sizes, flash attention, memory mapping, memory locking, KV offload, and seed controls.
- Has a headless command-line mode for batch runs (see below).
- Lets the user select `llama-server.exe` from Settings and copy the equivalent launch command.

## Interface

- A navigation rail (Playground, Diagnostics, Trace log, Settings) with an engine status card showing whether `llama-server.exe` was found.
- The Playground has three columns: the model library (file size, parameter count and quantization read from the GGUF file name), the conversation, and a live insights column with throughput tiles, a per-model decode-speed chart, llama.cpp signals and quick controls.
- Responses render as model-coloured cards with copy buttons, collapsible reasoning and code blocks with their own copy action.
- Diagnostics shows a card per model (the fastest decode is highlighted), a side-by-side table and the latest response grouped by identity, timing, tokens, server and resources. The trace log can be filtered and copied as tab-separated text.
- Shortcuts: `Ctrl+K` opens the command palette, `1`-`5` switch views, `Enter` runs the prompt, `Shift+Enter` adds a line.

## Pipeline

The Pipeline view wires several models together on a canvas so the output of one model becomes the input of another.

- **Nodes:** a Prompt node (the run prompt), any number of Model nodes and Result nodes. Double-click the canvas to add a model, drag from a node's right dot to another node to draw a channel, right-click for more, `Del` removes the selection.
- **Shapes:** serial chains (A → B → C), parallel fan-out (A → B, C, D at the same time), merges ("wait for all inputs") and loops (A → B → A). Templates create a chain, a fan-out, a fan-out with a judge, a three-round debate or a self-refine loop from the models selected in the playground.
- **Model nodes:** model, role (system prompt), prompt template (`{input}`, `{original}`, `{from}`, `{round}`, `{node}`, `{step}`), trigger, conversation memory, output shaping (strip `<think>` blocks, first code block, JSON only, first/last paragraph, single line, character cap) and optional per-node sampling (temperature, max tokens, top-p, top-k, repeat penalty, seed) and server settings (context, GPU layers).
- **Channels:** a message template (`{output}`, `{original}`, `{from}`, `{round}`), a pass limit and an on/off switch. Channels that close a loop get a pass limit automatically; the run also stops at the step limit.
- **Result nodes:** collect every message, only the last one, or all joined; copy or save as Markdown.
- **Run settings:** step limit, how many models may generate in parallel (each gets its own `llama-server` on `first port + node id`), and whether models stay loaded between turns, which makes loops much faster at the cost of memory until the run ends.
- Pipelines can be saved to and opened from `.renaro-pipeline` files; the transcript can be copied as Markdown.
- Icons come from the Segoe MDL2 Assets / Segoe Fluent Icons fonts that ship with Windows; if neither is present the UI falls back to text labels.

Models run sequentially so a later model does not compete with an earlier model for RAM or VRAM. This produces more useful performance comparisons on typical research workstations.

The model library starts empty by design. No model names or placeholder entries are bundled into the playground.

## Headless batch mode

Any command-line option starts the playground without a window. It runs prompts through `llama-server` one at a time, in file order, and writes three files. `--help` lists every option.

```
renaro_model_playground.exe --headless --config slm.yaml --model qwen3-1.7b-q4_k_m.gguf ^
    --input problems.jsonl --out runs\thinking-off --thinking off
```

**Input.** `--input` takes a JSONL file. Each line is `{"id": ..., "prompt": "...", "system": "..."}` (only `prompt` is required; `--prompt-field` and `--id-field` rename the fields), a chat `{"id": ..., "messages": [{"role": ..., "content": ...}, ...]}` ending with a user message, or a bare JSON string. Blank lines are skipped, and an invalid line stops the run before anything starts, naming the line. `--prompt "<text>"` runs a single prompt instead.

**Settings.** `--config` reads a YAML file such as `slm.yaml`: `key: value` pairs, `#` comments, nested sections and `|`/`>` blocks. Nesting is ignored, except that `server.port`, `server.path` and `model.path` keep their meaning. Unknown keys are reported and skipped. Relative paths are resolved from the YAML file's folder. Command-line options override the file. Accepted keys, with llama.cpp/OpenAI/Hugging Face spellings: `temperature`/`temp`, `top_p`, `top_k`, `min_p`, `max_tokens`/`n_predict`/`max_new_tokens`, `seed`, `repeat_penalty`/`repetition_penalty`, `presence_penalty`, `frequency_penalty`, `enable_thinking`/`thinking`, `ctx`/`n_ctx`/`ctx_size`/`context_length`, `threads`, `threads_batch`, `gpu_layers`/`n_gpu_layers`/`ngl`, `batch_size`, `ubatch_size`, `flash_attn`, `mmap`, `mlock`, `kv_offload`, `cache_prompt`, `port`, `server`, `model`/`model_path`, `system`/`system_prompt`, `input`, `out`, `load_timeout`.

**Fixed server.** One `llama-server` runs for each model, for the whole run, with one slot (`-np 1`) and fixed threads, GPU layers, context and batch sizes. Every request carries every sampling value explicitly (temperature, top_p, top_k, min_p, max_tokens, seed and the penalties), so nothing depends on server defaults. The prompt cache is off unless `--cache-prompt` is given, because reusing it can change results. `--thinking on|off` sends `chat_template_kwargs.enable_thinking` and starts the server with `--jinja`. If the port already answers, the run refuses to start rather than talk to the wrong server. `--attach` uses a server you started yourself. The server is killed when the run ends, even after Ctrl+C or a crash.

**`results.jsonl`.** One line per call, with keys `answer`, `error`, `finish_reason`, `id`, `index`, `model`, `ok`, `reasoning`, `truncated`. Keys are sorted, with no spaces, `\n` line endings, and no timestamps or timings. The format is exactly Python's `json.dumps(row, sort_keys=True, ensure_ascii=False, separators=(",", ":"))`, so two runs with the same settings can be compared byte for byte (`fc /b a\results.jsonl b\results.jsonl`). The answer and the reasoning are kept apart: `reasoning_content` from the server, or a `<think>...</think>` block split off the answer. `truncated` is true when `finish_reason` is `length`. A failed call has `ok: false`, the message in `error`, `finish_reason: "error"`, and the run continues.

**`perf.json`.** Per call, from the server's own counters: `prompt_tokens`, `output_tokens`, `cached_tokens`, `reasoning_tokens`, `prompt_ms`, `decode_ms`, `prompt_tps`, `decode_tps`, `request_seconds`, `server_load_seconds`, `finish_reason`. It also records `peak_memory_gb` (the peak working set of `llama-server` so far) and `peak_commit_gb` (peak private bytes). Totals are given per model and overall: token sums, aggregate and mean tok/s, error and cut-off counts, and the run's peak memory. GPU memory is not included. The file is rewritten after every call.

**`manifest.json`.** Records every request and server setting, the exact `llama-server` command line for each model, the model file sizes, and the input file's FNV-1a hash. It also has what the server reported (`build_info`, `total_slots`, `n_ctx`, a chat template hash), the playground's own command line, and the run status. `--dry-run` checks everything and writes only the manifest.

Exit codes: `0` every call succeeded, `1` some calls failed, `2` bad options or input, `3` the run could not start or write its output, `130` interrupted.

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

After the build, the workflow runs `tests/headless_smoke_test.py`. It drives headless mode end to end against `tests/mock_llama`, a small Python stand-in for `llama-server`, so no model is needed. It checks options, the YAML config, two runs with byte-identical results, the perf and manifest files, a failed call with a server restart, and `--attach`.
