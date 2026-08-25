#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace gpu2d {

	struct Vec2 {
		float x{};
		float y{};
	};

	struct Rect {
		float left{};
		float top{};
		float right{};
		float bottom{};

		[[nodiscard]] static constexpr Rect fromXYWH(float x, float y, float width, float height) {
			return Rect{ x, y, x + width, y + height };
		}

		[[nodiscard]] constexpr float width() const noexcept { return right - left; }
		[[nodiscard]] constexpr float height() const noexcept { return bottom - top; }
		[[nodiscard]] constexpr bool empty() const noexcept {
			return !(right > left && bottom > top);
		}
	};

	// Row-vector convention:
	// x' = x*m11 + y*m21 + dx
	// y' = x*m12 + y*m22 + dy
	struct Mat3x2 {
		float m11{ 1.0F };
		float m12{};
		float m21{};
		float m22{ 1.0F };
		float dx{};
		float dy{};

		[[nodiscard]] static constexpr Mat3x2 identity() noexcept { return {}; }
		[[nodiscard]] static constexpr Mat3x2 translation(float x, float y) noexcept {
			return Mat3x2{ 1.0F, 0.0F, 0.0F, 1.0F, x, y };
		}
		[[nodiscard]] static constexpr Mat3x2 scale(float x, float y) noexcept {
			return Mat3x2{ x, 0.0F, 0.0F, y, 0.0F, 0.0F };
		}
		[[nodiscard]] static Mat3x2 rotation(float radians) noexcept {
			const float cosine = std::cos(radians);
			const float sine = std::sin(radians);
			return Mat3x2{ cosine, sine, -sine, cosine, 0.0F, 0.0F };
		}
	};

	struct Color {
		float r{};
		float g{};
		float b{};
		float a{ 1.0F };

		[[nodiscard]] static constexpr Color fromStraight(float red, float green, float blue,
			float alpha = 1.0F) noexcept {
			return Color{ red * alpha, green * alpha, blue * alpha, alpha };
		}
	};

	[[nodiscard]] inline bool isFinite(const Vec2 value) noexcept {
		return std::isfinite(value.x) && std::isfinite(value.y);
	}

	[[nodiscard]] inline bool isFinite(const Rect value) noexcept {
		return std::isfinite(value.left) && std::isfinite(value.top) &&
			std::isfinite(value.right) && std::isfinite(value.bottom);
	}

	[[nodiscard]] inline bool isFinite(const Mat3x2& value) noexcept {
		return std::isfinite(value.m11) && std::isfinite(value.m12) &&
			std::isfinite(value.m21) && std::isfinite(value.m22) &&
			std::isfinite(value.dx) && std::isfinite(value.dy);
	}

	[[nodiscard]] inline bool isFinite(const Color value) noexcept {
		return std::isfinite(value.r) && std::isfinite(value.g) &&
			std::isfinite(value.b) && std::isfinite(value.a);
	}

	[[nodiscard]] inline Vec2 transformPoint(const Mat3x2& matrix, const Vec2 point) noexcept {
		return Vec2{ point.x * matrix.m11 + point.y * matrix.m21 + matrix.dx,
					point.x * matrix.m12 + point.y * matrix.m22 + matrix.dy };
	}

	// Returns a matrix that applies local first and parent second.
	[[nodiscard]] inline Mat3x2 multiply(const Mat3x2& parent, const Mat3x2& local) noexcept {
		return Mat3x2{
			local.m11 * parent.m11 + local.m12 * parent.m21,
			local.m11 * parent.m12 + local.m12 * parent.m22,
			local.m21 * parent.m11 + local.m22 * parent.m21,
			local.m21 * parent.m12 + local.m22 * parent.m22,
			local.dx * parent.m11 + local.dy * parent.m21 + parent.dx,
			local.dx * parent.m12 + local.dy * parent.m22 + parent.dy,
		};
	}

	[[nodiscard]] inline Rect intersect(const Rect left, const Rect right) noexcept {
		const Rect result{ std::max(left.left, right.left), std::max(left.top, right.top),
						  std::min(left.right, right.right), std::min(left.bottom, right.bottom) };
		return result.empty() ? Rect{} : result;
	}

	[[nodiscard]] inline Rect transformedBounds(const Mat3x2& matrix, const Rect rectangle) {
		if (!isFinite(matrix) || !isFinite(rectangle)) {
			throw std::invalid_argument("transformedBounds requires finite inputs");
		}
		if (rectangle.empty()) {
			return {};
		}

		const std::array<Vec2, 4> points{
			transformPoint(matrix, Vec2{rectangle.left, rectangle.top}),
			transformPoint(matrix, Vec2{rectangle.right, rectangle.top}),
			transformPoint(matrix, Vec2{rectangle.right, rectangle.bottom}),
			transformPoint(matrix, Vec2{rectangle.left, rectangle.bottom}),
		};

		Rect bounds{ points[0].x, points[0].y, points[0].x, points[0].y };
		for (const Vec2 point : points) {
			bounds.left = std::min(bounds.left, point.x);
			bounds.top = std::min(bounds.top, point.y);
			bounds.right = std::max(bounds.right, point.x);
			bounds.bottom = std::max(bounds.bottom, point.y);
		}
		return bounds;
	}

	[[nodiscard]] inline bool preservesAxisAlignment(const Mat3x2& matrix,
		const float epsilon = 1.0e-6F) noexcept {
		const bool diagonal = std::abs(matrix.m12) <= epsilon && std::abs(matrix.m21) <= epsilon;
		const bool quarter_turn = std::abs(matrix.m11) <= epsilon && std::abs(matrix.m22) <= epsilon;
		return diagonal || quarter_turn;
	}

	[[nodiscard]] inline Color checkedPremultiplied(Color color) {
		if (!isFinite(color) || color.a < 0.0F || color.a > 1.0F || color.r < 0.0F ||
			color.g < 0.0F || color.b < 0.0F || color.r > color.a || color.g > color.a ||
			color.b > color.a) {
			throw std::invalid_argument("color must be finite premultiplied RGBA in [0, alpha]");
		}
		return color;
	}

}  // namespace gpu2d
