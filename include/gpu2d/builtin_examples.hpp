#pragma once

#include <span>
#include <string_view>

namespace gpu2d {

	struct BuiltinExample {
		std::string_view name;
		std::string_view source;
		bool requires_skia{};
	};

	[[nodiscard]] std::span<const BuiltinExample> builtinExamples() noexcept;

}  // namespace gpu2d