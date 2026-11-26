#pragma once

#include "CoreMinimal.h"
#include "Engine/TimerHandle.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "HUICRSyncSpaceSetupSubsystem.generated.h"

/** Tracks the local Quest's physical calibration screens independently of the multiplayer session. */
UCLASS()
class HUICRSYNCRUNTIME_API UHUICRSyncSpaceSetupSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	void RefreshSpaceSetup();
	bool IsSpaceSetupRequired() const { return bRequiresScreen; }
	bool IsCalibrationScreenReady() const;
	bool IsScreenFromOtherRoom(const AActor* Screen) const;
	bool CanLaunchSpaceSetup() const;
	bool LaunchSpaceSetup();
	FText GetSpaceSetupHint() const;
	FSimpleMulticastDelegate OnSpaceSetupChanged;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

private:
	enum class EPhase : uint8 { Idle, WaitingForPermission, Capturing, Reloading, Denied, LaunchFailed, Cancelled, LoadFailed };
	EPhase Phase = EPhase::Idle;
	bool bRequiresScreen = false;
	bool bLoading = false;
	bool bPermissionGranted = false;
	bool bReloadAfterCurrentLoad = false;
	bool bCheckInitialLoad = false;
	TWeakObjectPtr<UWorld> SetupWorld;
	TWeakObjectPtr<AActor> LastObservedRoom;
	TArray<TWeakObjectPtr<AActor>> ScreenSpawners;
	TArray<TWeakObjectPtr<AActor>> PhysicalScreens;
	FTimerHandle RoomTimer;

	void ResetWorld();
	void CheckCurrentRoom();
	void UpdateScreens(bool bRepairSpawner);
	bool OpenCapture();
	void HandlePermissions(const TArray<FString>& Permissions, const TArray<bool>& Results);
	void OnForeground();

	UFUNCTION()
	void CaptureFinished(bool bSuccess);
	UFUNCTION()
	void SceneLoaded(bool bSuccess);
};
