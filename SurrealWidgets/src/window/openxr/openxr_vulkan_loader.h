#pragma once

// OpenXR function pointers and small shared state used by both OpenXRDisplayBackend and
// OpenXRDisplayWindow. Kept in one header since the OpenXR instance/session genuinely is
// process-global on this backend (there is only ever one XR session per app), unlike the
// desktop backends where multiple top-level DisplayWindows are a real possibility.

#define XR_USE_GRAPHICS_API_VULKAN 1
#define XR_USE_PLATFORM_ANDROID 1

#include <jni.h>
#include <android/native_window.h>
#include <vulkan/vulkan.h> // must precede openxr_platform.h, which uses Vk* types but does not include the Vulkan headers itself
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <vector>
#include <string>

// Declared PFN_ extern globals for every OpenXR function this backend calls, loaded once via
// xrGetInstanceProcAddr in OpenXRSession::LoadInstanceFunctions(). Mirrors the loading
// pattern used by Team Beef Studios' QuakeQuest (TBXR_Common.c), adapted to the
// XR_KHR_vulkan_enable (not vulkan_enable2 - see openxr_display_window.cpp for why) Vulkan
// binding and to C++.
struct OpenXRFunctions
{
	PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr = nullptr;
	PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR = nullptr;
	PFN_xrEnumerateInstanceExtensionProperties xrEnumerateInstanceExtensionProperties = nullptr;
	PFN_xrCreateInstance xrCreateInstance = nullptr;
	PFN_xrDestroyInstance xrDestroyInstance = nullptr;
	PFN_xrResultToString xrResultToString = nullptr;
	PFN_xrGetInstanceProperties xrGetInstanceProperties = nullptr;
	PFN_xrGetSystem xrGetSystem = nullptr;
	PFN_xrGetSystemProperties xrGetSystemProperties = nullptr;
	PFN_xrEnumerateViewConfigurations xrEnumerateViewConfigurations = nullptr;
	PFN_xrEnumerateViewConfigurationViews xrEnumerateViewConfigurationViews = nullptr;
	PFN_xrGetVulkanGraphicsRequirementsKHR xrGetVulkanGraphicsRequirementsKHR = nullptr;
	PFN_xrGetVulkanInstanceExtensionsKHR xrGetVulkanInstanceExtensionsKHR = nullptr;
	PFN_xrGetVulkanDeviceExtensionsKHR xrGetVulkanDeviceExtensionsKHR = nullptr;
	PFN_xrGetVulkanGraphicsDeviceKHR xrGetVulkanGraphicsDeviceKHR = nullptr;
	PFN_xrCreateSession xrCreateSession = nullptr;
	PFN_xrDestroySession xrDestroySession = nullptr;
	PFN_xrBeginSession xrBeginSession = nullptr;
	PFN_xrEndSession xrEndSession = nullptr;
	PFN_xrRequestExitSession xrRequestExitSession = nullptr;
	PFN_xrPollEvent xrPollEvent = nullptr;
	PFN_xrCreateReferenceSpace xrCreateReferenceSpace = nullptr;
	PFN_xrDestroySpace xrDestroySpace = nullptr;
	PFN_xrLocateSpace xrLocateSpace = nullptr;
	PFN_xrLocateViews xrLocateViews = nullptr;
	PFN_xrCreateSwapchain xrCreateSwapchain = nullptr;
	PFN_xrEnumerateSwapchainFormats xrEnumerateSwapchainFormats = nullptr;
	PFN_xrDestroySwapchain xrDestroySwapchain = nullptr;
	PFN_xrEnumerateSwapchainImages xrEnumerateSwapchainImages = nullptr;
	PFN_xrAcquireSwapchainImage xrAcquireSwapchainImage = nullptr;
	PFN_xrWaitSwapchainImage xrWaitSwapchainImage = nullptr;
	PFN_xrReleaseSwapchainImage xrReleaseSwapchainImage = nullptr;
	PFN_xrWaitFrame xrWaitFrame = nullptr;
	PFN_xrBeginFrame xrBeginFrame = nullptr;
	PFN_xrEndFrame xrEndFrame = nullptr;
	PFN_xrCreateActionSet xrCreateActionSet = nullptr;
	PFN_xrDestroyActionSet xrDestroyActionSet = nullptr;
	PFN_xrCreateAction xrCreateAction = nullptr;
	PFN_xrStringToPath xrStringToPath = nullptr;
	PFN_xrSuggestInteractionProfileBindings xrSuggestInteractionProfileBindings = nullptr;
	PFN_xrCreateActionSpace xrCreateActionSpace = nullptr;
	PFN_xrAttachSessionActionSets xrAttachSessionActionSets = nullptr;
	PFN_xrSyncActions xrSyncActions = nullptr;
	PFN_xrGetActionStateBoolean xrGetActionStateBoolean = nullptr;
	PFN_xrGetActionStateFloat xrGetActionStateFloat = nullptr;
	PFN_xrGetActionStateVector2f xrGetActionStateVector2f = nullptr;
	PFN_xrGetActionStatePose xrGetActionStatePose = nullptr;
	PFN_xrApplyHapticFeedback xrApplyHapticFeedback = nullptr;
	PFN_xrStopHapticFeedback xrStopHapticFeedback = nullptr;

	// XR_EXT_performance_settings (optional - null if the runtime doesn't support it, checked
	// before use). Requesting BOOST on both domains once the session is running matches Team
	// Beef Studios' QuakeQuest (TBXR_Common.c) - without it the Quest may run CPU/GPU at a
	// conservative default clock, and a missed 72/90Hz frame deadline makes the compositor's
	// own reprojection (ASW/PTW) visibly double/ghost the image during head movement, which is
	// indistinguishable from a rendering bug to the player.
	PFN_xrPerfSettingsSetPerformanceLevelEXT xrPerfSettingsSetPerformanceLevelEXT = nullptr;

	// Loads xrGetInstanceProcAddr (and xrInitializeLoaderKHR/xrCreateInstance/
	// xrEnumerateInstanceExtensionProperties, callable before any XrInstance exists) from
	// libopenxr_loader.so. Must be called before CreateInstance().
	bool LoadExportedFunctions();

	// Loads every other function pointer above via xrGetInstanceProcAddr(instance, ...).
	// Throws std::runtime_error if a required function isn't exposed by the runtime.
	void LoadInstanceFunctions(XrInstance instance);
};

// Prints an ALOGE-equivalent and, in debug builds, aborts. Used by the OXR() macro below,
// mirroring TBXR_Common.h's OXR_CheckErrors/OXR() pattern.
void OpenXRCheckResult(XrInstance instance, const OpenXRFunctions& fn, XrResult result, const char* function);

#define OXR(fn, instance, expr) OpenXRCheckResult(instance, fn, (expr), #expr)
