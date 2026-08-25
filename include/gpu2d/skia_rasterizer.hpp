#pragma once

#include "gpu2d/canvas_frame.hpp"

#include <cstdint>
#include <memory>

struct IDXGIAdapter1;
struct ID3D12CommandQueue;
struct ID3D12Device;
struct ID3D12Resource;

namespace gpu2d {

	struct SkiaGpuSurfaceInfo {
		ID3D12Resource* resource{};
		std::uint32_t width{};
		std::uint32_t height{};
		std::uint32_t resource_state{};
		bool recreated{};
	};

	class SkiaGpuCanvasSurface {
	public:
		SkiaGpuCanvasSurface(IDXGIAdapter1* adapter, ID3D12Device* device,
			ID3D12CommandQueue* queue);
		~SkiaGpuCanvasSurface();
		SkiaGpuCanvasSurface(const SkiaGpuCanvasSurface&) = delete;
		SkiaGpuCanvasSurface& operator=(const SkiaGpuCanvasSurface&) = delete;

		[[nodiscard]] SkiaGpuSurfaceInfo apply(const CanvasFrame& frame,
			std::uint32_t width,
			std::uint32_t height);
		void notifyExternalState(std::uint32_t resource_state);
		void waitIdle();

	private:
		struct Impl;
		std::unique_ptr<Impl> impl_;
	};

}  // namespace gpu2d