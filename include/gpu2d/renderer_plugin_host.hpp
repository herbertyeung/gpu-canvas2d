#pragma once

#include "gpu2d/canvas_frame.hpp"
#include "gpu2d/display_list.hpp"
#include "gpu2d/renderer_plugin_api.h"

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gpu2d {

	struct RendererPluginInfo {
		std::filesystem::path path;
		std::string id;
		std::string display_name;
		std::uint64_t capabilities{};
	};

	struct RendererPluginDiagnostic {
		std::filesystem::path path;
		std::string message;
	};

	class RendererPluginHost {
	public:
		RendererPluginHost() = default;
		~RendererPluginHost();

		RendererPluginHost(const RendererPluginHost&) = delete;
		RendererPluginHost& operator=(const RendererPluginHost&) = delete;

		[[nodiscard]] static std::vector<RendererPluginInfo> discover(
			const std::filesystem::path& directory,
			std::vector<RendererPluginDiagnostic>* diagnostics = nullptr);
		void load(const RendererPluginInfo& plugin, HWND window, std::uint32_t width,
			std::uint32_t height, bool vsync, bool debug_layer);
		void unload() noexcept;
		void resize(std::uint32_t width, std::uint32_t height);
		[[nodiscard]] Gpu2dRendererTelemetry render(const CanvasFrame& canvas_frame,
			std::uint32_t viewport_width,
			std::uint32_t viewport_height,
			std::uint32_t canvas_width,
			std::uint32_t canvas_height);

		[[nodiscard]] bool loaded() const noexcept { return instance_ != nullptr; }
		[[nodiscard]] std::string adapterName() const;
		[[nodiscard]] std::string rendererName() const;
		[[nodiscard]] std::uint64_t capabilities() const noexcept;

	private:
		[[nodiscard]] std::string pluginError(const char* operation) const;

		HMODULE module_{};
		const Gpu2dRendererApi* api_{};
		void* instance_{};
		std::string renderer_name_;
		std::vector<Gpu2dCanvasOperation> operations_;
		std::vector<Gpu2dCanvasPathSegment> path_segments_;
		std::vector<Gpu2dCanvasGradientStop> gradient_stops_;
	};

}  // namespace gpu2d