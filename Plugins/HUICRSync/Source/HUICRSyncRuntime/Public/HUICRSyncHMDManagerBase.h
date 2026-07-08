#pragma once

#include "CoreMinimal.h"
#include "HUICRSyncManagerBase.h"
#include "HUICRSyncHMDManagerBase.generated.h"

class UHUICRSyncCalibrationSaveGame;
class AHUICRSyncScreen;
class AHUICRSyncCalibrator_HMD;

UCLASS(Blueprintable)
class HUICRSYNCRUNTIME_API AHUICRSyncHMDManagerBase : public AHUICRSyncManagerBase
{
	GENERATED_BODY()

public:
	AHUICRSyncHMDManagerBase();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Connection")
	FString ConnectionSaveSlotName = TEXT("HUICRSync_PCConnection");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Connection")
	int32 ConnectionSaveUserIndex = 0;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Connection")
	FString SavedPCIpAddress;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Connection")
	int32 SavedTargetPCSyncPort = 4001;


	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Calibration")
	FString CalibrationSaveSlotName = TEXT("HUICRSync_Calibration");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Calibration")
	int32 CalibrationSaveUserIndex = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Calibration")
	FVector RetargetPawnScale = FVector::OneVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Motion Parallax")
	bool bMotionParallaxState = true;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Calibration")
	UHUICRSyncCalibrationSaveGame* CalibrationSaveGame = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Calibration")
	TArray<AHUICRSyncScreen*> RegisteredScreens;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Calibration")
	TArray<AHUICRSyncCalibrator_HMD*> RegisteredCalibrators;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Calibration|Spawn")
	TSubclassOf<AHUICRSyncCalibrator_HMD> CalibratorClass;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Calibration|Spawn")
	bool bAutoSpawnCalibratorsForRegisteredScreens = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Manipulation")
	AHUICRSyncScreen* ManipulatedTargetScreen = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Manipulation")
	AHUICRSyncCalibrator_HMD* ManipulatedTargetCalibrator = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Manipulation")
	double ManipulateAmplitude = 0.5;

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Connection")
	bool LoadSavedPCConnection();

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Connection")
	bool SavePCConnection(const FString& PCIpAddress, int32 TargetPCSyncPort);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Connection")
	bool ConnectToPC(const FString& PCIpAddress, int32 TargetPCSyncPort, bool bSaveConnection);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Connection")
	bool ConnectToSavedPC();

	UFUNCTION(BlueprintPure, Category = "HUICR Sync|Connection")
	FString GetLocalIpAddress() const;

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Calibration")
	bool LoadCalibrationData();

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Calibration")
	bool SaveCalibrationData();

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Calibration")
	bool SaveScreenData();

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Calibration")
	void RegisterScreen(AHUICRSyncScreen* Screen);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Calibration")
	void RegisterCalibrator(AHUICRSyncCalibrator_HMD* Calibrator);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Calibration")
	void RegisterCalibrationActorsInWorld();

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Calibration|Spawn")
	int32 SpawnCalibrators();

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Calibration")
	bool SendCalibrationAndScreens();

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Calibration")
	int32 SpawnDissolveBoxesForRegisteredScreens();

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Calibration")
	void ChangeCalibrationState();

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Calibration")
	void ChangeMotionParallaxState(bool bNewState);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Calibration")
	bool SendSelectedScreenID(int32 ScreenID);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Manipulation")
	void SelectScreen(AHUICRSyncScreen* SelectedScreen);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Manipulation")
	bool SelectScreenByID(int32 ScreenID);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Manipulation")
	void SelectCalibrator();

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Manipulation")
	bool SelectCalibratorByID(int32 CalibratorID);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Manipulation")
	void MoveScreen(FVector DeltaVector);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Manipulation")
	void RotateScreen(FRotator DeltaRotator);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Manipulation")
	void ScaleScreen(FVector DeltaVector);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Manipulation")
	void ScaleCalibrator(FVector DeltaVector);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Manipulation")
	void ResetScreenPosition();

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Manipulation")
	void ResetScreenRotation();

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Manipulation")
	void ResetCalibratorScale();

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Manipulation")
	void SetScreenHidden();

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Manipulation")
	void SetCalibratorHidden();

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Motion Parallax")
	void SetMotionParallaxStatePreference(bool bNewState);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Calibration")
	void UpdateRetargetPawnScale(const FVector& ActorScale);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Payload")
	bool SendCalibratorPayload(int32 ActorID, const FTransform& CalibratorTransform);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Payload")
	bool SendScreenPayload(int32 ScreenID, const FTransform& ScreenTransform, bool bHmdControlsPCScreen);

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void HandleNetDataReceived() override;

	bool ApplyMotionParallaxState(bool bNewState, bool bUpdatePreference, bool bSavePreference);

	UFUNCTION()
	void HandleSyncSceneReady();
};
