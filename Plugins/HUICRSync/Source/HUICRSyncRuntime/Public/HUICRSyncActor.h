#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "HUICRSyncFields.h"
#include "HUICRSyncTypes.h"
#include "StructUtils/InstancedStruct.h"
#include "HUICRSyncActor.generated.h"

class UHUICRSyncSubsystem;
class UScriptStruct;

UCLASS(Blueprintable)
class HUICRSYNCRUNTIME_API AHUICRSyncActor : public AActor
{
	GENERATED_BODY()

public:
	AHUICRSyncActor();

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync")
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

	UPROPERTY(BlueprintReadWrite, Category = "HUICR Sync|Payload")
	TArray<uint8> ActorInitPayloadData;

	UPROPERTY(BlueprintReadWrite, Category = "HUICR Sync|Payload")
	TArray<uint8> ActorSendPayloadData;

	UPROPERTY(BlueprintReadWrite, Category = "HUICR Sync|Payload")
	TArray<uint8> ActorReceivePayloadData;

	/** Select the same struct on both actor classes to enable independent shared fields. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUICR Sync|Payload")
	TObjectPtr<UScriptStruct> SharedPayloadStruct;

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Payload", meta = (BlueprintInternalUseOnly = "true"))
	bool SetInitialPayloadInternal(const FInstancedStruct& Payload);

	UFUNCTION(BlueprintCallable, BlueprintPure = false, Category = "HUICR Sync|Payload", meta = (BlueprintInternalUseOnly = "true"))
	bool ReadInitialPayloadInternal(FInstancedStruct& Payload) const;

	void InitializeSpawnPayload(const TArray<uint8>& SpawnPayload);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Payload", meta = (BlueprintInternalUseOnly = "true"))
	bool PrepareOutgoingPayloadInternal(const FInstancedStruct& Payload);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Payload", meta = (BlueprintInternalUseOnly = "true"))
	bool SetSyncFieldSourceInternal(FName Field, EHUICRSyncFieldSource Source, const FInstancedStruct& Payload);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Payload", meta = (BlueprintInternalUseOnly = "true"))
	bool ReadReceivedPayloadInternal(FInstancedStruct& Payload, TArray<FName>& FieldsToApply);

	bool GetPreparedSyncField(FName Field, TArray<uint8>& OutBytes) const;
	bool GetInitialSyncField(FName Field, TArray<uint8>& OutBytes) const;
	bool ValidateSyncField(FName Field, const TArray<uint8>* OptionalBytes = nullptr) const;
	bool ApplyConfirmedSyncField(FName Field, const TArray<uint8>& Value, bool bApplyLocally);
	void ClearSyncFieldToApply(FName Field);
	void ResetConfirmedSyncFields();

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
	UPROPERTY(Transient)
	FInstancedStruct PreparedFields;

	UPROPERTY(Transient)
	FInstancedStruct ConfirmedFields;

	struct FPendingFieldSource
	{
		EHUICRSyncFieldSource Source;
		TArray<uint8> Value;
	};

	TMap<FName, FPendingFieldSource> BeginPlayFieldRequests;
	TArray<FName> UnreadFields;
	TArray<FName> LastReadFields;
	uint64 LastFieldReadFrame = MAX_uint64;
	bool bHasExplicitSpawnPayload = false;
	bool bHiddenBeforeCalibration = false;
};
