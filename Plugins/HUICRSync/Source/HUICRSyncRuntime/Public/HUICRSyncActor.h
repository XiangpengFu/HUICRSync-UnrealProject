#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "HUICRSyncTypes.h"
#include "HUICRSyncActor.generated.h"

class UHUICRSyncSubsystem;
class UScriptStruct;
struct FInstancedStruct;

UCLASS(Blueprintable)
class HUICRSYNCRUNTIME_API AHUICRSyncActor : public AActor
{
	GENERATED_BODY()

public:
	AHUICRSyncActor();

	UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "HUICR Sync")
	UHUICRSyncSubsystem* SyncSubsystem = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	int32 ActorID = -1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	EHUICRSyncActorType TypeCode = EHUICRSyncActorType::HUIActor;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Spawn")
	EHUICRSyncActorClassRole SyncActorClassRole = EHUICRSyncActorClassRole::Shared;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Spawn")
	TSoftClassPtr<AHUICRSyncActor> RemoteCounterpartClass;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	bool bAutoAssignActorID = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Spawn", meta = (DisplayName = "Participate in Sync at Start"))
	bool bParticipateInSyncAtStart = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Payload")
	bool bSendDataToPeer = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Payload")
	bool bReceiveDataFromPeer = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Visibility")
	bool bHideDuringCalibration = true;

	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Payload")
	TArray<uint8> ActorInitPayloadData;

	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Payload")
	TArray<uint8> ActorSendPayloadData;

	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Payload")
	TArray<uint8> ActorReceivePayloadData;

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync")
	void InitSyncActorID();

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync")
	void SetSyncActorID(int32 NewActorID);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync")
	void RegisterPayload(bool bFirstFrame);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync")
	void DeserializePayload();

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync")
	void QueueSpawnMirror();

	UFUNCTION(BlueprintPure, Category = "HUICR Sync|Spawn")
	TSubclassOf<AHUICRSyncActor> GetRemoteCounterpartClass() const;

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync")
	void DestroySyncActor();

	UFUNCTION(BlueprintPure, Category = "HUICR Sync|Transform")
	FTransform ConvertHMDTransformToPC(const FTransform& InHMDTransform) const;

	UFUNCTION(BlueprintPure, Category = "HUICR Sync|Transform")
	FTransform ConvertPCTransformToHMD(const FTransform& InPCTransform) const;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UFUNCTION()
	void HandleNetDataReceived();

	UFUNCTION()
	void HandlePrepareNetDataSend();

	UFUNCTION()
	void HandleCalibrationStateChanged(bool bNewCalibrationState);

private:
	bool bHiddenBeforeCalibration = false;
};
