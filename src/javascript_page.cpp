#include "gpu2d/javascript_page.hpp"

#include <quickjs.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gpu2d {
	namespace {

		using Clock = std::chrono::steady_clock;

		struct HtmlScriptEnvelope {
			std::string canvas_id;
			std::uint32_t width{ 960 };
			std::uint32_t height{ 540 };
			std::string style;
			std::string script;
		};

		[[nodiscard]] std::string toLower(std::string text) {
			std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char value) {
				return static_cast<char>(std::tolower(value));
				});
			return text;
		}

		[[nodiscard]] std::string attributeValue(const std::string& tag, const std::string& name) {
			const std::string lowered = toLower(tag);
			const std::string key = toLower(name) + "=";
			const std::size_t position = lowered.find(key);
			if (position == std::string::npos) {
				return {};
			}
			std::size_t start = position + key.size();
			if (start >= tag.size()) {
				return {};
			}
			const char quote = tag[start];
			if (quote == '\'' || quote == '"') {
				++start;
				const std::size_t end = tag.find(quote, start);
				return end == std::string::npos ? std::string{} : tag.substr(start, end - start);
			}
			std::size_t end = start;
			while (end < tag.size() && !std::isspace(static_cast<unsigned char>(tag[end])) &&
				tag[end] != '>') {
				++end;
			}
			return tag.substr(start, end - start);
		}

		[[nodiscard]] std::uint32_t dimension(const std::string& value, const std::uint32_t fallback) {
			if (value.empty()) {
				return fallback;
			}
			std::uint32_t result{};
			const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
			if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || result == 0U ||
				result > 8192U) {
				throw std::invalid_argument("canvas dimensions must be integers in [1, 8192]");
			}
			return result;
		}

		[[nodiscard]] HtmlScriptEnvelope parseEnvelope(const std::string& html) {
			const std::string lowered = toLower(html);
			const std::size_t canvas_start = lowered.find("<canvas");
			if (canvas_start == std::string::npos) {
				throw std::invalid_argument("HTML must contain a <canvas> element");
			}
			const std::size_t canvas_end = lowered.find('>', canvas_start);
			if (canvas_end == std::string::npos) {
				throw std::invalid_argument("unterminated <canvas> element");
			}
			const std::string canvas_tag = html.substr(canvas_start, canvas_end - canvas_start + 1U);

			const std::size_t script_start = lowered.find("<script", canvas_end);
			if (script_start == std::string::npos) {
				throw std::invalid_argument("HTML must contain an ordinary <script> element");
			}
			const std::size_t script_body = lowered.find('>', script_start);
			const std::size_t script_end = lowered.find("</script>", script_body);
			if (script_body == std::string::npos || script_end == std::string::npos) {
				throw std::invalid_argument("unterminated <script> element");
			}

			HtmlScriptEnvelope result;
			result.canvas_id = attributeValue(canvas_tag, "id");
			result.width = dimension(attributeValue(canvas_tag, "width"), result.width);
			result.height = dimension(attributeValue(canvas_tag, "height"), result.height);
			const std::size_t style_start = lowered.find("<style");
			if (style_start != std::string::npos) {
				const std::size_t style_body = lowered.find('>', style_start);
				const std::size_t style_end = lowered.find("</style>", style_body);
				if (style_body == std::string::npos || style_end == std::string::npos) {
					throw std::invalid_argument("unterminated <style> element");
				}
				result.style = html.substr(style_body + 1U, style_end - style_body - 1U);
			}
			result.script = html.substr(script_body + 1U, script_end - script_body - 1U);
			return result;
		}

		[[nodiscard]] std::string jsQuoted(const std::string& text) {
			std::string result{ "\"" };
			for (const unsigned char value : text) {
				switch (value) {
				case '\\': result += "\\\\"; break;
				case '"': result += "\\\""; break;
				case '\n': result += "\\n"; break;
				case '\r': result += "\\r"; break;
				case '\t': result += "\\t"; break;
				default:
					if (value < 0x20U) {
						result += " ";
					}
					else {
						result.push_back(static_cast<char>(value));
					}
					break;
				}
			}
			result += '"';
			return result;
		}

		[[nodiscard]] Color parseColor(std::string text) {
			text = toLower(std::move(text));
			const auto byte = [&](const std::size_t index) -> float {
				const auto digit = [](const char value) -> int {
					if (value >= '0' && value <= '9') return value - '0';
					if (value >= 'a' && value <= 'f') return value - 'a' + 10;
					return -1;
					};
				const int high = digit(text[index]);
				const int low = digit(text[index + 1U]);
				if (high < 0 || low < 0) {
					throw std::invalid_argument("invalid hexadecimal canvas color");
				}
				return static_cast<float>(high * 16 + low) / 255.0F;
				};
			if (text.size() == 7U && text[0] == '#') {
				return Color::fromStraight(byte(1), byte(3), byte(5));
			}
			if (text.size() == 4U && text[0] == '#') {
				std::string expanded{ "#" };
				for (std::size_t index = 1; index < 4U; ++index) {
					expanded.push_back(text[index]);
					expanded.push_back(text[index]);
				}
				return parseColor(expanded);
			}
			constexpr std::array<std::pair<std::string_view, Color>, 10> named{ {
				{"black", Color::fromStraight(0, 0, 0)}, {"white", Color::fromStraight(1, 1, 1)},
				{"red", Color::fromStraight(1, 0, 0)}, {"green", Color::fromStraight(0, 0.5F, 0)},
				{"blue", Color::fromStraight(0, 0, 1)}, {"yellow", Color::fromStraight(1, 1, 0)},
				{"cyan", Color::fromStraight(0, 1, 1)}, {"magenta", Color::fromStraight(1, 0, 1)},
				{"transparent", Color::fromStraight(0, 0, 0, 0)},
				{"grey", Color::fromStraight(0.5F, 0.5F, 0.5F)},
			} };
			for (const auto& [name, color] : named) {
				if (text == name) return color;
			}
			const bool rgba = text.rfind("rgba(", 0) == 0;
			const bool rgb = text.rfind("rgb(", 0) == 0;
			if (rgba || rgb) {
				const std::size_t begin = text.find('(') + 1U;
				const std::size_t end = text.find(')', begin);
				if (end == std::string::npos) throw std::invalid_argument("unterminated rgb canvas color");
				std::string components = text.substr(begin, end - begin);
				std::replace(components.begin(), components.end(), ',', ' ');
				std::istringstream stream(components);
				float red{}, green{}, blue{}, alpha{ 1.0F };
				if (!(stream >> red >> green >> blue) || (rgba && !(stream >> alpha))) {
					throw std::invalid_argument("invalid rgb canvas color");
				}
				return Color::fromStraight(std::clamp(red / 255.0F, 0.0F, 1.0F),
					std::clamp(green / 255.0F, 0.0F, 1.0F),
					std::clamp(blue / 255.0F, 0.0F, 1.0F),
					std::clamp(alpha, 0.0F, 1.0F));
			}
  const bool hsla = text.rfind("hsla(", 0) == 0;
  const bool hsl = text.rfind("hsl(", 0) == 0;
  if (hsla || hsl) {
    const std::size_t begin = text.find('(') + 1U;
    const std::size_t end = text.find(')', begin);
    if (end == std::string::npos) throw std::invalid_argument("unterminated hsl canvas color");
    std::string components = text.substr(begin, end - begin);
    std::replace(components.begin(), components.end(), ',', ' ');
    std::istringstream stream(components);
    std::string hue_text;
    std::string saturation_text;
    std::string lightness_text;
    std::string alpha_text;
    if (!(stream >> hue_text >> saturation_text >> lightness_text) ||
        (hsla && !(stream >> alpha_text))) {
      throw std::invalid_argument("invalid hsl canvas color");
    }
    const auto number = [](const std::string& value) {
      std::size_t consumed{};
      const float result = std::stof(value, &consumed);
      if (consumed != value.size() || !std::isfinite(result)) {
        throw std::invalid_argument("invalid numeric color component");
      }
      return result;
    };
    const auto percentage = [&](const std::string& value) {
      if (value.empty() || value.back() != '%') {
        throw std::invalid_argument("HSL saturation and lightness must be percentages");
      }
      return std::clamp(number(value.substr(0, value.size() - 1U)) / 100.0F, 0.0F, 1.0F);
    };
    float hue = std::fmod(number(hue_text), 360.0F);
    if (hue < 0.0F) hue += 360.0F;
    const float saturation = percentage(saturation_text);
    const float lightness = percentage(lightness_text);
    float alpha = 1.0F;
    if (hsla) {
      alpha = !alpha_text.empty() && alpha_text.back() == '%'
                  ? percentage(alpha_text)
                  : std::clamp(number(alpha_text), 0.0F, 1.0F);
    }
    const float chroma = (1.0F - std::abs(2.0F * lightness - 1.0F)) * saturation;
    const float section = hue / 60.0F;
    const float secondary = chroma * (1.0F - std::abs(std::fmod(section, 2.0F) - 1.0F));
    float red{};
    float green{};
    float blue{};
    if (section < 1.0F) { red = chroma; green = secondary; }
    else if (section < 2.0F) { red = secondary; green = chroma; }
    else if (section < 3.0F) { green = chroma; blue = secondary; }
    else if (section < 4.0F) { green = secondary; blue = chroma; }
    else if (section < 5.0F) { red = secondary; blue = chroma; }
    else { red = chroma; blue = secondary; }
    const float match = lightness - chroma * 0.5F;
    return Color::fromStraight(red + match, green + match, blue + match, alpha);
  }
			throw std::invalid_argument("unsupported canvas color: " + text);
		}

	}  // namespace

	struct JavaScriptPage::Impl {
		struct RadialGradient {
			std::array<float, 6> geometry{};
			std::vector<CanvasGradientStop> stops;
		};
		struct RafCallback {
			std::uint32_t id{};
			JSValue function{ JS_UNDEFINED };
		};

		JSRuntime* runtime{};
		JSContext* context{};
		HtmlScriptEnvelope envelope;
		std::uint64_t revision{};
		bool reset_surface{ true };
		bool deadline_active{};
		bool timed_out{};
		bool faulted{};
		Clock::time_point deadline{};
		std::uint32_t next_raf_id{ 1 };
		std::uint32_t active_raf_id{};
		std::vector<RafCallback> pending_callbacks;
		std::vector<RafCallback> executing_callbacks;
		std::vector<CanvasFrameOperation> operations;
		std::vector<CanvasPathSegment> frame_path_segments;
		std::vector<CanvasPathSegment> current_path;
		std::vector<CanvasGradientStop> frame_gradient_stops;
		std::vector<RadialGradient> gradients;
		bool has_background_color{};
		Color background_color{};
		std::uint64_t pending_capabilities{CanvasCapabilityRectangles};
		JavaScriptPageStats stats;

		Impl(HtmlScriptEnvelope parsed, const std::uint64_t page_revision)
			: envelope(std::move(parsed)), revision(page_revision) {
			const std::string lowered_style = toLower(envelope.style);
			const std::string selector = envelope.canvas_id.empty() ? "canvas" : "#" + toLower(envelope.canvas_id);
			const std::size_t selector_position = lowered_style.find(selector);
			if (selector_position != std::string::npos) {
				const std::size_t block_start = lowered_style.find('{', selector_position);
				const std::size_t block_end = lowered_style.find('}', block_start);
				if (block_start != std::string::npos && block_end != std::string::npos) {
					const std::string block = envelope.style.substr(block_start + 1U, block_end - block_start - 1U);
					const std::string lowered_block = toLower(block);
					for (const std::string property : {std::string("background-color"), std::string("background")}) {
						const std::size_t property_position = lowered_block.find(property + ":");
						if (property_position == std::string::npos) continue;
						const std::size_t value_start = property_position + property.size() + 1U;
						const std::size_t value_end = block.find(';', value_start);
						std::string value = block.substr(value_start, value_end - value_start);
						value.erase(0, value.find_first_not_of(" \t\r\n"));
						value.erase(value.find_last_not_of(" \t\r\n") + 1U);
						background_color = parseColor(value);
						has_background_color = true;
						break;
					}
				}
			}
			runtime = JS_NewRuntime();
			if (runtime == nullptr) throw std::runtime_error("JS_NewRuntime failed");
			JS_SetMemoryLimit(runtime, 64U * 1024U * 1024U);
			JS_SetMaxStackSize(runtime, 2U * 1024U * 1024U);
			JS_SetInterruptHandler(runtime, interrupt, this);
			context = JS_NewContext(runtime);
			if (context == nullptr) throw std::runtime_error("JS_NewContext failed");
			JS_SetContextOpaque(context, this);
		}

		~Impl() {
			if (context != nullptr) {
				for (RafCallback& callback : pending_callbacks) {
					JS_FreeValue(context, callback.function);
				}
				for (RafCallback& callback : executing_callbacks) {
					JS_FreeValue(context, callback.function);
				}
				JS_FreeContext(context);
			}
			if (runtime != nullptr) JS_FreeRuntime(runtime);
		}

		static int interrupt(JSRuntime*, void* opaque) {
			auto& page = *static_cast<Impl*>(opaque);
			if (page.deadline_active && Clock::now() >= page.deadline) {
				page.timed_out = true;
				return 1;
			}
			return 0;
		}

		[[nodiscard]] std::string exceptionText() {
			JSValue exception = JS_GetException(context);
			JSValue stack = JS_GetPropertyStr(context, exception, "stack");
			const char* stack_text = JS_ToCString(context, stack);
			const char* exception_text = JS_ToCString(context, exception);
			std::string result = stack_text != nullptr ? stack_text
				: exception_text != nullptr ? exception_text
				: "unknown JavaScript exception";
			if (stack_text != nullptr) JS_FreeCString(context, stack_text);
			if (exception_text != nullptr) JS_FreeCString(context, exception_text);
			JS_FreeValue(context, stack);
			JS_FreeValue(context, exception);
			if (timed_out) result = "JavaScript execution timed out";
			return result;
		}

		void evaluate(const std::string& source, const char* filename,
			const std::chrono::milliseconds budget) {
			timed_out = false;
			deadline = Clock::now() + budget;
			deadline_active = true;
			JSValue result = JS_Eval(context, source.c_str(), source.size(), filename, JS_EVAL_TYPE_GLOBAL);
			deadline_active = false;
			if (JS_IsException(result)) {
				throw std::runtime_error(exceptionText());
			}
			JS_FreeValue(context, result);
		}

		[[nodiscard]] static Impl& self(JSContext* context) {
			return *static_cast<Impl*>(JS_GetContextOpaque(context));
		}

		[[nodiscard]] static std::string stringArg(JSContext* context, const JSValueConst value) {
			const char* text = JS_ToCString(context, value);
			if (text == nullptr) throw std::invalid_argument("expected string argument");
			std::string result(text);
			JS_FreeCString(context, text);
			return result;
		}

		[[nodiscard]] static double numberArg(JSContext* context, const JSValueConst value) {
			double result{};
			if (JS_ToFloat64(context, &result, value) < 0 || !std::isfinite(result)) {
				throw std::invalid_argument("expected finite numeric argument");
			}
			return result;
		}

		[[nodiscard]] static JSValue nativeCall(JSContext* context, JSValueConst, const int argc,
			JSValueConst* argv) {
			if (argc < 1) return JS_ThrowTypeError(context, "native command is required");
			try {
				Impl& page = self(context);
				const std::string command = stringArg(context, argv[0]);
				if (command == "raf") {
					if (argc < 2 || !JS_IsFunction(context, argv[1])) {
						return JS_ThrowTypeError(context, "requestAnimationFrame requires a function");
					}
					const std::uint32_t id = page.next_raf_id++;
					page.pending_callbacks.push_back(RafCallback{ id, JS_DupValue(context, argv[1]) });
					return JS_NewInt32(context, static_cast<std::int32_t>(id));
				}
				if (command == "cancelRaf") {
					if (argc < 2) return JS_ThrowTypeError(context, "RAF id is required");
					const auto id = static_cast<std::uint32_t>(numberArg(context, argv[1]));
					if (id == page.active_raf_id) return JS_UNDEFINED;
					const auto cancel = [&](std::vector<RafCallback>& callbacks, const bool erase) {
						auto found = std::find_if(callbacks.begin(), callbacks.end(),
							[id](const RafCallback& callback) { return callback.id == id; });
						if (found == callbacks.end()) return;
						JS_FreeValue(context, found->function);
						found->function = JS_UNDEFINED;
						if (erase) callbacks.erase(found);
						};
					cancel(page.pending_callbacks, true);
					cancel(page.executing_callbacks, false);
					return JS_UNDEFINED;
				}
				if (command == "console") {
					if (argc >= 3) {
						std::ostream& output = stringArg(context, argv[1]) == "error" ? std::cerr : std::cout;
						output << "[JS page " << page.revision << "] " << stringArg(context, argv[2]) << '\n';
					}
					return JS_UNDEFINED;
				}
				if (command == "materializeRadialGradient") {
					if (argc < 8) return JS_ThrowTypeError(context, "radial gradient data is incomplete");
					RadialGradient gradient;
					for (int index = 0; index < 6; ++index) {
						gradient.geometry[static_cast<std::size_t>(index)] =
							static_cast<float>(numberArg(context, argv[index + 1]));
					}
					if (gradient.geometry[2] < 0.0F || gradient.geometry[5] < 0.0F) {
						return JS_ThrowRangeError(context, "radial gradient radius must be non-negative");
					}
					const auto stop_count = static_cast<std::size_t>(numberArg(context, argv[7]));
					if (stop_count < 2U || stop_count > 64U ||
						argc < static_cast<int>(8U + stop_count * 2U)) {
						return JS_ThrowRangeError(context, "radial gradient requires 2 to 64 color stops");
					}
					for (std::size_t index = 0; index < stop_count; ++index) {
						const float offset = static_cast<float>(numberArg(context, argv[8U + index * 2U]));
						if (offset < 0.0F || offset > 1.0F) {
							return JS_ThrowRangeError(context, "gradient stop offset must be in [0, 1]");
						}
						gradient.stops.push_back(CanvasGradientStop{
							offset, parseColor(stringArg(context, argv[9U + index * 2U]))});
					}
					page.gradients.push_back(std::move(gradient));
					return JS_NewInt32(context, static_cast<std::int32_t>(page.gradients.size() - 1U));
				}
      if (command == "setCanvasWidth" || command == "setCanvasHeight") {
        if (argc < 2) return JS_ThrowTypeError(context, "canvas dimension is required");
        const double requested = numberArg(context, argv[1]);
        if (requested < 1.0 || requested > 8192.0) {
          return JS_ThrowRangeError(context, "canvas dimensions must be in [1, 8192]");
        }
        const auto value = static_cast<std::uint32_t>(requested);
        if (command == "setCanvasWidth") page.envelope.width = value;
        else page.envelope.height = value;
        page.operations.clear();
        page.frame_path_segments.clear();
        page.frame_gradient_stops.clear();
        page.current_path.clear();
        page.reset_surface = true;
        return JS_NewInt32(context, static_cast<std::int32_t>(value));
      }
				if (command == "beginPath") {
					page.current_path.clear();
					return JS_UNDEFINED;
				}
				if (command == "moveTo" || command == "lineTo") {
					if (argc < 3) return JS_ThrowTypeError(context, "%s requires x and y", command.c_str());
					page.current_path.push_back(CanvasPathSegment{
						command == "moveTo" ? CanvasPathVerb::MoveTo : CanvasPathVerb::LineTo,
						Vec2{static_cast<float>(numberArg(context, argv[1])),
							 static_cast<float>(numberArg(context, argv[2]))} });
					return JS_UNDEFINED;
				}
				if (command == "closePath") {
					page.current_path.push_back(CanvasPathSegment{ CanvasPathVerb::Close, {} });
					return JS_UNDEFINED;
				}
				if (command == "arc") {
					if (argc < 7) return JS_ThrowTypeError(context, "arc requires six arguments");
					page.pending_capabilities |= CanvasCapabilityArcs | CanvasCapabilityPaths;
					const double cx = numberArg(context, argv[1]);
					const double cy = numberArg(context, argv[2]);
					const double radius = numberArg(context, argv[3]);
					double start = numberArg(context, argv[4]);
					double end = numberArg(context, argv[5]);
					const bool anticlockwise = JS_ToBool(context, argv[6]) != 0;
					if (radius < 0.0) return JS_ThrowRangeError(context, "arc radius must be non-negative");
					constexpr double tau = 6.28318530717958647692;
					if (!anticlockwise) {
						while (end < start) end += tau;
						end = std::min(end, start + tau);
					}
					else {
						while (end > start) end -= tau;
						end = std::max(end, start - tau);
					}
					const int segments = std::max(1, static_cast<int>(std::ceil(std::abs(end - start) / tau * 48.0)));
					for (int index = 0; index <= segments; ++index) {
						const double angle = start + (end - start) * static_cast<double>(index) / segments;
						page.current_path.push_back(CanvasPathSegment{
							index == 0 && page.current_path.empty() ? CanvasPathVerb::MoveTo
																	 : CanvasPathVerb::LineTo,
							Vec2{static_cast<float>(cx + std::cos(angle) * radius),
								 static_cast<float>(cy + std::sin(angle) * radius)} });
					}
					return JS_UNDEFINED;
				}
				page.recordDraw(context, command, argc, argv);
				return JS_UNDEFINED;
			}
			catch (const std::exception& error) {
				return JS_ThrowTypeError(context, "%s", error.what());
			}
		}

		[[nodiscard]] CanvasPaint paintFrom(JSContext* ctx, const int argc, JSValueConst* argv,
			const int style_index, const int alpha_index,
			const int shadow_index, const int blur_index,
			const int offset_x_index, const int offset_y_index,
			const int composite_index, const int width_index) {
			if (argc <= composite_index) throw std::invalid_argument("incomplete canvas paint arguments");
			CanvasPaint paint;
			const std::string style = stringArg(ctx, argv[style_index]);
			if (style.rfind("@radial:", 0) == 0) {
				const std::size_t id = static_cast<std::size_t>(std::stoul(style.substr(8)));
				if (id >= gradients.size() || gradients[id].stops.size() < 2U) {
					throw std::invalid_argument("radial gradient requires at least two color stops");
				}
				RadialGradient gradient = gradients[id];
				std::stable_sort(gradient.stops.begin(), gradient.stops.end(),
					[](const CanvasGradientStop& left, const CanvasGradientStop& right) {
						return left.offset < right.offset;
					});
				paint.kind = CanvasPaintKind::RadialGradient;
				std::copy(gradient.geometry.begin(), gradient.geometry.end(), std::begin(paint.radial_gradient));
				paint.first_gradient_stop = static_cast<std::uint32_t>(frame_gradient_stops.size());
				paint.gradient_stop_count = static_cast<std::uint32_t>(gradient.stops.size());
				frame_gradient_stops.insert(frame_gradient_stops.end(), gradient.stops.begin(), gradient.stops.end());
				pending_capabilities |= CanvasCapabilityGradients;
			} else {
				paint.color = parseColor(style);
			}
			const float alpha = std::clamp(static_cast<float>(numberArg(ctx, argv[alpha_index])), 0.0F, 1.0F);
			paint.color = Color{ paint.color.r * alpha, paint.color.g * alpha, paint.color.b * alpha,
								paint.color.a * alpha };
			if (paint.kind == CanvasPaintKind::RadialGradient) {
				for (std::uint32_t index = 0; index < paint.gradient_stop_count; ++index) {
					CanvasGradientStop& stop = frame_gradient_stops[paint.first_gradient_stop + index];
					stop.color = Color{stop.color.r * alpha, stop.color.g * alpha,
						stop.color.b * alpha, stop.color.a * alpha};
				}
			}
			paint.shadow_color = parseColor(stringArg(ctx, argv[shadow_index]));
			paint.shadow_blur = std::max(0.0F, static_cast<float>(numberArg(ctx, argv[blur_index])));
			paint.shadow_offset_x = static_cast<float>(numberArg(ctx, argv[offset_x_index]));
			paint.shadow_offset_y = static_cast<float>(numberArg(ctx, argv[offset_y_index]));
			const std::string composite = stringArg(ctx, argv[composite_index]);
			if (composite == "source-over") paint.composite = CanvasCompositeMode::SourceOver;
			else if (composite == "lighter") paint.composite = CanvasCompositeMode::Lighter;
			else throw std::invalid_argument("unsupported globalCompositeOperation: " + composite);
			if (width_index >= 0) paint.line_width = std::max(0.0F, static_cast<float>(numberArg(ctx, argv[width_index])));
			return paint;
		}

		[[nodiscard]] Mat3x2 transformFrom(JSContext* ctx, JSValueConst* argv, const int index) {
			return Mat3x2{ static_cast<float>(numberArg(ctx, argv[index + 0])),
						  static_cast<float>(numberArg(ctx, argv[index + 1])),
						  static_cast<float>(numberArg(ctx, argv[index + 2])),
						  static_cast<float>(numberArg(ctx, argv[index + 3])),
						  static_cast<float>(numberArg(ctx, argv[index + 4])),
						  static_cast<float>(numberArg(ctx, argv[index + 5])) };
		}

		void recordDraw(JSContext* ctx, const std::string& command, const int argc, JSValueConst* argv) {
			CanvasFrameOperation operation;
			if (command == "clearRect" || command == "fillRect" || command == "strokeRect") {
				const int required = command == "clearRect" ? 11 : command == "fillRect" ? 18 : 19;
				if (argc < required) throw std::invalid_argument("incomplete rectangle arguments");
				operation.kind = command == "clearRect" ? CanvasOperationKind::ClearRect
					: command == "fillRect" ? CanvasOperationKind::FillRect
					: CanvasOperationKind::StrokeRect;
				float x = static_cast<float>(numberArg(ctx, argv[1]));
				float y = static_cast<float>(numberArg(ctx, argv[2]));
				float width = static_cast<float>(numberArg(ctx, argv[3]));
				float height = static_cast<float>(numberArg(ctx, argv[4]));
				if (width < 0) { x += width; width = -width; }
				if (height < 0) { y += height; height = -height; }
				operation.rectangle = Rect::fromXYWH(x, y, width, height);
				if (operation.rectangle.empty()) return;
				if (command != "clearRect") {
					operation.paint = paintFrom(ctx, argc, argv, 5, 6, 7, 8, 9, 10, 11,
						command == "strokeRect" ? 12 : -1);
					operation.transform = transformFrom(ctx, argv, command == "strokeRect" ? 13 : 12);
				}
				else {
					operation.transform = transformFrom(ctx, argv, 5);
				}
			}
			else if (command == "fill" || command == "stroke") {
				const int required = command == "fill" ? 14 : 15;
				if (argc < required) throw std::invalid_argument("incomplete path paint arguments");
				if (current_path.empty()) return;
				operation.kind = command == "fill" ? CanvasOperationKind::FillPath
					: CanvasOperationKind::StrokePath;
				operation.first_path_segment = static_cast<std::uint32_t>(frame_path_segments.size());
				operation.path_segment_count = static_cast<std::uint32_t>(current_path.size());
				frame_path_segments.insert(frame_path_segments.end(), current_path.begin(), current_path.end());
				operation.paint = paintFrom(ctx, argc, argv, 1, 2, 3, 4, 5, 6, 7,
					command == "stroke" ? 8 : -1);
				operation.transform = transformFrom(ctx, argv, command == "stroke" ? 9 : 8);
			}
			else {
				throw std::invalid_argument("unsupported native canvas command: " + command);
			}
			operations.push_back(operation);
		}

		void installBindings() {
			JSValue global = JS_GetGlobalObject(context);
			JS_SetPropertyStr(context, global, "__nativeCall",
				JS_NewCFunction(context, nativeCall, "__nativeCall", 1));
			JS_FreeValue(context, global);

			const std::string bootstrap = R"JS(
(() => {
  const clone = s => ({...s, transform: [...s.transform]});
  class Canvas2DContext {
    constructor() { this._stack = []; this._reset(); }
    _reset() {
      this._state = { fillStyle:'#000000', strokeStyle:'#000000', lineWidth:1,
        globalAlpha:1, shadowColor:'transparent', shadowBlur:0, shadowOffsetX:0,
        shadowOffsetY:0, globalCompositeOperation:'source-over', transform:[1,0,0,1,0,0] };
      this._stack.length = 0;
    }
    get fillStyle(){return this._state.fillStyle} set fillStyle(v){this._state.fillStyle=v&&v.__gradient?v:String(v)}
    get strokeStyle(){return this._state.strokeStyle} set strokeStyle(v){this._state.strokeStyle=v&&v.__gradient?v:String(v)}
    get lineWidth(){return this._state.lineWidth} set lineWidth(v){this._state.lineWidth=Number(v)}
    get globalAlpha(){return this._state.globalAlpha} set globalAlpha(v){this._state.globalAlpha=Number(v)}
    get shadowColor(){return this._state.shadowColor} set shadowColor(v){this._state.shadowColor=String(v)}
    get shadowBlur(){return this._state.shadowBlur} set shadowBlur(v){this._state.shadowBlur=Number(v)}
    get shadowOffsetX(){return this._state.shadowOffsetX} set shadowOffsetX(v){this._state.shadowOffsetX=Number(v)}
    get shadowOffsetY(){return this._state.shadowOffsetY} set shadowOffsetY(v){this._state.shadowOffsetY=Number(v)}
    get globalCompositeOperation(){return this._state.globalCompositeOperation}
    set globalCompositeOperation(v){this._state.globalCompositeOperation=String(v)}
    save(){this._stack.push(clone(this._state))}
    restore(){if(this._stack.length)this._state=this._stack.pop()}
    _paint(style, stroke=false){const s=this._state;let paint=String(style);if(style&&style.__gradient){const values=[];for(const stop of style.stops)values.push(stop.offset,stop.color);paint='@radial:'+__nativeCall('materializeRadialGradient',...style.geometry,style.stops.length,...values)}return [paint,s.globalAlpha,s.shadowColor,
      s.shadowBlur,s.shadowOffsetX,s.shadowOffsetY,s.globalCompositeOperation,
      ...(stroke?[s.lineWidth]:[]),...s.transform]}
    clearRect(x,y,w,h){__nativeCall('clearRect',x,y,w,h,...this._state.transform)}
    fillRect(x,y,w,h){__nativeCall('fillRect',x,y,w,h,...this._paint(this.fillStyle))}
    strokeRect(x,y,w,h){__nativeCall('strokeRect',x,y,w,h,...this._paint(this.strokeStyle,true))}
    beginPath(){__nativeCall('beginPath')}
    closePath(){__nativeCall('closePath')}
    moveTo(x,y){__nativeCall('moveTo',x,y)}
    lineTo(x,y){__nativeCall('lineTo',x,y)}
    rect(x,y,w,h){this.moveTo(x,y);this.lineTo(x+w,y);this.lineTo(x+w,y+h);this.lineTo(x,y+h);this.closePath()}
    arc(x,y,r,s,e,a=false){__nativeCall('arc',x,y,r,s,e,!!a)}
    fill(){__nativeCall('fill',...this._paint(this.fillStyle))}
    stroke(){__nativeCall('stroke',...this._paint(this.strokeStyle,true))}
    translate(x,y){const m=this._state.transform;m[4]+=x*m[0]+y*m[2];m[5]+=x*m[1]+y*m[3]}
    scale(x,y){const m=this._state.transform;m[0]*=x;m[1]*=x;m[2]*=y;m[3]*=y}
    rotate(r){const m=this._state.transform,c=Math.cos(r),s=Math.sin(r),a=m[0],b=m[1],d=m[2],e=m[3];m[0]=a*c+d*s;m[1]=b*c+e*s;m[2]=d*c-a*s;m[3]=e*c-b*s}
    setTransform(a,b,c,d,e,f){this._state.transform=[a,b,c,d,e,f].map(Number)}
    resetTransform(){this._state.transform=[1,0,0,1,0,0]}
    createRadialGradient(x0,y0,r0,x1,y1,r1){
      if(Number(r0)<0||Number(r1)<0)throw new RangeError('radial gradient radius must be non-negative');
      return {__gradient:true,geometry:[x0,y0,r0,x1,y1,r1].map(Number),stops:[],addColorStop(offset,color){offset=Number(offset);if(offset<0||offset>1)throw new RangeError('gradient stop offset must be in [0, 1]');this.stops.push({offset,color:String(color)})}};
    }
  }
  const context = new Canvas2DContext();
  let canvasWidth = __CANVAS_WIDTH__, canvasHeight = __CANVAS_HEIGHT__;
  const canvas = { id: __CANVAS_ID__, onmousemove:null,
    getContext(type){ if(type !== '2d') return null; return context; },
    getBoundingClientRect(){const width=globalThis.innerWidth||canvasWidth,height=globalThis.innerHeight||canvasHeight;return {left:0,top:0,right:width,bottom:height,width,height,x:0,y:0}} };
  Object.defineProperties(canvas, {
    width:{get(){return canvasWidth},set(v){canvasWidth=__nativeCall('setCanvasWidth',Number(v));context._reset()}},
    height:{get(){return canvasHeight},set(v){canvasHeight=__nativeCall('setCanvasHeight',Number(v));context._reset()}}
  });
  globalThis.window=globalThis; globalThis.self=globalThis;
  globalThis.innerWidth=canvas.width; globalThis.innerHeight=canvas.height;
  const listeners = new Map();
  globalThis.addEventListener=(type,callback)=>{
    if(typeof callback!=='function') return;
    const callbacks=listeners.get(String(type))||[];
    if(!callbacks.includes(callback)) callbacks.push(callback);
    listeners.set(String(type),callbacks);
  };
  globalThis.removeEventListener=(type,callback)=>{
    const callbacks=listeners.get(String(type));
    if(callbacks) listeners.set(String(type),callbacks.filter(item=>item!==callback));
  };
  globalThis.dispatchEvent=event=>{
    if(!event||!event.type) throw new TypeError('event.type is required');
    for(const callback of [...(listeners.get(String(event.type))||[])]) callback.call(globalThis,event);
    return true;
  };
  globalThis.__dispatchResize=(width,height)=>{
    globalThis.innerWidth=Number(width);globalThis.innerHeight=Number(height);
    globalThis.dispatchEvent({type:'resize',target:globalThis});
    if(typeof globalThis.onresize==='function')globalThis.onresize({type:'resize',target:globalThis});
  };
  globalThis.__dispatchCanvasMouseMove=(x,y)=>{
    if(typeof canvas.onmousemove==='function')canvas.onmousemove({type:'mousemove',target:canvas,offsetX:x,offsetY:y,layerX:x,layerY:y});
  };
  globalThis.document={getElementById(id){return canvas.id&&id===canvas.id?canvas:null},
    querySelector(selector){return selector==='canvas'?canvas:null}};
  if(canvas.id) globalThis[canvas.id]=canvas;
  globalThis.requestAnimationFrame=cb=>__nativeCall('raf',cb);
  globalThis.cancelAnimationFrame=id=>__nativeCall('cancelRaf',id);
  const log=(level,args)=>__nativeCall('console',level,args.map(String).join(' '));
  globalThis.console={log(...a){log('log',a)},warn(...a){log('warn',a)},error(...a){log('error',a)}};
  globalThis.performance={now(){return globalThis.__frameTimestamp||0}};
})();
)JS";
			std::string configured = bootstrap;
			const auto replace = [&](const std::string& token, const std::string& value) {
				const std::size_t position = configured.find(token);
				configured.replace(position, token.size(), value);
				};
			replace("__CANVAS_ID__", jsQuoted(envelope.canvas_id));
			replace("__CANVAS_WIDTH__", std::to_string(envelope.width));
			replace("__CANVAS_HEIGHT__", std::to_string(envelope.height));
			evaluate(configured, "browser-shim.js", std::chrono::milliseconds(50));
		}
	};

	JavaScriptPage::JavaScriptPage(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
	JavaScriptPage::~JavaScriptPage() = default;

	std::unique_ptr<JavaScriptPage> JavaScriptPage::create(const std::string& html,
		const std::uint64_t page_revision) {
		const auto start = Clock::now();
		auto impl = std::make_unique<Impl>(parseEnvelope(html), page_revision);
		impl->installBindings();
		impl->evaluate(impl->envelope.script, "inline-script.js", std::chrono::milliseconds(50));
		impl->stats.initialization_ms =
			std::chrono::duration<double, std::milli>(Clock::now() - start).count();
		return std::unique_ptr<JavaScriptPage>(new JavaScriptPage(std::move(impl)));
	}

	CanvasFrame JavaScriptPage::runAnimationFrame(const double timestamp_ms) {
		if (impl_->faulted) {
			throw std::runtime_error("JavaScript page is faulted; edit the source to create a new page");
		}
		const auto start = Clock::now();
		impl_->executing_callbacks = std::move(impl_->pending_callbacks);
		impl_->pending_callbacks.clear();
		impl_->stats.raf_callbacks = static_cast<std::uint32_t>(impl_->executing_callbacks.size());

		JSValue global = JS_GetGlobalObject(impl_->context);
		JS_SetPropertyStr(impl_->context, global, "__frameTimestamp",
			JS_NewFloat64(impl_->context, timestamp_ms));
		JS_FreeValue(impl_->context, global);

		impl_->timed_out = false;
		impl_->deadline = Clock::now() + std::chrono::milliseconds(8);
		impl_->deadline_active = true;
		for (std::size_t index = 0; index < impl_->executing_callbacks.size(); ++index) {
			if (JS_IsUndefined(impl_->executing_callbacks[index].function)) continue;
			impl_->active_raf_id = impl_->executing_callbacks[index].id;
			JSValue argument = JS_NewFloat64(impl_->context, timestamp_ms);
			JSValue result = JS_Call(impl_->context, impl_->executing_callbacks[index].function,
				JS_UNDEFINED, 1, &argument);
			JS_FreeValue(impl_->context, argument);
			JS_FreeValue(impl_->context, impl_->executing_callbacks[index].function);
			impl_->executing_callbacks[index].function = JS_UNDEFINED;
			if (JS_IsException(result)) {
				impl_->deadline_active = false;
				impl_->faulted = true;
				for (std::size_t remaining = index + 1U; remaining < impl_->executing_callbacks.size();
					++remaining) {
					JS_FreeValue(impl_->context, impl_->executing_callbacks[remaining].function);
					impl_->executing_callbacks[remaining].function = JS_UNDEFINED;
				}
				impl_->executing_callbacks.clear();
				impl_->active_raf_id = 0;
				throw std::runtime_error(impl_->exceptionText());
			}
			JS_FreeValue(impl_->context, result);
		}
		impl_->deadline_active = false;
		impl_->active_raf_id = 0;
		impl_->executing_callbacks.clear();

		CanvasFrame frame;
		frame.page_revision = impl_->revision;
		frame.required_capabilities = impl_->pending_capabilities;
		frame.reset_surface = std::exchange(impl_->reset_surface, false);
		frame.has_background_color = impl_->has_background_color;
		frame.background_color = impl_->background_color;
		frame.operations = std::move(impl_->operations);
		frame.path_segments = std::move(impl_->frame_path_segments);
		frame.gradient_stops = std::move(impl_->frame_gradient_stops);
		impl_->operations.clear();
		impl_->frame_path_segments.clear();
		impl_->frame_gradient_stops.clear();
		impl_->gradients.clear();
		impl_->pending_capabilities = CanvasCapabilityRectangles;
		for (const CanvasFrameOperation& operation : frame.operations) {
			if (operation.kind == CanvasOperationKind::FillPath ||
				operation.kind == CanvasOperationKind::StrokePath) {
				frame.required_capabilities |= CanvasCapabilityPaths;
			}
			if (operation.paint.shadow_blur > 0.0F || operation.paint.shadow_color.a > 0.0F) {
				frame.required_capabilities |= CanvasCapabilityShadows;
			}
			if (operation.paint.composite == CanvasCompositeMode::Lighter) {
				frame.required_capabilities |= CanvasCapabilityLighterBlend;
			}
		}
		const bool replaces_canvas = !frame.operations.empty() && [&] {
			const CanvasFrameOperation& first = frame.operations.front();
			if (first.kind == CanvasOperationKind::ClearRect) return true;
			if (first.kind != CanvasOperationKind::FillRect || first.paint.kind != CanvasPaintKind::Solid ||
				first.paint.composite != CanvasCompositeMode::SourceOver || first.paint.color.a < 0.999F) {
				return false;
			}
			const Rect& rect = first.rectangle;
			return first.transform.m11 == 1.0F && first.transform.m12 == 0.0F &&
				first.transform.m21 == 0.0F && first.transform.m22 == 1.0F &&
				first.transform.dx == 0.0F && first.transform.dy == 0.0F &&
				rect.left <= 0.0F && rect.top <= 0.0F &&
				rect.right >= static_cast<float>(impl_->envelope.width) &&
				rect.bottom >= static_cast<float>(impl_->envelope.height);
		}();
		if (!replaces_canvas) frame.required_capabilities |= CanvasCapabilityPersistent;
		impl_->stats.frame_ms =
			std::chrono::duration<double, std::milli>(Clock::now() - start).count();
		return frame;
	}

void JavaScriptPage::resizeViewport(const std::uint32_t width, const std::uint32_t height) {
  if (width == 0U || height == 0U) return;
  std::ostringstream script;
  script << "__dispatchResize(" << width << ',' << height << ");";
  impl_->evaluate(script.str(), "resize-event.js", std::chrono::milliseconds(8));
}

void JavaScriptPage::pointerMove(const float x, const float y) {
  if (!std::isfinite(x) || !std::isfinite(y)) return;
  std::ostringstream script;
  script << "__dispatchCanvasMouseMove(" << x << ',' << y << ");";
  impl_->evaluate(script.str(), "pointer-event.js", std::chrono::milliseconds(8));
}

	void JavaScriptPage::requestSurfaceReset() noexcept { impl_->reset_surface = true; }
	bool JavaScriptPage::faulted() const noexcept { return impl_->faulted; }
	std::uint32_t JavaScriptPage::canvasWidth() const noexcept { return impl_->envelope.width; }
	std::uint32_t JavaScriptPage::canvasHeight() const noexcept { return impl_->envelope.height; }
	std::uint64_t JavaScriptPage::revision() const noexcept { return impl_->revision; }
	const JavaScriptPageStats& JavaScriptPage::stats() const noexcept { return impl_->stats; }

}  // namespace gpu2d