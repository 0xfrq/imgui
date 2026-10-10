"""Smoke test for the playground's headless mode. The GitHub workflow runs it after the build.

    python headless_smoke_test.py <path to renaro_model_playground.exe>

llama-server is replaced by mock_llama/gguf.py (see its docstring), so no model is needed. The
test covers: --help, option errors, --dry-run, a YAML config with command-line overrides, JSONL
prompts (plain, chat messages, ids of different types), two identical runs whose results must
match byte for byte, results/perf/manifest contents, a failing call followed by a server
restart, and --attach.
"""

import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
RESULT_KEYS = {"answer", "error", "finish_reason", "id", "index", "model", "ok", "reasoning", "truncated"}
CALL_KEYS = {"cached_tokens", "decode_ms", "decode_tps", "finish_reason", "id", "index", "model", "ok",
             "output_tokens", "peak_commit_gb", "peak_memory_gb", "prompt_ms", "prompt_tokens", "prompt_tps",
             "reasoning_tokens", "request_seconds", "server_load_seconds", "timings_source", "truncated"}


def fail(message):
    print("FAIL: " + message, flush=True)
    sys.exit(1)


def check(condition, message):
    if not condition:
        fail(message)


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def run(exe, args, cwd, env, expect):
    print("$ renaro_model_playground.exe " + " ".join(args), flush=True)
    proc = subprocess.run([exe] + args, cwd=cwd, env=env, capture_output=True, timeout=300)
    stdout = proc.stdout.decode("utf-8", "replace")
    stderr = proc.stderr.decode("utf-8", "replace")
    if stdout:
        print(stdout, flush=True)
    if stderr:
        print("[stderr]\n" + stderr, flush=True)
    check(proc.returncode == expect, "exit code {} (expected {})".format(proc.returncode, expect))
    return stdout, stderr


def canonical(value):
    return json.dumps(value, sort_keys=True, ensure_ascii=False, separators=(",", ":"))


def read_results(path):
    with open(path, "rb") as handle:
        data = handle.read()
    check(b"\r" not in data, "results.jsonl must use \\n line endings")
    check(data.endswith(b"\n"), "results.jsonl must end with a newline")
    rows = []
    for line in data.decode("utf-8").split("\n")[:-1]:
        row = json.loads(line)
        check(canonical(row) == line, "line is not sorted-key canonical JSON: " + line[:160])
        check(set(row) == RESULT_KEYS, "unexpected result keys: {}".format(sorted(row)))
        check(row["truncated"] == (row["finish_reason"] == "length"), "truncated must match finish_reason length")
        rows.append(row)
    return rows, data


def read_json(path):
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def read_log(path):
    if not os.path.exists(path):
        return []
    with open(path, "r", encoding="utf-8") as handle:
        return [json.loads(line) for line in handle if line.strip()]


def write_jsonl(path, rows):
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        for row in rows:
            handle.write(json.dumps(row, ensure_ascii=False) + "\n")


def check_perf(perf, rows, memory_known):
    calls = perf["calls"]
    check(len(calls) == len(rows), "perf.json must have one call per result line")
    for call, row in zip(calls, rows):
        check(set(call) == CALL_KEYS, "unexpected perf call keys: {}".format(sorted(call)))
        check(call["index"] == row["index"] and call["id"] == row["id"], "perf calls must follow the results order")
        check(call["finish_reason"] == row["finish_reason"], "perf finish_reason must match the results")
        if row["ok"]:
            check(call["prompt_tokens"] > 0 and call["prompt_tps"] > 0, "prompt tokens and prompt tok/s must be reported")
            if row["answer"] or row["reasoning"]:
                check(call["output_tokens"] > 0 and call["decode_tps"] > 0, "output tokens and decode tok/s must be reported")
            if memory_known:
                check(call["peak_memory_gb"] is not None and call["peak_memory_gb"] > 0, "peak memory must be reported")
            else:
                check(call["peak_memory_gb"] is None, "attached runs cannot measure memory")
    totals = perf["totals"]
    ok_calls = [call for call in calls if call["ok"]]
    check(totals["calls"] == len(calls), "totals.calls")
    check(totals["ok"] == len(ok_calls), "totals.ok")
    check(totals["errors"] == len(calls) - len(ok_calls), "totals.errors")
    check(totals["cutoffs"] == sum(1 for call in ok_calls if call["truncated"]), "totals.cutoffs must count finish_reason length")
    check(totals["output_tokens"] == sum(call["output_tokens"] for call in ok_calls), "totals.output_tokens")
    check(totals["prompt_tokens"] == sum(call["prompt_tokens"] for call in ok_calls), "totals.prompt_tokens")
    if memory_known and ok_calls:
        check(abs(totals["peak_memory_gb"] - max(call["peak_memory_gb"] for call in ok_calls)) < 1e-9, "totals.peak_memory_gb is the run peak")
    check(len(perf["models"]) >= 1, "perf.json needs per-model totals")


def main():
    if len(sys.argv) != 2:
        fail("usage: headless_smoke_test.py <renaro_model_playground.exe>")
    exe = os.path.abspath(sys.argv[1])
    check(os.path.isfile(exe), "executable not found: " + exe)
    python = sys.executable
    work = tempfile.mkdtemp(prefix="renaro-headless-")
    print("work directory: " + work, flush=True)
    shutil.copytree(os.path.join(HERE, "mock_llama"), os.path.join(work, "mock_llama"))
    open(os.path.join(work, "mock_llama.gguf"), "wb").close()
    env = dict(os.environ, PYTHONPATH=work)
    env.pop("PYTHONSAFEPATH", None)

    write_jsonl(os.path.join(work, "prompts.jsonl"), [
        {"id": "p1", "prompt": "What is 2 + 2?"},
        {"id": 2, "prompt": "Explain gravity. LONG"},
        {"id": "p3", "messages": [
            {"role": "system", "content": "Be brief."},
            {"role": "user", "content": "Hi"},
            {"role": "assistant", "content": "Hello"},
            {"role": "user", "content": "Name a colour. INLINE_THINK"}]},
        {"id": "p4", "prompt": "Say nothing. EMPTY"},
        {"id": {"set": "b", "n": 5}, "prompt": "Unicode check: café ☕ \U0001F600", "system": "Reply in English."},
    ])
    with open(os.path.join(work, "prompts.jsonl"), "a", encoding="utf-8", newline="\n") as handle:
        handle.write("\n")  # trailing blank line must be skipped
    with open(os.path.join(work, "slm.yaml"), "w", encoding="utf-8", newline="\n") as handle:
        handle.write(
            "# settings for the small model\n"
            "model:\n"
            "  name: mock\n"
            "sampling:\n"
            "  temperature: 0.2   # low\n"
            "  top_p: 0.9\n"
            "  top_k: 20\n"
            "  min_p: 0.0\n"
            "  max_tokens: 48\n"
            "  seed: 7\n"
            "enable_thinking: true\n"
            "server:\n"
            "  ctx: 4096\n"
            "  threads: 2\n"
            "unknown_setting: 1\n")

    # Help and option errors.
    stdout, _ = run(exe, ["--help"], work, env, 0)
    check("--input" in stdout and "--thinking" in stdout, "--help must describe the options")
    run(exe, ["--headless", "--no-such-option", "1"], work, env, 2)
    run(exe, ["--headless", "--model", "mock_llama.gguf", "--server", python], work, env, 2)
    run(exe, ["--headless", "--model", "mock_llama.gguf", "--server", python, "--prompt", "x", "--top-p", "1.5"], work, env, 2)
    with open(os.path.join(work, "bad.jsonl"), "w", encoding="utf-8") as handle:
        handle.write('{"id": 1, "prompt": "ok"}\n{"id": 2, "prompt": \n')
    _, stderr = run(exe, ["--headless", "--model", "mock_llama.gguf", "--server", python, "--input", "bad.jsonl", "--out", "bad"], work, env, 2)
    check("line 2" in stderr, "invalid JSONL must name the line")

    def base_args(port, out):
        return ["--headless", "--config", "slm.yaml", "--model", "mock_llama.gguf", "--server", python,
                "--input", "prompts.jsonl", "--port", str(port), "--load-timeout", "60", "--out", out]

    # Dry run: manifest only.
    run(exe, base_args(free_port(), "dry") + ["--dry-run"], work, env, 0)
    manifest = read_json(os.path.join(work, "dry", "manifest.json"))
    check(manifest["dry_run"] is True and manifest["status"]["state"] == "dry-run", "dry-run manifest")
    check(not os.path.exists(os.path.join(work, "dry", "results.jsonl")), "dry run must not write results")
    check(manifest["status"]["calls_planned"] == 5, "dry run must count the prompts")

    # Two identical runs.
    outputs = []
    for name in ("run_a", "run_b"):
        port = free_port()
        log = os.path.join(work, name + "-server.log")
        run(exe, base_args(port, name), work, dict(env, MOCK_LLAMA_LOG=log), 0)
        rows, data = read_results(os.path.join(work, name, "results.jsonl"))
        starts = read_log(log)
        check(len(starts) == 1, "llama-server must be started once per run and reused for every prompt, got {}".format(len(starts)))
        outputs.append((name, port, rows, data, starts[0]["argv"]))

    check(outputs[0][3] == outputs[1][3], "results.jsonl differs between two runs with the same settings")
    rows = outputs[0][2]
    check([row["index"] for row in rows] == [0, 1, 2, 3, 4], "results must follow the input order")
    check([row["id"] for row in rows] == ["p1", 2, "p3", "p4", {"n": 5, "set": "b"}], "ids must be echoed")
    check(all(row["ok"] and row["error"] is None and row["model"] == "mock_llama" for row in rows), "every call should succeed")

    first = rows[0]
    for expected in ("seed=7", "temp=0.2", "top_p=0.9", "top_k=20", "min_p=0", "max_tokens=48", "thinking=True", "turns=1"):
        check(expected in first["answer"], "the request must carry " + expected + ": " + first["answer"][:200])
    check(first["reasoning"].startswith("Thinking about"), "reasoning must be kept apart from the answer")
    check("Thinking" not in first["answer"] and "<think>" not in first["answer"], "the answer must not contain reasoning")
    check("café" in first["answer"] and "\U0001F600" in first["answer"], "non-ASCII text must survive")
    check('quote="x"' in first["answer"] and "back\\slash" in first["answer"] and "line\nbreak" in first["answer"], "escaping")
    check(first["finish_reason"] == "stop" and not first["truncated"], "short answers finish with stop")
    check(rows[1]["finish_reason"] == "length" and rows[1]["truncated"], "a cut-off answer must be marked truncated")
    check(rows[2]["reasoning"] == "inline reasoning", "inline <think> blocks must be split off: " + repr(rows[2]["reasoning"]))
    check(rows[2]["answer"].startswith("[seed=7") and "system=yes" in rows[2]["answer"] and "turns=4" in rows[2]["answer"], "chat messages")
    check(rows[3]["answer"] == "" and rows[3]["reasoning"] == "" and rows[3]["ok"], "an empty answer is a valid result")
    check("system=yes" in rows[4]["answer"], "the per-prompt system message must be sent")

    perf = read_json(os.path.join(work, "run_a", "perf.json"))
    check_perf(perf, rows, True)
    check(perf["totals"]["cutoffs"] == 1, "one cut-off expected")

    manifest = read_json(os.path.join(work, "run_a", "manifest.json"))
    check(manifest["status"]["state"] == "complete" and manifest["status"]["calls_done"] == 5, "manifest status")
    check(manifest["server"]["parallel"] == 1 and manifest["server"]["threads"] == 2 and manifest["server"]["ctx"] == 4096, "manifest server settings")
    check(manifest["request"]["seed"] == 7 and manifest["request"]["top_k"] == 20, "manifest request settings")
    check(manifest["request"]["chat_template_kwargs"] == {"enable_thinking": True}, "manifest thinking setting")
    command = manifest["models"][0]["server_command"]
    for expected in (" -np 1", " --jinja", " -c 4096", " -t 2", " -tb 2", " -ngl 0", " --seed 7"):
        check(expected in command, "server command must contain '{}': {}".format(expected.strip(), command))
    argv = outputs[0][4]
    check(" ".join(argv) in command, "the manifest command must be the one the server received")
    observed = manifest["models"][0]["observed"]
    check(observed["total_slots"] == 1 and observed["build_info"] == "mock-b1" and observed["n_ctx"] == 4096, "observed server properties")

    # A failing call: the row records the error, the server restarts, the run goes on.
    write_jsonl(os.path.join(work, "errors.jsonl"), [
        {"id": "e1", "prompt": "first"},
        {"id": "e2", "prompt": "please FAIL_HTTP"},
        {"id": "e3", "prompt": "third"},
    ])
    log = os.path.join(work, "errors-server.log")
    args = base_args(free_port(), "errors")
    args[args.index("prompts.jsonl")] = "errors.jsonl"
    run(exe, args, work, dict(env, MOCK_LLAMA_LOG=log), 1)
    error_rows, _ = read_results(os.path.join(work, "errors", "results.jsonl"))
    check([row["ok"] for row in error_rows] == [True, False, True], "only the failing call is an error")
    check(error_rows[1]["finish_reason"] == "error" and "500" in error_rows[1]["error"] and error_rows[1]["answer"] == "", "error row")
    check(len(read_log(log)) == 2, "the server must be restarted after a failed call")
    check_perf(read_json(os.path.join(work, "errors", "perf.json")), error_rows, True)

    # Attach to a server that is already running; command-line options override the config.
    port = free_port()
    server = subprocess.Popen([python, "-m", "mock_llama.gguf", "--port", str(port), "-np", "1", "-c", "2048"], cwd=work, env=env)
    try:
        run(exe, ["--headless", "--config", "slm.yaml", "--attach", "--port", str(port), "--prompt", "What is 2 + 2?",
                  "--seed", "11", "--thinking", "off", "--out", "attach", "--quiet"], work, env, 0)
    finally:
        server.kill()
        server.wait()
    attach_rows, _ = read_results(os.path.join(work, "attach", "results.jsonl"))
    check(len(attach_rows) == 1 and attach_rows[0]["ok"] and attach_rows[0]["id"] is None, "attach result")
    check("seed=11" in attach_rows[0]["answer"] and "thinking=False" in attach_rows[0]["answer"], "command-line options must override the config")
    check(attach_rows[0]["reasoning"] == "", "thinking off must not return reasoning")
    check_perf(read_json(os.path.join(work, "attach", "perf.json")), attach_rows, False)
    attach_manifest = read_json(os.path.join(work, "attach", "manifest.json"))
    check(attach_manifest["server"]["attach"] is True and attach_manifest["models"][0]["server_command"] is None, "attach manifest")

    shutil.rmtree(work, ignore_errors=True)
    print("headless smoke test passed", flush=True)


if __name__ == "__main__":
    main()
