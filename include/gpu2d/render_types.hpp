#pragma once

#include "gpu2d/math.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace gpu2d {

	struct TextureHandle {
		std::uint32_t index{ std::numeric_limits<std::uint32_t>::max() };
		std::uint32_t generation{};

		[[nodiscard]] constexpr bool valid() const noexcept {
			return index != std::numeric_limits<std::uint32_t>::max();
		}
		friend constexpr bool operator==(TextureHandle, TextureHandle) noexcept = default;
	};

	enum class PipelineKind : std::uint8_t {
		Solid,
		Textured,
	};

	struct ScissorRect {
		std::int32_t left{};
		std::int32_t top{};
		std::int32_t right{};
		std::int32_t bottom{};

		friend constexpr bool operator==(ScissorRect, ScissorRect) noexcept = default;
	};

	// This structure is consumed directly as D3D12 per-instance vertex input.
	struct InstanceData {
		std::array<float, 4> local_rect{};
		std::array<float, 4> transform_linear{};
		std::array<float, 4> transform_translation{};
		std::array<float, 4> color{};
		std::array<float, 4> uv_rect{};
	};

	static_assert(sizeof(InstanceData) == 80);

	struct RenderBatch {
		PipelineKind pipeline{ PipelineKind::Solid };
		TextureHandle texture{};
		ScissorRect scissor{};
		std::uint32_t first_instance{};
		std::uint32_t instance_count{};
	};

	struct FrameTelemetry {
		std::uint32_t draw_ops{};
		std::uint32_t culled_ops{};
		std::uint32_t batches{};
		std::uint32_t instances{};
		std::size_t upload_bytes{};
		std::uint64_t submitted_fence{};
		std::uint64_t completed_fence{};
		std::size_t deferred_releases{};
		double cpu_frame_ms{};
	};

	struct RenderPlan {
		std::vector<InstanceData> instances;
		std::vector<RenderBatch> batches;
		std::vector<TextureHandle> referenced_textures;
		FrameTelemetry telemetry;
	};

}  // namespace gpu2d
