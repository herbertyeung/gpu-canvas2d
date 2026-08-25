#pragma once

#include "gpu2d/render_types.hpp"

#include <optional>
#include <span>
#include <vector>

namespace gpu2d {

	struct DrawOp {
		PipelineKind pipeline{ PipelineKind::Solid };
		Rect local_rect{};
		Mat3x2 transform{};
		std::optional<Rect> device_clip;
		Color color{};
		Rect uv_rect{ Rect::fromXYWH(0.0F, 0.0F, 1.0F, 1.0F) };
		TextureHandle texture{};
	};

	class DisplayList {
	public:
		DisplayList() = default;

		[[nodiscard]] std::span<const DrawOp> operations() const noexcept { return operations_; }
		[[nodiscard]] bool empty() const noexcept { return operations_.empty(); }
		[[nodiscard]] static DisplayList fromOperations(std::vector<DrawOp> operations) {
			return DisplayList(std::move(operations));
		}

	private:
		explicit DisplayList(std::vector<DrawOp> operations) : operations_(std::move(operations)) {}

		std::vector<DrawOp> operations_;
		friend class Canvas;
	};

	class Canvas {
	public:
		Canvas();

		void save();
		void restore();
		void concat(const Mat3x2& transform);
		void clipRect(Rect local_clip);
		void drawRect(Rect local_rect, Color premultiplied_color);
		void drawImage(Rect local_rect, TextureHandle texture, Rect uv_rect,
			Color premultiplied_modulation = Color::fromStraight(1.0F, 1.0F, 1.0F));
		[[nodiscard]] DisplayList finish();

	private:
		struct State {
			Mat3x2 transform{ Mat3x2::identity() };
			std::optional<Rect> device_clip;
		};

		void ensureRecording() const;
		static void validateRect(Rect rectangle, const char* operation);

		State state_;
		std::vector<State> stack_;
		std::vector<DrawOp> operations_;
		bool finished_{};
	};

}  // namespace gpu2d
