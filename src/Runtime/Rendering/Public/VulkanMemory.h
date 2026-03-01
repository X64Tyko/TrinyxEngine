#pragma once
#include <cstdint>

#include "volk.h"
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
// Owns a VkBuffer + VmaAllocation pair.  The embedded VmaAllocator is a
// non-owning reference (VMA's handle type is already a pointer); storing it
// here lets the destructor release the allocation without calling back into
// VulkanMemory.
// -----------------------------------------------------------------------
struct VulkanBuffer
{
	VkBuffer Buffer            = VK_NULL_HANDLE;
	VmaAllocation Allocation   = VK_NULL_HANDLE;
	VkDeviceAddress DeviceAddr = 0;       ///< 0 if BDA not requested
	void* MappedPtr            = nullptr; ///< Non-null for PersistentMapped/Staging
	VkDeviceSize Size          = 0;

	// Non-owning reference to the allocator used to create this buffer.
	// Set by VulkanMemory::AllocateBuffer(); null until then.
	VmaAllocator Allocator = VK_NULL_HANDLE;

	VulkanBuffer() = default;

	~VulkanBuffer()
	{
		if (Buffer != VK_NULL_HANDLE && Allocator != VK_NULL_HANDLE) vmaDestroyBuffer(Allocator, Buffer, Allocation);
	}

	// Move-only: copying a GPU buffer handle makes no sense.
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
		o.Buffer     = VK_NULL_HANDLE;
		o.Allocation = VK_NULL_HANDLE;
		o.Allocator  = VK_NULL_HANDLE;
	}

	VulkanBuffer& operator=(VulkanBuffer&& o) noexcept
	{
		if (this != &o)
		{
			if (Buffer != VK_NULL_HANDLE && Allocator != VK_NULL_HANDLE) vmaDestroyBuffer(Allocator, Buffer, Allocation);

			Buffer       = o.Buffer;
			o.Buffer     = VK_NULL_HANDLE;
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

	bool IsValid() const { return Buffer != VK_NULL_HANDLE; }

	/// Explicit release — use when you need to destroy before the destructor fires.
	void Free()
	{
		if (Buffer != VK_NULL_HANDLE && Allocator != VK_NULL_HANDLE)
		{
			vmaDestroyBuffer(Allocator, Buffer, Allocation);
			Buffer     = VK_NULL_HANDLE;
			Allocation = VK_NULL_HANDLE;
			Allocator  = VK_NULL_HANDLE;
		}
	}
};

// -----------------------------------------------------------------------
// VulkanImage — move-only RAII handle
//
// Owns a VkImage + VkImageView + VmaAllocation.  The view and image are
// always created/destroyed as a unit so they live together here.
// VkDevice is stored as a non-owning reference for vkDestroyImageView.
// -----------------------------------------------------------------------
struct VulkanImage
{
	VkImage Image            = VK_NULL_HANDLE;
	VkImageView View         = VK_NULL_HANDLE;
	VmaAllocation Allocation = VK_NULL_HANDLE;
	VkFormat Format          = VK_FORMAT_UNDEFINED;
	VkExtent2D Extent        = {};

	// Non-owning references needed by the destructor.
	VmaAllocator Allocator = VK_NULL_HANDLE;
	VkDevice Device        = VK_NULL_HANDLE;

	VulkanImage() = default;

	~VulkanImage()
	{
		if (Image != VK_NULL_HANDLE && Allocator != VK_NULL_HANDLE)
		{
			vkDestroyImageView(Device, View, nullptr);
			vmaDestroyImage(Allocator, Image, Allocation);
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
		o.Image      = VK_NULL_HANDLE;
		o.View       = VK_NULL_HANDLE;
		o.Allocation = VK_NULL_HANDLE;
		o.Allocator  = VK_NULL_HANDLE;
	}

	VulkanImage& operator=(VulkanImage&& o) noexcept
	{
		if (this != &o)
		{
			if (Image != VK_NULL_HANDLE && Allocator != VK_NULL_HANDLE)
			{
				vkDestroyImageView(Device, View, nullptr);
				vmaDestroyImage(Allocator, Image, Allocation);
			}
			Image        = o.Image;
			o.Image      = VK_NULL_HANDLE;
			View         = o.View;
			o.View       = VK_NULL_HANDLE;
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

	bool IsValid() const { return Image != VK_NULL_HANDLE; }

	void Free()
	{
		if (Image != VK_NULL_HANDLE && Allocator != VK_NULL_HANDLE)
		{
			vkDestroyImageView(Device, View, nullptr);
			vmaDestroyImage(Allocator, Image, Allocation);
			Image      = VK_NULL_HANDLE;
			View       = VK_NULL_HANDLE;
			Allocation = VK_NULL_HANDLE;
			Allocator  = VK_NULL_HANDLE;
		}
	}
};

// -----------------------------------------------------------------------
// VulkanMemory
//
// Wraps VMA.  Single instance owned by StrigidEngine, passed as a pointer
// to the RenderThread.  All GPU buffer and image allocation goes here.
// -----------------------------------------------------------------------
class VulkanMemory
{
public:
	VulkanMemory()  = default;
	~VulkanMemory() = default;

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

	// ----------------------------------------------------------------
	// Accessors
	// ----------------------------------------------------------------

	VmaAllocator GetAllocator() const { return Allocator; }
	bool HasReBAR() const { return bHasReBAR; }
	bool SupportsBDA() const { return bBDA; }

private:
	VmaAllocator Allocator = VK_NULL_HANDLE;
	VkDevice DeviceCache   = VK_NULL_HANDLE;
	bool bHasReBAR         = false;
	bool bBDA              = false;
};
