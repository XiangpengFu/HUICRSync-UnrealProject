#include "HUICRSyncHMDPawn.h"

#include "Engine/GameInstance.h"
#include "HUICRSyncSubsystem.h"

AHUICRSyncHMDPawn::AHUICRSyncHMDPawn()
{
	PrimaryActorTick.bCanEverTick = true;
	ActorID = 0;
	TypeCode = EHUICRSyncActorType::HMDPawn;
}

void AHUICRSyncHMDPawn::InitHUIID()
{
	// Matches the original HUIPawn flow: HMDPawn keeps ActorID 0 unless Blueprint changes it.
}

bool AHUICRSyncHMDPawn::RegisterPayload()
{
	if (!bSendDataToPeer || !SyncSubsystem)
	{
		return false;
	}

	TArray<uint8> PayloadBytes;
	PayloadBytes.Append(ActorSendPayloadData);

	FHUICRSyncNetEntry Entry;
	Entry.TypeCode = TypeCode;
	Entry.ActorID = ActorID;
	Entry.PayloadBytes = PayloadBytes;
	SyncSubsystem->QueueNetEntry(Entry);
	ActorSendPayloadData.Empty();

	return true;
}

void AHUICRSyncHMDPawn::DeserializePayload()
{
	if (!bReceiveDataFromPeer || !SyncSubsystem)
	{
		return;
	}

	const FHUICRSyncNetInnerMapEntry* InnerMapEntry = SyncSubsystem->ReceivedDataMap.Find(TypeCode);
	if (!InnerMapEntry)
	{
		return;
	}

	const FHUICRSyncNetEntry* FoundPayload = InnerMapEntry->InnerMap.Find(ActorID);
	if (!FoundPayload)
	{
		return;
	}

	ActorReceivePayloadData = FoundPayload->PayloadBytes;
}


void AHUICRSyncHMDPawn::BeginPlay()
{
	Super::BeginPlay();

	if (UGameInstance* GameInstance = GetGameInstance())
	{
		SyncSubsystem = GameInstance->GetSubsystem<UHUICRSyncSubsystem>();
	}

	if (SyncSubsystem)
	{
		InitHUIID();
		SyncSubsystem->OnPrepareNetDataSend.AddDynamic(this, &AHUICRSyncHMDPawn::HandlePrepareNetDataSend);
		SyncSubsystem->OnNetDataReceived.AddDynamic(this, &AHUICRSyncHMDPawn::DeserializePayload);
		SyncSubsystem->OnCalibrationStateChanged.AddDynamic(this, &AHUICRSyncHMDPawn::OnChangeCalibrationState);
	}
}

void AHUICRSyncHMDPawn::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (SyncSubsystem)
	{
		SyncSubsystem->OnPrepareNetDataSend.RemoveDynamic(this, &AHUICRSyncHMDPawn::HandlePrepareNetDataSend);
		SyncSubsystem->OnNetDataReceived.RemoveDynamic(this, &AHUICRSyncHMDPawn::DeserializePayload);
		SyncSubsystem->OnCalibrationStateChanged.RemoveDynamic(this, &AHUICRSyncHMDPawn::OnChangeCalibrationState);
	}

	Super::EndPlay(EndPlayReason);
}

void AHUICRSyncHMDPawn::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
}

void AHUICRSyncHMDPawn::OnChangeCalibrationState(bool bNewCalibrationState)
{
	// Kept intentionally empty to match the original HUIPawn behavior.
}

void AHUICRSyncHMDPawn::HandlePrepareNetDataSend()
{
	RegisterPayload();
}
