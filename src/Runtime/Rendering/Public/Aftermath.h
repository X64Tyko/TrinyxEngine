#pragma once
// Aftermath.h
//
// NVIDIA Nsight Aftermath integration scaffold.
//
// Aftermath captures a GPU crash dump (.nv-gpudmp) on TDR / device-lost so we can
// post-mortem the bad command buffer in Nsight Aftermath Monitor. Three pieces:
//
//   1. Enable VK_NV_device_diagnostics_config device extension (CreateLogicalDevice).
//      → Chain VkDeviceDiagnosticsConfigCreateInfoNV into VkDeviceCreateInfo's pNext.
//   2. Register crash-dump callbacks before device creation (Aftermath::Initialize).
//      → On VK_ERROR_DEVICE_LOST, the SDK invokes the callback with dump bytes.
//   3. (Optional) Sprinkle vkCmdSetCheckpointNV markers throughout the command
//      buffer so the dump points at the exact draw/dispatch that failed.
//
// This file is the API surface only. The actual SDK calls live in Aftermath.cpp
// and compile to no-ops when TNX_ENABLE_AFTERMATH is not defined or the SDK
// path was not provided to CMake (TNX_AFTERMATH_SDK_MISSING).

#include "VulkanInclude.h"
#include <cstdint>

namespace Aftermath
{
	/// Register crash-dump callbacks. Must be called BEFORE vkCreateDevice so
	/// that the GPU driver hooks into the new device. Returns true if Aftermath
	/// is fully active, false if the SDK is missing or the call failed (engine
	/// continues running without crash dumps in that case).
	bool Initialize();

	/// Release Aftermath state. Call after vkDeviceWaitIdle and before destroying
	/// the device.
	void Shutdown();

	/// True if Aftermath::Initialize succeeded and the SDK is wired in.
	bool IsEnabled();

	/// Append the VK_NV_device_diagnostics_config extension name to the device
	/// extension list if the physical device supports it. Caller passes the
	/// already-collected list of device extensions; this will push_back if
	/// supported. Returns true if the extension was added.
	bool TryAddDeviceExtension(VkPhysicalDevice physDev, const char** outExtensionName);

	/// Inserts Aftermath's VkDeviceDiagnosticsConfigCreateInfoNV at the head of
	/// the pNext chain and returns the new head pointer (assign to
	/// VkDeviceCreateInfo::pNext). When disabled / unsupported, returns
	/// `existingChain` unchanged.
	const void* PrependDeviceCreateChain(const void* existingChain);

	/// Optional: emit a checkpoint marker on the command buffer so Aftermath's
	/// dump points at the closest passed checkpoint when a crash occurs.
	/// `marker` must outlive the GPU frame (use string literals or a long-lived
	/// pool) — the SDK stores the pointer, not the bytes.
	void Checkpoint(VkCommandBuffer cmd, const char* marker);
} // namespace Aftermath
