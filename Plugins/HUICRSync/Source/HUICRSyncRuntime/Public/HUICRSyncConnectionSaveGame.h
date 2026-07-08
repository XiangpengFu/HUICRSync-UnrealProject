#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "HUICRSyncConnectionSaveGame.generated.h"

UCLASS()
class HUICRSYNCRUNTIME_API UHUICRSyncConnectionSaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	FString PCIpAddress;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	int32 TargetPCSyncPort = 4001;
};
