#pragma once

#include "window/window.h"
#include "openxr_vulkan_loader.h"
#include <vector>
#include <array>

// The one and only DisplayWindow for the OpenXR backend - see openxr_display_backend.h for
// why there is exactly one, and window.h's DisplayWindow class for the stereo-VR virtuals
// this class actually implements (IsStereoDisplay/GetEyeView/AcquireEyeImage/etc). Everything
// window-shaped (SetWindowTitle, Show, cursor locking, ...) is a no-op: there is no OS window
// on a standalone headset, just the XR session.
//
// Owns the XrInstance/XrSession/per-eye XrSwapchains. Uses XR_KHR_vulkan_enable (not
// vulkan_enable2): the "2" variant has the runtime create the VkInstance/VkDevice itself,
// which doesn't fit SurrealGPU's own VulkanInstanceBuilder/VulkanDeviceBuilder. The original
// extension is query-only - xrGetVulkanInstanceExtensionsKHR/xrGetVulkanDeviceExtensionsKHR
// just return extension name strings, and xrGetVulkanGraphicsDeviceKHR returns which
// VkPhysicalDevice to use - so the app (SurrealEngine's VulkanRenderDevice) keeps building
// its own VkInstance/VkDevice exactly as it does today, just with these extra
// runtime-required extensions folded in and the physical device runtime-selected instead of
// scored by SurrealGPU's own heuristics.
class OpenXRDisplayWindow : public DisplayWindow
{
public:
	OpenXRDisplayWindow(DisplayWindowHost* windowHost, WidgetType type, RenderAPI renderAPI);
	~OpenXRDisplayWindow();

	// DisplayWindow - window-shaped no-ops (see class comment)
	void SetWindowTitle(const std::string& text) override {}
	void SetWindowIcon(const std::vector<std::shared_ptr<Image>>& images) override {}
	void Show() override {}
	void ShowFullscreen() override {}
	void ShowMaximized() override {}
	void ShowMinimized() override {}
	void ShowNormal() override {}
	bool IsWindowFullscreen() override { return true; }
	void Hide() override {}
	void Activate() override {}
	void ShowCursor(bool enable) override {}
	void LockKeyboard() override {}
	void UnlockKeyboard() override {}
	void LockCursor() override {}
	void UnlockCursor() override {}
	void CaptureMouse() override {}
	void ReleaseMouseCapture() override {}
	void Update() override {}
	bool GetKeyState(InputKey key) override { return false; }

	void SetClientFrame(const Rect& box) override {}
	Rect GetClientFrame() const override;

	void SetCursor(StandardCursor cursor, std::shared_ptr<CustomCursor> custom) override {}

	Size GetClientSize() const override;

	// Reports whichever eye AcquireEyeImage() most recently acquired (see CurrentRenderEye
	// below) rather than always eye 0 - VulkanRenderDevice::Lock()/Unlock() (VulkanRenderDevice.cpp)
	// size the offscreen scene render target from these every frame via Viewport->
	// GetNativePixelWidth/Height(), and that target is shared across both eyes' draws within
	// the same app-frame, so if this always reported eye 0's size, eye 1 would render its 3D
	// content at the wrong aspect/resolution into a buffer sized for eye 0 and then get
	// stretched to fit eye 1's actual swapchain image on present - visible as distortion
	// between the two eyes (confirmed on real hardware).
	int GetPixelWidth() const override;
	int GetPixelHeight() const override;
	double GetDpiScale() const override { return 1.0; }

	Point MapFromGlobal(const Point& pos) const override { return pos; }
	Point MapToGlobal(const Point& pos) const override { return pos; }

	void SetBorderColor(uint32_t bgra8) override {}
	void SetCaptionColor(uint32_t bgra8) override {}
	void SetCaptionTextColor(uint32_t bgra8) override {}

	void PresentBitmap(int width, int height, const uint32_t* pixels) override {}

	std::string GetClipboardText() override { return {}; }
	void SetClipboardText(const std::string& text) override {}

	void* GetNativeHandle() override { return nullptr; }

	std::vector<std::string> GetVulkanInstanceExtensions() override { return {}; }
	VkSurfaceKHR CreateVulkanSurface(VkInstance instance) override { return VK_NULL_HANDLE; }

	// DisplayWindow - stereo VR support (the actual point of this backend)
	bool IsStereoDisplay() override { return true; }
	int GetEyeCount() override { return 2; }
	void GetEyeImageSize(int eye, int* width, int* height) override;

	std::vector<std::string> GetVulkanInstanceRequirements() override;
	std::vector<std::string> GetVulkanDeviceRequirements() override;
	VkPhysicalDevice SelectVulkanPhysicalDevice(VkInstance instance) override;
	void CreateVulkanSession(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex) override;

	bool WaitFrame() override;
	StereoEyeView GetEyeView(int eye) override;
	int AcquireEyeImage(int eye) override;
	VkImage GetEyeImage(int eye, int imageIndex) override;
	void ReleaseEyeImage(int eye) override;
	void EndFrame() override;

	void CreateScreenLayerSwapchain(int width, int height) override;
	void GetScreenLayerImageSize(int* width, int* height) override { *width = ScreenWidth; *height = ScreenHeight; }
	int AcquireScreenLayerImage() override;
	VkImage GetScreenLayerImage(int imageIndex) override;
	void ReleaseScreenLayerImage() override;
	void SetScreenLayerPose(float positionX, float positionY, float positionZ, float orientationX, float orientationY, float orientationZ, float orientationW, float widthMeters, float heightMeters) override;
	void SetScreenLayerActive(bool active) override { ScreenLayerActive = active; }
	bool ConsumeRecenterEvent() override { bool pending = RecenterPending; RecenterPending = false; return pending; }

	MotionControllerPose GetHeadPose() override;
	MotionControllerPose GetControllerPose(VRControllerHand hand) override;
	VRControllerState GetControllerState(VRControllerHand hand) override;
	void TriggerHapticPulse(VRControllerHand hand, float amplitude, float durationSeconds) override;

	// Called by OpenXRDisplayBackend::ProcessEvents()
	void PollEvents();

	// True once xrCreateInstance/xrGetSystem have succeeded and the session state machine
	// has reached XR_SESSION_STATE_READY or later - i.e. it's safe to call CreateVulkanSession.
	bool IsSystemReady() const { return SystemId != XR_NULL_SYSTEM_ID; }

	DisplayWindowHost* WindowHost = nullptr;

private:
	void CreateInstanceAndSystem();
	void EnumerateViews();
	void CreateSwapchain(int eye);
	void SetSwapchainFormat(XrSwapchainCreateInfo& createInfo);
	void DestroySwapchains();
	void CreateActions();
	void SyncActions();

	OpenXRFunctions fn;

	XrInstance Instance = XR_NULL_HANDLE;
	bool HasPicoControllerExt = false; // XR_BD_controller_interaction (Pico 4 / Neo 3 profiles)
	XrSystemId SystemId = XR_NULL_SYSTEM_ID;
	XrSession Session = XR_NULL_HANDLE;
	XrSpace HeadSpace = XR_NULL_HANDLE;
	XrSpace StageSpace = XR_NULL_HANDLE;

	bool SessionRunning = false;
	XrSessionState SessionState = XR_SESSION_STATE_UNKNOWN;

	static constexpr int EyeCount = 2;
	std::array<XrViewConfigurationView, EyeCount> ViewConfigs = {};
	std::array<XrSwapchain, EyeCount> Swapchains = { XR_NULL_HANDLE, XR_NULL_HANDLE };
	std::array<std::vector<XrSwapchainImageVulkanKHR>, EyeCount> SwapchainImages;
	std::array<int, EyeCount> AcquiredImageIndex = { -1, -1 };
	int CurrentRenderEye = 0; // set by AcquireEyeImage(), read by GetPixelWidth/GetPixelHeight - see their declarations above

	std::array<XrView, EyeCount> Views = {};
	XrFrameState FrameState = {};
	bool FrameStateValid = false;
	bool FrameBegun = false; // true only once xrBeginFrame has actually been called for the in-flight frame - EndFrame() must not call xrEndFrame without a matching xrBeginFrame (XR_ERROR_SESSION_NOT_RUNNING, confirmed on real Quest 3 hardware when WaitFrame() short-circuits on !SessionRunning)
	bool ViewsValidThisFrame = false; // true only once xrLocateViews reported valid position+orientation for BOTH eyes AND the caller actually rendered/AcquireEyeImage'd them - if WaitFrame() returns false because the pose was invalid, ProjectionViews[].pose/fov were never refreshed by AcquireEyeImage() this frame, so EndFrame() must not submit them (XR_ERROR_LAYER_INVALID, confirmed on real Quest 3 hardware - the runtime rejects a projection layer built from a stale/never-set pose)

	XrCompositionLayerProjectionView ProjectionViews[EyeCount] = {};
	XrCompositionLayerProjection ProjectionLayer = {};

	// Mono "big screen" quad layer for 2D UI (menu) - see window.h's CreateScreenLayerSwapchain
	// comment. Separate from the stereo eye swapchains above: one image, rendered once per
	// app-frame, not once per eye.
	XrSwapchain ScreenSwapchain = XR_NULL_HANDLE;
	std::vector<XrSwapchainImageVulkanKHR> ScreenSwapchainImages;
	int ScreenAcquiredImageIndex = -1;
	int ScreenWidth = 0;
	int ScreenHeight = 0;
	bool ScreenLayerActive = false;
	bool RecenterPending = false; // set by PollEvents on XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING
	bool ScreenRenderedThisFrame = false; // reset false at the top of WaitFrame(), set true by ReleaseScreenLayerImage() - see EndFrame()'s comment
	bool RenderingScreenLayer = false; // true between AcquireScreenLayerImage()/ReleaseScreenLayerImage() - see GetPixelWidth/GetPixelHeight()
	XrCompositionLayerQuad ScreenQuadLayer = {};

	// Input action set - one action per logical control, bound to both hands via
	// subactionPath (see CreateActions()/SyncActions() in the .cpp, and QuakeQuest's
	// OpenXrInput.c for the interaction-profile path strings this was modeled on).
	XrActionSet ActionSet = XR_NULL_HANDLE;
	XrPath HandSubactionPath[2] = { XR_NULL_PATH, XR_NULL_PATH }; // [Left, Right]
	XrAction PoseAction = XR_NULL_HANDLE;
	XrAction TriggerAction = XR_NULL_HANDLE;
	XrAction GripAction = XR_NULL_HANDLE;
	XrAction ThumbstickAction = XR_NULL_HANDLE;
	XrAction ThumbstickClickAction = XR_NULL_HANDLE;
	XrAction ButtonAAction = XR_NULL_HANDLE; // right hand: A, left hand: X
	XrAction ButtonBAction = XR_NULL_HANDLE; // right hand: B, left hand: Y
	XrAction MenuAction = XR_NULL_HANDLE; // left hand only on Touch
	XrAction HapticAction = XR_NULL_HANDLE;
	XrSpace HandSpace[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };

	int HandIndex(VRControllerHand hand) const { return hand == VRControllerHand::Left ? 0 : 1; }
};
