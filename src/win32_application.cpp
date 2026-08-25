#include "gpu2d/win32_application.hpp"

#include "gpu2d/builtin_examples.hpp"
#include "gpu2d/javascript_page.hpp"
#include "gpu2d/renderer_plugin_host.hpp"

#include <windows.h>
#include <windowsx.h>
#include <richedit.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace gpu2d {
	namespace {

		constexpr wchar_t kWindowClassName[] = L"Gpu2DLearningLabBrowserShell";
		constexpr wchar_t kRenderSurfaceClassName[] = L"Gpu2DLearningLabRenderSurface";
		constexpr int kEditorControlId = 1001;
		constexpr int kRendererControlId = 1002;
		constexpr int kExampleControlId = 1003;
		constexpr int kSplitterWidth = 8;
		constexpr int kStatusHeight = 26;
		constexpr int kToolbarHeight = 36;
		constexpr UINT_PTR kLiveParseTimerId = 1;
		constexpr UINT kLiveParseDelayMs = 180;

		[[nodiscard]] std::wstring widenUtf8(const std::string_view text) {
			if (text.empty()) {
				return {};
			}
			const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
				static_cast<int>(text.size()), nullptr, 0);
			if (required <= 0) {
				return std::wstring(text.begin(), text.end());
			}
			std::wstring result(static_cast<std::size_t>(required), L'\0');
			MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
				result.data(), required);
			return result;
		}

		[[nodiscard]] std::string narrowUtf8(const std::wstring_view text) {
			if (text.empty()) {
				return {};
			}
			const int required = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
				nullptr, 0, nullptr, nullptr);
			if (required <= 0) {
				return {};
			}
			std::string result(static_cast<std::size_t>(required), '\0');
			WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
				required, nullptr, nullptr);
			return result;
		}

		[[nodiscard]] std::string readTextFile(const std::filesystem::path& path) {
			std::ifstream file(path, std::ios::binary);
			if (!file) {
				throw std::runtime_error("failed to open HTML source file: " + path.string());
			}
			std::ostringstream stream;
			stream << file.rdbuf();
			return stream.str();
		}

		[[nodiscard]] std::filesystem::path executableDirectory() {
			std::wstring buffer(32768, L'\0');
			const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
				static_cast<DWORD>(buffer.size()));
			if (length == 0 || length >= buffer.size()) {
				throw std::runtime_error("GetModuleFileNameW failed");
			}
			buffer.resize(length);
			return std::filesystem::path(buffer).parent_path();
		}

		struct WindowState {
			std::uint32_t initial_width{};
			std::uint32_t initial_height{};
			HWND shell{};
			HWND renderer_label{};
			HWND renderer_combo{};
			HWND example_label{};
			HWND example_combo{};
			HWND editor{};
			HWND render_surface{};
			HWND status{};
			HFONT editor_font{};
			HFONT ui_font{};
			std::uint32_t render_width{};
			std::uint32_t render_height{};
			bool resize_pending{};
			bool renderer_switch_pending{};
			bool example_switch_pending{};
			bool parse_pending{};
			bool suppress_editor_change{};
			bool rich_editor{};
			bool paused{};
			bool reload_texture{};
			bool pointer_move_pending{};
			float pointer_x{};
			float pointer_y{};
			bool quit{};
			std::uint64_t parse_revision{};
			std::uint64_t renderer_revision{};
		};

		[[nodiscard]] std::string editorText(HWND editor) {
			const int length = GetWindowTextLengthW(editor);
			if (length <= 0) {
				return {};
			}
			std::wstring buffer(static_cast<std::size_t>(length) + 1U, L'\0');
			const int copied = GetWindowTextW(editor, buffer.data(), length + 1);
			buffer.resize(static_cast<std::size_t>(std::max(0, copied)));
			return narrowUtf8(buffer);
		}

		void setStatus(WindowState& state, const std::wstring& text) {
			if (state.status != nullptr) {
				SetWindowTextW(state.status, text.c_str());
			}
			if (state.shell != nullptr) {
				SetWindowTextW(state.shell, text.c_str());
			}
		}

		void layoutChildren(WindowState& state, const std::uint32_t width, const std::uint32_t height) {
			if (state.editor == nullptr || state.render_surface == nullptr || width == 0 || height == 0) {
				return;
			}

			const int available_height =
				std::max(1, static_cast<int>(height) - kToolbarHeight - kStatusHeight);
			const int editor_width = std::clamp(static_cast<int>(width) * 44 / 100, 320,
				std::max(320, static_cast<int>(width) - 320));
			const int render_x = editor_width + kSplitterWidth;
			const int render_width = std::max(1, static_cast<int>(width) - render_x);

			if (state.renderer_label != nullptr) {
				MoveWindow(state.renderer_label, 8, 9, 62, 22, TRUE);
			}
			if (state.renderer_combo != nullptr) {
				MoveWindow(state.renderer_combo, 72, 5, 220, 300, TRUE);
			}
			if (state.example_label != nullptr) {
				MoveWindow(state.example_label, 304, 9, 58, 22, TRUE);
			}
			if (state.example_combo != nullptr) {
				MoveWindow(state.example_combo, 364, 5,
					std::max(120, static_cast<int>(width) - 372), 320, TRUE);
			}
			MoveWindow(state.editor, 0, kToolbarHeight, editor_width, available_height, TRUE);
			MoveWindow(state.render_surface, render_x, kToolbarHeight, render_width, available_height, TRUE);
			if (state.status != nullptr) {
				MoveWindow(state.status, 0, kToolbarHeight + available_height, static_cast<int>(width),
					kStatusHeight, TRUE);
			}

			state.render_width = static_cast<std::uint32_t>(render_width);
			state.render_height = static_cast<std::uint32_t>(available_height);
			state.resize_pending = true;
		}

		[[nodiscard]] bool renderSurfaceFocused(const WindowState& state) noexcept {
			return GetFocus() == state.render_surface;
		}

		[[nodiscard]] bool handleKeyMessage(WindowState& state, const MSG& message) {
			if (message.message != WM_KEYDOWN || (message.lParam & (1LL << 30)) != 0) {
				return false;
			}
			const WPARAM key = message.wParam;
			if (key == VK_ESCAPE) {
				DestroyWindow(state.shell);
				return true;
			}
			if (key == VK_F5) {
				KillTimer(state.shell, kLiveParseTimerId);
				state.parse_pending = true;
				return true;
			}
			if (!renderSurfaceFocused(state)) {
				return false;
			}
			if (key == VK_SPACE) {
				state.paused = !state.paused;
			}
			else if (key == static_cast<WPARAM>('R')) {
				state.reload_texture = true;
			}
			else {
				return false;
			}
			return true;
		}

		LRESULT CALLBACK renderSurfaceProcedure(HWND window, const UINT message, const WPARAM wparam,
			const LPARAM lparam) {
			WindowState* state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
			switch (message) {
			case WM_NCCREATE: {
				const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
				SetWindowLongPtrW(window, GWLP_USERDATA,
					reinterpret_cast<LONG_PTR>(create->lpCreateParams));
				return TRUE;
			}
			case WM_LBUTTONDOWN:
				SetFocus(window);
				return 0;
			case WM_MOUSEMOVE:
				if (state != nullptr) {
					state->pointer_x = static_cast<float>(GET_X_LPARAM(lparam));
					state->pointer_y = static_cast<float>(GET_Y_LPARAM(lparam));
					state->pointer_move_pending = true;
				}
				return 0;
			case WM_ERASEBKGND:
				return 1;
			default:
				return DefWindowProcW(window, message, wparam, lparam);
			}
		}

		LRESULT CALLBACK windowProcedure(HWND window, const UINT message, const WPARAM wparam,
			const LPARAM lparam) {
			WindowState* state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
			if (message == WM_NCCREATE) {
				const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
				state = static_cast<WindowState*>(create->lpCreateParams);
				state->shell = window;
				SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
			}

			switch (message) {
			case WM_SIZE:
				if (state != nullptr) {
					layoutChildren(*state, static_cast<std::uint32_t>(LOWORD(lparam)),
						static_cast<std::uint32_t>(HIWORD(lparam)));
				}
				return 0;
			case WM_COMMAND:
				if (state != nullptr && LOWORD(wparam) == kEditorControlId && HIWORD(wparam) == EN_CHANGE &&
					!state->suppress_editor_change) {
					if (state->example_combo != nullptr) {
						SendMessageW(state->example_combo, CB_SETCURSEL, 0, 0);
					}
					SetTimer(window, kLiveParseTimerId, kLiveParseDelayMs, nullptr);
				}
				if (state != nullptr && LOWORD(wparam) == kRendererControlId &&
					HIWORD(wparam) == CBN_SELCHANGE) {
					state->renderer_switch_pending = true;
				}
				if (state != nullptr && LOWORD(wparam) == kExampleControlId &&
					HIWORD(wparam) == CBN_SELCHANGE) {
					state->example_switch_pending = true;
				}
				return 0;
			case WM_TIMER:
				if (state != nullptr && wparam == kLiveParseTimerId) {
					KillTimer(window, kLiveParseTimerId);
					state->parse_pending = true;
					return 0;
				}
				return DefWindowProcW(window, message, wparam, lparam);
			case WM_KEYDOWN:
				if (state != nullptr && (lparam & (1LL << 30)) == 0) {
					if (wparam == VK_ESCAPE) {
						DestroyWindow(window);
					}
					else if (wparam == VK_F5) {
						state->parse_pending = true;
					}
					else if (wparam == VK_SPACE && renderSurfaceFocused(*state)) {
						state->paused = !state->paused;
					}
					else if (wparam == static_cast<WPARAM>('R') && renderSurfaceFocused(*state)) {
						state->reload_texture = true;
					}
				}
				return 0;
			case WM_DESTROY:
				if (state != nullptr) {
					state->quit = true;
				}
				PostQuitMessage(0);
				return 0;
			default:
				return DefWindowProcW(window, message, wparam, lparam);
			}
		}

		void registerWindowClasses(const HINSTANCE instance) {
			WNDCLASSEXW shell_class{};
			shell_class.cbSize = sizeof(shell_class);
			shell_class.style = CS_HREDRAW | CS_VREDRAW;
			shell_class.lpfnWndProc = windowProcedure;
			shell_class.hInstance = instance;
			shell_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
			shell_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
			shell_class.lpszClassName = kWindowClassName;
			if (RegisterClassExW(&shell_class) == 0) {
				throw std::runtime_error("RegisterClassExW shell failed");
			}

			WNDCLASSEXW render_class{};
			render_class.cbSize = sizeof(render_class);
			render_class.style = CS_HREDRAW | CS_VREDRAW;
			render_class.lpfnWndProc = renderSurfaceProcedure;
			render_class.hInstance = instance;
			render_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
			render_class.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
			render_class.lpszClassName = kRenderSurfaceClassName;
			if (RegisterClassExW(&render_class) == 0) {
				throw std::runtime_error("RegisterClassExW render surface failed");
			}
		}

		[[nodiscard]] HWND createShellWindow(const ApplicationOptions& options, WindowState& state) {
			const HINSTANCE instance = GetModuleHandleW(nullptr);
			registerWindowClasses(instance);

			RECT window_rectangle{ 0, 0, static_cast<LONG>(options.width), static_cast<LONG>(options.height) };
			constexpr DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
			if (AdjustWindowRectEx(&window_rectangle, style, FALSE, 0) == FALSE) {
				throw std::runtime_error("AdjustWindowRectEx failed");
			}

			HWND window = CreateWindowExW(0, kWindowClassName, L"GPU 2D Lab by Hongtao",
				style, CW_USEDEFAULT, CW_USEDEFAULT,
				window_rectangle.right - window_rectangle.left,
				window_rectangle.bottom - window_rectangle.top, nullptr, nullptr,
				instance, &state);
			if (window == nullptr) {
				throw std::runtime_error("CreateWindowExW shell failed");
			}
			return window;
		}

		void createChildControls(WindowState& state, const std::string& initial_source) {
			const HINSTANCE instance = GetModuleHandleW(nullptr);
			static_cast<void>(LoadLibraryW(L"Msftedit.dll"));

			state.renderer_label = CreateWindowExW(0, L"STATIC", L"Renderer:", WS_CHILD | WS_VISIBLE,
				0, 0, 100, 24, state.shell, nullptr, instance, nullptr);
			state.renderer_combo = CreateWindowExW(
				0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
				0, 0, 300, 300, state.shell,
				reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRendererControlId)), instance, nullptr);
			if (state.renderer_combo == nullptr) {
				throw std::runtime_error("failed to create renderer selector");
			}
			state.example_label = CreateWindowExW(0, L"STATIC", L"Example:", WS_CHILD | WS_VISIBLE,
				0, 0, 100, 24, state.shell, nullptr, instance, nullptr);
			state.example_combo = CreateWindowExW(
				0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
				0, 0, 420, 320, state.shell,
				reinterpret_cast<HMENU>(static_cast<INT_PTR>(kExampleControlId)), instance, nullptr);
			if (state.example_combo == nullptr) {
				throw std::runtime_error("failed to create example selector");
			}

			state.editor = CreateWindowExW(WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"",
				WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE |
				ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_NOHIDESEL | ES_WANTRETURN,
				0, 0, 100, 100, state.shell,
				reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditorControlId)),
				instance, nullptr);
			state.rich_editor = state.editor != nullptr;
			if (state.editor == nullptr) {
				state.editor = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
					WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE |
					ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_NOHIDESEL | ES_WANTRETURN,
					0, 0, 100, 100, state.shell,
					reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditorControlId)),
					instance, nullptr);
			}
			if (state.editor == nullptr) {
				throw std::runtime_error("failed to create source editor control");
			}

			if (state.rich_editor) {
				const LRESULT current_mask = SendMessageW(state.editor, EM_GETEVENTMASK, 0, 0);
				SendMessageW(state.editor, EM_SETEVENTMASK, 0,
					static_cast<LPARAM>(current_mask | ENM_CHANGE));
				const LRESULT installed_mask = SendMessageW(state.editor, EM_GETEVENTMASK, 0, 0);
				if ((installed_mask & ENM_CHANGE) == 0) {
					throw std::runtime_error("RichEdit failed to enable ENM_CHANGE live-edit notifications");
				}
			}

			state.render_surface = CreateWindowExW(WS_EX_CLIENTEDGE, kRenderSurfaceClassName, L"",
				WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP, 0, 0,
				100, 100, state.shell, nullptr, instance, &state);
			if (state.render_surface == nullptr) {
				throw std::runtime_error("failed to create render surface child window");
			}

			state.status = CreateWindowExW(0, L"STATIC", L"Ready", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
				0, 0, 100, kStatusHeight, state.shell, nullptr, instance, nullptr);

			state.editor_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
				OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
				FIXED_PITCH | FF_MODERN, L"Consolas");
			state.ui_font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
			if (state.editor_font != nullptr) {
				SendMessageW(state.editor, WM_SETFONT, reinterpret_cast<WPARAM>(state.editor_font), TRUE);
			}
			if (state.status != nullptr && state.ui_font != nullptr) {
				SendMessageW(state.status, WM_SETFONT, reinterpret_cast<WPARAM>(state.ui_font), TRUE);
			}
			if (state.renderer_label != nullptr && state.ui_font != nullptr) {
				SendMessageW(state.renderer_label, WM_SETFONT, reinterpret_cast<WPARAM>(state.ui_font), TRUE);
			}
			if (state.renderer_combo != nullptr && state.ui_font != nullptr) {
				SendMessageW(state.renderer_combo, WM_SETFONT, reinterpret_cast<WPARAM>(state.ui_font), TRUE);
			}
			if (state.example_label != nullptr && state.ui_font != nullptr) {
				SendMessageW(state.example_label, WM_SETFONT, reinterpret_cast<WPARAM>(state.ui_font), TRUE);
			}
			if (state.example_combo != nullptr && state.ui_font != nullptr) {
				SendMessageW(state.example_combo, WM_SETFONT, reinterpret_cast<WPARAM>(state.ui_font), TRUE);
			}

			state.suppress_editor_change = true;
			const std::wstring source = widenUtf8(initial_source);
			SetWindowTextW(state.editor, source.c_str());
			state.suppress_editor_change = false;
		}

		void printTelemetry(const Gpu2dRendererTelemetry& telemetry, const std::uint64_t frame_number,
			const std::string& adapter_name) {
			std::cout << std::fixed << std::setprecision(3) << "FRAME n=" << frame_number
				<< " cpu_ms=" << telemetry.cpu_frame_ms << " ops=" << telemetry.draw_ops
				<< " culled=" << telemetry.culled_ops << " batches=" << telemetry.batches
				<< " instances=" << telemetry.instances << " upload=" << telemetry.upload_bytes
				<< " paint_ms=" << telemetry.paint_ms
				<< " texture_ms=" << telemetry.texture_update_ms
				<< " plugin_ms=" << telemetry.plugin_total_ms
				<< " submitted=" << telemetry.submitted_fence
				<< " completed=" << telemetry.completed_fence
				<< " deferred=" << telemetry.deferred_releases << " adapter=\"" << adapter_name
				<< "\"\n";
		}

		void simulateLiveEditorChange(WindowState& state) {
			SendMessageW(state.editor, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
			SendMessageW(state.editor, EM_REPLACESEL, TRUE,
				reinterpret_cast<LPARAM>(L"\r\n<!-- live edit self test -->"));
		}

		void populateRendererSelector(WindowState& state,
			const std::vector<RendererPluginInfo>& plugins) {
			SendMessageW(state.renderer_combo, CB_RESETCONTENT, 0, 0);
			for (const RendererPluginInfo& plugin : plugins) {
				const std::wstring name = widenUtf8(plugin.display_name);
				SendMessageW(state.renderer_combo, CB_ADDSTRING, 0,
					reinterpret_cast<LPARAM>(name.c_str()));
			}
			if (!plugins.empty()) {
				SendMessageW(state.renderer_combo, CB_SETCURSEL, 0, 0);
			}
		}

		void populateExampleSelector(WindowState& state, const bool external_source) {
			SendMessageW(state.example_combo, CB_RESETCONTENT, 0, 0);
			const wchar_t* custom_name = external_source ? L"Custom - external HTML file"
				: L"Custom - edited source";
			SendMessageW(state.example_combo, CB_ADDSTRING, 0,
				reinterpret_cast<LPARAM>(custom_name));
			for (const BuiltinExample& example : builtinExamples()) {
				const std::wstring name = widenUtf8(example.name);
				SendMessageW(state.example_combo, CB_ADDSTRING, 0,
					reinterpret_cast<LPARAM>(name.c_str()));
			}
			SendMessageW(state.example_combo, CB_SETCURSEL, external_source ? 0 : 1, 0);
		}

	}  // namespace

	int runApplication(const ApplicationOptions& options) {
		if (options.width == 0 || options.height == 0) {
			throw std::invalid_argument("application dimensions must be non-zero");
		}

		static_cast<void>(SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));

		const std::span<const BuiltinExample> examples = builtinExamples();
		if (examples.empty()) {
			throw std::runtime_error("no built-in Canvas examples are available");
		}
		const std::string initial_source = options.html_canvas_path.has_value()
			? readTextFile(*options.html_canvas_path)
			: std::string(examples.front().source);
		std::uint64_t next_page_revision = 1;
		std::unique_ptr<JavaScriptPage> page =
			JavaScriptPage::create(initial_source, next_page_revision++);

		WindowState window_state{};
		window_state.initial_width = options.width;
		window_state.initial_height = options.height;
		HWND window = createShellWindow(options, window_state);
		createChildControls(window_state, initial_source);
		populateExampleSelector(window_state, options.html_canvas_path.has_value());

		RECT client{};
		if (GetClientRect(window, &client) == FALSE) {
			DestroyWindow(window);
			throw std::runtime_error("GetClientRect failed");
		}
		layoutChildren(window_state, static_cast<std::uint32_t>(client.right - client.left),
			static_cast<std::uint32_t>(client.bottom - client.top));

		ShowWindow(window, SW_SHOWDEFAULT);
		UpdateWindow(window);

		const std::filesystem::path executable_directory = executableDirectory();
		const std::filesystem::path renderer_directory = executable_directory / L"renderers";
		std::vector<RendererPluginDiagnostic> renderer_diagnostics;
		const std::vector<RendererPluginInfo> renderer_plugins =
			RendererPluginHost::discover(renderer_directory, &renderer_diagnostics);
		if (renderer_plugins.empty()) {
			throw std::runtime_error("no renderer plugins were found in: " +
				renderer_directory.string());
		}
		populateRendererSelector(window_state, renderer_plugins);
		std::size_t initial_renderer_index = 0;
		if (!options.renderer_self_test && !options.example_self_test) {
			const auto skia = std::find_if(renderer_plugins.begin(), renderer_plugins.end(),
				[](const RendererPluginInfo& plugin) {
					return plugin.id == "skia";
				});
			if (skia != renderer_plugins.end()) {
				initial_renderer_index = static_cast<std::size_t>(skia - renderer_plugins.begin());
				SendMessageW(window_state.renderer_combo, CB_SETCURSEL,
					static_cast<WPARAM>(initial_renderer_index), 0);
			}
		}
		RendererPluginHost renderer;
		renderer.load(renderer_plugins[initial_renderer_index], window_state.render_surface,
			window_state.render_width, window_state.render_height, options.vsync,
			options.debug_layer);

		std::cout << "GPU 2D Learning Lab - Chromium-style Canvas Shell\nAdapter: "
			<< renderer.adapterName()
			<< "\nExecutable: " << (executable_directory / L"gpu_2d_demo.exe").string()
			<< "\nRenderer scan directory: " << renderer_directory.string()
			<< "\nLeft: live HTML canvas source editor"
			<< "\nRight: D3D12 compositor surface"
			<< "\nControls: edit HTML/JavaScript for live reparse, F5=force parse, "
			"Space=pause, R=reload renderer, Escape=quit\n";

		std::cout << "Renderer component: " << renderer.rendererName()
			<< "\nDiscovered renderer components: " << renderer_plugins.size() << '\n';
		for (const RendererPluginInfo& plugin : renderer_plugins) {
			std::cout << "  FOUND " << plugin.display_name << " -> " << plugin.path.string() << '\n';
		}
		for (const RendererPluginDiagnostic& diagnostic : renderer_diagnostics) {
			std::cerr << "  SKIPPED " << diagnostic.path.string() << ": " << diagnostic.message << '\n';
		}
		const bool has_skia = std::any_of(renderer_plugins.begin(), renderer_plugins.end(),
			[](const RendererPluginInfo& plugin) {
				return plugin.id == "skia";
			});
		if (has_skia) {
			setStatus(window_state,
				L"Ready: select Direct3D 12 or Skia + D3D12 Compositor from Renderer");
		}
		else {
			setStatus(window_state,
				L"Skia plugin was not built. Set GPU2D_SKIA_ROOT, then rebuild this directory.");
			std::cout << "Skia renderer was not built. Set GPU2D_SKIA_ROOT and reconfigure the same "
				"build directory:\n"
				<< "  cmake --preset default\n  cmake --build --preset debug\n";
		}
		using Clock = std::chrono::steady_clock;
		auto previous = Clock::now();
		auto last_report = previous;
		auto animation_origin = Clock::now();
		std::uint64_t frame_number = 0;
		Gpu2dRendererTelemetry telemetry{};
		std::size_t last_canvas_operation_count{};
		bool live_edit_injected = false;
		bool renderer_switch_injected = false;
		bool d3d12_animation_validated = false;
		bool example_switch_injected = false;
		const auto live_edit_deadline = Clock::now() + std::chrono::seconds(3);
		const auto renderer_test_deadline = Clock::now() + std::chrono::seconds(8);
		const auto example_test_deadline = Clock::now() + std::chrono::seconds(8);

		if (options.renderer_self_test && renderer_plugins.size() < 2U) {
			throw std::runtime_error("RENDERER_SELF_TEST requires at least two renderer plugins");
		}

		MSG message{};
		while (!window_state.quit) {
			while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
				if (message.message == WM_QUIT) {
					window_state.quit = true;
					break;
				}
				if (handleKeyMessage(window_state, message)) {
					continue;
				}
				TranslateMessage(&message);
				DispatchMessageW(&message);
			}
			if (window_state.quit) {
				break;
			}

			if (window_state.render_width == 0 || window_state.render_height == 0) {
				WaitMessage();
				previous = Clock::now();
				continue;
			}

			if (window_state.parse_pending) {
				window_state.parse_pending = false;
				const std::string source = editorText(window_state.editor);
				try {
					std::unique_ptr<JavaScriptPage> candidate =
						JavaScriptPage::create(source, next_page_revision++);
					page = std::move(candidate);
					++window_state.parse_revision;
					std::wostringstream status;
					status << L"JavaScript page " << page->revision() << L" ready | canvas "
						<< page->canvasWidth() << L"x" << page->canvasHeight() << L" | "
						<< widenUtf8(renderer.rendererName());
					setStatus(window_state, status.str());
				}
				catch (const std::exception& error) {
					const std::wstring error_text =
						L"JS/HTML error; keeping previous page: " + widenUtf8(error.what());
					setStatus(window_state, error_text);
					std::cerr << "JavaScript page error: " << error.what() << '\n';
				}
			}

			if (window_state.example_switch_pending) {
				window_state.example_switch_pending = false;
				const LRESULT selected = SendMessageW(window_state.example_combo, CB_GETCURSEL, 0, 0);
				if (selected > 0 && static_cast<std::size_t>(selected) <= examples.size()) {
					const BuiltinExample& example = examples[static_cast<std::size_t>(selected) - 1U];
					window_state.suppress_editor_change = true;
					const std::wstring source = widenUtf8(example.source);
					SetWindowTextW(window_state.editor, source.c_str());
					window_state.suppress_editor_change = false;
					KillTimer(window_state.shell, kLiveParseTimerId);
					window_state.parse_pending = true;
					window_state.paused = false;
					animation_origin = Clock::now();
					const auto skia = std::find_if(renderer_plugins.begin(), renderer_plugins.end(),
						[](const RendererPluginInfo& plugin) { return plugin.id == "skia"; });
					if (example.requires_skia && skia != renderer_plugins.end()) {
						const std::size_t skia_index = static_cast<std::size_t>(skia - renderer_plugins.begin());
						const LRESULT active_renderer =
							SendMessageW(window_state.renderer_combo, CB_GETCURSEL, 0, 0);
						if (active_renderer == CB_ERR || static_cast<std::size_t>(active_renderer) != skia_index) {
							SendMessageW(window_state.renderer_combo, CB_SETCURSEL,
								static_cast<WPARAM>(skia_index), 0);
							window_state.renderer_switch_pending = true;
						}
					}
					setStatus(window_state, L"Loading example: " + widenUtf8(example.name));
				}
			}

			if (window_state.renderer_switch_pending) {
				window_state.renderer_switch_pending = false;
				const LRESULT selected = SendMessageW(window_state.renderer_combo, CB_GETCURSEL, 0, 0);
				if (selected != CB_ERR && static_cast<std::size_t>(selected) < renderer_plugins.size()) {
					renderer.load(renderer_plugins[static_cast<std::size_t>(selected)],
						window_state.render_surface, window_state.render_width,
						window_state.render_height, options.vsync, options.debug_layer);
					++window_state.renderer_revision;
					page->requestSurfaceReset();
					setStatus(window_state, L"Renderer switched to " + widenUtf8(renderer.rendererName()));
					std::cout << "RENDERER switched component=\"" << renderer.rendererName()
						<< "\" adapter=\"" << renderer.adapterName() << "\"\n";
				}
			}

			if (window_state.resize_pending) {
				renderer.resize(window_state.render_width, window_state.render_height);
				try {
					page->resizeViewport(window_state.render_width, window_state.render_height);
				}
				catch (const std::exception& error) {
					setStatus(window_state, L"JavaScript resize listener failed: " + widenUtf8(error.what()));
					std::cerr << "JavaScript resize listener error: " << error.what() << '\n';
				}
				window_state.resize_pending = false;
			}

			if (options.reload_at_frame != 0 && frame_number == options.reload_at_frame) {
				window_state.reload_texture = true;
			}

			if (window_state.reload_texture) {
				window_state.reload_texture = false;
				const LRESULT selected = SendMessageW(window_state.renderer_combo, CB_GETCURSEL, 0, 0);
				const std::size_t index =
					selected == CB_ERR ? 0U : static_cast<std::size_t>(selected);
				renderer.load(renderer_plugins.at(index), window_state.render_surface,
					window_state.render_width, window_state.render_height, options.vsync,
					options.debug_layer);
				page->requestSurfaceReset();
				std::cout << "RENDERER component reloaded: " << renderer.rendererName() << '\n';
			}

			if (window_state.pointer_move_pending) {
				window_state.pointer_move_pending = false;
				try {
					const float canvas_x = window_state.pointer_x *
						static_cast<float>(page->canvasWidth()) /
						static_cast<float>(window_state.render_width);
					const float canvas_y = window_state.pointer_y *
						static_cast<float>(page->canvasHeight()) /
						static_cast<float>(window_state.render_height);
					page->pointerMove(canvas_x, canvas_y);
				} catch (const std::exception& error) {
					setStatus(window_state, L"JavaScript pointer handler failed: " + widenUtf8(error.what()));
					std::cerr << "JavaScript pointer handler error: " << error.what() << '\n';
				}
			}

			const auto now = Clock::now();
			previous = now;
			CanvasFrame canvas_frame;
			if (!window_state.paused && !page->faulted()) {
				const double timestamp_ms =
					std::chrono::duration<double, std::milli>(now - animation_origin).count();
				try {
					canvas_frame = page->runAnimationFrame(timestamp_ms);
				}
				catch (const std::exception& error) {
					setStatus(window_state, L"JavaScript runtime stopped: " + widenUtf8(error.what()));
					std::cerr << "JavaScript runtime error: " << error.what() << '\n';
					window_state.paused = true;
					canvas_frame.page_revision = page->revision();
				}
			}
			else {
				canvas_frame.page_revision = page->revision();
			}
			try {
				telemetry = renderer.render(canvas_frame, window_state.render_width,
					window_state.render_height, page->canvasWidth(),
					page->canvasHeight());
				last_canvas_operation_count = canvas_frame.operations.size();
			}
			catch (const std::exception& error) {
				setStatus(window_state, L"Renderer capability/error: " + widenUtf8(error.what()));
				std::cerr << "Renderer Canvas error: " << error.what() << '\n';
			}
			++frame_number;

			if (options.renderer_self_test && !renderer_switch_injected && frame_number >= 6U) {
				if (renderer_plugins[initial_renderer_index].id == "d3d12") {
					if (telemetry.instances < 2U || telemetry.culled_ops != 0U) {
						throw std::runtime_error(
							"RENDERER_SELF_TEST failed: D3D12 did not draw the animated background and pulse");
					}
					d3d12_animation_validated = true;
					std::cout << "D3D12_ANIMATION PASS frames=" << frame_number
						<< " instances=" << telemetry.instances << '\n';
				}
				SendMessageW(window_state.renderer_combo, CB_SETCURSEL,
					initial_renderer_index == 0U ? 1 : 0, 0);
				window_state.renderer_switch_pending = true;
				renderer_switch_injected = true;
			}
			else if (options.renderer_self_test && window_state.renderer_revision != 0 &&
				d3d12_animation_validated) {
				std::cout << "RENDERER_SELF_TEST PASS component=\"" << renderer.rendererName()
					<< "\" revision=" << window_state.renderer_revision << '\n';
				break;
			}
			if (options.renderer_self_test && Clock::now() >= renderer_test_deadline) {
				throw std::runtime_error("RENDERER_SELF_TEST failed: dynamic switch did not complete");
			}

			if (options.example_self_test && !example_switch_injected) {
				SendMessageW(window_state.example_combo, CB_SETCURSEL,
					static_cast<WPARAM>(examples.size()), 0);
				window_state.example_switch_pending = true;
				example_switch_injected = true;
			}
			else if (options.example_self_test && window_state.parse_revision != 0 &&
				renderer.rendererName() == "Skia Ganesh D3D12 GPU" && frame_number >= 8U) {
				std::cout << "EXAMPLE_SELF_TEST PASS selected=\""
					<< examples.back().name << "\" revision=" << window_state.parse_revision << '\n';
				break;
			}
			if (options.example_self_test && Clock::now() >= example_test_deadline) {
				throw std::runtime_error("EXAMPLE_SELF_TEST failed: dropdown did not reload the page");
			}

			if (options.live_edit_self_test && !live_edit_injected) {
				simulateLiveEditorChange(window_state);
				live_edit_injected = true;
			}
			if (options.live_edit_self_test && window_state.parse_revision != 0) {
				std::cout << "LIVE_EDIT_SELF_TEST PASS revision=" << window_state.parse_revision << '\n';
				break;
			}
			if (options.live_edit_self_test && Clock::now() >= live_edit_deadline) {
				throw std::runtime_error("LIVE_EDIT_SELF_TEST failed: EN_CHANGE did not produce a live reparse");
			}

			if (now - last_report >= std::chrono::seconds(1)) {
				printTelemetry(telemetry, frame_number, renderer.adapterName());
				const JavaScriptPageStats& stats = page->stats();
				std::cout << std::fixed << std::setprecision(3)
					<< "JS page_revision=" << page->revision()
					<< " js_init_ms=" << stats.initialization_ms
					<< " js_frame_ms=" << stats.frame_ms
					<< " raf_callbacks=" << stats.raf_callbacks
					<< " canvas_operations=" << last_canvas_operation_count
					<< " renderer_name=\"" << renderer.rendererName() << "\"\n";
				last_report = now;
			}

			if (options.max_frames != 0 && frame_number >= options.max_frames) {
				std::cout << "Reached requested frame limit: " << options.max_frames << '\n';
				break;
			}
		}

		printTelemetry(telemetry, frame_number, renderer.adapterName());
		renderer.unload();
		if (window_state.editor_font != nullptr) {
			DeleteObject(window_state.editor_font);
		}
		if (IsWindow(window) != FALSE) {
			DestroyWindow(window);
		}
		return 0;
	}

}  // namespace gpu2d
