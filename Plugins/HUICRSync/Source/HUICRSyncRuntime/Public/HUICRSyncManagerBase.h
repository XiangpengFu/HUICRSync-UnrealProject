#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "HUICRSyncTypes.h"
#include "HUICRSyncManagerBase.generated.h"

class UHUICRSyncSubsystem;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FHUICRSyncCalibratorPayloadReceived, const FHUICRSyncCalibratorPayload&, Payload);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FHUICRSyncScreenPayloadReceived, const FHUICRSyncScreenPayload&, Payload);

UCLASS(Abstract, Blueprintable)
class HUICRSYNCRUNTIME_API AHUICRSyncManagerBase : public AActor
{
	GENERATED_BODY()

public:
	AHUICRSyncManagerBase();

	UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "HUICR Sync")
	UHUICRSyncSubsystem* SyncSubsystem = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	bool bConfigureSyncOnBeginPlay = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	EHUICRSyncRole LocalRole = EHUICRSyncRole::None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Network")
	int32 LocalSyncPort = 4001;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Network")
	int32 RemoteSyncPort = 4001;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Network")
	FString RemoteIpAddress;

	UPROPERTY(BlueprintAssignable, Category = "HUICR Sync|Events")
	FHUICRSyncCalibratorPayloadReceived OnCalibratorPayloadReceived;

	UPROPERTY(BlueprintAssignable, Category = "HUICR Sync|Events")
	FHUICRSyncScreenPayloadReceived OnScreenPayloadReceived;

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync")
	virtual bool InitializeManager();

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Payload")
	bool QueueCalibratorPayload(const FHUICRSyncCalibratorPayload& Payload);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Payload")
	bool QueueScreenPayload(const FHUICRSyncScreenPayload& Payload);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Spawn", meta = (AutoCreateRefTerm = "InitPayloadBytes"))
	bool SpawnSyncActor(UClass* SpawnClass, const FTransform& SpawnTransform, const TArray<uint8>& InitPayloadBytes);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Spawn", meta = (DisplayName = "Spawn Sync Actor"))
	bool SpawnSyncActorSimple(UClass* SpawnClass, const FTransform& SpawnTransform);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Actor")
	bool DestroySyncActor(EHUICRSyncActorType TypeCode, int32 ActorID);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Command")
	bool SendReliableCommand(const FString& Address, const TArray<FHUICRSyncCommandArg>& Args);

	UFUNCTION(BlueprintPure, Category = "HUICR Sync")
	bool HasSyncSubsystem() const;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void HandleNetDataReceived();

	UFUNCTION()
	void OnSubsystemNetDataReceived();

	bool EnsureSyncSubsystem();
	bool GetFirstPayload(EHUICRSyncActorType TypeCode, FHUICRSyncNetEntry& OutEntry) const;
	bool DecodeCalibratorEntry(const FHUICRSyncNetEntry& Entry, FHUICRSyncCalibratorPayload& OutPayload) const;
	bool DecodeScreenEntry(const FHUICRSyncNetEntry& Entry, FHUICRSyncScreenPayload& OutPayload) const;
	bool DecodeTransformEntry(const FHUICRSyncNetEntry& Entry, FTransform& OutTransform) const;
};
