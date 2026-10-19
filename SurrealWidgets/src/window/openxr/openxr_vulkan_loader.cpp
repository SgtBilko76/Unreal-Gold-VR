#include "openxr_vulkan_loader.h"
#include <stdexcept>
#include <dlfcn.h>
#include <android/log.h>

#define LOG_TAG "SurrealWidgets-OpenXR"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static void* LoaderHandle = nullptr;

bool OpenXRFunctions::LoadExportedFunctions()
{
	if (!LoaderHandle)
	{
		LoaderHandle = dlopen("libopenxr_loader.so", RTLD_NOW | RTLD_LOCAL);
		if (!LoaderHandle)
		{
			ALOGE("Failed to load libopenxr_loader.so: %s", dlerror());
			return false;
		}
	}

	xrGetInstanceProcAddr = (PFN_xrGetInstanceProcAddr)dlsym(LoaderHandle, "xrGetInstanceProcAddr");
	if (!xrGetInstanceProcAddr)
	{
		ALOGE("Failed to load xrGetInstanceProcAddr: %s", dlerror());
		return false;
	}

	XrResult result = xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction*)&xrCreateInstance);
	if (XR_FAILED(result) || !xrCreateInstance)
	{
		ALOGE("Failed to load xrCreateInstance");
		return false;
	}

	result = xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction*)&xrEnumerateInstanceExtensionProperties);
	if (XR_FAILED(result) || !xrEnumerateInstanceExtensionProperties)
	{
		ALOGE("Failed to load xrEnumerateInstanceExtensionProperties");
		return false;
	}

	// xrInitializeLoaderKHR is optional - only present on Android loaders, and only needed
	// to hand the loader the JavaVM/Activity before xrCreateInstance (see
	// OpenXRSession::CreateInstance in openxr_display_window.cpp).
	result = xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction*)&xrInitializeLoaderKHR);
	if (XR_FAILED(result))
		xrInitializeLoaderKHR = nullptr;

	return true;
}

static void LoadFn(XrInstance instance, PFN_xrGetInstanceProcAddr getProcAddr, const char* name, PFN_xrVoidFunction* out)
{
	XrResult result = getProcAddr(instance, name, out);
	if (XR_FAILED(result) || *out == nullptr)
	{
		ALOGE("Failed to load required OpenXR function %s", name);
		throw std::runtime_error(std::string("Failed to load required OpenXR function ") + name);
	}
}

#define LOAD(name) LoadFn(instance, xrGetInstanceProcAddr, #name, (PFN_xrVoidFunction*)&name)

void OpenXRFunctions::LoadInstanceFunctions(XrInstance instance)
{
	LOAD(xrDestroyInstance);
	LOAD(xrResultToString);
	LOAD(xrGetInstanceProperties);
	LOAD(xrGetSystem);
	LOAD(xrGetSystemProperties);
	LOAD(xrEnumerateViewConfigurations);
	LOAD(xrEnumerateViewConfigurationViews);
	LOAD(xrGetVulkanGraphicsRequirementsKHR);
	LOAD(xrGetVulkanInstanceExtensionsKHR);
	LOAD(xrGetVulkanDeviceExtensionsKHR);
	LOAD(xrGetVulkanGraphicsDeviceKHR);
	LOAD(xrCreateSession);
	LOAD(xrDestroySession);
	LOAD(xrBeginSession);
	LOAD(xrEndSession);
	LOAD(xrRequestExitSession);
	LOAD(xrPollEvent);
	LOAD(xrCreateReferenceSpace);
	LOAD(xrDestroySpace);
	LOAD(xrLocateSpace);
	LOAD(xrLocateViews);
	LOAD(xrCreateSwapchain);
	LOAD(xrEnumerateSwapchainFormats);
	LOAD(xrDestroySwapchain);
	LOAD(xrEnumerateSwapchainImages);
	LOAD(xrAcquireSwapchainImage);
	LOAD(xrWaitSwapchainImage);
	LOAD(xrReleaseSwapchainImage);
	LOAD(xrWaitFrame);
	LOAD(xrBeginFrame);
	LOAD(xrEndFrame);
	LOAD(xrCreateActionSet);
	LOAD(xrDestroyActionSet);
	LOAD(xrCreateAction);
	LOAD(xrStringToPath);
	LOAD(xrSuggestInteractionProfileBindings);
	LOAD(xrCreateActionSpace);
	LOAD(xrAttachSessionActionSets);
	LOAD(xrSyncActions);
	LOAD(xrGetActionStateBoolean);
	LOAD(xrGetActionStateFloat);
	LOAD(xrGetActionStateVector2f);
	LOAD(xrGetActionStatePose);
	LOAD(xrApplyHapticFeedback);
	LOAD(xrStopHapticFeedback);

	// Optional (XR_EXT_performance_settings) - not LOAD()'d since a runtime that didn't
	// enable the extension (see CreateInstanceAndSystem's hasExtension check) won't expose it;
	// left null in that case and callers must check before use.
	XrResult perfResult = xrGetInstanceProcAddr(instance, "xrPerfSettingsSetPerformanceLevelEXT", (PFN_xrVoidFunction*)&xrPerfSettingsSetPerformanceLevelEXT);
	if (XR_FAILED(perfResult))
		xrPerfSettingsSetPerformanceLevelEXT = nullptr;
}

#undef LOAD

void OpenXRCheckResult(XrInstance instance, const OpenXRFunctions& fn, XrResult result, const char* function)
{
	if (XR_FAILED(result))
	{
		char buffer[XR_MAX_RESULT_STRING_SIZE] = {};
		if (fn.xrResultToString && instance != XR_NULL_HANDLE)
			fn.xrResultToString(instance, result, buffer);
		else
			snprintf(buffer, sizeof(buffer), "%d", (int)result);
		ALOGE("OpenXR error: %s: %s", function, buffer);
		throw std::runtime_error(std::string("OpenXR error in ") + function + ": " + buffer);
	}
}
