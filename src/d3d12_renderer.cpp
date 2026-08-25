#include "gpu2d/d3d12_renderer.hpp"

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace gpu2d {
	namespace {

		using Microsoft::WRL::ComPtr;

		constexpr std::uint32_t kFrameCount = 3;
		constexpr std::uint32_t kSrvCapacity = 64;
		constexpr std::size_t kInstanceBufferBytes = 4U * 1024U * 1024U;
		constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

		[[noreturn]] void throwHr(const HRESULT result, const std::string_view operation) {
			std::ostringstream stream;
			stream << operation << " failed with HRESULT 0x" << std::hex
				<< static_cast<unsigned long>(result);
			throw std::runtime_error(stream.str());
		}

		void checkHr(const HRESULT result, const std::string_view operation) {
			if (FAILED(result)) {
				throwHr(result, operation);
			}
		}

		[[nodiscard]] std::string narrow(const std::wstring_view value) {
			if (value.empty()) {
				return {};
			}
			const int required = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
				nullptr, 0, nullptr, nullptr);
			if (required <= 0) {
				return "<adapter name conversion failed>";
			}
			std::string result(static_cast<std::size_t>(required), '\0');
			WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(),
				required, nullptr, nullptr);
			return result;
		}

		[[nodiscard]] D3D12_HEAP_PROPERTIES heapProperties(const D3D12_HEAP_TYPE type) noexcept {
			D3D12_HEAP_PROPERTIES properties{};
			properties.Type = type;
			properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
			properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
			properties.CreationNodeMask = 1;
			properties.VisibleNodeMask = 1;
			return properties;
		}

		[[nodiscard]] D3D12_RESOURCE_DESC bufferDesc(const std::uint64_t size) noexcept {
			D3D12_RESOURCE_DESC description{};
			description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			description.Alignment = 0;
			description.Width = size;
			description.Height = 1;
			description.DepthOrArraySize = 1;
			description.MipLevels = 1;
			description.Format = DXGI_FORMAT_UNKNOWN;
			description.SampleDesc = { 1, 0 };
			description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			description.Flags = D3D12_RESOURCE_FLAG_NONE;
			return description;
		}

		[[nodiscard]] D3D12_RESOURCE_BARRIER transitionBarrier(ID3D12Resource* resource,
			const D3D12_RESOURCE_STATES before,
			const D3D12_RESOURCE_STATES after) noexcept {
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
			barrier.Transition.pResource = resource;
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = before;
			barrier.Transition.StateAfter = after;
			return barrier;
		}

		[[nodiscard]] std::filesystem::path moduleDirectory() {
			HMODULE module = nullptr;
			if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&moduleDirectory), &module) == FALSE) {
				throw std::runtime_error("GetModuleHandleExW failed for renderer module");
			}
			std::wstring path(32768, L'\0');
			const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
			if (length == 0 || static_cast<std::size_t>(length) >= path.size()) {
				throw std::runtime_error("GetModuleFileNameW failed");
			}
			path.resize(length);
			return std::filesystem::path(path).parent_path();
		}

		[[nodiscard]] ComPtr<ID3DBlob> compileShader(const std::filesystem::path& path,
			const char* entry_point, const char* target) {
			UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
			flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
			flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

			ComPtr<ID3DBlob> bytecode;
			ComPtr<ID3DBlob> errors;
			const HRESULT result = D3DCompileFromFile(path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
				entry_point, target, flags, 0, &bytecode, &errors);
			if (errors) {
				std::cerr.write(static_cast<const char*>(errors->GetBufferPointer()),
					static_cast<std::streamsize>(errors->GetBufferSize()));
				std::cerr << '\n';
			}
			checkHr(result, std::string("D3DCompileFromFile ") + entry_point);
			return bytecode;
		}

		[[nodiscard]] std::size_t checkedTextureBytes(const std::uint32_t width,
			const std::uint32_t height) {
			if (width == 0 || height == 0) {
				throw std::invalid_argument("texture dimensions must be non-zero");
			}
			constexpr std::size_t bytes_per_pixel = 4;
			if (width > std::numeric_limits<std::size_t>::max() / bytes_per_pixel) {
				throw std::overflow_error("texture row size overflow");
			}
			const std::size_t row_bytes = static_cast<std::size_t>(width) * bytes_per_pixel;
			if (height > std::numeric_limits<std::size_t>::max() / row_bytes) {
				throw std::overflow_error("texture byte size overflow");
			}
			return row_bytes * static_cast<std::size_t>(height);
		}

	}  // namespace

	struct D3D12Renderer::Impl {
		struct FrameContext {
			ComPtr<ID3D12CommandAllocator> allocator;
			ComPtr<ID3D12Resource> instance_upload;
			std::byte* mapped_instances{};
			std::uint64_t fence_value{};
		};

		struct TextureSlot {
			ComPtr<ID3D12Resource> resource;
			std::uint32_t descriptor_index{};
			std::uint32_t generation{ 1 };
			std::uint32_t width{};
			std::uint32_t height{};
			std::uint64_t last_use_fence{};
			D3D12_RESOURCE_STATES resource_state{ D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
			bool external{};
			bool alive{};
		};

		struct RetiredTexture {
			ComPtr<ID3D12Resource> resource;
			std::uint32_t descriptor_index{};
			std::uint64_t safe_after_fence{};
		};

		HWND window{};
		std::uint32_t width{};
		std::uint32_t height{};
		RendererOptions options{};
		bool tearing_supported{};
		UINT swap_chain_flags{};
		std::string adapter_name;

		ComPtr<IDXGIFactory6> factory;
		ComPtr<IDXGIAdapter1> adapter;
		ComPtr<ID3D12Device> device;
		ComPtr<ID3D12CommandQueue> queue;
		ComPtr<IDXGISwapChain3> swap_chain;
		ComPtr<ID3D12DescriptorHeap> rtv_heap;
		ComPtr<ID3D12DescriptorHeap> srv_heap;
		std::uint32_t rtv_increment{};
		std::uint32_t srv_increment{};
		std::array<ComPtr<ID3D12Resource>, kFrameCount> back_buffers;
		std::array<FrameContext, kFrameCount> frames;
		ComPtr<ID3D12GraphicsCommandList> command_list;

		ComPtr<ID3D12RootSignature> root_signature;
		ComPtr<ID3D12PipelineState> solid_pipeline;
		ComPtr<ID3D12PipelineState> textured_pipeline;
		ComPtr<ID3D12Resource> quad_vertex_buffer;
		ComPtr<ID3D12Resource> quad_index_buffer;
		D3D12_VERTEX_BUFFER_VIEW quad_vertex_view{};
		D3D12_INDEX_BUFFER_VIEW quad_index_view{};

		ComPtr<ID3D12CommandAllocator> upload_allocator;
		ComPtr<ID3D12GraphicsCommandList> upload_list;
		ComPtr<ID3D12Fence> fence;
		HANDLE fence_event{};
		std::uint64_t next_fence_value{ 1 };

		std::vector<TextureSlot> texture_slots;
		std::vector<std::uint32_t> free_texture_slots;
		std::vector<std::uint32_t> free_descriptors;
		std::deque<RetiredTexture> retired_textures;
		ComPtr<ID3D12Resource> white_texture;

		Impl(HWND target_window, const std::uint32_t initial_width, const std::uint32_t initial_height,
			const RendererOptions renderer_options)
			: window(target_window), width(initial_width), height(initial_height), options(renderer_options) {
			if (window == nullptr || width == 0 || height == 0) {
				throw std::invalid_argument("D3D12Renderer requires a window and non-zero dimensions");
			}
			initialize();
		}

		~Impl() {
			try {
				waitIdle();
				collectRetired(true);
			}
			catch (const std::exception& error) {
				std::cerr << "Renderer shutdown warning: " << error.what() << '\n';
			}

			for (FrameContext& frame : frames) {
				if (frame.instance_upload && frame.mapped_instances != nullptr) {
					frame.instance_upload->Unmap(0, nullptr);
					frame.mapped_instances = nullptr;
				}
			}
			if (fence_event != nullptr) {
				CloseHandle(fence_event);
				fence_event = nullptr;
			}
		}

		void initialize() {
			UINT factory_flags = 0;
			if (options.enable_debug_layer) {
				ComPtr<ID3D12Debug> debug;
				checkHr(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)),
					"D3D12GetDebugInterface (--debug-layer requires Graphics Tools)");
				debug->EnableDebugLayer();
				factory_flags |= DXGI_CREATE_FACTORY_DEBUG;
				std::cout << "D3D12 debug layer enabled by --debug-layer\n";
			}

			checkHr(CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");

			BOOL allow_tearing = FALSE;
			if (SUCCEEDED(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing,
				sizeof(allow_tearing)))) {
				tearing_supported = allow_tearing == TRUE;
			}
			swap_chain_flags = tearing_supported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0U;

			selectHardwareAdapter();
			checkHr(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)),
				"D3D12CreateDevice");

			D3D12_COMMAND_QUEUE_DESC queue_description{};
			queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
			queue_description.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
			checkHr(device->CreateCommandQueue(&queue_description, IID_PPV_ARGS(&queue)),
				"CreateCommandQueue");

			createSwapChain();
			createDescriptorHeaps();
			createBackBufferViews();
			createFrameContexts();
			createFenceObjects();
			createUploadObjects();
			createPipelines();
			createQuadBuffers();
			createWhiteTexture();
		}

		void selectHardwareAdapter() {
			for (UINT index = 0;; ++index) {
				ComPtr<IDXGIAdapter1> candidate;
				const HRESULT enumerate = factory->EnumAdapterByGpuPreference(
					index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate));
				if (enumerate == DXGI_ERROR_NOT_FOUND) {
					break;
				}
				checkHr(enumerate, "EnumAdapterByGpuPreference");

				DXGI_ADAPTER_DESC1 description{};
				checkHr(candidate->GetDesc1(&description), "IDXGIAdapter1::GetDesc1");
				if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
					std::cout << "Skipping software DXGI adapter: " << narrow(description.Description) << '\n';
					continue;
				}
				ComPtr<ID3D12Device> probe_device;
				const HRESULT probe = D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0,
					IID_PPV_ARGS(&probe_device));
				std::cout << "Probing DXGI adapter: " << narrow(description.Description)
					<< " HRESULT=0x" << std::hex << static_cast<unsigned long>(probe) << std::dec
					<< '\n';
				if (SUCCEEDED(probe)) {
					adapter = candidate;
					adapter_name = narrow(description.Description);
					return;
				}
			}
			throw std::runtime_error("no hardware Direct3D 12 adapter is available; WARP fallback is disabled");
		}

		void createSwapChain() {
			DXGI_SWAP_CHAIN_DESC1 description{};
			description.Width = width;
			description.Height = height;
			description.Format = kBackBufferFormat;
			description.Stereo = FALSE;
			description.SampleDesc = { 1, 0 };
			description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			description.BufferCount = kFrameCount;
			description.Scaling = DXGI_SCALING_STRETCH;
			description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
			description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
			description.Flags = swap_chain_flags;

			ComPtr<IDXGISwapChain1> base_swap_chain;
			checkHr(factory->CreateSwapChainForHwnd(queue.Get(), window, &description, nullptr, nullptr,
				&base_swap_chain),
				"CreateSwapChainForHwnd");
			checkHr(factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER),
				"MakeWindowAssociation");
			checkHr(base_swap_chain.As(&swap_chain), "Query IDXGISwapChain3");
		}

		void createDescriptorHeaps() {
			D3D12_DESCRIPTOR_HEAP_DESC rtv_description{};
			rtv_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
			rtv_description.NumDescriptors = kFrameCount;
			checkHr(device->CreateDescriptorHeap(&rtv_description, IID_PPV_ARGS(&rtv_heap)),
				"Create RTV heap");
			rtv_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

			D3D12_DESCRIPTOR_HEAP_DESC srv_description{};
			srv_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			srv_description.NumDescriptors = kSrvCapacity;
			srv_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			checkHr(device->CreateDescriptorHeap(&srv_description, IID_PPV_ARGS(&srv_heap)),
				"Create SRV heap");
			srv_increment =
				device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			for (std::uint32_t index = kSrvCapacity; index-- > 1;) {
				free_descriptors.push_back(index);
			}
		}

		void createBackBufferViews() {
			D3D12_CPU_DESCRIPTOR_HANDLE handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
			for (std::uint32_t index = 0; index < kFrameCount; ++index) {
				checkHr(swap_chain->GetBuffer(index, IID_PPV_ARGS(&back_buffers[index])),
					"IDXGISwapChain::GetBuffer");
				device->CreateRenderTargetView(back_buffers[index].Get(), nullptr, handle);
				handle.ptr += rtv_increment;
			}
		}

		void createFrameContexts() {
			const D3D12_HEAP_PROPERTIES upload_heap = heapProperties(D3D12_HEAP_TYPE_UPLOAD);
			const D3D12_RESOURCE_DESC upload_description = bufferDesc(kInstanceBufferBytes);
			for (FrameContext& frame : frames) {
				checkHr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
					IID_PPV_ARGS(&frame.allocator)),
					"Create frame command allocator");
				checkHr(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE,
					&upload_description, D3D12_RESOURCE_STATE_GENERIC_READ,
					nullptr, IID_PPV_ARGS(&frame.instance_upload)),
					"Create instance upload buffer");
				void* mapped = nullptr;
				const D3D12_RANGE no_read{ 0, 0 };
				checkHr(frame.instance_upload->Map(0, &no_read, &mapped), "Map instance upload buffer");
				frame.mapped_instances = static_cast<std::byte*>(mapped);
			}

			checkHr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
				frames[0].allocator.Get(), nullptr,
				IID_PPV_ARGS(&command_list)),
				"Create graphics command list");
			checkHr(command_list->Close(), "Close initial graphics command list");
		}

		void createFenceObjects() {
			checkHr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Create fence");
			fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			if (fence_event == nullptr) {
				throw std::runtime_error("CreateEventW for fence failed");
			}
		}

		void createUploadObjects() {
			checkHr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
				IID_PPV_ARGS(&upload_allocator)),
				"Create upload allocator");
			checkHr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, upload_allocator.Get(),
				nullptr, IID_PPV_ARGS(&upload_list)),
				"Create upload command list");
			checkHr(upload_list->Close(), "Close initial upload command list");
		}

		void createPipelines() {
			const std::filesystem::path shader_path =
				moduleDirectory() / L"shaders" / L"quad.hlsl";
			const ComPtr<ID3DBlob> vertex_shader = compileShader(shader_path, "VSMain", "vs_5_1");
			const ComPtr<ID3DBlob> solid_shader = compileShader(shader_path, "PSSolid", "ps_5_1");
			const ComPtr<ID3DBlob> textured_shader = compileShader(shader_path, "PSTextured", "ps_5_1");

			D3D12_DESCRIPTOR_RANGE srv_range{};
			srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			srv_range.NumDescriptors = 1;
			srv_range.BaseShaderRegister = 0;
			srv_range.RegisterSpace = 0;
			srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

			std::array<D3D12_ROOT_PARAMETER, 2> parameters{};
			parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			parameters[0].Constants.ShaderRegister = 0;
			parameters[0].Constants.RegisterSpace = 0;
			parameters[0].Constants.Num32BitValues = 2;
			parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
			parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			parameters[1].DescriptorTable.NumDescriptorRanges = 1;
			parameters[1].DescriptorTable.pDescriptorRanges = &srv_range;
			parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

			D3D12_STATIC_SAMPLER_DESC sampler{};
			sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			sampler.MipLODBias = 0.0F;
			sampler.MaxAnisotropy = 1;
			sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
			sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
			sampler.MinLOD = 0.0F;
			sampler.MaxLOD = D3D12_FLOAT32_MAX;
			sampler.ShaderRegister = 0;
			sampler.RegisterSpace = 0;
			sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

			D3D12_ROOT_SIGNATURE_DESC root_description{};
			root_description.NumParameters = static_cast<UINT>(parameters.size());
			root_description.pParameters = parameters.data();
			root_description.NumStaticSamplers = 1;
			root_description.pStaticSamplers = &sampler;
			root_description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
				D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
				D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
				D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

			ComPtr<ID3DBlob> serialized;
			ComPtr<ID3DBlob> errors;
			const HRESULT serialize_result =
				D3D12SerializeRootSignature(&root_description, D3D_ROOT_SIGNATURE_VERSION_1,
					&serialized, &errors);
			if (errors) {
				std::cerr.write(static_cast<const char*>(errors->GetBufferPointer()),
					static_cast<std::streamsize>(errors->GetBufferSize()));
				std::cerr << '\n';
			}
			checkHr(serialize_result, "D3D12SerializeRootSignature");
			checkHr(device->CreateRootSignature(0, serialized->GetBufferPointer(),
				serialized->GetBufferSize(),
				IID_PPV_ARGS(&root_signature)),
				"CreateRootSignature");

			const std::array<D3D12_INPUT_ELEMENT_DESC, 7> input_elements{
				D3D12_INPUT_ELEMENT_DESC{"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
										 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
				D3D12_INPUT_ELEMENT_DESC{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
										 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
				D3D12_INPUT_ELEMENT_DESC{"INSTANCE_RECT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0,
										 D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
				D3D12_INPUT_ELEMENT_DESC{"INSTANCE_MATRIX", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16,
										 D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
				D3D12_INPUT_ELEMENT_DESC{"INSTANCE_MATRIX", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32,
										 D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
				D3D12_INPUT_ELEMENT_DESC{"INSTANCE_COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48,
										 D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
				D3D12_INPUT_ELEMENT_DESC{"INSTANCE_UV", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 64,
										 D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
			};

			D3D12_RASTERIZER_DESC rasterizer{};
			rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
			rasterizer.CullMode = D3D12_CULL_MODE_NONE;
			rasterizer.FrontCounterClockwise = FALSE;
			rasterizer.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
			rasterizer.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
			rasterizer.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
			rasterizer.DepthClipEnable = TRUE;
			rasterizer.MultisampleEnable = FALSE;
			rasterizer.AntialiasedLineEnable = FALSE;
			rasterizer.ForcedSampleCount = 0;
			rasterizer.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

			D3D12_BLEND_DESC blend{};
			blend.AlphaToCoverageEnable = FALSE;
			blend.IndependentBlendEnable = FALSE;
			D3D12_RENDER_TARGET_BLEND_DESC target_blend{};
			target_blend.BlendEnable = TRUE;
			target_blend.LogicOpEnable = FALSE;
			target_blend.SrcBlend = D3D12_BLEND_ONE;
			target_blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
			target_blend.BlendOp = D3D12_BLEND_OP_ADD;
			target_blend.SrcBlendAlpha = D3D12_BLEND_ONE;
			target_blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
			target_blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
			target_blend.LogicOp = D3D12_LOGIC_OP_NOOP;
			target_blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			blend.RenderTarget[0] = target_blend;

			D3D12_DEPTH_STENCIL_DESC depth{};
			depth.DepthEnable = FALSE;
			depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
			depth.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
			depth.StencilEnable = FALSE;

			D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
			pipeline.pRootSignature = root_signature.Get();
			pipeline.VS = { vertex_shader->GetBufferPointer(), vertex_shader->GetBufferSize() };
			pipeline.BlendState = blend;
			pipeline.SampleMask = UINT_MAX;
			pipeline.RasterizerState = rasterizer;
			pipeline.DepthStencilState = depth;
			pipeline.InputLayout = { input_elements.data(), static_cast<UINT>(input_elements.size()) };
			pipeline.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
			pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			pipeline.NumRenderTargets = 1;
			pipeline.RTVFormats[0] = kBackBufferFormat;
			pipeline.SampleDesc = { 1, 0 };

			pipeline.PS = { solid_shader->GetBufferPointer(), solid_shader->GetBufferSize() };
			checkHr(device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&solid_pipeline)),
				"Create solid pipeline");
			pipeline.PS = { textured_shader->GetBufferPointer(), textured_shader->GetBufferSize() };
			checkHr(device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&textured_pipeline)),
				"Create textured pipeline");
		}

		[[nodiscard]] std::uint64_t signalQueue() {
			const std::uint64_t value = next_fence_value++;
			checkHr(queue->Signal(fence.Get(), value), "ID3D12CommandQueue::Signal");
			return value;
		}

		void waitForFence(const std::uint64_t value) {
			if (value == 0 || fence->GetCompletedValue() >= value) {
				return;
			}
			checkHr(fence->SetEventOnCompletion(value, fence_event), "SetEventOnCompletion");
			if (WaitForSingleObject(fence_event, INFINITE) != WAIT_OBJECT_0) {
				throw std::runtime_error("WaitForSingleObject for GPU fence failed");
			}
		}

		template <typename Record>
		std::uint64_t executeUpload(Record&& record) {
			checkHr(upload_allocator->Reset(), "Reset upload allocator");
			checkHr(upload_list->Reset(upload_allocator.Get(), nullptr), "Reset upload command list");
			record(upload_list.Get());
			checkHr(upload_list->Close(), "Close upload command list");
			ID3D12CommandList* lists[]{ upload_list.Get() };
			queue->ExecuteCommandLists(1, lists);
			const std::uint64_t value = signalQueue();
			waitForFence(value);
			return value;
		}

		[[nodiscard]] ComPtr<ID3D12Resource> createDefaultBuffer(const void* source,
			const std::size_t size,
			const D3D12_RESOURCE_STATES final_state) {
			if (source == nullptr || size == 0) {
				throw std::invalid_argument("createDefaultBuffer requires data");
			}
			const D3D12_HEAP_PROPERTIES default_heap = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
			const D3D12_HEAP_PROPERTIES upload_heap = heapProperties(D3D12_HEAP_TYPE_UPLOAD);
			const D3D12_RESOURCE_DESC description = bufferDesc(size);
			ComPtr<ID3D12Resource> destination;
			ComPtr<ID3D12Resource> staging;
			checkHr(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &description,
				D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
				IID_PPV_ARGS(&destination)),
				"Create default buffer");
			checkHr(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &description,
				D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
				IID_PPV_ARGS(&staging)),
				"Create buffer staging resource");
			void* mapped = nullptr;
			const D3D12_RANGE no_read{ 0, 0 };
			checkHr(staging->Map(0, &no_read, &mapped), "Map buffer staging resource");
			std::memcpy(mapped, source, size);
			staging->Unmap(0, nullptr);
			executeUpload([&](ID3D12GraphicsCommandList* list) {
				list->CopyBufferRegion(destination.Get(), 0, staging.Get(), 0, size);
				const D3D12_RESOURCE_BARRIER barrier =
					transitionBarrier(destination.Get(), D3D12_RESOURCE_STATE_COPY_DEST, final_state);
				list->ResourceBarrier(1, &barrier);
				});
			return destination;
		}

		void createQuadBuffers() {
			struct Vertex {
				float x;
				float y;
				float u;
				float v;
			};
			constexpr std::array<Vertex, 4> vertices{ {{0.0F, 0.0F, 0.0F, 0.0F},
													 {1.0F, 0.0F, 1.0F, 0.0F},
													 {1.0F, 1.0F, 1.0F, 1.0F},
													 {0.0F, 1.0F, 0.0F, 1.0F}} };
			constexpr std::array<std::uint16_t, 6> indices{ {0, 1, 2, 0, 2, 3} };

			quad_vertex_buffer = createDefaultBuffer(vertices.data(), sizeof(vertices),
				D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
			quad_index_buffer = createDefaultBuffer(indices.data(), sizeof(indices),
				D3D12_RESOURCE_STATE_INDEX_BUFFER);
			quad_vertex_view = { quad_vertex_buffer->GetGPUVirtualAddress(),
								static_cast<UINT>(sizeof(vertices)), static_cast<UINT>(sizeof(Vertex)) };
			quad_index_view = { quad_index_buffer->GetGPUVirtualAddress(),
							   static_cast<UINT>(sizeof(indices)),
							   DXGI_FORMAT_R16_UINT };
		}

		[[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE cpuSrv(const std::uint32_t index) const noexcept {
			D3D12_CPU_DESCRIPTOR_HANDLE handle = srv_heap->GetCPUDescriptorHandleForHeapStart();
			handle.ptr += static_cast<SIZE_T>(index) * srv_increment;
			return handle;
		}

		[[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE gpuSrv(const std::uint32_t index) const noexcept {
			D3D12_GPU_DESCRIPTOR_HANDLE handle = srv_heap->GetGPUDescriptorHandleForHeapStart();
			handle.ptr += static_cast<UINT64>(index) * srv_increment;
			return handle;
		}

		[[nodiscard]] ComPtr<ID3D12Resource> uploadTexture(const TextureUpload& upload,
			const std::uint32_t descriptor_index) {
			const std::size_t expected_bytes = checkedTextureBytes(upload.width, upload.height);
			if (upload.rgba8.size() != expected_bytes) {
				throw std::invalid_argument("RGBA8 texture byte count does not match dimensions");
			}

			D3D12_RESOURCE_DESC texture_description{};
			texture_description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			texture_description.Width = upload.width;
			texture_description.Height = upload.height;
			texture_description.DepthOrArraySize = 1;
			texture_description.MipLevels = 1;
			texture_description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			texture_description.SampleDesc = { 1, 0 };
			texture_description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			texture_description.Flags = D3D12_RESOURCE_FLAG_NONE;

			const D3D12_HEAP_PROPERTIES default_heap = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
			ComPtr<ID3D12Resource> texture;
			checkHr(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE,
				&texture_description, D3D12_RESOURCE_STATE_COPY_DEST,
				nullptr, IID_PPV_ARGS(&texture)),
				"Create texture resource");

			D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
			UINT rows = 0;
			UINT64 total_size = 0;
			device->GetCopyableFootprints(&texture_description, 0, 1, 0, &footprint, &rows, nullptr,
				&total_size);
			const D3D12_RESOURCE_DESC staging_description = bufferDesc(total_size);
			const D3D12_HEAP_PROPERTIES upload_heap = heapProperties(D3D12_HEAP_TYPE_UPLOAD);
			ComPtr<ID3D12Resource> staging;
			checkHr(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE,
				&staging_description, D3D12_RESOURCE_STATE_GENERIC_READ,
				nullptr, IID_PPV_ARGS(&staging)),
				"Create texture staging resource");

			std::byte* mapped = nullptr;
			const D3D12_RANGE no_read{ 0, 0 };
			checkHr(staging->Map(0, &no_read, reinterpret_cast<void**>(&mapped)),
				"Map texture staging resource");
			const std::size_t source_row_bytes = static_cast<std::size_t>(upload.width) * 4U;
			for (UINT row = 0; row < rows; ++row) {
				std::memcpy(mapped + footprint.Offset + static_cast<std::size_t>(row) * footprint.Footprint.RowPitch,
					upload.rgba8.data() + static_cast<std::size_t>(row) * source_row_bytes,
					source_row_bytes);
			}
			staging->Unmap(0, nullptr);

			executeUpload([&](ID3D12GraphicsCommandList* list) {
				D3D12_TEXTURE_COPY_LOCATION destination{};
				destination.pResource = texture.Get();
				destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				destination.SubresourceIndex = 0;
				D3D12_TEXTURE_COPY_LOCATION source{};
				source.pResource = staging.Get();
				source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
				source.PlacedFootprint = footprint;
				list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
				const D3D12_RESOURCE_BARRIER barrier = transitionBarrier(
					texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				list->ResourceBarrier(1, &barrier);
				});

			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Texture2D.MipLevels = 1;
			device->CreateShaderResourceView(texture.Get(), &srv, cpuSrv(descriptor_index));
			return texture;
		}

		void createWhiteTexture() {
			TextureUpload white{ 1, 1, {std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF},
									   std::byte{0xFF}} };
			white_texture = uploadTexture(white, 0);
		}

		TextureSlot& checkedTexture(const TextureHandle handle) {
			if (!handle.valid() || handle.index >= texture_slots.size()) {
				throw std::invalid_argument("texture handle index is invalid");
			}
			TextureSlot& slot = texture_slots[handle.index];
			if (!slot.alive || slot.generation != handle.generation) {
				throw std::invalid_argument("texture handle is stale or released");
			}
			return slot;
		}

		TextureHandle createTexture(const TextureUpload& upload) {
			collectRetired(false);
			if (free_descriptors.empty()) {
				throw std::runtime_error("shader-visible texture descriptor heap is full");
			}
			const std::uint32_t descriptor = free_descriptors.back();
			free_descriptors.pop_back();

			ComPtr<ID3D12Resource> resource;
			try {
				resource = uploadTexture(upload, descriptor);
			}
			catch (...) {
				free_descriptors.push_back(descriptor);
				throw;
			}

			std::uint32_t slot_index = 0;
			if (!free_texture_slots.empty()) {
				slot_index = free_texture_slots.back();
				free_texture_slots.pop_back();
			}
			else {
				if (texture_slots.size() >= std::numeric_limits<std::uint32_t>::max()) {
					throw std::overflow_error("texture slot index overflow");
				}
				slot_index = static_cast<std::uint32_t>(texture_slots.size());
				texture_slots.emplace_back();
			}

			TextureSlot& slot = texture_slots[slot_index];
			slot.resource = std::move(resource);
			slot.descriptor_index = descriptor;
			slot.width = upload.width;
			slot.height = upload.height;
			slot.last_use_fence = 0;
			slot.resource_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			slot.external = false;
			slot.alive = true;
			return TextureHandle{ slot_index, slot.generation };
		}

		TextureHandle registerExternalTexture(ID3D12Resource* external_resource,
			const std::uint32_t texture_width, const std::uint32_t texture_height,
			const D3D12_RESOURCE_STATES state) {
			if (external_resource == nullptr || texture_width == 0U || texture_height == 0U) {
				throw std::invalid_argument("external texture requires a resource and non-zero dimensions");
			}
			collectRetired(false);
			if (free_descriptors.empty()) {
				throw std::runtime_error("shader-visible texture descriptor heap is full");
			}
			const std::uint32_t descriptor = free_descriptors.back();
			free_descriptors.pop_back();
			std::uint32_t slot_index{};
			if (!free_texture_slots.empty()) {
				slot_index = free_texture_slots.back();
				free_texture_slots.pop_back();
			} else {
				if (texture_slots.size() >= std::numeric_limits<std::uint32_t>::max()) {
					free_descriptors.push_back(descriptor);
					throw std::overflow_error("texture slot index overflow");
				}
				slot_index = static_cast<std::uint32_t>(texture_slots.size());
				texture_slots.emplace_back();
			}
			TextureSlot& slot = texture_slots[slot_index];
			slot.resource = external_resource;
			slot.descriptor_index = descriptor;
			slot.width = texture_width;
			slot.height = texture_height;
			slot.last_use_fence = 0;
			slot.resource_state = state;
			slot.external = true;
			slot.alive = true;
			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Texture2D.MipLevels = 1;
			device->CreateShaderResourceView(slot.resource.Get(), &srv, cpuSrv(descriptor));
			return TextureHandle{ slot_index, slot.generation };
		}

		void setExternalTextureState(const TextureHandle handle, const D3D12_RESOURCE_STATES state) {
			TextureSlot& slot = checkedTexture(handle);
			if (!slot.external) {
				throw std::invalid_argument("texture handle is not an external texture");
			}
			slot.resource_state = state;
		}

		void updateTexture(const TextureHandle handle, const TextureUpload& upload) {
			TextureSlot& slot = checkedTexture(handle);
			if (upload.width != slot.width || upload.height != slot.height) {
				throw std::invalid_argument("updated texture dimensions must match the existing texture");
			}
			const std::size_t expected_bytes = checkedTextureBytes(upload.width, upload.height);
			if (upload.rgba8.size() != expected_bytes) {
				throw std::invalid_argument("updated RGBA8 texture byte count does not match dimensions");
			}

			waitForFence(slot.last_use_fence);

			const D3D12_RESOURCE_DESC texture_description = slot.resource->GetDesc();
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
			UINT rows = 0;
			UINT64 staging_size = 0;
			device->GetCopyableFootprints(&texture_description, 0, 1, 0, &footprint, &rows, nullptr,
				&staging_size);
			const D3D12_HEAP_PROPERTIES upload_heap = heapProperties(D3D12_HEAP_TYPE_UPLOAD);
			const D3D12_RESOURCE_DESC staging_description = bufferDesc(staging_size);
			ComPtr<ID3D12Resource> staging;
			checkHr(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE,
				&staging_description,
				D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
				IID_PPV_ARGS(&staging)),
				"Create texture update staging buffer");

			void* mapped_data = nullptr;
			const D3D12_RANGE no_read{ 0, 0 };
			checkHr(staging->Map(0, &no_read, &mapped_data),
				"Map texture update staging buffer");
			auto* mapped = static_cast<std::byte*>(mapped_data);
			const std::size_t source_row_bytes = static_cast<std::size_t>(upload.width) * 4U;
			for (UINT row = 0; row < rows; ++row) {
				std::memcpy(mapped + footprint.Offset + static_cast<std::size_t>(row) * footprint.Footprint.RowPitch,
					upload.rgba8.data() + static_cast<std::size_t>(row) * source_row_bytes,
					source_row_bytes);
			}
			staging->Unmap(0, nullptr);

			executeUpload([&](ID3D12GraphicsCommandList* list) {
				const D3D12_RESOURCE_BARRIER to_copy = transitionBarrier(
					slot.resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
					D3D12_RESOURCE_STATE_COPY_DEST);
				list->ResourceBarrier(1, &to_copy);
				D3D12_TEXTURE_COPY_LOCATION destination{};
				destination.pResource = slot.resource.Get();
				destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				destination.SubresourceIndex = 0;
				D3D12_TEXTURE_COPY_LOCATION source{};
				source.pResource = staging.Get();
				source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
				source.PlacedFootprint = footprint;
				list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
				const D3D12_RESOURCE_BARRIER to_sample = transitionBarrier(
					slot.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
					D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				list->ResourceBarrier(1, &to_sample);
				});
		}

		void releaseTexture(const TextureHandle handle) {
			TextureSlot& slot = checkedTexture(handle);
			retired_textures.push_back(
				RetiredTexture{ std::move(slot.resource), slot.descriptor_index, slot.last_use_fence });
			slot.alive = false;
			slot.width = 0;
			slot.height = 0;
			slot.last_use_fence = 0;
			++slot.generation;
			if (slot.generation == 0) {
				++slot.generation;
			}
			free_texture_slots.push_back(handle.index);
			collectRetired(false);
		}

		void collectRetired(const bool force) {
			const std::uint64_t completed = force ? std::numeric_limits<std::uint64_t>::max()
				: fence->GetCompletedValue();
			for (auto iterator = retired_textures.begin(); iterator != retired_textures.end();) {
				if (iterator->safe_after_fence <= completed) {
					free_descriptors.push_back(iterator->descriptor_index);
					iterator = retired_textures.erase(iterator);
				}
				else {
					++iterator;
				}
			}
		}

		void stampTextures(const std::vector<TextureHandle>& handles, const std::uint64_t fence_value) {
			for (const TextureHandle handle : handles) {
				TextureSlot& slot = checkedTexture(handle);
				slot.last_use_fence = std::max(slot.last_use_fence, fence_value);
			}
		}

		[[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE rtv(const std::uint32_t index) const noexcept {
			D3D12_CPU_DESCRIPTOR_HANDLE handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
			handle.ptr += static_cast<SIZE_T>(index) * rtv_increment;
			return handle;
		}

		FrameTelemetry render(const RenderPlan& plan) {
			const auto start = std::chrono::steady_clock::now();
			if (width == 0 || height == 0) {
				return plan.telemetry;
			}
			if (plan.telemetry.upload_bytes > kInstanceBufferBytes) {
				throw std::overflow_error("frame instance data exceeds the per-frame upload buffer");
			}

			const std::uint32_t frame_index = swap_chain->GetCurrentBackBufferIndex();
			FrameContext& frame = frames[frame_index];
			waitForFence(frame.fence_value);
			checkHr(frame.allocator->Reset(), "Reset frame command allocator");
			checkHr(command_list->Reset(frame.allocator.Get(), nullptr), "Reset graphics command list");

			if (!plan.instances.empty()) {
				std::memcpy(frame.mapped_instances, plan.instances.data(), plan.telemetry.upload_bytes);
			}

			const D3D12_RESOURCE_BARRIER to_render = transitionBarrier(
				back_buffers[frame_index].Get(), D3D12_RESOURCE_STATE_PRESENT,
				D3D12_RESOURCE_STATE_RENDER_TARGET);
			command_list->ResourceBarrier(1, &to_render);
			const D3D12_CPU_DESCRIPTOR_HANDLE target = rtv(frame_index);
			command_list->OMSetRenderTargets(1, &target, FALSE, nullptr);
			constexpr std::array<float, 4> clear_color{ 0.025F, 0.035F, 0.055F, 1.0F };
			command_list->ClearRenderTargetView(target, clear_color.data(), 0, nullptr);

			const D3D12_VIEWPORT viewport{ 0.0F, 0.0F, static_cast<float>(width),
										  static_cast<float>(height), 0.0F, 1.0F };
			command_list->RSSetViewports(1, &viewport);
			command_list->SetGraphicsRootSignature(root_signature.Get());
			ID3D12DescriptorHeap* heaps[]{ srv_heap.Get() };
			command_list->SetDescriptorHeaps(1, heaps);
			const std::array<float, 2> viewport_constants{ static_cast<float>(width),
														 static_cast<float>(height) };
			command_list->SetGraphicsRoot32BitConstants(0, 2, viewport_constants.data(), 0);
			command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			command_list->IASetIndexBuffer(&quad_index_view);

			for (const RenderBatch& batch : plan.batches) {
				const std::uint64_t batch_end =
					static_cast<std::uint64_t>(batch.first_instance) + batch.instance_count;
				if (batch.instance_count == 0 || batch_end > plan.instances.size()) {
					throw std::logic_error("RenderBatch instance range is invalid");
				}

				const D3D12_RECT scissor{ batch.scissor.left, batch.scissor.top, batch.scissor.right,
										 batch.scissor.bottom };
				command_list->RSSetScissorRects(1, &scissor);
				command_list->SetPipelineState(batch.pipeline == PipelineKind::Solid
					? solid_pipeline.Get()
					: textured_pipeline.Get());

				D3D12_VERTEX_BUFFER_VIEW instance_view{};
				instance_view.BufferLocation = frame.instance_upload->GetGPUVirtualAddress() +
					static_cast<UINT64>(batch.first_instance) *
					sizeof(InstanceData);
				const std::uint64_t instance_bytes =
					static_cast<std::uint64_t>(batch.instance_count) * sizeof(InstanceData);
				if (instance_bytes > std::numeric_limits<UINT>::max()) {
					throw std::overflow_error("batch instance buffer view exceeds UINT");
				}
				instance_view.SizeInBytes = static_cast<UINT>(instance_bytes);
				instance_view.StrideInBytes = static_cast<UINT>(sizeof(InstanceData));
				const std::array<D3D12_VERTEX_BUFFER_VIEW, 2> views{ quad_vertex_view, instance_view };
				command_list->IASetVertexBuffers(0, static_cast<UINT>(views.size()), views.data());

				std::uint32_t descriptor_index = 0;
				if (batch.pipeline == PipelineKind::Textured) {
					TextureSlot& texture = checkedTexture(batch.texture);
					if (texture.resource_state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
						const D3D12_RESOURCE_BARRIER barrier = transitionBarrier(
							texture.resource.Get(), texture.resource_state,
							D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
						command_list->ResourceBarrier(1, &barrier);
						texture.resource_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
					}
					descriptor_index = texture.descriptor_index;
				}
				command_list->SetGraphicsRootDescriptorTable(1, gpuSrv(descriptor_index));
				command_list->DrawIndexedInstanced(6, batch.instance_count, 0, 0, 0);
			}

			const D3D12_RESOURCE_BARRIER to_present = transitionBarrier(
				back_buffers[frame_index].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
				D3D12_RESOURCE_STATE_PRESENT);
			command_list->ResourceBarrier(1, &to_present);
			checkHr(command_list->Close(), "Close graphics command list");
			ID3D12CommandList* command_lists[]{ command_list.Get() };
			queue->ExecuteCommandLists(1, command_lists);

			const UINT present_interval = options.vsync ? 1U : 0U;
			const UINT present_flags = !options.vsync && tearing_supported ? DXGI_PRESENT_ALLOW_TEARING : 0U;
			const HRESULT present_result = swap_chain->Present(present_interval, present_flags);
			if (FAILED(present_result)) {
				if (present_result == DXGI_ERROR_DEVICE_REMOVED || present_result == DXGI_ERROR_DEVICE_RESET) {
					throwHr(device->GetDeviceRemovedReason(), "D3D12 device removed during Present");
				}
				checkHr(present_result, "IDXGISwapChain::Present");
			}

			const std::uint64_t submitted = signalQueue();
			frame.fence_value = submitted;
			stampTextures(plan.referenced_textures, submitted);
			collectRetired(false);

			FrameTelemetry telemetry = plan.telemetry;
			telemetry.submitted_fence = submitted;
			telemetry.completed_fence = fence->GetCompletedValue();
			telemetry.deferred_releases = retired_textures.size();
			telemetry.cpu_frame_ms =
				std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			return telemetry;
		}

		void resize(const std::uint32_t new_width, const std::uint32_t new_height) {
			if (new_width == 0 || new_height == 0 || (new_width == width && new_height == height)) {
				return;
			}
			waitIdle();
			for (ComPtr<ID3D12Resource>& buffer : back_buffers) {
				buffer.Reset();
			}
			checkHr(swap_chain->ResizeBuffers(kFrameCount, new_width, new_height, kBackBufferFormat,
				swap_chain_flags),
				"IDXGISwapChain::ResizeBuffers");
			width = new_width;
			height = new_height;
			createBackBufferViews();
			for (FrameContext& frame : frames) {
				frame.fence_value = 0;
			}
		}

		void waitIdle() {
			if (!queue || !fence) {
				return;
			}
			const std::uint64_t value = signalQueue();
			waitForFence(value);
			collectRetired(false);
		}
	};

	D3D12Renderer::D3D12Renderer(HWND window, const std::uint32_t width,
		const std::uint32_t height, const RendererOptions options)
		: impl_(std::make_unique<Impl>(window, width, height, options)) {
	}

	D3D12Renderer::~D3D12Renderer() = default;

	TextureHandle D3D12Renderer::createTexture(const TextureUpload& upload) {
		return impl_->createTexture(upload);
	}

	TextureHandle D3D12Renderer::registerExternalTexture(
		ID3D12Resource* resource, const std::uint32_t width, const std::uint32_t height,
		const std::uint32_t resource_state) {
		return impl_->registerExternalTexture(
			resource, width, height, static_cast<D3D12_RESOURCE_STATES>(resource_state));
	}

	void D3D12Renderer::updateTexture(const TextureHandle handle, const TextureUpload& upload) {
		impl_->updateTexture(handle, upload);
	}

	void D3D12Renderer::setExternalTextureState(const TextureHandle handle,
		const std::uint32_t resource_state) {
		impl_->setExternalTextureState(handle, static_cast<D3D12_RESOURCE_STATES>(resource_state));
	}

	void D3D12Renderer::releaseTexture(const TextureHandle handle) { impl_->releaseTexture(handle); }

	FrameTelemetry D3D12Renderer::render(const RenderPlan& plan) { return impl_->render(plan); }

	void D3D12Renderer::resize(const std::uint32_t width, const std::uint32_t height) {
		impl_->resize(width, height);
	}

	void D3D12Renderer::waitIdle() { impl_->waitIdle(); }

	std::string D3D12Renderer::adapterName() const { return impl_->adapter_name; }

	IDXGIAdapter1* D3D12Renderer::nativeAdapter() const noexcept { return impl_->adapter.Get(); }

	ID3D12Device* D3D12Renderer::nativeDevice() const noexcept { return impl_->device.Get(); }

	ID3D12CommandQueue* D3D12Renderer::nativeQueue() const noexcept { return impl_->queue.Get(); }

	std::size_t D3D12Renderer::deferredReleaseCount() const noexcept {
		return impl_->retired_textures.size();
	}

}  // namespace gpu2d
