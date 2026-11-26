#include "HUICRSyncSpaceSetupSubsystem.h"

#if PLATFORM_ANDROID
#include "AndroidPermissionCallbackProxy.h"
#include "AndroidPermissionFunctionLibrary.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HUICRSyncHMDManagerBase.h"
#include "HUICRSyncScreen.h"
#include "MRUtilityKitAnchor.h"
#include "MRUtilityKitAnchorActorSpawner.h"
#include "MRUtilityKitRoom.h"
#include "MRUtilityKitSubsystem.h"
#include "Misc/CoreDelegates.h"
#include "TimerManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHUICRSpaceSetup, Log, All);

namespace HUICRSpaceSetup
{
	const TCHAR* const ScenePermission = TEXT("com.oculus.permission.USE_SCENE");

	bool SpawnsCalibrationScreens(const AMRUKAnchorActorSpawner& Spawner)
	{
		const FMRUKSpawnGroup* Group = Spawner.SpawnGroups.Find(FMRUKLabels::Screen);
		return Group && Group->Actors.ContainsByPredicate([](const FMRUKSpawnActor& Entry)
		{
			return Entry.Actor && Entry.Actor->IsChildOf(AHUICRSyncScreen::StaticClass());
		});
	}
}
#endif

#define LOCTEXT_NAMESPACE "HUICRSpaceSetup"

void UHUICRSyncSpaceSetupSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
#if PLATFORM_ANDROID
	FWorldDelegates::OnWorldInitializedActors.AddWeakLambda(this, [this](const UWorld::FActorsInitializedParams& Params)
	{
		if (Params.World && Params.World->GetGameInstance() == GetGameInstance())
		{
			RefreshSpaceSetup();
		}
	});
#endif
}

void UHUICRSyncSpaceSetupSubsystem::Deinitialize()
{
#if PLATFORM_ANDROID
	FWorldDelegates::OnWorldInitializedActors.RemoveAll(this);
#endif
	ResetWorld();
	Super::Deinitialize();
}

void UHUICRSyncSpaceSetupSubsystem::ResetWorld()
{
#if PLATFORM_ANDROID
	if (UWorld* Previous = SetupWorld.Get())
	{
		Previous->GetTimerManager().ClearTimer(RoomTimer);
	}
	if (bRequiresScreen)
	{
		if (UMRUKSubsystem* MRUK = GetGameInstance()->GetSubsystem<UMRUKSubsystem>())
		{
			MRUK->OnCaptureComplete.RemoveDynamic(this, &ThisClass::CaptureFinished);
			MRUK->OnSceneLoaded.RemoveDynamic(this, &ThisClass::SceneLoaded);
		}
		UAndroidPermissionCallbackProxy::GetInstance()->OnPermissionsGrantedDelegate.RemoveAll(this);
		FCoreDelegates::ApplicationHasEnteredForegroundDelegate.RemoveAll(this);
	}
#endif
	SetupWorld.Reset();
	LastObservedRoom.Reset();
	ScreenSpawners.Reset();
	PhysicalScreens.Reset();
	Phase = EPhase::Idle;
	bLoading = false;
	bRequiresScreen = false;
	bReloadAfterCurrentLoad = false;
	bCheckInitialLoad = false;
	bPermissionGranted = false;
}

void UHUICRSyncSpaceSetupSubsystem::RefreshSpaceSetup()
{
#if PLATFORM_ANDROID
	UWorld* World = GetWorld();
	if (!World || !World->IsGameWorld() || SetupWorld.Get() == World)
	{
		return;
	}
	ResetWorld();
	SetupWorld = World;
	for (TActorIterator<AMRUKAnchorActorSpawner> It(World); It; ++It)
	{
		if (HUICRSpaceSetup::SpawnsCalibrationScreens(**It))
		{
			ScreenSpawners.Add(*It);
		}
	}
	bRequiresScreen = !ScreenSpawners.IsEmpty();
	if (bRequiresScreen)
	{
		bPermissionGranted = UAndroidPermissionFunctionLibrary::CheckPermission(HUICRSpaceSetup::ScenePermission);
		bCheckInitialLoad = bPermissionGranted;
		UAndroidPermissionCallbackProxy::GetInstance()->OnPermissionsGrantedDelegate.AddUObject(this, &ThisClass::HandlePermissions);
		FCoreDelegates::ApplicationHasEnteredForegroundDelegate.AddUObject(this, &ThisClass::OnForeground);
		if (UMRUKSubsystem* MRUK = GetGameInstance()->GetSubsystem<UMRUKSubsystem>())
		{
			MRUK->OnCaptureComplete.AddUniqueDynamic(this, &ThisClass::CaptureFinished);
			MRUK->OnSceneLoaded.AddUniqueDynamic(this, &ThisClass::SceneLoaded);
		}
		World->GetTimerManager().SetTimer(RoomTimer, this, &ThisClass::CheckCurrentRoom, 0.5f, true);
		UpdateScreens(false);
	}
	OnSpaceSetupChanged.Broadcast();
#endif
}

bool UHUICRSyncSpaceSetupSubsystem::IsCalibrationScreenReady() const
{
#if PLATFORM_ANDROID
	if (UWorld* World = GetWorld(); World && World != SetupWorld.Get())
	{
		// A direct calibration request can arrive before the menu or the world-start notification.
		for (TActorIterator<AMRUKAnchorActorSpawner> It(World); It; ++It)
		{
			if (HUICRSpaceSetup::SpawnsCalibrationScreens(**It))
			{
				return false;
			}
		}
		return true;
	}
	if (bRequiresScreen)
	{
		const auto* MRUK = GetGameInstance()->GetSubsystem<UMRUKSubsystem>();
		if (!MRUK || MRUK->SceneLoadStatus != EMRUKInitStatus::Complete ||
			!LastObservedRoom.IsValid() || MRUK->GetCurrentRoom() != LastObservedRoom.Get())
		{
			return false;
		}
	}
#endif
	return !bRequiresScreen || (CanLaunchSpaceSetup() && !bLoading && bPermissionGranted &&
		Phase != EPhase::Reloading && Phase != EPhase::LoadFailed &&
		PhysicalScreens.ContainsByPredicate([](const TWeakObjectPtr<AActor>& Screen) { return Screen.IsValid(); }));
}

bool UHUICRSyncSpaceSetupSubsystem::CanLaunchSpaceSetup() const
{
	// Quest may keep a room read pending until tracking is available. The system setup UI
	// can be opened independently of that read, even if calibration is still unavailable.
	return bRequiresScreen && Phase != EPhase::WaitingForPermission && Phase != EPhase::Capturing;
}

bool UHUICRSyncSpaceSetupSubsystem::IsScreenFromOtherRoom(const AActor* Screen) const
{
#if PLATFORM_ANDROID
	if (bRequiresScreen && IsValid(Screen))
	{
		if (const AMRUKAnchor* Anchor = Cast<AMRUKAnchor>(Screen->GetAttachParentActor()))
		{
			if (const auto* MRUK = GetGameInstance()->GetSubsystem<UMRUKSubsystem>())
			{
				return Anchor->Room != MRUK->GetCurrentRoom();
			}
		}
	}
#endif
	return false;
}

bool UHUICRSyncSpaceSetupSubsystem::LaunchSpaceSetup()
{
#if PLATFORM_ANDROID
	RefreshSpaceSetup();
	if (CanLaunchSpaceSetup())
	{
		bPermissionGranted = UAndroidPermissionFunctionLibrary::CheckPermission(HUICRSpaceSetup::ScenePermission);
		if (bPermissionGranted)
		{
			return OpenCapture();
		}
		Phase = EPhase::WaitingForPermission;
		OnSpaceSetupChanged.Broadcast();
		UAndroidPermissionFunctionLibrary::AcquirePermissions({ HUICRSpaceSetup::ScenePermission });
		return true;
	}
#endif
	return false;
}

bool UHUICRSyncSpaceSetupSubsystem::OpenCapture()
{
#if PLATFORM_ANDROID
	UMRUKSubsystem* MRUK = GetGameInstance()->GetSubsystem<UMRUKSubsystem>();
	Phase = EPhase::Capturing;
	bCheckInitialLoad = false;
	if (MRUK && MRUK->LaunchSceneCapture())
	{
		UE_LOG(LogHUICRSpaceSetup, Log, TEXT("Launched Quest Space Setup."));
		OnSpaceSetupChanged.Broadcast();
		return true;
	}
	Phase = EPhase::LaunchFailed;
	UE_LOG(LogHUICRSpaceSetup, Warning, TEXT("Quest could not launch Space Setup."));
	OnSpaceSetupChanged.Broadcast();
#endif
	return false;
}

void UHUICRSyncSpaceSetupSubsystem::HandlePermissions(const TArray<FString>& Permissions, const TArray<bool>& Results)
{
#if PLATFORM_ANDROID
	const int32 Index = Permissions.IndexOfByKey(HUICRSpaceSetup::ScenePermission);
	if (!Results.IsValidIndex(Index) || !SetupWorld.IsValid() || SetupWorld.Get() != GetWorld())
	{
		return;
	}
	bPermissionGranted = Results[Index];
	if (!bPermissionGranted)
	{
		Phase = EPhase::Denied;
	}
	else if (Phase == EPhase::WaitingForPermission)
	{
		OpenCapture();
		return;
	}
	else if (Phase != EPhase::Capturing && Phase != EPhase::Reloading)
	{
		Phase = EPhase::Idle;
		bCheckInitialLoad = true;
	}
	OnSpaceSetupChanged.Broadcast();
#endif
}

void UHUICRSyncSpaceSetupSubsystem::OnForeground()
{
#if PLATFORM_ANDROID
	const bool bGranted = UAndroidPermissionFunctionLibrary::CheckPermission(HUICRSpaceSetup::ScenePermission);
	if (Phase != EPhase::WaitingForPermission && bGranted != bPermissionGranted)
	{
		HandlePermissions({ HUICRSpaceSetup::ScenePermission }, { bGranted });
	}
#endif
}

void UHUICRSyncSpaceSetupSubsystem::CaptureFinished(bool bSuccess)
{
#if PLATFORM_ANDROID
	if (Phase != EPhase::Capturing || !SetupWorld.IsValid() || SetupWorld.Get() != GetWorld())
	{
		return;
	}
	UMRUKSubsystem* MRUK = GetGameInstance()->GetSubsystem<UMRUKSubsystem>();
	if (bSuccess && MRUK)
	{
		UE_LOG(LogHUICRSpaceSetup, Log, TEXT("Quest Space Setup finished. Reloading the room from the headset."));
		Phase = EPhase::Reloading;
		// A discovery that began before the user edited the room cannot confirm the new layout.
		bReloadAfterCurrentLoad = MRUK->SceneLoadStatus == EMRUKInitStatus::Busy;
		if (!bReloadAfterCurrentLoad)
		{
			MRUK->LoadSceneFromDevice();
		}
	}
	else
	{
		Phase = EPhase::Cancelled;
		UE_LOG(LogHUICRSpaceSetup, Log, TEXT("Quest Space Setup was cancelled or failed. Keeping the previous room."));
	}
	OnSpaceSetupChanged.Broadcast();
#endif
}

void UHUICRSyncSpaceSetupSubsystem::SceneLoaded(bool bSuccess)
{
#if PLATFORM_ANDROID
	if (!SetupWorld.IsValid() || SetupWorld.Get() != GetWorld())
	{
		return;
	}
	if (bReloadAfterCurrentLoad)
	{
		// Let all listeners finish processing the older discovery before starting the new one.
		return;
	}
	const bool bSetupReload = Phase == EPhase::Reloading;
	if (bSetupReload || (bSuccess && Phase == EPhase::LoadFailed))
	{
		Phase = bSuccess ? EPhase::Idle : EPhase::LoadFailed;
	}
	UpdateScreens(bSuccess);
	UE_LOG(LogHUICRSpaceSetup, Log, TEXT("Quest room %s %s. Found %d physical calibration screen(s) in the current room."),
		bSetupReload ? TEXT("reload") : TEXT("discovery"), bSuccess ? TEXT("succeeded") : TEXT("failed"), PhysicalScreens.Num());
	OnSpaceSetupChanged.Broadcast();
#endif
}

void UHUICRSyncSpaceSetupSubsystem::CheckCurrentRoom()
{
#if PLATFORM_ANDROID
	if (!SetupWorld.IsValid() || SetupWorld.Get() != GetWorld())
	{
		return;
	}
	UMRUKSubsystem* MRUK = GetGameInstance()->GetSubsystem<UMRUKSubsystem>();
	if (MRUK && Phase != EPhase::Capturing && MRUK->SceneLoadStatus != EMRUKInitStatus::Busy)
	{
		// Give the map's existing BeginPlay permission/load flow first chance to discover the room.
		const bool bReadInitial = bCheckInitialLoad && Phase == EPhase::Idle &&
			bPermissionGranted && MRUK->SceneLoadStatus != EMRUKInitStatus::Complete;
		bCheckInitialLoad = false;
		if (bReloadAfterCurrentLoad || bReadInitial)
		{
			bReloadAfterCurrentLoad = false;
			MRUK->LoadSceneFromDevice();
		}
	}
	UpdateScreens(false);
#endif
}

void UHUICRSyncSpaceSetupSubsystem::UpdateScreens(bool bRepairSpawner)
{
#if PLATFORM_ANDROID
	UMRUKSubsystem* MRUK = GetGameInstance()->GetSubsystem<UMRUKSubsystem>();
	const bool bWasLoading = bLoading;
	bLoading = MRUK && MRUK->SceneLoadStatus == EMRUKInitStatus::Busy;
	if (bLoading)
	{
		if (!bWasLoading)
		{
			OnSpaceSetupChanged.Broadcast();
		}
		return;
	}

	TArray<TWeakObjectPtr<AActor>> Found;
	AMRUKRoom* Room = MRUK && MRUK->SceneLoadStatus == EMRUKInitStatus::Complete ? MRUK->GetCurrentRoom() : nullptr;
	const bool bRoomChanged = LastObservedRoom.Get() != Room;
	if (IsValid(Room) && !bRepairSpawner && !bRoomChanged && !bWasLoading && !PhysicalScreens.IsEmpty() &&
		!PhysicalScreens.ContainsByPredicate([](const auto& Screen) { return !Screen.IsValid(); }))
	{
		return;
	}
	LastObservedRoom = Room;
	if (IsValid(Room) && Room->GetWorld() == SetupWorld.Get())
	{
		for (const auto& WeakSpawner : ScreenSpawners)
		{
			auto* Spawner = Cast<AMRUKAnchorActorSpawner>(WeakSpawner.Get());
			if (!Spawner)
			{
				continue;
			}
			TArray<AActor*> Actors;
			Spawner->GetSpawnedActorsByRoom(Room, Actors);
			bool bHasStaleScreen = false;
			auto CollectScreens = [&](TSet<AMRUKAnchor*>& Represented)
			{
				for (AActor* Actor : Actors)
				{
					if (IsValid(Actor) && Actor->IsA<AHUICRSyncScreen>())
					{
						auto* Anchor = Cast<AMRUKAnchor>(Actor->GetAttachParentActor());
						if (IsValid(Anchor) && Anchor->HasLabel(FMRUKLabels::Screen) && Room->AllAnchors.Contains(Anchor))
						{
							Represented.Add(Anchor);
							Found.AddUnique(Actor);
						}
						else if (PhysicalScreens.Contains(Actor))
						{
							bHasStaleScreen = true;
						}
					}
				}
			};
			TSet<AMRUKAnchor*> Represented;
			CollectScreens(Represented);
			// CurrentRoomOnly spawners can miss a new room when they started with a room already loaded.
			if ((bRepairSpawner || bRoomChanged) && Spawner->HasActorBegunPlay() && Spawner->SpawnMode != EMRUKSpawnMode::None &&
				(bHasStaleScreen || Room->AllAnchors.ContainsByPredicate([&](const TObjectPtr<AMRUKAnchor>& Anchor)
				{
					return IsValid(Anchor) && Anchor->HasLabel(FMRUKLabels::Screen) && !Represented.Contains(Anchor.Get());
				})))
			{
				Spawner->SpawnActors(Room);
				Found.RemoveAll([](const auto& Screen) { return !Screen.IsValid(); });
				Actors.Reset();
				Spawner->GetSpawnedActorsByRoom(Room, Actors);
				CollectScreens(Represented);
			}
		}
	}
	const bool bChanged = PhysicalScreens != Found;
	if (bChanged)
	{
		PhysicalScreens = MoveTemp(Found);
		UE_LOG(LogHUICRSpaceSetup, Log, TEXT("Physical calibration screens in the current room changed. Count = %d."), PhysicalScreens.Num());
		for (TActorIterator<AHUICRSyncHMDManagerBase> It(GetWorld()); It; ++It)
		{
			It->RegisterCalibrationActorsInWorld();
		}
	}
	if (bChanged || bWasLoading != bLoading)
	{
		OnSpaceSetupChanged.Broadcast();
	}
#endif
}

FText UHUICRSyncSpaceSetupSubsystem::GetSpaceSetupHint() const
{
	if (!bRequiresScreen)
	{
		return FText::GetEmpty();
	}
	switch (Phase)
	{
	case EPhase::WaitingForPermission:
		return LOCTEXT("Permission", "Allow spatial data access on your headset.");
	case EPhase::Capturing:
		return LOCTEXT("Capturing", "Finish Space Setup on your headset.");
	case EPhase::Reloading:
		return LOCTEXT("Reloading", "Checking the updated room for screens...");
	case EPhase::Denied:
		return LOCTEXT("Denied", "Allow spatial data in app permissions, then choose Set Up Space.");
	case EPhase::LaunchFailed:
		return LOCTEXT("LaunchFailed", "Space Setup could not open. Choose Set Up Space to retry.");
	case EPhase::LoadFailed:
		return LOCTEXT("LoadFailed", "Could not read the updated room. Choose Set Up Space to retry.");
	case EPhase::Cancelled:
		if (!IsCalibrationScreenReady())
		{
			return LOCTEXT("Cancelled", "Setup closed. Choose Set Up Space to try again.\nOutline your main screen and label it 'Screen'.");
		}
		break;
	default:
		break;
	}
	if (bLoading)
	{
		return LOCTEXT("Loading", "Checking your room for screens...");
	}
	if (IsCalibrationScreenReady())
	{
		return FText::GetEmpty();
	}
	return LOCTEXT("Missing", "In Set Up Space, outline your main screen.\nGive it the 'Screen' label.");
}

#undef LOCTEXT_NAMESPACE
