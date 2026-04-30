#include "VulkanDebug.h"

#ifdef TNX_ENABLE_NSIGHT

#include <atomic>

namespace VulkanDebug
{
	// Single global flag — VK_EXT_debug_utils is per-instance, but a process
	// only ever creates one instance, so an atomic bool is enough.
	static std::atomic<bool> gAvailable{false};

	bool IsAvailable() { return gAvailable.load(std::memory_order_acquire); }
	void SetAvailable(bool available) { gAvailable.store(available, std::memory_order_release); }

	void SetObjectName(VkDevice device, VkObjectType type, uint64_t handle, const char* name)
	{
		if (!gAvailable.load(std::memory_order_acquire) || handle == 0 || name == nullptr) return;

		VkDebugUtilsObjectNameInfoEXT info{};
		info.sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
		info.objectType   = type;
		info.objectHandle = handle;
		info.pObjectName  = name;

		// Function pointer is loaded by volk after the device exists. Guard against
		// the (unlikely) case where someone calls before volkLoadDevice.
		if (vkSetDebugUtilsObjectNameEXT) vkSetDebugUtilsObjectNameEXT(device, &info);
	}

	static VkDebugUtilsLabelEXT MakeLabel(const char* name, LabelColor c)
	{
		VkDebugUtilsLabelEXT label{};
		label.sType      = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
		label.pLabelName = name;
		label.color[0]   = c.R;
		label.color[1]   = c.G;
		label.color[2]   = c.B;
		label.color[3]   = c.A;
		return label;
	}

	void BeginLabel(VkCommandBuffer cmd, const char* name, LabelColor color)
	{
		if (!gAvailable.load(std::memory_order_acquire) || !vkCmdBeginDebugUtilsLabelEXT) return;
		auto label = MakeLabel(name, color);
		vkCmdBeginDebugUtilsLabelEXT(cmd, &label);
	}

	void EndLabel(VkCommandBuffer cmd)
	{
		if (!gAvailable.load(std::memory_order_acquire) || !vkCmdEndDebugUtilsLabelEXT) return;
		vkCmdEndDebugUtilsLabelEXT(cmd);
	}

	void InsertLabel(VkCommandBuffer cmd, const char* name, LabelColor color)
	{
		if (!gAvailable.load(std::memory_order_acquire) || !vkCmdInsertDebugUtilsLabelEXT) return;
		auto label = MakeLabel(name, color);
		vkCmdInsertDebugUtilsLabelEXT(cmd, &label);
	}
} // namespace VulkanDebug

#endif // TNX_ENABLE_NSIGHT
