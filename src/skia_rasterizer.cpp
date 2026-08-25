#include "gpu2d/skia_rasterizer.hpp"

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkBlurTypes.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkRect.h"
#include "include/core/SkSurface.h"
#include "include/effects/SkGradientShader.h"
#include "include/gpu/GpuTypes.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/d3d/GrD3DBackendContext.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>

namespace gpu2d {
	namespace {

		[[nodiscard]] SkRect toSkRect(const Rect rectangle) noexcept {
			return SkRect::MakeLTRB(rectangle.left, rectangle.top, rectangle.right, rectangle.bottom);
		}

		[[nodiscard]] SkMatrix toSkMatrix(const Mat3x2& matrix) noexcept {
			return SkMatrix::MakeAll(matrix.m11, matrix.m21, matrix.dx, matrix.m12, matrix.m22, matrix.dy,
				0.0F, 0.0F, 1.0F);
		}

		[[nodiscard]] SkColor4f toStraightSkColor(const Color color) noexcept {
			if (color.a <= 0.0F) {
				return SkColor4f{ 0.0F, 0.0F, 0.0F, 0.0F };
			}
			return SkColor4f{ std::clamp(color.r / color.a, 0.0F, 1.0F),
							 std::clamp(color.g / color.a, 0.0F, 1.0F),
							 std::clamp(color.b / color.a, 0.0F, 1.0F), color.a };
		}

	}  // namespace

	namespace {

		[[nodiscard]] SkPath makePath(const CanvasFrame& frame, const CanvasFrameOperation& operation) {
			const std::uint64_t end = static_cast<std::uint64_t>(operation.first_path_segment) +
				operation.path_segment_count;
			if (end > frame.path_segments.size()) {
				throw std::invalid_argument("Canvas path segment range is invalid");
			}
			SkPath path;
			for (std::uint32_t index = 0; index < operation.path_segment_count; ++index) {
				const CanvasPathSegment& segment =
					frame.path_segments[operation.first_path_segment + index];
				switch (segment.verb) {
				case CanvasPathVerb::MoveTo: path.moveTo(segment.point.x, segment.point.y); break;
				case CanvasPathVerb::LineTo: path.lineTo(segment.point.x, segment.point.y); break;
				case CanvasPathVerb::Close: path.close(); break;
				}
			}
			return path;
		}

		[[nodiscard]] SkPaint makePaint(const CanvasFrame& frame,
			const CanvasFrameOperation& operation, const bool shadow) {
			SkPaint paint;
			paint.setAntiAlias(true);
			paint.setStyle(operation.kind == CanvasOperationKind::StrokeRect ||
				operation.kind == CanvasOperationKind::StrokePath
				? SkPaint::kStroke_Style
				: SkPaint::kFill_Style);
			paint.setStrokeWidth(operation.paint.line_width);
			paint.setBlendMode(operation.paint.composite == CanvasCompositeMode::Lighter
				? SkBlendMode::kPlus
				: SkBlendMode::kSrcOver);
			paint.setColor4f(toStraightSkColor(shadow ? operation.paint.shadow_color
				: operation.paint.color));
			if (!shadow && operation.paint.kind == CanvasPaintKind::RadialGradient) {
				const std::uint64_t end = static_cast<std::uint64_t>(operation.paint.first_gradient_stop) +
					operation.paint.gradient_stop_count;
				if (operation.paint.gradient_stop_count < 2U || end > frame.gradient_stops.size()) {
					throw std::invalid_argument("Canvas radial gradient stop range is invalid");
				}
				std::vector<SkColor4f> colors;
				std::vector<SkScalar> positions;
				colors.reserve(operation.paint.gradient_stop_count);
				positions.reserve(operation.paint.gradient_stop_count);
				for (std::uint32_t index = 0; index < operation.paint.gradient_stop_count; ++index) {
					const CanvasGradientStop& stop =
						frame.gradient_stops[operation.paint.first_gradient_stop + index];
					colors.push_back(toStraightSkColor(stop.color));
					positions.push_back(stop.offset);
				}
				const float* geometry = operation.paint.radial_gradient;
				paint.setShader(SkGradientShader::MakeTwoPointConical(
					SkPoint::Make(geometry[0], geometry[1]), geometry[2],
					SkPoint::Make(geometry[3], geometry[4]), geometry[5], colors.data(), nullptr,
					positions.data(), static_cast<int>(colors.size()), SkTileMode::kClamp));
			}
			if (shadow && operation.paint.shadow_blur > 0.0F) {
				paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle,
					operation.paint.shadow_blur * 0.5F));
			}
			return paint;
		}

		void drawGeometry(SkCanvas& canvas, const CanvasFrame& frame,
			const CanvasFrameOperation& operation, const SkPaint& paint) {
			if (operation.kind == CanvasOperationKind::FillPath ||
				operation.kind == CanvasOperationKind::StrokePath) {
				canvas.drawPath(makePath(frame, operation), paint);
			}
			else {
				canvas.drawRect(toSkRect(operation.rectangle), paint);
			}
		}

		void applyOperation(SkCanvas& canvas, const CanvasFrame& frame,
			const CanvasFrameOperation& operation) {
			canvas.save();
			canvas.concat(toSkMatrix(operation.transform));
			if (operation.kind == CanvasOperationKind::ClearRect) {
				SkPaint clear;
				if (frame.has_background_color) {
					clear.setBlendMode(SkBlendMode::kSrc);
					clear.setColor4f(toStraightSkColor(frame.background_color));
				}
				else {
					clear.setBlendMode(SkBlendMode::kClear);
				}
				canvas.drawRect(toSkRect(operation.rectangle), clear);
				canvas.restore();
				return;
			}
			if (operation.paint.shadow_color.a > 0.0F &&
				(operation.paint.shadow_blur > 0.0F || operation.paint.shadow_offset_x != 0.0F ||
					operation.paint.shadow_offset_y != 0.0F)) {
				canvas.save();
				canvas.translate(operation.paint.shadow_offset_x, operation.paint.shadow_offset_y);
				drawGeometry(canvas, frame, operation, makePaint(frame, operation, true));
				canvas.restore();
			}
			drawGeometry(canvas, frame, operation, makePaint(frame, operation, false));
			canvas.restore();
		}

	}  // namespace

	struct SkiaGpuCanvasSurface::Impl {
		sk_sp<GrDirectContext> context;
		sk_sp<SkSurface> surface;
		GrBackendTexture backend_texture;
		std::uint64_t revision{};
		std::uint32_t width{};
		std::uint32_t height{};

		Impl(IDXGIAdapter1* adapter, ID3D12Device* device, ID3D12CommandQueue* queue) {
			if (adapter == nullptr || device == nullptr || queue == nullptr) {
				throw std::invalid_argument("Skia GPU requires a D3D12 adapter, device, and command queue");
			}
			GrD3DBackendContext backend;
			backend.fAdapter.reset(GrSafeComAddRef(adapter));
			backend.fDevice.reset(GrSafeComAddRef(device));
			backend.fQueue.reset(GrSafeComAddRef(queue));
			context = GrDirectContext::MakeDirect3D(backend);
			if (!context) {
				throw std::runtime_error("Skia Ganesh failed to create a Direct3D 12 GPU context");
			}
		}

		bool ensure(const CanvasFrame& frame, const std::uint32_t new_width,
			const std::uint32_t new_height) {
			bool recreated = false;
			if (!surface || width != new_width || height != new_height) {
				context->flushAndSubmit(GrSyncCpu::kYes);
				backend_texture = GrBackendTexture{};
				surface.reset();
				const SkImageInfo info = SkImageInfo::Make(
					static_cast<int>(new_width), static_cast<int>(new_height),
					kRGBA_8888_SkColorType, kPremul_SkAlphaType);
				surface = SkSurfaces::RenderTarget(context.get(), skgpu::Budgeted::kNo, info, 0,
					kTopLeft_GrSurfaceOrigin, nullptr);
				if (!surface) {
					throw std::runtime_error("Skia Ganesh failed to create a D3D12 GPU surface");
				}
				width = new_width;
				height = new_height;
				revision = frame.page_revision;
				recreated = true;
			}
			if (recreated || frame.reset_surface || revision != frame.page_revision) {
				revision = frame.page_revision;
				surface->getCanvas()->clear(frame.has_background_color
					? toStraightSkColor(frame.background_color).toSkColor()
					: SK_ColorTRANSPARENT);
			}
			return recreated;
		}
	};

	SkiaGpuCanvasSurface::SkiaGpuCanvasSurface(IDXGIAdapter1* adapter, ID3D12Device* device,
		ID3D12CommandQueue* queue)
		: impl_(std::make_unique<Impl>(adapter, device, queue)) {
	}

	SkiaGpuCanvasSurface::~SkiaGpuCanvasSurface() = default;

	SkiaGpuSurfaceInfo SkiaGpuCanvasSurface::apply(const CanvasFrame& frame,
		const std::uint32_t width,
		const std::uint32_t height) {
		if (width == 0U || height == 0U) {
			throw std::invalid_argument("Skia GPU canvas dimensions must be non-zero");
		}
		const bool recreated = impl_->ensure(frame, width, height);
		SkCanvas& canvas = *impl_->surface->getCanvas();
		for (const CanvasFrameOperation& operation : frame.operations) {
			applyOperation(canvas, frame, operation);
		}
		impl_->backend_texture = SkSurfaces::GetBackendTexture(
			impl_->surface.get(), SkSurface::BackendHandleAccess::kFlushRead);
		if (!impl_->backend_texture.isValid()) {
			throw std::runtime_error("Skia GPU surface did not expose a backend D3D12 texture");
		}
		impl_->context->submit(GrSyncCpu::kNo);
		GrD3DTextureResourceInfo info;
		if (!impl_->backend_texture.getD3DTextureResourceInfo(&info) || !info.fResource) {
			throw std::runtime_error("Skia GPU backend texture is not a D3D12 resource");
		}
		return SkiaGpuSurfaceInfo{ info.fResource.get(), width, height,
								  static_cast<std::uint32_t>(info.fResourceState), recreated };
	}

	void SkiaGpuCanvasSurface::notifyExternalState(const std::uint32_t resource_state) {
		if (impl_->backend_texture.isValid()) {
			impl_->backend_texture.setD3DResourceState(
				static_cast<GrD3DResourceStateEnum>(resource_state));
		}
	}

	void SkiaGpuCanvasSurface::waitIdle() {
		impl_->context->flushAndSubmit(GrSyncCpu::kYes);
	}

}  // namespace gpu2d