#pragma once

#include "gpu2d/canvas_frame.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace gpu2d {

	struct JavaScriptPageStats {
		double initialization_ms{};
		double frame_ms{};
		std::uint32_t raf_callbacks{};
	};

	class JavaScriptPage {
	public:
		[[nodiscard]] static std::unique_ptr<JavaScriptPage> create(const std::string& html,
			std::uint64_t page_revision);
		~JavaScriptPage();

		JavaScriptPage(const JavaScriptPage&) = delete;
		JavaScriptPage& operator=(const JavaScriptPage&) = delete;

		[[nodiscard]] CanvasFrame runAnimationFrame(double timestamp_ms);
		void resizeViewport(std::uint32_t width, std::uint32_t height);
		void pointerMove(float x, float y);
		void requestSurfaceReset() noexcept;
		[[nodiscard]] bool faulted() const noexcept;

		[[nodiscard]] std::uint32_t canvasWidth() const noexcept;
		[[nodiscard]] std::uint32_t canvasHeight() const noexcept;
		[[nodiscard]] std::uint64_t revision() const noexcept;
		[[nodiscard]] const JavaScriptPageStats& stats() const noexcept;

	private:
		struct Impl;
		explicit JavaScriptPage(std::unique_ptr<Impl> impl);
		std::unique_ptr<Impl> impl_;
	};

}  // namespace gpu2d