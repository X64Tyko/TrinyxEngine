// VOLK_IMPLEMENTATION must be defined exactly once — this TU provides Vulkan function bodies.
#define VOLK_IMPLEMENTATION
#include "VulkanContext.h"

// VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE must appear exactly once per program,
// in the same TU as VOLK_IMPLEMENTATION so both live in the same object file.
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstring>
#include <set>

#include "Logger.h"

// Validation layer callback
static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
	VkDebugUtilsMessageSeverityFlagBitsEXT severity,
	VkDebugUtilsMessageTypeFlagsEXT /*type*/,
	const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
	void* /*userData*/)
{
	if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
	{
		LOG_ENG_WARN_F("[Vulkan Validation] %s", callbackData->pMessage);
	}
	return VK_FALSE;
}

// VulkanContext::Initialize
bool VulkanContext::Initialize(SDL_Window* window, bool enableValidation)
{
	bValidationEnabled = enableValidation;

	if (volkInitialize() != VK_SUCCESS)
	{
		LOG_ENG_ERROR("[VulkanContext] volkInitialize failed – Vulkan loader not found");
		return false;
	}

	// Seed vulkan.hpp's global dynamic dispatcher with volk's loader entry point.
	// raii objects also carry their own dispatchers seeded from this.
	VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);

	// Create the raii Context (uses its own DynamicLoader internally).
	VkContext = vk::raii::Context{};

	if (!CreateInstance(window, enableValidation)) return false;
	// Populate volk's global function table for the C recording path.
	volkLoadInstance(*Instance);

	if (enableValidation && !SetupDebugMessenger())
	{
		LOG_ENG_WARN("[VulkanContext] Could not set up debug messenger (continuing without validation output)");
	}

	if (!CreateSurface(window)) return false;
	if (!SelectPhysicalDevice()) return false;
	if (!CreateLogicalDevice()) return false;
	volkLoadDevice(*Device);

	if (!CreateCommandPools()) return false;
	if (!CreateSwapchain(window)) return false;
	// Depth image is owned by VulkRender (VMA-allocated, recreated on resize).

	LOG_ENG_INFO("[VulkanContext] Initialized successfully");
	return true;
}

// VulkanContext::Shutdown
void VulkanContext::Shutdown()
{
	if (*Instance == VK_NULL_HANDLE) return; // already shut down

	vkDeviceWaitIdle(*Device);

	// Destroy owned objects in strict dependency order.
	// raii move-assign from nullptr triggers the underlying vkDestroy* call.
	DestroySwapchain();
	TransferCommandPool = vk::raii::CommandPool{nullptr};
	ComputeCommandPool  = vk::raii::CommandPool{nullptr};
	GraphicsCommandPool = vk::raii::CommandPool{nullptr};
	Device              = vk::raii::Device{nullptr};
	PhysicalDevice      = vk::raii::PhysicalDevice{nullptr};
	Surface             = vk::raii::SurfaceKHR{nullptr};
	DebugMessenger      = vk::raii::DebugUtilsMessengerEXT{nullptr};
	Instance            = vk::raii::Instance{nullptr};
}

// VulkanContext::RecreateSwapchain
bool VulkanContext::RecreateSwapchain(SDL_Window* window)
{
	vkDeviceWaitIdle(*Device);
	DestroySwapchain();
	return CreateSwapchain(window);
	// VulkRender recreates its depth image separately after receiving the resize notification.
}

// Instance creation
bool VulkanContext::CreateInstance(SDL_Window* /*window*/, bool enableValidation)
{
	try
	{
		unsigned int sdlExtCount   = 0;
		const char* const* sdlExts = SDL_Vulkan_GetInstanceExtensions(&sdlExtCount);

		std::vector<const char*> extensions(sdlExts, sdlExts + sdlExtCount);
		if (enableValidation) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

		static const char* ValidationLayer = "VK_LAYER_KHRONOS_validation";
		std::vector<const char*> layers;
		if (enableValidation)
		{
			auto available = VkContext.enumerateInstanceLayerProperties();
			bool found     = false;
			for (const auto& layer : available)
				if (strcmp(layer.layerName, ValidationLayer) == 0)
				{
					found = true;
					break;
				}
			if (found) layers.push_back(ValidationLayer);
			else LOG_ENG_WARN("[VulkanContext] Validation layer requested but not available");
		}

		vk::ApplicationInfo appInfo{};
		appInfo.pApplicationName   = "TrinyxEngine";
		appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
		appInfo.pEngineName        = "TrinyxEngine";
		appInfo.engineVersion      = VK_MAKE_VERSION(0, 1, 0);
		appInfo.apiVersion         = VK_API_VERSION_1_4;

		vk::InstanceCreateInfo createInfo{};
		createInfo.pApplicationInfo        = &appInfo;
		createInfo.enabledExtensionCount   = static_cast<uint32_t>(extensions.size());
		createInfo.ppEnabledExtensionNames = extensions.data();
		createInfo.enabledLayerCount       = static_cast<uint32_t>(layers.size());
		createInfo.ppEnabledLayerNames     = layers.data();

		Instance = VkContext.createInstance(createInfo);
		LOG_ENG_INFO("[VulkanContext] Vulkan instance created");
		return true;
	}
	catch (const vk::SystemError& e)
	{
		LOG_ENG_ERROR_F("[VulkanContext] vkCreateInstance failed: %s", e.what());
		return false;
	}
}

// Debug messenger
bool VulkanContext::SetupDebugMessenger()
{
	// Use C structs here so DebugCallback's C-API signature matches without a cast.
	// The resulting handle is wrapped in raii::DebugUtilsMessengerEXT for auto-cleanup.
	VkDebugUtilsMessengerCreateInfoEXT createInfo{};
	createInfo.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
	createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
	createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
	createInfo.pfnUserCallback = DebugCallback;

	VkDebugUtilsMessengerEXT rawMessenger = VK_NULL_HANDLE;
	VkResult result                       = vkCreateDebugUtilsMessengerEXT(*Instance, &createInfo, nullptr, &rawMessenger);
	if (result != VK_SUCCESS)
	{
		LOG_ENG_WARN_F("[VulkanContext] vkCreateDebugUtilsMessengerEXT failed: %d", result);
		return false;
	}
	DebugMessenger = vk::raii::DebugUtilsMessengerEXT{Instance, rawMessenger};
	return true;
}

// Surface
bool VulkanContext::CreateSurface(SDL_Window* window)
{
	VkSurfaceKHR rawSurface = VK_NULL_HANDLE;
	if (!SDL_Vulkan_CreateSurface(window, *Instance, nullptr, &rawSurface))
	{
		LOG_ENG_ERROR_F("[VulkanContext] SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
		return false;
	}
	// Wrap the SDL-provided C handle in a raii::SurfaceKHR (takes ownership of destruction).
	Surface = vk::raii::SurfaceKHR{Instance, rawSurface};
	return true;
}

// Physical device selection
bool VulkanContext::SelectPhysicalDevice()
{
	try
	{
		auto physDevices = Instance.enumeratePhysicalDevices();
		if (physDevices.empty())
		{
			LOG_ENG_ERROR("[VulkanContext] No Vulkan-capable GPUs found");
			return false;
		}

		// Prefer discrete GPU; fall back to first device that has VK_KHR_SWAPCHAIN.
		// Everything else we need is core in Vulkan 1.2 / 1.3 / 1.4.
		size_t selectedIdx = physDevices.size();
		size_t fallbackIdx = physDevices.size();

		for (size_t i = 0; i < physDevices.size(); ++i)
		{
			auto& dev = physDevices[i];
			auto exts = dev.enumerateDeviceExtensionProperties();

			bool hasSwapchain = false;
			for (const auto& ext : exts)
				if (strcmp(ext.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0)
				{
					hasSwapchain = true;
					break;
				}
			if (!hasSwapchain) continue;

			// Track optional shader-object extension.
			for (const auto& ext : exts)
				if (strcmp(ext.extensionName, VK_EXT_SHADER_OBJECT_EXTENSION_NAME) == 0) bSupportsShaderObject = true;

			auto props = dev.getProperties();
			if (props.deviceType == vk::PhysicalDeviceType::eDiscreteGpu)
			{
				selectedIdx = i;
				LOG_ENG_INFO_F("[VulkanContext] Selected discrete GPU: %s", props.deviceName.data());
				break;
			}
			if (fallbackIdx == physDevices.size()) fallbackIdx = i;
		}

		if (selectedIdx == physDevices.size()) selectedIdx = fallbackIdx;
		if (selectedIdx == physDevices.size())
		{
			LOG_ENG_ERROR("[VulkanContext] No suitable GPU found (VK_KHR_SWAPCHAIN not supported)");
			return false;
		}

		PhysicalDevice = std::move(physDevices[selectedIdx]);

		// ---- Feature queries ----

		// Vulkan 1.4 features (hostImageCopy, pushDescriptor, indexTypeUint8).
		// Vulkan 1.2 features (bufferDeviceAddress, timelineSemaphore).
		// Dynamic rendering and synchronization2 are core in 1.3 — assumed available.
		{
			VkPhysicalDeviceVulkan14Features vk14{};
			vk14.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES;

			VkPhysicalDeviceVulkan12Features vk12{};
			vk12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
			vk12.pNext = &vk14;

			VkPhysicalDeviceFeatures2 features2{};
			features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
			features2.pNext = &vk12;

			vkGetPhysicalDeviceFeatures2(*PhysicalDevice, &features2);

			bSupportsBufferDeviceAddress = (vk12.bufferDeviceAddress == VK_TRUE);
			bSupportsHostImageCopy       = (vk14.hostImageCopy == VK_TRUE);
			bSupportsPushDescriptors     = (vk14.pushDescriptor == VK_TRUE);
			bSupportsIndexTypeUint8      = (vk14.indexTypeUint8 == VK_TRUE);
		}

		bSupportsDynamicRendering  = true; // Core in 1.3 — implied by 1.4 device
		bSupportsTimelineSemaphore = true; // Core in 1.2

		// ---- ReBAR detection ----
		auto memProps = PhysicalDevice.getMemoryProperties();
		for (uint32_t i = 0; i < memProps.memoryHeapCount; ++i)
		{
			if (!(memProps.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal)) continue;
			for (uint32_t j = 0; j < memProps.memoryTypeCount; ++j)
			{
				if (memProps.memoryTypes[j].heapIndex != i) continue;
				constexpr auto ReBarFlags = vk::MemoryPropertyFlagBits::eDeviceLocal |
					vk::MemoryPropertyFlagBits::eHostVisible;
				if ((memProps.memoryTypes[j].propertyFlags & ReBarFlags) == ReBarFlags)
				{
					if (memProps.memoryHeaps[i].size >= 256ull * 1024 * 1024)
					{
						bHasReBAR      = true;
						ReBarHeapIndex = j;
						LOG_ENG_INFO_F("[VulkanContext] ReBAR detected: heap %u, memory type %u (%.0f GB)",
									   i, j,
								   static_cast<double>(memProps.memoryHeaps[i].size) /
								   (1024.0 * 1024.0 * 1024.0));
					}
				}
			}
		}

		if (!bHasReBAR) LOG_ENG_INFO("[VulkanContext] ReBAR not detected – will use staging buffer for delta uploads");

		LOG_ENG_INFO_F("[VulkanContext] Buffer device address: %s", bSupportsBufferDeviceAddress ? "YES" : "NO");
		LOG_ENG_INFO_F("[VulkanContext] Host image copy:       %s", bSupportsHostImageCopy ? "YES" : "NO");
		LOG_ENG_INFO_F("[VulkanContext] Push descriptors:      %s", bSupportsPushDescriptors ? "YES" : "NO");
		LOG_ENG_INFO_F("[VulkanContext] Shader objects:        %s",
					   bSupportsShaderObject ? "YES" : "NO (will use traditional pipelines)");

		return true;
	}
	catch (const vk::SystemError& e)
	{
		LOG_ENG_ERROR_F("[VulkanContext] Physical device selection failed: %s", e.what());
		return false;
	}
}

// Logical device creation
bool VulkanContext::CreateLogicalDevice()
{
	try
	{
		// Find queue families
		auto families = PhysicalDevice.getQueueFamilyProperties();

		for (uint32_t i = 0; i < static_cast<uint32_t>(families.size()); ++i)
		{
			bool presentSupport = PhysicalDevice.getSurfaceSupportKHR(i, *Surface);

			if ((families[i].queueFlags & vk::QueueFlagBits::eGraphics) && presentSupport &&
				Queues.GraphicsFamily == UINT32_MAX)
			{
				Queues.GraphicsFamily = i;
			}

			if ((families[i].queueFlags & vk::QueueFlagBits::eCompute) &&
				!(families[i].queueFlags & vk::QueueFlagBits::eGraphics) &&
				Queues.ComputeFamily == UINT32_MAX)
			{
				Queues.ComputeFamily = i;
			}

			if ((families[i].queueFlags & vk::QueueFlagBits::eTransfer) &&
				!(families[i].queueFlags & vk::QueueFlagBits::eGraphics) &&
				!(families[i].queueFlags & vk::QueueFlagBits::eCompute) &&
				Queues.TransferFamily == UINT32_MAX)
			{
				Queues.TransferFamily = i;
			}
		}

		if (Queues.GraphicsFamily == UINT32_MAX)
		{
			LOG_ENG_ERROR("[VulkanContext] No graphics+present queue family found");
			return false;
		}
		if (Queues.ComputeFamily == UINT32_MAX) Queues.ComputeFamily = Queues.GraphicsFamily;
		if (Queues.TransferFamily == UINT32_MAX) Queues.TransferFamily = Queues.GraphicsFamily;

		std::set<uint32_t> uniqueFamilies = {
			Queues.GraphicsFamily, Queues.ComputeFamily, Queues.TransferFamily
		};

		float queuePriority = 1.0f;
		std::vector<vk::DeviceQueueCreateInfo> queueCreateInfos;
		for (uint32_t family : uniqueFamilies)
		{
			vk::DeviceQueueCreateInfo info{};
			info.queueFamilyIndex = family;
			info.queueCount       = 1;
			info.pQueuePriorities = &queuePriority;
			queueCreateInfos.push_back(info);
		}

		// Device extensions: only swapchain required — everything else is core in 1.4.
		std::vector<const char*> deviceExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
		if (bSupportsShaderObject) deviceExtensions.push_back(VK_EXT_SHADER_OBJECT_EXTENSION_NAME);

		// Feature chain using StructureChain for automatic pNext wiring.
		auto featureChain = vk::StructureChain<
			vk::PhysicalDeviceFeatures2,
			vk::PhysicalDeviceVulkan14Features,
			vk::PhysicalDeviceVulkan13Features,
			vk::PhysicalDeviceVulkan12Features,
			vk::PhysicalDeviceVulkan11Features>{};

		featureChain.get<vk::PhysicalDeviceFeatures2>()
					.features.setSamplerAnisotropy(true)
					.setMultiDrawIndirect(true)
					.setDrawIndirectFirstInstance(true)
					.setShaderInt64(true)
					.setIndependentBlend(true); // pick pipeline uses different write masks per attachment

		featureChain.get<vk::PhysicalDeviceVulkan11Features>()
					.setShaderDrawParameters(true);

		featureChain.get<vk::PhysicalDeviceVulkan13Features>()
					.setDynamicRendering(true)
					.setSynchronization2(true);

		featureChain.get<vk::PhysicalDeviceVulkan12Features>()
					.setBufferDeviceAddress(bSupportsBufferDeviceAddress)
					.setTimelineSemaphore(true)
					.setDescriptorIndexing(true)
					.setShaderSampledImageArrayNonUniformIndexing(true)
					.setDescriptorBindingVariableDescriptorCount(true);

		// Enable supported Vulkan 1.4 features.
		featureChain.get<vk::PhysicalDeviceVulkan14Features>()
					.setHostImageCopy(bSupportsHostImageCopy)
					.setPushDescriptor(bSupportsPushDescriptors)
					.setIndexTypeUint8(bSupportsIndexTypeUint8);

		vk::DeviceCreateInfo deviceCreateInfo{};
		deviceCreateInfo.pNext                   = &featureChain.get<vk::PhysicalDeviceFeatures2>();
		deviceCreateInfo.queueCreateInfoCount    = static_cast<uint32_t>(queueCreateInfos.size());
		deviceCreateInfo.pQueueCreateInfos       = queueCreateInfos.data();
		deviceCreateInfo.enabledExtensionCount   = static_cast<uint32_t>(deviceExtensions.size());
		deviceCreateInfo.ppEnabledExtensionNames = deviceExtensions.data();
		// pEnabledFeatures must be null when using pNext VkPhysicalDeviceFeatures2.
		deviceCreateInfo.pEnabledFeatures = nullptr;

		Device = PhysicalDevice.createDevice(deviceCreateInfo);

		// Retrieve queue handles via volk (volkLoadDevice populates the globals).
		auto getQ = [this](uint32_t family) -> vk::Queue
		{
			VkQueue q = VK_NULL_HANDLE;
			vkGetDeviceQueue(*Device, family, 0, &q);
			return vk::Queue{q};
		};
		Queues.Graphics = getQ(Queues.GraphicsFamily);
		Queues.Compute  = getQ(Queues.ComputeFamily);
		Queues.Transfer = getQ(Queues.TransferFamily);

		LOG_ENG_INFO_F("[VulkanContext] Logical device created (Graphics=%u, Compute=%u, Transfer=%u)",
					   Queues.GraphicsFamily, Queues.ComputeFamily, Queues.TransferFamily);
		return true;
	}
	catch (const vk::SystemError& e)
	{
		LOG_ENG_ERROR_F("[VulkanContext] vkCreateDevice failed: %s", e.what());
		return false;
	}
}

// Command pools
bool VulkanContext::CreateCommandPools()
{
	try
	{
		auto makePool = [this](uint32_t family) -> vk::raii::CommandPool
		{
			vk::CommandPoolCreateInfo info{};
			info.flags            = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
			info.queueFamilyIndex = family;
			return Device.createCommandPool(info);
		};

		GraphicsCommandPool = makePool(Queues.GraphicsFamily);
		ComputeCommandPool  = makePool(Queues.ComputeFamily);
		TransferCommandPool = makePool(Queues.TransferFamily);
		return true;
	}
	catch (const vk::SystemError& e)
	{
		LOG_ENG_ERROR_F("[VulkanContext] Failed to create command pools: %s", e.what());
		return false;
	}
}

// Swapchain
bool VulkanContext::CreateSwapchain(SDL_Window* window)
{
	try
	{
		auto caps         = PhysicalDevice.getSurfaceCapabilitiesKHR(*Surface);
		auto formats      = PhysicalDevice.getSurfaceFormatsKHR(*Surface);
		auto presentModes = PhysicalDevice.getSurfacePresentModesKHR(*Surface);

		auto surfaceFormat = ChooseSurfaceFormat(formats);
		auto presentMode   = ChoosePresentMode(presentModes);
		auto extent        = ChooseExtent(caps, window);

		uint32_t imageCount = caps.minImageCount + 1;
		if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

		vk::SwapchainCreateInfoKHR createInfo{};
		createInfo.surface          = *Surface;
		createInfo.minImageCount    = imageCount;
		createInfo.imageFormat      = surfaceFormat.format;
		createInfo.imageColorSpace  = surfaceFormat.colorSpace;
		createInfo.imageExtent      = extent;
		createInfo.imageArrayLayers = 1;
		createInfo.imageUsage       = vk::ImageUsageFlagBits::eColorAttachment
			| vk::ImageUsageFlagBits::eTransferDst;

		uint32_t indices[] = {Queues.GraphicsFamily, Queues.ComputeFamily};
		if (Queues.GraphicsFamily != Queues.ComputeFamily)
		{
			createInfo.imageSharingMode      = vk::SharingMode::eConcurrent;
			createInfo.queueFamilyIndexCount = 2;
			createInfo.pQueueFamilyIndices   = indices;
		}
		else
		{
			createInfo.imageSharingMode = vk::SharingMode::eExclusive;
		}

		createInfo.preTransform   = caps.currentTransform;
		createInfo.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
		createInfo.presentMode    = presentMode;
		createInfo.clipped        = VK_TRUE;

		Swapchain.Handle = Device.createSwapchainKHR(createInfo);
		Swapchain.Format = surfaceFormat.format;
		Swapchain.Extent = extent;

		// Images are non-owning (owned by the swapchain handle).
		auto images = Swapchain.Handle.getImages();
		for (vk::Image img : images)
		{
			Swapchain.Images.push_back(img);

			vk::ImageViewCreateInfo viewInfo{};
			viewInfo.image            = img;
			viewInfo.viewType         = vk::ImageViewType::e2D;
			viewInfo.format           = surfaceFormat.format;
			viewInfo.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
			Swapchain.ImageViews.push_back(Device.createImageView(viewInfo));
		}

		LOG_ENG_INFO_F("[VulkanContext] Swapchain created: %ux%u, %zu images, format %d",
					   extent.width, extent.height, images.size(),
				   static_cast<int>(surfaceFormat.format));
		return true;
	}
	catch (const vk::SystemError& e)
	{
		LOG_ENG_ERROR_F("[VulkanContext] Swapchain creation failed: %s", e.what());
		return false;
	}
}

void VulkanContext::DestroySwapchain()
{
	Swapchain.ImageViews.clear(); // each raii::ImageView destructs → vkDestroyImageView
	Swapchain.Images.clear();
	Swapchain.Handle = vk::raii::SwapchainKHR{nullptr}; // raii destructs → vkDestroySwapchainKHR
}

// Helpers
vk::SurfaceFormatKHR VulkanContext::ChooseSurfaceFormat(
	const std::vector<vk::SurfaceFormatKHR>& available) const
{
	for (const auto& fmt : available)
		if (fmt.format == vk::Format::eB8G8R8A8Srgb &&
			fmt.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear)
			return fmt;
	return available[0];
}

vk::PresentModeKHR VulkanContext::ChoosePresentMode(
	const std::vector<vk::PresentModeKHR>& available) const
{
	// Prefer mailbox (triple-buffered, tear-free, no VSync stall)
	for (auto mode : available) if (mode == vk::PresentModeKHR::eMailbox) return mode;
	return vk::PresentModeKHR::eFifo; // always available
}

vk::Extent2D VulkanContext::ChooseExtent(const vk::SurfaceCapabilitiesKHR& caps,
										 SDL_Window* window) const
{
	if (caps.currentExtent.width != UINT32_MAX) return caps.currentExtent;

	int w = 0, h = 0;
	SDL_GetWindowSizeInPixels(window, &w, &h);

	vk::Extent2D extent{};
	extent.width = std::clamp(static_cast<uint32_t>(w),
							  caps.minImageExtent.width, caps.maxImageExtent.width);
	extent.height = std::clamp(static_cast<uint32_t>(h),
							   caps.minImageExtent.height, caps.maxImageExtent.height);
	return extent;
}

VkFormat VulkanContext::FindSupportedFormat(const std::vector<VkFormat>& candidates,
											VkImageTiling tiling,
											VkFormatFeatureFlags features) const
{
	for (VkFormat fmt : candidates)
	{
		auto props = PhysicalDevice.getFormatProperties(static_cast<vk::Format>(fmt));

		if (tiling == VK_IMAGE_TILING_LINEAR &&
			(static_cast<VkFormatFeatureFlags>(props.linearTilingFeatures) & features) == features)
			return fmt;
		if (tiling == VK_IMAGE_TILING_OPTIMAL &&
			(static_cast<VkFormatFeatureFlags>(props.optimalTilingFeatures) & features) == features)
			return fmt;
	}
	return VK_FORMAT_UNDEFINED;
}
