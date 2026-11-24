#include "HUICRSyncSubsystem.h"

#include "HUICRSyncActor.h"
#include "HUICRSyncFields.h"
#include "Misc/Base64.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Sockets.h"
#include "UObject/UnrealType.h"

namespace HUICRSharedFields
{
constexpr uint8 Upload = 1;
constexpr uint8 State = 2;
constexpr uint8 Ack = 3;
constexpr double RepairInterval = 0.35;
const TCHAR* const SourceAddress = TEXT("/HUICRSync/Fields/Source");

// Actor ID plus incarnation avoids applying delayed data to a new actor using the same ID.
// Use the reflected property name (including Blueprint member GUID) across cooked targets.
struct FAddress
{
	int32 ActorID = INDEX_NONE;
	uint64 Instance = 0;
	FName Field;

	void Write(FArchive& Ar) const
	{
		int32 Id = ActorID;
		uint64 Token = Instance;
		const FString Name = Field.ToString();
		const auto Utf8Name = StringCast<UTF8CHAR>(*Name);
		uint16 Length = static_cast<uint16>(Utf8Name.Length());
		Ar << Id << Token << Length;
		Ar.Serialize(const_cast<UTF8CHAR*>(Utf8Name.Get()), Length);
	}

	bool Read(FArchive& Ar)
	{
		uint16 Length = 0;
		Ar << ActorID << Instance << Length;
		if (Ar.IsError() || ActorID < 0 || Instance == 0 || Length == 0 || Length > 4 * NAME_SIZE ||
			Length > Ar.TotalSize() - Ar.Tell())
		{
			return false;
		}
		TArray<UTF8CHAR, TInlineAllocator<96>> Bytes;
		Bytes.SetNumUninitialized(Length);
		Ar.Serialize(Bytes.GetData(), Length);
		const auto Name = StringCast<TCHAR>(Bytes.GetData(), Length);
		if (Name.Length() >= NAME_SIZE)
		{
			return false;
		}
		const FString PropertyName(Name.Length(), Name.Get());
		Field = FName(*PropertyName, FNAME_Find);
		return !Field.IsNone() && !Ar.IsError();
	}
};

// Frame payload bytes occupy exactly the remainder of a bounded datagram or a bounded scene operation.
bool ReadValue(FArchive& Ar, TArray<uint8>& Value, int32 MaxBytes)
{
	const int64 Remaining = Ar.TotalSize() - Ar.Tell();
	if (Ar.IsError() || Remaining < 0 || Remaining > MaxBytes)
	{
		return false;
	}
	Value.SetNumUninitialized(static_cast<int32>(Remaining));
	if (Remaining)
	{
		Ar.Serialize(Value.GetData(), Remaining);
	}
	return !Ar.IsError();
}

void WriteValue(FArchive& Ar, const TArray<uint8>& Value)
{
	if (!Value.IsEmpty())
	{
		Ar.Serialize(const_cast<uint8*>(Value.GetData()), Value.Num());
	}
}

void WriteVersion(FArchive& Ar, uint64 Epoch, uint32 Sequence)
{
	Ar << Epoch << Sequence;
}

TArray<uint8> BuildState(const FAddress& Address, uint64 Epoch, uint32 Sequence, int32 WriterId, const TArray<uint8>& Value)
{
	TArray<uint8> Bytes;
	FMemoryWriter Ar(Bytes);
	Address.Write(Ar);
	WriteVersion(Ar, Epoch, Sequence);
	Ar << WriterId;
	WriteValue(Ar, Value);
	return Bytes;
}

// Keep room for session headers, the longest reflected field name and Base64 in a source command.
bool FitsDatagram(FName Name, int32 NumBytes, int32 PacketLimit)
{
	const FString TextName = Name.ToString();
	const int32 Length = StringCast<UTF8CHAR>(*TextName).Length();
	return NumBytes >= 0 && (static_cast<int64>(NumBytes) + Length + 32) * 4 / 3 + 160 <= FMath::Min(PacketLimit, 65507);
}
}

UHUICRSyncSubsystem::FFieldActorState& UHUICRSyncSubsystem::EnsurePCFieldActor(AHUICRSyncActor& Actor)
{
	FFieldActorState& State = FieldActors.FindOrAdd(Actor.ActorID);
	if (State.Actor.Get() != &Actor)
	{
		State = FFieldActorState();
		State.Instance = ++NextFieldVersion;
		State.Actor = &Actor;
	}
	return State;
}

void UHUICRSyncSubsystem::SeedInitialFields(AHUICRSyncActor& Actor)
{
	if (!Actor.SharedPayloadStruct)
	{
		return;
	}
	FFieldActorState& ActorState = EnsurePCFieldActor(Actor);
	for (TFieldIterator<FProperty> It(Actor.SharedPayloadStruct); It; ++It)
	{
		const FName Field = It->GetFName();
		TArray<uint8> Value;
		if (ActorState.Fields.Contains(Field) || !Actor.GetInitialSyncField(Field, Value) ||
			!HUICRSharedFields::FitsDatagram(Field, Value.Num(), MaxPacketBytes))
		{
			continue;
		}
		FSharedField& NewField = ActorState.Fields.Add(Field);
		SetFieldWriter(Actor, Field, NewField, INDEX_NONE, &Value);
	}
}

void UHUICRSyncSubsystem::SetFieldWriter(AHUICRSyncActor& Actor, FName Field, FSharedField& State,
	int32 Writer, const TArray<uint8>* Value)
{
	State.Writer = Writer;
	State.Epoch = ++NextFieldVersion;
	State.Sequence = 0;
	State.Deliveries.Reset();
	if (Value)
	{
		State.Value = *Value;
	}
	if (!State.Value.IsEmpty())
	{
		Actor.ApplyConfirmedSyncField(Field, State.Value, Writer != 0);
		bFieldReceivePending |= Writer != 0;
	}
	else if (Writer == 0)
	{
		Actor.ClearSyncFieldToApply(Field);
	}
}

bool UHUICRSyncSubsystem::RequestSyncFieldSource(AHUICRSyncActor* Actor, FName Field, EHUICRSyncFieldSource Source,
	const TArray<uint8>& SpecifiedValue)
{
	using namespace HUICRSharedFields;
	const bool bFixed = Source == EHUICRSyncFieldSource::UseSpecifiedValue;
	if (!IsValid(Actor) || Actor->TypeCode != EHUICRSyncActorType::HUIActor ||
		(!bFixed && Source != EHUICRSyncFieldSource::FollowMyPayload) ||
		!FitsDatagram(Field, SpecifiedValue.Num(), MaxPacketBytes) || !Actor->ValidateSyncField(Field, bFixed ? &SpecifiedValue : nullptr))
	{
		return false;
	}
	if (Role == EHUICRSyncRole::None || (Role == EHUICRSyncRole::PC &&
		(Actor->ActorID < 0 || FindSyncActor(Actor->TypeCode, Actor->ActorID) != Actor)))
	{
		// A PC Actor's Blueprint BeginPlay may precede its manager's role configuration or actor registration.
		for (FEarlyFieldRequest& Pending : EarlyFieldRequests)
		{
			if (Pending.Actor.Get() == Actor && Pending.Field == Field)
			{
				Pending.Source = Source;
				Pending.Value = SpecifiedValue;
				return true;
			}
		}
		EarlyFieldRequests.Add({ Actor, Field, Source, SpecifiedValue });
		return true;
	}
	if (Role == EHUICRSyncRole::PC)
	{
		FFieldActorState& ActorState = EnsurePCFieldActor(*Actor);
		FSharedField& FieldState = ActorState.Fields.FindOrAdd(Field);
		if (!FieldState.Epoch && !bFixed)
		{
			Actor->GetInitialSyncField(Field, FieldState.Value);
		}
		SetFieldWriter(*Actor, Field, FieldState, bFixed ? INDEX_NONE : 0, bFixed ? &SpecifiedValue : nullptr);
		return true;
	}
	const FFieldActorState* ActorState = FieldActors.Find(Actor->ActorID);
	if (!ActorState || ActorState->Actor.Get() != Actor || !bLocalSceneReady || bGameStopped || !bManagedCalibration)
	{
		return false;
	}
	TArray<uint8> Bytes;
	FMemoryWriter Writer(Bytes);
	FAddress{ Actor->ActorID, ActorState->Instance, Field }.Write(Writer);
	uint32 Request = ++NextFieldRequest;
	uint8 Mode = static_cast<uint8>(Source);
	Writer << LocalSceneGeneration << Request << Mode;
	if (bFixed)
	{
		WriteValue(Writer, SpecifiedValue);
	}
	FHUICRSyncCommandArg Arg;
	Arg.Type = EHUICRSyncCommandArgType::String;
	Arg.StringValue = FBase64::Encode(Bytes);
	return SendReliableCommand(SourceAddress, { Arg });
}

bool UHUICRSyncSubsystem::HandleFieldCommand(FHUICRSyncPeerRuntimeState& Peer, const FHUICRSyncCommandPayload& Command)
{
	using namespace HUICRSharedFields;
	if (Command.Address != SourceAddress)
	{
		return false;
	}
	if (Role != EHUICRSyncRole::PC || !bManagedCalibration || bGameStopped || !Peer.Info.bConnected ||
		!Peer.bHasCalibrated || Peer.ReadySceneGeneration <= 0 || Command.Args.Num() != 1 ||
		Command.Args[0].Type != EHUICRSyncCommandArgType::String || Command.Args[0].StringValue.Len() > MaxPacketBytes)
	{
		return true;
	}
	TArray<uint8> Bytes;
	if (!FBase64::Decode(Command.Args[0].StringValue, Bytes))
	{
		return true;
	}
	FMemoryReader Reader(Bytes);
	FAddress Address;
	if (!Address.Read(Reader))
	{
		return true;
	}
	int32 Generation = 0;
	uint32 Request = 0;
	uint8 Mode = 0;
	Reader << Generation << Request << Mode;
	const bool bFixed = Mode == static_cast<uint8>(EHUICRSyncFieldSource::UseSpecifiedValue);
	FFieldActorState* ActorState = FieldActors.Find(Address.ActorID);
	const FManagedSceneActor* Record = ManagedSceneActors.Find(MakeActorKey(EHUICRSyncActorType::HUIActor, Address.ActorID));
	const int32 Participant = Peer.Info.ParticipantId;
	TArray<uint8> Value;
	if (!ActorState || ActorState->Instance != Address.Instance || !ActorState->Actor.IsValid() ||
		!Record || Record->ExcludedParticipantId == Participant || Generation != Peer.ReadySceneGeneration ||
		Generation != Peer.SceneGeneration || !Request || (!bFixed && Mode != static_cast<uint8>(EHUICRSyncFieldSource::FollowMyPayload)) ||
		!ReadValue(Reader, Value, MaxPacketBytes) || !FitsDatagram(Address.Field, Value.Num(), MaxPacketBytes) ||
		(!bFixed && !Value.IsEmpty()) || !ActorState->Actor->ValidateSyncField(Address.Field, bFixed ? &Value : nullptr))
	{
		return true;
	}
	FSharedField& FieldState = ActorState->Fields.FindOrAdd(Address.Field);
	uint32& LastRequest = FieldState.LastRequests.FindOrAdd(Participant);
	if (Request > LastRequest)
	{
		LastRequest = Request;
		SetFieldWriter(*ActorState->Actor.Get(), Address.Field, FieldState, bFixed ? INDEX_NONE : Participant, bFixed ? &Value : nullptr);
	}
	return true;
}

void UHUICRSyncSubsystem::RevokeParticipantFields(int32 ParticipantId)
{
	for (auto& ActorPair : FieldActors)
	{
		AHUICRSyncActor* Actor = ActorPair.Value.Actor.Get();
		for (auto& FieldPair : ActorPair.Value.Fields)
		{
			FSharedField& State = FieldPair.Value;
			State.Deliveries.Remove(ParticipantId);
			State.LastRequests.Remove(ParticipantId);
			if (Actor && State.Writer == ParticipantId)
			{
				SetFieldWriter(*Actor, FieldPair.Key, State, INDEX_NONE, nullptr);
			}
		}
	}
}

void UHUICRSyncSubsystem::SendFieldPacket(FHUICRSyncPeerRuntimeState& Peer, uint8 Kind, const TArray<uint8>& Payload)
{
	if (!SenderSocket || !Peer.UDPAddr.IsValid())
	{
		return;
	}
	TArray<uint8> Datagram;
	FMemoryWriter Writer(Datagram);
	WriteSessionHeader(Writer, Peer);
	uint32 Magic = FieldPacketMagic;
	int32 Generation = Role == EHUICRSyncRole::PC ? Peer.SceneGeneration : LocalSceneGeneration;
	Writer << Magic << Kind << Generation;
	HUICRSharedFields::WriteValue(Writer, Payload);
	if (Datagram.Num() <= FMath::Min(MaxPacketBytes, 65507))
	{
		int32 BytesSent = 0;
		SenderSocket->SendTo(Datagram.GetData(), Datagram.Num(), BytesSent, *Peer.UDPAddr);
	}
}

bool UHUICRSyncSubsystem::ReceiveFieldPacket(FHUICRSyncPeerRuntimeState& Peer, FArchive& Reader)
{
	using namespace HUICRSharedFields;
	if (!bManagedCalibration || !Peer.Info.bConnected || Reader.TotalSize() > MaxPacketBytes)
	{
		return false;
	}
	uint8 Kind = 0;
	int32 Generation = 0;
	Reader << Kind << Generation;
	if (Reader.IsError() || Generation <= 0)
	{
		return false;
	}
	if (Role == EHUICRSyncRole::HMD)
	{
		return Kind == State && !bIsLocalHMDCalibrating && Generation == LocalSceneGeneration &&
			Generation >= ExpectedSceneGeneration && ReceiveFieldState(Reader, false, &Peer);
	}
	if (Role != EHUICRSyncRole::PC || (Kind != Ack && Kind != Upload) || !Peer.bHasCalibrated ||
		Generation != Peer.ReadySceneGeneration || Generation != Peer.SceneGeneration)
	{
		return false;
	}
	FAddress Address;
	if (!Address.Read(Reader))
	{
		return false;
	}
	uint64 Epoch = 0;
	uint32 Sequence = 0;
	Reader << Epoch << Sequence;
	FFieldActorState* ActorState = FieldActors.Find(Address.ActorID);
	FSharedField* Current = ActorState ? ActorState->Fields.Find(Address.Field) : nullptr;
	if (Reader.IsError() || !ActorState || ActorState->Instance != Address.Instance || !ActorState->Actor.IsValid() ||
		!Current || Epoch == 0 || Epoch != Current->Epoch)
	{
		return false;
	}
	const int32 Participant = Peer.Info.ParticipantId;
	if (Kind == Ack)
	{
		FFieldDelivery* Delivery = Current->Deliveries.Find(Participant);
		if (Delivery && Reader.AtEnd() && Delivery->SentEpoch == Epoch && Delivery->SentSequence == Sequence)
		{
			Delivery->bAcknowledged = true;
		}
		return false;
	}
	TArray<uint8> Value;
	if (bGameStopped || Current->Writer != Participant || Sequence <= Current->Sequence ||
		!ReadValue(Reader, Value, MaxPacketBytes) || Value.IsEmpty() ||
		!FitsDatagram(Address.Field, Value.Num(), MaxPacketBytes) ||
		!ActorState->Actor->ApplyConfirmedSyncField(Address.Field, Value, true))
	{
		return false;
	}
	Current->Value = MoveTemp(Value);
	Current->Sequence = Sequence;
	return true;
}

bool UHUICRSyncSubsystem::ReceiveFieldState(FArchive& Reader, bool bSnapshot, FHUICRSyncPeerRuntimeState* Peer)
{
	using namespace HUICRSharedFields;
	FAddress Address;
	if (!Address.Read(Reader))
	{
		return false;
	}
	uint64 Epoch = 0;
	uint32 Sequence = 0;
	int32 Writer = INDEX_NONE;
	Reader << Epoch << Sequence << Writer;
	FFieldActorState* ActorState = FieldActors.Find(Address.ActorID);
	TArray<uint8> Value;
	if (!ActorState || ActorState->Instance != Address.Instance || !ActorState->Actor.IsValid() || Epoch == 0 || Writer < INDEX_NONE ||
		!ReadValue(Reader, Value, MaxPacketBytes))
	{
		return false;
	}
	AHUICRSyncActor& Actor = *ActorState->Actor.Get();
	if (!Actor.ValidateSyncField(Address.Field, nullptr))
	{
		return false;
	}
	FSharedField& Current = ActorState->Fields.FindOrAdd(Address.Field);
	const bool bNewEpoch = Epoch > Current.Epoch;
	const bool bNewValue = bNewEpoch || (Epoch == Current.Epoch && Sequence > Current.Sequence);
	if (bNewValue)
	{
		const bool bApplyLocally = Writer != LocalParticipantId;
		if (!Value.IsEmpty() && !Actor.ApplyConfirmedSyncField(Address.Field, Value, bApplyLocally))
		{
			return false;
		}
		if (!bApplyLocally && Value.IsEmpty())
		{
			Actor.ClearSyncFieldToApply(Address.Field);
		}
		Current.Value = MoveTemp(Value);
		Current.Writer = Writer;
		Current.Sequence = Sequence;
		Current.Epoch = Epoch;
		if (bNewEpoch)
		{
			Current.UploadSequence = Sequence;
			Current.UploadValue = Current.Value;
			Current.RetryUploadAt = 0;
		}
		bFieldReceivePending |= bApplyLocally && !Current.Value.IsEmpty();
	}
	if (!bSnapshot && Peer && Epoch == Current.Epoch && Sequence == Current.Sequence)
	{
		TArray<uint8> Response;
		FMemoryWriter ResponseWriter(Response);
		Address.Write(ResponseWriter);
		WriteVersion(ResponseWriter, Epoch, Sequence);
		SendFieldPacket(*Peer, Ack, Response);
	}
	return bNewValue && Writer != LocalParticipantId && !Current.Value.IsEmpty();
}

void UHUICRSyncSubsystem::QueueFieldSnapshot(FHUICRSyncPeerRuntimeState& Peer, const FFieldActorState& ActorState)
{
	for (const auto& FieldPair : ActorState.Fields)
	{
		const FSharedField& Field = FieldPair.Value;
		FHUICRSyncSceneOperation& Operation = Peer.SceneOperations.AddDefaulted_GetRef();
		Operation.Kind = EHUICRSyncSceneOp::Field;
		Operation.Bytes = HUICRSharedFields::BuildState({ ActorState.Actor->ActorID, ActorState.Instance, FieldPair.Key },
			Field.Epoch, Field.Sequence, Field.Writer, Field.Value);
	}
}

void UHUICRSyncSubsystem::TickSyncFields()
{
	using namespace HUICRSharedFields;
	if (Role != EHUICRSyncRole::None && !EarlyFieldRequests.IsEmpty())
	{
		TArray<FEarlyFieldRequest> Requests = MoveTemp(EarlyFieldRequests);
		if (Role == EHUICRSyncRole::PC)
		{
			for (const FEarlyFieldRequest& Request : Requests)
			{
				if (AHUICRSyncActor* Actor = Request.Actor.Get())
				{
					RequestSyncFieldSource(Actor, Request.Field, Request.Source, Request.Value);
				}
			}
		}
	}
	if (!bManagedCalibration || !bConnectionState || bGameStopped)
	{
		return;
	}
	const bool bPC = Role == EHUICRSyncRole::PC;
	FHUICRSyncPeerRuntimeState* Server = bPC ? nullptr : FindDefaultPeer();
	if (!bPC && (!Server || !bLocalSceneReady))
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	for (auto It = FieldActors.CreateIterator(); It; ++It)
	{
		FFieldActorState& ActorState = It.Value();
		AHUICRSyncActor* Actor = ActorState.Actor.Get();
		if (!Actor)
		{
			It.RemoveCurrent();
			continue;
		}
		for (auto& FieldPair : ActorState.Fields)
		{
			FSharedField& Field = FieldPair.Value;
			const FName FieldName = FieldPair.Key;
			const FAddress Address { Actor->ActorID, ActorState.Instance, FieldName };
			if (Field.Writer == (bPC ? 0 : LocalParticipantId))
			{
				TArray<uint8> Sample;
				if (Actor->GetPreparedSyncField(FieldName, Sample) && FitsDatagram(FieldName, Sample.Num(), MaxPacketBytes))
				{
					if (bPC && Sample != Field.Value)
					{
						++Field.Sequence;
						Field.Value = MoveTemp(Sample);
						Actor->ApplyConfirmedSyncField(FieldName, Field.Value, false);
					}
					else if (!bPC && Sample != Field.UploadValue)
					{
						++Field.UploadSequence;
						Field.UploadValue = MoveTemp(Sample);
						Field.RetryUploadAt = 0;
					}
				}
				if (!bPC && Field.UploadSequence > Field.Sequence && Now >= Field.RetryUploadAt)
				{
					TArray<uint8> Packet;
					FMemoryWriter Ar(Packet);
					Address.Write(Ar);
					WriteVersion(Ar, Field.Epoch, Field.UploadSequence);
					WriteValue(Ar, Field.UploadValue);
					SendFieldPacket(*Server, Upload, Packet);
					Field.RetryUploadAt = Now + RepairInterval;
				}
			}
			if (!bPC)
			{
				continue;
			}
			const FManagedSceneActor* SceneActor = ManagedSceneActors.Find(MakeActorKey(Actor->TypeCode, Actor->ActorID));
			if (!SceneActor)
			{
				continue;
			}
			TArray<uint8> Packet;
			for (auto& PeerPair : Peers)
			{
				FHUICRSyncPeerRuntimeState& Peer = PeerPair.Value;
				if (!Peer.Info.bConnected || !Peer.bHasCalibrated || Peer.ReadySceneGeneration <= 0 ||
					Peer.ReadySceneGeneration != Peer.SceneGeneration || Peer.Info.ParticipantId == SceneActor->ExcludedParticipantId)
				{
					continue;
				}
				FFieldDelivery& Delivery = Field.Deliveries.FindOrAdd(Peer.Info.ParticipantId);
				if (Delivery.SentEpoch == Field.Epoch && Delivery.SentSequence == Field.Sequence &&
					(Delivery.bAcknowledged || Now < Delivery.RetryAt))
				{
					continue;
				}
				if (Packet.IsEmpty())
				{
					Packet = BuildState(Address, Field.Epoch, Field.Sequence, Field.Writer, Field.Value);
				}
				SendFieldPacket(Peer, State, Packet);
				Delivery = {Field.Epoch, Field.Sequence, Now + RepairInterval, false};
			}
		}
	}
}
