
#include "Precomp.h"
#include "Packages/Engine/Resources/Mesh/UMesh.h"
#include <map>
#include <algorithm>
#include <set>
#include "Package/PackageManager.h"
#include "Utils/Logger.h"
#include "RenderSubsystem.h"
#include "VisibleMesh.h"
#include "RenderDevice/RenderDevice.h"
#include "Packages/Engine/Actors/Pawn/UPlayerPawn.h"
#include "Packages/Engine/Actors/Inventory/UWeapon.h"
#include "Packages/Engine/UViewport.h"
#include "Packages/Engine/UCanvas.h"
#include "Packages/Engine/UConsole.h"
#include "Packages/Engine/Resources/UFont.h"
#include "Packages/Engine/Resources/UPalette.h"
#include "Packages/Engine/Resources/Level/UModel.h"
#include "GameWindow.h"
#include "VM/ScriptCall.h"
#include "Engine.h"
#include "VR/VRInput.h"
#include "VR/VRCamera.h"

void RenderSubsystem::ResetCanvas()
{
	// Scale the UI so it matches what you saw on a 1024x768 CRT monitor for Unreal and other older games.
	// Assume 1280x960 for UT and newer.
	int vertResolution = engine->LaunchInfo.ue1Version < 400 ? 768 : 960;
	Canvas.uiscale = std::max((engine->viewport->ViewportHeight() + vertResolution / 2) / vertResolution, 1);
	if (RenderingMenuTexture)
	{
		// The VR menu panel: always uiscale 1. Pixel-doubling (a 540-line logical canvas) was
		// tried for readability and rejected on real Quest 3 hardware for being far too
		// low resolution - the panel only covers ~780 eye pixels across, so 720 logical pixels look like
		// a 640x480 desktop. Legibility comes from the panel's physical size instead
		// (Engine::RunVRMenuScreen's screenWidth/HeightMeters), keeping 1 logical pixel = 1
		// texture pixel, sharper than the eye display itself can resolve.
		// Unreal (Gold) is the exception: its older UWindow/UMenu does not pick larger fonts at higher
		// resolutions the way UT's does, so at 1 texel = 1 logical pixel its menus were unreadably
		// small on the panel (observed on Quest 3). Pixel-double it to the original 800x600 look.
		Canvas.uiscale = MenuUIScale();
	}

	SceneNode frame;
	Canvas.Frame.XB = 0;
	Canvas.Frame.YB = 0;
	Canvas.Frame.X = engine->viewport->ViewportWidth();
	Canvas.Frame.Y = engine->viewport->ViewportHeight();
	Canvas.Frame.FX = (float)engine->viewport->ViewportWidth();
	Canvas.Frame.FY = (float)engine->viewport->ViewportHeight();
	Canvas.Frame.FX2 = Canvas.Frame.FX * 0.5f;
	Canvas.Frame.FY2 = Canvas.Frame.FY * 0.5f;
	Canvas.Frame.ObjectToWorld = mat4::identity();
	Canvas.Frame.WorldToView = mat4::identity();
	Canvas.Frame.FovAngle = engine->CameraFovAngle;
	float Aspect = Canvas.Frame.FY / Canvas.Frame.FX;
	if (VREyeOverride.Active)
	{
		// VR HUD placement. DrawTile & co (VulkanRenderDevice.cpp) unproject each screen pixel
		// onto a view-space plane using RFX2/RFY2, i.e. a SYMMETRIC tangent range of
		// +-RProjZ derived from FovAngle - the HUD is effectively a flat card of that angular
		// size straight ahead. Projecting that card with the matching symmetric frustum (the
		// desktop path below) centers it on the IMAGE center of each eye - but with a real
		// HMD's asymmetric per-eye FOV, "straight ahead" sits ~25% of the image width off
		// center (leftwards in the left eye, rightwards in the right), so identical-pixel HUD
		// in both eyes lands on opposite sides of forward: a divergent disparity no one can
		// fuse. Confirmed on real Quest 3 hardware as "all HUDs and text doubled" (and very
		// likely the crosshair half of the long-running "eyes too different" report).
		//
		// So instead: keep the symmetric card (RFX2/RFY2 stay consistent with it), but project
		// it through the eye's REAL projection, so it's anchored to the forward direction in
		// both eyes. Two refinements: (1) the card must fit inside the narrower nasal side of
		// BOTH eyes, otherwise its outer edges fall off one eye's image - so FovAngle here is
		// derived from the projection's smaller half-tangents instead of CameraFovAngle;
		// (2) a card at infinity is uncomfortable against a nearby world, so shift it in
		// clip space by half the IPD over a 2m viewing distance, bringing it to ~2m.
		const mat4& P = VREyeOverride.Projection; // column-major, P[col*4+row]
		float diffX = 2.0f / P[0 * 4 + 0];                // tR - tL (near plane = 1)
		float sumX = P[2 * 4 + 0] * diffX;                // +-(tR + tL) - sign is irrelevant below
		float diffY = 2.0f / P[1 * 4 + 1];
		float sumY = P[2 * 4 + 1] * diffY;
		float nasalX = std::min(std::fabs((sumX + diffX) * 0.5f), std::fabs((sumX - diffX) * 0.5f));
		float nasalY = std::min(std::fabs((sumY + diffY) * 0.5f), std::fabs((sumY - diffY) * 0.5f));
		// 0.65 of the nasal FOV rather than nearly all of it: at ~40 degrees off-center the
		// HUD's corner elements (score, health, ammo) fell outside the comfortable field of
		// view on real Quest 3 hardware ("out of my sight range") - this pulls them in to
		// roughly 27 degrees.
		float hudHalfTangent = std::min(nasalX, nasalY / Aspect) * 0.65f;
		Canvas.Frame.FovAngle = degrees(2.0f * std::atan(hudHalfTangent));

		const float halfIpdMeters = 0.032f;
		const float hudDistanceMeters = 2.0f;
		float ndcShift = (halfIpdMeters / hudDistanceMeters) * P[0 * 4 + 0]; // tangent shift -> NDC via 2/(tR-tL)
		if (VREyeOverride.Eye == 1)
			ndcShift = -ndcShift; // left eye sees a 2m object right of forward, right eye left of it
		if (RenderingScopeTexture)
			ndcShift = 0.0f; // the scope is a mono render, its reticle must sit dead center
		Canvas.Frame.Projection = mat4::translate(vec3(ndcShift, 0.0f, 0.0f)) * P;
	}
	else
	{
		float RProjZ = (float)std::tan(radians(Canvas.Frame.FovAngle) * 0.5f);
		Canvas.Frame.Projection = mat4::frustum(-RProjZ, RProjZ, -Aspect * RProjZ, Aspect * RProjZ, 1.0f, 32768.0f, handedness::left, clipzrange::zero_positive_w);
	}

	int sizeX = (int)(engine->viewport->ViewportWidth() / (float)Canvas.uiscale);
	int sizeY = (int)(engine->viewport->ViewportHeight() / (float)Canvas.uiscale);
	engine->canvas->CurX() = 0.0f;
	engine->canvas->CurY() = 0.0f;
	if (engine->LaunchInfo.ue1Version > 219)
	{
		engine->console->FrameX() = (float)sizeX;
		engine->console->FrameY() = (float)sizeY;
	}
	engine->canvas->ClipX() = (float)sizeX;
	engine->canvas->ClipY() = (float)sizeY;
	engine->canvas->SizeX() = sizeX;
	engine->canvas->SizeY() = sizeY;

	if (engine->LaunchInfo.IsHarryPotter1())
	{
		engine->canvas->ClipX() = (float)sizeY * (4.0f / 3.0f);
		engine->canvas->SizeX() = (int)std::round(sizeY * (4.0f / 3.0f));
	}

	//engine->viewport->bShowWindowsMouse() = true; // bShowWindowsMouse is set to true by WindowConsole if mouse cursor should be visible
	//engine->viewport->bWindowsMouseAvailable() = true; // if true then RenderUWindow updates mouse pos from (WindowsMouseX,WindowsMouseY), otherwise it uses KeyEvent(IK_MouseX, delta) + KeyEvent(IK_MouseY, delta). Maybe used for windowed mode?
	//engine->viewport->WindowsMouseX() = 10.0f;
	//engine->viewport->WindowsMouseY() = 200.0f;
	CallEvent(engine->canvas, EventName::Reset);
}

void RenderSubsystem::PreRender()
{
	Device->SetSceneNode(&Canvas.Frame);
	CallEvent(engine->console, EventName::PreRender, { ExpressionValue::ObjectValue(engine->canvas) });
	if (engine->viewport->Actor())
		CallEvent(engine->viewport->Actor(), EventName::PreRender, { ExpressionValue::ObjectValue(engine->canvas) });
}

void RenderSubsystem::RenderOverlays()
{
	Device->SetSceneNode(&Canvas.Frame);
	if (engine->viewport->Actor())
	{
		if (engine->LaunchInfo.ue1Version > 219)
		{
			CallEvent(engine->viewport->Actor(), EventName::RenderOverlays, { ExpressionValue::ObjectValue(engine->canvas) });
		}
		else
		{
			UWeapon* weapon = engine->viewport->Actor()->Weapon();
			if (weapon)
			{
				CallEvent(weapon, "InvCalcView", {});
				DrawActor(weapon, false, false);
			}
		}
	}
}

void RenderSubsystem::PostRender()
{
	Device->SetSceneNode(&Canvas.Frame);
	if (engine->viewport->Actor())
		CallEvent(engine->viewport->Actor(), EventName::PostRender, { ExpressionValue::ObjectValue(engine->canvas) });
	CallEvent(engine->console, EventName::PostRender, { ExpressionValue::ObjectValue(engine->canvas) });
	DrawTimedemoStats();
	
	if (ShowCollisionDebug)
		DrawCollisionDebug();
}

void RenderSubsystem::PostRenderFlash()
{
	Device->SetSceneNode(&Canvas.Frame);
	if (engine->viewport->Actor())
		CallEvent(engine->viewport->Actor(), "PostRenderFlash", {ExpressionValue::ObjectValue(engine->canvas)});
}

void RenderSubsystem::DrawActor(UActor* actor, bool WireFrame, bool ClearZ)
{
	Device->SetSceneNode(&MainFrame.Frame);
	if (ClearZ)
		Device->ClearZ();

	// VR: Canvas.DrawActor is how the first-person weapon gets drawn (the script's
	// RenderOverlays event places the weapon at the eye + PlayerViewOffset, then calls this),
	// which in VR would leave it floating in front of the face, glued to the head. Instead
	// hold it in the right hand: put it at the right controller's world position (same
	// STAGE-to-world chain the eye cameras use, so hand and eyes agree on where the room is)
	// pointing along the controller's aim direction (VRInput::GetAimRotation, the same
	// rotation weapon fire is overridden to). Restored afterwards so the script's own
	// bookkeeping (muzzle flash positions etc.) sees what it set.
	bool overridden = false;
	vec3 savedLocation;
	Rotator savedRotation;
	float savedDrawScale = 1.0f;
	// UT's dual Enforcers: the second pistol is a separate "slave" weapon actor (bIsSlave) the
	// script draws through this same path - that one goes in the LEFT hand.
	bool slaveWeapon = actor->HasProperty("bIsSlave") && actor->GetBool("bIsSlave");
	VRInput* vr = engine->vrInput.get();
	bool handActive = vr && (slaveWeapon ? vr->LeftControllerActive : vr->RightControllerActive);
	if (VREyeOverride.Active && handActive)
	{
		savedLocation = actor->Location();
		savedRotation = actor->Rotation();
		savedDrawScale = actor->DrawScale();
		// Position from the holding hand; direction ALWAYS the right hand's aim - both pistols
		// fire along the single right-hand crosshair (UT's dual-Enforcer script has one aim), so
		// the left one pointing where its own controller points would misrepresent where its
		// shots go.
		const vec3& handPosition = slaveWeapon ? vr->LeftControllerPosition : vr->RightControllerPosition;
		actor->Location() = VRCamera::StageToWorldPosition(handPosition, engine->CameraLocation, vr->BodyYawRadians, VRCamera::UnitsPerMeter, vr->HeadAnchorPosition);
		if (slaveWeapon)
		{
			// The second Enforcer's mesh sits higher relative to its origin than the first one
			// (the second pistol sat visibly higher) - bring it level.
			const float slaveWeaponDropMeters = 0.07f; // 0 was too high, 0.14 too low
			actor->Location().z -= slaveWeaponDropMeters * VRCamera::UnitsPerMeter;
		}
		// Unreal (Gold) hand offsets (meters, world up). The fire ray runs along the controller aim
		// through the mesh ORIGIN, but Unreal's first-person meshes are modeled with the barrel a
		// little ABOVE their origin, so shots appeared to leave below the barrel
		// (observed on Quest 3). Drop every held mesh so the visual barrel sits on the shot line;
		// the Eightball keeps its own larger, earlier-tuned drop (not stacked).
		// Unreal (Gold) per-weapon hand offsets (meters, world up): the Eightball's mesh origin
		// sits low in its geometry, so it rides high once scaled (sat ~30 cm too high).
		// (A general 6 cm drop briefly lived here to hide shots landing below the barrel; the real
		// cause was the fire-convergence origin in VRAimOverrideScope, fixed there.)
		if (engine->packages->IsUnreal1() && actor->IsA("Eightball"))
			actor->Location().z -= 0.30f * VRCamera::UnitsPerMeter;
		// Pistols: lowered 15 cm - they sat too high in the hand.
		if (engine->packages->IsUnreal1() && (actor->IsA("AutoMag") || actor->IsA("DispersionPistol")))
			actor->Location().z -= 0.15f * VRCamera::UnitsPerMeter;
		actor->Rotation() = vr->GetAimRotation();
		// First-person weapon meshes are modeled for a flat screen where the camera sits right
		// behind them; held out at arm's length in VR they read as toy-sized (real Quest 3
		// testing: they read as toy-sized without it).
		// Tuned on real Quest 3 hardware: 2x too small, 8x too big, 4x confirmed - except the
		// minigun and flak cannon, whose first-person meshes are modeled noticeably smaller and
		// needed doubling again.
		float vrWeaponScale = 4.0f;
		if (engine->packages->IsUnreal1())
		{
			// Unreal (Gold): a uniform 4x for every weapon, settled on after testing on Quest 3
			// hardware after trying per-weapon tables (8x rocket launcher/minigun/flak, 6x rifle) and two
			// data-driven variants (scale by PlayerViewOffset; by apparent size = mesh extent / offset).
			// The numbers those used are still logged once per weapon class for future tuning.
			static std::set<std::string> loggedClasses;
			std::string className = UObject::GetUClassName(actor).ToString();
			if (loggedClasses.insert(className).second)
			{
				UMesh* mesh = actor->Mesh();
				float extent = 0.0f;
				if (mesh && mesh->FrameVerts > 0 && (size_t)mesh->FrameVerts <= mesh->Verts.size())
				{
					vec3 bmin(1e30f), bmax(-1e30f);
					for (int i = 0; i < mesh->FrameVerts; i++)
					{
						vec4 v = mesh->meshToObject * vec4(mesh->Verts[i], 1.0f);
						bmin = vec3(std::min(bmin.x, v.x), std::min(bmin.y, v.y), std::min(bmin.z, v.z));
						bmax = vec3(std::max(bmax.x, v.x), std::max(bmax.y, v.y), std::max(bmax.z, v.z));
					}
					extent = length(bmax - bmin);
				}
				vec3 viewOffset = actor->HasProperty("PlayerViewOffset") ? actor->GetVector("PlayerViewOffset") : vec3(0.0f);
				LogMessage("VR weapon " + className + ": PlayerViewOffset=(" + std::to_string(viewOffset.x) + "," + std::to_string(viewOffset.y) + "," + std::to_string(viewOffset.z) + ") len=" + std::to_string(length(viewOffset)) + " extent=" + std::to_string(extent) + " scale=" + std::to_string(vrWeaponScale));
			}
			// Per-weapon adjustments from Quest 3 testing on top of the uniform 4x default
			// (Stinger halved, Eightball enlarged).
			if (actor->IsA("Stinger"))
				vrWeaponScale = 2.0f;
			else if (actor->IsA("Eightball"))
				vrWeaponScale = 8.4f; // 8x proved too small, 12x too big
			else if (actor->IsA("Rifle")) // the sniper rifle
				vrWeaponScale = 8.0f;
			else if (actor->IsA("GESBioRifle"))
				vrWeaponScale = 8.0f; // undersized at 4x
			else if (actor->IsA("Razorjack"))
				vrWeaponScale = 8.0f; // undersized at 4x
			else if (actor->IsA("FlakCannon"))
				vrWeaponScale = 8.0f; // undersized at 4x
		}
		else if (actor->IsA("minigun2") || actor->IsA("UT_FlakCannon") || actor->IsA("WarheadLauncher") || actor->IsA("ut_biorifle"))
			vrWeaponScale = 8.0f;
		else if (actor->IsA("enforcer")) // covers doubleenforcer too; slightly oversized at 4x
			vrWeaponScale = 2.8f;
		else if (actor->IsA("Translocator")) // twice too big at 4x (Quest 3 testing)
			vrWeaponScale = 2.0f;
		actor->DrawScale() = savedDrawScale * vrWeaponScale;
		overridden = true;
	}

	actor->bHidden() = false;
	VisibleMesh vismesh;
	if (vismesh.DrawMesh(&MainFrame, actor, WireFrame, false))
		vismesh.DrawMesh(&MainFrame, actor, WireFrame, true);
	actor->bHidden() = true;

	if (overridden)
	{
		actor->Location() = savedLocation;
		actor->Rotation() = savedRotation;
		actor->DrawScale() = savedDrawScale;
	}

	Device->SetSceneNode(&Canvas.Frame);
}

void RenderSubsystem::DrawClippedActor(UActor* actor, bool WireFrame, int X, int Y, int XB, int YB, bool ClearZ)
{
	SceneNode frame;
	frame.XB = XB * Canvas.uiscale;
	frame.YB = YB * Canvas.uiscale;
	frame.X = X * Canvas.uiscale;
	frame.Y = Y * Canvas.uiscale;
	frame.FX = (float)X * Canvas.uiscale;
	frame.FY = (float)Y * Canvas.uiscale;
	frame.FX2 = frame.FX * 0.5f;
	frame.FY2 = frame.FY * 0.5f;
	frame.ObjectToWorld = Coords::ViewToRenderDev().ToMatrix();
	frame.WorldToView = mat4::identity();
	frame.FovAngle = engine->CameraFovAngle;
	float Aspect = frame.FY / frame.FX;
	float RProjZ = (float)std::tan(radians(frame.FovAngle) * 0.5f);
	float RFX2 = 2.0f * RProjZ / frame.FX;
	float RFY2 = 2.0f * RProjZ * Aspect / frame.FY;
	frame.Projection = mat4::frustum(-RProjZ, RProjZ, -Aspect * RProjZ, Aspect * RProjZ, 1.0f, 32768.0f, handedness::left, clipzrange::zero_positive_w);
	Device->SetSceneNode(&frame);

	if (ClearZ)
		Device->ClearZ();

	actor->bHidden() = false;
	VisibleMesh vismesh;
	// VisibleMesh::DrawMesh() reads its view/projection matrices straight from the
	// VisibleFrame argument's Frame member (VisibleMesh.cpp: frame->Frame.WorldToView), NOT
	// from whatever Device->SetSceneNode() above was just called with - those are two
	// different things Device tracks separately. Passing &MainFrame here means this preview
	// draw was silently using MainFrame.Frame's transform - in VR, whatever the CURRENT eye's
	// live head-tracked camera happens to be - instead of the neutral, fixed `frame` built
	// above for exactly this preview. Confirmed as a real bug (not VR-specific, just far more
	// visible with stereo depth cues exposing it) by an equivalent, already-fixed issue in a
	// sibling VR port project (Sauerbraten-Quest/rendergl.cpp's setcammatrix(), which hit the
	// identical symptom - "way too much difference between the eyes" on a menu preview model -
	// tracing to the same class of mistake: a should-be-fixed camera pass inheriting the live
	// head-tracked one). Temporarily swap in the correct transform for this call only.
	SceneNode savedMainFrame = MainFrame.Frame;
	MainFrame.Frame = frame;
	if (vismesh.DrawMesh(&MainFrame, actor, WireFrame, false))
		vismesh.DrawMesh(&MainFrame, actor, WireFrame, true);
	MainFrame.Frame = savedMainFrame;
	actor->bHidden() = true;

	Device->SetSceneNode(&Canvas.Frame);
}

void RenderSubsystem::DrawTile(UTexture* Tex, float x, float y, float XL, float YL, float U, float V, float UL, float VL, float Z, vec4 color, vec4 fog, uint32_t flags)
{
	if (!Tex)
		return;
	UpdateTexture(Tex);
	Tex = Tex->GetAnimTexture();
	UpdateTexture(Tex);

	TextureInfo texinfo;
	texinfo.CacheID = (uint64_t)(ptrdiff_t)Tex;
	texinfo.Texture = Tex;
	texinfo.Format = texinfo.Texture->UsedFormat;
	texinfo.Mips = Tex->UsedMipmaps.data();
	texinfo.NumMips = (int)Tex->UsedMipmaps.size();
	texinfo.USize = Tex->USize();
	texinfo.VSize = Tex->VSize();
	if (Tex->Palette())
		texinfo.Palette = (TextureColor*)Tex->Palette()->Colors.data();

	if (Tex->bMasked())
		flags |= PF_Masked;

	// VR: the HUD's own crosshair draw (UT's ChallengeHUD.DrawCrosshair -> Canvas.DrawIcon ->
	// here, with the player's configured CrosshairTextures[Crosshair] - see
	// UpdateVRAimPoint(), RenderSubsystem.cpp) is redirected from screen center to a
	// camera-facing billboard where the right controller's aim ray hits the world. Same
	// texture, color, style flags and on-screen size as the game intended (the tile's pixel
	// size is converted to the same angular size via the canvas' pixel-to-tangent factors,
	// then scaled by the hit distance), so the player's crosshair choice is honored and
	// nothing else about the HUD changes.
	// The weapon's muzzle flash (TournamentWeapon.DrawMuzzleFlash -> Canvas.DrawIcon(MFTexture))
	// gets the same redirect, anchored at the gun's muzzle instead of the aim point - as a 2D
	// icon it followed the head, not the gun (Quest 3 testing on the minigun).
	auto matches = [&](UTexture* wanted) { return wanted && (Tex == wanted || Tex == wanted->GetAnimTexture()); };
	bool isCrosshair = matches(VRAim.CrosshairTexture);
	bool isMuzzleFlash = !isCrosshair && VREyeOverride.Active && matches(CurrentMuzzleFlashTexture());
	if (VREyeOverride.Active && VRAim.Valid && (isCrosshair || isMuzzleFlash))
	{
		// Anchor and the distance that sets the billboard's world size (so it keeps the
		// on-screen size the game intended, measured from the viewer).
		vec3 anchor = isCrosshair ? VRAim.HitPoint - VRAim.Dir * (VRAim.Dist * 0.005f) : VRAim.MuzzlePoint; // crosshair fractionally in front of the hit surface
		float dist = isCrosshair ? VRAim.Dist : std::max(length(VRAim.MuzzlePoint - VREyeOverride.EyeLocation), 1.0f);

		float Aspect = Canvas.Frame.FY / Canvas.Frame.FX;
		float RProjZ = (float)std::tan(radians(Canvas.Frame.FovAngle) * 0.5f);
		float RFX2 = 2.0f * RProjZ / Canvas.Frame.FX;
		float RFY2 = 2.0f * RProjZ * Aspect / Canvas.Frame.FY;
		float halfW = dist * RFX2 * XL * Canvas.uiscale * 0.5f;
		float halfH = dist * RFY2 * YL * Canvas.uiscale * 0.5f;

		vec3 right = VREyeOverride.EyeRotation.YAxis;
		vec3 up = VREyeOverride.EyeRotation.ZAxis;
		vec3 center = anchor;

		GouraudVertex pts[4];
		pts[0].Point = center - right * halfW + up * halfH; pts[0].UV = vec2(U, V);
		pts[1].Point = center + right * halfW + up * halfH; pts[1].UV = vec2(U + UL, V);
		pts[2].Point = center + right * halfW - up * halfH; pts[2].UV = vec2(U + UL, V + VL);
		pts[3].Point = center - right * halfW - up * halfH; pts[3].UV = vec2(U, V + VL);
		for (GouraudVertex& p : pts)
		{
			p.Light = color.xyz();
			p.Fog = vec4(0.0f);
		}

		Device->SetSceneNode(&MainFrame.Frame);
		Device->ClearZ(); // always visible, like the HUD element it replaces
		Device->DrawGouraudPolygon(&MainFrame.Frame, texinfo, pts, 4, flags);
		Device->SetSceneNode(&Canvas.Frame);
		return;
	}

	Device->DrawTile(&Canvas.Frame, texinfo, x * Canvas.uiscale, y * Canvas.uiscale, XL * Canvas.uiscale, YL * Canvas.uiscale, U, V, UL, VL, Z, color, fog, flags);
}

void RenderSubsystem::DrawTileClipped(UTexture* Tex, float orgX, float orgY, float curX, float curY, float XL, float YL, float U, float V, float UL, float VL, float Z, vec4 color, vec4 fog, uint32_t flags, float clipX, float clipY)
{
	if (!Tex)
		return;
	UpdateTexture(Tex);
	Tex = Tex->GetAnimTexture();
	UpdateTexture(Tex);

	TextureInfo texinfo;
	texinfo.CacheID = (uint64_t)(ptrdiff_t)Tex;
	texinfo.Texture = Tex;
	texinfo.Format = texinfo.Texture->UsedFormat;
	texinfo.Mips = Tex->UsedMipmaps.data();
	texinfo.NumMips = (int)Tex->UsedMipmaps.size();
	texinfo.USize = Tex->USize();
	texinfo.VSize = Tex->VSize();
	if (Tex->Palette())
		texinfo.Palette = (TextureColor*)Tex->Palette()->Colors.data();

	if (Tex->bMasked())
		flags |= PF_Masked;

	Rectf clipBox = Rectf::xywh(orgX, orgY, clipX, clipY);
	Rectf dest = Rectf::xywh(orgX + curX, orgY + curY, XL, YL);
	Rectf src = Rectf::xywh(U, V, UL, VL);
	DrawTile(texinfo, dest, src, clipBox, Z, color, fog, flags);
}

Array<std::string> RenderSubsystem::FindTextBlocks(const std::string& text)
{
	// Split text into words, whitespace or newline
	Array<std::string> textBlocks;
	size_t pos = 0;
	while (pos < text.size())
	{
		if (text[pos] == '\n')
		{
			textBlocks.push_back("\n");
			pos++;
		}
		else if (text[pos] == ' ')
		{
			size_t end = std::min(text.find_first_not_of(' ', pos + 1), text.size());
			textBlocks.push_back(text.substr(pos, end - pos));
			pos = end;
		}
		else
		{
			size_t end = std::min(text.find_first_of(" \n", pos + 1), text.size());
			textBlocks.push_back(text.substr(pos, end - pos));
			pos = end;
		}
	}
	return textBlocks;
}

void RenderSubsystem::DrawTextBlockRange(float x, float y, const Array<std::string>& textBlocks, size_t start, size_t end, UFont* font, vec4 color, uint32_t polyflags, float spaceX)
{
	for (size_t i = start; i < end; i++)
	{
		for (char c : textBlocks[i])
		{
			FontGlyph glyph = font->GetGlyph(c);

			if (!glyph.Texture)
				continue;

			TextureInfo texinfo;
			texinfo.CacheID = (uint64_t)(ptrdiff_t)glyph.Texture;
			texinfo.Texture = glyph.Texture;
			texinfo.Format = texinfo.Texture->UsedFormat;
			texinfo.Mips = glyph.Texture->UsedMipmaps.data();
			texinfo.NumMips = (int)glyph.Texture->UsedMipmaps.size();
			texinfo.USize = glyph.Texture->USize();
			texinfo.VSize = glyph.Texture->VSize();
			if (glyph.Texture->Palette())
				texinfo.Palette = (TextureColor*)glyph.Texture->Palette()->Colors.data();

			int width = glyph.USize;
			int height = glyph.VSize;
			float StartU = (float)glyph.StartU;
			float StartV = (float)glyph.StartV;
			float USize = (float)glyph.USize;
			float VSize = (float)glyph.VSize;

			Device->DrawTile(&Canvas.Frame, texinfo, x * Canvas.uiscale, y * Canvas.uiscale, (float)width * Canvas.uiscale, (float)height * Canvas.uiscale, StartU, StartV, USize, VSize, 1.0f, color, vec4(0.0f), polyflags);

			x += width + spaceX;
		}
	}
}

void RenderSubsystem::DrawText(UFont* font, vec4 color, float orgX, float orgY, float& curX, float& curY, float& curXL, float& curYL, bool newlineAtEnd, const std::string& text, uint32_t polyflags, bool center, float spaceX, float spaceY, float clipX, float clipY, bool noDraw)
{
	float totalWidth = 0.0f;
	float totalHeight = 0.0f;

	Array<std::string> textBlocks = FindTextBlocks(text);
	size_t lineBegin = 0;
	float lineWidth = 0.0f;
	float lineHeight = 0.0f;
	for (size_t pos = 0; pos < textBlocks.size(); pos++)
	{
		if (textBlocks[pos].front() == '\n')
		{
			if (pos != lineBegin)
			{
				float centerX = 0;
				if (center)
					centerX = std::round((clipX - lineWidth) * 0.5f);
				if (!noDraw)
					DrawTextBlockRange(orgX + curX + centerX, orgY + curY, textBlocks, lineBegin, pos, font, color, polyflags, spaceX);
				curY += lineHeight;
				totalHeight += lineHeight;
				totalWidth = std::max(totalWidth, lineWidth);
			}

			curX = 0;
			lineBegin = pos + 1;
			lineWidth = 0.0f;
			lineHeight = 0.0f;
		}
		else
		{
			vec2 blockSize = GetTextSize(font, textBlocks[pos], spaceX, spaceY);
			if (lineWidth + blockSize.x > clipX)
			{
				float centerX = 0;
				if (center)
					centerX = std::round((clipX - lineWidth) * 0.5f);
				if (!noDraw)
					DrawTextBlockRange(orgX + curX + centerX, orgY + curY, textBlocks, lineBegin, pos, font, color, polyflags, spaceX);

				curX = 0;
				curY += lineHeight;
				totalHeight += lineHeight;
				totalWidth = std::max(totalWidth, lineWidth);

				if (textBlocks[pos].front() == ' ')
				{
					// Ignore whitespace at the beginning of a word wrapped line
					lineBegin = pos + 1;
					lineWidth = 0.0f;
					lineHeight = 0.0f;
				}
				else
				{
					lineBegin = pos;
					lineWidth = blockSize.x;
					lineHeight = blockSize.y;
				}
			}
			else
			{
				lineWidth += blockSize.x;
				lineHeight = std::max(lineHeight, blockSize.y);
			}
		}
	}

	if (lineBegin < textBlocks.size())
	{
		float centerX = 0;
		if (center)
			centerX = std::round((clipX - lineWidth) * 0.5f);
		if (!noDraw)
			DrawTextBlockRange(orgX + curX + centerX, orgY + curY, textBlocks, lineBegin, textBlocks.size(), font, color, polyflags, spaceX);
		curX += centerX + lineWidth;
		curY += lineHeight;
		totalHeight += lineHeight;
		totalWidth = std::max(totalWidth, lineWidth);
	}

	curXL = std::max(curXL, totalWidth);
	curYL = std::max(curYL, totalHeight);

	if (newlineAtEnd)
	{
		curX = 0;
		curY += curYL;
		curXL = 0;
		curYL = 0;
	}
}

void RenderSubsystem::DrawTextClipped(UFont* font, vec4 color, float orgX, float orgY, float curX, float curY, const std::string& text, uint32_t polyflags, bool checkHotKey, float clipX, float clipY, bool center)
{
	FontGlyph uglyph = font->GetGlyph('_');
	int uwidth = uglyph.USize;
	int uheight = uglyph.VSize;
	float uStartU = (float)uglyph.StartU;
	float uStartV = (float)uglyph.StartV;
	float uUSize = (float)uglyph.USize;
	float uVSize = (float)uglyph.VSize;

	Rectf clipBox = Rectf::xywh(orgX, orgY, clipX, clipY);

	float centerX = 0;
	if (center)
		centerX = std::round((clipX - GetTextSize(font, text).x) * 0.5f);

	bool foundAmpersand = false;
	int maxY = 0;
	for (char c : text)
	{
		if (checkHotKey && c == '&' && !foundAmpersand)
		{
			foundAmpersand = true;
		}
		else if (foundAmpersand && c != '&')
		{
			foundAmpersand = false;

			FontGlyph glyph = font->GetGlyph(c);
			if (curX + glyph.USize > (int)clipX)
				break;

			TextureInfo texinfo;
			texinfo.CacheID = (uint64_t)(ptrdiff_t)glyph.Texture;
			texinfo.Texture = glyph.Texture;
			texinfo.Format = texinfo.Texture->UsedFormat;
			texinfo.Mips = glyph.Texture->UsedMipmaps.data();
			texinfo.NumMips = (int)glyph.Texture->UsedMipmaps.size();
			texinfo.USize = glyph.Texture->USize();
			texinfo.VSize = glyph.Texture->VSize();
			if (glyph.Texture->Palette())
				texinfo.Palette = (TextureColor*)glyph.Texture->Palette()->Colors.data();

			Rectf dest = Rectf::xywh(orgX + curX + centerX, orgY + curY, (float)glyph.USize, (float)glyph.VSize);
			Rectf src = Rectf::xywh((float)glyph.StartU, (float)glyph.StartV, (float)glyph.USize, (float)glyph.VSize);
			DrawTile(texinfo, dest, src, clipBox, 1.0f, color, vec4(0.0f), polyflags);

			texinfo.CacheID = (uint64_t)(ptrdiff_t)uglyph.Texture;
			texinfo.Texture = uglyph.Texture;

			dest = Rectf::xywh(orgX + curX + (glyph.USize - uwidth) / 2, orgY + curY, (float)uwidth, (float)uheight);
			src = Rectf::xywh(uStartU, uStartV, uUSize, uVSize);
			DrawTile(texinfo, dest, src, clipBox, 1.0f, color, vec4(0.0f), polyflags);

			curX += glyph.USize;
			maxY = std::max(maxY, glyph.VSize);
		}
		else
		{
			foundAmpersand = false;

			FontGlyph glyph = font->GetGlyph(c);
			if (curX + glyph.USize > (int)clipX)
				break;

			TextureInfo texinfo;
			texinfo.CacheID = (uint64_t)(ptrdiff_t)glyph.Texture;
			texinfo.Texture = glyph.Texture;
			texinfo.Format = texinfo.Texture->UsedFormat;
			texinfo.Mips = glyph.Texture->UsedMipmaps.data();
			texinfo.NumMips = (int)glyph.Texture->UsedMipmaps.size();
			texinfo.USize = glyph.Texture->USize();
			texinfo.VSize = glyph.Texture->VSize();
			if (glyph.Texture->Palette())
				texinfo.Palette = (TextureColor*)glyph.Texture->Palette()->Colors.data();

			Rectf dest = Rectf::xywh(orgX + curX + centerX, orgY + curY, (float)glyph.USize, (float)glyph.VSize);
			Rectf src = Rectf::xywh((float)glyph.StartU, (float)glyph.StartV, (float)glyph.USize, (float)glyph.VSize);
			DrawTile(texinfo, dest, src, clipBox, 1.0f, color, vec4(0.0f), PF_Highlighted | PF_NoSmooth | PF_Masked);

			curX += glyph.USize;
			maxY = std::max(maxY, glyph.VSize);
		}
	}
}

void RenderSubsystem::DrawTile(TextureInfo& texinfo, const Rectf& dest, const Rectf& src, const Rectf& clipBox, float Z, vec4 color, vec4 fog, uint32_t flags)
{
	if (dest.left > dest.right || dest.top > dest.bottom)
		return;

	if (dest.left >= clipBox.left && dest.top >= clipBox.top && dest.right <= clipBox.right && dest.bottom <= clipBox.bottom)
	{
		Device->DrawTile(&Canvas.Frame, texinfo, dest.left * Canvas.uiscale, dest.top * Canvas.uiscale, (dest.right - dest.left) * Canvas.uiscale, (dest.bottom - dest.top) * Canvas.uiscale, src.left, src.top, src.right - src.left, src.bottom - src.top, Z, color, fog, flags);
	}
	else
	{
		Rectf d = dest;
		Rectf s = src;

		float scaleX = (s.right - s.left) / (d.right - d.left);
		float scaleY = (s.bottom - s.top) / (d.bottom - d.top);

		if (d.left < clipBox.left)
		{
			s.left += scaleX * (clipBox.left - d.left);
			d.left = clipBox.left;
		}
		if (d.right > clipBox.right)
		{
			s.right += scaleX * (clipBox.right - d.right);
			d.right = clipBox.right;
		}
		if (d.top < clipBox.top)
		{
			s.top += scaleY * (clipBox.top - d.top);
			d.top = clipBox.top;
		}
		if (d.bottom > clipBox.bottom)
		{
			s.bottom += scaleY * (clipBox.bottom - d.bottom);
			d.bottom = clipBox.bottom;
		}

		if (d.left < d.right && d.top < d.bottom)
			Device->DrawTile(&Canvas.Frame, texinfo, d.left * Canvas.uiscale, d.top * Canvas.uiscale, (d.right - d.left) * Canvas.uiscale, (d.bottom - d.top) * Canvas.uiscale, s.left, s.top, s.right - s.left, s.bottom - s.top, Z, color, fog, flags);
	}
}

void RenderSubsystem::Draw2DLine(vec4 Color, uint32_t LineFlags, vec3 P1, vec3 P2, bool useUIScale)
{
	auto uiscale = useUIScale ? static_cast<float>(Canvas.uiscale) : 1.0f;
	Device->Draw2DLine(&Canvas.Frame, Color, LineFlags, vec3(P1.xy() * uiscale, P1.z), vec3(P2.xy() * uiscale, P2.z));
}

void RenderSubsystem::Draw3DLine(vec4 Color, uint32_t LineFlags, vec3 P1, vec3 P2)
{
	Device->Draw3DLine(&Canvas.Frame, Color, LineFlags, P1, P2);
}

void RenderSubsystem::DrawTile(TextureInfo& Info, float X, float Y, float XL, float YL, float U, float V, float UL, float VL, float Z, vec4 Color, vec4 Fog, uint32_t PolyFlags)
{
	Device->DrawTile(&Canvas.Frame, Info, X, Y, XL, YL, U, V, UL, VL, Z, Color, Fog, PolyFlags);
}

vec2 RenderSubsystem::GetTextSize(UFont* font, const std::string& text, float spaceX, float spaceY)
{
	float x = 0.0f;
	float y = 0.0f;
	for (char c : text)
	{
		FontGlyph glyph = font->GetGlyph(c);
		x += (float)glyph.USize + spaceX;
		y = std::max(y, (float)glyph.VSize + spaceY);
	}
	return { x, y };
}

void RenderSubsystem::DrawTimedemoStats()
{
	Canvas.framesDrawn++;
	if (Canvas.startFPSTime == 0 || engine->lastTime - Canvas.startFPSTime >= 1'000'000)
	{
		Canvas.fps = Canvas.framesDrawn;
		Canvas.startFPSTime = engine->lastTime;
		Canvas.framesDrawn = 0;
	}

	if (ShowTimedemoStats || engine->LaunchInfo.IsDeusEx())
	{
		Array<std::string> lines;
		lines.push_back(std::to_string(Canvas.fps) + " FPS");
		lines.push_back(std::to_string(engine->Level->Actors.size()) + " actors");
		lines.push_back(std::to_string(GC::GetStats().numObjects) + " GC objects");
		lines.push_back(std::to_string(GC::GetStats().memoryUsage / (1024 * 1024)) + " mb memory used");
		lines.push_back(std::to_string(Stats.Frames) + " visible frames");
		lines.push_back(std::to_string(Stats.Surfaces) + " visible surfaces");
		lines.push_back(std::to_string(Stats.Actors) + " visible actors");
		lines.push_back(std::to_string(Stats.LightmapsUpdated) + " lightmaps updated");

		UFont* font = engine->canvas->SmallFont();
		if (font)
		{
			float curY = 180;
			for (const std::string& text : lines)
			{
				float curX = engine->viewport->ViewportWidth() / (float)Canvas.uiscale - GetTextSize(font, text).x - 16;
				float curXL = 0.0f;
				float curYL = 0.0f;
				DrawText(font, vec4(1.0f), 0.0f, 0.0f, curX, curY, curXL, curYL, false, text, PF_NoSmooth | PF_Masked, false);
				curY += curYL;
			}

			/*
			Array<std::string> leftlines;
			engine->audiodev->AddStats(leftlines);
			curY = 64;
			for (const std::string& text : leftlines)
			{
				float curX = 16.0f;
				float curXL = 0.0f;
				float curYL = 0.0f;
				DrawText(font, vec4(1.0f), 0.0f, 0.0f, curX, curY, curXL, curYL, false, text, PF_NoSmooth | PF_Masked, false);
				curY += curYL;
			}
			*/
		}
	}

	if (ShowRenderStats)
	{
		Array<std::string> lines;
		lines.push_back(std::to_string(Canvas.fps) + " FPS");
		lines.push_back(std::to_string(engine->Level->Actors.size()) + " actors");
		lines.push_back(std::to_string(GC::GetStats().numObjects) + " GC objects");
		lines.push_back(std::to_string(GC::GetStats().memoryUsage / (1024 * 1024)) + " mb memory used");

		/*size_t numCollisionActors = 0;
		for (auto& it : engine->Level->Hash.CollisionActors)
			numCollisionActors += it.second.size();
		lines.push_back(std::to_string(numCollisionActors) + " collision actors");*/

		/*lines.push_back(std::to_string(Scene.OpaqueNodes.size() + Scene.TranslucentNodes.size()) + " visible surfaces");
		lines.push_back(std::to_string(Scene.Actors.size()) + " visible actors");
		lines.push_back(std::to_string(Scene.Coronas.size()) + " visible coronas");

		lines.push_back(std::to_string(Scene.Clipper.numDrawSpans) + " spans");
		lines.push_back(std::to_string(Scene.Clipper.numSurfs) + " checked surfaces");
		lines.push_back(std::to_string(Scene.Clipper.numTris) + " checked triangles");*/

		UFont* font = engine->canvas->MedFont();
		if (font)
		{
			float curY = 180;
			for (const std::string& text : lines)
			{
				float curX = engine->viewport->ViewportWidth() / (float)Canvas.uiscale - GetTextSize(font, text).x - 16;
				float curXL = 0.0f;
				float curYL = 0.0f;
				DrawText(font, vec4(1.0f), 0.0f, 0.0f, curX, curY, curXL, curYL, false, text, PF_NoSmooth | PF_Masked, false);
				curY += curYL;
			}
		}
	}
}

void RenderSubsystem::DrawCollisionDebug()
{
	Array<std::string> lines;
	if (engine->PlayerBspNode)
	{
		BspNode* node = engine->PlayerBspNode;
		vec3& normal = engine->PlayerHitNormal;
		vec3& location = engine->PlayerHitLocation;
		BspSurface* surf = (node->Surf >= 0) ? &engine->Level->Model->Surfaces[node->Surf] : nullptr;

		lines.push_back("BspNode CollisionBound: " + std::to_string(node->CollisionBound));
		lines.push_back("BspNode Surface: " + std::to_string(node->Surf));

		if (surf && surf->Material)
			lines.push_back("BspNode Texture: " + surf->Material->Name.ToString());

		lines.push_back("BspNode Plane: (" +
			std::to_string(node->PlaneX) + ", " +
			std::to_string(node->PlaneY) + ", " +
			std::to_string(node->PlaneZ) + ", " +
			std::to_string(node->PlaneW) + ")"
		);

		BBox box = node->GetCollisionBox(engine->Level->Model);
		lines.push_back("BspNode Bound Min: (" +
			std::to_string(box.min.x) + ", " +
			std::to_string(box.min.y) + ", " +
			std::to_string(box.min.z) + ")"
		);

		lines.push_back("BspNode Bound Max: (" +
			std::to_string(box.max.x) + ", " +
			std::to_string(box.max.y) + ", " +
			std::to_string(box.max.z) + ")"
		);

		lines.push_back("HitNormal: (" +
			std::to_string(normal.x) + ", " +
			std::to_string(normal.y) + ", " +
			std::to_string(normal.z) + ")"
		);

		lines.push_back("HitLocation: (" +
			std::to_string(location.x) + ", " +
			std::to_string(location.y) + ", " +
			std::to_string(location.z) + ")"
		);
	}

	UFont* font = engine->canvas->MedFont();
	if (font)
	{
		float curY = 180;
		for (const std::string& text : lines)
		{
			float curX = engine->viewport->ViewportWidth() / (float)Canvas.uiscale - GetTextSize(font, text).x - 16;
			float curXL = 0.0f;
			float curYL = 0.0f;
			DrawText(font, vec4(1.0f), 0.0f, 0.0f, curX, curY, curXL, curYL, false, text, PF_NoSmooth | PF_Masked, false);
			curY += curYL;
		}
	}
}

void RenderSubsystem::SetTile3DOffset(bool enabled, std::optional<vec3> offset, std::optional<Rotator> rotOffset, std::optional<bool> bFlatZ, std::optional<float> Scale, std::optional<bool> bWorldOffset)
{
	Tile3DOffset.Enabled = enabled;

	if (enabled)
	{
		LogUnimplemented("Canvas.SetTile3DOffset() [U227] - Enabling 3D offset is not implemented. Options will have no effect.");
		if (offset)
			Tile3DOffset.Offset = *offset;
		if (rotOffset)
			Tile3DOffset.RotOffset = *rotOffset;
		if (bFlatZ)
			Tile3DOffset.FlatZ = *bFlatZ;
		if (Scale)
			Tile3DOffset.Scale = *Scale;
		if (bWorldOffset)
			Tile3DOffset.bWorldOffset = *bWorldOffset;
	}
	else
	{
		// Should these be reset?
		Tile3DOffset.Offset = vec3();
		Tile3DOffset.RotOffset = Rotator();
		Tile3DOffset.FlatZ = false;
		Tile3DOffset.bWorldOffset = false;
		Tile3DOffset.Scale = 1.0f;
	}
}

// ---------------------------------------------------------------------------
// In-engine on-screen keyboard (VR). Drawn into the menu texture (logical canvas
// ~800x600, matching the cursor units RunVRMenuScreen computes) and hit-tested
// against the same controller-ray cursor.
// ---------------------------------------------------------------------------
namespace
{
	struct VRKey { float x, y, w, h; std::string label; int type; char ch; }; // type: 0 char, 1 backspace, 2 enter, 3 shift, 4 space

	std::vector<VRKey> BuildVRKeyboard(bool shift)
	{
		std::vector<VRKey> keys;
		const float left = 16.0f, top = 356.0f, kw = 74.0f, kh = 44.0f, gap = 4.0f;
		const char* rows[4] = { "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm" };
		const float rowIndent[4] = { 0.0f, 0.0f, kw * 0.5f, kw * 1.0f };
		for (int r = 0; r < 4; r++)
		{
			float y = top + r * (kh + gap);
			float x = left + rowIndent[r];
			for (const char* p = rows[r]; *p; ++p)
			{
				char c = *p;
				if (shift && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
				keys.push_back({ x, y, kw, kh, std::string(1, c), 0, c });
				x += kw + gap;
			}
		}
		float y = top + 4 * (kh + gap);
		float x = left;
		keys.push_back({ x, y, kw * 1.5f, kh, "Shift", 3, 0 }); x += kw * 1.5f + gap;
		keys.push_back({ x, y, kw * 4.0f, kh, "Space", 4, ' ' }); x += kw * 4.0f + gap;
		keys.push_back({ x, y, kw * 1.5f, kh, "Del", 1, 0 });     x += kw * 1.5f + gap;
		keys.push_back({ x, y, kw * 2.0f, kh, "Enter", 2, 0 });
		return keys;
	}
}

void RenderSubsystem::DrawVRKeyboard(float cursorX, float cursorY)
{
	UFont* font = engine->canvas ? engine->canvas->MedFont() : nullptr;
	const vec4 outline(0.60f, 0.60f, 0.70f, 1.0f);

	// Dark backdrop band behind the keys, filled with horizontal lines (no solid-fill primitive
	// on the 2D canvas). Covers the lower part of the menu while the keyboard is open.
	for (float yy = 348.0f; yy <= 598.0f; yy += 1.0f)
		Draw2DLine(vec4(0.04f, 0.04f, 0.07f, 1.0f), 0, vec3(8.0f, yy, 1.0f), vec3(792.0f, yy, 1.0f), true);

	for (const VRKey& k : BuildVRKeyboard(vrKeyboardShift))
	{
		bool hover = cursorX >= k.x && cursorX <= k.x + k.w && cursorY >= k.y && cursorY <= k.y + k.h;
		bool lit = hover || (k.type == 3 && vrKeyboardShift);
		vec4 fill = lit ? vec4(0.32f, 0.44f, 0.64f, 1.0f) : vec4(0.17f, 0.17f, 0.21f, 1.0f);
		for (float yy = k.y + 1.0f; yy < k.y + k.h; yy += 1.0f)
			Draw2DLine(fill, 0, vec3(k.x + 1.0f, yy, 1.0f), vec3(k.x + k.w - 1.0f, yy, 1.0f), true);
		Draw2DLine(outline, 0, vec3(k.x, k.y, 1.0f), vec3(k.x + k.w, k.y, 1.0f), true);
		Draw2DLine(outline, 0, vec3(k.x, k.y + k.h, 1.0f), vec3(k.x + k.w, k.y + k.h, 1.0f), true);
		Draw2DLine(outline, 0, vec3(k.x, k.y, 1.0f), vec3(k.x, k.y + k.h, 1.0f), true);
		Draw2DLine(outline, 0, vec3(k.x + k.w, k.y, 1.0f), vec3(k.x + k.w, k.y + k.h, 1.0f), true);
		if (font)
		{
			vec2 sz = GetTextSize(font, k.label);
			float cx = k.x + (k.w - sz.x) * 0.5f;
			float cy = k.y + (k.h - sz.y) * 0.5f;
			float cxl = 0.0f, cyl = 0.0f;
			DrawText(font, vec4(1.0f), 0.0f, 0.0f, cx, cy, cxl, cyl, false, k.label, PF_NoSmooth | PF_Masked, false);
		}
	}
}

RenderSubsystem::VRKeyHit RenderSubsystem::VRKeyboardHitTest(float cursorX, float cursorY) const
{
	VRKeyHit hit;
	for (const VRKey& k : BuildVRKeyboard(vrKeyboardShift))
	{
		if (cursorX >= k.x && cursorX <= k.x + k.w && cursorY >= k.y && cursorY <= k.y + k.h)
		{
			hit.hit = true;
			if (k.type == 0 || k.type == 4) hit.ch = k.ch;
			else if (k.type == 1) hit.backspace = true;
			else if (k.type == 2) hit.enter = true;
			else if (k.type == 3) hit.shift = true;
			return hit;
		}
	}
	return hit;
}
