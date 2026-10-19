
#include "Precomp.h"
#include <cctype>
#include "Engine.h"
#include "Utils/File.h"
#include "Utils/StrTools.h"
#include "Utils/SHA1Sum.h"
#include "Render/RenderSubsystem.h"
#include "Package/PackageManager.h"
#include "Package/ObjectStream.h"
#include "Packages/Core/UClass.h"
#include "Packages/Core/UFunction.h"
#include "Packages/Core/USubsystem.h"
#include "Packages/Core/Properties/UObjectProperty.h"
#include "Packages/Core/Properties/UStringProperty.h"
#include "Packages/Core/Properties/UFloatProperty.h"
#include "Packages/Core/Properties/UIntProperty.h"
#include "Packages/Engine/UClient.h"
#include "Packages/Engine/UConsole.h"
#include "Packages/Engine/UViewport.h"
#include "Packages/Engine/UCanvas.h"
#include "Packages/Engine/Actors/UActor.h"
#include "Packages/Engine/Actors/Pawn/UPlayerPawn.h"
#include "Packages/Engine/Actors/UHUD.h"
#include "Packages/Engine/Actors/Info/ULevelInfo.h"
#include "Packages/Engine/Actors/Info/UGameInfo.h"
#include "Packages/Engine/Actors/Info/UZoneInfo.h"
#include "Packages/Engine/Actors/Info/UPlayerReplicationInfo.h"
#include "Packages/Engine/Resources/Level/ULevel.h"
#include "Packages/Engine/Resources/Level/UModel.h"
#include "Packages/Engine/Resources/UFont.h"
#include "Packages/Engine/Resources/Mesh/USkeletalMesh.h"
#include "Packages/Engine/Resources/Textures/UTexture.h"
#include "Packages/Engine/Resources/UMusic.h"
#include "Packages/Engine/Resources/USound.h"
#include "Packages/Engine/USurrealClient.h"
#include "Packages/Engine/Subsystems/UGameEngine.h"
#include "Packages/Engine/Subsystems/USurrealRenderDevice.h"
#include "Packages/Engine/Subsystems/USurrealAudioDevice.h"
#include "Packages/Engine/Subsystems/USurrealNetworkDevice.h"
#include "Packages/Extension/UPlayerPawnExt.h"
#include "Packages/Extension/Flags/UFlag.h"
#include "Packages/Extension/Flags/UFlagBase.h"
#include "Packages/Extension/Windows/UGC.h"
#include "Packages/Extension/Windows/TabGroup/URootWindow.h"
#include "Packages/ConSys/UConItem.h"
#include "Packages/ConSys/UConversation.h"
#include "Packages/ConSys/UConversationList.h"
#include "Packages/ConSys/UConversationMissionList.h"
#include "Packages/ConSys/Events/UConEvent.h"
#include "Packages/ConSys/Events/UConEventTransferObject.h"
#include "Packages/ConSys/Events/UConEventCheckObject.h"
#include "Packages/DeusEx/UDeusExLevelInfo.h"
#include "Packages/DeusEx/UDeusExSaveInfo.h"
#include "ObjectTravelInfo.h"
#include "Math/quaternion.h"
#include "Math/FrustumPlanes.h"
#include "GameWindow.h"
#include "RenderDevice/RenderDevice.h"
#include "VR/VRInput.h"
#include "VR/VRCamera.h"

// Temporarily points the player Pawn's Rotation AND ViewRotation at the VR crosshair for the
// scope's lifetime, restoring both afterwards. UT's PlayerPawn.AdjustAim - which every weapon's
// fire path goes through - reads ViewRotation (not Rotation), and UT spawns shots at the EYE,
// so the aim is taken from the head straight at the crosshair's hit point
// (RenderSubsystem::UpdateVRAimPoint) rather than copying the controller direction: shots land
// on the crosshair at any range. Used around Level->Tick (weapon state code firing on bFire) and
// around the synthesized fire key press (UT's `exec function Fire()` fires synchronously).
struct VRAimOverrideScope
{
	VRAimOverrideScope(Engine* engine, bool active)
	{
		if (!active || !engine->vrInput || !engine->vrInput->RightControllerActive)
			return;
		pawn = UObject::TryCast<UPawn>(engine->viewport->Actor());
		if (!pawn)
			return;
		// Converge from the game's ACTUAL fire origin: CameraLocation includes the VR head raise
		// (VRCamera::HeadHeightOffsetMeters, Engine::RunVR), but UnrealScript fires from the
		// unraised pawn eye. Converging from the raised point tilted every shot slightly downward
		// (observed on Quest 3: impacts consistently landed slightly below the barrel).
		vec3 fireOrigin = engine->CameraLocation - vec3(0.0f, 0.0f, VRCamera::HeadHeightOffsetMeters * VRCamera::UnitsPerMeter);
		Rotator aim = engine->render->VRAim.Valid ? Rotator::FromVector(engine->render->VRAim.HitPoint - fireOrigin) : engine->vrInput->GetAimRotation();
		aim.Roll = 0;
		savedRotation = pawn->Rotation();
		savedViewRotation = pawn->ViewRotation();
		pawn->Rotation() = aim;
		pawn->ViewRotation() = aim;
	}

	~VRAimOverrideScope()
	{
		if (pawn)
		{
			pawn->Rotation() = savedRotation;
			pawn->ViewRotation() = savedViewRotation;
		}
	}

	UPawn* pawn = nullptr;
	Rotator savedRotation;
	Rotator savedViewRotation;
};
#ifdef ANDROID
#include <android/log.h>
#endif
#include "VM/Frame.h"
#include "VM/ScriptCall.h"
#include "Video/VideoPlayer.h"
#include "Utils/Convert.h"
#include <chrono>
#include <set>

Engine* engine = nullptr;

Engine::Engine(GameLaunchInfo launchinfo) : LaunchInfo(launchinfo)
{
	engine = this;

	packages = std::make_unique<PackageManager>(LaunchInfo);

	std::srand((unsigned int)std::time(nullptr));

	auto transientpkg = packages->GetTransientPackage();
	auto enginepkg = packages->GetPackage("Engine");
	gameengine = UObject::Cast<UGameEngine>(transientpkg->NewObject("gameengine", enginepkg->GetClass("GameEngine"), ObjectFlags::Transient));
	audiodev = UObject::Cast<USurrealAudioDevice>(transientpkg->NewObject("audiodev", enginepkg->GetClass("SurrealAudioDevice"), ObjectFlags::Transient));
	renderdev = UObject::Cast<USurrealRenderDevice>(transientpkg->NewObject("renderdev", enginepkg->GetClass("SurrealRenderDevice"), ObjectFlags::Transient));
	netdev = UObject::Cast<USurrealNetworkDevice>(transientpkg->NewObject("netdev", enginepkg->GetClass("SurrealNetworkDevice"), ObjectFlags::Transient));
	client = UObject::Cast<USurrealClient>(transientpkg->NewObject("client", enginepkg->GetClass("SurrealClient"), ObjectFlags::Transient));
	viewport = UObject::Cast<UViewport>(transientpkg->NewObject("viewport", enginepkg->GetClass("Viewport"), ObjectFlags::Transient));
	canvas = UObject::Cast<UCanvas>(transientpkg->NewObject("canvas", enginepkg->GetClass("Canvas"), ObjectFlags::Transient));
	DefaultTexture = UObject::Cast<UTexture>(packages->GetPackage("Engine")->GetUObject("Texture", "DefaultTexture"));

	floatprop = GC::Alloc<UFloatProperty>(NameString(), nullptr, ObjectFlags::NoFlags);

	if (LaunchInfo.IsDeusEx())
	{
		auto extpkg = packages->GetPackage("Extension");
		deusExPackage = packages->GetPackage("DeusEx");
		dxgc = UObject::Cast<UGC>(transientpkg->NewObject("gc", extpkg->GetClass("GC"), ObjectFlags::Transient));
		dxgc->Canvas() = canvas;
		dxSaveInfo = UObject::Cast<UDXSaveInfo>(transientpkg->NewObject("DeusExSaveInfo", deusExPackage->GetClass("DeusExSaveInfo"), ObjectFlags::Transient));
		dxConMissionList = UObject::Cast<UConversationMissionList>(packages->GetPackage("DeusExConText")->GetUObject("ConversationMissionList", "ConMissionList"));
	}

	std::string consolestr = packages->GetIniValue("system", "Engine.Engine", "Console");
	std::string consolepkg = consolestr.substr(0, consolestr.find('.'));
	std::string consolecls = consolestr.substr(consolestr.find('.') + 1);
	console = UObject::Cast<UConsole>(transientpkg->NewObject("console", packages->GetPackage(consolepkg)->GetClass(consolecls), ObjectFlags::Transient));

	console->Viewport() = viewport;
	canvas->Viewport() = viewport;
	viewport->Console() = console;
}

Engine::~Engine()
{
	if (audiodev)
		audiodev->ShutdownDevice();

	// Best effort: on Android the .config folder may not exist yet, and a throw here (from a
	// destructor already unwinding an engine error) turned a logged script error into a hard
	// abort with no message.
	try
	{
		Logger::Get()->SaveLogAsPlaintext((Directory::localAppData() / "SurrealEngine/SE-Log-LastRun.txt").string());
	}
	catch (...)
	{
	}

	engine = nullptr;
}

void Engine::Run()
{
	LogMessage("Game: " + LaunchInfo.gameName + " (Version: " + LaunchInfo.gameVersionString + ")");
	LoadEngineSettings();
	LogMessage("Loaded Engine settings");
	LoadKeybindings();
	LogMessage("Loaded key bindings");
	LogGamePackageSHA1Sums();

	OpenWindow();

	if (window->GetRenderDevice()->Viewport->IsStereoDisplay())
	{
		vrInput = std::make_unique<VRInput>();
		// No CreateScreenLayerSwapchain() any more - the menu "big screen" is now drawn as
		// world-space geometry inside the normal per-eye render (RunVRMenuScreen()), not as a
		// separate OpenXR quad layer.
	}

	audiodev->InitDevice();
	render = std::make_unique<RenderSubsystem>(window->GetRenderDevice());

	// VR always: the menu cursor is fed as ABSOLUTE positions (WindowsMouseX/Y from the
	// controller-ray/panel intersection in RunVRMenuScreen()), and UT's UWindow system only
	// reads those when bWindowsMouseAvailable is set - otherwise it moves its cursor by relative
	// mouse deltas that VR never produces (real Quest 3: "menu works but the mouse pointer is
	// not working"; the Quest session counts as fullscreen, so the desktop rule skipped it).
	if (engine->LaunchInfo.ue1Version > 219 && (!client->StartupFullscreen || vrInput))
		viewport->bWindowsMouseAvailable() = true;

	window->LockCursor();

	if (packages->IsKlingonHonorGuard())
	{
		PlayAVI({ "playavi", "INTRO.AVI", "N" });
	}

	if (!LaunchInfo.noEntryMap)
		LoadEntryMap();

	if (LaunchInfo.url.empty())
		LoadMap(GetDefaultURL(packages->GetIniValue("system", "URL", "LocalMap")));
	else if (UnrealURL(LaunchInfo.url).HasOption("load"))
	{
		// --url=?load=N: start on the default map, then let the run loop's deferred travel handling
		// load save slot N exactly like the in-game Load menu does (used to reproduce save/load
		// problems from the headset on the desktop build).
		LoadMap(GetDefaultURL(packages->GetIniValue("system", "URL", "LocalMap")));
		ClientTravelInfo.URL = UnrealURL(LaunchInfo.url);
	}
	else
		LoadMap(UnrealURL(GetDefaultURL(packages->GetIniValue("system", "URL", "LocalMap")), LaunchInfo.url));

	LoginPlayer();

	auto objprop = GC::Alloc<UObjectProperty>(NameString(), nullptr, ObjectFlags::NoFlags);
	auto vecprop = GC::Alloc<UStructProperty>(NameString(), nullptr, ObjectFlags::NoFlags);
	auto rotprop = GC::Alloc<UStructProperty>(NameString(), nullptr, ObjectFlags::NoFlags);

	bool firstCall = true;

	float currentZoneTimeDilation = 1.0f; // Unreal 227 allows zones to specify their own time dilations

	while (!quit)
	{
		// Main game loop should consist of these 4 steps:
		// Tick everything
		// Render the scene
		// Check if there is a request to save the game: save the game if that's the case
		// Check if there is a new map to load (next level, saved game etc.): load it if that's the case

		// Tick everything
		if (LaunchInfo.IsUnreal1_227())
			currentZoneTimeDilation = viewport->Actor()->PlayerReplicationInfo()->PlayerZone()->ZoneTimeDilation();

		float realTimeElapsed = CalcTimeElapsed();
		float entryLevelElapsed = EntryLevel ? realTimeElapsed * clamp(EntryLevelInfo->TimeDilation() * currentZoneTimeDilation, 0.0025f, 25.0f) : 0.0f;
		float levelElapsed = realTimeElapsed * clamp(LevelInfo->TimeDilation() * currentZoneTimeDilation, 0.0025f, 25.0f);

		TotalTime += realTimeElapsed;

		if (EntryLevel)
			EntryLevelInfo->TimeSeconds() += entryLevelElapsed;
		LevelInfo->TimeSeconds() += levelElapsed;
		Logger::Get()->SetTimeSeconds(LevelInfo->TimeSeconds());

		// Update the time fields
		std::time_t now = std::time(nullptr);
		std::tm* timedesc = std::localtime(&now);

		LevelInfo->Year() = timedesc->tm_year;
		LevelInfo->Month() = timedesc->tm_mon;
		LevelInfo->Day() = timedesc->tm_mday;
		LevelInfo->DayOfWeek() = timedesc->tm_wday;
		LevelInfo->Hour() = timedesc->tm_hour;
		LevelInfo->Minute() = timedesc->tm_min;
		LevelInfo->Second() = timedesc->tm_sec;
		LevelInfo->Millisecond() = 0; // No timedesc equivalent for LevelInfo->Millisecond()

		// VR: refresh controller/head state and push it onto the Pawn (movement axes, fire
		// buttons, body-facing Rotation()) BEFORE Level->Tick() runs this frame's
		// movement/physics - the previous ordering called vrInput->Update() only after Tick()
		// had already run (inside the PlayerCalcView block below, using stale data from last
		// frame at best), which is why neither controller input nor movement worked at all on
		// real hardware. Unconditional here (not gated on PlayerCalcView existing) so VR state
		// stays fresh regardless of which UE1 game/version is running.
		if (vrInput)
		{
			vrInput->Update(window.get(), realTimeElapsed);
			UpdateVRInput(realTimeElapsed);
		}

		UpdateInput(realTimeElapsed);

		SetPause(!LevelInfo->Pauser().empty());

		// Do NOT pause this Tick event otherwise some messages will stay on screen forever.
		CallEvent(console, EventName::Tick, { ExpressionValue::FloatValue(levelElapsed) });

		// To do: set these to true if the frame rate is too low
		if (LaunchInfo.ue1Version >= 436)
		{
			LevelInfo->bDropDetail() = false;
			LevelInfo->bAggressiveLOD() = false;
		}

		// VR: temporarily override the Pawn's body Rotation() with the right controller's aim
		// direction for the duration of this tick, restoring it immediately after. SurrealEngine
		// has no native "weapon fire direction" function to hook (that logic is compiled
		// UnrealScript reading Owner.Rotation - see VR/VRInput.h's ApplyAimOverride comment), so
		// this is the only integration point available without patching game bytecode.
		// KNOWN RISK (flagged, unverified on real hardware): movement/physics code also reads
		// Rotation() every tick, so movement direction will follow aim direction during the
		// tick(s) the trigger is held, not just fire itself. Scoped to only-while-firing
		// (UPawn::bFire()) to bound the effect, but this needs real-device playtesting to judge
		// whether it feels acceptable or needs a more surgical hook (e.g. patching the specific
		// UnrealScript AdjustAim/fire-trace path per weapon class instead).
		//
		// Two refinements from Quest 3 testing: (1) UT's PlayerPawn.AdjustAim - which every
		// weapon's fire path goes through - reads ViewRotation, NOT Rotation, so ViewRotation
		// (yaw AND pitch) must be
		// overridden too; overriding Rotation alone left shots going along the body yaw at
		// zero pitch. (2) UT spawns projectiles/traces at the EYE (Location + draw offset),
		// while the crosshair sits on the HAND's ray (RenderSubsystem::UpdateVRAimPoint) - a
		// parallel ray from the eye misses that point by the eye-to-hand offset. So aim from
		// the head straight AT the crosshair's hit point instead of copying the controller's
		// direction: shots then land on the crosshair exactly, at any range.
		//
		// The same override is applied around the synthesized fire KEY PRESS in UpdateVRInput():
		// UT's `exec function Fire()` calls Weapon.Fire() synchronously from the key event, i.e.
		// outside this tick - so without that, the first shot of every trigger pull (every shot,
		// for single-fire weapons) went along the body yaw at zero pitch (real Quest 3: "shots go
		// 45 degrees left" after snap-turns, while the crosshair and gun were right).
		{
			UPawn* pawn = UObject::TryCast<UPawn>(viewport->Actor());
			bool firing = pawn && (pawn->bFire() || pawn->bAltFire() || vrAimOverrideAfterRelease > 0.0f);
			VRAimOverrideScope aimOverride(this, firing);

			if (EntryLevel)
				EntryLevel->Tick(entryLevelElapsed, m_GamePaused);
			Level->Tick(levelElapsed, m_GamePaused);
		}

		if (dxRootWindow)
			dxRootWindow->Tick(levelElapsed); // Should this maybe be realTimeElapsed?

		// To do: improve CallEvent so parameter passing isn't this painful
		UFunction* funcPlayerCalcView = viewport->Actor() ? FindEventFunction(viewport->Actor(), "PlayerCalcView") : nullptr;
		if (funcPlayerCalcView)
		{
			vecprop->Struct = UObject::Cast<UStructProperty>(funcPlayerCalcView->Properties[1])->Struct;
			rotprop->Struct = UObject::Cast<UStructProperty>(funcPlayerCalcView->Properties[2])->Struct;
			CameraActor = viewport->Actor();
			CameraLocation = viewport->Actor()->Location();
			CameraRotation = viewport->Actor()->Rotation();
			CameraFovAngle = viewport->Actor()->FovAngle();
			CallEvent(viewport->Actor(), EventName::PlayerCalcView, {
				ExpressionValue::Variable(&CameraActor, objprop),
				ExpressionValue::Variable(&CameraLocation, vecprop),
				ExpressionValue::Variable(&CameraRotation, rotprop)
				});

			// VR: head tracking overrides the script-computed camera rotation only, leaving
			// viewport->Actor()->Rotation() (the Pawn's body-facing rotation, which drives
			// movement/physics/weapon-aim script logic) untouched - see VR/VRInput.h and
			// VR/VRCamera.h for the full head/body/aim split this implements.
			if (vrInput)
			{
				CameraRotation.Yaw = (int)(vrInput->BodyYawRadians * (0x10000 / (2.0f * 3.14159265359f)));
			}
		}

		UpdateAudio();

		viewport->SetViewportRect(0, 0, engine->window->GetPixelWidth(), engine->window->GetPixelHeight());

		if (vrInput)
			RunVR(levelElapsed);
		else
			render->DrawGame(levelElapsed);

		// Save the game if there is a request for it
		if (SaveGameInfo.SaveGameSlot != DONT_SAVE_GAME)
		{
			SaveGameToSlot(SaveGameInfo.SaveGameSlot, SaveGameInfo.SaveGameDescription);

			SaveGameInfo.SaveGameSlot = DONT_SAVE_GAME;
			SaveGameInfo.SaveGameDescription.clear();
		}

		// Check if there is a new map to load
		if (!LevelInfo->NextURL().empty())
		{
			LevelInfo->NextSwitchCountdown() -= levelElapsed;
			if (LevelInfo->NextSwitchCountdown() <= 0.0f)
			{
				// LoginPlayer only transfers travel actors when ClientTravelInfo.TravelType is
				// TRAVEL_Relative, and TravelType is otherwise assigned only in ClientTravel. These
				// NextURL routes never go through ClientTravel, so without setting it here they
				// inherit whatever the last ClientTravel left behind (or TRAVEL_Absolute, the
				// default) - which silently discarded the inventory the bNextItems branch exists
				// specifically to carry. State the intent explicitly on each branch instead of
				// depending on leftover state.
				if (UnrealURL(LevelInfo->NextURL()).HasOption("restart"))
				{
					// Passes the level's own TravelInfo back in, so it means to preserve it.
					ClientTravelInfo.TravelType = ETravelType::TRAVEL_Relative;
					LoadMap(LevelInfo->URL, Level->TravelInfo);
					LoginPlayer();
				}
				else if (LevelInfo->bNextItems())
				{
					ClientTravelInfo.TravelType = ETravelType::TRAVEL_Relative;
					LoadMap(UnrealURL(LevelInfo->URL, LevelInfo->NextURL()), CreateTravelInfo(true));
					LoginPlayer();
				}
				else
				{
					// Deliberately carries nothing - it passes no travel info at all.
					ClientTravelInfo.TravelType = ETravelType::TRAVEL_Absolute;
					LoadMap(UnrealURL(LevelInfo->URL, LevelInfo->NextURL()), {});
					LoginPlayer();
				}
			}
		}

		if (ClientTravelInfo.URL.HasOption("restart"))
		{
			LoadMap(LevelInfo->URL, Level->TravelInfo);
			LoginPlayer();
		}

		if (ClientTravelInfo.URL.HasOption("load"))
		{
			UnrealURL url(ClientTravelInfo.URL);
			LoadFromSaveFile(url);
			PossessSavedPlayer();
		}

		if (!ClientTravelInfo.URL.Map.empty())
		{
			// To do: need to do something about that travel type and transfering of items

			UnrealURL url(ClientTravelInfo.URL);
			LogMessage("Client travel to " + url.ToString());
			LoadMap(url, CreateTravelInfo(ClientTravelInfo.TransferItems));
			LoginPlayer();
		}
	}

	LogMessage("Shutting down...");
	window->UnlockCursor();

	LogMessage("Saving configurations...");
	if (packages->MissingSESystemIni())
	{
		// Add the missing Subsystem entries
		client->SaveConfig();
		audiodev->SaveConfig();
		renderdev->SaveConfig();
	}
	packages->SetIniValue("System", "Engine.SurrealWindowSystem", "WindowSystem", windowingSystemName);
	packages->SaveAllIniFiles();

	LogMessage("Closing window...");
	CloseWindow();
}

void Engine::PlayAVI(const Array<std::string>& args)
{
	if (args.size() < 3)
		return;

	// What did KHG request?
	std::string buildup, breakdown, video;
	if (args.size() > 4 && args[2] == "C")
	{
		buildup = args[1];
		video = args[3];
		breakdown = "breakdn.avi";
	}
	else if (args.size() > 3 && args[2] == "Y")
	{
		buildup = "buildup.avi";
		video = args[1];
		breakdown = "breakdn.avi";
	}
	else
	{
		video = args[1];
	}

	playingAvi = true;
	skipAvi = false;

	try
	{
		// Load the videos

		std::unique_ptr<VideoPlayer> buildupPlayer, videoPlayer, breakdownPlayer;
		if (!buildup.empty())
			buildupPlayer = VideoPlayer::Create(packages->GetVideoFilename(buildup));
		if (!video.empty())
			videoPlayer = VideoPlayer::Create(packages->GetVideoFilename(video));
		if (!breakdown.empty())
			breakdownPlayer = VideoPlayer::Create(packages->GetVideoFilename(breakdown));

		CalcTimeElapsed(); // Reset so load time doesn't affect playback

		UnrealMipmap* background = nullptr;
		if (buildupPlayer)
		{
			background = PlayVideo(buildupPlayer.get(), nullptr);
			if (!background)
			{
				CalcTimeElapsed();
				return;
			}

			// Cut a hole in the background image
			uint32_t* pixels = (uint32_t*)background->Data.data();
			int w = background->Width;
			int h = background->Height;

			ivec2 v0 = { 232, 125 };
			ivec2 v1 = { 161, 246 };
			ivec2 v2 = { 406, 125 };
			ivec2 v3 = { 475, 246 };
			ivec2 v4 = { 258, 125 };
			ivec2 v5 = { 270, 144 };
			ivec2 v6 = { 380, 125 };
			ivec2 v7 = { 368, 144 };
			ivec2 v8 = { 210, 327 };
			ivec2 v9 = { 427, 327 };

			for (int y = 126; y < 144; y++)
			{
				int x0 = (int)(v0.x + 0.5f + (y - v0.y + 0.5f) * (v1.x - v0.x) / (v1.y - v0.y));
				int x1 = (int)(v4.x + 0.5f + (y - v4.y + 0.5f) * (v5.x - v4.x) / (v5.y - v4.y));
				int x2 = (int)(v6.x + 0.5f + (y - v6.y + 0.5f) * (v7.x - v6.x) / (v7.y - v6.y));
				int x3 = (int)(v2.x + 0.5f + (y - v2.y + 0.5f) * (v3.x - v2.x) / (v3.y - v2.y));

				pixels[x0 + y * w] = 0x80000000;
				pixels[x1 - 1 + y * w] = 0x80000000;
				for (int x = x0 + 1; x < x1 - 1; x++)
					pixels[x + y * w] = 0;

				pixels[x2 + y * w] = 0x80000000;
				pixels[x3 - 1 + y * w] = 0x80000000;
				for (int x = x2 + 1; x < x3 - 1; x++)
					pixels[x + y * w] = 0;
			}

			for (int y = 144; y < 246; y++)
			{
				int x0 = (int)(v0.x + 0.5f + (y - v0.y + 0.5f) * (v1.x - v0.x) / (v1.y - v0.y));
				int x1 = (int)(v2.x + 0.5f + (y - v2.y + 0.5f) * (v3.x - v2.x) / (v3.y - v2.y));
				pixels[x0 + y * w] = 0x80000000;
				pixels[x1 - 1 + y * w] = 0x80000000;
				for (int x = x0 + 1; x < x1 - 1; x++)
					pixels[x + y * w] = 0;
			}

			for (int y = 246; y < 325; y++)
			{
				int x0 = (int)(v1.x + 0.5f + (y - v1.y + 0.5f) * (v8.x - v1.x) / (v8.y - v1.y));
				int x1 = (int)(v3.x + 0.5f + (y - v3.y + 0.5f) * (v9.x - v3.x) / (v9.y - v3.y));
				pixels[x0 + y * w] = 0x80000000;
				pixels[x1 - 1 + y * w] = 0x80000000;
				for (int x = x0 + 1; x < x1 - 1; x++)
					pixels[x + y * w] = 0;
			}
		}

		if (videoPlayer)
		{
			if (!PlayVideo(videoPlayer.get(), background))
			{
				playingAvi = false;
				CalcTimeElapsed();
				return;
			}
		}

		if (breakdownPlayer)
		{
			PlayVideo(breakdownPlayer.get(), nullptr);
		}
	}
	catch (const std::exception& e)
	{
		LogMessage("Error playing " + video + ": " + e.what());
	}

	playingAvi = false;
	CalcTimeElapsed(); // Reset so game isn't affected
}

UnrealMipmap* Engine::PlayVideo(VideoPlayer* video, UnrealMipmap* background)
{
	UnrealMipmap* frame = nullptr;

	TextureInfo texinfo[2];
	texinfo[0].CacheID = 0xffffffff'ffffffffULL;
	texinfo[0].Format = TextureFormat::BGRA8;
	texinfo[0].NumMips = 1;

	if (background)
	{
		texinfo[1].CacheID = 0xffffffff'fffffffeULL;
		texinfo[1].Format = TextureFormat::BGRA8;
		texinfo[1].NumMips = 1;
		texinfo[1].Mips = background;
		texinfo[1].USize = background->Width;
		texinfo[1].VSize = background->Height;
		texinfo[1].bRealtimeChanged = true;
	}

	audiodev->SetViewport(nullptr);
	audiodev->GetDevice()->PlayMusic(video->GetAudio());

	float timestamp = 0.0f;
	int curframe = -1;
	while (!quit && !skipAvi)
	{
		timestamp += CalcTimeElapsed();

		bool done = false;
		while (curframe < video->GetFrameIndexForTime(timestamp))
		{
			while (true)
			{
				UnrealMipmap* nextframe = video->NextVideoFrame();
				if (nextframe)
				{
					frame = nextframe;
					curframe++;
					texinfo[0].bRealtimeChanged = true;
					break;
				}
				if (!video->Decode())
				{
					done = true;
					break;
				}
			}
			if (done)
				break;
		}
		if (done)
			break;

		audiodev->GetDevice()->Update();
		GameWindow::ProcessEvents();

		if (frame)
		{
			texinfo[0].Mips = frame;
			texinfo[0].USize = frame->Width;
			texinfo[0].VSize = frame->Height;

			viewport->SetViewportRect(0, 0, engine->window->GetPixelWidth(), engine->window->GetPixelHeight());
			render->DrawVideoFrame(&texinfo[0], background ? &texinfo[1] : nullptr);
		}
	}

	audiodev->GetDevice()->PlayMusic(nullptr);

	if (quit || skipAvi)
		return nullptr;

	return frame;
}

UConversationList* Engine::GetDeusExMission()
{
	if (!dxConMissionList || !DeusExLevelInfo)
		return nullptr;

	int missionNumber = DeusExLevelInfo->MissionNumber();
	for (UConItem* item = dxConMissionList->missions(); item; item = item->Next())
	{
		auto mission = UObject::Cast<UConversationList>(item->ConObject());
		if (mission->missionNumber() == missionNumber)
		{
			return mission;
		}
	}
	return nullptr;
}

void Engine::UpdateAudio()
{
	// VR note: the audio listener currently only follows body yaw (CameraRotation.Yaw is
	// overridden in Run()'s VR branch), not full head orientation/position - turning your
	// head in VR doesn't move the stereo audio image the way a real head would. Getting that
	// right needs CameraLocation/CameraRotation (or a separate listener transform) driven
	// from vrInput->HeadView via VRCamera the same way the render camera is; left as a
	// follow-up since it doesn't affect the render/movement/aim decoupling this port
	// otherwise implements.
	mat4 translate = mat4::translate(vec3(0.0f) - CameraLocation);
	mat4 listener = Coords::ViewToAudioDev().ToMatrix() * Coords::Rotation(CameraRotation).ToMatrix() * translate;

	audiodev->SetViewport(viewport);
	audiodev->Update(listener);
}

void Engine::ClientTravel(const std::string& newURL, ETravelType travelType, bool transferItems)
{
	UnrealURL url(newURL);

	// If the URL doesn't contain the player info, add them here.
	// As they have to persist somehow
	for (std::string optionKey : { "Name", "Class", "team", "skin", "Face", "Voice", "OverrideClass" })
	{
		if (engine->LaunchInfo.ue1Version > 219)
		{
			if (url.HasOption(optionKey))
				engine->packages->SetIniValue("User", "DefaultPlayer", optionKey, url.GetOption(optionKey));
			else
				url.AddOrReplaceOption(optionKey + "=" + packages->GetIniValue("user", "DefaultPlayer", optionKey));
		}
		else
		{
			if (url.HasOption(optionKey))
				engine->packages->SetIniValue("System", "URL", optionKey, url.GetOption(optionKey));
			else
				url.AddOrReplaceOption(optionKey + "=" + packages->GetIniValue("System", "URL", optionKey));
		}
	}

	if (travelType == ETravelType::TRAVEL_Absolute)
		ClientTravelInfo.URL = url;
	else if (travelType == ETravelType::TRAVEL_Partial)
	{
		auto name = ClientTravelInfo.URL.GetOption("name");
		ClientTravelInfo.URL = url;
		ClientTravelInfo.URL.AddOrReplaceOption("name=" + name);
	}
	else if (travelType == ETravelType::TRAVEL_Relative)
	{
		ClientTravelInfo.URL = UnrealURL(ClientTravelInfo.URL, url);
		// Add difficulty if it isn't there
		if (ClientTravelInfo.URL.GetOption("Difficulty").empty())
		{
			ClientTravelInfo.URL.AddOrReplaceOption("Difficulty=" + std::to_string(GameInfo->Difficulty()));
		}
	}

	ClientTravelInfo.TravelType = travelType;
	ClientTravelInfo.TransferItems = transferItems;
}

UnrealURL Engine::GetDefaultURL(const std::string& map)
{
	UnrealURL url;
	std::string teleporterTag = "";
	std::string finalMapName = map;

	size_t tagPos = map.find('#');

	if (tagPos != std::string::npos)
	{
		teleporterTag = map.substr(tagPos + 1);
		finalMapName = map.substr(0, tagPos);
	}
	if (map.find("." + packages->GetMapExtension()) == std::string::npos)
		url.Map = finalMapName + "." + packages->GetMapExtension();
	else
		url.Map = finalMapName;

	if (!teleporterTag.empty())
		url.Portal = teleporterTag;
	for (std::string optionKey : { "Name", "Class", "team", "skin", "Face", "Voice", "OverrideClass" })
	{
		url.Options.push_back(optionKey + "=" + packages->GetIniValue("user", "DefaultPlayer", optionKey));
	}
	return url;
}

void Engine::LoadEntryMap()
{
	// The entry map is the map you see in the game when no other map is playing. For example when disconnected from a server. It is always loaded and running.
	const auto entryMapName = packages->GetIniValue("System", "URL", "EntryMap", "Entry");
	LoadMap(GetDefaultURL(entryMapName));
	EntryLevelInfo = LevelInfo;
	EntryLevel = Level;
	EntryLevelPackage = std::move(LevelPackage);
	LevelInfo = nullptr;
	Level = nullptr;
	viewport->Actor() = nullptr;
}

void Engine::UnloadMap()
{
	if (!LevelPackage)
		return;

	LevelInfo = nullptr;
	if (packages->IsDeusEx())
		DeusExLevelInfo = nullptr;
	Level = nullptr;
	viewport->Actor() = nullptr;
	dxRootWindow = nullptr;
	packages->UnloadPackage(std::move(LevelPackage));

	// GC::Collect();
}

void Engine::LoadMap(const UnrealURL& url, const std::map<std::string, std::string>& travelInfo)
{
	ClientTravelInfo.URL.Clear();

	if (Level)
		CallEvent(console, EventName::NotifyLevelChange);

	if (url.HasOption("entry")) // Not sure what the purpose of this kind of travel is - do nothing for now.
		return;

	audiodev->StopSounds();
	UnloadMap();

	// Load map objects

	LevelPackage = packages->LoadMap(url.Map);

	GetLevelInfoObject();

	LevelInfo->ComputerName() = "MyComputer";
	LevelInfo->HubStackLevel() = 0; // To do: handle level hubs
	LevelInfo->EngineVersion() = LaunchInfo.gameVersionString + " SE";
	if (LaunchInfo.ue1Version > 219)
		LevelInfo->MinNetVersion() = LaunchInfo.gameVersionString + " SE";
	LevelInfo->bHighDetailMode() = true;
	LevelInfo->NetMode() = 0; // NM_StandAlone
	LevelInfo->DefaultTexture() = engine->DefaultTexture;

	LevelInfo->URL = url;

	GetLevelObject();

	Level->TravelInfo = travelInfo; // Initially used travel info for level restart

	// Remove the actors meant for the editor (to do: should we do this at the package manager level?)
	for (UActor*& actor : Level->Actors)
	{
		if (actor && AllFlags(actor->Flags, ObjectFlags::NotForServer))
		{
			actor->bDeleteMe() = true;
			actor = nullptr;
		}
	}

	LinkActorsToLevel();

	// Find the game info class
	UClass* gameInfoClass = packages->FindClass(LevelInfo->URL.GetOption("game"));
	if (!gameInfoClass)
		gameInfoClass = LevelInfo->DefaultGameType();
	if (!gameInfoClass)
		gameInfoClass = packages->FindClass(packages->GetIniValue("system", "Engine.Engine", "DefaultGame"));
	if (!gameInfoClass)
		gameInfoClass = packages->FindClass("Botpack.DeathMatchPlus");
	if (!gameInfoClass)
		Exception::Throw("Could not find any gameinfo class!");

	// Spawn GameInfo actor
	GameInfo = UObject::Cast<UGameInfo>(LevelPackage->NewObject("gameinfo", gameInfoClass, ObjectFlags::NoFlags));
	GameInfo->XLevel() = Level;
	GameInfo->Level() = LevelInfo;
	Level->Collision.AddToCollision(GameInfo);
	GameInfo->Tag() = gameInfoClass->Name;
	GameInfo->bTicked() = false;
	GameInfo->InitActorZone();
	GameInfo->Index = (int)Level->Actors.size();
	Level->Actors.push_back(GameInfo);

	LevelInfo->Game() = GameInfo;

	if (!LevelInfo->bBegunPlay())
	{
		LevelInfo->TimeSeconds() = 0.0f;
		LevelInfo->bBegunPlay() = true;

		std::string options = url.GetOptions();

		auto stringProp = GC::Alloc<UStringProperty>("", nullptr, ObjectFlags::NoFlags);
		std::string error;

		// Only call PreBegin/Begin/PostBegin/SetInitialState for loaded objects. Spawned objects are added at the end of the Actors array.
		size_t loadActorCount = Level->Actors.size();

		LevelInfo->bStartup() = true;
		CallEvent(GameInfo, EventName::InitGame, { ExpressionValue::StringValue(options), ExpressionValue::Variable(&error, stringProp) });
		if (!error.empty())
			Exception::Throw("InitGame failed: " + error);

		// Note: the events may spawn actors. We can't use iterators here.
		for (size_t i = 0; i < loadActorCount; i++) { if (Level->Actors[i]) CallEvent(Level->Actors[i], EventName::PreBeginPlay); }
		for (size_t i = 0; i < loadActorCount; i++) { if (Level->Actors[i]) CallEvent(Level->Actors[i], EventName::BeginPlay); }
		for (size_t i = 0; i < loadActorCount; i++) { if (Level->Actors[i]) CallEvent(Level->Actors[i], EventName::PostBeginPlay); }
		for (size_t i = 0; i < loadActorCount; i++) { if (Level->Actors[i]) CallEvent(Level->Actors[i], EventName::SetInitialState); }

		if (engine->LaunchInfo.IsDeusEx())
		{
			for (size_t i = 0; i < loadActorCount; i++) { if (Level->Actors[i]) CallEvent(Level->Actors[i], "PostPostBeginPlay"); }
		}

		for (size_t i = 0; i < loadActorCount; i++) { if (Level->Actors[i]) Level->Actors[i]->InitBase(); }
		LevelInfo->bStartup() = false;
	}

	if (LevelInfo->Game())
		CallEvent(LevelInfo->Game(), "DetailChange", {});
}

void Engine::LoadFromSaveFile(const UnrealURL& url)
{
	ClientTravelInfo.URL.Clear();

	if (Level)
		CallEvent(console, EventName::NotifyLevelChange);

	if (url.HasOption("entry")) // Not sure what the purpose of this kind of travel is - do nothing for now.
		return;

	Package* savefilePackage = nullptr;
	uint32_t slotNum = 0;

	if (url.HasOption("load"))
	{
		slotNum = Convert::to_uint32(url.GetOption("load"));
		savefilePackage = packages->LoadSaveSlot(slotNum);
		LogMessage("Load save slot " + std::to_string(slotNum) + ": " + (savefilePackage ? "found package " + savefilePackage->GetPackageName().ToString() : "NOT FOUND in " + packages->GetSaveFolderPath().string()));
	}

	if (!savefilePackage)
		return; // silently keeps the current level - the log line above is the only trace

	audiodev->StopSounds();
	UnloadMap();

	LevelPackage = savefilePackage;

	GetLevelInfoObject();

	// Same as LoadMap: these are session/engine identity, not save data, and must be
	// re-established on every load regardless of what the package/save file contains.
	LevelInfo->ComputerName() = "MyComputer";
	LevelInfo->HubStackLevel() = 0; // To do: handle level hubs
	LevelInfo->EngineVersion() = LaunchInfo.gameVersionString + " SE";
	if (LaunchInfo.ue1Version > 219)
		LevelInfo->MinNetVersion() = LaunchInfo.gameVersionString + " SE";
	LevelInfo->bHighDetailMode() = true;
	LevelInfo->NetMode() = 0; // NM_StandAlone
	LevelInfo->DefaultTexture() = engine->DefaultTexture;

	// LevelInfo->URL is not serialized, so it is
	// never restored by loading the save package and must be rebuilt
	std::string realMapName = packages->GetIniValue("user", "SaveGame", "MapName" + std::to_string(slotNum));
	if (realMapName.empty())
		realMapName = LevelPackage->GetPackageName().ToString();
	LevelInfo->URL = UnrealURL(realMapName);

	GetLevelObject();

	LinkActorsToLevel();

	// engine->GameInfo is otherwise only assigned in LoadMap, so without this it keeps
	// pointing at the previous, now-unloaded level's GameInfo. LevelInfo->Game() is a normal
	// script property, so it round-trips through the save correctly; just re-point at it.
	GameInfo = UObject::Cast<UGameInfo>(LevelInfo->Game());
	if (!GameInfo)
		Exception::Throw("Save file has no GameInfo actor for " + LevelPackage->GetPackageName().ToString() + "!");
}

void Engine::PossessSavedPlayer()
{
	// Loading a save must not reuse LoginPlayer, because that always calls GameInfo.Login,
	// which always spawns a brand new pawn. The save package already contains the actual saved
	// pawn - deserialized with its real position, health and inventory - sitting in
	// Level->Actors. Find and possess that one directly instead.
	UPlayerPawn* pawn = nullptr;
	for (UActor* actor : Level->Actors)
	{
		UPlayerPawn* p = UObject::TryCast<UPlayerPawn>(actor);
		if (p && p->bIsPlayer())
		{
			pawn = p;
			break;
		}
	}

	if (!pawn)
		Exception::Throw("Save file has no player pawn for " + LevelPackage->GetPackageName().ToString() + "!");

	if (auto pawnExt = UObject::TryCast<UPlayerPawnExt>(pawn))
	{
		// FlagBase is Transient (not saved), so it needs the same reconstruction LoginPlayer
		// does for a fresh spawn.
		if (!pawnExt->FlagBase())
		{
			auto flagBaseCls = packages->FindClass("Extension.FlagBase");
			pawnExt->FlagBase() = UObject::Cast<UFlagBase>(packages->GetTransientPackage()->NewObject("FlagBase", flagBaseCls, ObjectFlags::Transient));
		}
	}

	viewport->Actor() = pawn;
	viewport->Actor()->Player() = viewport;
	CallEvent(viewport->Actor(), EventName::Possess);

	render->OnMapLoaded();
}

void Engine::SaveGameToSlot(int32_t slotNum, const std::string& saveDescription) const
{
	if (slotNum < -1 || (!packages->IsDeusEx() && slotNum < 0))
		Exception::Throw("Invalid save slot: " + std::to_string(slotNum));

	// First and foremost ensure the Save folder exists
	const auto saveFolderPath = packages->GetSaveFolderPath();
	if (!fs::exists(saveFolderPath))
		fs::create_directory(saveFolderPath);

	if (packages->IsDeusEx())
	{
		// Saving a game on Deus Ex does the following:
		// - Create a folder using the slotNum (e.g. 1 -> "Save0001")
		// - Save the level package using the name [MapName].dxs
		// - Save the associated DeusExSaveInfo class as SaveInfo.dxs within that same folder,
		// in which saveDescription parameter will be used in DeusExSaveInfo.Description
		auto slotNumStr = std::to_string(slotNum);
		slotNumStr.insert(0, 4 - slotNumStr.length(), '0'); // Pad it with 0s
		auto saveFolder = "Save" + slotNumStr;

		auto saveSlotFolder = saveFolderPath / saveFolder;
		if (!fs::exists(saveSlotFolder) || !fs::is_directory(saveSlotFolder))
			fs::create_directory(saveSlotFolder);

		auto levelName = Level->package->GetPackageName().ToString() + "." + packages->GetSaveExtension();
		auto saveInfoName = "SaveInfo." + packages->GetSaveExtension();
		auto saveFileFullPath = (saveSlotFolder / levelName).string();
		auto saveInfoFullPath = (saveSlotFolder / saveInfoName).string();
		LevelPackage->Save(Level, saveFileFullPath);

		dxSaveInfo->DirectoryIndex() = slotNum;
		dxSaveInfo->Description() = saveDescription;
		dxSaveInfo->MissionLocation() = DeusExLevelInfo ? DeusExLevelInfo->MissionLocation() : "";
		dxSaveInfo->MapName() = Level->package->GetPackageName().ToString();
		dxSaveInfo->UpdateTimeStamp();
		deusExPackage->Save(dxSaveInfo, saveInfoFullPath);
	}
	else
	{
		const std::string saveFileName = "Save" + std::to_string(slotNum) + "." + packages->GetSaveExtension();
		const std::string saveFileFullPath = (saveFolderPath / saveFileName).string();
		LevelPackage->Save(Level, saveFileFullPath);

		// The save package is later reloaded by its slot filename ("SaveN"), not by the original
		// map name, so record the real map name here for LoadFromSaveFile() to recover.
		packages->SetIniValue("user", "SaveGame", "MapName" + std::to_string(slotNum), Level->package->GetPackageName().ToString());
	}

	// Flush the ini files now. The in-memory config (this slot's MapName above, and the menu's
	// SlotNames[] written by UMenu's SaveConfig() just before it issued "SaveGame N") was
	// otherwise only written at a clean engine shutdown - which never happens when the app is
	// simply killed, as it routinely is on the Quest. The .usa file then existed on disk but the
	// Load menu showed "..Empty.." (saves appeared lost after closing the app).
	packages->SaveAllIniFiles();
}

std::map<std::string, std::string> Engine::CreateTravelInfo(bool transferItems)
{
	auto travelInfo = Level->TravelInfo;
	for (UActor* actor : Level->Actors)
	{
		UPlayerPawn* pawn = UObject::TryCast<UPlayerPawn>(actor);
		if (pawn && pawn->Player())
		{
			std::string playerName = engine->LaunchInfo.ue1Version > 219 ? pawn->PlayerReplicationInfo()->PlayerName() : std::string("Player"); // To do: how to get the travel player name?
			travelInfo[playerName] = ActorTravelInfo::Create(pawn, transferItems);
		}
	}
	return travelInfo;
}

void Engine::LoginPlayer()
{
	UnrealURL url = LevelInfo->URL;
	std::map<std::string, std::string> travelInfo = Level->TravelInfo;

	auto stringProp = GC::Alloc<UStringProperty>("", nullptr, ObjectFlags::NoFlags);
	std::string error, failcode;

	std::string portal = url.GetPortal();
	std::string options = url.GetOptions();

	std::string playerPawnClass = url.GetOption("Class");
	if (playerPawnClass.empty())
		playerPawnClass = packages->GetIniValue("system", "URL", "Class");
	UClass* pawnClass = packages->FindClass(playerPawnClass);

	// Perform PreLogin check (used for early rejection in network games)
	CallEvent(LevelInfo->Game(), EventName::PreLogin, {
		ExpressionValue::StringValue(options),
		ExpressionValue::Variable(&error, stringProp),
		ExpressionValue::Variable(&failcode, stringProp),
		});
	if (!error.empty() || !failcode.empty())
		Exception::Throw("GameInfo prelogin failed: " + error + " (" + failcode + ")");

	// Create viewport pawn
	size_t numActors = Level->Actors.size();
	UPlayerPawn* pawn = UObject::Cast<UPlayerPawn>(CallEvent(LevelInfo->Game(), EventName::Login, {
		ExpressionValue::StringValue(portal),
		ExpressionValue::StringValue(options),
		ExpressionValue::Variable(&error, stringProp),
		ExpressionValue::ObjectValue(pawnClass)
		}).ToObject());
	if (!pawn || !error.empty())
		Exception::Throw("GameInfo login failed: " + error);
	bool actorActuallySpawned = Level->Actors.size() != numActors;

	pawn->LoadProperties();

	if (auto pawnExt = UObject::TryCast<UPlayerPawnExt>(pawn))
	{
		// Unclear if this is how DeusEx spawned this object
		if (!pawnExt->FlagBase())
		{
			auto flagBaseCls = packages->FindClass("Extension.FlagBase");
			pawnExt->FlagBase() = UObject::Cast<UFlagBase>(packages->GetTransientPackage()->NewObject("FlagBase", flagBaseCls, ObjectFlags::Transient));
		}
	}

	// Assign the pawn to the viewport
	viewport->Actor() = pawn;
	viewport->Actor()->Player() = viewport;
	CallEvent(viewport->Actor(), EventName::Possess);

	// Transfer travel actors to the new map

	CallEvent(pawn, EventName::TravelPreAccept);

	Array<UObject*> acceptedObjects;
	if (actorActuallySpawned && ClientTravelInfo.TravelType == ETravelType::TRAVEL_Relative)
	{
		std::string playerName = url.GetOption("Name");
		if (playerName.empty())
			playerName = packages->GetIniValue("system", "URL", "Name");

		auto it = travelInfo.find(playerName);
		if (!playerName.empty() && it != travelInfo.end())
		{
			acceptedObjects = ActorTravelInfo::Accept(pawn, it->second);
		}
		else
		{
			if (travelInfo.empty())
				LogMessage("Skipping travel transfer. No travel data");
			else if (playerName.empty())
				LogMessage("Skipping travel transfer. Player name is empty");
			else
				LogMessage("Skipping travel transfer. Player '" + playerName + "' not found in travel info");
		}
	}

	for (UObject* object : acceptedObjects)
		CallEvent(object, EventName::TravelPreAccept);

	CallEvent(LevelInfo->Game(), EventName::AcceptInventory, { ExpressionValue::ObjectValue(pawn) });

	for (UObject* object : acceptedObjects)
		CallEvent(object, EventName::TravelPostAccept);

	CallEvent(pawn, EventName::TravelPostAccept);
	CallEvent(LevelInfo->Game(), EventName::PostLogin, { ExpressionValue::ObjectValue(pawn) });

	render->OnMapLoaded();
}

UZoneInfo* Engine::GetZoneActor(int zoneIndex)
{
	if (auto zone = UObject::TryCast<UZoneInfo>(Level->Model->Zones[zoneIndex].ZoneActor))
		return zone;
	else
		return LevelInfo;
}

UObject* Engine::FindObject(NameString name, NameString className)
{
	for (auto actor : Level->Actors)
	{
		if (actor && actor->Name == name && UObject::GetUClassFullName(actor) == className)
			return actor;
	}

	return nullptr;
}

float Engine::CalcTimeElapsed()
{
	using namespace std::chrono;

	uint64_t currentTime = duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
	if (lastTime == 0)
		lastTime = currentTime;

	uint64_t deltaTime = currentTime - lastTime;
	lastTime = currentTime;
	return clamp(deltaTime / 1'000'000.0f, 0.0f, 1.0f);
}

std::string Engine::ParseClassName(std::string className)
{
	// Workaround for broken unrealscript code referencing windrv.windowsclient directly
	if (className == "windrv.windowsclient")
	{
		className = "ini:Engine.ViewportManager";
	}

	if (className.size() < 4 || className.substr(0, 4) != "ini:")
		return className;

	size_t pos = className.find_last_of('.');
	if (pos == std::string::npos)
		Exception::Throw("Parse error");

	NameString sectionName = className.substr(4, pos - 4);
	NameString keyName = className.substr(pos + 1);

	// Override the ini file for things that are internal in Surreal Engine
	if (sectionName == "Engine.Engine")
	{
		if (keyName == "GameRenderDevice" || keyName == "WindowedRenderDevice")
		{
			return renderdev->Class;
		}
		else if (keyName == "AudioDevice")
		{
			return audiodev->Class;
		}
		else if (keyName == "NetworkDevice")
		{
			return netdev->Class;
		}
		else if (keyName == "ViewportManager")
		{
			return client->Class;
		}
	}

	return packages->GetIniValue("system", sectionName, keyName);
}

std::string Engine::ConsoleCommand(UObject* context, const std::string& commandline, BitfieldBool& found)
{
	found = false;

	Array<std::string> args = GetArgs(commandline);
	if (args.empty())
	{
		return {};
	}

	std::string command = args[0];
	for (char& c : command) c = std::tolower(c);

	found = true;
	if (command == "exit" || command == "quit")
	{
		quit = true;
	}
	else if (command == "timedemo" && args.size() == 2)
	{
		render->ShowTimedemoStats = args[1] == "1";
	}
	else if (command == "stat" && args.size() == 2)
	{
		render->ShowRenderStats = 0;

		if (args[1] == "render")
			render->ShowRenderStats = 1;
	}
	else if (command == "collisiondebug" && args.size() == 2)
	{
		render->ShowCollisionDebug = args[1] == "1";
	}
	else if (command == "dxwindowdebug" && LaunchInfo.IsDeusEx())
	{
		m_DrawDebugDXWindowHierarchy = !m_DrawDebugDXWindowHierarchy;
	}
	else if (command == "showlog")
	{
		//Frame::ShowDebuggerWindow();
	}
	/*else if (command == "playsong")
	{
		auto music = LevelInfo->Song();
		if (music)
			audio->PlayMusic(AudioSource::CreateMod(music->Data, true, 0, LevelInfo->SongSection()));
	}
	else if (command == "stopsong")
	{
		audio->PlayMusic(nullptr);
	}*/
	else if (command == "getres")
	{
		if (LaunchInfo.IsHarryPotter1()) // FEOptionsPage.IsSupportedResolution is so sad :)
			return "1024x768";
		return window->GetAvailableResolutions();
	}
	else if (command == "getcolordepths")
	{
		return "32 16";
	}
	else if (command == "getcurrentres")
	{
		int width = window->GetPixelWidth();
		int height = window->GetPixelHeight();

		return std::to_string(width) + "x" + std::to_string(height);
	}
	else if (command == "getcurrentcolordepth")
	{
		return "32";
	}
	else if (command == "getping")
	{
		return "0";
	}
	else if (command == "getloss")
	{
		return "0";
	}
	else if (command == "keyname" && args.size() == 2)
	{
		uint8_t index = Convert::to_uint8(args[1]);
		return keynames[index];
	}
	else if (command == "keybinding" && args.size() == 2)
	{
		const std::string& name = args[1];
		return keybindings[name];
	}
	else if ((command == "open" || command == "start") && args.size() == 2)
	{
		const std::string& maparg = args[1];

		UnrealURL url(maparg);

		for (auto& map : packages->GetMaps())
		{
			std::string mapname = fs::path(map).stem().string();

			if (StrTools::equals_ignore_case(mapname, url.Map))
			{
				ClientTravel(url.ToString(), ETravelType::TRAVEL_Absolute, false);
				return {};
			}	
		}

		LogMessage("Couldn't find map " + maparg);
	}
	else if (command == "switchlevel" && args.size() == 2)
	{
		// This works like open/start, but keeps the difficulty level
		// As well as the game type
		const std::string& maparg = args[1];

		UnrealURL url(maparg);

		// First check if the provided URL has a difficulty option
		std::string difficulty = url.GetOption("difficulty");

		// If there isn't, try to get it from the current level's options
		if (difficulty.empty())
			difficulty = LevelInfo->URL.GetOption("difficulty");

		// If there still isn't, refer to GameInfo for difficulty
		if (difficulty.empty())
			difficulty = std::to_string(GameInfo->Difficulty());

		if (!difficulty.empty())
			url.AddOrReplaceOption("difficulty=" + difficulty);

		for (auto& map : packages->GetMaps())
		{
			std::string mapname = fs::path(map).stem().string();

			if (StrTools::equals_ignore_case(mapname, url.Map))
			{
				LevelInfo->NextURL() = url.ToString();
				return {};
			}
		}

		LogMessage("Couldn't find map " + maparg);
	}
	else if (command == "savegame" && (args.size() == 2 || args.size() == 3))
	{

		int32_t slotNum;
		// slotNum not being parsable shouldn't cause a crash
		try
		{
			slotNum = Convert::to_int32(args[1]);
		}
		catch (...)
		{
			return {};
		}

		SaveGameInfo.SaveGameSlot = slotNum;

		if (args.size() == 3)
			SaveGameInfo.SaveGameDescription = args[2];

		// SaveGameToSlot(slotNum, "");

		//LogMessage("SaveGame command not fully implemented yet!");
		return {};
	}
	else if (command == "get" && args.size() == 3)
	{
		NameString className = ParseClassName(args[1]);
		NameString propertyName = args[2];

		UClass* cls = packages->FindClass(className);
		if (!cls)
		{
			LogMessage("Could not find class '" + className.ToString() + "': " + commandline);
			return {};
		}

		if (className == renderdev->Class)
		{
			return renderdev->GetPropertyAsString(propertyName);
		}
		else if (className == audiodev->Class)
		{
			return audiodev->GetPropertyAsString(propertyName);
		}
		else if (className == netdev->Class)
		{
			return netdev->GetPropertyAsString(propertyName);
		}
		else if (className == client->Class)
		{
			return client->GetPropertyAsString(propertyName);
		}
		else
		{
			try
			{
				return cls->GetPropertyAsString(propertyName);
			}
			catch (const std::exception&)
			{
				LogMessage("Could not get property '" + propertyName.ToString() + "': " + commandline);
				return {};
			}
		}
	}
	else if (command == "set" && args.size() == 4)
	{
		NameString className = ParseClassName(args[1]);
		NameString propertyName = args[2];
		std::string value = args[3];

		// Special input setting handling
		if (className == "input")
		{
			keybindings[propertyName.ToString()] = value;
			packages->SetIniValue("user", "Engine.Input", propertyName, value);
			return {};
		}

		UClass* cls = packages->FindClass(className);
		if (!cls)
		{
			LogMessage("Could not find class '" + className.ToString() + "': " + commandline);
			return {};
		}

		if (className == renderdev->Class)
		{
			renderdev->SetPropertyFromString(propertyName, value);
		}
		else if (className == audiodev->Class)
		{
			audiodev->SetPropertyFromString(propertyName, value);
		}
		else if (className == netdev->Class)
		{
			netdev->SetPropertyFromString(propertyName, value);
		}
		else if (className == client->Class)
		{
			client->SetPropertyFromString(propertyName, value);
		}
		else
		{
			try
			{
				cls->SetPropertyFromString(propertyName, value);
			}
			catch (const std::exception&)
			{
				LogMessage("Could not set property '" + propertyName.ToString() + "': " + commandline);
			}
			return {};
		}
	}
	else if (command == "setres" && args.size() == 2)
	{
		window->SetResolution(args[1]);
	}
	else if (command == "togglefullscreen")
	{
		bool isFullscreen = window->IsFullscreen();

		// Get the resolution to SWITCH TO
		int width = isFullscreen ? client->WindowedViewportX : client->FullscreenViewportX;
		int height = isFullscreen ? client->WindowedViewportY : client->FullscreenViewportY;

		Size resolution;
		resolution.width = width;
		resolution.height = height;

		window->ToggleWindowFullscreen(resolution);
		viewport->SetViewportRect(0, 0, width, height);

		return {};
	}
	else if (command == "prsq")
	{
		// Klingon Honor Guard: CD check
		// "mpgameplay"
		// "mpinstall"
		return "mpgameplay";
	}
	else if (command == "os")
	{
		// 227 Seems to have this command
#ifdef WIN32
		return "Windows";
#elif __APPLE__
		return "macOS";
#else
		return "Linux"; // With apologies to BSD, Haiku and others...
#endif
	}
	else if (command == "getsplash")
	{
		return khgSplashScreen ? "true" : "false";
	}
	else if (command == "setsplash")
	{
		khgSplashScreen = true;
		return {};
	}
	else if (command == "playavi")
	{
		PlayAVI(args);
		return {};
	}
	else if (command == "flush")
	{
		engine->render->Device->Flush(1);
		return {};
	}
	else
	{
		if (!ExecCommand(args))
		{
			LogMessage("Unknown command: " + commandline);
			found = false;
		}
	}
	return {};
}

Array<std::string> Engine::GetArgs(const std::string& commandline)
{
	Array<std::string> args;
	size_t i = 0;
	while (i < commandline.size())
	{
		size_t j = commandline.find_first_not_of(" \t", i);
		if (j == std::string::npos)
			break;
		i = j;
		j = commandline.find_first_of(" \t", i);
		if (j == std::string::npos)
			j = commandline.size();
		if (j > i)
			args.push_back(commandline.substr(i, j - i));
		i = j;
	}
	return args;
}

Array<std::string> Engine::GetSubcommands(const std::string& command)
{
	Array<std::string> subcommands;
	size_t pos = 0;
	while (pos < command.size())
	{
		size_t endpos = command.find('|', pos);
		if (endpos == std::string::npos)
			endpos = command.size();

		std::string subcommand = command.substr(pos, endpos - pos);
		if (!subcommand.empty())
			subcommands.push_back(subcommand);
		pos = endpos + 1;
	}
	return subcommands;
}

void Engine::LoadEngineSettings()
{
	if (packages->MissingSESystemIni())
	{
		client->LoadProperties("WinDrv.WindowsClient");
		audiodev->LoadProperties("Galaxy.GalaxyAudioSubsystem");
		renderdev->LoadProperties("D3DDrv.Direct3DRenderDevice");
	}
	else
	{
		client->LoadProperties();
		audiodev->LoadProperties();
		renderdev->LoadProperties();
	}

#ifdef WIN32
	windowingSystemName = packages->GetIniValue("System", "Engine.SurrealWindowSystem", "WindowSystem", "Win32");
#else
	windowingSystemName = packages->GetIniValue("System", "Engine.SurrealWindowSystem", "WindowSystem", "SDL2");
#endif
}

void Engine::LoadKeybindings()
{
	for (int i = 0; i < 256; i++)
	{
		std::string keyname = keynames[i];
		keybindings[keyname] = packages->GetIniValue("user", "Engine.Input", keyname);
	}

	for (int i = 0; i < 40; i++)
	{
		std::string alias = packages->GetIniValue("user", "Engine.Input", "Aliases[" + std::to_string(i) + "]");

		// Total trash parsing, but it will do for the aliases I have! Feel free to improve it!
		std::string commandStart = "(Command=\"";
		std::string commandSplit = "\",Alias=";
		std::string commandEnd = ")";
		if (alias.size() > commandStart.size() + commandSplit.size() + commandEnd.size())
		{
			size_t pos = alias.find(commandSplit, commandStart.size());
			if (pos != std::string::npos)
			{
				size_t pos2 = alias.find(commandEnd, pos);
				if (pos2 != std::string::npos)
				{
					std::string aliasCommand = alias.substr(commandStart.size(), pos - commandStart.size());
					std::string aliasName = alias.substr(pos + commandSplit.size(), pos2 - pos - commandSplit.size());
					if (!aliasName.empty() && aliasName != "None")
						inputAliases[aliasName] = aliasCommand;
				}
			}
		}
	}
}

// VR: feeds vrInput's per-frame controller state into the Pawn's movement/action properties,
// bypassing the desktop keybinding-string machinery (activeInputAxes/activeInputButtons,
// InputCommand) entirely - that system is built around discrete keyboard press/release events
// resolved through User.ini alias strings (e.g. "MoveForward" -> "Axis aBaseY Speed=+300.0"),
// which doesn't fit a continuously-varying analog thumbstick well. Written directly here
// instead, matching the same property names those aliases ultimately target (confirmed against
// a real UT99 User.ini: aBaseY=forward/back, aStrafe=left/right, aBaseX=turn, aUp=jump,
// bFire/bAltFire=trigger/grip). Called once per frame, before UpdateInput() so a real keyboard
// (if one happens to be connected) can still layer on top without being immediately overwritten
// - though VR is expected to be the sole input source in practice.
void Engine::UpdateVRInput(float timeElapsed)
{
	if (!vrInput)
		return;

	// Left controller's Menu button opens/closes the pause/main menu exactly like the desktop
	// Escape key - synthesized as a real IK_Escape press so it goes through the normal
	// keybinding/menu-opening path (Engine::OnWindowKeyDown, Engine.cpp) rather than needing to
	// reverse-engineer how URootWindow gets created. Checked unconditionally (not gated on
	// viewport->Actor(), unlike the movement/fire block below) so the menu can still be opened
	// even without a live Pawn (e.g. from a death/spectator state). Also toggles the "big
	// screen" quad (vrBigScreenActive, see its own comment) in lockstep - the quad's visibility
	// is driven directly by this same button press, not by trying to detect the game's own UI
	// state.
	if (vrInput->MenuButtonJustPressed)
	{
		InputEvent(EInputKey::IK_Escape, EInputType::IST_Press, 0);
		vrBigScreenManualToggle = !vrBigScreenManualToggle;
	}

	// Left grip: crouch. Unreal binds C to "Duck" ("Button bDuck | Axis aUp Speed=-300"), a HELD
	// command, so this is a real IK_C press on squeeze and release on let-go. Kept
	// ahead of the menu/pawn early-outs below so the release always reaches the game.
	if (vrInput->CrouchJustPressed)
		InputEvent(EInputKey::IK_C, EInputType::IST_Press, 0);
	if (vrInput->CrouchJustReleased)
		InputEvent(EInputKey::IK_C, EInputType::IST_Release, 0);

	// Panel visibility: for games whose console is UWindow-based follow the game's own
	// Viewport.bShowWindowsMouse - UWindowConsole raises it whenever a mouse-driven menu is
	// showing and clears it when that menu closes BY ANY MEANS, including the player clicking
	// "Return to game"/"Start" inside the menu. A plain Menu-button toggle can't see those
	// in-menu closes and stayed stuck on (confirmed on real Quest 3 hardware via a screen
	// recording: a black panel showing just the HUD hovering in front of the player during
	// gameplay, and - because DrawEyeVR suppresses the per-eye overlays while the panel is
	// up - no first-person weapon either, and no movement since the stick axes are zeroed
	// below while the panel is up). Detected by the console CLASS, not the engine version:
	// Unreal Gold 226b (ue1Version 226) already ships UWindow.u/UMenu.u and its
	// UPak.UPakConsole derives from UWindowConsole, so the old ">= 400" gate left it on the
	// manual toggle and reproduced exactly that stuck black panel on the Quest 3 (menu
	// "moving with my head" from re-anchoring on every toggle press, black view, no movement).
	// Games with a pre-UWindow console keep the manual toggle as the only option.
	// UT names the class UWindowConsole; Unreal Gold 226's UWindow.u names it WindowConsole.
	if (console && (console->IsA("UWindowConsole") || console->IsA("WindowConsole")))
		vrBigScreenActive = viewport->bShowWindowsMouse();
	else
		vrBigScreenActive = vrBigScreenManualToggle;

	if (!viewport->Actor() || timeElapsed <= 0.0f)
		return;

	UPlayerPawn* pawn = viewport->Actor();

	// (Re)spawn: a new Pawn object arrives facing its PlayerStart. Adopt that yaw as the body
	// yaw before the per-frame override below replaces it, otherwise the player always spawns
	// facing whatever BodyYawRadians happened to be (initially world +X) - reported on real
	// Quest 3 hardware as consistently spawning facing the wrong direction.
	if (pawn != vrLastPawn)
	{
		vrLastPawn = pawn;
		constexpr float twoPi = 2.0f * 3.14159265359f;
		float spawnYaw = (float)(pawn->Rotation().Yaw & 0xFFFF) * (twoPi / 65536.0f);
		vrInput->BodyYawRadians = spawnYaw;

		// Deactivate double-tap dodging in VR: analog
		// stick motion crossing the center reads as key double-taps and triggers hop-dodges.
		pawn->SetFloat("DodgeClickTime", 0.0f);

		// Deactivate walking view bob in VR: PlayerPawn.Bob drives the WalkBob offset PlayerCalcView adds to the camera -
		// artificial vertical head motion, a classic VR comfort problem. The headset supplies
		// real head motion instead.
		pawn->SetFloat("Bob", 0.0f);
	}

	// While the big screen is showing, the right controller drives the menu cursor (see
	// RunVRMenuScreen()) instead of movement/aim/fire - skip those entirely (and zero
	// movement so the Pawn doesn't keep drifting from whatever axis values were last set before
	// the menu opened).
	if (vrBigScreenActive)
	{
		pawn->SetFloat("aBaseY", 0.0f);
		pawn->SetFloat("aStrafe", 0.0f);
		pawn->SetFloat("aUp", 0.0f);
		// Fire/AltFire aren't zeroed here: they're driven by synthesized mouse-button
		// press/release events below, and RunVRMenuScreen() sends the matching release
		// (OnWindowMouseUp -> InputEvent IST_Release) if the trigger is let go while the menu
		// is up, which clears the "Button bFire" binding the same way a real mouse would.
		return;
	}

	// Body-facing yaw drives the Pawn's actual world rotation, not just the render camera's
	// CameraRotation copy - without this the Pawn stays frozen at spawn orientation forever
	// (VR bypasses the normal mouselook path that would otherwise turn it), so movement axes
	// below would push relative to a rotation that never matches where the player is actually
	// facing, and the Pawn would drift somewhere unintended (confirmed on real hardware: player
	// ended up outside the map). Pitch/Roll are left alone - the body doesn't lean or tilt.
	//
	// ViewRotation must get the same yaw: UT's PlayerPawn.UpdateRotation() rebuilds Rotation
	// from ViewRotation every tick (ViewRotation is what mouselook/aTurn normally drive, and
	// nothing drives it in VR), so writing only Rotation() here was overwritten within the same
	// tick and movement stayed locked to the spawn yaw no matter how many snap-turns were made
	// (so movement follows snap-turns).
	int bodyYawUnreal = (int)(vrInput->BodyYawRadians * (0x10000 / (2.0f * 3.14159265359f)));
	Rotator bodyRotation = pawn->Rotation();
	bodyRotation.Yaw = bodyYawUnreal;
	pawn->Rotation() = bodyRotation;
	Rotator viewRotation = pawn->ViewRotation();
	viewRotation.Yaw = bodyYawUnreal;
	// Redeemer guided missile: UT's GuidedWarShell steers itself from the player's
	// ViewRotation every tick (and PlayerCalcView shows its camera, which the VR eyes follow
	// automatically). Pinning ViewRotation to the body yaw at zero pitch made it fly dead
	// straight - hand the full right-controller aim (yaw AND pitch) over while it's in flight.
	if (pawn->ViewTarget() && pawn->ViewTarget()->IsA("GuidedWarShell") && vrInput->RightControllerActive)
		viewRotation = vrInput->GetAimRotation();
	pawn->ViewRotation() = viewRotation;

	// Well above Pawn.AccelRate (2048 for UT players): UActor::ApplyMovementAcceleration
	// (UActor_Phys.cpp) uses the axis-derived Acceleration vector as-is and only CLAMPS it down
	// to AccelRate, so User.ini's nominal Speed=300 meant accelerating at 300 uu/s^2 - over a
	// second to reach run speed, felt as "starting slow, I want to run directly" on real
	// hardware. Anything >= AccelRate clamps to AccelRate, i.e. full acceleration at once; the
	// direction comes from MoveAxis (already snapped to unit length in VRInput).
	const float moveSpeed = 4096.0f;
	pawn->SetFloat("aBaseY", vrInput->MoveAxisY * moveSpeed);
	pawn->SetFloat("aStrafe", vrInput->MoveAxisX * moveSpeed);
	pawn->SetFloat("aUp", vrInput->JumpPressed ? 300.0f : (vrInput->CrouchPressed ? -300.0f : 0.0f)); // swim/fly up, or down while crouching (Duck's "Axis aUp Speed=-300" half); a real jump is the exec function below

	// UT jumps via the pawn's `exec function Jump()` (Space's binding), which sets bPressedJump
	// for the next PlayerMove - the aUp axis alone only matters when swimming/flying.
	if (vrInput->JumpJustPressed)
		InputCommand("Jump", (EInputKey)0, 20);

	// Fire/AltFire go through the SAME path a desktop mouse click takes - synthesized
	// LeftMouse/RightMouse press+release events into InputEvent() - rather than poking the
	// bFire/bAltFire properties directly. Setting the property only makes an already-armed
	// weapon shoot; it never invokes the pawn's `exec function Fire()` that the LeftMouse
	// keybinding's "Fire" alias ("Button bFire | Fire") calls, and in UT that exec function is
	// also the "press fire to ready up / respawn" handler in the PlayerWaiting/Dying states.
	// Confirmed on real Quest 3 hardware: trigger registered (TriggerRaw=1.0, Fire=1 in the
	// diagnostics) yet "Waiting for ready signals" never cleared. InputEvent() also hands the
	// key to the console's KeyEvent first, exactly like a real click, so the game's own UI
	// still gets first refusal. Fallback: if User.ini has no LeftMouse/RightMouse binding,
	// issue the stock UT alias command directly so the controls still work.
	auto sendFireKey = [this](EInputKey key, EInputType type, const char* fallbackCommand)
	{
		InputEvent(key, type, 0);
		if (type == EInputType::IST_Press && keybindings[keynames[key]].empty())
			InputCommand(fallbackCommand, key, 20);
	};

	// Which mouse button carries AltFire. UT binds RightMouse=AltFire, but Unreal Gold's DefUser.ini
	// has RightMouse=Jump and MiddleMouse=AltFire - so blindly synthesizing RightMouse for the grip
	// made the player JUMP. Prefer a button whose binding names AltFire;
	// otherwise an UNBOUND button, so sendFireKey's fallback command runs instead of whatever the
	// game happens to have on that button.
	auto bindingOf = [this](EInputKey key) -> std::string
	{
		auto it = keybindings.find(keynames[(int)key]);
		std::string b = it != keybindings.end() ? it->second : std::string();
		for (char& c : b) c = (char)std::tolower((unsigned char)c);
		return b;
	};
	EInputKey altFireKey;
	if (bindingOf(EInputKey::IK_RightMouse).find("altfire") != std::string::npos) altFireKey = EInputKey::IK_RightMouse;
	else if (bindingOf(EInputKey::IK_MiddleMouse).find("altfire") != std::string::npos) altFireKey = EInputKey::IK_MiddleMouse;
	else if (bindingOf(EInputKey::IK_MiddleMouse).empty()) altFireKey = EInputKey::IK_MiddleMouse;
	else if (bindingOf(EInputKey::IK_RightMouse).empty()) altFireKey = EInputKey::IK_RightMouse;
	else altFireKey = EInputKey::IK_MiddleMouse; // both bound to something else: least harmful guess
	{
		// The press runs the pawn's `exec function Fire()`, which calls Weapon.Fire() right here,
		// synchronously - so the crosshair aim must already be in place (see VRAimOverrideScope).
		VRAimOverrideScope aimOverride(this, vrInput->TriggerJustPressed || vrInput->GripJustPressed);
		if (vrInput->TriggerJustPressed)
			sendFireKey(EInputKey::IK_LeftMouse, EInputType::IST_Press, "Button bFire | Fire");
		if (vrInput->GripJustPressed)
			sendFireKey(altFireKey, EInputType::IST_Press, "Button bAltFire | AltFire");
	}
	if (vrInput->TriggerJustReleased)
		sendFireKey(EInputKey::IK_LeftMouse, EInputType::IST_Release, nullptr);
	if (vrInput->GripJustReleased)
		sendFireKey(altFireKey, EInputType::IST_Release, nullptr);
	if (vrInput->TriggerJustReleased || vrInput->GripJustReleased)
		vrAimOverrideAfterRelease = 0.35f; // see Engine.h - release-fired weapons (rocket launcher)
	else if (vrAimOverrideAfterRelease > 0.0f)
		vrAimOverrideAfterRelease -= timeElapsed;

	// Right stick up/down: the pawn's NextWeapon/PrevWeapon exec functions (what the mouse
	// wheel is bound to on desktop).
	if (vrInput->WeaponNextJustPressed)
		InputCommand("NextWeapon", (EInputKey)0, 20);
	else if (vrInput->WeaponPrevJustPressed)
		InputCommand("PrevWeapon", (EInputKey)0, 20);

	// Left Y: scoreboard toggle (desktop F1 - the pawn's ShowScores exec function).
	if (vrInput->ScoresJustPressed)
		InputCommand("ShowScores", (EInputKey)0, 20);

	// Right B: F2, as a real key tap so whatever the game binds to F2 runs (Unreal Gold's DefUser.ini:
	// "ActivateTranslator | FunctionKey 2" - the universal translator).
	if (vrInput->TranslatorJustPressed)
	{
		InputEvent(EInputKey::IK_F2, EInputType::IST_Press, 0);
		InputEvent(EInputKey::IK_F2, EInputType::IST_Release, 0);
	}

	// Right A: Enter, as a real key tap - Unreal binds it to "InventoryActivate" (ActivateItem,
	// e.g. the selected inventory item such as the flashlight).
	if (vrInput->ActivateItemJustPressed)
	{
		InputEvent(EInputKey::IK_Enter, EInputType::IST_Press, 0);
		InputEvent(EInputKey::IK_Enter, EInputType::IST_Release, 0);
	}

	// Left X: RightBracket ("]"), as a real key tap - Unreal binds it to "InventoryNext" (select the next
	// inventory item).
	if (vrInput->NextItemJustPressed)
	{
		InputEvent(EInputKey::IK_RightBracket, EInputType::IST_Press, 0);
		InputEvent(EInputKey::IK_RightBracket, EInputType::IST_Release, 0);
	}
}

// Raw OpenXR-space forward direction (local -Z rotated by the quaternion) - NOT run through
// VRCamera's OpenXRToUnrealDirection/body-yaw remap, unlike VRInput::GetAimForwardWorld().
// RunVRMenuScreen() below needs this because it positions and ray-casts against the screen
// quad entirely in OpenXR/STAGE space (meters, matching GetHeadPose()/RightControllerPosition
// directly) rather than converting everything into UE1 world space - simpler and one fewer
// axis-convention risk than the menu's first (reverted) implementation, which tried to invert
// the 2D Canvas overlay's own screen-space projection instead of using a real world anchor.
static vec3 GetOpenXRForwardRaw(float qx, float qy, float qz, float qw)
{
	mat4 m = mat4::quaternion(qx, qy, qz, qw);
	return vec3(-m[2 * 4 + 0], -m[2 * 4 + 1], -m[2 * 4 + 2]); // -column2 = local -Z (forward)
}

static constexpr float VRUnitsPerMeter = VRCamera::UnitsPerMeter;

// "Big screen" menu (Engine::RunVR()'s VR branch, once per app-frame) - matches Team Beef
// Studios' QuakeQuest's bigScreen/VR_UseScreenLayer() mode (TBXR_Common.c/QuakeQuest_OpenXR.c)
// in spirit: a flat panel a fixed distance in front of wherever the player was looking when
// the menu opened. Renders the 2D menu/UI into an engine-owned texture
// (RenderSubsystem::RenderMenuTexture()) and then hands the per-eye render loop a world-space
// quad to draw with it (RenderSubsystem::SetMenuWorldQuad() -> DrawEyeVR()), so the panel is
// rendered by the exact same camera transform as the level geometry.
//
// This deliberately does NOT use an OpenXR XrCompositionLayerQuad the way QuakeQuest does (the
// first implementation did - see RenderDevice.h's UnlockScreen comment): on real Quest 3
// hardware that layer stayed glued directly in front of the player's face however they turned,
// despite diagnostic logging confirming the submitted STAGE-space pose was byte-identical for
// 20+ seconds. Rendering the quad ourselves removes the compositor from the equation entirely.
//
// The pose is computed ONCE (on the closed->open transition, via vrMenuScreenPositioned -
// reset when the menu closes) rather than every frame from the live head position, unlike
// QuakeQuest's own literal formula (which keys position off gAppState.xfStageFromHead.position
// freshly each frame, only freezing the YAW/facing direction via its own playerYaw latch).
// Confirmed on real Quest 3 hardware: continuous repositioning made the panel feel head-locked
// and following the head rather than a fixed screen - locking it once on open, as a real
// static object placed in the room, is what was actually wanted.
//
// All positioning and the cursor's ray-vs-plane intersection happen in OpenXR/STAGE space
// (meters, matching GetHeadPose()/RightControllerPosition directly - see GetOpenXRForwardRaw()
// above); only the final four corners are converted to UE1 world space for rendering, via the
// same anchor/scale/body-yaw chain the eye cameras use (VRCamera::StageToWorldPosition), so
// the quad and the camera agree on where "the room" is.
// True while a UWindow text field has keyboard focus: WindowConsole.Root.KeyFocusWindow is a
// UWindowEditBox (Unreal Gold's and UT's UWindow both work this way).
bool Engine::IsVRMenuEditBoxFocused()
{
	if (!console || !console->HasProperty("Root"))
		return false;
	UObject* root = console->GetUObject("Root");
	if (!root || !root->HasProperty("KeyFocusWindow"))
		return false;
	UObject* focus = root->GetUObject("KeyFocusWindow");
	if (!focus || !focus->IsA("UWindowEditBox"))
		return false;
	return !focus->HasProperty("bHasKeyboardFocus") || focus->GetBool("bHasKeyboardFocus");
}

void Engine::RunVRMenuScreen()
{
	if (!vrInput || !vrBigScreenActive)
	{
		render->SetMenuWorldStrips(false, nullptr, 0);
		vrMenuScreenPositioned = false; // re-anchor fresh next time the menu opens
		render->vrKeyboardActive = false; // close the on-screen keyboard when the menu closes
		vrEditBoxFocused = false;
		return;
	}

	Widget* vrWindow = window.get();

	const float screenDistanceMeters = 2.5f; // a comfortable reading distance
	// Cinema-sized (~72 x 57 degrees at 2.5m): the panel is rendered 1:1 at eye resolution
	// (ResetCanvas keeps uiscale 1 for it), so UT's small bitmap fonts get their legibility from
	// physical size rather than pixel-doubling - see the RenderingMenuTexture comment there.
	const float screenWidthMeters = 3.6f;
	const float screenHeightMeters = 2.7f; // 4:3, matching the canvas sub-rectangle below

	if (!vrMenuScreenPositioned)
	{
		// Horizontal component of where the HEAD is physically looking right now, in STAGE
		// space - NOT BodyYawRadians: that's a game-world yaw (snap-turn accumulator) that has
		// no meaning in the physical room's coordinate frame, and using it here put the panel
		// in a fixed room direction regardless of which way the player actually faced.
		MotionControllerPose headPose = vrWindow->GetHeadPose();
		vec3 headForward = GetOpenXRForwardRaw(headPose.OrientationX, headPose.OrientationY, headPose.OrientationZ, headPose.OrientationW);
		vec3 forward(headForward.x, 0.0f, headForward.z);
		float len = std::sqrt(forward.x * forward.x + forward.z * forward.z);
		forward = len > 0.001f ? forward / len : vec3(0.0f, 0.0f, -1.0f); // looking straight up/down: fall back to STAGE forward

		vrMenuScreenForward = forward;
		vrMenuScreenPos = vec3(headPose.PositionX, headPose.PositionY, headPose.PositionZ) + forward * screenDistanceMeters;
		vrMenuScreenPositioned = true;
	}

	// Panel basis in STAGE space, as seen BY THE PLAYER: right = forward x up (right-handed,
	// +Y up), up = +Y, normal points back at the player. Image (0,0) is the top-left corner
	// from the player's viewpoint - see VulkanRenderDevice::DrawMenuWorldQuad's UV assignment.
	vec3 quadPos = vrMenuScreenPos;
	vec3 quadForward = vrMenuScreenForward;
	vec3 quadUp(0.0f, 1.0f, 0.0f);
	vec3 quadRight(-quadForward.z, 0.0f, quadForward.x);
	vec3 quadNormal = -quadForward;

	// Render the menu texture. It's sized to match the EYE render target rather than a
	// separate UI resolution, so VulkanRenderDevice::Lock() doesn't tear down and rebuild the
	// shared offscreen Scene buffers (and every pipeline with them) three times per frame
	// because the size keeps alternating - the previous screen-layer path paid exactly that
	// cost. The 2D canvas only covers a 4:3 sub-rectangle at the top-left of it, and the quad's
	// UVs are clipped to match (uvMax).
	int eyeWidth, eyeHeight;
	vrWindow->GetEyeImageSize(0, &eyeWidth, &eyeHeight);
	int canvasWidth = eyeWidth;
	int canvasHeight = std::min(eyeHeight, eyeWidth * 3 / 4);
	viewport->SetViewportRect(0, 0, canvasWidth, canvasHeight);
	render->RenderMenuTexture(eyeWidth, eyeHeight);
	vec2 uvMax((float)canvasWidth / (float)eyeWidth, (float)canvasHeight / (float)eyeHeight);

	// Flat screen: a single planar quad facing the viewer. A curved panel was tried first, but the
	// curvature made menu text harder to read, so the panel is flat (spanning quadRight x quadUp,
	// normal quadForward). The cursor is a ray-vs-plane hit mapped to canvas pixels - geometry and
	// cursor share the same plane so clicks stay aligned with what is drawn.
	const float halfH = screenHeightMeters * 0.5f;
	const float halfW = screenWidthMeters * 0.5f;
	{
		vec3 stageCorners[4] = {
			quadPos - quadRight * halfW + quadUp * halfH, // top-left
			quadPos + quadRight * halfW + quadUp * halfH, // top-right
			quadPos + quadRight * halfW - quadUp * halfH, // bottom-right
			quadPos - quadRight * halfW - quadUp * halfH, // bottom-left
		};
		RenderSubsystem::MenuStrip strip;
		for (int c = 0; c < 4; c++)
			strip.Corners[c] = VRCamera::StageToWorldPosition(stageCorners[c], CameraLocation, vrInput->BodyYawRadians, VRUnitsPerMeter, vrInput->HeadAnchorPosition);
		strip.UVMin = vec2(0.0f, 0.0f);
		strip.UVMax = uvMax;
		render->SetMenuWorldStrips(true, &strip, 1);
	}

	// Cursor: intersect the controller ray with the panel plane (point quadPos, normal quadForward).
	vec3 rayOrigin = vrInput->RightControllerPosition; // already raw OpenXR/STAGE space, no UE1 remap
	vec3 rayDir = GetOpenXRForwardRaw(vrInput->RightControllerOrientation[0], vrInput->RightControllerOrientation[1], vrInput->RightControllerOrientation[2], vrInput->RightControllerOrientation[3]);

	float denom = dot(rayDir, quadForward);
	if (std::fabs(denom) < 0.000001f)
		return; // ray parallel to the panel
	float t = dot(quadPos - rayOrigin, quadForward) / denom;
	if (t <= 0.0f)
		return;

	vec3 hitPoint = rayOrigin + rayDir * t;
	vec3 hitRel = hitPoint - quadPos;
	float localX = dot(hitRel, quadRight);
	float localY = dot(hitRel, quadUp);

	// Outside the physical panel - leave the cursor wherever it last was.
	if (std::fabs(localX) > halfW || std::fabs(localY) > halfH)
		return;

	Point cursorPos;
	// UWindow works in the logical canvas (texture pixels / MenuUIScale), so feed it those units.
	const int menuUIScale = render->MenuUIScale();
	cursorPos.x = (localX / screenWidthMeters + 0.5) * canvasWidth / menuUIScale;
	cursorPos.y = (0.5 - localY / screenHeightMeters) * canvasHeight / menuUIScale; // screen Y grows downward, quadUp grows upward

	render->vrKeyboardCursorX = (float)cursorPos.x;
	render->vrKeyboardCursorY = (float)cursorPos.y;

	// The keyboard opens by itself when a text field (e.g. Player Name) gets keyboard focus and
	// closes when the focus moves away from it; the left stick click still toggles it by hand.
	bool editBoxFocused = IsVRMenuEditBoxFocused();
	if (editBoxFocused != vrEditBoxFocused)
	{
		vrEditBoxFocused = editBoxFocused;
		render->vrKeyboardActive = editBoxFocused;
	}
	if (vrInput->KeyboardToggleJustPressed)
		render->vrKeyboardActive = !render->vrKeyboardActive;

	if (render->vrKeyboardActive)
	{
		RenderSubsystem::VRKeyHit hk = render->VRKeyboardHitTest((float)cursorPos.x, (float)cursorPos.y);
		if (hk.hit)
		{
			OnWindowMouseMove(cursorPos); // keep the pointer visible over the keys
			if (vrInput->TriggerJustPressed)
			{
				// UWindow edit boxes only insert on KeyType while bKeyDown is set, which a KeyDown
				// sets and a KeyUp clears - so each character needs the full KeyDown/KeyType/KeyUp
				// sequence, not KeyType alone. Backspace/Enter are handled by the box's KeyDown.
				if (hk.shift)
					render->vrKeyboardShift = !render->vrKeyboardShift;
				else if (hk.backspace)
				{
					OnWindowKeyDown(EInputKey::IK_Backspace);
					OnWindowKeyUp(EInputKey::IK_Backspace);
				}
				else if (hk.enter)
				{
					OnWindowKeyDown(EInputKey::IK_Enter);
					OnWindowKeyUp(EInputKey::IK_Enter);
				}
				else if (hk.ch)
				{
					// UE1 key codes for letters/digits/space equal their uppercase ASCII value.
					char up = (hk.ch >= 'a' && hk.ch <= 'z') ? (char)(hk.ch - 32) : hk.ch;
					EInputKey kd = (EInputKey)(unsigned char)up;
					OnWindowKeyDown(kd);
					OnWindowKeyChar(std::string(1, hk.ch));
					OnWindowKeyUp(kd);
				}
			}
			return; // over a key: the keyboard owns this cursor, do not click the menu behind it
		}
	}

	OnWindowMouseMove(cursorPos);

	if (vrInput->TriggerJustPressed)
		OnWindowMouseDown(cursorPos, EInputKey::IK_LeftMouse);
	else if (vrInput->TriggerJustReleased)
		OnWindowMouseUp(cursorPos, EInputKey::IK_LeftMouse);
}

// VR sniper scope. UT's zoom only shrinks PlayerPawn.FovAngle, which the OpenXR eye projection
// ignores - so instead, while the game reports a zoomed FOV, render the world from the gun's
// viewpoint with that FOV into the shared menu texture (RenderSubsystem::RenderScopeTexture)
// and show it on a small screen fixed to the right controller, just above and ahead of the
// barrel: aiming is done by moving the hand, exactly like the crosshair. Yields to the menu
// panel (same texture) and switches off when the FOV goes back to normal.
void Engine::RunVRScope()
{
	UPlayerPawn* pawn = viewport->Actor();
	const float defaultFov = pawn && pawn->HasProperty("DefaultFOV") ? pawn->GetFloat("DefaultFOV") : 90.0f;
	bool zoomed = pawn && CameraFovAngle < defaultFov - 1.0f;
	if (!vrInput || !zoomed || vrBigScreenActive || !vrInput->RightControllerActive || !Level)
	{
		render->SetScopeWorldQuad(false, nullptr, vec2(1.0f));
		return;
	}

	Widget* vrWindow = window.get();

	// Controller basis in STAGE space (mat4::quaternion columns: 0 = right, 1 = up, 2 = back).
	const float* q = vrInput->RightControllerOrientation;
	mat4 m = mat4::quaternion(q[0], q[1], q[2], q[3]);
	vec3 right(m[0 * 4 + 0], m[0 * 4 + 1], m[0 * 4 + 2]);
	vec3 up(m[1 * 4 + 0], m[1 * 4 + 1], m[1 * 4 + 2]);
	vec3 forward = GetOpenXRForwardRaw(q[0], q[1], q[2], q[3]);
	vec3 hand = vrInput->RightControllerPosition;

	// Scope camera: a little ahead of the hand along the barrel, looking where the gun points,
	// with the game's zoomed FOV on a square image.
	vec3 cameraLocation = VRCamera::StageToWorldPosition(hand + forward * 0.15f, CameraLocation, vrInput->BodyYawRadians, VRUnitsPerMeter, vrInput->HeadAnchorPosition);
	Coords cameraRotation = VRCamera::GetEyeWorldRotation(q[0], q[1], q[2], q[3], vrInput->BodyYawRadians);
	mat4 worldToView = Coords::ViewToRenderDev().ToMatrix() * cameraRotation.Inverse().ToMatrix() * Coords::Location(cameraLocation).ToMatrix();
	// Half the game's zoomed FOV = twice its magnification ("the zoom should be stronger" on
	// real hardware - a flat-screen zoom level reads weaker on a small floating scope screen).
	float scopeFov = std::max(CameraFovAngle * 0.5f, 4.0f);
	float rprojz = (float)std::tan(radians(scopeFov) * 0.5f);
	mat4 projection = mat4::frustum(-rprojz, rprojz, -rprojz, rprojz, 1.0f, 32768.0f, handedness::left, clipzrange::zero_positive_w);

	// Square render region at the texture's top-left (the texture itself stays eye-sized so
	// the scene buffers aren't rebuilt - see RunVRMenuScreen's comment).
	int eyeWidth, eyeHeight;
	vrWindow->GetEyeImageSize(0, &eyeWidth, &eyeHeight);
	int scopeSize = std::min(std::min(eyeWidth, eyeHeight), 900);
	viewport->SetViewportRect(0, 0, scopeSize, scopeSize);
	render->RenderScopeTexture(eyeWidth, eyeHeight, worldToView, projection, cameraLocation, cameraRotation);
	vec2 uvMax((float)scopeSize / (float)eyeWidth, (float)scopeSize / (float)eyeHeight);

	// Scope screen: 32cm square (doubled from 16cm on real-hardware feedback), 30cm ahead of
	// the hand and 8cm above the barrel, facing back along the barrel so it's seen square-on
	// when sighting down the gun.
	const float screenHalf = 0.16f;
	vec3 center = hand + forward * 0.30f + up * 0.08f;
	vec3 stageCorners[4] = {
		center - right * screenHalf + up * screenHalf, // top-left (as seen from behind the gun)
		center + right * screenHalf + up * screenHalf, // top-right
		center + right * screenHalf - up * screenHalf, // bottom-right
		center - right * screenHalf - up * screenHalf, // bottom-left
	};
	vec3 worldCorners[4];
	for (int i = 0; i < 4; i++)
		worldCorners[i] = VRCamera::StageToWorldPosition(stageCorners[i], CameraLocation, vrInput->BodyYawRadians, VRUnitsPerMeter, vrInput->HeadAnchorPosition);
	render->SetScopeWorldQuad(true, worldCorners, uvMax);
}

void Engine::UpdateInput(float timeElapsed)
{
	if (timeElapsed <= 0.0f)
		return;

	TickWindow();
	if (tickDebugger)
		tickDebugger();

	if (!viewport->Actor())
		return;

	for (auto& it : activeInputButtons)
		viewport->Actor()->SetBool(it.first, true);
	for (auto& it : activeInputAxes)
	{
		if (it.first == "aMouseX" || it.first == "aMouseY")
		{
			viewport->Actor()->SetFloat(it.first, it.second.Value / (timeElapsed * 150.0f));
		}
		else
		{
			viewport->Actor()->SetFloat(it.first, it.second.Value);
		}
	}
}

void Engine::OpenWindow()
{
	if (!window)
		window = GameWindow::Create(this);

	int width = client->StartupFullscreen ? client->FullscreenViewportX : client->WindowedViewportX;
	int height = client->StartupFullscreen ? client->FullscreenViewportY : client->WindowedViewportY;
	bool fullscreen = client->StartupFullscreen;

	std::string versionString = !LaunchInfo.gameVersionString.empty() ? " (v" + LaunchInfo.gameVersionString + ")" : "";

	window->SetWindowTitle(LaunchInfo.gameName + versionString + " - Surreal Engine");
	window->SetFrameGeometry(Rect::xywh(0.0, 0.0, width, height));
	viewport->SetViewportRect(0, 0, width, height);

	if (fullscreen)
		window->ShowFullscreen();
	else
		window->ShowNormal();
}

void Engine::CloseWindow()
{
	window.reset();
}

// Per-eye VR frame loop, called from Run() in place of a single render->DrawGame(levelElapsed)
// call once vrInput exists (i.e. window->GetRenderDevice()->Viewport->IsStereoDisplay()).
// Everything preceding this in Run()'s loop body (tick, PlayerCalcView, head-yaw override) has
// already happened for this frame; this function only drives OpenXR's frame lifecycle and
// renders/presents both eyes.
void Engine::RunVR(float levelTimeElapsed)
{
	Widget* vrWindow = window.get();

	if (!vrWindow->WaitFrame())
	{
		// Nothing to render this iteration - either the session isn't running yet/anymore
		// (headset off head, app backgrounded), or it is but this particular frame shouldn't
		// render. EndFrame() is safe to call unconditionally either way: it no-ops if no
		// matching xrBeginFrame happened, and otherwise submits a zero-layer frame so the
		// compositor's frame-timing stays in sync (see OpenXRDisplayWindow::EndFrame).
		vrWindow->EndFrame();
		return;
	}

	const float uuPerMeter = VRUnitsPerMeter;

	// Raise the VR head above the script camera (see VRCamera::HeadHeightOffsetMeters). Done here,
	// after this frame's PlayerCalcView/UpdateAudio and before anything VR reads CameraLocation.
	CameraLocation.z += VRCamera::HeadHeightOffsetMeters * uuPerMeter;

	// eyeSeparationScale=1.0 (unreduced) matches Team Beef Studios' QuakeQuest's own approach
	// (Projects/Android/jni/darkplaces/gl_rmain.c: GetStereoSeparation() = vr_worldscale.value
	// * VR_GetIPD()) - one unified world scale applied directly to the real measured IPD, no
	// separate reduction factor. An artificially reduced value (tried at 0.5, then 0.3) didn't
	// fix a "the two eyes feel too different" report even near-zero - see headPose below for
	// what actually was the cause.
	const float eyeSeparationScale = 1.0f;
	vec3 headCenterPositionMeters = vec3(vrInput->HeadView.PositionX, vrInput->HeadView.PositionY, vrInput->HeadView.PositionZ);

	// The SHARED head orientation (see VRCamera.h's BuildWorldToView comment and Widget::
	// GetHeadPose()'s comment) - queried once per app-frame here and used for BOTH eyes'
	// rotation below, rather than each eye's own individually-reported orientation. This -
	// not eye separation distance - turned out to be the actual cause of a real-hardware
	// "the two eyes feel too different" report.
	MotionControllerPose headPose = vrWindow->GetHeadPose();

	render->BeginVRFrame(levelTimeElapsed);

	// "Big screen" menu - see RunVRMenuScreen()'s comment. Renders the menu texture and arms
	// (or disarms) the per-eye world quad draw below, so it must run before the eye loop.
	RunVRMenuScreen();

	// Sniper scope (shares the menu texture, so it runs after and yields to the menu).
	RunVRScope();

#ifdef ANDROID
	// Throttled once-per-~2s diagnostic dump of the actual computed VR camera/input values -
	// added to debug "camera outside the map"/"stereo distorted"/"no controller input"
	// reports from real-device testing that aren't reproducible from a screenshot (OpenXR
	// swapchain content doesn't appear in adb screencap, which only captures the flat 2D
	// compositor surface). Remove once those are confirmed fixed.
	static int vrDiagFrameCounter = 0;
	bool vrDiagLogThisFrame = (++vrDiagFrameCounter % 30) == 0; // was 120 - shortened while debugging trigger input so a quick press is more likely to land on a logged sample
	if (vrDiagLogThisFrame)
	{
		vec3 headFwd = VRCamera::GetEyeWorldRotation(headPose.OrientationX, headPose.OrientationY, headPose.OrientationZ, headPose.OrientationW, vrInput->BodyYawRadians).XAxis;
		vec3 aimFwd = vrInput->GetAimForwardWorld();
		Rotator pawnRot = viewport->Actor() ? viewport->Actor()->Rotation() : Rotator(0, 0, 0);
		__android_log_print(ANDROID_LOG_INFO, "SurrealEngine-VRDiag",
			"CameraLocation=(%.1f,%.1f,%.1f) BodyYaw=%.2f headFwd=(%.2f,%.2f,%.2f) aimFwd=(%.2f,%.2f,%.2f) pawnYaw=%d MoveAxis=(%.2f,%.2f) Fire=%d RightCtrlActive=%d TriggerRaw=%.3f"
			" | BigScreen=%d WinMouse=%d NoDrawWorld=%d MenuPositioned=%d MenuPos=(%.2f,%.2f,%.2f) Anchor=(%.2f,%.2f,%.2f) HeadPos=(%.2f,%.2f,%.2f) levelDt=%.4f",
			CameraLocation.x, CameraLocation.y, CameraLocation.z,
			vrInput->BodyYawRadians,
			headFwd.x, headFwd.y, headFwd.z, aimFwd.x, aimFwd.y, aimFwd.z, pawnRot.Yaw,
			vrInput->MoveAxisX, vrInput->MoveAxisY, vrInput->FirePressed ? 1 : 0, vrInput->RightControllerActive ? 1 : 0,
			vrInput->RightTriggerValueRaw,
			vrBigScreenActive ? 1 : 0, viewport->bShowWindowsMouse() ? 1 : 0, (console && console->bNoDrawWorld()) ? 1 : 0, vrMenuScreenPositioned ? 1 : 0,
			vrMenuScreenPos.x, vrMenuScreenPos.y, vrMenuScreenPos.z,
			vrInput->HeadAnchorPosition.x, vrInput->HeadAnchorPosition.y, vrInput->HeadAnchorPosition.z,
			headPose.PositionX, headPose.PositionY, headPose.PositionZ, levelTimeElapsed);
	}
#endif

#ifdef ANDROID
	auto vrFrameRenderStart = std::chrono::steady_clock::now();
#endif

	for (int eye = 0; eye < 2; eye++)
	{
		StereoEyeView eyeView = vrWindow->GetEyeView(eye);

		mat4 worldToView = VRCamera::BuildWorldToView(eyeView, CameraLocation, vrInput->BodyYawRadians, uuPerMeter, vrInput->HeadAnchorPosition, headCenterPositionMeters, eyeSeparationScale,
			headPose.OrientationX, headPose.OrientationY, headPose.OrientationZ, headPose.OrientationW);
		mat4 projection = VRCamera::BuildProjection(eyeView, 1.0f, 32768.0f);
		vec3 eyeLocation = VRCamera::GetEyeWorldLocation(eyeView, CameraLocation, vrInput->BodyYawRadians, uuPerMeter, vrInput->HeadAnchorPosition, headCenterPositionMeters, eyeSeparationScale);
		Coords eyeRotation = VRCamera::GetEyeWorldRotation(headPose.OrientationX, headPose.OrientationY, headPose.OrientationZ, headPose.OrientationW, vrInput->BodyYawRadians);

#ifdef ANDROID
		// Diagnostic for the "menu panel follows my head" report: the panel centre in THIS eye's
		// view space. World-fixed => moves as the head turns; head-locked => constant.
		if (vrDiagLogThisFrame && eye == 0 && vrBigScreenActive && vrMenuScreenPositioned)
		{
			vec3 panelCenterWorld = VRCamera::StageToWorldPosition(vrMenuScreenPos, CameraLocation, vrInput->BodyYawRadians, uuPerMeter, vrInput->HeadAnchorPosition);
			vec4 pv = worldToView * vec4(panelCenterWorld, 1.0f);
			__android_log_print(ANDROID_LOG_INFO, "SurrealEngine-VRDiag", "PanelView eye0=(%.1f,%.1f,%.1f) panelWorld=(%.1f,%.1f,%.1f) eyeWorld=(%.1f,%.1f,%.1f)",
				pv.x, pv.y, pv.z, panelCenterWorld.x, panelCenterWorld.y, panelCenterWorld.z, eyeLocation.x, eyeLocation.y, eyeLocation.z);
		}
#endif

		render->SetVREyeOverride(eye, worldToView, projection, eyeLocation, eyeRotation);

		int width, height;
		vrWindow->GetEyeImageSize(eye, &width, &height);
		viewport->SetViewportRect(0, 0, width, height);

#ifdef ANDROID
		if (vrDiagLogThisFrame)
		{
			__android_log_print(ANDROID_LOG_INFO, "SurrealEngine-VRDiag",
				"eye=%d size=%dx%d rawEyePos=(%.3f,%.3f,%.3f) eyeQuat=(%.3f,%.3f,%.3f,%.3f) fov(L,R,U,D)=(%.3f,%.3f,%.3f,%.3f) eyeWorldLoc=(%.1f,%.1f,%.1f)",
				eye, width, height,
				eyeView.PositionX, eyeView.PositionY, eyeView.PositionZ,
				eyeView.OrientationX, eyeView.OrientationY, eyeView.OrientationZ, eyeView.OrientationW,
				eyeView.FovAngleLeft, eyeView.FovAngleRight, eyeView.FovAngleUp, eyeView.FovAngleDown,
				eyeLocation.x, eyeLocation.y, eyeLocation.z);
		}
#endif

		// Unswapped (eye N's camera data goes to swapchain N directly). Flip-flopped multiple
		// times based on direct real-hardware reports (wrong eye -> swap -> too different ->
		// revert -> inverted again -> swap -> still not correct -> revert, this time explicitly
		// confirmed wrong). Eye indexing itself is consistent everywhere in this loop and in
		// OpenXRDisplayWindow (GetEyeView/AcquireEyeImage/etc. all keyed by the same `eye`), and
		// the runtime's own per-eye FOV asymmetry data matches "index 0 = left" per the OpenXR
		// spec - given the swap has now been tried and rejected twice, whatever's causing a
		// perceived left/right issue likely isn't a simple index swap; don't re-try this without
		// new evidence pointing specifically at it.
		int outputEye = eye;
		int imageIndex = vrWindow->AcquireEyeImage(outputEye);
		VkImage eyeImage = vrWindow->GetEyeImage(outputEye, imageIndex);
		render->DrawEyeVR(outputEye, imageIndex, eyeImage, width, height);
		vrWindow->ReleaseEyeImage(outputEye);
	}

	render->EndVRFrame();
	vrWindow->EndFrame();

#ifdef ANDROID
	// TEMP diagnostic - see the render-resolution-scale comment in openxr_display_window.cpp's
	// EnumerateViews(). Logs how long the two-eye render+present actually takes so a doubling/
	// ghosting report can be correlated against real frame time instead of guessed at - Quest
	// needs a full app-frame (both eyes) done well under ~11ms (72Hz) / ~13.9ms (90Hz) to avoid
	// the compositor's own reprojection kicking in and visibly smearing/doubling the image.
	if (vrDiagLogThisFrame)
	{
		double frameMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - vrFrameRenderStart).count();
		__android_log_print(ANDROID_LOG_INFO, "SurrealEngine-VRDiag", "Eye render+present took %.2f ms this frame", frameMs);
	}
#endif
}

void Engine::TickWindow()
{
	if (window && engine->LaunchInfo.ue1Version > 219)
	{
		if (viewport->bShowWindowsMouse() && viewport->bWindowsMouseAvailable())
			window->UnlockCursor();
		else
			window->LockCursor();
	}

	InputEvent(IK_MouseX, IST_Axis, 0);
	InputEvent(IK_MouseY, IST_Axis, 0);

	GameWindow::ProcessEvents();

	if (MouseMoveX != 0 || MouseMoveY != 0)
	{
		int dx = MouseMoveX;
		int dy = MouseMoveY;
		MouseMoveX = 0;
		MouseMoveY = 0;

		// Send to input subsystem.
		if (dx)
			InputEvent(IK_MouseX, IST_Axis, dx);
		if (dy)
			InputEvent(IK_MouseY, IST_Axis, -dy);
	}
}

void Engine::OnWindowPaint()
{
}

void Engine::OnWindowMouseMove(const Point& pos)
{
	if (playingAvi)
		return;

	if (engine->dxRootWindow && engine->dxRootWindow->OnWindowMouseMove(pos))
		return;

	if (engine->LaunchInfo.ue1Version > 219)
	{
		viewport->WindowsMouseX() = (float)(pos.x * window->GetDpiScale());
		viewport->WindowsMouseY() = (float)(pos.y * window->GetDpiScale());
	}
}

void Engine::OnWindowMouseDown(const Point& pos, EInputKey key)
{
	if (playingAvi)
		return;

	if (engine->dxRootWindow && engine->dxRootWindow->OnWindowMouseDown(pos, key))
		return;

	InputEvent(key, IST_Press);
}

void Engine::OnWindowMouseDoubleclick(const Point& pos, EInputKey key)
{
	if (engine->dxRootWindow && engine->dxRootWindow->OnWindowMouseDoubleclick(pos, key))
		return;

	// Double click event sequence is: mouse down, up, doubleclick, up
	// Since we don't have double click events in the UE1 event system, the second click
	// should be treated as a normal mouse down event.
	InputEvent(key, IST_Press);
}

void Engine::OnWindowMouseUp(const Point& pos, EInputKey key)
{
	if (playingAvi)
		return;

	if (engine->dxRootWindow && engine->dxRootWindow->OnWindowMouseUp(pos, key))
		return;

	InputEvent(key, IST_Release);
}

void Engine::OnWindowMouseWheel(const Point& pos, EInputKey key)
{
	if (playingAvi)
		return;

	if (engine->dxRootWindow && engine->dxRootWindow->OnWindowMouseWheel(pos, key))
		return;

	InputEvent(key, IST_Press);
	InputEvent(key, IST_Release);
}

void Engine::OnWindowRawMouseMove(int dx, int dy)
{
	if (playingAvi)
		return;

	if (engine->dxRootWindow && engine->dxRootWindow->OnWindowRawMouseMove(dx, dy))
		return;

	MouseMoveX += dx;
	MouseMoveY += dy;
}

void Engine::OnWindowKeyChar(std::string chars)
{
	if (playingAvi)
		return;

	if (engine->dxRootWindow && engine->dxRootWindow->OnWindowKeyChar(chars))
		return;

	Key(chars);
}

void Engine::OnWindowKeyDown(EInputKey key)
{
	if (playingAvi)
	{
		if (key == EInputKey::IK_Escape)
			skipAvi = true;
		return;
	}

	if (engine->dxRootWindow && engine->dxRootWindow->OnWindowKeyDown(key))
		return;

	InputEvent(key, IST_Press);
}

void Engine::OnWindowKeyUp(EInputKey key)
{
	if (playingAvi)
		return;

	if (engine->dxRootWindow && engine->dxRootWindow->OnWindowKeyUp(key))
		return;

	InputEvent(key, IST_Release);
}

void Engine::OnWindowGeometryChanged()
{
}

void Engine::OnWindowClose()
{
	quit = true;
}

void Engine::OnWindowActivated()
{
	//SetPause(false);
}

void Engine::OnWindowDeactivated()
{
	//SetPause(true);
}

void Engine::OnWindowDpiScaleChanged()
{
}

void Engine::LockCursor()
{
	if (window)
		window->LockCursor();
}

void Engine::UnlockCursor()
{
	if (window)
		window->UnlockCursor();
}

void Engine::Key(std::string key)
{
	if (Frame::RunState != FrameRunState::Running || playingAvi)
		return;

	if (LaunchInfo.IsDeusEx() && key == "~" && window->GetKeyState(IK_Shift))
	{
		// Did they REALLY hack UE1 to do something as lame as this??
		console->GotoState("Typing", {});
	}

	for (char c : key)
	{
		CallEvent(console, EventName::KeyType, { ExpressionValue::ByteValue(c) });
	}
}

void Engine::InputEvent(EInputKey key, EInputType type, int delta)
{
	if (Frame::RunState != FrameRunState::Running || playingAvi)
		return;

	bool handled = CallEvent(console, EventName::KeyEvent, { ExpressionValue::ByteValue(key), ExpressionValue::ByteValue(type), ExpressionValue::FloatValue((float)delta) }).ToBool();
	
	if (!handled)
	{
		if ((type == EInputType::IST_Press || type == EInputType::IST_Axis) && key >= 0 && key < 256)
		{
			if (type == EInputType::IST_Press)
				delta = 20;
			else
				delta *= 16;

			for (const std::string& command : GetSubcommands(keybindings[keynames[key]]))
			{
				auto it = inputAliases.find(command);
				if (it != inputAliases.end())
				{
					InputCommand(it->second, key, delta);
				}
				else
				{
					InputCommand(command, key, delta);
				}
			}
		}
		else if (type == EInputType::IST_Release)
		{
			for (auto it = activeInputButtons.begin(); it != activeInputButtons.end();)
			{
				if (it->second == key)
				{
					viewport->Actor()->SetBool(it->first, false);
					it = activeInputButtons.erase(it);
				}
				else
				{
					++it;
				}
			}

			for (auto it = activeInputAxes.begin(); it != activeInputAxes.end();)
			{
				if (it->second.Key == key)
				{
					viewport->Actor()->SetFloat(it->first, 0.0f);
					it = activeInputAxes.erase(it);
				}
				else
				{
					++it;
				}
			}
		}
	}
}

bool Engine::ExecCommand(const Array<std::string>& args)
{
	for (UObject* target : { static_cast<UObject*>(viewport->Actor()), static_cast<UObject*>(console) })
	{
		if (!target)
			continue;

		UFunction* func = FindEventFunction(target, args[0]);
		if (func && AllFlags(func->FuncFlags, FunctionFlags::Exec))
		{
			Array<ExpressionValue> vmArgs;
			int argindex = 0;
			for (UField* field = func->Children; field != nullptr; field = field->Next)
			{
				UProperty* prop = UObject::TryCast<UProperty>(field);
				if (!prop)
					continue;

				if (AllFlags(prop->PropFlags, PropertyFlags::ReturnParm))
					continue;

				if (!AllFlags(prop->PropFlags, PropertyFlags::Parm))
					continue;

				if (argindex + 1 < args.size())
				{
					const std::string& arg = args[1 + argindex];
					switch (prop->ValueType)
					{
					case ExpressionValueType::Nothing: vmArgs.push_back(ExpressionValue::NothingValue()); break;
					case ExpressionValueType::ValueByte: vmArgs.push_back(ExpressionValue::ByteValue(std::atoi(arg.c_str()))); break;
					case ExpressionValueType::ValueInt: vmArgs.push_back(ExpressionValue::IntValue(std::atoi(arg.c_str()))); break;
					case ExpressionValueType::ValueBool: vmArgs.push_back(ExpressionValue::BoolValue(arg == "1" || arg == "true")); break;
					case ExpressionValueType::ValueFloat: vmArgs.push_back(ExpressionValue::FloatValue((float)std::atof(arg.c_str()))); break;
					case ExpressionValueType::ValueString: vmArgs.push_back(ExpressionValue::StringValue(arg)); break;
					case ExpressionValueType::ValueName: vmArgs.push_back(ExpressionValue::NameValue(arg)); break;
					default: LogMessage("Unsupported value type found in Engine.ExecCommand"); return false;
					}
				}
				else if (AllFlags(prop->PropFlags, PropertyFlags::OptionalParm))
				{
					vmArgs.push_back(ExpressionValue::NothingValue());
				}
				else
				{
					switch (prop->ValueType)
					{
					case ExpressionValueType::Nothing: vmArgs.push_back(ExpressionValue::NothingValue()); break;
					case ExpressionValueType::ValueByte: vmArgs.push_back(ExpressionValue::ByteValue(0)); break;
					case ExpressionValueType::ValueInt: vmArgs.push_back(ExpressionValue::IntValue(0)); break;
					case ExpressionValueType::ValueBool: vmArgs.push_back(ExpressionValue::BoolValue(false)); break;
					case ExpressionValueType::ValueFloat: vmArgs.push_back(ExpressionValue::FloatValue(0.0f)); break;
					case ExpressionValueType::ValueString: vmArgs.push_back(ExpressionValue::StringValue({})); break;
					case ExpressionValueType::ValueName: vmArgs.push_back(ExpressionValue::NameValue({})); break;
					default: LogMessage("Unsupported value type found in Engine.ExecCommand"); return false;
					}
				}
				argindex++;
			}

			CallEvent(target, func->Name, vmArgs);
			return true;
		}
	}

	return false;
}

void Engine::InputCommand(const std::string& commands, EInputKey key, int delta)
{
	for (const std::string& commandline : GetSubcommands(commands))
	{
		Array<std::string> args = GetArgs(commandline);
		if (!args.empty())
		{
			std::string command = args[0];
			for (char& c : command) c = std::tolower(c);

			if (command == "button" && args.size() == 2)
			{
				activeInputButtons[args[1]] = key;
			}
			else if (command == "axis" && args.size() == 3)
			{
				float speed = 1.0f;
				if (args[2].size() > 6 && args[2].substr(0, 6) == "Speed=")
					speed = (float)std::atof(args[2].substr(6).c_str());
				activeInputAxes[args[1]] = { speed * delta, key };
			}
			else
			{
				ExecCommand(args);
			}
		}
	}
}

void Engine::SetPause(bool value)
{
	m_GamePaused = value;
}

void Engine::LogGamePackageSHA1Sums() const
{
	auto systemPath = packages->GetSystemFolderPath();

	LogMessage("SHA1Sums of some system files:");
	LogMessage("Core.u: " + SHA1Sum::of_file(systemPath / "Core.u"));
	LogMessage("Engine.u: " + SHA1Sum::of_file(systemPath / "Engine.u"));

	if (packages->IsUnrealTournament())
	{
		LogMessage("Botpack.u: " + SHA1Sum::of_file(systemPath / "Botpack.u"));
		LogMessage("UnrealI.u: " + SHA1Sum::of_file(systemPath / "UnrealI.u"));
		LogMessage("UnrealShare.u: " + SHA1Sum::of_file(systemPath / "UnrealShare.u"));
	}
	else if (packages->IsUnreal1())
	{
		LogMessage("UnrealI.u: " + SHA1Sum::of_file(systemPath / "UnrealI.u"));
		LogMessage("UnrealShare.u: " + SHA1Sum::of_file(systemPath / "UnrealShare.u"));
	}
	else if (packages->IsKlingonHonorGuard())
	{
		LogMessage("Klingons.u: " + SHA1Sum::of_file(systemPath / "Klingons.u"));
	}
}

void Engine::GetLevelInfoObject()
{
	LevelInfo = UObject::Cast<ULevelInfo>(LevelPackage->GetUObject("LevelInfo", "LevelInfo0"));
	if (LaunchInfo.ue1Version < 300) // Unknown when this changed
	{
		for (int grr = 1; !LevelInfo && grr < 20; grr++)
			LevelInfo = UObject::Cast<ULevelInfo>(LevelPackage->GetUObject("LevelInfo", "LevelInfo" + std::to_string(grr)));
	}
	if (!LevelInfo)
		Exception::Throw("Could not find the LevelInfo object for " + LevelPackage->GetPackageName().ToString() + "!");
}

void Engine::GetLevelObject()
{
	Level = UObject::Cast<ULevel>(LevelPackage->GetUObject("Level", "MyLevel"));
	if (!Level)
		Exception::Throw("Could not find the Level object for" + LevelPackage->GetPackageName().ToString() + "!");

	if (LaunchInfo.IsDeusEx())
	{
		// Also try to find DeusExLevelInfo
		DeusExLevelInfo = UObject::Cast<UDeusExLevelInfo>(LevelPackage->GetUObject("DeusExLevelInfo", "DeusExLevelInfo0"));

		// Didn't find it. Keep searching.
		for (int grr = 1; !DeusExLevelInfo && grr < 20; grr++)
			DeusExLevelInfo = UObject::Cast<UDeusExLevelInfo>(LevelPackage->GetUObject("DeusExLevelInfo", "DeusExLevelInfo" + std::to_string(grr)));

		// Entry.dx does not have a DeusExLevelInfo
		/*
		if (!DeusExLevelInfo)
			Exception::Throw("Could not find the DeusExLevelInfo object for " + url.Map + "!");
		*/

		// Link giveObject for all events in the mission
		if (UConversationList* conList = GetDeusExMission())
		{
			for (UConItem* item = conList->conversations(); item; item = item->Next())
			{
				auto conversation = UObject::Cast<UConversation>(item->ConObject());
				for (UConEvent* e = conversation->eventList(); e; e = e->nextEvent())
				{
					EEventType eventType = (EEventType)e->eventType();
					if (eventType == EEventType::TransferObject)
					{
						if (auto transfer = UObject::Cast<UConEventTransferObject>(e))
						{
							UClass* cls = engine->packages->FindClass("DeusEx." + transfer->ObjectName());
							if (!cls)
								LogMessage("Could not find class for TransferObject: " + transfer->ObjectName());
							transfer->giveObject() = cls;
						}
					}
					else if (eventType == EEventType::CheckObject)
					{
						if (auto eventCheckObject = UObject::Cast<UConEventCheckObject>(e))
						{
							if (eventCheckObject->ObjectName().starts_with("NK_"))
							{
								eventCheckObject->checkObject() = nullptr;
							}
							else
							{
								UClass* cls = engine->packages->FindClass("DeusEx." + eventCheckObject->ObjectName());
								if (!cls)
									LogMessage("Could not find class for CheckObject: " + eventCheckObject->ObjectName());
								eventCheckObject->checkObject() = cls;
							}
						}
					}
				}

				// Remove comments from event lists:
				while (conversation->eventList() && (EEventType)conversation->eventList()->eventType() == EEventType::Comment)
					conversation->eventList() = conversation->eventList()->nextEvent();
				UConEvent* cur = conversation->eventList();
				while (cur != nullptr)
				{
					auto next = cur->nextEvent();
					if (next && (EEventType)next->eventType() == EEventType::Comment)
					{
						cur->nextEvent() = next->nextEvent();
					}
					else
					{
						cur = next;
					}
				}
			}
		}
	}
}

void Engine::LinkActorsToLevel()
{
	// Link actors to the level
	for (UActor* actor : Level->Actors)
	{
		if (actor)
		{
			actor->XLevel() = Level;
			Level->Collision.AddToCollision(actor);
		}
	}

	// BasedActors (who is standing on me, so I carry them when I move) is native, runtime-only
	// state - never serialized - while ActorBase() (what am I standing on) is a normal property
	// that does round-trip through a package/save. Without this, an actor that starts a level (or
	// a load) already resting on a mover has its own ActorBase() pointer intact, but the mover's
	// own BasedActors list is empty, so the mover has no way to carry it along once it moves.
	for (UActor* actor : Level->Actors)
	{
		if (actor)
			actor->RelinkBasedActor();
	}
}

const char* Engine::keynames[256] =
{
	/*00*/ "None", "LeftMouse", "RightMouse", "Cancel",
	/*04*/ "MiddleMouse", "Unknown05", "Unknown06", "Unknown07",
	/*08*/ "Backspace", "Tab", "Unknown0A", "Unknown0B",
	/*0C*/ "Unknown0C", "Enter", "Unknown0E", "Unknown0F",
	/*10*/ "Shift", "Ctrl", "Alt", "Pause",
	/*14*/ "CapsLock", "Unknown15", "Unknown16", "Unknown17",
	/*18*/ "Unknown18", "Unknown19", "Unknown1A", "Escape",
	/*1C*/ "Unknown1C", "Unknown1D", "Unknown1E", "Unknown1F",
	/*20*/ "Space", "PageUp", "PageDown", "End",
	/*24*/ "Home", "Left", "Up", "Right",
	/*28*/ "Down", "Select", "Print", "Execute",
	/*2C*/ "PrintScrn", "Insert", "Delete", "Help",
	/*30*/ "0", "1", "2", "3",
	/*34*/ "4", "5", "6", "7",
	/*38*/ "8", "9", "Unknown3A", "Unknown3B",
	/*3C*/ "Unknown3C", "Unknown3D", "Unknown3E", "Unknown3F",
	/*40*/ "Unknown40", "A", "B", "C",
	/*44*/ "D", "E", "F", "G",
	/*48*/ "H", "I", "J", "K",
	/*4C*/ "L", "M", "N", "O",
	/*50*/ "P", "Q", "R", "S",
	/*54*/ "T", "U", "V", "W",
	/*58*/ "X", "Y", "Z", "Unknown5B",
	/*5C*/ "Unknown5C", "Unknown5D", "Unknown5E", "Unknown5F",
	/*60*/ "NumPad0", "NumPad1", "NumPad2", "NumPad3",
	/*64*/ "NumPad4", "NumPad5", "NumPad6", "NumPad7",
	/*68*/ "NumPad8", "NumPad9", "GreyStar", "GreyPlus",
	/*6C*/ "Separator", "GreyMinus", "NumPadPeriod", "GreySlash",
	/*70*/ "F1", "F2", "F3", "F4",
	/*74*/ "F5", "F6", "F7", "F8",
	/*78*/ "F9", "F10", "F11", "F12",
	/*7C*/ "F13", "F14", "F15", "F16",
	/*80*/ "F17", "F18", "F19", "F20",
	/*84*/ "F21", "F22", "F23", "F24",
	/*88*/ "Unknown88", "Unknown89", "Unknown8A", "Unknown8B",
	/*8C*/ "Unknown8C", "Unknown8D", "Unknown8E", "Unknown8F",
	/*90*/ "NumLock", "ScrollLock", "Unknown92", "Unknown93",
	/*94*/ "Unknown94", "Unknown95", "Unknown96", "Unknown97",
	/*98*/ "Unknown98", "Unknown99", "Unknown9A", "Unknown9B",
	/*9C*/ "Unknown9C", "Unknown9D", "Unknown9E", "Unknown9F",
	/*A0*/ "LShift", "RShift", "LControl", "RControl",
	/*A4*/ "UnknownA4", "UnknownA5", "UnknownA6", "UnknownA7",
	/*A8*/ "UnknownA8", "UnknownA9", "UnknownAA", "UnknownAB",
	/*AC*/ "UnknownAC", "UnknownAD", "UnknownAE", "UnknownAF",
	/*B0*/ "UnknownB0", "UnknownB1", "UnknownB2", "UnknownB3",
	/*B4*/ "UnknownB4", "UnknownB5", "UnknownB6", "UnknownB7",
	/*B8*/ "UnknownB8", "UnknownB9", "Semicolon", "Equals",
	/*BC*/ "Comma", "Minus", "Period", "Slash",
	/*C0*/ "Tilde", "UnknownC1", "UnknownC2", "UnknownC3",
	/*C4*/ "UnknownC4", "UnknownC5", "UnknownC6", "UnknownC7",
	/*C8*/ "Joy1", "Joy2", "Joy3", "Joy4",
	/*CC*/ "Joy5", "Joy6", "Joy7", "Joy8",
	/*D0*/ "Joy9", "Joy10", "Joy11", "Joy12",
	/*D4*/ "Joy13", "Joy14", "Joy15", "Joy16",
	/*D8*/ "UnknownD8", "UnknownD9", "UnknownDA", "LeftBracket",
	/*DC*/ "Backslash", "RightBracket", "SingleQuote", "UnknownDF",
	/*E0*/ "JoyX", "JoyY", "JoyZ", "JoyR",
	/*E4*/ "MouseX", "MouseY", "MouseZ", "MouseW",
	/*E8*/ "JoyU", "JoyV", "UnknownEA", "UnknownEB",
	/*EC*/ "MouseWheelUp", "MouseWheelDown", "Unknown10E", "Unknown10F",
	/*F0*/ "JoyPovUp", "JoyPovDown", "JoyPovLeft", "JoyPovRight",
	/*F4*/ "UnknownF4", "UnknownF5", "Attn", "CrSel",
	/*F8*/ "ExSel", "ErEof", "Play", "Zoom",
	/*FC*/ "NoName", "PA1", "OEMClear", ""
};
