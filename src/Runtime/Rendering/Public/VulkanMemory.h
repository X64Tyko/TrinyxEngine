#pragma once
#include <cstdint>

#include "VulkanInclude.h"
#include "vk_mem_alloc.h"

class VulkanContext;

// -----------------------------------------------------------------------
// Allocation domain
//
//  DeviceLocal      – VRAM, GPU-only.  Upload via staging buffer.
//                     Use for buffers written rarely (mesh geometry, etc.).
//  PersistentMapped – Primary delta-upload target.
//                     On ReBAR hardware: DEVICE_LOCAL | HOST_VISIBLE — CPU
//                     writes go directly into VRAM, no staging needed.
//                     Falls back to HOST_VISIBLE | HOST_COHERENT on
//                     non-ReBAR hardware (correct but slower).
//  Staging          – HOST_VISIBLE | HOST_COHERENT, persistently mapped.
//                     Used as transfer source when uploading to DeviceLocal
//                     buffers on non-ReBAR hardware.
// -----------------------------------------------------------------------
enum class GpuMemoryDomain : uint8_t
{
	DeviceLocal,
	PersistentMapped,
	Staging,
};

// -----------------------------------------------------------------------
// VulkanBuffer — move-only RAII handle
//
// Owns a VkBuffer + VmaAllocation pair.  Buffer uses vk::Buffer as the
// handle type (thin wrapper over VkBuffer, zero overhead).
// The embedded VmaAllocator is a non-owning reference; storing it here
// lets the destructor release the allocation without calling back into
// VulkanMemory.
// -----------------------------------------------------------------------
struct VulkanBuffer
{
	vk::Buffer Buffer; ///< thin wrapper, null until allocated
	VmaAllocation Allocation     = VK_NULL_HANDLE;
	vk::DeviceAddress DeviceAddr = 0;       ///< 0 if BDA not requested
	void* MappedPtr              = nullptr; ///< Non-null for PersistentMapped/Staging
	VkDeviceSize Size            = 0;

	// Non-owning reference to the allocator used to create this buffer.
	VmaAllocator Allocator = VK_NULL_HANDLE;

	VulkanBuffer() = default;

	~VulkanBuffer()
	{
		if (static_cast<VkBuffer>(Buffer) != VK_NULL_HANDLE && Allocator != VK_NULL_HANDLE) vmaDestroyBuffer(Allocator, static_cast<VkBuffer>(Buffer), Allocation);
	}

	VulkanBuffer(const VulkanBuffer&)            = delete;
	VulkanBuffer& operator=(const VulkanBuffer&) = delete;

	VulkanBuffer(VulkanBuffer&& o) noexcept
		: Buffer(o.Buffer)
		, Allocation(o.Allocation)
		, DeviceAddr(o.DeviceAddr)
		, MappedPtr(o.MappedPtr)
		, Size(o.Size)
		, Allocator(o.Allocator)
	{
		o.Buffer     = vk::Buffer{};
		o.Allocation = VK_NULL_HANDLE;
		o.Allocator  = VK_NULL_HANDLE;
	}

	VulkanBuffer& operator=(VulkanBuffer&& o) noexcept
	{
		if (this != &o)
		{
			if (static_cast<VkBuffer>(Buffer) != VK_NULL_HANDLE && Allocator != VK_NULL_HANDLE) vmaDestroyBuffer(Allocator, static_cast<VkBuffer>(Buffer), Allocation);

			Buffer       = o.Buffer;
			o.Buffer     = vk::Buffer{};
			Allocation   = o.Allocation;
			o.Allocation = VK_NULL_HANDLE;
			Allocator    = o.Allocator;
			o.Allocator  = VK_NULL_HANDLE;
			DeviceAddr   = o.DeviceAddr;
			MappedPtr    = o.MappedPtr;
			Size         = o.Size;
		}
		return *this;
	}

	bool IsValid() const { return static_cast<VkBuffer>(Buffer) != VK_NULL_HANDLE; }

	/// Explicit release — use when you need to destroy before the destructor fires.
	void Free()
	{
		if (static_cast<VkBuffer>(Buffer) != VK_NULL_HANDLE && Allocator != VK_NULL_HANDLE)
		{
			vmaDestroyBuffer(Allocator, static_cast<VkBuffer>(Buffer), Allocation);
			Buffer     = vk::Buffer{};
			Allocation = VK_NULL_HANDLE;
			Allocator  = VK_NULL_HANDLE;
		}
	}
};

// -----------------------------------------------------------------------
// VulkanImage — move-only RAII handle
//
// Owns a VkImage + VkImageView + VmaAllocation.  Handle types use vk::
// wrappers (thin, zero overhead).  VkDevice is stored as a non-owning
// reference for vkDestroyImageView.
// -----------------------------------------------------------------------
struct VulkanImage
{
	vk::Image Image;
	vk::ImageView View;
	VmaAllocation Allocation = VK_NULL_HANDLE;
	VkFormat Format          = VK_FORMAT_UNDEFINED;
	VkExtent2D Extent        = {};

	// Non-owning references needed by the destructor.
	VmaAllocator Allocator = VK_NULL_HANDLE;
	VkDevice Device        = VK_NULL_HANDLE;

	VulkanImage() = default;

	~VulkanImage()
	{
		if (static_cast<VkImage>(Image) != VK_NULL_HANDLE && Allocator != VK_NULL_HANDLE)
		{
			vkDestroyImageView(Device, static_cast<VkImageView>(View), nullptr);
			vmaDestroyImage(Allocator, static_cast<VkImage>(Image), Allocation);
		}
	}

	VulkanImage(const VulkanImage&)            = delete;
	VulkanImage& operator=(const VulkanImage&) = delete;

	VulkanImage(VulkanImage&& o) noexcept
		: Image(o.Image)
		, View(o.View)
		, Allocation(o.Allocation)
		, Format(o.Format)
		, Extent(o.Extent)
		, Allocator(o.Allocator)
		, Device(o.Device)
	{
		o.Image      = vk::Image{};
		o.View       = vk::ImageView{};
		o.Allocation = VK_NULL_HANDLE;
		o.Allocator  = VK_NULL_HANDLE;
	}

	VulkanImage& operator=(VulkanImage&& o) noexcept
	{
		if (this != &o)
		{
			if (static_cast<VkImage>(Image) != VK_NULL_HANDLE && Allocator != VK_NULL_HANDLE)
			{
				vkDestroyImageView(Device, static_cast<VkImageView>(View), nullptr);
				vmaDestroyImage(Allocator, static_cast<VkImage>(Image), Allocation);
			}
			Image        = o.Image;
			o.Image      = vk::Image{};
			View         = o.View;
			o.View       = vk::ImageView{};
			Allocation   = o.Allocation;
			o.Allocation = VK_NULL_HANDLE;
			Allocator    = o.Allocator;
			o.Allocator  = VK_NULL_HANDLE;
			Device       = o.Device;
			Format       = o.Format;
			Extent       = o.Extent;
		}
		return *this;
	}

	bool IsValid() const { return static_cast<VkImage>(Image) != VK_NULL_HANDLE; }

	void Free()
	{
		if (static_cast<VkImage>(Image) != VK_NULL_HANDLE && Allocator != VK_NULL_HANDLE)
		{
			vkDestroyImageView(Device, static_cast<VkImageView>(View), nullptr);
			vmaDestroyImage(Allocator, static_cast<VkImage>(Image), Allocation);
			Image      = vk::Image{};
			View       = vk::ImageView{};
			Allocation = VK_NULL_HANDLE;
			Allocator  = VK_NULL_HANDLE;
		}
	}
};

// -----------------------------------------------------------------------
// VulkanMemory
//
// Wraps VMA.  Single instance owned by TrinyxEngine, passed as a pointer
// to VulkRender.  All GPU buffer and image allocation goes here.
// -----------------------------------------------------------------------
class VulkanMemory
{
public:
	VulkanMemory() = default;
	~VulkanMemory() { Shutdown(); }

	VulkanMemory(const VulkanMemory&)            = delete;
	VulkanMemory& operator=(const VulkanMemory&) = delete;

	bool Initialize(const VulkanContext& ctx);
	void Shutdown();

	// ----------------------------------------------------------------
	// Buffer allocation
	// ----------------------------------------------------------------

	/// Allocate a GPU buffer.  Returns a move-only RAII handle.
	/// Set requestDeviceAddress=true to populate VulkanBuffer::DeviceAddr
	/// (requires BDA support, checked at runtime).
	[[nodiscard]] VulkanBuffer AllocateBuffer(VkDeviceSize size,
											  VkBufferUsageFlags usage,
											  GpuMemoryDomain domain,
											  bool requestDeviceAddress = false);

	// ----------------------------------------------------------------
	// Image allocation
	// ----------------------------------------------------------------

	/// Allocate a DEVICE_LOCAL image + view.
	/// aspectMask determines the view's subresource (COLOR or DEPTH_BIT).
	[[nodiscard]] VulkanImage AllocateImage(VkExtent2D extent,
											VkFormat format,
											VkImageUsageFlags usage,
											VkImageAspectFlags aspectMask);

	/// Directly write pixels into a device-local image — no staging buffer, no command buffer.
	/// Requires bSupportsHostImageCopy (Vulkan 1.4 hostImageCopy feature).
	/// Image must have been created with VK_IMAGE_USAGE_HOST_TRANSFER_BIT.
	/// Falls back gracefully: returns false if host image copy is unavailable.
	bool UploadImage(VulkanImage& image,
					 const void* pixels,
					 VkDeviceSize byteSize,
					 uint32_t width,
					 uint32_t height);

	// ----------------------------------------------------------------
	// Accessors
	// ----------------------------------------------------------------

	VmaAllocator GetAllocator() const { return Allocator; }
	bool HasReBAR() const { return bHasReBAR; }
	bool SupportsBDA() const { return bBDA; }
	bool SupportsHostImageCopy() const { return bHostImageCopy; }

private:
	VmaAllocator Allocator = VK_NULL_HANDLE;
	VkDevice DeviceCache   = VK_NULL_HANDLE;
	bool bHasReBAR         = false;
	bool bBDA              = false;
	bool bHostImageCopy    = false;
};
