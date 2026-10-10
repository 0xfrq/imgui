#pragma once

#include <string>

// True when the command line asks for headless mode: any argument that starts with '-'.
bool HeadlessRequested(int argc, wchar_t** argv);

// Runs prompts through llama-server without a window and returns the process exit code:
// 0 every call succeeded, 1 at least one call failed, 2 invalid options or input,
// 3 the run could not start or write its output, 130 interrupted.
int RunHeadless(int argc, wchar_t** argv, const std::wstring& detected_server_path);
