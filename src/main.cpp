#include "gpu2d/win32_application.hpp"

#include <windows.h>

#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

	[[nodiscard]] std::uint64_t parsePositiveInteger(const std::wstring_view text) {
		std::size_t consumed = 0;
		const std::wstring owned(text);
		const unsigned long long value = std::stoull(owned, &consumed, 10);
		if (consumed != owned.size() || value == 0) {
			throw std::invalid_argument("expected a positive integer");
		}
		return static_cast<std::uint64_t>(value);
	}

	void printUsage() {
		std::cout << "Usage: gpu_2d_demo [--frames N] [--reload-at N] [--no-vsync] "
			"[--debug-layer] [--html-canvas PATH] [--live-edit-self-test] "
			"[--renderer-self-test] [--example-self-test] [--help]\n"
			"  --frames N   Exit after N rendered frames\n"
			"  --reload-at N  Reload the active renderer after frame N\n"
			"  --no-vsync   Present without the vsync interval\n"
			"  --debug-layer  Enable the optional D3D12 SDK validation layer\n"
			"  --html-canvas PATH  Load a complete HTML/JavaScript Canvas page\n"
			"  --live-edit-self-test  Verify RichEdit EN_CHANGE live reparsing\n"
			"  --renderer-self-test  Switch between dynamically loaded renderers\n"
			"  --example-self-test  Switch the built-in Example dropdown and verify reload\n"
			"  --help       Show this message\n";
	}

}  // namespace

int wmain(const int argc, wchar_t** argv) {
	try {
		gpu2d::ApplicationOptions options;
		for (int index = 1; index < argc; ++index) {
			const std::wstring_view argument(argv[index]);
			if (argument == L"--help") {
				printUsage();
				return 0;
			}
			if (argument == L"--no-vsync") {
				options.vsync = false;
				continue;
			}
			if (argument == L"--debug-layer") {
				options.debug_layer = true;
				continue;
			}
			if (argument == L"--live-edit-self-test") {
				options.live_edit_self_test = true;
				continue;
			}
			if (argument == L"--renderer-self-test") {
				options.renderer_self_test = true;
				continue;
			}
			if (argument == L"--example-self-test") {
				options.example_self_test = true;
				continue;
			}
			if (argument == L"--html-canvas") {
				if (index + 1 >= argc) {
					throw std::invalid_argument("--html-canvas requires a path");
				}
				options.html_canvas_path = std::filesystem::path(argv[++index]);
				continue;
			}
			if (argument == L"--frames") {
				if (index + 1 >= argc) {
					throw std::invalid_argument("--frames requires a value");
				}
				options.max_frames = parsePositiveInteger(argv[++index]);
				continue;
			}
			if (argument == L"--reload-at") {
				if (index + 1 >= argc) {
					throw std::invalid_argument("--reload-at requires a value");
				}
				options.reload_at_frame = parsePositiveInteger(argv[++index]);
				continue;
			}
			throw std::invalid_argument("unknown command-line option");
		}
		return gpu2d::runApplication(options);
	}
	catch (const std::exception& error) {
		std::cerr << "ERROR: " << error.what() << '\n';
		return 1;
	}
}
