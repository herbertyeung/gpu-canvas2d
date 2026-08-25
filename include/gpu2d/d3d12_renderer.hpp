#pragma once

#include "gpu2d/render_types.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct IDXGIAdapter1;
struct ID3D12CommandQueue;
struct ID3D12Device;
struct ID3D12Resource;

namespace gpu2d {

	struct RendererOptions {
		bool vsync{ true };
		bool enable_debug_layer{};
	};

	struct TextureUpload {
		std::uint32_t width{};
		std::uint32_t height{};
		std::vector<std::byte> rgba8;
	};

	class D3D12Renderer {
	public:
		D3D12Renderer(HWND window, std::uint32_t width, std::uint32_t height,
			RendererOptions options = {});
		~D3D12Renderer();

		D3D12Renderer(const D3D12Renderer&) = delete;
		D3D12Renderer& operator=(const D3D12Renderer&) = delete;
		D3D12Renderer(D3D12Renderer&&) = delete;
		D3D12Renderer& operator=(D3D12Renderer&&) = delete;

		[[nodiscard]] TextureHandle createTexture(const TextureUpload& upload);
		[[nodiscard]] TextureHandle registerExternalTexture(ID3D12Resource* resource,
			std::uint32_t width,
			std::uint32_t height,
			std::uint32_t resource_state);
		void updateTexture(TextureHandle handle, const TextureUpload& upload);
		void setExternalTextureState(TextureHandle handle, std::uint32_t resource_state);
		void releaseTexture(TextureHandle handle);
		[[nodiscard]] FrameTelemetry render(const RenderPlan& plan);
		void resize(std::uint32_t width, std::uint32_t height);
		void waitIdle();

		[[nodiscard]] std::string adapterName() const;
		[[nodiscard]] IDXGIAdapter1* nativeAdapter() const noexcept;
		[[nodiscard]] ID3D12Device* nativeDevice() const noexcept;
		[[nodiscard]] ID3D12CommandQueue* nativeQueue() const noexcept;
		[[nodiscard]] std::size_t deferredReleaseCount() const noexcept;

	private:
		struct Impl;
		std::unique_ptr<Impl> impl_;
	};

}  // namespace gpu2d
