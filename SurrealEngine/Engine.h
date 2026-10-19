#pragma once

#include "Utils/Logger.h"
#include "Math/vec.h"
#include "Math/mat.h"
#include "Math/floating.h"
#include "RenderDevice/RenderDevice.h"
#include "GameWindow.h"
#include "Packages/Engine/Actors/UActor.h"
#include "UnrealURL.h"
#include "GameFolder.h"
#include <set>
#include <list>

class RenderSubsystem;
class PackageManager;
class UObject;
class ULevel;
class UModel;
class GameWindow;
class UTexture;
class UActor;
class UFont;
class UMesh;
class ULodMesh;
class USkeletalMesh;
class ULevelSummary;
class ULevelInfo;
class UZoneInfo;
class UClient;
class UViewport;
class UCanvas;
class UConsole;
class UPlayerPawn;
class UGameInfo;
class UGameReplicationInfo;
class UPlayerReplicationInfo;
class UGameEngine;
class USurrealRenderDevice;
class USurrealAudioDevice;
class USurrealNetworkDevice;
class USurrealClient;
class BspSurface;
class BspNode;
class LightMapIndex;
class FrustumPlanes;
class AudioSubsystem;
class Rotator;
class ExpressionValue;
class UnrealURL;
class VideoPlayer;
class UnrealMipmap;
class UFloatProperty;
class UConversationMissionList;
class UConversationList;
class UDXSaveInfo;
class UDeusExLevelInfo;
class URootWindow;
class UGC;
struct TextureInfo;
struct SceneNode;
struct SurfaceFacet;
struct MeshFace;

static constexpr int32_t DONT_SAVE_GAME = -2; // Since -1 might be used as the autosave slot in some UE1 games...

class Engine : public GameWindowHost
{
public:
	Engine(GameLaunchInfo launchinfo);
	~Engine();

	void Run();
	void ClientTravel(const std::string& URL, ETravelType travelType, bool transferItems);
	UnrealURL GetDefaultURL(const std::string& map);
	void LoadEntryMap();
	void LoadMap(const UnrealURL& url, const std::map<std::string, std::string>& travelInfo = {});
	void LoadFromSaveFile(const UnrealURL& url);
	void SaveGameToSlot(int32_t slotNum, const std::string& saveDescription) const;
	void UnloadMap();
	void LoginPlayer();
	void PossessSavedPlayer();

	UObject* FindObject(NameString name, NameString className);

	std::string ConsoleCommand(UObject* context, const std::string& command, BitfieldBool& found);

	void UpdateInput(float timeElapsed);
	void InputCommand(const std::string& command, EInputKey key, int delta);

	void LockCursor();
	void UnlockCursor();

	bool ExecCommand(const Array<std::string>& args);
	Array<std::string> GetArgs(const std::string& commandline);
	Array<std::string> GetSubcommands(const std::string& commandline);

	void PlayAVI(const Array<std::string>& args);
	UnrealMipmap* PlayVideo(VideoPlayer* video, UnrealMipmap* background);

	UConversationList* GetDeusExMission();

	void UpdateAudio();

	void OpenWindow();
	void CloseWindow();
	void TickWindow();

	void Key(std::string key);
	void InputEvent(EInputKey key, EInputType type, int delta = 0);

	void OnWindowPaint() override;
	void OnWindowMouseMove(const Point& pos) override;
	void OnWindowMouseDown(const Point& pos, EInputKey key) override;
	void OnWindowMouseDoubleclick(const Point& pos, EInputKey key) override;
	void OnWindowMouseUp(const Point& pos, EInputKey key) override;
	void OnWindowMouseWheel(const Point& pos, EInputKey key) override;
	void OnWindowRawMouseMove(int dx, int dy) override;
	void OnWindowKeyChar(std::string chars) override;
	void OnWindowKeyDown(EInputKey key) override;
	void OnWindowKeyUp(EInputKey key) override;
	void OnWindowGeometryChanged() override;
	void OnWindowClose() override;
	void OnWindowActivated() override;
	void OnWindowDeactivated() override;
	void OnWindowDpiScaleChanged() override;

	void SetPause(bool value);

	UZoneInfo* GetZoneActor(int zoneIndex);

	std::string ParseClassName(std::string className);

	UGameEngine* gameengine = nullptr;
	USurrealRenderDevice* renderdev = nullptr;
	USurrealAudioDevice* audiodev = nullptr;
	USurrealNetworkDevice* netdev = nullptr;
	USurrealClient* client = nullptr;
	UViewport* viewport = nullptr;
	UCanvas* canvas = nullptr;
	UGC* dxgc = nullptr;
	UDXSaveInfo* dxSaveInfo = nullptr;
	UConversationMissionList* dxConMissionList = nullptr;
	UConsole* console = nullptr;
	URootWindow* dxRootWindow = nullptr;

	UFloatProperty* floatprop = nullptr;

	ULevelInfo* EntryLevelInfo = nullptr;
	ULevel* EntryLevel = nullptr;
	UGameInfo* EntryGameInfo = nullptr;
	Package* EntryLevelPackage = nullptr;

	double TotalTime = 0.0;

	ULevelInfo* LevelInfo = nullptr;
	ULevel* Level = nullptr;
	Package* LevelPackage;
	UGameInfo* GameInfo = nullptr;
	UTexture* DefaultTexture = nullptr;

	Package* deusExPackage = nullptr;
	UDeusExLevelInfo* DeusExLevelInfo = nullptr;
	struct
	{
		UnrealURL URL;
		ETravelType TravelType = ETravelType::TRAVEL_Absolute;
		bool TransferItems = false;
	} ClientTravelInfo;

	struct
	{
		int32_t SaveGameSlot = DONT_SAVE_GAME;
		std::string SaveGameDescription;
	} SaveGameInfo;

	GameLaunchInfo LaunchInfo;
	std::unique_ptr<PackageManager> packages;
	std::unique_ptr<GameWindow> window; // TODO: Move into UViewport
	std::unique_ptr<RenderSubsystem> render;

	// VR: non-null only when window->GetRenderDevice()'s Viewport is an OpenXR stereo
	// display (see VR/VRInput.h, VR/VRRenderLoop.cpp). Constructed in Run() right after
	// OpenWindow(), once it's known whether this is a VR session.
	std::unique_ptr<class VRInput> vrInput;
	void RunVR(float levelTimeElapsed);
	void UpdateVRInput(float timeElapsed);
	void RunVRMenuScreen();
	bool IsVRMenuEditBoxFocused();
	void RunVRScope();

	// Whether the "big screen" panel should be showing right now. For UT and later this
	// mirrors Viewport.bShowWindowsMouse (UWindowConsole's "a mouse-driven menu is open" flag)
	// every frame; for older games it's a plain toggle flipped by the left controller's Menu
	// button - see UpdateVRInput(), Engine.cpp. (Three earlier detection attempts -
	// dxRootWindow, UHUD::MainMenu(), Level==EntryLevel - each missed real UT menu screens;
	// bShowWindowsMouse is the one the UWindow system itself maintains.)
	bool vrBigScreenActive = false;
	bool vrBigScreenManualToggle = false; // pre-UWindow games only - see UpdateVRInput()

	// The player Pawn seen by the previous UpdateVRInput() - a change means a (re)spawn, at
	// which point BodyYawRadians is synced from the new Pawn's spawn rotation (see there).
	class UPlayerPawn* vrLastPawn = nullptr;

	// Seconds left during which the crosshair aim override (VRAimOverrideScope, Engine.cpp)
	// stays applied to the level tick after the trigger/grip was RELEASED. Weapons that fire on
	// release - UT's rocket launcher launches its loaded rockets from state code that waits for
	// bFire == 0 - would otherwise fire on the first tick where the bFire-gated override is no
	// longer active, i.e. along the body yaw (observed on Quest 3 with the rocket launcher,
	// while every press-fired weapon hit the crosshair).
	float vrAimOverrideAfterRelease = 0.0f;

	// The "big screen" quad's OpenXR/STAGE-space pose (meters; center position + yaw-only
	// orientation quaternion), captured ONCE when the menu transitions closed->open and held
	// fixed until it closes again - see RunVRMenuScreen()'s comment for why (a real-hardware
	// report that recomputing position every frame from the live head position, matching
	// QuakeQuest's own literal formula, made the panel follow the head rather than
	// a static screen). Converted into UE1 world space fresh each frame for rendering
	// (VRCamera::StageToWorldPosition) so it follows the player through any camera
	// teleport/map change while staying still in the physical room.
	bool vrMenuScreenPositioned = false;
	bool vrEditBoxFocused = false; // last IsVRMenuEditBoxFocused() result, opens/closes the VR keyboard
	vec3 vrMenuScreenPos = vec3(0.0f);
	vec3 vrMenuScreenForward = vec3(0.0f, 0.0f, -1.0f); // horizontal unit vector, player -> screen

	int MouseMoveX = 0;
	int MouseMoveY = 0;

	float CalcTimeElapsed();

	UActor* CameraActor = nullptr;
	vec3 CameraLocation = vec3(0.0f);
	Rotator CameraRotation = Rotator(0,0,0);
	float CameraFovAngle = 95.0f;

	std::string windowingSystemName;

	// Collision debug
	BspNode* PlayerBspNode = nullptr;
	vec3 PlayerHitNormal = vec3(0.0f);
	vec3 PlayerHitLocation = vec3(0.0f);

	bool quit = false;

	uint64_t lastTime = 0;

	void LoadEngineSettings();

	void LoadKeybindings();
	std::map<std::string, std::string> keybindings;
	std::map<std::string, std::string> inputAliases;
	static const char* keynames[256];

	struct ActiveInputAxis
	{
		float Value;
		EInputKey Key;
	};

	std::map<std::string, EInputKey> activeInputButtons;
	std::map<std::string, ActiveInputAxis> activeInputAxes;

	std::function<void()> tickDebugger;

	bool getEditorMode() const { return m_EditorMode; }
	void setEditorMode(const bool value) { m_EditorMode = value; }

	bool getDXWindowDebugMode() const { return m_DrawDebugDXWindowHierarchy; }

private:
	std::map<std::string, std::string> CreateTravelInfo(bool transferItems);

	void LogGamePackageSHA1Sums() const;
	void GetLevelInfoObject();
	void GetLevelObject();
	void LinkActorsToLevel();

	bool m_EditorMode = false; // Set this to true to allow rendering of invisible polys.
	bool m_GamePaused = false;

	bool m_DrawDebugDXWindowHierarchy = false; // If set to true, engine will show the Deus Ex window hierarchy

	bool khgSplashScreen = false;
	bool playingAvi = false;
	bool skipAvi = false;
};

extern Engine* engine;
