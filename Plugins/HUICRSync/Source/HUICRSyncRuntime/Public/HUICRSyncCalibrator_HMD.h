#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "HUICRSyncTypes.h"
#include "HUICRSyncCalibrator_HMD.generated.h"

class AHUICRSyncScreen;
class UHUICRSyncCalibrationSaveGame;
class UHUICRSyncSubsystem;

UCLASS(Blueprintable)
class HUICRSYNCRUNTIME_API AHUICRSyncCalibrator_HMD : public AActor
{
	GENERATED_BODY()

public:
	AHUICRSyncCalibrator_HMD();

	UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "HUICR Sync")
	UHUICRSyncSubsystem* SyncSubsystem = nullptr;

	UPROPERTY(BlueprintReadWrite, VisibleAnywhere, Category = "HUICR Sync|Components")
	USceneComponent* DefaultSceneRoot = nullptr;

	UPROPERTY(BlueprintReadWrite, VisibleAnywhere, Category = "HUICR Sync|Components")
	UStaticMeshComponent* CalibratorMesh = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Calibrator")
	int32 CalibratorID = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Calibrator")
	AHUICRSyncScreen* BoundScreen = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Calibrator")
	bool bFollowBoundScreenEveryTick = true;

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Calibrator")
	void InitCalibrator(UHUICRSyncCalibrationSaveGame* SaveGame);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Calibrator")
	void SetupTransformWithScreen();

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Calibrator")
	FHUICRSyncCalibratorPayload BuildCalibratorPayload() const;

	UFUNCTION(BlueprintPure, Category = "HUICR Sync|Calibrator")
	int32 GetEffectiveCalibratorID() const;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaTime) override;

	UFUNCTION()
	void HandleCalibrationStateChanged(bool bNewCalibrationState);
};
