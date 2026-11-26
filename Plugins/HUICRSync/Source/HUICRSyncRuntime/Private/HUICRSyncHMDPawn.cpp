#include "HUICRSyncHMDPawn.h"

#include "Camera/CameraComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "HUICRSyncPawnEndpoint.h"
#include "HUICRSyncPayloadLibrary.h"
#include "HUICRSyncSubsystem.h"
#include "UObject/UnrealType.h"

namespace
{
	constexpr double HeadPoseSendInterval = 1.0 / 15.0;

	bool HasLocalPlayerController(const APawn& Pawn)
	{
		const AController* Controller = Pawn.GetController();
		return Controller && Controller->IsLocalPlayerController();
	}
}

AHUICRSyncHMDPawn::AHUICRSyncHMDPawn()
{
	PrimaryActorTick.bCanEverTick = true;
}

void AHUICRSyncHMDPawn::InitHUIID()
{
	if (SyncSubsystem && HasActorBegunPlay() && HasLocalPlayerController(*this))
	{
		SyncSubsystem->RegisterLocalHMDPawn(this);
	}
}

AHUICRSyncPawnEndpoint::AHUICRSyncPawnEndpoint()
{
	TypeCode = EHUICRSyncActorType::HMDPawn;
	bAutoAssignActorID = false;
	bParticipateInSyncAtStart = false;
	bHideDuringCalibration = false;
}

void AHUICRSyncPawnEndpoint::OnPrepareSyncFields_Implementation(const TArray<FName>& FieldsToPrepare)
{
	if (AHUICRSyncHMDPawn* Pawn = Cast<AHUICRSyncHMDPawn>(GetOwner()); Pawn && Pawn->bSendDataToPeer)
	{
		Pawn->OnPrepareSyncFields(FieldsToPrepare);
	}
}

void AHUICRSyncPawnEndpoint::OnSyncFieldsReceived_Implementation()
{
	if (AHUICRSyncHMDPawn* Pawn = Cast<AHUICRSyncHMDPawn>(GetOwner()); Pawn && Pawn->bReceiveDataFromPeer)
	{
		Pawn->ActorReceivePayloadData = ActorReceivePayloadData;
		Pawn->OnSyncFieldsReceived();
	}
}

AHUICRSyncActor* AHUICRSyncHMDPawn::GetPawnSyncEndpoint() const
{
	return PawnSyncEndpoint;
}

AHUICRSyncActor* AHUICRSyncHMDPawn::EnsurePawnSyncEndpoint()
{
	if (IsValid(PawnSyncEndpoint) || !SharedPayloadStruct || !GetWorld())
	{
		return PawnSyncEndpoint;
	}
	FActorSpawnParameters Params;
	Params.Owner = this;
	Params.OverrideLevel = GetLevel();
	Params.ObjectFlags = RF_Transient;
	Params.bDeferConstruction = true;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	auto* Endpoint = GetWorld()->SpawnActor<AHUICRSyncPawnEndpoint>(AHUICRSyncPawnEndpoint::StaticClass(), FTransform::Identity, Params);
	if (Endpoint)
	{
		PawnSyncEndpoint = Endpoint;
		Endpoint->SyncSubsystem = SyncSubsystem;
		Endpoint->SharedPayloadStruct = SharedPayloadStruct;
		Endpoint->bSendDataToPeer = bSendDataToPeer;
		Endpoint->bReceiveDataFromPeer = bReceiveDataFromPeer;
		Endpoint->InitializeSpawnPayload(ActorInitPayloadData);
		Endpoint->FinishSpawning(FTransform::Identity);
		if (PreparedBeforePairing.IsValid())
		{
			Endpoint->PrepareOutgoingPayloadInternal(PreparedBeforePairing);
			PreparedBeforePairing.Reset();
		}
	}
	return PawnSyncEndpoint;
}

bool AHUICRSyncHMDPawn::SetInitialPayloadInternal(const FInstancedStruct& Payload)
{
	if (!UHUICRSyncPayloadLibrary::WriteInitialValues(ActorInitPayloadData, SharedPayloadStruct, Payload))
	{
		return false;
	}
	if (PawnSyncEndpoint)
	{
		PawnSyncEndpoint->ActorInitPayloadData = ActorInitPayloadData;
	}
	return true;
}

bool AHUICRSyncHMDPawn::ReadInitialPayloadInternal(FInstancedStruct& Payload) const
{
	return UHUICRSyncPayloadLibrary::ReadInitialValues(ActorInitPayloadData, SharedPayloadStruct, Payload);
}

bool AHUICRSyncHMDPawn::PrepareOutgoingPayloadInternal(const FInstancedStruct& Payload)
{
	if (!bSendDataToPeer || !SharedPayloadStruct || Payload.GetScriptStruct() != SharedPayloadStruct)
	{
		return false;
	}
	if (PawnSyncEndpoint)
	{
		return PawnSyncEndpoint->PrepareOutgoingPayloadInternal(Payload);
	}
	PreparedBeforePairing = Payload;
	return true;
}

bool AHUICRSyncHMDPawn::SetSyncFieldSourceInternal(FName Field, EHUICRSyncFieldSource Source, const FInstancedStruct& Payload)
{
	const FProperty* Property = SharedPayloadStruct ? SharedPayloadStruct->FindPropertyByName(Field) : nullptr;
	if (!bSendDataToPeer || !Property || !HUICRSyncFields::IsSupportedField(*Property) ||
		(Source != EHUICRSyncFieldSource::FollowMyPayload && Source != EHUICRSyncFieldSource::UseSpecifiedValue))
	{
		return false;
	}
	TArray<uint8> Value;
	if (Source == EHUICRSyncFieldSource::UseSpecifiedValue && (Payload.GetScriptStruct() != SharedPayloadStruct ||
		!HUICRSyncFields::Encode(*Property, Payload, Value)))
	{
		return false;
	}
	const int32 PacketLimit = SyncSubsystem ? SyncSubsystem->MaxPacketBytes : GetDefault<UHUICRSyncSubsystem>()->MaxPacketBytes;
	if (!HUICRSyncFields::FitsDatagram(Field, Value.Num(), PacketLimit))
	{
		return false;
	}
	if (PawnSyncEndpoint && SyncSubsystem && ActorID > 0 &&
		SyncSubsystem->RequestSyncFieldSource(PawnSyncEndpoint, Field, Source, Value))
	{
		DeferredFieldSources.Remove(Field);
		return true;
	}
	// BeginPlay can supply sources before the PC has assigned a participant ID.
	DeferredFieldSources.Add(Field, {Source, MoveTemp(Value)});
	return true;
}

void AHUICRSyncHMDPawn::SubmitDeferredFieldSources()
{
	if (!PawnSyncEndpoint || !SyncSubsystem || !bSendDataToPeer || ActorID <= 0)
	{
		return;
	}
	for (auto It = DeferredFieldSources.CreateIterator(); It; ++It)
	{
		if (SyncSubsystem->RequestSyncFieldSource(PawnSyncEndpoint, It.Key(), It.Value().Source, It.Value().Value))
		{
			It.RemoveCurrent();
		}
	}
}

bool AHUICRSyncHMDPawn::ReadReceivedPayloadInternal(FInstancedStruct& Payload, TArray<FName>& FieldsToApply)
{
	TArray<FName> FieldKeys;
	return ReadReceivedPayloadWithKeys(Payload, FieldsToApply, FieldKeys);
}

bool AHUICRSyncHMDPawn::ReadReceivedPayloadWithKeys(FInstancedStruct& Payload, TArray<FName>& FieldsToApply, TArray<FName>& FieldKeys)
{
	if (bReceiveDataFromPeer && PawnSyncEndpoint)
	{
		return PawnSyncEndpoint->ReadReceivedPayloadWithKeys(Payload, FieldsToApply, FieldKeys);
	}
	Payload.Reset();
	FieldsToApply.Reset();
	FieldKeys.Reset();
	return false;
}

bool AHUICRSyncHMDPawn::RegisterPayload()
{
	if (!bSendDataToPeer || !SyncSubsystem || ActorID <= 0 || SyncSubsystem->Role != EHUICRSyncRole::HMD)
	{
		return false;
	}

	if (SharedPayloadStruct)
	{
		FInstancedStruct Legacy;
		const bool bRead = !ActorSendPayloadData.IsEmpty() && HUICRSyncFields::CanUseLegacyBytes(SharedPayloadStruct) &&
			UHUICRSyncPayloadLibrary::ReadInstancedStruct(ActorSendPayloadData, SharedPayloadStruct, Legacy);
		ActorSendPayloadData.Reset();
		return bRead && PrepareOutgoingPayloadInternal(Legacy);
	}

	FHUICRSyncNetEntry Entry;
	Entry.TypeCode = EHUICRSyncActorType::HMDPawn;
	Entry.ActorID = ActorID;
	Entry.PayloadBytes = MoveTemp(ActorSendPayloadData);
	SyncSubsystem->QueueNetEntry(Entry);

	return true;
}

void AHUICRSyncHMDPawn::DeserializePayload()
{
	if (!bReceiveDataFromPeer || !SyncSubsystem || ActorID <= 0 || SharedPayloadStruct)
	{
		return;
	}

	const FHUICRSyncNetInnerMapEntry* InnerMapEntry = SyncSubsystem->ReceivedDataMap.Find(EHUICRSyncActorType::HMDPawn);
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

FTransform AHUICRSyncHMDPawn::ConvertHMDTransformToPC(const FTransform& InHMDTransform) const
{
	return SyncSubsystem ? SyncSubsystem->ConvertHMDActorTransformToPC(InHMDTransform) : InHMDTransform;
}

FTransform AHUICRSyncHMDPawn::ConvertPCTransformToHMD(const FTransform& InPCTransform) const
{
	return SyncSubsystem ? SyncSubsystem->ConvertPCActorTransformToHMD(InPCTransform) : InPCTransform;
}

void AHUICRSyncHMDPawn::BeginPlay()
{
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		SyncSubsystem = GameInstance->GetSubsystem<UHUICRSyncSubsystem>();
	}

	Super::BeginPlay();
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
		SyncSubsystem->UnregisterLocalHMDPawn(this);
		SyncSubsystem->OnPrepareNetDataSend.RemoveDynamic(this, &AHUICRSyncHMDPawn::HandlePrepareNetDataSend);
		SyncSubsystem->OnNetDataReceived.RemoveDynamic(this, &AHUICRSyncHMDPawn::DeserializePayload);
		SyncSubsystem->OnCalibrationStateChanged.RemoveDynamic(this, &AHUICRSyncHMDPawn::OnChangeCalibrationState);
	}

	if (IsValid(PawnSyncEndpoint))
	{
		PawnSyncEndpoint->Destroy();
		PawnSyncEndpoint = nullptr;
	}
	Super::EndPlay(EndPlayReason);
}

void AHUICRSyncHMDPawn::NotifyControllerChanged()
{
	Super::NotifyControllerChanged();
	if (!SyncSubsystem || !HasActorBegunPlay())
	{
		return;
	}
	if (HasLocalPlayerController(*this))
	{
		InitHUIID();
	}
	else
	{
		SyncSubsystem->UnregisterLocalHMDPawn(this);
		DeferredFieldSources.Reset();
	}
}

void AHUICRSyncHMDPawn::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (!SyncSubsystem || SyncSubsystem->Role != EHUICRSyncRole::HMD ||
		!SyncSubsystem->bConnectionState || !bSendDataToPeer || !HasLocalPlayerController(*this))
	{
		return;
	}

	const double Now = FPlatformTime::Seconds();
	if (Now < NextHeadPoseSendTime)
	{
		return;
	}
	NextHeadPoseSendTime = Now + HeadPoseSendInterval;

	// Participant tracking also runs before GameStart. Leave the existing Blueprint gameplay payload intact.
	TInlineComponentArray<UCameraComponent*> Cameras(this);
	for (const UCameraComponent* Camera : Cameras)
	{
		if (Camera->IsActive())
		{
			FTransform HeadPose = SyncSubsystem->ConvertHMDTransformToPC(Camera->GetComponentTransform());
			HeadPose.SetScale3D(FVector::OneVector);
			SyncSubsystem->SendLocalHeadPose(HeadPose);
			break;
		}
	}
}

void AHUICRSyncHMDPawn::OnChangeCalibrationState(bool bNewCalibrationState)
{
	// Kept intentionally empty to match the original HUIPawn behavior.
}

void AHUICRSyncHMDPawn::HandlePrepareNetDataSend()
{
	if (PawnSyncEndpoint)
	{
		PawnSyncEndpoint->bSendDataToPeer = bSendDataToPeer;
		PawnSyncEndpoint->bReceiveDataFromPeer = bReceiveDataFromPeer;
		SubmitDeferredFieldSources();
	}
	RegisterPayload();
}
