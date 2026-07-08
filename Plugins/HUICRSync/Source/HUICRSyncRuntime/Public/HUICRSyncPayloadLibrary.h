#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "StructUtils/InstancedStruct.h"
#include "HUICRSyncPayloadLibrary.generated.h"

UCLASS()
class HUICRSYNCRUNTIME_API UHUICRSyncPayloadLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Payload")
	static bool WriteInstancedStruct(TArray<uint8>& OutBytes, UScriptStruct* StructType, const FInstancedStruct& InStruct);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Payload")
	static bool ReadInstancedStruct(const TArray<uint8>& InBytes, UScriptStruct* StructType, FInstancedStruct& OutStruct);

	static void WriteSpawnRequestPayload(TArray<uint8>& OutBytes, UClass* SpawnClass, const FTransform& SpawnTransform, bool bSpawnBackOnSender, const TArray<uint8>& InitPayloadBytes = TArray<uint8>());
	static bool ReadSpawnRequestPayload(const TArray<uint8>& InBytes, FString& OutClassPath, FTransform& OutTransform, bool& bOutSpawnBackOnSender, TArray<uint8>& OutInitPayloadBytes);

	static void WriteSpawnMirrorPayload(TArray<uint8>& OutBytes, UClass* SpawnClass, int32 ActorID, const FTransform& SpawnTransform, const TArray<uint8>& InitPayloadBytes);
	static bool ReadSpawnMirrorPayload(const TArray<uint8>& InBytes, FString& OutClassPath, int32& OutActorID, FTransform& OutTransform, TArray<uint8>& OutInitPayloadBytes);
};
