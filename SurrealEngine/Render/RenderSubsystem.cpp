
#include "Precomp.h"
#include "RenderSubsystem.h"
#include "RenderDevice/RenderDevice.h"
#include "GameWindow.h"
#include "Packages/Engine/USurrealClient.h"
#include "Packages/Engine/UConsole.h"
#include "Packages/Engine/UViewport.h"
#include "Packages/Engine/Actors/Pawn/UPlayerPawn.h"
#include "Packages/Engine/Resources/UPalette.h"
#include "Packages/Engine/Resources/Level/UModel.h"
#include "Packages/Engine/Resources/Level/UPolys.h"
#include "VM/ScriptCall.h"
#include "Engine.h"
#include "Package/PackageManager.h"
#include "VR/VRInput.h"
#include "VR/VRCamera.h"
#include "Collision/TopLevel/CollisionHit.h"
#include "Packages/Engine/Actors/UHUD.h"
#include "Packages/Engine/Actors/Inventory/UWeapon.h"
#include "Packages/Core/Properties/UProperty.h"

RenderSubsystem::RenderSubsystem(RenderDevice* renderdevice) : Device(renderdevice)
{
}

void RenderSubsystem::SetVREyeOverride(int eye, const mat4& worldToView, const mat4& projection, vec3 eyeLocation, const Coords& eyeRotation)
{
	VREyeOverride.Active = true;
	VREyeOverride.Eye = eye;
	VREyeOverride.WorldToView = worldToView;
	VREyeOverride.Projection = projection;
	VREyeOverride.EyeLocation = eyeLocation;
	VREyeOverride.EyeRotation = eyeRotation;
}

void RenderSubsystem::ClearVREyeOverride()
{
	VREyeOverride.Active = false;
}

void RenderSubsystem::BeginVRFrame(float levelTimeElapsed)
{
	LevelTimeElapsed = levelTimeElapsed;
	AutoUV += levelTimeElapsed * 64.0f;
	engine->Level->Light.Tick(levelTimeElapsed);

	// Moved out of DrawScene() (see its comment) - must run exactly once per app-frame, not
	// once per eye. Light.BeginFrame() rebuilds the whole light tree and advances
	// flicker/pulse light timing; TextureFrameCounter gates animated-texture updates in
	// RenderSubsystem::UpdateTexture(). Both would otherwise run twice per app-frame in VR
	// (DrawScene() runs once per eye), which for BeginFrame() means real risk of the two eyes'
	// lighting diverging within the same frame - the actual cause of a real-hardware "ghost
	// offset sideways" report that persisted even after camera/stereo math was verified
	// correct via a forced-identical-eyes diagnostic build.
	engine->Level->Light.BeginFrame();
	TextureFrameCounter++;

	Stats.Frames = 0;
	Stats.Surfaces = 0;
	Stats.Actors = 0;
	Stats.LightmapsUpdated = 0;
}

void RenderSubsystem::DrawEyeVR(int eye, int imageIndex, VkImage eyeImage, int width, int height)
{
	vec3 flashScale = 0.5f;
	vec3 flashFog = vec3(0.0f, 0.0f, 0.0f);

	UPlayerPawn* player = UObject::TryCast<UPlayerPawn>(engine->CameraActor);
	if (player)
	{
		flashScale = player->FlashScale();
		flashFog = player->FlashFog();
	}

	Device->Brightness = engine->client->RenderBrightness();
	Device->Lock(vec4(flashScale, 1.0f), vec4(flashFog, 1.0f), vec4(0.0f), nullptr, nullptr);

	ResetCanvas();
	PreRender();

	bool drawWorld = engine->LaunchInfo.ue1Version <= 219 || engine->console->bNoDrawWorld() == false;
	if (drawWorld)
	{
		DrawScene();
	}
	else if (MenuWorldQuad.Active && VREyeOverride.Active)
	{
		// World suppressed (UT's console raises bNoDrawWorld while its menu is up) so
		// DrawScene() didn't run and MainFrame.Frame holds a stale camera - build this eye's
		// frame without the BSP traversal so the panel below still gets the right transform.
		// Without this the panel was skipped along with the world and, with the per-eye 2D
		// overlay also suppressed, the whole view went black the moment Menu was pressed
		// (real Quest 3 hardware).
		MainFrame.SetupSceneFrame(VREyeOverride.WorldToView, &VREyeOverride.Projection);
	}

	if (MenuWorldQuad.Active)
	{
		// Submitted while MainFrame.Frame holds this eye's real camera transform (DrawScene()
		// leaves it set, though MainFrame.DrawCoronas() pointed the device at its own 2D
		// frame2d in between - reset explicitly rather than assume). ClearZ first so the panel
		// always draws on top of any level geometry closer than its 2.5m - it's a UI element,
		// not something a nearby wall should hide.
		Device->SetSceneNode(&MainFrame.Frame);
		Device->ClearZ();
		for (const MenuStrip& strip : MenuWorldQuad.Strips)
			Device->DrawMenuWorldQuad(&MainFrame.Frame, strip.Corners, strip.UVMin, strip.UVMax);
	}

	if (drawWorld)
	{
		// While the world-space menu panel is up, the 2D UI must NOT also be drawn into the
		// eye as a screen-space overlay: the game's menu system (UT's UWindow root, Unreal's
		// console menus) renders itself from the HUD/console PostRender events, exactly like the
		// HUD - so with these still running per eye, the menu appeared as a head-locked overlay
		// right in front of the face, completely hiding the panel behind it. Confirmed on real
		// Quest 3 hardware as the actual cause of "the menu is mounted to my head and too
		// near" - through BOTH the old OpenXR quad-layer version and this one, since neither
		// ever removed this per-eye draw. RenderMenuTexture() already runs the same events once
		// per frame into the panel's texture, so nothing is lost by skipping them here.
		if (!MenuWorldQuad.Active)
		{
			UpdateVRAimPoint();
			RenderOverlays();
			if (ScopeQuad.Active)
			{
				// After the held weapon (RenderOverlays) so the scope screen sits on top of it.
				Device->SetSceneNode(&MainFrame.Frame);
				Device->ClearZ();
				Device->DrawMenuWorldQuad(&MainFrame.Frame, ScopeQuad.Corners, vec2(0.0f), ScopeQuad.UVMax);
			}
		}
		if (engine->LaunchInfo.IsDeusEx())
			PostRenderFlash();
		Device->EndFlash();
	}

	if (!MenuWorldQuad.Active)
		PostRender();

	Device->UnlockVR(eye, imageIndex, eyeImage, width, height);
}

void RenderSubsystem::UpdateVRAimPoint()
{
	VRAim.Valid = false;
	VRAim.CrosshairTexture = nullptr;

	VRInput* vr = engine->vrInput.get();
	UPlayerPawn* pawn = engine->viewport->Actor();
	if (!vr || !vr->RightControllerActive || !pawn || !engine->Level)
		return;

	// Same hand position/aim direction the held weapon (DrawActor, RenderCanvas.cpp) and
	// weapon fire (Engine::Run's aim override) use, so the crosshair marks where shots go.
	vec3 origin = VRCamera::StageToWorldPosition(vr->RightControllerPosition, engine->CameraLocation, vr->BodyYawRadians, VRCamera::UnitsPerMeter, vr->HeadAnchorPosition);
	vec3 dir = vr->GetAimForwardWorld();

	const float maxDist = 20000.0f;
	CollisionHitList hits = engine->Level->Collision.Trace(origin, origin + dir * maxDist, 0.0f, 0.0f, true, true, false);
	float fraction = 1.0f;
	for (const CollisionHit& hit : hits)
	{
		if (hit.Actor == pawn) // the ray starts at the hand, inside/next to our own cylinder
			continue;
		if (hit.Fraction < fraction)
			fraction = hit.Fraction;
	}
	VRAim.Dist = maxDist * fraction;
	VRAim.Dir = dir;
	VRAim.HitPoint = origin + dir * VRAim.Dist;
	VRAim.Valid = true;

	// Muzzle: the held weapon's barrel tip as DrawActor last measured it (rotated with the aim,
	// moved with the hand); ~30cm ahead of the hand until that weapon has been drawn once. The
	// flash texture comes from the held weapon's MFTexture (TournamentWeapon), when it has one.
	VRAim.Weapon = pawn->Weapon();
	if (VRAim.MuzzleOwner && VRAim.MuzzleOwner == VRAim.Weapon)
		VRAim.MuzzlePoint = origin + VRAim.MuzzleLocal;
	else
		VRAim.MuzzlePoint = origin + dir * (0.30f * VRCamera::UnitsPerMeter);
	VRAim.WeaponHasMuzzleFlash = VRAim.Weapon && VRAim.Weapon->HasProperty("MFTexture");

	// Which texture the HUD will draw as its crosshair this frame - UT's ChallengeHUD keeps a
	// static array CrosshairTextures[] indexed by the player's configured Crosshair choice.
	// Games without those properties just keep their screen-center crosshair.
	UHUD* hud = pawn->myHUD();
	if (hud && hud->HasProperty("Crosshair") && hud->HasProperty("CrosshairTextures"))
	{
		UProperty* prop = hud->GetMemberProperty("CrosshairTextures");
		int index = (int)hud->GetInt("Crosshair");
		if (prop && index >= 0 && index < prop->ArrayDimension)
		{
			UObject* tex = *(UObject**)((uint8_t*)hud->GetProperty(prop) + index * prop->ElementPitch());
			VRAim.CrosshairTexture = UObject::TryCast<UTexture>(tex);
		}
	}
	else if (hud && hud->HasProperty("Crosshair") && engine->packages->IsUnreal1())
	{
		// Unreal (Gold): UnrealShare.UnrealHUD / UPak.UPakHUD.DrawCrossHair has no texture array -
		// it hard-codes the Crosshair choice onto UnrealShare's Crosshair1..5 textures, with
		// choice 5 mapping to Crosshair7 (there is no Crosshair6 in the script). Mirror that
		// table so the same DrawIcon redirect (RenderCanvas.cpp) finds the crosshair.
		static const char* const unrealCrosshairNames[] = { "Crosshair1", "Crosshair2", "Crosshair3", "Crosshair4", "Crosshair5", "Crosshair7" };
		int index = (int)hud->GetInt("Crosshair");
		if (index >= 0 && index < 6)
		{
			Package* pkg = engine->packages->GetPackage("UnrealShare");
			if (pkg)
				VRAim.CrosshairTexture = UObject::TryCast<UTexture>(pkg->GetUObject("Texture", unrealCrosshairNames[index]));
		}
	}
}

UTexture* RenderSubsystem::CurrentMuzzleFlashTexture() const
{
	if (!VRAim.Valid || !VRAim.WeaponHasMuzzleFlash)
		return nullptr;
	return UObject::TryCast<UTexture>(VRAim.Weapon->GetUObject("MFTexture"));
}

void RenderSubsystem::DrawMenuScreen(int imageIndex, VkImage screenImage, int width, int height)
{
	Device->Brightness = engine->client->RenderBrightness();
	Device->Lock(vec4(0.5f, 0.5f, 0.5f, 1.0f), vec4(0.0f), vec4(0.0f), nullptr, nullptr);

	// No DrawScene() here - the screen layer only ever shows 2D UI (the menu), not the 3D
	// world (that keeps rendering normally in the stereo eyes underneath/around it - see
	// Engine::RunVRMenuScreen()'s comment for why both render simultaneously).
	ResetCanvas();
	PreRender();
	PostRender();

	Device->UnlockScreen(imageIndex, screenImage, width, height);
}

int RenderSubsystem::MenuUIScale() const
{
	// Pixel-double the menu (800x600 logical onto the ~1600px panel texture) for both games. At
	// 1 texel = 1 logical pixel the menu fonts render at their native pixel size, which on the
	// arm's-length VR panel is too small to read - reported for UT as well as Unreal Gold. The
	// controller-ray cursor is scaled by this same factor (see RunVRMenuScreen), so clicks stay
	// aligned with what is drawn.
	return 2;
}

void RenderSubsystem::RenderMenuTexture(int width, int height)
{
	Device->Brightness = engine->client->RenderBrightness();
	Device->Lock(vec4(0.5f, 0.5f, 0.5f, 1.0f), vec4(0.0f), vec4(0.0f), nullptr, nullptr);

	// No DrawScene() here either - see DrawMenuScreen's identical comment above.
	RenderingMenuTexture = true;
	ResetCanvas();
	PreRender();
	PostRender();

	// On-screen keyboard (VR) over the lower part of the menu, when toggled on.
	if (vrKeyboardActive)
		DrawVRKeyboard(vrKeyboardCursorX, vrKeyboardCursorY);

	// Cursor. With bWindowsMouseAvailable set (Engine::Run - required so UWindow reads the
	// absolute controller-ray position), UT's UWindow system assumes the OS draws the mouse
	// cursor and draws none of its own; on the Quest nobody else will, so paint a simple
	// crosshair-style pointer at the current position (real Quest 3: "pointer is invisible").
	{
		Device->SetSceneNode(&Canvas.Frame);
		float mx = engine->viewport->WindowsMouseX() * Canvas.uiscale; // WindowsMouseX/Y are logical canvas units (see MenuUIScale), Canvas.Frame is texture pixels
		float my = engine->viewport->WindowsMouseY() * Canvas.uiscale;
		float arm = 14.0f * Canvas.uiscale;
		float gap = 4.0f * Canvas.uiscale;
		vec4 black(0.0f, 0.0f, 0.0f, 1.0f);
		vec4 white(1.0f, 1.0f, 1.0f, 1.0f);
		// Black underlay one pixel wider on each side, then the white cross, so it stays
		// visible over both light and dark menu backgrounds.
		for (int pass = 0; pass < 2; pass++)
		{
			vec4 color = pass == 0 ? black : white;
			float w = pass == 0 ? 2.0f : 1.0f; // half-thickness in pixels
			for (float o = -w; o <= w; o += 1.0f)
			{
				Device->Draw2DLine(&Canvas.Frame, color, LINE_None, vec3(mx - arm, my + o, 1.0f), vec3(mx - gap, my + o, 1.0f));
				Device->Draw2DLine(&Canvas.Frame, color, LINE_None, vec3(mx + gap, my + o, 1.0f), vec3(mx + arm, my + o, 1.0f));
				Device->Draw2DLine(&Canvas.Frame, color, LINE_None, vec3(mx + o, my - arm, 1.0f), vec3(mx + o, my - gap, 1.0f));
				Device->Draw2DLine(&Canvas.Frame, color, LINE_None, vec3(mx + o, my + gap, 1.0f), vec3(mx + o, my + arm, 1.0f));
			}
		}
	}
	RenderingMenuTexture = false;

	Device->UnlockMenuTexture(width, height);
}

void RenderSubsystem::SetMenuWorldStrips(bool active, const MenuStrip* strips, int count)
{
	MenuWorldQuad.Active = active;
	MenuWorldQuad.Strips.clear();
	if (active)
		MenuWorldQuad.Strips.assign(strips, strips + count);
}

void RenderSubsystem::RenderScopeTexture(int width, int height, const mat4& worldToView, const mat4& projection, vec3 cameraLocation, const Coords& cameraRotation)
{
	if (!engine->Level)
		return;

	// The scope camera takes the place of an eye for this render (DrawScene reads
	// VREyeOverride); Engine::RunVR sets the real eyes' overrides afterwards.
	SetVREyeOverride(0, worldToView, projection, cameraLocation, cameraRotation);

	Device->Brightness = engine->client->RenderBrightness();
	Device->Lock(vec4(0.5f, 0.5f, 0.5f, 1.0f), vec4(0.0f), vec4(0.0f), nullptr, nullptr);

	RenderingScopeTexture = true;
	ResetCanvas();
	DrawScene();

	// Reticle: thin cross, black outline then white, in the middle of the scope square.
	{
		Device->SetSceneNode(&Canvas.Frame);
		float cx = engine->viewport->ViewportWidth() * 0.5f;
		float cy = engine->viewport->ViewportHeight() * 0.5f;
		float arm = engine->viewport->ViewportWidth() * 0.08f;
		float gap = engine->viewport->ViewportWidth() * 0.015f;
		for (int pass = 0; pass < 2; pass++)
		{
			vec4 color = pass == 0 ? vec4(0.0f, 0.0f, 0.0f, 1.0f) : vec4(1.0f, 1.0f, 1.0f, 1.0f);
			float w = pass == 0 ? 2.0f : 1.0f;
			for (float o = -w; o <= w; o += 1.0f)
			{
				Device->Draw2DLine(&Canvas.Frame, color, LINE_None, vec3(cx - arm, cy + o, 1.0f), vec3(cx - gap, cy + o, 1.0f));
				Device->Draw2DLine(&Canvas.Frame, color, LINE_None, vec3(cx + gap, cy + o, 1.0f), vec3(cx + arm, cy + o, 1.0f));
				Device->Draw2DLine(&Canvas.Frame, color, LINE_None, vec3(cx + o, cy - arm, 1.0f), vec3(cx + o, cy - gap, 1.0f));
				Device->Draw2DLine(&Canvas.Frame, color, LINE_None, vec3(cx + o, cy + gap, 1.0f), vec3(cx + o, cy + arm, 1.0f));
			}
		}
	}
	RenderingScopeTexture = false;

	Device->EndFlash();
	Device->UnlockMenuTexture(width, height);
	VREyeOverride.Active = false;
}

void RenderSubsystem::SetScopeWorldQuad(bool active, const vec3 corners[4], vec2 uvMax)
{
	ScopeQuad.Active = active;
	if (active)
	{
		for (int i = 0; i < 4; i++)
			ScopeQuad.Corners[i] = corners[i];
		ScopeQuad.UVMax = uvMax;
	}
}

void RenderSubsystem::EndVRFrame()
{
	VREyeOverride.Active = false;
}

void RenderSubsystem::DrawGame(float levelTimeElapsed)
{
	LevelTimeElapsed = levelTimeElapsed;
	AutoUV += levelTimeElapsed * 64.0f;
	engine->Level->Light.Tick(levelTimeElapsed);

	// See BeginVRFrame()'s comment - moved out of DrawScene() so both call sites (this one,
	// the single-eye desktop path, and BeginVRFrame(), the VR path) run these exactly once per
	// app-frame regardless of how many times DrawScene() itself gets called this frame.
	engine->Level->Light.BeginFrame();
	TextureFrameCounter++;

	Stats.Frames = 0;
	Stats.Surfaces = 0;
	Stats.Actors = 0;
	Stats.LightmapsUpdated = 0;

	vec3 flashScale = 0.5f;
	vec3 flashFog = vec3(0.0f, 0.0f, 0.0f);

	UPlayerPawn* player = UObject::TryCast<UPlayerPawn>(engine->CameraActor);
	if (player)
	{
		flashScale = player->FlashScale();
		flashFog = player->FlashFog();
	}

	Device->Brightness = engine->client->RenderBrightness();
	Device->Lock(vec4(flashScale, 1.0f), vec4(flashFog, 1.0f), vec4(0.0f), nullptr, nullptr);

	ResetCanvas();
	PreRender();

	if (engine->LaunchInfo.ue1Version <= 219 || engine->console->bNoDrawWorld() == false)
	{
		DrawScene();
		RenderOverlays();
		if (engine->LaunchInfo.IsDeusEx())
			PostRenderFlash();
		Device->EndFlash();
	}

	PostRender();

	Device->Unlock(true);

	VREyeOverride.Active = false;
}

void RenderSubsystem::DrawEditorViewport()
{
	Device->Brightness = engine->client->RenderBrightness();

	// SurrealEditor's own repaint loop calls this directly, once per repaint, with no
	// DrawGame()/BeginVRFrame() call surrounding it - so unlike those two (which now do this
	// once per app-frame themselves, see their comments), this call site still needs to do it
	// here, matching DrawScene()'s original per-call behavior before it moved out.
	engine->Level->Light.BeginFrame();
	TextureFrameCounter++;

	DrawScene();
}

void RenderSubsystem::DrawVideoFrame(TextureInfo* frame, TextureInfo* background)
{
	vec3 flashScale = 0.5f;
	vec3 flashFog = vec3(0.0f, 0.0f, 0.0f);
	Device->Brightness = 0.4f;// engine->client->Brightness;
	Device->Lock(vec4(flashScale, 1.0f), vec4(flashFog, 1.0f), vec4(0.0f), nullptr, nullptr);
	ResetCanvas();
	Device->SetSceneNode(&Canvas.Frame);

	float sizeX = (float)(int)(engine->viewport->ViewportWidth() / (float)Canvas.uiscale);
	float sizeY = (float)(int)(engine->viewport->ViewportHeight() / (float)Canvas.uiscale);

	Rectf clipBox = Rectf::xywh(0.0f, 0.0f, sizeX, sizeY);
	Rectf dest = clipBox;

	if (frame)
	{
		Rectf src = Rectf::xywh(0.0f, 0.0f, (float)frame->USize, (float)frame->VSize);
		DrawTile(*frame, dest, src, clipBox, 1.0f, vec4(1.0f), vec4(0.0f), PF_TwoSided);
	}

	if (background)
	{
		Rectf src = Rectf::xywh(0.0f, 0.0f, (float)background->USize, (float)background->VSize);
		DrawTile(*background, dest, src, clipBox, 1.0f, vec4(1.0f), vec4(0.0f), PF_TwoSided | PF_Highlighted);
	}

	Device->EndFlash();
	Device->Unlock(true);
}

void RenderSubsystem::UpdateTexture(UTexture* tex)
{
	if (tex && tex->FrameCounter != TextureFrameCounter)
	{
		tex->Update(LevelTimeElapsed);
		tex->FrameCounter = TextureFrameCounter;
	}
}

void RenderSubsystem::UpdateTextureInfo(TextureInfo& info, BspSurface& surface, UTexture* texture, float ZoneUPanSpeed, float ZoneVPanSpeed)
{
	UpdateTextureInfo(info, texture);

	info.Pan.x = -(float)surface.PanU;
	info.Pan.y = -(float)surface.PanV;
	if (surface.PolyFlags & PF_AutoUPan) info.Pan.x -= AutoUV * ZoneUPanSpeed;
	if (surface.PolyFlags & PF_AutoVPan) info.Pan.y -= AutoUV * ZoneVPanSpeed;
}

void RenderSubsystem::UpdateTextureInfo(TextureInfo& info, const Poly& poly, UTexture* texture, float ZoneUPanSpeed, float ZoneVPanSpeed)
{
	UpdateTextureInfo(info, texture);

	info.Pan.x = -(float)poly.PanU;
	info.Pan.y = -(float)poly.PanV;
	if (poly.PolyFlags & PF_AutoUPan) info.Pan.x -= AutoUV * ZoneUPanSpeed;
	if (poly.PolyFlags & PF_AutoVPan) info.Pan.y -= AutoUV * ZoneVPanSpeed;
}

void RenderSubsystem::UpdateTextureInfo(TextureInfo& info, UTexture* texture)
{
	info.Texture = texture;
	info.CacheID = (uint64_t)(ptrdiff_t)texture;

	if (!info.Texture)
		return;

	info.UScale = texture->DrawScale();
	info.VScale = texture->DrawScale();
	info.Format = texture->UsedFormat;
	info.Mips = texture->UsedMipmaps.data();
	info.NumMips = (int)texture->UsedMipmaps.size();
	info.USize = texture->USize();
	info.VSize = texture->VSize();
	if (texture->Palette())
		info.Palette = (TextureColor*)texture->Palette()->Colors.data();

	info.bRealtimeChanged = texture->TextureModified;
	if (texture->TextureModified)
		texture->TextureModified = false;
}

void RenderSubsystem::OnMapLoaded()
{
	Device->Flush(true);
	engine->Level->Light.OnMapLoaded();
}
