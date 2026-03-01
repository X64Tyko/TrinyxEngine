// VOLK_IMPLEMENTATION must be defined exactly once before including volk.h
// This generates the Vulkan function loader bodies in this translation unit.
#define VOLK_IMPLEMENTATION
#include "VulkanContext.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <set>

#include "Logger.h"

// -----------------------------------------------------------------------
// Validation layer callback
// -----------------------------------------------------------------------
static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
	VkDebugUtilsMessageSeverityFlagBitsEXT severity,
	VkDebugUtilsMessageTypeFlagsEXT /*type*/,
	const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
	void* /*userData*/)
{
	if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
	{
		LOG_WARN_F("[Vulkan Validation] %s", callbackData->pMessage);
	}
	return VK_FALSE;
}

// -----------------------------------------------------------------------
// VulkanContext::Initialize
// -----------------------------------------------------------------------
bool VulkanContext::Initialize(SDL_Window* window, bool enableValidation)
{
	bValidationEnabled = enableValidation;

	if (volkInitialize() != VK_SUCCESS)
	{
		LOG_ERROR("[VulkanContext] volkInitialize failed – Vulkan loader not found");
		return false;
	}

	if (!CreateInstance(window, enableValidation)) return false;
	volkLoadInstance(Instance);

	if (enableValidation && !SetupDebugMessenger())
	{
		LOG_WARN("[VulkanContext] Could not set up debug messenger (continuing without validation output)");
	}

	if (!CreateSurface(window)) return false;
	if (!SelectPhysicalDevice()) return false;
	if (!CreateLogicalDevice()) return false;
	volkLoadDevice(Device);

	if (!CreateCommandPools()) return false;
	if (!CreateSwapchain(window)) return false;
	// Depth image is owned by RenderThread (VMA-allocated, recreated on resize).

	LOG_INFO("[VulkanContext] Initialized successfully");
	return true;
}

// -----------------------------------------------------------------------
// VulkanContext::Shutdown
// -----------------------------------------------------------------------
void VulkanContext::Shutdown()
{
	if (Device != VK_NULL_HANDLE) vkDeviceWaitIdle(Device);

	DestroySwapchain();

	if (GraphicsCommandPool != VK_NULL_HANDLE)
	{
		vkDestroyCommandPool(Device, GraphicsCommandPool, nullptr);
		GraphicsCommandPool = VK_NULL_HANDLE;
	}
	if (ComputeCommandPool != VK_NULL_HANDLE)
	{
		vkDestroyCommandPool(Device, ComputeCommandPool, nullptr);
		ComputeCommandPool = VK_NULL_HANDLE;
	}
	if (TransferCommandPool != VK_NULL_HANDLE)
	{
		vkDestroyCommandPool(Device, TransferCommandPool, nullptr);
		TransferCommandPool = VK_NULL_HANDLE;
	}

	if (Device != VK_NULL_HANDLE)
	{
		vkDestroyDevice(Device, nullptr);
		Device = VK_NULL_HANDLE;
	}

	if (Surface != VK_NULL_HANDLE)
	{
		vkDestroySurfaceKHR(Instance, Surface, nullptr);
		Surface = VK_NULL_HANDLE;
	}

	if (DebugMessenger != VK_NULL_HANDLE)
	{
		vkDestroyDebugUtilsMessengerEXT(Instance, DebugMessenger, nullptr);
		DebugMessenger = VK_NULL_HANDLE;
	}

	if (Instance != VK_NULL_HANDLE)
	{
		vkDestroyInstance(Instance, nullptr);
		Instance = VK_NULL_HANDLE;
	}

	LOG_INFO("[VulkanContext] Shutdown complete");
}

// -----------------------------------------------------------------------
// VulkanContext::RecreateSwapchain
// -----------------------------------------------------------------------
bool VulkanContext::RecreateSwapchain(SDL_Window* window)
{
	vkDeviceWaitIdle(Device);
	DestroySwapchain();
	return CreateSwapchain(window);
	// RenderThread recreates its depth image separately after receiving the resize notification.
}

// -----------------------------------------------------------------------
// Instance creation
// -----------------------------------------------------------------------
bool VulkanContext::CreateInstance(SDL_Window* /*window*/, bool enableValidation)
{
	// Collect extensions required by SDL3
	unsigned int sdlExtCount   = 0;
	const char* const* sdlExts = SDL_Vulkan_GetInstanceExtensions(&sdlExtCount);

	std::vector<const char*> extensions(sdlExts, sdlExts + sdlExtCount);
	if (enableValidation) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

	// Optional: portability enumeration (required on macOS/MoltenVK)
	//extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);

	// Validation layers
	static const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
	std::vector<const char*> layers;
	if (enableValidation)
	{
		uint32_t layerCount = 0;
		vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
		std::vector<VkLayerProperties> available(layerCount);
		vkEnumerateInstanceLayerProperties(&layerCount, available.data());

		bool found = false;
		for (auto& layer : available)
		{
			if (strcmp(layer.layerName, kValidationLayer) == 0)
			{
				found = true;
				break;
			}
		}
		if (found) layers.push_back(kValidationLayer);
		else
			LOG_WARN("[VulkanContext] Validation layer requested but not available");
	}

	VkApplicationInfo appInfo{};
	appInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	appInfo.pApplicationName   = "StrigidEngine";
	appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
	appInfo.pEngineName        = "StrigidEngine";
	appInfo.engineVersion      = VK_MAKE_VERSION(0, 1, 0);
	appInfo.apiVersion         = VK_API_VERSION_1_4;

	VkInstanceCreateInfo createInfo{};
	createInfo.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	createInfo.pApplicationInfo        = &appInfo;
	createInfo.enabledExtensionCount   = static_cast<uint32_t>(extensions.size());
	createInfo.ppEnabledExtensionNames = extensions.data();
	createInfo.enabledLayerCount       = static_cast<uint32_t>(layers.size());
	createInfo.ppEnabledLayerNames     = layers.data();

	VkResult result = vkCreateInstance(&createInfo, nullptr, &Instance);
	if (result != VK_SUCCESS)
	{
		LOG_ERROR_F("[VulkanContext] vkCreateInstance failed: %d", result);
		return false;
	}

	LOG_INFO("[VulkanContext] Vulkan instance created");
	return true;
}

// -----------------------------------------------------------------------
// Debug messenger
// -----------------------------------------------------------------------
bool VulkanContext::SetupDebugMessenger()
{
	VkDebugUtilsMessengerCreateInfoEXT createInfo{};
	createInfo.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
	createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
	createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
	createInfo.pfnUserCallback = DebugCallback;

	VkResult result = vkCreateDebugUtilsMessengerEXT(Instance, &createInfo, nullptr, &DebugMessenger);
	if (result != VK_SUCCESS)
	{
		LOG_WARN_F("[VulkanContext] vkCreateDebugUtilsMessengerEXT failed: %d", result);
		return false;
	}
	return true;
}

// -----------------------------------------------------------------------
// Surface
// -----------------------------------------------------------------------
bool VulkanContext::CreateSurface(SDL_Window* window)
{
	if (!SDL_Vulkan_CreateSurface(window, Instance, nullptr, &Surface))
	{
		LOG_ERROR_F("[VulkanContext] SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
		return false;
	}
	return true;
}

// -----------------------------------------------------------------------
// Physical device selection
// -----------------------------------------------------------------------
bool VulkanContext::SelectPhysicalDevice()
{
	uint32_t deviceCount = 0;
	vkEnumeratePhysicalDevices(Instance, &deviceCount, nullptr);
	if (deviceCount == 0)
	{
		LOG_ERROR("[VulkanContext] No Vulkan-capable GPUs found");
		return false;
	}

	std::vector<VkPhysicalDevice> devices(deviceCount);
	vkEnumeratePhysicalDevices(Instance, &deviceCount, devices.data());

	// Required device extensions
	std::vector<const char*> required = {
		VK_KHR_SWAPCHAIN_EXTENSION_NAME,
		VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
		VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
		VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
		VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
		VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME,
		VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME,
		VK_KHR_MULTIVIEW_EXTENSION_NAME,
		VK_KHR_MAINTENANCE2_EXTENSION_NAME,
	};

	// Prefer discrete GPU; fallback to first device that has required extensions
	VkPhysicalDevice fallback = VK_NULL_HANDLE;
	for (VkPhysicalDevice dev : devices)
	{
		if (!CheckDeviceExtensionSupport(dev, required)) continue;

		VkPhysicalDeviceProperties props{};
		vkGetPhysicalDeviceProperties(dev, &props);

		if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
		{
			PhysicalDevice = dev;
			LOG_INFO_F("[VulkanContext] Selected discrete GPU: %s", props.deviceName);
			break;
		}
		if (fallback == VK_NULL_HANDLE) fallback = dev;
	}

	if (PhysicalDevice == VK_NULL_HANDLE) PhysicalDevice = fallback;

	if (PhysicalDevice == VK_NULL_HANDLE)
	{
		LOG_ERROR("[VulkanContext] No suitable GPU found (required extensions not supported)");
		return false;
	}

	// Check optional features
	VkPhysicalDeviceFeatures2 features2{};
	features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

	VkPhysicalDeviceShaderObjectFeaturesEXT shaderObjFeatures{};
	shaderObjFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT;

	VkPhysicalDeviceBufferDeviceAddressFeaturesKHR bdaFeatures{};
	bdaFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_KHR;
	bdaFeatures.pNext = &shaderObjFeatures;
	features2.pNext   = &bdaFeatures;

	vkGetPhysicalDeviceFeatures2(PhysicalDevice, &features2);
	bSupportsBufferDeviceAddress = (bdaFeatures.bufferDeviceAddress == VK_TRUE);
	bSupportsDynamicRendering    = true; // Required extension implies support
	bSupportsTimelineSemaphore   = true; // Required extension implies support

	uint32_t extCount = 0;
	vkEnumerateDeviceExtensionProperties(PhysicalDevice, nullptr, &extCount, nullptr);
	std::vector<VkExtensionProperties> exts(extCount);
	vkEnumerateDeviceExtensionProperties(PhysicalDevice, nullptr, &extCount, exts.data());
	for (auto& ext : exts)
	{
		if (strcmp(ext.extensionName, VK_EXT_SHADER_OBJECT_EXTENSION_NAME) == 0) bSupportsShaderObject = true;
	}

	// ReBAR detection: look for a heap that is both DEVICE_LOCAL and HOST_VISIBLE
	VkPhysicalDeviceMemoryProperties memProps{};
	vkGetPhysicalDeviceMemoryProperties(PhysicalDevice, &memProps);
	for (uint32_t i = 0; i < memProps.memoryHeapCount; ++i)
	{
		if ((memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0) continue;
		// Check if any memory type in this heap is both DEVICE_LOCAL and HOST_VISIBLE
		for (uint32_t j = 0; j < memProps.memoryTypeCount; ++j)
		{
			if (memProps.memoryTypes[j].heapIndex != i) continue;
			constexpr VkMemoryPropertyFlags kReBarFlags =
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
			if ((memProps.memoryTypes[j].propertyFlags & kReBarFlags) == kReBarFlags)
			{
				// ReBAR: the heap must be large (>= 256 MB) to exclude fallback bars
				if (memProps.memoryHeaps[i].size >= 256ull * 1024 * 1024)
				{
					bHasReBAR      = true;
					ReBarHeapIndex = j;
					LOG_INFO_F("[VulkanContext] ReBAR detected: heap %u, memory type %u (%.0f GB)",
							   i, j,
							   static_cast<double>(memProps.memoryHeaps[i].size) / (1024.0*1024.0*1024.0));
				}
			}
		}
	}

	if (!bHasReBAR)
		LOG_INFO("[VulkanContext] ReBAR not detected – will use staging buffer for delta uploads");

	LOG_INFO_F("[VulkanContext] Buffer device address: %s",
			   bSupportsBufferDeviceAddress ? "YES" : "NO");
	LOG_INFO_F("[VulkanContext] Shader objects:        %s",
			   bSupportsShaderObject ? "YES" : "NO (will use traditional pipelines)");

	return true;
}

// -----------------------------------------------------------------------
// Logical device creation
// -----------------------------------------------------------------------
bool VulkanContext::CreateLogicalDevice()
{
	// Find queue families
	uint32_t queueFamilyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(PhysicalDevice, &queueFamilyCount, nullptr);
	std::vector<VkQueueFamilyProperties> families(queueFamilyCount);
	vkGetPhysicalDeviceQueueFamilyProperties(PhysicalDevice, &queueFamilyCount, families.data());

	for (uint32_t i = 0; i < queueFamilyCount; ++i)
	{
		// Graphics + present must be the same family for simplicity
		VkBool32 presentSupport = VK_FALSE;
		vkGetPhysicalDeviceSurfaceSupportKHR(PhysicalDevice, i, Surface, &presentSupport);

		if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && presentSupport &&
			Queues.GraphicsFamily == UINT32_MAX)
		{
			Queues.GraphicsFamily = i;
		}

		// Prefer a dedicated compute queue (different from graphics)
		if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
			!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
			Queues.ComputeFamily == UINT32_MAX)
		{
			Queues.ComputeFamily = i;
		}

		// Prefer a dedicated transfer queue
		if ((families[i].queueFlags & VK_QUEUE_TRANSFER_BIT) &&
			!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
			!(families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
			Queues.TransferFamily == UINT32_MAX)
		{
			Queues.TransferFamily = i;
		}
	}

	if (Queues.GraphicsFamily == UINT32_MAX)
	{
		LOG_ERROR("[VulkanContext] No graphics+present queue family found");
		return false;
	}

	// Fall back to graphics queue for compute/transfer if dedicated ones not found
	if (Queues.ComputeFamily == UINT32_MAX) Queues.ComputeFamily = Queues.GraphicsFamily;
	if (Queues.TransferFamily == UINT32_MAX) Queues.TransferFamily = Queues.GraphicsFamily;

	// Build unique queue create infos
	std::set<uint32_t> uniqueFamilies = {
		Queues.GraphicsFamily, Queues.ComputeFamily, Queues.TransferFamily
	};

	float queuePriority = 1.0f;
	std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
	for (uint32_t family : uniqueFamilies)
	{
		VkDeviceQueueCreateInfo info{};
		info.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		info.queueFamilyIndex = family;
		info.queueCount       = 1;
		info.pQueuePriorities = &queuePriority;
		queueCreateInfos.push_back(info);
	}

	// Required extensions
	std::vector<const char*> deviceExtensions = {
		VK_KHR_SWAPCHAIN_EXTENSION_NAME,
		VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME,
		VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME,
		VK_KHR_MULTIVIEW_EXTENSION_NAME,
		VK_KHR_MAINTENANCE2_EXTENSION_NAME,
	};

	// Optional: shader objects
	if (bSupportsShaderObject) deviceExtensions.push_back(VK_EXT_SHADER_OBJECT_EXTENSION_NAME);

	// Chain pNext for required features
	VkPhysicalDeviceVulkan11Features vk11Features{};
	vk11Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;

	VkPhysicalDeviceVulkan12Features vk12Features{};
	vk12Features.sType                                     = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	vk12Features.pNext                                     = &vk11Features;
	vk12Features.bufferDeviceAddress                       = VK_TRUE;
	vk12Features.timelineSemaphore                         = VK_TRUE;
	vk12Features.descriptorIndexing                        = VK_TRUE;
	vk12Features.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
	vk12Features.descriptorBindingVariableDescriptorCount  = VK_TRUE;

	VkPhysicalDeviceVulkan13Features vk13Features{};
	vk13Features.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
	vk13Features.pNext            = &vk12Features;
	vk13Features.dynamicRendering = VK_TRUE;
	vk13Features.synchronization2 = VK_TRUE;

	VkPhysicalDeviceFeatures2 features2{};
	features2.sType                      = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	features2.pNext                      = &vk13Features;
	features2.features.samplerAnisotropy = VK_TRUE;

	VkDeviceCreateInfo createInfo{};
	createInfo.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	createInfo.pNext                   = &features2;
	createInfo.queueCreateInfoCount    = static_cast<uint32_t>(queueCreateInfos.size());
	createInfo.pQueueCreateInfos       = queueCreateInfos.data();
	createInfo.enabledExtensionCount   = static_cast<uint32_t>(deviceExtensions.size());
	createInfo.ppEnabledExtensionNames = deviceExtensions.data();
	// pEnabledFeatures must be null when using pNext chain with VkPhysicalDeviceFeatures2
	createInfo.pEnabledFeatures = nullptr;

	VkResult result = vkCreateDevice(PhysicalDevice, &createInfo, nullptr, &Device);
	if (result != VK_SUCCESS)
	{
		LOG_ERROR_F("[VulkanContext] vkCreateDevice failed: %d", result);
		return false;
	}

	// Retrieve queue handles
	vkGetDeviceQueue(Device, Queues.GraphicsFamily, 0, &Queues.Graphics);
	vkGetDeviceQueue(Device, Queues.ComputeFamily, 0, &Queues.Compute);
	vkGetDeviceQueue(Device, Queues.TransferFamily, 0, &Queues.Transfer);

	LOG_INFO_F("[VulkanContext] Logical device created (Graphics=%u, Compute=%u, Transfer=%u)",
			   Queues.GraphicsFamily, Queues.ComputeFamily, Queues.TransferFamily);
	return true;
}

// -----------------------------------------------------------------------
// Command pools
// -----------------------------------------------------------------------
bool VulkanContext::CreateCommandPools()
{
	auto makePool = [&](uint32_t family, VkCommandPool& out) -> bool
	{
		VkCommandPoolCreateInfo info{};
		info.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		info.queueFamilyIndex = family;
		info.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		VkResult result       = vkCreateCommandPool(Device, &info, nullptr, &out);
		return result == VK_SUCCESS;
	};

	if (!makePool(Queues.GraphicsFamily, GraphicsCommandPool))
	{
		LOG_ERROR("[VulkanContext] Failed to create graphics command pool");
		return false;
	}
	if (!makePool(Queues.ComputeFamily, ComputeCommandPool))
	{
		LOG_ERROR("[VulkanContext] Failed to create compute command pool");
		return false;
	}
	if (!makePool(Queues.TransferFamily, TransferCommandPool))
	{
		LOG_ERROR("[VulkanContext] Failed to create transfer command pool");
		return false;
	}
	return true;
}

// -----------------------------------------------------------------------
// Swapchain
// -----------------------------------------------------------------------
bool VulkanContext::CreateSwapchain(SDL_Window* window)
{
	SwapchainSupportDetails support = QuerySwapchainSupport(PhysicalDevice);

	VkSurfaceFormatKHR surfaceFormat = ChooseSurfaceFormat(support.Formats);
	VkPresentModeKHR presentMode     = ChoosePresentMode(support.PresentModes);
	VkExtent2D extent                = ChooseExtent(support.Capabilities, window);

	uint32_t imageCount = support.Capabilities.minImageCount + 1;
	if (support.Capabilities.maxImageCount > 0 && imageCount > support.Capabilities.maxImageCount) imageCount = support.Capabilities.maxImageCount;

	VkSwapchainCreateInfoKHR createInfo{};
	createInfo.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	createInfo.surface          = Surface;
	createInfo.minImageCount    = imageCount;
	createInfo.imageFormat      = surfaceFormat.format;
	createInfo.imageColorSpace  = surfaceFormat.colorSpace;
	createInfo.imageExtent      = extent;
	createInfo.imageArrayLayers = 1;
	createInfo.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

	if (Queues.GraphicsFamily != Queues.ComputeFamily)
	{
		uint32_t indices[]               = {Queues.GraphicsFamily, Queues.ComputeFamily};
		createInfo.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
		createInfo.queueFamilyIndexCount = 2;
		createInfo.pQueueFamilyIndices   = indices;
	}
	else
	{
		createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	}

	createInfo.preTransform   = support.Capabilities.currentTransform;
	createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	createInfo.presentMode    = presentMode;
	createInfo.clipped        = VK_TRUE;
	createInfo.oldSwapchain   = VK_NULL_HANDLE;

	VkResult result = vkCreateSwapchainKHR(Device, &createInfo, nullptr, &Swapchain.Handle);
	if (result != VK_SUCCESS)
	{
		LOG_ERROR_F("[VulkanContext] vkCreateSwapchainKHR failed: %d", result);
		return false;
	}

	Swapchain.Format = surfaceFormat.format;
	Swapchain.Extent = extent;

	// Get swapchain images
	uint32_t swapImageCount = 0;
	vkGetSwapchainImagesKHR(Device, Swapchain.Handle, &swapImageCount, nullptr);
	Swapchain.Images.resize(swapImageCount);
	vkGetSwapchainImagesKHR(Device, Swapchain.Handle, &swapImageCount, Swapchain.Images.data());

	// Create image views
	Swapchain.ImageViews.resize(swapImageCount);
	for (uint32_t i = 0; i < swapImageCount; ++i)
	{
		VkImageViewCreateInfo viewInfo{};
		viewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image                           = Swapchain.Images[i];
		viewInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format                          = Swapchain.Format;
		viewInfo.components.r                    = VK_COMPONENT_SWIZZLE_IDENTITY;
		viewInfo.components.g                    = VK_COMPONENT_SWIZZLE_IDENTITY;
		viewInfo.components.b                    = VK_COMPONENT_SWIZZLE_IDENTITY;
		viewInfo.components.a                    = VK_COMPONENT_SWIZZLE_IDENTITY;
		viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.baseMipLevel   = 0;
		viewInfo.subresourceRange.levelCount     = 1;
		viewInfo.subresourceRange.baseArrayLayer = 0;
		viewInfo.subresourceRange.layerCount     = 1;

		VkResult viewResult = vkCreateImageView(Device, &viewInfo, nullptr, &Swapchain.ImageViews[i]);
		if (viewResult != VK_SUCCESS)
		{
			LOG_ERROR_F("[VulkanContext] Failed to create swapchain image view %u: %d", i, viewResult);
			return false;
		}
	}

	LOG_INFO_F("[VulkanContext] Swapchain created: %ux%u, %u images, format %d",
			   extent.width, extent.height, swapImageCount, surfaceFormat.format);
	return true;
}

void VulkanContext::DestroySwapchain()
{
	for (VkImageView view : Swapchain.ImageViews) if (view != VK_NULL_HANDLE) vkDestroyImageView(Device, view, nullptr);
	Swapchain.ImageViews.clear();
	Swapchain.Images.clear();

	if (Swapchain.Handle != VK_NULL_HANDLE)
	{
		vkDestroySwapchainKHR(Device, Swapchain.Handle, nullptr);
		Swapchain.Handle = VK_NULL_HANDLE;
	}
}

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------
bool VulkanContext::CheckDeviceExtensionSupport(VkPhysicalDevice device,
												const std::vector<const char*>& required) const
{
	uint32_t extCount = 0;
	vkEnumerateDeviceExtensionProperties(device, nullptr, &extCount, nullptr);
	std::vector<VkExtensionProperties> available(extCount);
	vkEnumerateDeviceExtensionProperties(device, nullptr, &extCount, available.data());

	for (const char* req : required)
	{
		bool found = false;
		for (auto& ext : available)
			if (strcmp(ext.extensionName, req) == 0)
			{
				found = true;
				break;
			}
		if (!found) return false;
	}
	return true;
}

SwapchainSupportDetails VulkanContext::QuerySwapchainSupport(VkPhysicalDevice device) const
{
	SwapchainSupportDetails details{};
	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, Surface, &details.Capabilities);

	uint32_t formatCount = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(device, Surface, &formatCount, nullptr);
	if (formatCount)
	{
		details.Formats.resize(formatCount);
		vkGetPhysicalDeviceSurfaceFormatsKHR(device, Surface, &formatCount, details.Formats.data());
	}

	uint32_t modeCount = 0;
	vkGetPhysicalDeviceSurfacePresentModesKHR(device, Surface, &modeCount, nullptr);
	if (modeCount)
	{
		details.PresentModes.resize(modeCount);
		vkGetPhysicalDeviceSurfacePresentModesKHR(device, Surface, &modeCount, details.PresentModes.data());
	}

	return details;
}

VkSurfaceFormatKHR VulkanContext::ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& available) const
{
	for (const auto& fmt : available)
		if (fmt.format == VK_FORMAT_B8G8R8A8_SRGB &&
			fmt.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
			return fmt;
	return available[0];
}

VkPresentModeKHR VulkanContext::ChoosePresentMode(const std::vector<VkPresentModeKHR>& available) const
{
	// Prefer mailbox (triple-buffered, tear-free, no VSync stall)
	for (auto mode : available) if (mode == VK_PRESENT_MODE_MAILBOX_KHR) return mode;
	return VK_PRESENT_MODE_FIFO_KHR; // always available
}

VkExtent2D VulkanContext::ChooseExtent(const VkSurfaceCapabilitiesKHR& caps,
									   SDL_Window* window) const
{
	if (caps.currentExtent.width != UINT32_MAX) return caps.currentExtent;

	int w = 0, h = 0;
	SDL_GetWindowSizeInPixels(window, &w, &h);

	VkExtent2D extent{};
	extent.width = std::clamp(static_cast<uint32_t>(w),
							  caps.minImageExtent.width, caps.maxImageExtent.width);
	extent.height = std::clamp(static_cast<uint32_t>(h),
							   caps.minImageExtent.height, caps.maxImageExtent.height);
	return extent;
}

uint32_t VulkanContext::FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required) const
{
	VkPhysicalDeviceMemoryProperties memProps{};
	vkGetPhysicalDeviceMemoryProperties(PhysicalDevice, &memProps);

	for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
		if ((typeBits & (1u << i)) &&
			(memProps.memoryTypes[i].propertyFlags & required) == required)
			return i;

	return UINT32_MAX;
}

VkFormat VulkanContext::FindSupportedFormat(const std::vector<VkFormat>& candidates,
											VkImageTiling tiling,
											VkFormatFeatureFlags features) const
{
	for (VkFormat fmt : candidates)
	{
		VkFormatProperties props{};
		vkGetPhysicalDeviceFormatProperties(PhysicalDevice, fmt, &props);

		if (tiling == VK_IMAGE_TILING_LINEAR && (props.linearTilingFeatures & features) == features) return fmt;
		if (tiling == VK_IMAGE_TILING_OPTIMAL && (props.optimalTilingFeatures & features) == features) return fmt;
	}
	return VK_FORMAT_UNDEFINED;
}
