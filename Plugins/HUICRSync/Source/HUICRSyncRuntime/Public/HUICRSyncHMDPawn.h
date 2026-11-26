#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "HUICRSyncFields.h"
#include "HUICRSyncTypes.h"
#include "StructUtils/InstancedStruct.h"
#include "HUICRSyncHMDPawn.generated.h"

class UHUICRSyncSubsystem;
class AHUICRSyncActor;
class UScriptStruct;

UCLASS(Blueprintable)
class HUICRSYNCRUNTIME_API AHUICRSyncHMDPawn : public APawn
{
	GENERATED_BODY()

public:
	AHUICRSyncHMDPawn();

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync")
	UHUICRSyncSubsystem* SyncSubsystem = nullptr;

	/** The PC assigns this pawn its participant ID. It is unassigned before joining. */
	UPROPERTY(BlueprintReadOnly, Transient, Category = "HUICR Sync")
	int32 ActorID = INDEX_NONE;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync")
	EHUICRSyncActorType TypeCode = EHUICRSyncActorType::HMDPawn;

	/** PC-side HUI Pawn Sync Actor spawned for the participant who controls this pawn. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUICR Sync|Spawn")
	TSoftClassPtr<AHUICRSyncActor> RemoteCounterpartClass;

	/** Select the same shared structure on the locally possessed pawn and its PC counterpart. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUICR Sync|Payload")
	TObjectPtr<UScriptStruct> SharedPayloadStruct;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Payload")
	bool bSendDataToPeer = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Payload")
	bool bReceiveDataFromPeer = true;

	UPROPERTY(BlueprintReadWrite, Category = "HUICR Sync|Payload")
	TArray<uint8> ActorInitPayloadData;

	UPROPERTY(BlueprintReadWrite, Category = "HUICR Sync|Payload")
	TArray<uint8> ActorSendPayloadData;

	UPROPERTY(BlueprintReadWrite, Category = "HUICR Sync|Payload")
	TArray<uint8> ActorReceivePayloadData;

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Payload", meta = (BlueprintInternalUseOnly = "true"))
	bool SetInitialPayloadInternal(const FInstancedStruct& Payload);

	UFUNCTION(BlueprintCallable, BlueprintPure = false, Category = "HUICR Sync|Payload", meta = (BlueprintInternalUseOnly = "true"))
	bool ReadInitialPayloadInternal(FInstancedStruct& Payload) const;

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Payload", meta = (BlueprintInternalUseOnly = "true"))
	bool PrepareOutgoingPayloadInternal(const FInstancedStruct& Payload);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Payload", meta = (BlueprintInternalUseOnly = "true"))
	bool SetSyncFieldSourceInternal(FName Field, EHUICRSyncFieldSource Source, const FInstancedStruct& Payload);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Payload", meta = (BlueprintInternalUseOnly = "true"))
	bool ReadReceivedPayloadInternal(FInstancedStruct& Payload, TArray<FName>& FieldsToApply);

	/** Prepare current local values for this pawn's listed fields before the plugin samples them. */
	UFUNCTION(BlueprintNativeEvent, Category = "HUICR Sync|Payload", meta = (DisplayName = "On Prepare Sync Fields"))
	void OnPrepareSyncFields(const TArray<FName>& FieldsToPrepare);
	virtual void OnPrepareSyncFields_Implementation(const TArray<FName>& FieldsToPrepare) {}

	/** Read Received Values here to apply updates from this pawn's own PC counterpart. */
	UFUNCTION(BlueprintNativeEvent, Category = "HUICR Sync|Payload", meta = (DisplayName = "On Sync Fields Received"))
	void OnSyncFieldsReceived();
	virtual void OnSyncFieldsReceived_Implementation() {}

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync")
	void InitHUIID();

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Payload")
	bool RegisterPayload();

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Payload")
	void DeserializePayload();

	UFUNCTION(BlueprintPure, Category = "HUICR Sync|Transform")
	FTransform ConvertHMDTransformToPC(const FTransform& InHMDTransform) const;

	UFUNCTION(BlueprintPure, Category = "HUICR Sync|Transform")
	FTransform ConvertPCTransformToHMD(const FTransform& InPCTransform) const;

	AHUICRSyncActor* EnsurePawnSyncEndpoint();
	AHUICRSyncActor* GetPawnSyncEndpoint() const;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaTime) override;
	virtual void NotifyControllerChanged() override;

	UFUNCTION()
	void OnChangeCalibrationState(bool bNewCalibrationState);

	UFUNCTION()
	void HandlePrepareNetDataSend();

private:
	UPROPERTY(Transient)
	TObjectPtr<AHUICRSyncActor> PawnSyncEndpoint;

	UPROPERTY(Transient)
	FInstancedStruct PreparedBeforePairing;

	struct FDeferredFieldSource
	{
		EHUICRSyncFieldSource Source;
		TArray<uint8> Value;
	};
	TMap<FName, FDeferredFieldSource> DeferredFieldSources;
	void SubmitDeferredFieldSources();

	double NextHeadPoseSendTime = 0.0;
};
