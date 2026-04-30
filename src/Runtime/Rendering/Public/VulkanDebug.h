#pragma once
// VulkanDebug.h
//
// Thin wrapper around VK_EXT_debug_utils for Nsight Graphics + RenderDoc + validation.
//
// Two pieces of functionality:
//   1. Object naming   — every Vulkan handle that survives a frame (buffers, images,
//                        pipelines, command pools, queues) gets a human-readable
//                        name in capture tools.
//   2. Command buffer  — region markers (Begin/End) and instantaneous markers (Insert)
//      labels            so a captured frame in Nsight reads as a tree of named
//                        passes instead of a flat list of vkCmd* calls.
//
// All entry points compile to nothing when TNX_ENABLE_NSIGHT is not defined, so
// they are safe to sprinkle through the renderer at zero shipping cost.

#include "VulkanInclude.h"
#include <cstdint>

#ifdef TNX_ENABLE_NSIGHT

namespace VulkanDebug
{
	/// True after the first VulkanContext that supports VK_EXT_debug_utils has
	/// finished CreateInstance(). Loaded entry points are global (volk) so a
	/// single check is enough across all threads.
	bool IsAvailable();

	/// Mark debug-utils as available. Called once by VulkanContext after the
	/// extension is enabled and volkLoadInstance has populated the function
	/// pointers.
	void SetAvailable(bool available);

	void SetObjectName(VkDevice device, VkObjectType type, uint64_t handle, const char* name);

	// Typed convenience wrappers — saves the caller from reinterpret_cast and
	// remembering the matching VkObjectType for each handle type.
	inline void Name(VkDevice device, VkInstance h, const char* name)        { SetObjectName(device, VK_OBJECT_TYPE_INSTANCE,        reinterpret_cast<uint64_t>(h), name); }
	inline void Name(VkDevice device, VkPhysicalDevice h, const char* name)  { SetObjectName(device, VK_OBJECT_TYPE_PHYSICAL_DEVICE, reinterpret_cast<uint64_t>(h), name); }
	inline void Name(VkDevice device, VkDevice h, const char* name)          { SetObjectName(device, VK_OBJECT_TYPE_DEVICE,          reinterpret_cast<uint64_t>(h), name); }
	inline void Name(VkDevice device, VkQueue h, const char* name)           { SetObjectName(device, VK_OBJECT_TYPE_QUEUE,           reinterpret_cast<uint64_t>(h), name); }
	inline void Name(VkDevice device, VkCommandPool h, const char* name)     { SetObjectName(device, VK_OBJECT_TYPE_COMMAND_POOL,    reinterpret_cast<uint64_t>(h), name); }
	inline void Name(VkDevice device, VkCommandBuffer h, const char* name)   { SetObjectName(device, VK_OBJECT_TYPE_COMMAND_BUFFER,  reinterpret_cast<uint64_t>(h), name); }
	inline void Name(VkDevice device, VkBuffer h, const char* name)          { SetObjectName(device, VK_OBJECT_TYPE_BUFFER,          reinterpret_cast<uint64_t>(h), name); }
	inline void Name(VkDevice device, VkImage h, const char* name)           { SetObjectName(device, VK_OBJECT_TYPE_IMAGE,           reinterpret_cast<uint64_t>(h), name); }
	inline void Name(VkDevice device, VkImageView h, const char* name)       { SetObjectName(device, VK_OBJECT_TYPE_IMAGE_VIEW,      reinterpret_cast<uint64_t>(h), name); }
	inline void Name(VkDevice device, VkPipeline h, const char* name)        { SetObjectName(device, VK_OBJECT_TYPE_PIPELINE,        reinterpret_cast<uint64_t>(h), name); }
	inline void Name(VkDevice device, VkPipelineLayout h, const char* name)  { SetObjectName(device, VK_OBJECT_TYPE_PIPELINE_LAYOUT, reinterpret_cast<uint64_t>(h), name); }
	inline void Name(VkDevice device, VkShaderModule h, const char* name)    { SetObjectName(device, VK_OBJECT_TYPE_SHADER_MODULE,   reinterpret_cast<uint64_t>(h), name); }
	inline void Name(VkDevice device, VkSemaphore h, const char* name)       { SetObjectName(device, VK_OBJECT_TYPE_SEMAPHORE,       reinterpret_cast<uint64_t>(h), name); }
	inline void Name(VkDevice device, VkFence h, const char* name)           { SetObjectName(device, VK_OBJECT_TYPE_FENCE,           reinterpret_cast<uint64_t>(h), name); }
	inline void Name(VkDevice device, VkSwapchainKHR h, const char* name)    { SetObjectName(device, VK_OBJECT_TYPE_SWAPCHAIN_KHR,   reinterpret_cast<uint64_t>(h), name); }

	struct LabelColor
	{
		float R, G, B, A;
	};

	// Standard palette — keeps Nsight regions colour-coded by category.
	inline constexpr LabelColor ColorCompute  {0.30f, 0.70f, 1.00f, 1.0f};
	inline constexpr LabelColor ColorRaster   {1.00f, 0.75f, 0.30f, 1.0f};
	inline constexpr LabelColor ColorBarrier  {0.70f, 0.70f, 0.70f, 1.0f};
	inline constexpr LabelColor ColorTransfer {0.40f, 0.90f, 0.40f, 1.0f};
	inline constexpr LabelColor ColorEditor   {0.85f, 0.50f, 1.00f, 1.0f};
	inline constexpr LabelColor ColorPicking  {1.00f, 0.40f, 0.40f, 1.0f};

	void BeginLabel(VkCommandBuffer cmd, const char* name, LabelColor color = ColorRaster);
	void EndLabel(VkCommandBuffer cmd);
	void InsertLabel(VkCommandBuffer cmd, const char* name, LabelColor color = ColorRaster);

	// RAII region — automatically calls EndLabel on scope exit.
	struct Scope
	{
		VkCommandBuffer Cmd;
		Scope(VkCommandBuffer cmd, const char* name, LabelColor color = ColorRaster) : Cmd(cmd) { BeginLabel(cmd, name, color); }
		~Scope() { EndLabel(Cmd); }
		Scope(const Scope&)            = delete;
		Scope& operator=(const Scope&) = delete;
	};
} // namespace VulkanDebug

	// Stringification helper for unique scope variable names.
	#define TNX_VKDBG_CONCAT2(a, b) a##b
	#define TNX_VKDBG_CONCAT(a, b)  TNX_VKDBG_CONCAT2(a, b)

	#define TNX_VKDBG_SET_NAME(device, handle, name) \
		::VulkanDebug::Name((device), (handle), (name))

	#define TNX_VKDBG_LABEL_BEGIN(cmd, name, color) \
		::VulkanDebug::BeginLabel((cmd), (name), (color))

	#define TNX_VKDBG_LABEL_END(cmd) \
		::VulkanDebug::EndLabel((cmd))

	#define TNX_VKDBG_LABEL_INSERT(cmd, name, color) \
		::VulkanDebug::InsertLabel((cmd), (name), (color))

	#define TNX_VKDBG_SCOPE(cmd, name, color) \
		::VulkanDebug::Scope TNX_VKDBG_CONCAT(_vkdbg_scope_, __LINE__)((cmd), (name), (color))

#else // TNX_ENABLE_NSIGHT

	#define TNX_VKDBG_SET_NAME(device, handle, name)        ((void)0)
	#define TNX_VKDBG_LABEL_BEGIN(cmd, name, color)         ((void)0)
	#define TNX_VKDBG_LABEL_END(cmd)                        ((void)0)
	#define TNX_VKDBG_LABEL_INSERT(cmd, name, color)        ((void)0)
	#define TNX_VKDBG_SCOPE(cmd, name, color)               ((void)0)

#endif // TNX_ENABLE_NSIGHT
