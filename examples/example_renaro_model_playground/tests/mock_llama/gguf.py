"""Stand-in for llama-server, used by headless_smoke_test.py.

The playground launches llama-server as `<server> -m <model> -c ... --port N ...`. The smoke
test passes python.exe as the server and `mock_llama.gguf` as the model, which turns that
command into `python -m mock_llama.gguf -c ... --port N ...`: this module runs with the same
arguments llama-server would get.

It serves /health (503 while "loading"), /props, /slots, /metrics and a streaming
/v1/chat/completions whose output is a deterministic function of the request, so two runs with
the same settings must produce identical results. Markers in the prompt pick special cases:
  LONG          more tokens than max_tokens, so the answer is cut off (finish_reason "length")
  INLINE_THINK  reasoning returned inside the content as <think>...</think>
  EMPTY         no content and no reasoning
  FAIL_HTTP     HTTP 500
"""

import hashlib
import json
import os
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

WORDS = ["alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "golf", "hotel",
         "india", "juliet", "kilo", "lima", "mike", "november", "oscar", "papa"]


def parse_args(argv):
    args = {"port": 8080, "ctx": 0, "slots": 4, "jinja": False, "load_delay": 0.4}
    index = 0
    while index < len(argv):
        name = argv[index]
        value = argv[index + 1] if index + 1 < len(argv) else ""
        if name == "--port":
            args["port"] = int(value)
            index += 1
        elif name == "-c":
            args["ctx"] = int(value)
            index += 1
        elif name == "-np":
            args["slots"] = int(value)
            index += 1
        elif name == "--load-delay":
            args["load_delay"] = float(value)
            index += 1
        elif name == "--jinja":
            args["jinja"] = True
        index += 1
    return args


ARGS = parse_args(sys.argv[1:])
READY_AT = time.time() + ARGS["load_delay"]
TOTALS = {"prompt": 0, "predicted": 0, "requests": 0}


def log_start():
    path = os.environ.get("MOCK_LLAMA_LOG")
    if path:
        with open(path, "a", encoding="utf-8") as log:
            log.write(json.dumps({"pid": os.getpid(), "argv": sys.argv[1:]}) + "\n")


def plan(body):
    """Returns (reasoning pieces, content pieces, prompt token count)."""
    messages = body["messages"]
    prompt = messages[-1]["content"]
    system = next((message["content"] for message in messages if message["role"] == "system"), "")
    thinking = (body.get("chat_template_kwargs") or {}).get("enable_thinking")
    params = "[seed={} temp={:g} top_p={:g} top_k={} min_p={:g} max_tokens={} thinking={} system={} turns={}]".format(
        body.get("seed"), body.get("temperature", -1), body.get("top_p", -1), body.get("top_k"),
        body.get("min_p", -1), body.get("max_tokens"), thinking, "yes" if system else "no", len(messages))
    digest = hashlib.sha256(json.dumps(body, sort_keys=True).encode("utf-8")).hexdigest()
    words = [WORDS[int(digest[i:i + 2], 16) % len(WORDS)] for i in range(0, 40, 2)]
    prompt_tokens = len(" ".join(message["content"] for message in messages).split()) + 5

    if "EMPTY" in prompt:
        return [], [], prompt_tokens
    content = [params] + words + ['quote="x"', "back\\slash", "café", "\U0001F600", "line\nbreak"]
    if "LONG" in prompt:
        content += [WORDS[(i * 7) % len(WORDS)] for i in range(200)]
    if "INLINE_THINK" in prompt:
        return [], ["<think>", "inline reasoning", "</think>"] + content, prompt_tokens
    reasoning = ["Thinking", "about"] + words[:3] if thinking is True else []
    return reasoning, content, prompt_tokens


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, *args):
        pass

    def send_json(self, status, value):
        body = json.dumps(value).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/health":
            if time.time() < READY_AT:
                self.send_json(503, {"error": {"code": 503, "message": "Loading model", "type": "unavailable_error"}})
            else:
                self.send_json(200, {"status": "ok"})
        elif self.path == "/props":
            self.send_json(200, {
                "build_info": "mock-b1",
                "model_path": "mock_llama.gguf",
                "total_slots": ARGS["slots"],
                "chat_template": "{{ mock template }}",
                "default_generation_settings": {"n_ctx": ARGS["ctx"]},
            })
        elif self.path == "/slots":
            self.send_json(200, [{"id": 0, "n_ctx": ARGS["ctx"], "is_processing": False}])
        elif self.path == "/metrics":
            text = ("llamacpp:prompt_tokens_total {}\nllamacpp:tokens_predicted_total {}\n"
                    "llamacpp:requests_processing 0\nllamacpp:requests_deferred 0\n").format(TOTALS["prompt"], TOTALS["predicted"])
            body = text.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; version=0.0.4")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_json(404, {"error": {"code": 404, "message": "File Not Found", "type": "not_found_error"}})

    def do_POST(self):
        if self.path != "/v1/chat/completions":
            self.send_json(404, {"error": {"code": 404, "message": "File Not Found", "type": "not_found_error"}})
            return
        length = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(length).decode("utf-8"))
        if "FAIL_HTTP" in body["messages"][-1]["content"]:
            self.send_json(500, {"error": {"code": 500, "message": "mock failure", "type": "server_error"}})
            return

        reasoning, content, prompt_tokens = plan(body)
        limit = body.get("max_tokens", -1)
        pieces = [("reasoning_content", piece) for piece in reasoning] + [("content", piece) for piece in content]
        finish_reason = "stop"
        if limit is not None and limit > 0 and len(pieces) > limit:
            pieces = pieces[:limit]
            finish_reason = "length"
        TOTALS["prompt"] += prompt_tokens
        TOTALS["predicted"] += len(pieces)
        TOTALS["requests"] += 1

        def chunk(delta, finish=None, extra=None):
            value = {"choices": [{"finish_reason": finish, "index": 0, "delta": delta}],
                     "created": 1700000000, "id": "chatcmpl-mock", "model": "mock",
                     "system_fingerprint": "b1-mock", "object": "chat.completion.chunk"}
            if extra:
                value.update(extra)
            return ("data: " + json.dumps(value) + "\n\n").encode("utf-8")

        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(chunk({"role": "assistant", "content": None}))
        for field, piece in pieces:
            self.wfile.write(chunk({field: piece + " "}))
            self.wfile.flush()
        produced = len(pieces)
        timings = {"cache_n": 0, "prompt_n": prompt_tokens, "prompt_ms": prompt_tokens * 0.5,
                   "prompt_per_token_ms": 0.5, "prompt_per_second": 2000.0,
                   "predicted_n": produced, "predicted_ms": produced * 2.0,
                   "predicted_per_token_ms": 2.0 if produced else 0.0,
                   "predicted_per_second": 500.0 if produced else 0.0}
        usage = {"completion_tokens": produced, "prompt_tokens": prompt_tokens, "total_tokens": prompt_tokens + produced}
        self.wfile.write(chunk({}, finish_reason, {"usage": usage, "timings": timings}))
        self.wfile.write(b"data: [DONE]\n\n")
        self.wfile.flush()


def main():
    log_start()
    server = ThreadingHTTPServer(("127.0.0.1", ARGS["port"]), Handler)
    server.daemon_threads = True
    server.serve_forever()


if __name__ == "__main__":
    main()
