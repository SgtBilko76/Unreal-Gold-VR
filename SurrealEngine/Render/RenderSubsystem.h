#pragma once

#include "VisibleFrame.h"

class RenderDevice;
class UWindow;
class UFont;
class UMover;
class UViewportWindow;
class UWeapon;

class RenderSubsystem
{
public:
	RenderSubsystem(RenderDevice* renderdevice);

	void PreRenderWindows(UCanvas* canvas);
	void PostRenderWindows(UCanvas* canvas);
	void DrawWindowInfo(UFont* font, UWindow* window, int depth, float& curY);
	void DrawWindow(UWindow* window, float offsetX, float offsetY);
	void ResetWindowGC(UWindow* window, float offsetX, float offsetY);

	void DrawEditorViewport();
	void DrawVideoFrame(TextureInfo* frame, TextureInfo* background);

	void DrawGame(float levelTimeElapsed);
	void OnMapLoaded();

	// VR stereo rendering (see Engine::RunVR(), Engine.cpp): renders and presents one eye.
	// Unlike DrawGame(), this does NOT tick level lighting/AutoUV/Stats reset - call
	// BeginVRFrame(levelTimeElapsed) once per app-frame first, then DrawEyeVR() once per eye
	// (each with its own SetVREyeOverride() call beforehand), then EndVRFrame(). Mirrors
	// DrawGame()'s body (RenderSubsystem.cpp) split across these three calls so the
	// per-frame-once and per-eye parts don't get conflated.
	void BeginVRFrame(float levelTimeElapsed);
	void DrawEyeVR(int eye, int imageIndex, VkImage eyeImage, int width, int height);
	void EndVRFrame();

	// "Big screen" quad-layer path (Engine::RunVRMenuScreen(), Engine.cpp) - draws ONLY the 2D
	// menu/UI overlay (ResetCanvas/RenderOverlays/PostRender), no 3D world (DrawScene()) at
	// all, into the mono screen-layer swapchain image instead of a per-eye stereo one. Called
	// once per app-frame while the menu is open, not once per eye. Currently unused - see
	// RenderDevice.h's UnlockScreen comment for why (superseded by RenderMenuTexture/
	// SetMenuWorldQuad below).
	void DrawMenuScreen(int imageIndex, VkImage screenImage, int width, int height);

	// World-space "big screen" menu quad path (Engine::RunVRMenuScreen(), Engine.cpp) - same
	// 2D-overlay-only rendering as DrawMenuScreen above, but into RenderDevice's own owned
	// menu texture (RenderDevice.h's UnlockMenuTexture) instead of an OpenXR swapchain image.
	// Called once per app-frame, before either eye renders.
	void RenderMenuTexture(int width, int height);

	// UI pixel-doubling factor used for the VR menu texture (ResetCanvas) - the cursor fed to
	// UWindow (Engine::RunVRMenuScreen) and the pointer drawn over it must use the same value.
	int MenuUIScale() const;

	// Arms (or disarms) DrawEyeVR()'s per-eye submission of the menu panel geometry sampling
	// that texture - call once per app-frame (not per eye) with this frame's world-space strips
	// (the curved panel is a fan of vertical quads, each showing a texture slice; see
	// RenderDevice.h's DrawMenuWorldQuad for corner winding), before DrawEyeVR().
	struct MenuStrip
	{
		vec3 Corners[4];
		vec2 UVMin;
		vec2 UVMax;
	};
	void SetMenuWorldStrips(bool active, const MenuStrip* strips, int count);

	// In-engine on-screen keyboard drawn on the menu panel and operated with the controller ray
	// (the headset system keyboard cannot surface in an immersive OpenXR app). Public so Engine
	// (RunVRMenuScreen) can toggle it, feed the cursor and route clicks to keys.
	bool vrKeyboardActive = false;
	bool vrKeyboardShift = false;
	float vrKeyboardCursorX = -1000.0f; // last frame's cursor in logical canvas units (for hover)
	float vrKeyboardCursorY = -1000.0f;
	struct VRKeyHit { bool hit = false; char ch = 0; bool backspace = false; bool enter = false; bool shift = false; };
	void DrawVRKeyboard(float cursorX, float cursorY);
	VRKeyHit VRKeyboardHitTest(float cursorX, float cursorY) const;

	// VR sniper scope (Engine::RunVRScope(), Engine.cpp): renders the 3D world from the gun's
	// viewpoint with the game's (zoomed) FovAngle into the shared menu texture - a
	// width x height square at the texture's top-left - with a reticle in the middle. Called
	// once per app-frame before the eyes, only while zoomed and the menu panel is not showing
	// (they share the texture).
	void RenderScopeTexture(int width, int height, const mat4& worldToView, const mat4& projection, vec3 cameraLocation, const Coords& cameraRotation);

	// Arms (or disarms) DrawEyeVR()'s per-eye draw of the scope screen quad - drawn after the
	// held weapon so it sits on top of the gun. Same corner winding as the menu strips.
	void SetScopeWorldQuad(bool active, const vec3 corners[4], vec2 uvMax);

	// Sets the projection/view transform DrawEyeVR() uses for its next call - see
	// SurrealEngine/VR/VRCamera.h for how callers build these from an OpenXR eye pose.
	void SetVREyeOverride(int eye, const mat4& worldToView, const mat4& projection, vec3 eyeLocation, const Coords& eyeRotation);
	void ClearVREyeOverride();

	void DrawActor(UActor* actor, bool WireFrame, bool ClearZ);
	void DrawClippedActor(UActor* actor, bool WireFrame, int X, int Y, int XB, int YB, bool ClearZ);
	void DrawTile(UTexture* Tex, float x, float y, float XL, float YL, float U, float V, float UL, float VL, float Z, vec4 color, vec4 fog, uint32_t flags);
	void DrawTileClipped(UTexture* Tex, float orgX, float orgY, float curX, float curY, float XL, float YL, float U, float V, float UL, float VL, float Z, vec4 color, vec4 fog, uint32_t flags, float clipX, float clipY);
	void DrawText(UFont* font, vec4 color, float orgX, float orgY, float& curX, float& curY, float& curXL, float& curYL, bool newlineAtEnd, const std::string& text, uint32_t polyflags, bool center, float spaceX = 0.0f, float spaceY = 0.0f, float clipX = 100000.0f, float clipY = 100000.0f, bool noDraw = false);
	void DrawTextClipped(UFont* font, vec4 color, float orgX, float orgY, float curX, float curY, const std::string& text, uint32_t polyflags, bool checkHotKey, float clipX, float clipY, bool center);
	vec2 GetTextSize(UFont* font, const std::string& text, float spaceX = 0.0f, float spaceY = 0.0f);

	void DrawTile(TextureInfo& Info, float X, float Y, float XL, float YL, float U, float V, float UL, float VL, float Z, vec4 Color, vec4 Fog, uint32_t PolyFlags);
	void Draw2DLine(vec4 Color, uint32_t LineFlags, vec3 P1, vec3 P2, bool useUIScale);
	void Draw3DLine(vec4 Color, uint32_t LineFlags, vec3 P1, vec3 P2);
	void UpdateTexture(UTexture* tex);

	void DrawViewport(UViewportWindow* viewport);

	bool ShowTimedemoStats = false;
	bool ShowRenderStats = false;
	bool ShowCollisionDebug = false;

	int TextureFrameCounter = 0;
	int FrameCounter = 0;

	vec3* GetTempVertexBuffer(size_t count)
	{
		if (VertexBuffer.size() < count)
			VertexBuffer.resize(count);
		return VertexBuffer.data();
	}

	GouraudVertex* GetTempGouraudVertexBuffer(size_t count)
	{
		if (GouraudVertexBuffer.size() < count)
			GouraudVertexBuffer.resize(count);
		return GouraudVertexBuffer.data();
	}

	void UpdateTextureInfo(TextureInfo& info, BspSurface& surface, UTexture* texture, float ZoneUPanSpeed, float ZoneVPanSpeed);
	void UpdateTextureInfo(TextureInfo& info, const Poly& poly, UTexture* texture, float ZoneUPanSpeed, float ZoneVPanSpeed);
	void UpdateTextureInfo(TextureInfo& info, UTexture* texture);

	void SetTile3DOffset(bool enabled, std::optional<vec3> offset, std::optional<Rotator> rotOffset, std::optional<bool> bFlatZ, std::optional<float> Scale, std::optional<bool> bWorldOffset);

	RenderDevice* Device = nullptr;

	struct
	{
		Array<UTexture*> textures;
		UTexture* envmap = nullptr;
	} Mesh;

	struct
	{
		int Frames = 0;
		int Surfaces = 0;
		int Actors = 0;
		int LightmapsUpdated = 0;
	} Stats;

	VisibleFrame MainFrame;

private:
	void DrawScene();

	void ResetCanvas();
	void PreRender();
	void RenderOverlays();
	void PostRender();
	void PostRenderFlash();
	void DrawTimedemoStats();
	void DrawCollisionDebug();
	void DrawTile(TextureInfo& texinfo, const Rectf& dest, const Rectf& src, const Rectf& clipBox, float Z, vec4 color, vec4 fog, uint32_t flags);

	static Array<std::string> FindTextBlocks(const std::string& text);
	void DrawTextBlockRange(float x, float y, const Array<std::string>& textBlocks, size_t start, size_t end, UFont* font, vec4 color, uint32_t polyflags, float spaceX);

	float LevelTimeElapsed = 0.0f;
	float AutoUV = 0.0f;

	struct
	{
		int uiscale = 1;
		int fps = 0;
		int framesDrawn = 0;
		uint64_t startFPSTime = 0;
		SceneNode Frame;
	} Canvas;

	// Unreal 227
	struct
	{
		vec3 Offset = vec3();
		Rotator RotOffset = Rotator();
		float Scale = 1.0f;
		bool Enabled = false;
		bool FlatZ = false;
		bool bWorldOffset = false; // 227k - Offset and RotOffset are absolute world coordinates
	} Tile3DOffset;

	Array<vec3> VertexBuffer;
	Array<GouraudVertex> GouraudVertexBuffer;

	// Traces the right controller's aim ray against the level once per eye (DrawEyeVR()) and
	// records where it lands plus which texture the HUD is currently using as its crosshair -
	// DrawTile(UTexture*...) (RenderCanvas.cpp) then redirects the HUD's own crosshair draw to
	// a world-space billboard at that point instead of screen center.
	void UpdateVRAimPoint();

public:
	// Result of UpdateVRAimPoint(): where the right controller is pointing this frame. Also
	// read by Engine::Run()'s fire override so shots (which UT spawns at the eye, not the
	// hand) are aimed from the head at exactly this point and land on the crosshair.
	struct
	{
		bool Valid = false;
		vec3 HitPoint = vec3(0.0f);
		vec3 Dir = vec3(1.0f, 0.0f, 0.0f);
		float Dist = 0.0f;
		UTexture* CrosshairTexture = nullptr;
		// Where the held weapon's muzzle is (a little ahead of the right hand along the aim)
		// and the texture UT's TournamentWeapon.DrawMuzzleFlash draws as a 2D icon - that
		// draw is redirected to a billboard at MuzzlePoint (RenderCanvas.cpp's DrawTile).
		vec3 MuzzlePoint = vec3(0.0f);
		// Barrel tip of the held weapon as last drawn, relative to the right hand (RenderCanvas.cpp
		// DrawActor) - lets the next frame's MuzzlePoint follow the hand.
		vec3 MuzzleLocal = vec3(0.0f);
		UActor* MuzzleOwner = nullptr;
		// The held weapon, when it has an MFTexture property. Read at DRAW time (DrawTile),
		// not once per frame: UT weapons pick a new MFTexture from MuzzleFlashVariations each
		// shot, inside RenderOverlays itself, so a value cached beforehand never matched.
		UWeapon* Weapon = nullptr;
		bool WeaponHasMuzzleFlash = false;
	} VRAim;

	UTexture* CurrentMuzzleFlashTexture() const;

private:

	struct
	{
		bool Active = false;
		int Eye = 0; // 0 = left, 1 = right
		mat4 WorldToView;
		mat4 Projection;
		vec3 EyeLocation;
		Coords EyeRotation;
	} VREyeOverride;

	struct
	{
		bool Active = false;
		std::vector<MenuStrip> Strips;
	} MenuWorldQuad;

	bool RenderingMenuTexture = false; // true inside RenderMenuTexture() - see ResetCanvas()'s uiscale override
	bool RenderingScopeTexture = false; // true inside RenderScopeTexture() - ResetCanvas() skips the per-eye HUD shift

	struct
	{
		bool Active = false;
		vec3 Corners[4];
		vec2 UVMax = vec2(1.0f, 1.0f);
	} ScopeQuad;
};
