#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "HUICRSyncCalibrationSaveGame.generated.h"

UCLASS()
class HUICRSYNCRUNTIME_API UHUICRSyncCalibrationSaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	UHUICRSyncCalibrationSaveGame();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Connection")
	FString PCIpAddress;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Connection")
	int32 TargetPCSyncPort = 4001;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Calibration")
	TMap<int32, FVector> ScreenOffsetMap;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Calibration")
	TMap<int32, FRotator> ScreenRotationMap;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Calibration")
	TMap<int32, FVector> ScreenScaleMap;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Calibration")
	TMap<int32, bool> ShouldScreenControlRemoteMap;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Calibration")
	FVector HUIScale = FVector::OneVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Calibration")
	FVector RetargetPawnScale = FVector::OneVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Calibration")
	bool bMotionParallaxState = true;
};
