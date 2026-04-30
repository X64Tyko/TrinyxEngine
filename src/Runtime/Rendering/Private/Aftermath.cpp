#include "Aftermath.h"

#include "Logger.h"

#include <cstring>

#if defined(TNX_ENABLE_AFTERMATH) && !defined(TNX_AFTERMATH_SDK_MISSING)
	// Real SDK headers — only included when the user pointed CMake at a valid
	// Aftermath SDK install. Otherwise everything below stubs out cleanly.
	#include "GFSDK_Aftermath.h"
	#include "GFSDK_Aftermath_GpuCrashDump.h"
	#define TNX_AFTERMATH_HAVE_SDK 1
#else
	#define TNX_AFTERMATH_HAVE_SDK 0
#endif

namespace Aftermath
{
#if defined(TNX_ENABLE_AFTERMATH)
	static bool gEnabled = false;

	#if TNX_AFTERMATH_HAVE_SDK
	// Crash-dump bytes arrive on a worker thread inside the driver. Write them
	// to disk and return; decoding happens out-of-process in Nsight Aftermath
	// Monitor or via the standalone decoder.
	static void GFSDK_AFTERMATH_CALL OnCrashDump(const void* dump, uint32_t size, void* /*user*/)
	{
		// TODO: Write `dump` (size bytes) to a timestamped .nv-gpudmp file.
		LOG_ENG_ERROR_F("[Aftermath] GPU crash dump received (%u bytes). Writing to disk is not yet implemented.", size);
		(void)dump;
	}

	static void GFSDK_AFTERMATH_CALL OnShaderDebugInfo(const void* /*data*/, uint32_t /*size*/, void* /*user*/)
	{
		// Per-shader debug info — write alongside the dump for source-level decoding.
	}

	static void GFSDK_AFTERMATH_CALL OnDescription(PFN_GFSDK_Aftermath_AddGpuCrashDumpDescription addDesc, void* /*user*/)
	{
		addDesc(GFSDK_Aftermath_GpuCrashDumpDescriptionKey_ApplicationName, "TrinyxEngine");
		addDesc(GFSDK_Aftermath_GpuCrashDumpDescriptionKey_ApplicationVersion, "0.1");
	}
	#endif

	bool Initialize()
	{
	#if TNX_AFTERMATH_HAVE_SDK
		auto result = GFSDK_Aftermath_EnableGpuCrashDumps(
			GFSDK_Aftermath_Version_API,
			GFSDK_Aftermath_GpuCrashDumpWatchedApiFlags_Vulkan,
			GFSDK_Aftermath_GpuCrashDumpFeatureFlags_Default,
			OnCrashDump,
			OnShaderDebugInfo,
			OnDescription,
			nullptr,
			nullptr);
		if (GFSDK_Aftermath_SUCCEED(result))
		{
			gEnabled = true;
			LOG_ENG_INFO("[Aftermath] GPU crash dump capture: ENABLED");
			return true;
		}
		LOG_ENG_WARN_F("[Aftermath] GFSDK_Aftermath_EnableGpuCrashDumps failed: 0x%x", static_cast<unsigned>(result));
		return false;
	#else
		LOG_ENG_INFO("[Aftermath] SDK not present at build time — crash dump capture disabled (stub).");
		return false;
	#endif
	}

	void Shutdown()
	{
	#if TNX_AFTERMATH_HAVE_SDK
		if (gEnabled) GFSDK_Aftermath_DisableGpuCrashDumps();
		gEnabled = false;
	#endif
	}

	bool IsEnabled() { return gEnabled; }

	bool TryAddDeviceExtension(VkPhysicalDevice physDev, const char** outExtensionName)
	{
		if (!gEnabled || physDev == VK_NULL_HANDLE) return false;

		uint32_t count = 0;
		vkEnumerateDeviceExtensionProperties(physDev, nullptr, &count, nullptr);
		if (count == 0) return false;

		// Stack-allocated for typical extension count (<256). Heap if needed.
		VkExtensionProperties props[256];
		if (count > 256) count = 256;
		vkEnumerateDeviceExtensionProperties(physDev, nullptr, &count, props);

		for (uint32_t i = 0; i < count; ++i)
		{
			if (std::strcmp(props[i].extensionName, "VK_NV_device_diagnostics_config") == 0)
			{
				if (outExtensionName) *outExtensionName = "VK_NV_device_diagnostics_config";
				return true;
			}
		}
		return false;
	}

	const void* PrependDeviceCreateChain(const void* existingChain)
	{
		if (!gEnabled) return existingChain;
		// VkDeviceDiagnosticsConfigCreateInfoNV is in the standard Vulkan
		// headers (extension VK_NV_device_diagnostics_config), so the type
		// is available even when the Aftermath SDK is missing — but enabling
		// it is only meaningful with the SDK active.
		static VkDeviceDiagnosticsConfigCreateInfoNV info{};
		info.sType = VK_STRUCTURE_TYPE_DEVICE_DIAGNOSTICS_CONFIG_CREATE_INFO_NV;
		info.flags = VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_RESOURCE_TRACKING_BIT_NV
			| VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_AUTOMATIC_CHECKPOINTS_BIT_NV
			| VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_SHADER_DEBUG_INFO_BIT_NV;
		info.pNext = const_cast<void*>(existingChain);
		return &info;
	}

	void Checkpoint(VkCommandBuffer cmd, const char* marker)
	{
	#if TNX_AFTERMATH_HAVE_SDK
		if (!gEnabled || !vkCmdSetCheckpointNV) return;
		vkCmdSetCheckpointNV(cmd, marker);
	#else
		(void)cmd; (void)marker;
	#endif
	}

#else // TNX_ENABLE_AFTERMATH

	bool Initialize()                                                   { return false; }
	void Shutdown()                                                     {}
	bool IsEnabled()                                                    { return false; }
	bool TryAddDeviceExtension(VkPhysicalDevice, const char**)          { return false; }
	const void* PrependDeviceCreateChain(const void* existing)          { return existing; }
	void Checkpoint(VkCommandBuffer, const char*)                       {}

#endif // TNX_ENABLE_AFTERMATH
} // namespace Aftermath
