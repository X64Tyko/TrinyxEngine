#pragma once
#include <cstdint>
#include <vector>

// Forward declarations
struct SDL_Window;

// VulkanInclude.h must be the single Vulkan entry point: volk first, then vulkan.hpp + raii.
#include "VulkanInclude.h"

// -----------------------------------------------------------------------
// Helper structures
// -----------------------------------------------------------------------

struct VulkanQueues
{
	vk::Queue Graphics;
	vk::Queue Compute;
	vk::Queue Transfer;

	uint32_t GraphicsFamily = UINT32_MAX;
	uint32_t ComputeFamily  = UINT32_MAX;
	uint32_t TransferFamily = UINT32_MAX;
};

struct VulkanSwapchain
{
	vk::raii::SwapchainKHR Handle{nullptr};
	vk::Format Format = vk::Format::eUndefined;
	vk::Extent2D Extent{};
	std::vector<vk::Image> Images;               // non-owning (owned by swapchain)
	std::vector<vk::raii::ImageView> ImageViews; // owning
	// WSI only — no depth/render-target resources here.
	// Depth image lives in VulkRender, allocated via VMA.
};

// -----------------------------------------------------------------------
// VulkanContext
//
// Owns the core Vulkan objects for the lifetime of the application:
//   vk::raii::Instance, PhysicalDevice, Device, queues, surface, swapchain,
//   command pools, and feature-detection flags.
//
// Created and destroyed on the main (Sentinel) thread. All other threads
// receive a const* and call only thread-safe accessors.
//
// Shutdown is implicit: raii members destroy in reverse declaration order.
// -----------------------------------------------------------------------
class VulkanContext
{
public:
	VulkanContext()  = default;
	~VulkanContext() = default;

	VulkanContext(const VulkanContext&)            = delete;
	VulkanContext& operator=(const VulkanContext&) = delete;

	/// Initialize instance -> surface -> device -> swapchain -> command pools.
	bool Initialize(SDL_Window* window, bool enableValidation = true);

	/// Explicit teardown — must be called from TrinyxEngine::Shutdown() so that
	/// all vkDestroy* calls happen while the validation layer is still alive.
	/// raii members would otherwise be destroyed at atexit(), after validation
	/// layer statics are gone, causing a dispatch-handle crash.
	void Shutdown();

	/// Recreate swapchain after resize. Destroys the old swapchain first.
	bool RecreateSwapchain(SDL_Window* window);

	// ----------------------------------------------------------------
	// Accessors — return raw C handles for VMA / volk / SDL compatibility
	// ----------------------------------------------------------------

	VkInstance GetInstance() const { return *Instance; }
	VkPhysicalDevice GetPhysicalDevice() const { return *PhysicalDevice; }
	VkDevice GetDevice() const { return *Device; }
	VkSurfaceKHR GetSurface() const { return *Surface; }

	const VulkanQueues& GetQueues() const { return Queues; }
	const VulkanSwapchain& GetSwapchain() const { return Swapchain; }

	VkCommandPool GetGraphicsCommandPool() const { return *GraphicsCommandPool; }
	VkCommandPool GetComputeCommandPool() const { return *ComputeCommandPool; }
	VkCommandPool GetTransferCommandPool() const { return *TransferCommandPool; }

	/// Returns the owning raii::Device — needed to create vk::raii:: sync objects in VulkRender.
	const vk::raii::Device& GetRaiiDevice() const { return Device; }

	bool HasReBAR() const { return bHasReBAR; }
	bool SupportsBufferDeviceAddress() const { return bSupportsBufferDeviceAddress; }
	bool SupportsShaderObject() const { return bSupportsShaderObject; }
	bool SupportsDynamicRendering() const { return bSupportsDynamicRendering; }
	bool SupportsTimelineSemaphore() const { return bSupportsTimelineSemaphore; }
	bool SupportsAsyncCompute() const { return Queues.ComputeFamily != Queues.GraphicsFamily; }
	bool SupportsAsyncTransfer() const { return Queues.TransferFamily != Queues.GraphicsFamily; }
	bool SupportsHostImageCopy() const { return bSupportsHostImageCopy; }
	bool SupportsPushDescriptors() const { return bSupportsPushDescriptors; }
	bool SupportsIndexTypeUint8() const { return bSupportsIndexTypeUint8; }

	uint32_t GetReBarHeapIndex() const { return ReBarHeapIndex; }

	/// Find a supported image format from the candidates list.
	/// Used by VulkRender when selecting depth/render-target formats.
	VkFormat FindSupportedFormat(const std::vector<VkFormat>& candidates,
								 VkImageTiling tiling,
								 VkFormatFeatureFlags features) const;

private:
	bool CreateInstance(SDL_Window* window, bool enableValidation);
	bool SetupDebugMessenger();
	bool CreateSurface(SDL_Window* window);
	bool SelectPhysicalDevice();
	bool CreateLogicalDevice();
	bool CreateCommandPools();
	bool CreateSwapchain(SDL_Window* window);
	void DestroySwapchain();

	vk::SurfaceFormatKHR ChooseSurfaceFormat(const std::vector<vk::SurfaceFormatKHR>& available) const;
	vk::PresentModeKHR ChoosePresentMode(const std::vector<vk::PresentModeKHR>& available) const;
	vk::Extent2D ChooseExtent(const vk::SurfaceCapabilitiesKHR& caps, SDL_Window* window) const;

	// Core Vulkan handles (raii — destroy in reverse declaration order)
	vk::raii::Context VkContext;
	vk::raii::Instance Instance{nullptr};
	vk::raii::DebugUtilsMessengerEXT DebugMessenger{nullptr};
	vk::raii::SurfaceKHR Surface{nullptr};
	vk::raii::PhysicalDevice PhysicalDevice{nullptr};
	vk::raii::Device Device{nullptr};

	VulkanQueues Queues;
	VulkanSwapchain Swapchain;

	vk::raii::CommandPool GraphicsCommandPool{nullptr};
	vk::raii::CommandPool ComputeCommandPool{nullptr};
	vk::raii::CommandPool TransferCommandPool{nullptr};

	// Feature flags
	bool bHasReBAR                    = false;
	bool bSupportsBufferDeviceAddress = false;
	bool bSupportsShaderObject        = false;
	bool bSupportsDynamicRendering    = false;
	bool bSupportsTimelineSemaphore   = false;
	bool bValidationEnabled           = false;
	bool bSupportsHostImageCopy       = false;
	bool bSupportsPushDescriptors     = false;
	bool bSupportsDynRenderLocalRead  = false;
	bool bSupportsIndexTypeUint8      = false;
	uint32_t ReBarHeapIndex           = UINT32_MAX;
};
