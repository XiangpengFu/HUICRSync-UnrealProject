#include "HUICRSyncActor.h"

#include "Engine/GameInstance.h"
#include "HUICRSyncPayloadLibrary.h"
#include "HUICRSyncSubsystem.h"
#include "UObject/UnrealType.h"

namespace HUICRSyncActorFields
{
FProperty* FindField(const UScriptStruct* Schema, FName Field)
{
	FProperty* Property = Schema && !Field.IsNone() ? Schema->FindPropertyByName(Field) : nullptr;
	return Property && HUICRSyncFields::IsSupportedField(*Property) ? Property : nullptr;
}
}

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

	if (SharedPayloadStruct)
	{
		// Existing Make Struct -> Write Instanced Struct graphs can prepare the same schema.
		if (!ActorSendPayloadData.IsEmpty() && HUICRSyncFields::CanUseLegacyBytes(SharedPayloadStruct))
		{
			FInstancedStruct LocalPayload;
			if (UHUICRSyncPayloadLibrary::ReadInstancedStruct(ActorSendPayloadData, SharedPayloadStruct, LocalPayload))
			{
				PrepareOutgoingPayloadInternal(LocalPayload);
			}
		}
		ActorSendPayloadData.Reset();
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
	if (!SyncSubsystem || !bReceiveDataFromPeer || SharedPayloadStruct)
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

void AHUICRSyncActor::InitializeSpawnPayload(const TArray<uint8>& SpawnPayload)
{
	bHasExplicitSpawnPayload = !SpawnPayload.IsEmpty();
	ActorInitPayloadData = SpawnPayload;
}

bool AHUICRSyncActor::SetInitialPayloadInternal(const FInstancedStruct& Payload)
{
	// Construction Script and BeginPlay may provide defaults when the spawner supplied none.
	if ((bHasExplicitSpawnPayload && !HasActorBegunPlay()) || !SharedPayloadStruct ||
		Payload.GetScriptStruct() != SharedPayloadStruct || !HUICRSyncFields::CanUseLegacyBytes(SharedPayloadStruct))
	{
		return false;
	}
	TArray<uint8> InitialBytes;
	if (!UHUICRSyncPayloadLibrary::WriteInstancedStruct(InitialBytes, SharedPayloadStruct, Payload))
	{
		return false;
	}
	ActorInitPayloadData = MoveTemp(InitialBytes);
	return true;
}

bool AHUICRSyncActor::ReadInitialPayloadInternal(FInstancedStruct& Payload) const
{
	// Spawn bytes have no schema tag. Accept only a complete round trip with this actor's shared struct.
	TArray<uint8> RoundTrip;
	if (!ActorInitPayloadData.IsEmpty() && HUICRSyncFields::CanUseLegacyBytes(SharedPayloadStruct) &&
		UHUICRSyncPayloadLibrary::ReadInstancedStruct(ActorInitPayloadData, SharedPayloadStruct, Payload) &&
		UHUICRSyncPayloadLibrary::WriteInstancedStruct(RoundTrip, SharedPayloadStruct, Payload) &&
		RoundTrip == ActorInitPayloadData)
	{
		return true;
	}
	Payload.Reset();
	return false;
}

bool AHUICRSyncActor::PrepareOutgoingPayloadInternal(const FInstancedStruct& Payload)
{
	if (!SharedPayloadStruct || !bSendDataToPeer || Payload.GetScriptStruct() != SharedPayloadStruct)
	{
		return false;
	}
	PreparedFields = Payload;
	return true;
}

bool AHUICRSyncActor::SetSyncFieldSourceInternal(FName Field, EHUICRSyncFieldSource Source, const FInstancedStruct& Payload)
{
	const FProperty* Property = HUICRSyncActorFields::FindField(SharedPayloadStruct, Field);
	if (!bSendDataToPeer || !Property)
	{
		return false;
	}
	TArray<uint8> Value;
	if (Source == EHUICRSyncFieldSource::UseSpecifiedValue)
	{
		if (Payload.GetScriptStruct() != SharedPayloadStruct || !HUICRSyncFields::Encode(*Property, Payload, Value))
		{
			return false;
		}
	}
	else if (Source != EHUICRSyncFieldSource::FollowMyPayload)
	{
		return false;
	}
	if (!SyncSubsystem)
	{
		// ReceiveBeginPlay runs inside Super::BeginPlay, before this actor has its participant and actor IDs.
		if (!IsActorBeginningPlay())
		{
			return false;
		}
		BeginPlayFieldRequests.Add(Property->GetFName(), { Source, MoveTemp(Value) });
		return true;
	}
	return SyncSubsystem->RequestSyncFieldSource(this, Property->GetFName(), Source, Value);
}

bool AHUICRSyncActor::GetPreparedSyncField(FName Field, TArray<uint8>& OutBytes) const
{
	OutBytes.Reset();
	const FProperty* Property = HUICRSyncActorFields::FindField(SharedPayloadStruct, Field);
	return bSendDataToPeer && Property && PreparedFields.GetScriptStruct() == SharedPayloadStruct &&
		HUICRSyncFields::Encode(*Property, PreparedFields, OutBytes);
}

bool AHUICRSyncActor::GetInitialSyncField(FName Field, TArray<uint8>& OutBytes) const
{
	OutBytes.Reset();
	const FProperty* Property = HUICRSyncActorFields::FindField(SharedPayloadStruct, Field);
	FInstancedStruct Initial;
	return Property && ReadInitialPayloadInternal(Initial) && HUICRSyncFields::Encode(*Property, Initial, OutBytes);
}

bool AHUICRSyncActor::ValidateSyncField(FName Field, const TArray<uint8>* OptionalBytes) const
{
	FProperty* Property = HUICRSyncActorFields::FindField(SharedPayloadStruct, Field);
	return Property && (!OptionalBytes || HUICRSyncFields::Decode(*Property, *OptionalBytes, nullptr));
}

bool AHUICRSyncActor::ApplyConfirmedSyncField(FName Field, const TArray<uint8>& Value, bool bApplyLocally)
{
	FProperty* Property = HUICRSyncActorFields::FindField(SharedPayloadStruct, Field);
	if (!Property)
	{
		return false;
	}
	const bool bHadPayload = ConfirmedFields.GetScriptStruct() == SharedPayloadStruct;
	if (!bHadPayload)
	{
		ConfirmedFields.InitializeAs(SharedPayloadStruct);
	}
	if (!HUICRSyncFields::Decode(*Property, Value, &ConfirmedFields))
	{
		if (!bHadPayload)
		{
			ConfirmedFields.Reset();
		}
		return false;
	}
	const FName AuthoredName(*Property->GetAuthoredName());
	if (bApplyLocally && bReceiveDataFromPeer)
	{
		UnreadFields.AddUnique(AuthoredName);
	}
	else
	{
		UnreadFields.Remove(AuthoredName);
		LastReadFields.Remove(AuthoredName);
	}
	if (bReceiveDataFromPeer && HUICRSyncFields::CanUseLegacyBytes(SharedPayloadStruct))
	{
		UHUICRSyncPayloadLibrary::WriteInstancedStruct(ActorReceivePayloadData, SharedPayloadStruct, ConfirmedFields);
	}
	return true;
}

void AHUICRSyncActor::ClearSyncFieldToApply(FName Field)
{
	if (const FProperty* Property = HUICRSyncActorFields::FindField(SharedPayloadStruct, Field))
	{
		const FName AuthoredName(*Property->GetAuthoredName());
		UnreadFields.Remove(AuthoredName);
		LastReadFields.Remove(AuthoredName);
	}
}

void AHUICRSyncActor::ResetConfirmedSyncFields()
{
	if (SharedPayloadStruct)
	{
		ActorReceivePayloadData.Reset();
	}
	ConfirmedFields.Reset();
	LastReadFields.Reset();
	UnreadFields.Reset();
	LastFieldReadFrame = MAX_uint64;
}

bool AHUICRSyncActor::ReadReceivedPayloadInternal(FInstancedStruct& Payload, TArray<FName>& FieldsToApply)
{
	FieldsToApply.Reset();
	if (!bReceiveDataFromPeer || !SharedPayloadStruct || ConfirmedFields.GetScriptStruct() != SharedPayloadStruct)
	{
		Payload.Reset();
		return false;
	}
	if (LastFieldReadFrame != GFrameCounter)
	{
		LastFieldReadFrame = GFrameCounter;
		LastReadFields.Reset();
	}
	for (FName Field : UnreadFields)
	{
		LastReadFields.AddUnique(Field);
	}
	UnreadFields.Reset();
	FieldsToApply = LastReadFields;
	Payload = ConfirmedFields;
	return true;
}

void AHUICRSyncActor::QueueSpawnMirror()
{
	if (!SyncSubsystem)
	{
		return;
	}

	if (SyncSubsystem->Role == EHUICRSyncRole::PC)
	{
		SyncSubsystem->QueueSpawnMirror(GetRemoteCounterpartClass(), ActorID, GetActorTransform(), ActorInitPayloadData);
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
	return SyncSubsystem ? SyncSubsystem->ConvertHMDActorTransformToPC(InHMDTransform) : InHMDTransform;
}

FTransform AHUICRSyncActor::ConvertPCTransformToHMD(const FTransform& InPCTransform) const
{
	return SyncSubsystem ? SyncSubsystem->ConvertPCActorTransformToHMD(InPCTransform) : InPCTransform;
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
		for (const auto& Pair : BeginPlayFieldRequests)
		{
			SyncSubsystem->RequestSyncFieldSource(this, Pair.Key, Pair.Value.Source, Pair.Value.Value);
		}
		BeginPlayFieldRequests.Reset();
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
