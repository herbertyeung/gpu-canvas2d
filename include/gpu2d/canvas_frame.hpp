#pragma once

#include "gpu2d/math.hpp"

#include <cstdint>
#include <vector>

namespace gpu2d {

	enum class CanvasOperationKind : std::uint32_t {
		ClearRect,
		FillRect,
		StrokeRect,
		FillPath,
		StrokePath,
	};

	enum class CanvasPathVerb : std::uint32_t {
		MoveTo,
		LineTo,
		Close,
	};

	enum class CanvasCompositeMode : std::uint32_t {
		SourceOver,
		Lighter,
	};

	enum class CanvasPaintKind : std::uint32_t {
		Solid,
		RadialGradient,
	};

	enum CanvasCapability : std::uint64_t {
		CanvasCapabilityRectangles = 1ULL << 0U,
		CanvasCapabilityPaths = 1ULL << 1U,
		CanvasCapabilityArcs = 1ULL << 2U,
		CanvasCapabilityShadows = 1ULL << 3U,
		CanvasCapabilityLighterBlend = 1ULL << 4U,
		CanvasCapabilityPersistent = 1ULL << 5U,
		CanvasCapabilityGradients = 1ULL << 6U,
	};

	struct CanvasGradientStop {
		float offset{};
		Color color{};
	};

	struct CanvasPathSegment {
		CanvasPathVerb verb{ CanvasPathVerb::MoveTo };
		Vec2 point{};
	};

	struct CanvasPaint {
		CanvasPaintKind kind{ CanvasPaintKind::Solid };
		Color color{ Color::fromStraight(0.0F, 0.0F, 0.0F) };
		float line_width{ 1.0F };
		Color shadow_color{ Color::fromStraight(0.0F, 0.0F, 0.0F, 0.0F) };
		float shadow_blur{};
		float shadow_offset_x{};
		float shadow_offset_y{};
		CanvasCompositeMode composite{ CanvasCompositeMode::SourceOver };
		float radial_gradient[6]{};
		std::uint32_t first_gradient_stop{};
		std::uint32_t gradient_stop_count{};
	};

	struct CanvasFrameOperation {
		CanvasOperationKind kind{ CanvasOperationKind::FillRect };
		Rect rectangle{};
		Mat3x2 transform{};
		CanvasPaint paint{};
		std::uint32_t first_path_segment{};
		std::uint32_t path_segment_count{};
	};

	struct CanvasFrame {
		std::uint64_t page_revision{};
		std::uint64_t required_capabilities{ CanvasCapabilityRectangles };
		bool reset_surface{};
		bool has_background_color{};
		Color background_color{};
		std::vector<CanvasFrameOperation> operations;
		std::vector<CanvasPathSegment> path_segments;
		std::vector<CanvasGradientStop> gradient_stops;
	};

}  // namespace gpu2d