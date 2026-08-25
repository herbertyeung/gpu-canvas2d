#pragma once

#include "gpu2d/canvas_frame.hpp"
#include "gpu2d/display_list.hpp"
#include "gpu2d/renderer_plugin_api.h"

#include <optional>
#include <string>
#include <vector>

namespace gpu2d::plugin {

	inline thread_local std::string last_error;

	[[nodiscard]] inline CanvasFrame rebuildCanvasFrame(const Gpu2dRendererFrame& frame) {
		if (frame.canvas_width == 0U || frame.canvas_height == 0U || frame.canvas_width > 8192U ||
			frame.canvas_height > 8192U) {
			throw std::invalid_argument("renderer canvas dimensions must be in [1, 8192]");
		}
		if ((frame.operation_count != 0U && frame.operations == nullptr) ||
			(frame.path_segment_count != 0U && frame.path_segments == nullptr) ||
			(frame.gradient_stop_count != 0U && frame.gradient_stops == nullptr)) {
			throw std::invalid_argument("renderer Canvas frame arrays are null");
		}
		CanvasFrame result;
		result.page_revision = frame.page_revision;
		result.required_capabilities = frame.required_capabilities;
		result.reset_surface = frame.reset_surface != 0U;
		result.has_background_color = frame.has_background_color != 0U;
		result.background_color = Color{ frame.background_color[0], frame.background_color[1],
										frame.background_color[2], frame.background_color[3] };
		result.path_segments.reserve(frame.path_segment_count);
		for (std::uint32_t index = 0; index < frame.path_segment_count; ++index) {
			const Gpu2dCanvasPathSegment& source = frame.path_segments[index];
			if (source.verb > static_cast<std::uint32_t>(CanvasPathVerb::Close)) {
				throw std::invalid_argument("renderer Canvas path verb is invalid");
			}
			result.path_segments.push_back(CanvasPathSegment{
				static_cast<CanvasPathVerb>(source.verb), Vec2{source.x, source.y} });
		}
		result.gradient_stops.reserve(frame.gradient_stop_count);
		for (std::uint32_t index = 0; index < frame.gradient_stop_count; ++index) {
			const Gpu2dCanvasGradientStop& source = frame.gradient_stops[index];
			if (!std::isfinite(source.offset) || source.offset < 0.0F || source.offset > 1.0F) {
				throw std::invalid_argument("renderer Canvas gradient stop offset is invalid");
			}
			result.gradient_stops.push_back(CanvasGradientStop{
				source.offset, Color{source.color[0], source.color[1], source.color[2], source.color[3]} });
		}
		result.operations.reserve(frame.operation_count);
		for (std::uint32_t index = 0; index < frame.operation_count; ++index) {
			const Gpu2dCanvasOperation& source = frame.operations[index];
			if (source.kind > static_cast<std::uint32_t>(CanvasOperationKind::StrokePath) ||
				source.paint_kind > static_cast<std::uint32_t>(CanvasPaintKind::RadialGradient) ||
				source.composite_mode > static_cast<std::uint32_t>(CanvasCompositeMode::Lighter)) {
				throw std::invalid_argument("renderer Canvas operation enum is invalid");
			}
			const std::uint64_t path_end = static_cast<std::uint64_t>(source.first_path_segment) +
				source.path_segment_count;
			if (path_end > frame.path_segment_count) {
				throw std::invalid_argument("renderer Canvas operation path range is invalid");
			}
			const std::uint64_t gradient_end = static_cast<std::uint64_t>(source.first_gradient_stop) +
				source.gradient_stop_count;
			if (gradient_end > frame.gradient_stop_count) {
				throw std::invalid_argument("renderer Canvas operation gradient range is invalid");
			}
			CanvasFrameOperation operation;
			operation.kind = static_cast<CanvasOperationKind>(source.kind);
			operation.rectangle =
				Rect{ source.rectangle[0], source.rectangle[1], source.rectangle[2], source.rectangle[3] };
			operation.transform = Mat3x2{ source.transform[0], source.transform[1], source.transform[2],
										 source.transform[3], source.transform[4], source.transform[5] };
			operation.paint.color =
				Color{ source.color[0], source.color[1], source.color[2], source.color[3] };
			operation.paint.line_width = source.line_width;
			operation.paint.kind = static_cast<CanvasPaintKind>(source.paint_kind);
			std::copy(std::begin(source.radial_gradient), std::end(source.radial_gradient),
				std::begin(operation.paint.radial_gradient));
			operation.paint.first_gradient_stop = source.first_gradient_stop;
			operation.paint.gradient_stop_count = source.gradient_stop_count;
			operation.paint.shadow_color = Color{ source.shadow_color[0], source.shadow_color[1],
												 source.shadow_color[2], source.shadow_color[3] };
			operation.paint.shadow_blur = source.shadow_blur;
			operation.paint.shadow_offset_x = source.shadow_offset_x;
			operation.paint.shadow_offset_y = source.shadow_offset_y;
			operation.paint.composite = static_cast<CanvasCompositeMode>(source.composite_mode);
			operation.first_path_segment = source.first_path_segment;
			operation.path_segment_count = source.path_segment_count;
			result.operations.push_back(operation);
		}
		return result;
	}

	inline void copyTelemetry(const FrameTelemetry& source, Gpu2dRendererTelemetry& destination) {
		destination.draw_ops = source.draw_ops;
		destination.culled_ops = source.culled_ops;
		destination.batches = source.batches;
		destination.instances = source.instances;
		destination.upload_bytes = source.upload_bytes;
		destination.submitted_fence = source.submitted_fence;
		destination.completed_fence = source.completed_fence;
		destination.deferred_releases = source.deferred_releases;
		destination.cpu_frame_ms = source.cpu_frame_ms;
	}

	template <typename Function>
	[[nodiscard]] int32_t guarded(Function&& function) noexcept {
		try {
			function();
			last_error.clear();
			return 1;
		}
		catch (const std::exception& error) {
			last_error = error.what();
		}
		catch (...) {
			last_error = "unknown renderer plugin exception";
		}
		return 0;
	}

}  // namespace gpu2d::plugin