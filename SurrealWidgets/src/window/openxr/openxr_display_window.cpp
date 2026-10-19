#include "openxr_display_window.h"
#include "openxr_display_backend.h"
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <android/log.h>

#define LOG_TAG "SurrealWidgets-OpenXR"
#define ALOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Set by OpenXRDisplayBackend::SetAndroidApp before the window is constructed.
extern JavaVM* OpenXR_JavaVM;
extern jobject OpenXR_ActivityObject;

static const char* ViewConfigType() { return "stereo"; }

OpenXRDisplayWindow::OpenXRDisplayWindow(DisplayWindowHost* windowHost, WidgetType type, RenderAPI renderAPI) : WindowHost(windowHost)
{
	if (renderAPI != RenderAPI::Vulkan)
		throw std::runtime_error("The OpenXR backend only supports the Vulkan render API");

	if (!fn.LoadExportedFunctions())
		throw std::runtime_error("Could not load libopenxr_loader.so");

	CreateInstanceAndSystem();
	EnumerateViews();
}

OpenXRDisplayWindow::~OpenXRDisplayWindow()
{
	OpenXRDisplayBackend::ClearActiveWindow(this);

	DestroySwapchains();

	for (XrSpace& space : HandSpace)
	{
		if (space != XR_NULL_HANDLE)
			fn.xrDestroySpace(space);
	}
	if (ActionSet != XR_NULL_HANDLE)
		fn.xrDestroyActionSet(ActionSet);
	if (HeadSpace != XR_NULL_HANDLE)
		fn.xrDestroySpace(HeadSpace);
	if (StageSpace != XR_NULL_HANDLE)
		fn.xrDestroySpace(StageSpace);
	if (Session != XR_NULL_HANDLE)
		fn.xrDestroySession(Session);
	if (Instance != XR_NULL_HANDLE)
		fn.xrDestroyInstance(Instance);
}

void OpenXRDisplayWindow::CreateInstanceAndSystem()
{
#ifdef XR_USE_PLATFORM_ANDROID
	if (fn.xrInitializeLoaderKHR && OpenXR_JavaVM && OpenXR_ActivityObject)
	{
		XrLoaderInitInfoAndroidKHR loaderInitInfo = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
		loaderInitInfo.applicationVM = OpenXR_JavaVM;
		loaderInitInfo.applicationContext = OpenXR_ActivityObject;
		fn.xrInitializeLoaderKHR((const XrLoaderInitInfoBaseHeaderKHR*)&loaderInitInfo);
	}
#endif

	uint32_t extensionCount = 0;
	fn.xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr);
	std::vector<XrExtensionProperties> extensions(extensionCount, { XR_TYPE_EXTENSION_PROPERTIES });
	fn.xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensions.data());

	auto hasExtension = [&](const char* name)
	{
		for (const auto& ext : extensions)
			if (std::strcmp(ext.extensionName, name) == 0)
				return true;
		return false;
	};

	if (!hasExtension(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME))
		throw std::runtime_error("OpenXR runtime does not support " XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);

	std::vector<const char*> enabledExtensions;
	enabledExtensions.push_back(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
#ifdef XR_USE_PLATFORM_ANDROID
	if (hasExtension(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME))
		enabledExtensions.push_back(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
#endif
	// See openxr_vulkan_loader.h's xrPerfSettingsSetPerformanceLevelEXT comment - requests max
	// CPU/GPU clocks once the session is running (PollEvents' XR_SESSION_STATE_READY case
	// below), matching QuakeQuest, to avoid compositor-side ghosting from missed frame
	// deadlines. Optional: only enabled if the runtime actually supports it.
	if (hasExtension(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME))
		enabledExtensions.push_back(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);

	// Pico headsets: PICO OS serves the standard Android OpenXR loader, but its controllers
	// have their own interaction profiles behind this vendor extension (see CreateActions).
	if (hasExtension("XR_BD_controller_interaction"))
	{
		enabledExtensions.push_back("XR_BD_controller_interaction");
		HasPicoControllerExt = true;
	}

	XrInstanceCreateInfo createInfo = { XR_TYPE_INSTANCE_CREATE_INFO };
#ifdef XR_USE_PLATFORM_ANDROID
	XrInstanceCreateInfoAndroidKHR androidInfo = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
	if (OpenXR_JavaVM && OpenXR_ActivityObject)
	{
		androidInfo.applicationVM = OpenXR_JavaVM;
		androidInfo.applicationActivity = OpenXR_ActivityObject;
		createInfo.next = &androidInfo;
	}
#endif
	createInfo.enabledExtensionCount = (uint32_t)enabledExtensions.size();
	createInfo.enabledExtensionNames = enabledExtensions.data();
	std::strncpy(createInfo.applicationInfo.applicationName, "SurrealEngine", XR_MAX_APPLICATION_NAME_SIZE - 1);
	createInfo.applicationInfo.applicationVersion = 1;
	createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

	OXR(fn, XR_NULL_HANDLE, fn.xrCreateInstance(&createInfo, &Instance));

	fn.LoadInstanceFunctions(Instance);

	XrSystemGetInfo systemInfo = { XR_TYPE_SYSTEM_GET_INFO };
	systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	OXR(fn, Instance, fn.xrGetSystem(Instance, &systemInfo, &SystemId));
}

void OpenXRDisplayWindow::EnumerateViews()
{
	uint32_t viewCount = 0;
	fn.xrEnumerateViewConfigurationViews(Instance, SystemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr);
	if (viewCount != EyeCount)
		throw std::runtime_error("Expected a stereo (2-view) OpenXR view configuration");

	for (auto& view : ViewConfigs)
		view.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;

	fn.xrEnumerateViewConfigurationViews(Instance, SystemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, ViewConfigs.data());

	// Cap the per-eye resolution to an ABSOLUTE ceiling rather than a scale relative to
	// whatever the runtime recommends. SurrealEngine renders a full lightmapped BSP level with
	// two complete passes per app-frame (one per eye, no multiview - see the VR port plan's
	// non-goals), much heavier than a typical mobile VR title; confirmed on real Quest 3
	// hardware via a screen recording (adb shell screenrecord - regular screencap doesn't
	// capture OpenXR swapchain content) that a too-high resolution pushes render time to ~20ms/
	// eye-pair (vs. the ~11ms/13.9ms budget for 90Hz/72Hz), and the Quest compositor's own
	// frame reprojection then visibly hard-doubles both the 2D HUD text and 3D geometry within
	// each eye - not a rendering logic bug, a frame-budget one. A relative scale (the previous
	// approach here) isn't reliable: the runtime-recommended size varies noticeably between
	// sessions (observed both ~1680x1760 and ~2800x2933 per eye on the same headset), so a 0.7x
	// multiplier of it doesn't bound the actual render cost. This caps to whichever is smaller
	// - the recommended size, or this absolute ceiling - so cost stays bounded regardless of
	// what the runtime asks for this session. Applied here (once, to the recommended sizes
	// themselves) so every downstream consumer - CreateSwapchain(), GetEyeImageSize(),
	// GetPixelWidth/Height(), GetClientSize/Frame() - automatically uses the capped resolution
	// without needing its own scaling logic.
	// Raised from 1200x1260; VrApi stats at the lower cap showed the GPU at
	// ~50% with the CPU-side double scene pass as the limiting factor, so there was headroom.
	constexpr uint32_t MaxEyeRenderWidth = 1600;  // raised again once CommandBufferManager pipelined the eye submissions (68-73 fps at 1440x1512 with GPU ~74%)
	constexpr uint32_t MaxEyeRenderHeight = 1680;
	for (auto& view : ViewConfigs)
	{
		view.recommendedImageRectWidth = std::min(view.recommendedImageRectWidth, MaxEyeRenderWidth);
		view.recommendedImageRectHeight = std::min(view.recommendedImageRectHeight, MaxEyeRenderHeight);
	}

	for (auto& view : Views)
		view.type = XR_TYPE_VIEW;
}

void OpenXRDisplayWindow::GetEyeImageSize(int eye, int* width, int* height)
{
	*width = (int)ViewConfigs[eye].recommendedImageRectWidth;
	*height = (int)ViewConfigs[eye].recommendedImageRectHeight;
}

Size OpenXRDisplayWindow::GetClientSize() const
{
	return Size((double)ViewConfigs[0].recommendedImageRectWidth, (double)ViewConfigs[0].recommendedImageRectHeight);
}

int OpenXRDisplayWindow::GetPixelWidth() const
{
	// RenderingScreenLayer (set by AcquireScreenLayerImage()/ReleaseScreenLayerImage()) takes
	// priority over CurrentRenderEye - VulkanRenderDevice::Lock() sizes the shared offscreen
	// "Scene" buffer from this every frame via Viewport->GetNativePixelWidth/Height(), and the
	// screen layer's resolution (ScreenWidth/Height) is unrelated to and normally much lower
	// than either eye's, so reporting an eye's size while rendering the screen layer would
	// size the Scene buffer wrong for it (the same class of bug CurrentRenderEye itself was
	// added to fix - see this method's original comment history).
	if (RenderingScreenLayer)
		return ScreenWidth;
	return (int)ViewConfigs[CurrentRenderEye].recommendedImageRectWidth;
}

int OpenXRDisplayWindow::GetPixelHeight() const
{
	if (RenderingScreenLayer)
		return ScreenHeight;
	return (int)ViewConfigs[CurrentRenderEye].recommendedImageRectHeight;
}

Rect OpenXRDisplayWindow::GetClientFrame() const
{
	return Rect::xywh(0.0, 0.0, (double)ViewConfigs[0].recommendedImageRectWidth, (double)ViewConfigs[0].recommendedImageRectHeight);
}

std::vector<std::string> OpenXRDisplayWindow::GetVulkanInstanceRequirements()
{
	uint32_t size = 0;
	OXR(fn, Instance, fn.xrGetVulkanInstanceExtensionsKHR(Instance, SystemId, 0, &size, nullptr));
	std::string buffer(size, '\0');
	OXR(fn, Instance, fn.xrGetVulkanInstanceExtensionsKHR(Instance, SystemId, size, &size, buffer.data()));

	std::vector<std::string> result;
	size_t start = 0;
	for (size_t i = 0; i <= buffer.size(); i++)
	{
		if (i == buffer.size() || buffer[i] == ' ' || buffer[i] == '\0')
		{
			if (i > start)
				result.push_back(buffer.substr(start, i - start));
			start = i + 1;
		}
	}
	return result;
}

std::vector<std::string> OpenXRDisplayWindow::GetVulkanDeviceRequirements()
{
	uint32_t size = 0;
	OXR(fn, Instance, fn.xrGetVulkanDeviceExtensionsKHR(Instance, SystemId, 0, &size, nullptr));
	std::string buffer(size, '\0');
	OXR(fn, Instance, fn.xrGetVulkanDeviceExtensionsKHR(Instance, SystemId, size, &size, buffer.data()));

	std::vector<std::string> result;
	size_t start = 0;
	for (size_t i = 0; i <= buffer.size(); i++)
	{
		if (i == buffer.size() || buffer[i] == ' ' || buffer[i] == '\0')
		{
			if (i > start)
				result.push_back(buffer.substr(start, i - start));
			start = i + 1;
		}
	}
	return result;
}

VkPhysicalDevice OpenXRDisplayWindow::SelectVulkanPhysicalDevice(VkInstance instance)
{
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	OXR(fn, Instance, fn.xrGetVulkanGraphicsDeviceKHR(Instance, SystemId, instance, &physicalDevice));
	return physicalDevice;
}

void OpenXRDisplayWindow::CreateVulkanSession(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex)
{
	// The spec requires this call to precede xrCreateSession with a Vulkan graphics binding,
	// even though nothing here needs its output (SurrealGPU/VulkanRenderDevice already
	// negotiated the Vulkan API version on its own) - omitting it fails session creation with
	// XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING (confirmed on real Quest 3 hardware).
	XrGraphicsRequirementsVulkanKHR requirements = { XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR };
	OXR(fn, Instance, fn.xrGetVulkanGraphicsRequirementsKHR(Instance, SystemId, &requirements));

	XrGraphicsBindingVulkanKHR binding = { XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR };
	binding.instance = instance;
	binding.physicalDevice = physicalDevice;
	binding.device = device;
	binding.queueFamilyIndex = queueFamilyIndex;
	binding.queueIndex = queueIndex;

	XrSessionCreateInfo sessionInfo = { XR_TYPE_SESSION_CREATE_INFO };
	sessionInfo.next = &binding;
	sessionInfo.systemId = SystemId;
	OXR(fn, Instance, fn.xrCreateSession(Instance, &sessionInfo, &Session));

	XrReferenceSpaceCreateInfo spaceInfo = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;

	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	OXR(fn, Instance, fn.xrCreateReferenceSpace(Session, &spaceInfo, &HeadSpace));

	// LOCAL, not STAGE, as the tracking space (the member keeps its historical name): the
	// headset's own recenter gesture (long-press on the Meta button) moves the LOCAL space
	// origin to the current head pose and reports it via
	// XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING (PollEvents), whereas STAGE is pinned
	// to the room's guardian and ignores it - confirmed on real Quest 3 hardware as "the
	// re-centering button does not work". The engine never relied on STAGE's floor-level
	// origin: every pose is taken relative to a head anchor captured at start (VRInput), which
	// is simply re-captured after a recenter.
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	XrResult localResult = fn.xrCreateReferenceSpace(Session, &spaceInfo, &StageSpace);
	if (XR_FAILED(localResult))
	{
		spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
		OXR(fn, Instance, fn.xrCreateReferenceSpace(Session, &spaceInfo, &StageSpace));
	}

	for (int eye = 0; eye < EyeCount; eye++)
		CreateSwapchain(eye);

	CreateActions();

	ProjectionLayer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
	ProjectionLayer.space = StageSpace;
	ProjectionLayer.viewCount = EyeCount;
	ProjectionLayer.views = ProjectionViews;
	for (int eye = 0; eye < EyeCount; eye++)
	{
		ProjectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
		ProjectionViews[eye].subImage.swapchain = Swapchains[eye];
		ProjectionViews[eye].subImage.imageRect.offset = { 0, 0 };
		ProjectionViews[eye].subImage.imageRect.extent = { (int32_t)ViewConfigs[eye].recommendedImageRectWidth, (int32_t)ViewConfigs[eye].recommendedImageRectHeight };
	}
}

// The present shader writes gamma-encoded (sRGB) colors, like the desktop swapchain. The OpenXR
// compositor treats a UNORM swapchain as LINEAR and encodes it again for the display, which
// lifts all dark tones (the game looked too bright, with flat lighting and no visible shadows on
// the Quest 3). So the swapchain is created as _SRGB - telling the compositor the bytes are
// sRGB - with the mutable-format bit, and the engine keeps rendering through UNORM image views
// (FramebufferManager: PresentVR.Format), so the bytes are stored as written, not re-encoded.
// Falls back to plain UNORM if the runtime offers no R8G8B8A8_SRGB swapchains.
void OpenXRDisplayWindow::SetSwapchainFormat(XrSwapchainCreateInfo& createInfo)
{
	createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	createInfo.format = VK_FORMAT_R8G8B8A8_UNORM;

	uint32_t count = 0;
	if (fn.xrEnumerateSwapchainFormats && XR_SUCCEEDED(fn.xrEnumerateSwapchainFormats(Session, 0, &count, nullptr)) && count > 0)
	{
		std::vector<int64_t> formats(count);
		if (XR_SUCCEEDED(fn.xrEnumerateSwapchainFormats(Session, count, &count, formats.data())))
		{
			for (uint32_t i = 0; i < count; i++)
			{
				if (formats[i] == VK_FORMAT_R8G8B8A8_SRGB)
				{
					createInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
					createInfo.usageFlags |= XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
					break;
				}
			}
		}
	}
}

void OpenXRDisplayWindow::CreateSwapchain(int eye)
{
	XrSwapchainCreateInfo createInfo = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	SetSwapchainFormat(createInfo);
	createInfo.sampleCount = 1;
	createInfo.width = ViewConfigs[eye].recommendedImageRectWidth;
	createInfo.height = ViewConfigs[eye].recommendedImageRectHeight;
	createInfo.faceCount = 1;
	createInfo.arraySize = 1;
	createInfo.mipCount = 1;

	OXR(fn, Instance, fn.xrCreateSwapchain(Session, &createInfo, &Swapchains[eye]));

	uint32_t imageCount = 0;
	fn.xrEnumerateSwapchainImages(Swapchains[eye], 0, &imageCount, nullptr);
	SwapchainImages[eye].resize(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR });
	OXR(fn, Instance, fn.xrEnumerateSwapchainImages(Swapchains[eye], imageCount, &imageCount, (XrSwapchainImageBaseHeader*)SwapchainImages[eye].data()));
}

void OpenXRDisplayWindow::DestroySwapchains()
{
	for (int eye = 0; eye < EyeCount; eye++)
	{
		if (Swapchains[eye] != XR_NULL_HANDLE)
		{
			fn.xrDestroySwapchain(Swapchains[eye]);
			Swapchains[eye] = XR_NULL_HANDLE;
		}
		SwapchainImages[eye].clear();
	}

	if (ScreenSwapchain != XR_NULL_HANDLE)
	{
		fn.xrDestroySwapchain(ScreenSwapchain);
		ScreenSwapchain = XR_NULL_HANDLE;
	}
	ScreenSwapchainImages.clear();
}

void OpenXRDisplayWindow::CreateScreenLayerSwapchain(int width, int height)
{
	ScreenWidth = width;
	ScreenHeight = height;

	XrSwapchainCreateInfo createInfo = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	SetSwapchainFormat(createInfo);
	createInfo.sampleCount = 1;
	createInfo.width = (uint32_t)width;
	createInfo.height = (uint32_t)height;
	createInfo.faceCount = 1;
	createInfo.arraySize = 1;
	createInfo.mipCount = 1;

	OXR(fn, Instance, fn.xrCreateSwapchain(Session, &createInfo, &ScreenSwapchain));

	uint32_t imageCount = 0;
	fn.xrEnumerateSwapchainImages(ScreenSwapchain, 0, &imageCount, nullptr);
	ScreenSwapchainImages.resize(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR });
	OXR(fn, Instance, fn.xrEnumerateSwapchainImages(ScreenSwapchain, imageCount, &imageCount, (XrSwapchainImageBaseHeader*)ScreenSwapchainImages.data()));

	ScreenQuadLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
	ScreenQuadLayer.layerFlags = 0;
	ScreenQuadLayer.space = StageSpace;
	ScreenQuadLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	ScreenQuadLayer.subImage.swapchain = ScreenSwapchain;
	ScreenQuadLayer.subImage.imageRect.offset = { 0, 0 };
	ScreenQuadLayer.subImage.imageRect.extent = { width, height };
	ScreenQuadLayer.pose.orientation.w = 1.0f;
}

int OpenXRDisplayWindow::AcquireScreenLayerImage()
{
	RenderingScreenLayer = true; // see GetPixelWidth/GetPixelHeight()

	XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	uint32_t index = 0;
	OXR(fn, Instance, fn.xrAcquireSwapchainImage(ScreenSwapchain, &acquireInfo, &index));

	XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	waitInfo.timeout = XR_INFINITE_DURATION;
	OXR(fn, Instance, fn.xrWaitSwapchainImage(ScreenSwapchain, &waitInfo));

	ScreenAcquiredImageIndex = (int)index;
	return (int)index;
}

VkImage OpenXRDisplayWindow::GetScreenLayerImage(int imageIndex)
{
	return ScreenSwapchainImages[imageIndex].image;
}

void OpenXRDisplayWindow::ReleaseScreenLayerImage()
{
	XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	OXR(fn, Instance, fn.xrReleaseSwapchainImage(ScreenSwapchain, &releaseInfo));
	ScreenAcquiredImageIndex = -1;
	ScreenRenderedThisFrame = true;
	RenderingScreenLayer = false;
}

void OpenXRDisplayWindow::SetScreenLayerPose(float positionX, float positionY, float positionZ, float orientationX, float orientationY, float orientationZ, float orientationW, float widthMeters, float heightMeters)
{
	ScreenQuadLayer.pose.position = { positionX, positionY, positionZ };
	ScreenQuadLayer.pose.orientation = { orientationX, orientationY, orientationZ, orientationW };
	ScreenQuadLayer.size = { widthMeters, heightMeters };
}

static XrPath MakePath(const OpenXRFunctions& fn, XrInstance instance, const char* pathString)
{
	XrPath path = XR_NULL_PATH;
	fn.xrStringToPath(instance, pathString, &path);
	return path;
}

static XrAction CreateAction(const OpenXRFunctions& fn, XrInstance instance, XrActionSet actionSet, XrActionType type, const char* name, const XrPath* subactionPaths, uint32_t subactionCount)
{
	XrActionCreateInfo info = { XR_TYPE_ACTION_CREATE_INFO };
	std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
	std::strncpy(info.localizedActionName, name, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
	info.actionType = type;
	info.countSubactionPaths = subactionCount;
	info.subactionPaths = subactionPaths;

	XrAction action = XR_NULL_HANDLE;
	OXR(fn, instance, fn.xrCreateAction(actionSet, &info, &action));
	return action;
}

// Path strings and interaction-profile binding shape verified against Team Beef Studios'
// QuakeQuest (a real, working OpenXR title on Quest), Projects/Android/jni/QuakeQuestSrc/
// OpenXrInput.c - see openxr_display_window.h's class comment. On Touch controllers, X/Y live
// on the left hand and A/B on the right; ButtonAAction/ButtonBAction below reuse the same two
// XrActions across both hands via subactionPath rather than needing four separate actions,
// matching how VRControllerState models "primary/secondary face button" generically per hand.
void OpenXRDisplayWindow::CreateActions()
{
	HandSubactionPath[0] = MakePath(fn, Instance, "/user/hand/left");
	HandSubactionPath[1] = MakePath(fn, Instance, "/user/hand/right");

	XrActionSetCreateInfo setInfo = { XR_TYPE_ACTION_SET_CREATE_INFO };
	std::strncpy(setInfo.actionSetName, "gameplay", XR_MAX_ACTION_SET_NAME_SIZE - 1);
	std::strncpy(setInfo.localizedActionSetName, "Gameplay", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
	setInfo.priority = 0;
	OXR(fn, Instance, fn.xrCreateActionSet(Instance, &setInfo, &ActionSet));

	PoseAction = CreateAction(fn, Instance, ActionSet, XR_ACTION_TYPE_POSE_INPUT, "hand_pose", HandSubactionPath, 2);
	TriggerAction = CreateAction(fn, Instance, ActionSet, XR_ACTION_TYPE_FLOAT_INPUT, "trigger", HandSubactionPath, 2);
	GripAction = CreateAction(fn, Instance, ActionSet, XR_ACTION_TYPE_FLOAT_INPUT, "grip", HandSubactionPath, 2);
	ThumbstickAction = CreateAction(fn, Instance, ActionSet, XR_ACTION_TYPE_VECTOR2F_INPUT, "thumbstick", HandSubactionPath, 2);
	ThumbstickClickAction = CreateAction(fn, Instance, ActionSet, XR_ACTION_TYPE_BOOLEAN_INPUT, "thumbstick_click", HandSubactionPath, 2);
	ButtonAAction = CreateAction(fn, Instance, ActionSet, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_a", HandSubactionPath, 2);
	ButtonBAction = CreateAction(fn, Instance, ActionSet, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_b", HandSubactionPath, 2);
	MenuAction = CreateAction(fn, Instance, ActionSet, XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", HandSubactionPath, 2);
	HapticAction = CreateAction(fn, Instance, ActionSet, XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", HandSubactionPath, 2);

	XrPath aimPathL = MakePath(fn, Instance, "/user/hand/left/input/aim/pose");
	XrPath aimPathR = MakePath(fn, Instance, "/user/hand/right/input/aim/pose");
	XrPath triggerPathL = MakePath(fn, Instance, "/user/hand/left/input/trigger/value");
	XrPath triggerPathR = MakePath(fn, Instance, "/user/hand/right/input/trigger/value");
	XrPath gripPathL = MakePath(fn, Instance, "/user/hand/left/input/squeeze/value");
	XrPath gripPathR = MakePath(fn, Instance, "/user/hand/right/input/squeeze/value");
	XrPath thumbstickPathL = MakePath(fn, Instance, "/user/hand/left/input/thumbstick");
	XrPath thumbstickPathR = MakePath(fn, Instance, "/user/hand/right/input/thumbstick");
	XrPath thumbstickClickPathL = MakePath(fn, Instance, "/user/hand/left/input/thumbstick/click");
	XrPath thumbstickClickPathR = MakePath(fn, Instance, "/user/hand/right/input/thumbstick/click");
	XrPath xClickPath = MakePath(fn, Instance, "/user/hand/left/input/x/click");
	XrPath yClickPath = MakePath(fn, Instance, "/user/hand/left/input/y/click");
	XrPath aClickPath = MakePath(fn, Instance, "/user/hand/right/input/a/click");
	XrPath bClickPath = MakePath(fn, Instance, "/user/hand/right/input/b/click");
	XrPath menuClickPath = MakePath(fn, Instance, "/user/hand/left/input/menu/click");
	XrPath hapticPathL = MakePath(fn, Instance, "/user/hand/left/output/haptic");
	XrPath hapticPathR = MakePath(fn, Instance, "/user/hand/right/output/haptic");

	XrActionSuggestedBinding bindings[] = {
		{ PoseAction, aimPathL }, { PoseAction, aimPathR },
		{ TriggerAction, triggerPathL }, { TriggerAction, triggerPathR },
		{ GripAction, gripPathL }, { GripAction, gripPathR },
		{ ThumbstickAction, thumbstickPathL }, { ThumbstickAction, thumbstickPathR },
		{ ThumbstickClickAction, thumbstickClickPathL }, { ThumbstickClickAction, thumbstickClickPathR },
		{ ButtonAAction, xClickPath }, { ButtonAAction, aClickPath },
		{ ButtonBAction, yClickPath }, { ButtonBAction, bClickPath },
		{ MenuAction, menuClickPath },
		{ HapticAction, hapticPathL }, { HapticAction, hapticPathR },
	};

	XrInteractionProfileSuggestedBinding suggested = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
	suggested.interactionProfile = MakePath(fn, Instance, "/interaction_profiles/oculus/touch_controller");
	suggested.suggestedBindings = bindings;
	suggested.countSuggestedBindings = (uint32_t)(sizeof(bindings) / sizeof(bindings[0]));
	OXR(fn, Instance, fn.xrSuggestInteractionProfileBindings(Instance, &suggested));

	// Pico 4 / Neo 3 (XR_BD_controller_interaction): the Pico 4 profile uses the same input
	// path names as Touch, so the same table is suggested verbatim; the Neo 3's grip is a
	// click, which OpenXR converts to our float action. Non-fatal: a runtime that rejects a
	// profile simply leaves it unbound (Quest ignores these entirely). UNTESTED on hardware.
	if (HasPicoControllerExt)
	{
		XrInteractionProfileSuggestedBinding pico = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
		pico.interactionProfile = MakePath(fn, Instance, "/interaction_profiles/bytedance/pico4_controller");
		pico.suggestedBindings = bindings;
		pico.countSuggestedBindings = (uint32_t)(sizeof(bindings) / sizeof(bindings[0]));
		XrResult picoResult = fn.xrSuggestInteractionProfileBindings(Instance, &pico);
		if (XR_FAILED(picoResult))
			ALOGE("Pico 4 controller binding suggestion failed: %d", (int)picoResult);
	
		XrPath gripClickPathL = MakePath(fn, Instance, "/user/hand/left/input/squeeze/click");
		XrPath gripClickPathR = MakePath(fn, Instance, "/user/hand/right/input/squeeze/click");
		XrActionSuggestedBinding neo3Bindings[] = {
			{ PoseAction, aimPathL }, { PoseAction, aimPathR },
			{ TriggerAction, triggerPathL }, { TriggerAction, triggerPathR },
			{ GripAction, gripClickPathL }, { GripAction, gripClickPathR },
			{ ThumbstickAction, thumbstickPathL }, { ThumbstickAction, thumbstickPathR },
			{ ThumbstickClickAction, thumbstickClickPathL }, { ThumbstickClickAction, thumbstickClickPathR },
			{ ButtonAAction, xClickPath }, { ButtonAAction, aClickPath },
			{ ButtonBAction, yClickPath }, { ButtonBAction, bClickPath },
			{ MenuAction, menuClickPath },
			{ HapticAction, hapticPathL }, { HapticAction, hapticPathR },
		};
		pico.interactionProfile = MakePath(fn, Instance, "/interaction_profiles/bytedance/pico_neo3_controller");
		pico.suggestedBindings = neo3Bindings;
		pico.countSuggestedBindings = (uint32_t)(sizeof(neo3Bindings) / sizeof(neo3Bindings[0]));
		picoResult = fn.xrSuggestInteractionProfileBindings(Instance, &pico);
		if (XR_FAILED(picoResult))
			ALOGE("Pico Neo 3 controller binding suggestion failed: %d", (int)picoResult);
	}

	XrActionSpaceCreateInfo spaceInfo = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
	spaceInfo.action = PoseAction;
	spaceInfo.poseInActionSpace.orientation.w = 1.0f;
	spaceInfo.subactionPath = HandSubactionPath[0];
	OXR(fn, Instance, fn.xrCreateActionSpace(Session, &spaceInfo, &HandSpace[0]));
	spaceInfo.subactionPath = HandSubactionPath[1];
	OXR(fn, Instance, fn.xrCreateActionSpace(Session, &spaceInfo, &HandSpace[1]));

	XrSessionActionSetsAttachInfo attachInfo = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	attachInfo.countActionSets = 1;
	attachInfo.actionSets = &ActionSet;
	OXR(fn, Instance, fn.xrAttachSessionActionSets(Session, &attachInfo));
}

void OpenXRDisplayWindow::SyncActions()
{
	if (ActionSet == XR_NULL_HANDLE || !SessionRunning)
		return;

	XrActiveActionSet active = {};
	active.actionSet = ActionSet;
	active.subactionPath = XR_NULL_PATH;

	XrActionsSyncInfo syncInfo = { XR_TYPE_ACTIONS_SYNC_INFO };
	syncInfo.countActiveActionSets = 1;
	syncInfo.activeActionSets = &active;
	fn.xrSyncActions(Session, &syncInfo); // not fatal if this fails transiently (e.g. focus loss), so not OXR()-wrapped
}

MotionControllerPose OpenXRDisplayWindow::GetHeadPose()
{
	MotionControllerPose result;

	if (HeadSpace == XR_NULL_HANDLE || !FrameStateValid)
		return result;

	XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
	XrResult xrResult = fn.xrLocateSpace(HeadSpace, StageSpace, FrameState.predictedDisplayTime, &location);
	if (XR_FAILED(xrResult))
		return result;

	bool posValid = (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
	bool oriValid = (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
	result.Active = posValid && oriValid;
	result.PositionX = location.pose.position.x;
	result.PositionY = location.pose.position.y;
	result.PositionZ = location.pose.position.z;
	result.OrientationX = location.pose.orientation.x;
	result.OrientationY = location.pose.orientation.y;
	result.OrientationZ = location.pose.orientation.z;
	result.OrientationW = location.pose.orientation.w;
	return result;
}

MotionControllerPose OpenXRDisplayWindow::GetControllerPose(VRControllerHand hand)
{
	int index = HandIndex(hand);
	MotionControllerPose result;

	if (HandSpace[index] == XR_NULL_HANDLE || !FrameStateValid)
		return result;

	XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
	XrResult xrResult = fn.xrLocateSpace(HandSpace[index], StageSpace, FrameState.predictedDisplayTime, &location);
	if (XR_FAILED(xrResult))
		return result;

	bool posValid = (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
	bool oriValid = (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
	result.Active = posValid && oriValid;
	result.PositionX = location.pose.position.x;
	result.PositionY = location.pose.position.y;
	result.PositionZ = location.pose.position.z;
	result.OrientationX = location.pose.orientation.x;
	result.OrientationY = location.pose.orientation.y;
	result.OrientationZ = location.pose.orientation.z;
	result.OrientationW = location.pose.orientation.w;
	return result;
}

static bool GetBool(const OpenXRFunctions& fn, XrSession session, XrAction action, XrPath subactionPath)
{
	XrActionStateGetInfo getInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
	getInfo.action = action;
	getInfo.subactionPath = subactionPath;
	XrActionStateBoolean state = { XR_TYPE_ACTION_STATE_BOOLEAN };
	fn.xrGetActionStateBoolean(session, &getInfo, &state);
	return state.isActive && state.currentState;
}

static float GetFloat(const OpenXRFunctions& fn, XrSession session, XrAction action, XrPath subactionPath)
{
	XrActionStateGetInfo getInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
	getInfo.action = action;
	getInfo.subactionPath = subactionPath;
	XrActionStateFloat state = { XR_TYPE_ACTION_STATE_FLOAT };
	fn.xrGetActionStateFloat(session, &getInfo, &state);
	return state.isActive ? state.currentState : 0.0f;
}

VRControllerState OpenXRDisplayWindow::GetControllerState(VRControllerHand hand)
{
	int index = HandIndex(hand);
	XrPath subactionPath = HandSubactionPath[index];
	VRControllerState result;

	if (ActionSet == XR_NULL_HANDLE)
		return result;

	result.TriggerValue = GetFloat(fn, Session, TriggerAction, subactionPath);
	result.GripValue = GetFloat(fn, Session, GripAction, subactionPath);
	result.ButtonA = GetBool(fn, Session, ButtonAAction, subactionPath);
	result.ButtonB = GetBool(fn, Session, ButtonBAction, subactionPath);
	result.ThumbstickClick = GetBool(fn, Session, ThumbstickClickAction, subactionPath);
	result.MenuButton = hand == VRControllerHand::Left ? GetBool(fn, Session, MenuAction, subactionPath) : false;

	XrActionStateGetInfo getInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
	getInfo.action = ThumbstickAction;
	getInfo.subactionPath = subactionPath;
	XrActionStateVector2f stick = { XR_TYPE_ACTION_STATE_VECTOR2F };
	fn.xrGetActionStateVector2f(Session, &getInfo, &stick);
	if (stick.isActive)
	{
		result.ThumbstickX = stick.currentState.x;
		result.ThumbstickY = stick.currentState.y;
	}

	return result;
}

void OpenXRDisplayWindow::TriggerHapticPulse(VRControllerHand hand, float amplitude, float durationSeconds)
{
	if (HapticAction == XR_NULL_HANDLE)
		return;

	XrHapticVibration vibration = { XR_TYPE_HAPTIC_VIBRATION };
	vibration.duration = (XrDuration)(durationSeconds * 1e9);
	vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
	vibration.amplitude = amplitude;

	XrHapticActionInfo actionInfo = { XR_TYPE_HAPTIC_ACTION_INFO };
	actionInfo.action = HapticAction;
	actionInfo.subactionPath = HandSubactionPath[HandIndex(hand)];
	fn.xrApplyHapticFeedback(Session, &actionInfo, (const XrHapticBaseHeader*)&vibration);
}

void OpenXRDisplayWindow::PollEvents()
{
	if (Instance == XR_NULL_HANDLE)
		return;

	XrEventDataBuffer event = { XR_TYPE_EVENT_DATA_BUFFER };
	while (fn.xrPollEvent(Instance, &event) == XR_SUCCESS)
	{
		if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
		{
			auto* stateEvent = (XrEventDataSessionStateChanged*)&event;
			SessionState = stateEvent->state;

			switch (SessionState)
			{
			case XR_SESSION_STATE_READY:
			{
				XrSessionBeginInfo beginInfo = { XR_TYPE_SESSION_BEGIN_INFO };
				beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				OXR(fn, Instance, fn.xrBeginSession(Session, &beginInfo));
				SessionRunning = true;

				// Request maximum CPU/GPU clocks (see openxr_vulkan_loader.h's
				// xrPerfSettingsSetPerformanceLevelEXT comment) - not OXR()-wrapped since the
				// function pointer is null (and this a no-op) on a runtime that doesn't support
				// XR_EXT_performance_settings.
				if (fn.xrPerfSettingsSetPerformanceLevelEXT)
				{
					fn.xrPerfSettingsSetPerformanceLevelEXT(Session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, XR_PERF_SETTINGS_LEVEL_BOOST_EXT);
					fn.xrPerfSettingsSetPerformanceLevelEXT(Session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, XR_PERF_SETTINGS_LEVEL_BOOST_EXT);
				}
				break;
			}
			case XR_SESSION_STATE_STOPPING:
				fn.xrEndSession(Session);
				SessionRunning = false;
				break;
			case XR_SESSION_STATE_EXITING:
			case XR_SESSION_STATE_LOSS_PENDING:
				SessionRunning = false;
				if (WindowHost)
					WindowHost->OnWindowClose();
				break;
			default:
				break;
			}
		}
		else if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING)
		{
			// The user recentered (or the runtime re-established tracking): the LOCAL space
			// origin is about to jump to the current head pose. The engine re-anchors its head
			// offset on the next frames (VRInput::Update via ConsumeRecenterEvent()).
			RecenterPending = true;
		}

		event.type = XR_TYPE_EVENT_DATA_BUFFER;
	}
}

bool OpenXRDisplayWindow::WaitFrame()
{
	ViewsValidThisFrame = false;
	ScreenRenderedThisFrame = false;

	if (!SessionRunning)
	{
		FrameStateValid = false;
		FrameBegun = false;
		return false;
	}

	SyncActions();

	FrameState = { XR_TYPE_FRAME_STATE };
	XrFrameWaitInfo waitInfo = { XR_TYPE_FRAME_WAIT_INFO };
	OXR(fn, Instance, fn.xrWaitFrame(Session, &waitInfo, &FrameState));

	XrFrameBeginInfo beginInfo = { XR_TYPE_FRAME_BEGIN_INFO };
	OXR(fn, Instance, fn.xrBeginFrame(Session, &beginInfo));
	FrameBegun = true;

	FrameStateValid = true;

	if (!FrameState.shouldRender)
	{
		// TEMP diagnostic - see the xrLocateViews-path log below for context.
		static int shouldRenderDiagCounter = 0;
		if ((++shouldRenderDiagCounter % 120) == 0)
			ALOGE("WaitFrame diag: shouldRender=false, sessionState=%d", (int)SessionState);
		return false;
	}

	XrViewLocateInfo locateInfo = { XR_TYPE_VIEW_LOCATE_INFO };
	locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locateInfo.displayTime = FrameState.predictedDisplayTime;
	locateInfo.space = StageSpace;

	XrViewState viewState = { XR_TYPE_VIEW_STATE };
	uint32_t viewCount = EyeCount;
	OXR(fn, Instance, fn.xrLocateViews(Session, &locateInfo, &viewState, EyeCount, &viewCount, Views.data()));

	bool posValid = (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
	bool oriValid = (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
	ViewsValidThisFrame = posValid && oriValid;

	// TEMP diagnostic (see Engine::RunVR()'s VRDiag comment, Engine.cpp) - WaitFrame()
	// returning false with no VRDiag output logged from RunVR() means the eye loop never
	// runs, which was otherwise invisible (no crash, just a silent, CPU-pegging spin as
	// Run()'s while(!quit) loop calls WaitFrame()->EndFrame() every iteration with nothing to
	// render). Logs why every ~120 calls so it doesn't flood but still shows up quickly.
	static int waitFrameDiagCounter = 0;
	if ((++waitFrameDiagCounter % 120) == 0)
	{
		ALOGE("WaitFrame diag: shouldRender=%d posValid=%d oriValid=%d viewStateFlags=0x%llx",
			FrameState.shouldRender, posValid, oriValid, (unsigned long long)viewState.viewStateFlags);
	}

	return ViewsValidThisFrame;
}

StereoEyeView OpenXRDisplayWindow::GetEyeView(int eye)
{
	const XrView& view = Views[eye];
	StereoEyeView result;
	result.PositionX = view.pose.position.x;
	result.PositionY = view.pose.position.y;
	result.PositionZ = view.pose.position.z;
	result.OrientationX = view.pose.orientation.x;
	result.OrientationY = view.pose.orientation.y;
	result.OrientationZ = view.pose.orientation.z;
	result.OrientationW = view.pose.orientation.w;
	result.FovAngleLeft = view.fov.angleLeft;
	result.FovAngleRight = view.fov.angleRight;
	result.FovAngleUp = view.fov.angleUp;
	result.FovAngleDown = view.fov.angleDown;
	return result;
}

int OpenXRDisplayWindow::AcquireEyeImage(int eye)
{
	CurrentRenderEye = eye; // see GetPixelWidth/GetPixelHeight's declaration comment

	XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	uint32_t index = 0;
	OXR(fn, Instance, fn.xrAcquireSwapchainImage(Swapchains[eye], &acquireInfo, &index));

	XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	waitInfo.timeout = XR_INFINITE_DURATION;
	OXR(fn, Instance, fn.xrWaitSwapchainImage(Swapchains[eye], &waitInfo));

	AcquiredImageIndex[eye] = (int)index;

	ProjectionViews[eye].pose = Views[eye].pose;
	ProjectionViews[eye].fov = Views[eye].fov;

	return (int)index;
}

VkImage OpenXRDisplayWindow::GetEyeImage(int eye, int imageIndex)
{
	return SwapchainImages[eye][imageIndex].image;
}

void OpenXRDisplayWindow::ReleaseEyeImage(int eye)
{
	XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	OXR(fn, Instance, fn.xrReleaseSwapchainImage(Swapchains[eye], &releaseInfo));
	AcquiredImageIndex[eye] = -1;
}

void OpenXRDisplayWindow::EndFrame()
{
	// No matching xrBeginFrame happened (WaitFrame() returned early because the session
	// isn't running - not visible, stopping, etc.) - xrEndFrame would fail with
	// XR_ERROR_SESSION_NOT_RUNNING (confirmed on real Quest 3 hardware). Nothing to submit.
	if (!FrameBegun)
		return;
	FrameBegun = false;

	XrFrameEndInfo endInfo = { XR_TYPE_FRAME_END_INFO };
	endInfo.displayTime = FrameState.predictedDisplayTime;
	endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

	// ViewsValidThisFrame being false means WaitFrame() returned false because xrLocateViews
	// reported an invalid pose (not because the session wasn't running) - shouldRender was
	// still true in that case, so ProjectionLayer would otherwise look submittable, but
	// AcquireEyeImage() (the only thing that refreshes ProjectionViews[].pose/fov from
	// Views[]) was never called this frame because the caller's eye-render loop never ran.
	// Submitting the resulting stale/uninitialized layer is what produced
	// XR_ERROR_LAYER_INVALID on real hardware.
	const XrCompositionLayerBaseHeader* layers[2] = {};
	uint32_t layerCount = 0;
	if (FrameStateValid && FrameState.shouldRender && ViewsValidThisFrame)
		layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&ProjectionLayer;
	// ScreenLayerActive is the caller's declaration of intent (Engine::RunVRMenuScreen(),
	// Engine.cpp) - ScreenRenderedThisFrame additionally guards against submitting a quad whose
	// image was never actually acquired/rendered/released this frame (mirrors
	// ViewsValidThisFrame's guard above for the same XR_ERROR_LAYER_INVALID reason).
	if (ScreenLayerActive && ScreenRenderedThisFrame)
		layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&ScreenQuadLayer;
	endInfo.layerCount = layerCount;
	endInfo.layers = layerCount > 0 ? layers : nullptr;

	OXR(fn, Instance, fn.xrEndFrame(Session, &endInfo));
}
