#include "gpu2d/display_list.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace gpu2d {

	Canvas::Canvas() = default;

	void Canvas::ensureRecording() const {
		if (finished_) {
			throw std::logic_error("Canvas cannot be used after finish()");
		}
	}

	void Canvas::validateRect(const Rect rectangle, const char* operation) {
		if (!isFinite(rectangle) || rectangle.empty()) {
			throw std::invalid_argument(std::string(operation) + " requires a finite non-empty rectangle");
		}
	}

	void Canvas::save() {
		ensureRecording();
		stack_.push_back(state_);
	}

	void Canvas::restore() {
		ensureRecording();
		if (stack_.empty()) {
			throw std::logic_error("Canvas::restore stack underflow");
		}
		state_ = stack_.back();
		stack_.pop_back();
	}

	void Canvas::concat(const Mat3x2& transform) {
		ensureRecording();
		if (!isFinite(transform)) {
			throw std::invalid_argument("Canvas::concat requires a finite matrix");
		}
		state_.transform = multiply(state_.transform, transform);
	}

	void Canvas::clipRect(const Rect local_clip) {
		ensureRecording();
		validateRect(local_clip, "Canvas::clipRect");
		if (!preservesAxisAlignment(state_.transform)) {
			throw std::invalid_argument(
				"milestone 1 only supports clips that remain axis-aligned in device space");
		}
		const Rect device_clip = transformedBounds(state_.transform, local_clip);
		state_.device_clip = state_.device_clip ? intersect(*state_.device_clip, device_clip) : device_clip;
	}

	void Canvas::drawRect(const Rect local_rect, const Color premultiplied_color) {
		ensureRecording();
		validateRect(local_rect, "Canvas::drawRect");
		operations_.push_back(DrawOp{ PipelineKind::Solid, local_rect, state_.transform,
									 state_.device_clip, checkedPremultiplied(premultiplied_color),
									 Rect::fromXYWH(0.0F, 0.0F, 1.0F, 1.0F), {} });
	}

	void Canvas::drawImage(const Rect local_rect, const TextureHandle texture, const Rect uv_rect,
		const Color premultiplied_modulation) {
		ensureRecording();
		validateRect(local_rect, "Canvas::drawImage");
		validateRect(uv_rect, "Canvas::drawImage uv");
		if (!texture.valid()) {
			throw std::invalid_argument("Canvas::drawImage requires a valid texture handle");
		}
		operations_.push_back(DrawOp{ PipelineKind::Textured, local_rect, state_.transform,
									 state_.device_clip, checkedPremultiplied(premultiplied_modulation),
									 uv_rect, texture });
	}

	DisplayList Canvas::finish() {
		ensureRecording();
		if (!stack_.empty()) {
			throw std::logic_error("Canvas::finish requires balanced save/restore calls");
		}
		finished_ = true;
		return DisplayList(std::move(operations_));
	}

}  // namespace gpu2d
