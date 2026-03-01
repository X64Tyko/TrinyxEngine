#pragma once
#include <cstdint>
#include <vector>

// Forward declarations
struct SDL_Window;

// volk must be included before any vulkan headers
// Inclince the ude volk.h (which defines all Vulkan symbols) instead of <vulkan/vulkan.h>
#include "volk.h"

// -----------------------------------------------------------------------
// Helper structures
// -----------------------------------------------------------------------

struct SwapchainSupportDetails
{
	VkSurfaceCapabilitiesKHR Capabilities{};
	std::vector<VkSurfaceFormatKHR> Formats;
	std::vector<VkPresentModeKHR> PresentModes;
};

struct VulkanQueues
{
	VkQueue Graphics = VK_NULL_HANDLE;
	VkQueue Compute  = VK_NULL_HANDLE;
	VkQueue Transfer = VK_NULL_HANDLE;

	uint32_t GraphicsFamily = UINT32_MAX;
	uint32_t ComputeFamily  = UINT32_MAX;
	uint32_t TransferFamily = UINT32_MAX;
};

struct VulkanSwapchain
{
	VkSwapchainKHR Handle = VK_NULL_HANDLE;
	VkFormat Format       = VK_FORMAT_UNDEFINED;
	VkExtent2D Extent     = {};
	std::vector<VkImage> Images;
	std::vector<VkImageView> ImageViews;
	// WSI only — no depth/render-target resources here.
	// Depth image lives in RenderThread, allocated via VMA.
};

// -----------------------------------------------------------------------
// VulkanContext
//
// Owns the core Vulkan objects for the lifetime of the application:
//   VkInstance, VkPhysicalDevice, VkDevice, queues, surface, swapchain,
//   command pools, and feature-detection flags.
//
// Created and destroyed on the main (Sentinel) thread. All other threads
// receive a const* and call only thread-safe accessors.
// -----------------------------------------------------------------------
class VulkanContext
{
public:
	VulkanContext()  = default;
	~VulkanContext() = default;

	VulkanContext(const VulkanContext&)            = delete;
	VulkanContext& operator=(const VulkanContext&) = delete;

	// ----------------------------------------------------------------
	// Lifecycle
	// ----------------------------------------------------------------

	/// Initialize instance → surface → device → swapchain → command pools.
	bool Initialize(SDL_Window* window, bool enableValidation = true);
	void Shutdown();

	/// Recreate swapchain after resize. Destroys the old swapchain first.
	bool RecreateSwapchain(SDL_Window* window);

	// ----------------------------------------------------------------
	// Accessors
	// ----------------------------------------------------------------

	VkInstance GetInstance() const { return Instance; }
	VkPhysicalDevice GetPhysicalDevice() const { return PhysicalDevice; }
	VkDevice GetDevice() const { return Device; }
	VkSurfaceKHR GetSurface() const { return Surface; }

	const VulkanQueues& GetQueues() const { return Queues; }
	const VulkanSwapchain& GetSwapchain() const { return Swapchain; }

	VkCommandPool GetGraphicsCommandPool() const { return GraphicsCommandPool; }
	VkCommandPool GetComputeCommandPool() const { return ComputeCommandPool; }
	VkCommandPool GetTransferCommandPool() const { return TransferCommandPool; }

	// ----------------------------------------------------------------
	// Feature flags
	// ----------------------------------------------------------------

	bool HasReBAR() const { return bHasReBAR; }
	bool SupportsBufferDeviceAddress() const { return bSupportsBufferDeviceAddress; }
	bool SupportsShaderObject() const { return bSupportsShaderObject; }
	bool SupportsDynamicRendering() const { return bSupportsDynamicRendering; }
	bool SupportsTimelineSemaphore() const { return bSupportsTimelineSemaphore; }
	bool SupportsAsyncCompute() const { return Queues.ComputeFamily != Queues.GraphicsFamily; }
	bool SupportsAsyncTransfer() const { return Queues.TransferFamily != Queues.GraphicsFamily; }

	uint32_t GetReBarHeapIndex() const { return ReBarHeapIndex; }

	// ----------------------------------------------------------------
	// Helpers exposed for RenderThread
	// ----------------------------------------------------------------

	/// Find a supported image format from the candidates list.
	/// Used by RenderThread when selecting depth/render-target formats.
	VkFormat FindSupportedFormat(const std::vector<VkFormat>& candidates,
								 VkImageTiling tiling,
								 VkFormatFeatureFlags features) const;

private:
	// ----------------------------------------------------------------
	// Private init steps
	// ----------------------------------------------------------------
	bool CreateInstance(SDL_Window* window, bool enableValidation);
	bool SetupDebugMessenger();
	bool CreateSurface(SDL_Window* window);
	bool SelectPhysicalDevice();
	bool CreateLogicalDevice();
	bool CreateCommandPools();
	bool CreateSwapchain(SDL_Window* window);
	void DestroySwapchain();

	// ----------------------------------------------------------------
	// Physical-device helpers
	// ----------------------------------------------------------------
	bool CheckDeviceExtensionSupport(VkPhysicalDevice device,
									 const std::vector<const char*>& required) const;
	SwapchainSupportDetails QuerySwapchainSupport(VkPhysicalDevice device) const;
	VkSurfaceFormatKHR ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& available) const;
	VkPresentModeKHR ChoosePresentMode(const std::vector<VkPresentModeKHR>& available) const;
	VkExtent2D ChooseExtent(const VkSurfaceCapabilitiesKHR& caps,
							SDL_Window* window) const;
	uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required) const;
	// Note: raw vkAllocateMemory is only used by FindSupportedFormat's format-query path.
	// All actual GPU buffer/image allocation goes through VMA (VulkanMemory).

	// ----------------------------------------------------------------
	// Core Vulkan handles
	// ----------------------------------------------------------------
	VkInstance Instance                     = VK_NULL_HANDLE;
	VkDebugUtilsMessengerEXT DebugMessenger = VK_NULL_HANDLE;
	VkSurfaceKHR Surface                    = VK_NULL_HANDLE;
	VkPhysicalDevice PhysicalDevice         = VK_NULL_HANDLE;
	VkDevice Device                         = VK_NULL_HANDLE;

	VulkanQueues Queues;
	VulkanSwapchain Swapchain;

	VkCommandPool GraphicsCommandPool = VK_NULL_HANDLE;
	VkCommandPool ComputeCommandPool  = VK_NULL_HANDLE;
	VkCommandPool TransferCommandPool = VK_NULL_HANDLE;

	// ----------------------------------------------------------------
	// Feature flags
	// ----------------------------------------------------------------
	bool bHasReBAR                    = false;
	bool bSupportsBufferDeviceAddress = false;
	bool bSupportsShaderObject        = false;
	bool bSupportsDynamicRendering    = false;
	bool bSupportsTimelineSemaphore   = false;
	bool bValidationEnabled           = false;
	uint32_t ReBarHeapIndex           = UINT32_MAX;
};
