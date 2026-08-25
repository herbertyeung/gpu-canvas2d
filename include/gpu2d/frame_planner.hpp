#pragma once

#include "gpu2d/display_list.hpp"

namespace gpu2d {

	class FramePlanner {
	public:
		[[nodiscard]] RenderPlan build(const DisplayList& display_list, Rect viewport) const;
	};

}  // namespace gpu2d
