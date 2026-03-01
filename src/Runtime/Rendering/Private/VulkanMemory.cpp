#define VMA_STATIC_VULKAN_FUNCTIONS  0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_IMPLEMENTATION
#include "VulkanMemory.h"

#include "VulkanContext.h"
#include "Logger.h"

// -----------------------------------------------------------------------
// Initialize
// -----------------------------------------------------------------------
bool VulkanMemory::Initialize(const VulkanContext& ctx)
{
	bHasReBAR   = ctx.HasReBAR();
	bBDA        = ctx.SupportsBufferDeviceAddress();
	DeviceCache = ctx.GetDevice();

	// Wire VMA's dynamic dispatch through volk's already-loaded function pointers.
	VmaVulkanFunctions vkFuncs{};
	vkFuncs.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
	vkFuncs.vkGetDeviceProcAddr   = vkGetDeviceProcAddr;

	VmaAllocatorCreateInfo allocInfo{};
	allocInfo.physicalDevice   = ctx.GetPhysicalDevice();
	allocInfo.device           = ctx.GetDevice();
	allocInfo.instance         = ctx.GetInstance();
	allocInfo.vulkanApiVersion = VK_API_VERSION_1_4;
	allocInfo.pVulkanFunctions = &vkFuncs;

	if (bBDA) allocInfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;

	VkResult result = vmaCreateAllocator(&allocInfo, &Allocator);
	if (result != VK_SUCCESS)
	{
		LOG_ERROR_F("[VulkanMemory] vmaCreateAllocator failed: %d", result);
		return false;
	}

	LOG_INFO_F("[VulkanMemory] Initialized (ReBAR: %s, BDA: %s)",
			   bHasReBAR ? "YES" : "NO",
			   bBDA ? "YES" : "NO");
	return true;
}

// -----------------------------------------------------------------------
// Shutdown
// -----------------------------------------------------------------------
void VulkanMemory::Shutdown()
{
	if (Allocator != VK_NULL_HANDLE)
	{
		vmaDestroyAllocator(Allocator);
		Allocator = VK_NULL_HANDLE;
	}
}

// -----------------------------------------------------------------------
// AllocateBuffer
// -----------------------------------------------------------------------
VulkanBuffer VulkanMemory::AllocateBuffer(VkDeviceSize size,
										  VkBufferUsageFlags usage,
										  GpuMemoryDomain domain,
										  bool requestDeviceAddress)
{
	VulkanBuffer out{};
	out.Size      = size;
	out.Allocator = Allocator;

	if (requestDeviceAddress && bBDA) usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

	VkBufferCreateInfo bufInfo{};
	bufInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufInfo.size        = size;
	bufInfo.usage       = usage;
	bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo vmaInfo{};
	switch (domain)
	{
		case GpuMemoryDomain::DeviceLocal: vmaInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
			break;

		case GpuMemoryDomain::PersistentMapped: if (bHasReBAR)
			{
				// Primary path: direct CPU→VRAM writes with no staging.
				vmaInfo.usage         = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
				vmaInfo.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
					VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
				vmaInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
					VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
			}
			else
			{
				// Non-ReBAR fallback: host-visible, host-coherent (PCIe bandwidth).
				vmaInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
				vmaInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
			}
			break;

		case GpuMemoryDomain::Staging: vmaInfo.usage = VMA_MEMORY_USAGE_CPU_ONLY;
			vmaInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
			break;
	}

	VmaAllocationInfo allocResult{};
	VkResult result = vmaCreateBuffer(Allocator, &bufInfo, &vmaInfo,
									  &out.Buffer, &out.Allocation, &allocResult);
	if (result != VK_SUCCESS)
	{
		LOG_ERROR_F("[VulkanMemory] vmaCreateBuffer failed (size=%llu domain=%d): %d",
					static_cast<unsigned long long>(size),
					static_cast<int>(domain),
					result);
		out.Allocator = VK_NULL_HANDLE; // prevent double-free in destructor
		return out;
	}

	if (domain == GpuMemoryDomain::PersistentMapped || domain == GpuMemoryDomain::Staging) out.MappedPtr = allocResult.pMappedData;

	if (requestDeviceAddress && bBDA)
	{
		VkBufferDeviceAddressInfo addrInfo{};
		addrInfo.sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
		addrInfo.buffer = out.Buffer;
		out.DeviceAddr  = vkGetBufferDeviceAddress(DeviceCache, &addrInfo);
	}

	return out;
}

// -----------------------------------------------------------------------
// AllocateImage
// -----------------------------------------------------------------------
VulkanImage VulkanMemory::AllocateImage(VkExtent2D extent,
										VkFormat format,
										VkImageUsageFlags usage,
										VkImageAspectFlags aspectMask)
{
	VulkanImage out{};
	out.Format    = format;
	out.Extent    = extent;
	out.Allocator = Allocator;
	out.Device    = DeviceCache;

	VkImageCreateInfo imgInfo{};
	imgInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imgInfo.imageType     = VK_IMAGE_TYPE_2D;
	imgInfo.format        = format;
	imgInfo.extent        = {extent.width, extent.height, 1};
	imgInfo.mipLevels     = 1;
	imgInfo.arrayLayers   = 1;
	imgInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
	imgInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
	imgInfo.usage         = usage;
	imgInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
	imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VmaAllocationCreateInfo vmaInfo{};
	vmaInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

	VkResult result = vmaCreateImage(Allocator, &imgInfo, &vmaInfo,
									 &out.Image, &out.Allocation, nullptr);
	if (result != VK_SUCCESS)
	{
		LOG_ERROR_F("[VulkanMemory] vmaCreateImage failed (format=%d): %d",
					static_cast<int>(format), result);
		out.Allocator = VK_NULL_HANDLE;
		return out;
	}

	VkImageViewCreateInfo viewInfo{};
	viewInfo.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image                       = out.Image;
	viewInfo.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format                      = format;
	viewInfo.subresourceRange.aspectMask = aspectMask;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = 1;

	result = vkCreateImageView(DeviceCache, &viewInfo, nullptr, &out.View);
	if (result != VK_SUCCESS)
	{
		LOG_ERROR_F("[VulkanMemory] vkCreateImageView failed: %d", result);
		// Destroy the image we just made; destructor won't fire since
		// we're about to clear Allocator to signal invalid state.
		vmaDestroyImage(Allocator, out.Image, out.Allocation);
		out.Allocator = VK_NULL_HANDLE;
		return out;
	}

	return out;
}
