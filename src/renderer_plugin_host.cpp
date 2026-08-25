#include "gpu2d/renderer_plugin_host.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace gpu2d {
	namespace {

		constexpr DWORD kRendererLoadFlags =
			LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32;

		[[nodiscard]] std::string windowsErrorMessage(const DWORD error) {
			char* buffer = nullptr;
			const DWORD length = FormatMessageA(
				FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
				FORMAT_MESSAGE_IGNORE_INSERTS,
				nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
				reinterpret_cast<char*>(&buffer), 0, nullptr);
			std::string message = length != 0 && buffer != nullptr
				? std::string(buffer, static_cast<std::size_t>(length))
				: std::string("Win32 error ") + std::to_string(error);
			if (buffer != nullptr) {
				LocalFree(buffer);
			}
			while (!message.empty() && (message.back() == '\r' || message.back() == '\n' ||
				message.back() == ' ' || message.back() == '.')) {
				message.pop_back();
			}
			return message + " (error " + std::to_string(error) + ")";
		}

		[[nodiscard]] Gpu2dCanvasOperation flatten(const CanvasFrameOperation& operation) noexcept {
			Gpu2dCanvasOperation result{};
			result.kind = static_cast<std::uint32_t>(operation.kind);
			result.rectangle[0] = operation.rectangle.left;
			result.rectangle[1] = operation.rectangle.top;
			result.rectangle[2] = operation.rectangle.right;
			result.rectangle[3] = operation.rectangle.bottom;
			result.transform[0] = operation.transform.m11;
			result.transform[1] = operation.transform.m12;
			result.transform[2] = operation.transform.m21;
			result.transform[3] = operation.transform.m22;
			result.transform[4] = operation.transform.dx;
			result.transform[5] = operation.transform.dy;
			result.color[0] = operation.paint.color.r;
			result.color[1] = operation.paint.color.g;
			result.color[2] = operation.paint.color.b;
			result.color[3] = operation.paint.color.a;
			result.line_width = operation.paint.line_width;
			result.shadow_color[0] = operation.paint.shadow_color.r;
			result.shadow_color[1] = operation.paint.shadow_color.g;
			result.shadow_color[2] = operation.paint.shadow_color.b;
			result.shadow_color[3] = operation.paint.shadow_color.a;
			result.shadow_blur = operation.paint.shadow_blur;
			result.shadow_offset_x = operation.paint.shadow_offset_x;
			result.shadow_offset_y = operation.paint.shadow_offset_y;
			result.composite_mode = static_cast<std::uint32_t>(operation.paint.composite);
			result.paint_kind = static_cast<std::uint32_t>(operation.paint.kind);
			std::copy(std::begin(operation.paint.radial_gradient),
				std::end(operation.paint.radial_gradient), std::begin(result.radial_gradient));
			result.first_gradient_stop = operation.paint.first_gradient_stop;
			result.gradient_stop_count = operation.paint.gradient_stop_count;
			result.first_path_segment = operation.first_path_segment;
			result.path_segment_count = operation.path_segment_count;
			return result;
		}

		[[nodiscard]] const Gpu2dRendererApi* loadApi(HMODULE module) {
			const auto get_api = reinterpret_cast<Gpu2dGetRendererApiFunction>(
				GetProcAddress(module, GPU2D_RENDERER_ENTRY_POINT));
			if (get_api == nullptr) {
				return nullptr;
			}
			const Gpu2dRendererApi* api = get_api(GPU2D_RENDERER_ABI_VERSION);
			if (api == nullptr || api->struct_size < sizeof(Gpu2dRendererApi) ||
				api->abi_version != GPU2D_RENDERER_ABI_VERSION || api->renderer_id == nullptr ||
				api->display_name == nullptr || api->create == nullptr || api->destroy == nullptr ||
				api->resize == nullptr || api->render == nullptr || api->wait_idle == nullptr ||
				api->adapter_name == nullptr || api->last_error == nullptr) {
				return nullptr;
			}
			return api;
		}

	}  // namespace

	RendererPluginHost::~RendererPluginHost() { unload(); }

	std::vector<RendererPluginInfo> RendererPluginHost::discover(
		const std::filesystem::path& directory,
		std::vector<RendererPluginDiagnostic>* diagnostics) {
		std::vector<RendererPluginInfo> plugins;
		std::error_code error;
		if (!std::filesystem::is_directory(directory, error)) {
			if (diagnostics != nullptr) {
				diagnostics->push_back(RendererPluginDiagnostic{
					directory, error ? error.message() : "renderer directory does not exist" });
			}
			return plugins;
		}
		for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
			if (error || !entry.is_regular_file() ||
				entry.path().filename().wstring().rfind(L"gpu2d_renderer_", 0) != 0 ||
				entry.path().extension() != L".dll") {
				continue;
			}
			HMODULE module = LoadLibraryExW(entry.path().c_str(), nullptr, kRendererLoadFlags);
			if (module == nullptr) {
				if (diagnostics != nullptr) {
					diagnostics->push_back(
						RendererPluginDiagnostic{ entry.path(), windowsErrorMessage(GetLastError()) });
				}
				continue;
			}
			const Gpu2dRendererApi* api = loadApi(module);
			if (api != nullptr) {
				plugins.push_back(
					RendererPluginInfo{ entry.path(), api->renderer_id, api->display_name, api->capabilities });
			}
			else if (diagnostics != nullptr) {
				diagnostics->push_back(
					RendererPluginDiagnostic{ entry.path(), "missing or incompatible renderer ABI" });
			}
			FreeLibrary(module);
		}
		std::sort(plugins.begin(), plugins.end(), [](const auto& left, const auto& right) {
			return left.display_name < right.display_name;
			});
		return plugins;
	}

	void RendererPluginHost::load(const RendererPluginInfo& plugin, HWND window,
		const std::uint32_t width, const std::uint32_t height,
		const bool vsync, const bool debug_layer) {
		unload();
		module_ = LoadLibraryExW(plugin.path.c_str(), nullptr, kRendererLoadFlags);
		if (module_ == nullptr) {
			throw std::runtime_error("LoadLibraryW failed for renderer plugin " + plugin.path.string() +
				": " + windowsErrorMessage(GetLastError()));
		}
		api_ = loadApi(module_);
		if (api_ == nullptr) {
			unload();
			throw std::runtime_error("renderer plugin ABI validation failed");
		}
		const Gpu2dRendererCreateInfo create_info{ sizeof(Gpu2dRendererCreateInfo), window, width, height,
												  vsync ? 1U : 0U, debug_layer ? 1U : 0U };
		instance_ = api_->create(&create_info);
		if (instance_ == nullptr) {
			const std::string error = pluginError("renderer create");
			unload();
			throw std::runtime_error(error);
		}
		renderer_name_ = api_->display_name;
	}

	void RendererPluginHost::unload() noexcept {
		if (api_ != nullptr && instance_ != nullptr) {
			api_->wait_idle(instance_);
			api_->destroy(instance_);
		}
		instance_ = nullptr;
		api_ = nullptr;
		renderer_name_.clear();
		operations_.clear();
		path_segments_.clear();
	gradient_stops_.clear();
		if (module_ != nullptr) {
			FreeLibrary(module_);
			module_ = nullptr;
		}
	}

	void RendererPluginHost::resize(const std::uint32_t width, const std::uint32_t height) {
		if (!loaded() || api_->resize(instance_, width, height) == 0) {
			throw std::runtime_error(pluginError("renderer resize"));
		}
	}

	Gpu2dRendererTelemetry RendererPluginHost::render(const CanvasFrame& canvas_frame,
		const std::uint32_t viewport_width,
		const std::uint32_t viewport_height,
		const std::uint32_t canvas_width,
		const std::uint32_t canvas_height) {
		if (!loaded()) {
			throw std::logic_error("no renderer plugin is loaded");
		}
		operations_.clear();
		operations_.reserve(canvas_frame.operations.size());
		for (const CanvasFrameOperation& operation : canvas_frame.operations) {
			operations_.push_back(flatten(operation));
		}
		path_segments_.clear();
		path_segments_.reserve(canvas_frame.path_segments.size());
		for (const CanvasPathSegment& segment : canvas_frame.path_segments) {
			path_segments_.push_back(Gpu2dCanvasPathSegment{
				static_cast<std::uint32_t>(segment.verb), segment.point.x, segment.point.y });
		}
	gradient_stops_.clear();
	gradient_stops_.reserve(canvas_frame.gradient_stops.size());
	for (const CanvasGradientStop& stop : canvas_frame.gradient_stops) {
		Gpu2dCanvasGradientStop flattened{};
		flattened.offset = stop.offset;
		flattened.color[0] = stop.color.r;
		flattened.color[1] = stop.color.g;
		flattened.color[2] = stop.color.b;
		flattened.color[3] = stop.color.a;
		gradient_stops_.push_back(flattened);
	}
	if (operations_.size() > UINT32_MAX || path_segments_.size() > UINT32_MAX ||
		gradient_stops_.size() > UINT32_MAX) {
			throw std::overflow_error("renderer canvas frame exceeds uint32");
		}
		if ((canvas_frame.required_capabilities & ~api_->capabilities) != 0U) {
			throw std::runtime_error("renderer capability mismatch for this Canvas page");
		}
		const Gpu2dRendererFrame frame{
			sizeof(Gpu2dRendererFrame), viewport_width, viewport_height, canvas_width, canvas_height,
			canvas_frame.page_revision, canvas_frame.required_capabilities,
		canvas_frame.reset_surface ? 1U : 0U, canvas_frame.has_background_color ? 1U : 0U,
		{canvas_frame.background_color.r, canvas_frame.background_color.g,
		 canvas_frame.background_color.b, canvas_frame.background_color.a}, operations_.data(),
			static_cast<std::uint32_t>(operations_.size()), path_segments_.data(),
		static_cast<std::uint32_t>(path_segments_.size()), gradient_stops_.data(),
		static_cast<std::uint32_t>(gradient_stops_.size())};
		Gpu2dRendererTelemetry telemetry{};
		if (api_->render(instance_, &frame, &telemetry) == 0) {
			throw std::runtime_error(pluginError("renderer render"));
		}
		return telemetry;
	}

	std::string RendererPluginHost::adapterName() const {
		return loaded() ? api_->adapter_name(instance_) : std::string{};
	}

	std::string RendererPluginHost::rendererName() const { return renderer_name_; }

	std::uint64_t RendererPluginHost::capabilities() const noexcept {
		return api_ != nullptr ? api_->capabilities : 0U;
	}

	std::string RendererPluginHost::pluginError(const char* operation) const {
		const char* detail = api_ != nullptr && api_->last_error != nullptr ? api_->last_error() : nullptr;
		return std::string(operation) + " failed" +
			(detail != nullptr && detail[0] != '\0' ? std::string(": ") + detail : std::string{});
	}

}  // namespace gpu2d