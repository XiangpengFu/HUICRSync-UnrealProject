#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "HUICRSyncCalibrator_PC.generated.h"

class UHUICRSyncSubsystem;

UCLASS(Blueprintable)
class HUICRSYNCRUNTIME_API AHUICRSyncCalibrator_PC : public AActor
{
	GENERATED_BODY()

public:
	AHUICRSyncCalibrator_PC();

	UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "HUICR Sync")
	UHUICRSyncSubsystem* SyncSubsystem = nullptr;

	UPROPERTY(BlueprintReadWrite, VisibleAnywhere, Category = "HUICR Sync|Components")
	USceneComponent* DefaultSceneRoot = nullptr;

	UPROPERTY(BlueprintReadWrite, VisibleAnywhere, Category = "HUICR Sync|Components")
	UStaticMeshComponent* CalibratorMesh = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Calibrator")
	int32 CalibratorID = 0;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Calibrator")
	USceneComponent* BoundScreenComponent = nullptr;

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Calibrator")
	void BindScreenComponent(int32 ScreenID, USceneComponent* ScreenComponent);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Calibrator")
	void UpdateTransformFromBoundScreen();

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Calibrator")
	void SetCalibratorScale(const FVector& NewScale);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Calibrator")
	void HandleCalibrationStateChanged(bool bNewCalibrationState);

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaTime) override;
};
