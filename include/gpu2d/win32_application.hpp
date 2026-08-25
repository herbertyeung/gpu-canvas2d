#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

namespace gpu2d {

	struct ApplicationOptions {
		std::uint32_t width{ 1280 };
		std::uint32_t height{ 720 };
		std::uint64_t max_frames{};
		std::uint64_t reload_at_frame{};
		bool vsync{ true };
		bool debug_layer{};
		bool live_edit_self_test{};
		bool renderer_self_test{};
		bool example_self_test{};
		std::optional<std::filesystem::path> html_canvas_path;
	};

	[[nodiscard]] int runApplication(const ApplicationOptions& options);

}  // namespace gpu2d
