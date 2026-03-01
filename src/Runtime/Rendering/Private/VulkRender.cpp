#include "VulkRender.h"

#include <cassert>

#include "Logger.h"

void VulkRender::Initialize(Registry* registry,
							LogicThread* logic,
							const EngineConfig* config,
							VulkanContext* vkCtx,
							VulkanMemory* vkMem,
							SDL_Window* window)
{
	RegistryPtr = registry;
	LogicPtr    = logic;
	ConfigPtr   = config;
	VkCtx       = vkCtx;
	VkMem       = vkMem;
	WindowPtr   = window;

	LOG_INFO("[VulkRender] Initialized");
}

void VulkRender::Start()
{
	bIsRunning.store(true, std::memory_order_release);
	Thread = std::thread(&VulkRender::ThreadMain, this);
	LOG_INFO("[VulkRender] Started");

	// Depth attachment
	if (!CreateDepthImage())
	{
		LOG_ERROR("[VulkRender] Failed to create depth image");
		return;
	}
}

void VulkRender::Stop()
{
	bIsRunning.store(false, std::memory_order_release);
	LOG_INFO("[VulkRender] Stop requested");
}

void VulkRender::Join()
{
	if (Thread.joinable())
	{
		Thread.join();
		LOG_INFO("[VulkRender] Joined");
	}
}

void VulkRender::ThreadMain()
{
	// Step 1: acquire → clear → present
	// See docs/RENDERING_REFERENCES.md for build order and resource links.
	LOG_INFO("[VulkRender] Thread running — not yet implemented");
	bIsRunning.store(false, std::memory_order_release);
}

bool VulkRender::CreateDepthImage()
{
	std::vector<VkFormat> depthFormatList{VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT};
	VkFormat depthFormat{VK_FORMAT_UNDEFINED};
	for (VkFormat& format : depthFormatList)
	{
		VkFormatProperties2 formatProperties{};
		formatProperties.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;

		vkGetPhysicalDeviceFormatProperties2(VkCtx->GetPhysicalDevice(), format, &formatProperties);
		if (formatProperties.formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
		{
			depthFormat = format;
			break;
		}
	}

	assert(depthFormat != VK_FORMAT_UNDEFINED);
	VkImageCreateInfo depthImageCI{};
	depthImageCI.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	depthImageCI.imageType     = VK_IMAGE_TYPE_2D;
	depthImageCI.format        = depthFormat;
	depthImageCI.extent        = {VkCtx->GetSwapchain().Extent.width, VkCtx->GetSwapchain().Extent.height, 1};
	depthImageCI.mipLevels     = 1;
	depthImageCI.arrayLayers   = 1;
	depthImageCI.samples       = VK_SAMPLE_COUNT_1_BIT;
	depthImageCI.tiling        = VK_IMAGE_TILING_OPTIMAL;
	depthImageCI.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	depthImageCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VmaAllocationCreateInfo allocCI{};
	allocCI.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
	allocCI.usage = VMA_MEMORY_USAGE_AUTO;
	vmaCreateImage(VkMem->GetAllocator(), &depthImageCI, &allocCI, &DepthImage, &DepthImageAllocation, nullptr);
	VkImageViewCreateInfo depthViewCI{};
	depthViewCI.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	depthViewCI.image            = DepthImage;
	depthViewCI.viewType         = VK_IMAGE_VIEW_TYPE_2D;
	depthViewCI.format           = depthFormat;
	depthViewCI.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .levelCount = 1, .layerCount = 1};
	vkCreateImageView(VkCtx->GetDevice(), &depthViewCI, nullptr, &DepthImageView);

	return true;
}
