#include "HUICRSyncActor.h"

#include "Engine/GameInstance.h"
#include "HUICRSyncSubsystem.h"

AHUICRSyncActor::AHUICRSyncActor()
{
	PrimaryActorTick.bCanEverTick = false;
}

void AHUICRSyncActor::InitSyncActorID()
{
	if (SyncSubsystem && bAutoAssignActorID && ActorID < 0)
	{
		ActorID = SyncSubsystem->AllocateActorID(TypeCode);
	}
}

void AHUICRSyncActor::SetSyncActorID(int32 NewActorID)
{
	ActorID = NewActorID;
}

void AHUICRSyncActor::RegisterPayload(bool bFirstFrame)
{
	if (!SyncSubsystem)
	{
		return;
	}

	if (bFirstFrame)
	{
		if (bParticipateInSyncAtStart)
		{
			QueueSpawnMirror();
		}
		return;
	}

	if (!bSendDataToPeer)
	{
		return;
	}

	TArray<uint8> PayloadBytes;
	PayloadBytes.Append(ActorSendPayloadData);

	FHUICRSyncNetEntry Entry;
	Entry.TypeCode = TypeCode;
	Entry.ActorID = ActorID;
	Entry.PayloadBytes = PayloadBytes;

	SyncSubsystem->QueueNetEntry(Entry);
	ActorSendPayloadData.Empty();
}

void AHUICRSyncActor::DeserializePayload()
{
	if (!SyncSubsystem || !bReceiveDataFromPeer)
	{
		return;
	}

	FHUICRSyncNetInnerMapEntry* InnerMapEntry = SyncSubsystem->ReceivedDataMap.Find(TypeCode);
	if (!InnerMapEntry)
	{
		return;
	}

	FHUICRSyncNetEntry* FoundPayload = InnerMapEntry->InnerMap.Find(ActorID);
	if (!FoundPayload)
	{
		return;
	}

	ActorReceivePayloadData.Empty();
	if (!FoundPayload->PayloadBytes.IsEmpty())
	{
		ActorReceivePayloadData.Append(FoundPayload->PayloadBytes);
	}
}

void AHUICRSyncActor::QueueSpawnMirror()
{
	if (!SyncSubsystem)
	{
		return;
	}

	if (SyncSubsystem->Role == EHUICRSyncRole::PC)
	{
		SyncSubsystem->QueueSpawnMirror(GetRemoteCounterpartClass(), ActorID, ConvertPCTransformToHMD(GetActorTransform()), ActorInitPayloadData);
		return;
	}

	if (SyncSubsystem->Role == EHUICRSyncRole::HMD)
	{
		SyncSubsystem->SpawnSyncActor(GetClass(), GetActorTransform(), ActorInitPayloadData);
	}
}

void AHUICRSyncActor::DestroySyncActor()
{
	if (SyncSubsystem && ActorID >= 0)
	{
		SyncSubsystem->DestroySyncActor(TypeCode, ActorID);
		return;
	}

	Destroy();
}

TSubclassOf<AHUICRSyncActor> AHUICRSyncActor::GetRemoteCounterpartClass() const
{
	if (UClass* ExplicitCounterpartClass = RemoteCounterpartClass.LoadSynchronous())
	{
		return ExplicitCounterpartClass;
	}

	return GetClass();
}


FTransform AHUICRSyncActor::ConvertHMDTransformToPC(const FTransform& InHMDTransform) const
{
	return SyncSubsystem ? SyncSubsystem->ConvertHMDTransformToPC(InHMDTransform) : InHMDTransform;
}

FTransform AHUICRSyncActor::ConvertPCTransformToHMD(const FTransform& InPCTransform) const
{
	return SyncSubsystem ? SyncSubsystem->ConvertPCTransformToHMD(InPCTransform) : InPCTransform;
}

void AHUICRSyncActor::BeginPlay()
{
	Super::BeginPlay();

	bHiddenBeforeCalibration = IsHidden();

	if (UGameInstance* GameInstance = GetGameInstance())
	{
		SyncSubsystem = GameInstance->GetSubsystem<UHUICRSyncSubsystem>();
	}

	if (SyncSubsystem)
	{
		InitSyncActorID();
		SyncSubsystem->RegisterSyncActor(this);
		SyncSubsystem->OnPrepareNetDataSend.AddDynamic(this, &AHUICRSyncActor::HandlePrepareNetDataSend);
		SyncSubsystem->OnNetDataReceived.AddDynamic(this, &AHUICRSyncActor::HandleNetDataReceived);
		SyncSubsystem->OnCalibrationStateChanged.AddDynamic(this, &AHUICRSyncActor::HandleCalibrationStateChanged);
		HandleCalibrationStateChanged(SyncSubsystem->bCalibrationState);
	}
}

void AHUICRSyncActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (SyncSubsystem)
	{
		SyncSubsystem->OnPrepareNetDataSend.RemoveDynamic(this, &AHUICRSyncActor::HandlePrepareNetDataSend);
		SyncSubsystem->OnNetDataReceived.RemoveDynamic(this, &AHUICRSyncActor::HandleNetDataReceived);
		SyncSubsystem->OnCalibrationStateChanged.RemoveDynamic(this, &AHUICRSyncActor::HandleCalibrationStateChanged);
		SyncSubsystem->UnregisterSyncActor(this);
	}

	Super::EndPlay(EndPlayReason);
}

void AHUICRSyncActor::HandleNetDataReceived()
{
	DeserializePayload();
}

void AHUICRSyncActor::HandlePrepareNetDataSend()
{
	RegisterPayload(false);
}

void AHUICRSyncActor::HandleCalibrationStateChanged(bool bNewCalibrationState)
{
	if (!bHideDuringCalibration)
	{
		return;
	}

	if (bNewCalibrationState)
	{
		bHiddenBeforeCalibration = IsHidden();
		SetActorHiddenInGame(true);
		return;
	}

	SetActorHiddenInGame(bHiddenBeforeCalibration);
}
