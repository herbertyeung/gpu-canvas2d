#include "gpu2d/frame_planner.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace gpu2d {
	namespace {

		[[nodiscard]] ScissorRect toScissor(const Rect clip, const Rect viewport) {
			const Rect clamped = intersect(clip, viewport);
			if (clamped.empty()) {
				return {};
			}

			const auto checkedInt = [](const float value, const bool round_down) -> std::int32_t {
				const double rounded = round_down ? std::floor(static_cast<double>(value))
					: std::ceil(static_cast<double>(value));
				if (rounded < static_cast<double>(std::numeric_limits<std::int32_t>::min()) ||
					rounded > static_cast<double>(std::numeric_limits<std::int32_t>::max())) {
					throw std::overflow_error("scissor coordinate does not fit int32");
				}
				return static_cast<std::int32_t>(rounded);
				};

			return ScissorRect{ checkedInt(clamped.left, true), checkedInt(clamped.top, true),
							   checkedInt(clamped.right, false), checkedInt(clamped.bottom, false) };
		}

		[[nodiscard]] bool sameBatch(const RenderBatch& batch, const DrawOp& operation,
			const ScissorRect scissor) noexcept {
			return batch.pipeline == operation.pipeline && batch.texture == operation.texture &&
				batch.scissor == scissor;
		}

		[[nodiscard]] InstanceData makeInstance(const DrawOp& operation) noexcept {
			return InstanceData{
				{operation.local_rect.left, operation.local_rect.top, operation.local_rect.width(),
				 operation.local_rect.height()},
				{operation.transform.m11, operation.transform.m12, operation.transform.m21,
				 operation.transform.m22},
				{operation.transform.dx, operation.transform.dy, 0.0F, 0.0F},
				{operation.color.r, operation.color.g, operation.color.b, operation.color.a},
				{operation.uv_rect.left, operation.uv_rect.top, operation.uv_rect.width(),
				 operation.uv_rect.height()},
			};
		}

	}  // namespace

	RenderPlan FramePlanner::build(const DisplayList& display_list, const Rect viewport) const {
		if (!isFinite(viewport) || viewport.empty()) {
			throw std::invalid_argument("FramePlanner requires a finite non-empty viewport");
		}

		RenderPlan plan;
		const auto operations = display_list.operations();
		if (operations.size() > std::numeric_limits<std::uint32_t>::max()) {
			throw std::overflow_error("display list is too large");
		}
		plan.telemetry.draw_ops = static_cast<std::uint32_t>(operations.size());
		plan.instances.reserve(operations.size());
		plan.batches.reserve(operations.size());

		for (const DrawOp& operation : operations) {
			const Rect bounds = transformedBounds(operation.transform, operation.local_rect);
			const Rect effective_clip = operation.device_clip ? intersect(*operation.device_clip, viewport)
				: viewport;
			if (effective_clip.empty() || intersect(bounds, effective_clip).empty()) {
				++plan.telemetry.culled_ops;
				continue;
			}

			const ScissorRect scissor = toScissor(effective_clip, viewport);
			if (scissor.right <= scissor.left || scissor.bottom <= scissor.top) {
				++plan.telemetry.culled_ops;
				continue;
			}

			if (plan.instances.size() >= std::numeric_limits<std::uint32_t>::max()) {
				throw std::overflow_error("instance count exceeds uint32");
			}
			const std::uint32_t instance_index = static_cast<std::uint32_t>(plan.instances.size());
			plan.instances.push_back(makeInstance(operation));

			if (!plan.batches.empty() && sameBatch(plan.batches.back(), operation, scissor)) {
				++plan.batches.back().instance_count;
			}
			else {
				plan.batches.push_back(
					RenderBatch{ operation.pipeline, operation.texture, scissor, instance_index, 1 });
			}

			if (operation.pipeline == PipelineKind::Textured) {
				const auto found = std::find(plan.referenced_textures.begin(), plan.referenced_textures.end(),
					operation.texture);
				if (found == plan.referenced_textures.end()) {
					plan.referenced_textures.push_back(operation.texture);
				}
			}
		}

		plan.telemetry.instances = static_cast<std::uint32_t>(plan.instances.size());
		plan.telemetry.batches = static_cast<std::uint32_t>(plan.batches.size());
		plan.telemetry.upload_bytes = plan.instances.size() * sizeof(InstanceData);
		return plan;
	}

}  // namespace gpu2d
