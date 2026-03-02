#pragma once
// VulkanInclude.h — always include this instead of volk.h or vulkan.hpp directly.
// volk MUST precede vulkan.hpp so VK_NO_PROTOTYPES is set before vulkan/vulkan.h is parsed.
#include "volk.h"

// vulkan.hpp sees VK_NO_PROTOTYPES (set by volk.h) and uses dynamic dispatch.
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
