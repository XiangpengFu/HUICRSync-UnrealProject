#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "HUICRSyncTypes.h"
#include "HUICRSyncHMDPawn.generated.h"

class UHUICRSyncSubsystem;
class UScriptStruct;
struct FInstancedStruct;

UCLASS(Blueprintable)
class HUICRSYNCRUNTIME_API AHUICRSyncHMDPawn : public APawn
{
	GENERATED_BODY()

public:
	AHUICRSyncHMDPawn();

	UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "HUICR Sync")
	UHUICRSyncSubsystem* SyncSubsystem = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	int32 ActorID = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	EHUICRSyncActorType TypeCode = EHUICRSyncActorType::HMDPawn;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Payload")
	bool bSendDataToPeer = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Payload")
	bool bReceiveDataFromPeer = true;

	UPROPERTY(VisibleDefaultsOnly, BlueprintReadWrite, Category = "HUICR Sync|Payload")
	TArray<uint8> ActorInitPayloadData;

	UPROPERTY(VisibleDefaultsOnly, BlueprintReadWrite, Category = "HUICR Sync|Payload")
	TArray<uint8> ActorSendPayloadData;

	UPROPERTY(VisibleDefaultsOnly, BlueprintReadWrite, Category = "HUICR Sync|Payload")
	TArray<uint8> ActorReceivePayloadData;

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync")
	void InitHUIID();

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Payload")
	bool RegisterPayload();

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Payload")
	void DeserializePayload();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaTime) override;

	UFUNCTION()
	void OnChangeCalibrationState(bool bNewCalibrationState);

	UFUNCTION()
	void HandlePrepareNetDataSend();
};
