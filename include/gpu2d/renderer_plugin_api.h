#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GPU2D_RENDERER_ABI_VERSION 4U
#define GPU2D_RENDERER_ENTRY_POINT "gpu2dGetRendererApi"

	typedef struct Gpu2dRendererCreateInfo {
		uint32_t struct_size;
		void* native_window;
		uint32_t width;
		uint32_t height;
		uint32_t vsync;
		uint32_t enable_debug_layer;
	} Gpu2dRendererCreateInfo;

	typedef struct Gpu2dCanvasPathSegment {
		uint32_t verb;
		float x;
		float y;
	} Gpu2dCanvasPathSegment;

	typedef struct Gpu2dCanvasGradientStop {
		float offset;
		float color[4];
	} Gpu2dCanvasGradientStop;

	typedef struct Gpu2dCanvasOperation {
		uint32_t kind;
		float rectangle[4];
		float transform[6];
		float color[4];
		float line_width;
		float shadow_color[4];
		float shadow_blur;
		float shadow_offset_x;
		float shadow_offset_y;
		uint32_t composite_mode;
		uint32_t paint_kind;
		float radial_gradient[6];
		uint32_t first_gradient_stop;
		uint32_t gradient_stop_count;
		uint32_t first_path_segment;
		uint32_t path_segment_count;
	} Gpu2dCanvasOperation;

	typedef struct Gpu2dRendererFrame {
		uint32_t struct_size;
		uint32_t width;
		uint32_t height;
		uint32_t canvas_width;
		uint32_t canvas_height;
		uint64_t page_revision;
		uint64_t required_capabilities;
		uint32_t reset_surface;
		uint32_t has_background_color;
		float background_color[4];
		const Gpu2dCanvasOperation* operations;
		uint32_t operation_count;
		const Gpu2dCanvasPathSegment* path_segments;
		uint32_t path_segment_count;
		const Gpu2dCanvasGradientStop* gradient_stops;
		uint32_t gradient_stop_count;
	} Gpu2dRendererFrame;

	typedef struct Gpu2dRendererTelemetry {
		uint32_t draw_ops;
		uint32_t culled_ops;
		uint32_t batches;
		uint32_t instances;
		uint64_t upload_bytes;
		uint64_t submitted_fence;
		uint64_t completed_fence;
		uint64_t deferred_releases;
		double cpu_frame_ms;
		double paint_ms;
		double texture_update_ms;
		double plugin_total_ms;
	} Gpu2dRendererTelemetry;

	typedef struct Gpu2dRendererApi {
		uint32_t struct_size;
		uint32_t abi_version;
		const char* renderer_id;
		const char* display_name;
		uint64_t capabilities;
		void* (*create)(const Gpu2dRendererCreateInfo* create_info);
		void (*destroy)(void* instance);
		int32_t(*resize)(void* instance, uint32_t width, uint32_t height);
		int32_t(*render)(void* instance, const Gpu2dRendererFrame* frame,
			Gpu2dRendererTelemetry* telemetry);
		void (*wait_idle)(void* instance);
		const char* (*adapter_name)(void* instance);
		const char* (*last_error)(void);
	} Gpu2dRendererApi;

	typedef const Gpu2dRendererApi* (*Gpu2dGetRendererApiFunction)(uint32_t requested_abi_version);

#ifdef __cplusplus
}
#endif