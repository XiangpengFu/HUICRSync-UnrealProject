#include "HUICRSyncSubsystem.h"

#include "HUICRSyncActor.h"
#include "HUICRSyncHMDPawn.h"
#include "Serialization/MemoryWriter.h"
#include "Sockets.h"
#include "UObject/UnrealType.h"

namespace HUICRPawnNet
{
enum : uint8 { Describe, Confirm, Sample };
constexpr double RetryInterval = 0.4;

// Length-check before allocating. Class and struct paths are small relative to the UDP limit.
bool SerializePath(FArchive& Ar, FString& Path)
{
	const auto Utf8 = StringCast<UTF8CHAR>(*Path);
	int32 Length = Utf8.Length();
	Ar << Length;
	if (Ar.IsError() || Length < 0 || Length > 4096 || (Ar.IsLoading() && Length > Ar.TotalSize() - Ar.Tell()))
	{
		return false;
	}
	if (Ar.IsLoading())
	{
		TArray<UTF8CHAR, TInlineAllocator<256>> Bytes;
		Bytes.SetNumUninitialized(Length + 1);
		Ar.Serialize(Bytes.GetData(), Length);
		Bytes[Length] = static_cast<UTF8CHAR>(0);
		Path = UTF8_TO_TCHAR(Bytes.GetData());
	}
	else if (Length)
	{
		Ar.Serialize(const_cast<UTF8CHAR*>(Utf8.Get()), Length);
	}
	return !Ar.IsError();
}
}

int32 UHUICRSyncSubsystem::FieldActorKey(const AHUICRSyncActor& Actor)
{
	return Actor.TypeCode == EHUICRSyncActorType::HMDPawn ? -Actor.ActorID : Actor.ActorID;
}

void UHUICRSyncSubsystem::RegisterLocalHMDPawn(AHUICRSyncHMDPawn* Pawn)
{
	if (!IsValid(Pawn) || LocalHMDPawn.Get() == Pawn || Role == EHUICRSyncRole::PC)
	{
		return;
	}
	ResetLocalPawnPairing();
	LocalHMDPawn = Pawn;
	Pawn->ActorID = INDEX_NONE;
	++LocalPawnRevision;
}

void UHUICRSyncSubsystem::UnregisterLocalHMDPawn(AHUICRSyncHMDPawn* Pawn)
{
	if (LocalHMDPawn.Get(true) == Pawn)
	{
		ResetLocalPawnPairing();
		LocalHMDPawn.Reset();
		++LocalPawnRevision;
	}
}

void UHUICRSyncSubsystem::ResetLocalPawnPairing()
{
	if (AHUICRSyncHMDPawn* Pawn = LocalHMDPawn.Get(true))
	{
		if (AHUICRSyncActor* Endpoint = Pawn->GetPawnSyncEndpoint(); IsValid(Endpoint) && Endpoint->ActorID > 0)
		{
			FieldActors.Remove(FieldActorKey(*Endpoint));
			UnregisterSyncActor(Endpoint);
			Endpoint->ResetConfirmedSyncFields();
			Endpoint->SetSyncActorID(INDEX_NONE);
		}
		Pawn->ActorID = INDEX_NONE;
		Pawn->ActorReceivePayloadData.Reset();
	}
	ConfirmedPawnRevision = 0;
	NextPawnDescriptionTime = 0;
	NextPawnSample = 0;
}

void UHUICRSyncSubsystem::RemovePCPawn(FHUICRSyncPeerRuntimeState& Peer)
{
	AHUICRSyncActor* Actor = Peer.PawnCounterpart.Get();
	Peer.PawnCounterpart.Reset();
	Peer.PawnInstance = 0;
	Peer.LastPawnSample = 0;
	if (Actor)
	{
		UE_LOG(LogTemp, Log, TEXT("HUICRSync removing HUI Pawn %s for participant %d"), *Actor->GetName(), Peer.Info.ParticipantId);
		FieldActors.Remove(FieldActorKey(*Actor));
		UnregisterSyncActor(Actor);
		Actor->Destroy();
	}
}

void UHUICRSyncSubsystem::SendPawnPacket(FHUICRSyncPeerRuntimeState& Peer, uint8 Kind, int32 Revision, const TArray<uint8>& Bytes)
{
	if (!SenderSocket || !Peer.UDPAddr || !Peer.Info.bConnected)
	{
		return;
	}
	TArray<uint8> Packet;
	FMemoryWriter Writer(Packet);
	WriteSessionHeader(Writer, Peer);
	uint32 Magic = PawnPacketMagic;
	Writer << Magic << Kind << Revision;
	Packet.Append(Bytes);
	if (Packet.Num() <= FMath::Min(MaxPacketBytes, 65507))
	{
		int32 Sent = 0;
		SenderSocket->SendTo(Packet.GetData(), Packet.Num(), Sent, *Peer.UDPAddr);
	}
}

void UHUICRSyncSubsystem::TickPawnPairing()
{
	if (Role != EHUICRSyncRole::HMD || !bConnectionState || LocalParticipantId <= 0 || !LocalPawnRevision)
	{
		return;
	}
	FHUICRSyncPeerRuntimeState* Server = FindDefaultPeer();
	AHUICRSyncHMDPawn* Pawn = LocalHMDPawn.Get();
	if (!Server)
	{
		return;
	}
	if (Pawn && Pawn->ActorID != LocalParticipantId)
	{
		Pawn->ActorID = LocalParticipantId;
		if (AHUICRSyncActor* Endpoint = Pawn->EnsurePawnSyncEndpoint())
		{
			Endpoint->SetSyncActorID(LocalParticipantId);
			RegisterSyncActor(Endpoint);
		}
	}
	const double Now = FPlatformTime::Seconds();
	if (Now < NextPawnDescriptionTime)
	{
		return;
	}
	// Retrying also restores a counterpart if the PC map was reloaded while the connection stayed up.
	NextPawnDescriptionTime = Now + (ConfirmedPawnRevision == LocalPawnRevision ? 5.0 : HUICRPawnNet::RetryInterval);
	TArray<uint8> Bytes;
	FMemoryWriter Writer(Bytes);
	if (Pawn)
	{
		FString HMDClass = Pawn->GetClass()->GetPathName();
		FString PCClass = Pawn->RemoteCounterpartClass.ToSoftObjectPath().ToString();
		FString Schema = GetPathNameSafe(Pawn->SharedPayloadStruct);
		if (!HUICRPawnNet::SerializePath(Writer, HMDClass) || !HUICRPawnNet::SerializePath(Writer, PCClass) ||
			!HUICRPawnNet::SerializePath(Writer, Schema))
		{
			return;
		}
		Bytes.Append(Pawn->ActorInitPayloadData);
	}
	SendPawnPacket(*Server, HUICRPawnNet::Describe, LocalPawnRevision, Bytes);
}

void UHUICRSyncSubsystem::SendPawnSample(const FHUICRSyncNetEntry& Entry)
{
	if (bGameStopped || Entry.ActorID <= 0 || Entry.PayloadBytes.IsEmpty())
	{
		return;
	}
	const bool bPC = Role == EHUICRSyncRole::PC;
	FHUICRSyncPeerRuntimeState* Peer = bPC ? FindCalibrationParticipant(Entry.ActorID) : FindDefaultPeer();
	if (!Peer || !Peer->PawnInstance || (bPC ? !Peer->PawnCounterpart.IsValid() :
		Entry.ActorID != LocalParticipantId || ConfirmedPawnRevision != LocalPawnRevision))
	{
		return;
	}
	if (bManagedCalibration && (bPC ? !Peer->bHasCalibrated || Peer->ReadySceneGeneration <= 0 ||
		Peer->ReadySceneGeneration != Peer->SceneGeneration : !bLocalSceneReady))
	{
		return;
	}
	TArray<uint8> Bytes;
	FMemoryWriter Writer(Bytes);
	uint32 Sequence = ++NextPawnSample;
	int32 Generation = bPC ? Peer->SceneGeneration : LocalSceneGeneration;
	Writer << Peer->PawnInstance << Sequence << Generation;
	Bytes.Append(Entry.PayloadBytes);
	SendPawnPacket(*Peer, HUICRPawnNet::Sample, bPC ? Peer->PawnRevision : LocalPawnRevision, Bytes);
}

bool UHUICRSyncSubsystem::ReceivePawnPacket(FHUICRSyncPeerRuntimeState& Peer, FArchive& Reader)
{
	using namespace HUICRPawnNet;
	uint8 Kind = 0;
	int32 Revision = 0;
	Reader << Kind << Revision;
	if (Reader.IsError() || Revision <= 0 || !Peer.Info.bConnected || Reader.TotalSize() > FMath::Min(MaxPacketBytes, 65507))
	{
		return false;
	}
	const bool bPC = Role == EHUICRSyncRole::PC;
	const int32 Participant = bPC ? Peer.Info.ParticipantId : LocalParticipantId;
	if (Participant <= 0)
	{
		return false;
	}
	if (Kind == Sample)
	{
		uint64 Instance = 0;
		uint32 Sequence = 0;
		int32 Generation = 0;
		Reader << Instance << Sequence << Generation;
		if (Reader.IsError() || Reader.AtEnd() || bGameStopped || !Instance || Instance != Peer.PawnInstance ||
			Revision != (bPC ? Peer.PawnRevision : ConfirmedPawnRevision) || static_cast<int32>(Sequence - Peer.LastPawnSample) <= 0 ||
			(bManagedCalibration && (bPC ? !Peer.bHasCalibrated || Peer.ReadySceneGeneration <= 0 ||
				Generation != Peer.ReadySceneGeneration || Generation != Peer.SceneGeneration : !bLocalSceneReady || Generation != LocalSceneGeneration)))
		{
			return false;
		}
		AHUICRSyncHMDPawn* Pawn = LocalHMDPawn.Get();
		AHUICRSyncActor* PCActor = Peer.PawnCounterpart.Get();
		if (bPC ? !PCActor || PCActor->SharedPayloadStruct : !Pawn || Pawn->SharedPayloadStruct)
		{
			return false;
		}
		Peer.LastPawnSample = Sequence;
		FHUICRSyncNetEntry& Entry = ReceivedDataMap.FindOrAdd(EHUICRSyncActorType::HMDPawn).InnerMap.FindOrAdd(Participant);
		Entry.TypeCode = EHUICRSyncActorType::HMDPawn;
		Entry.ActorID = Participant;
		Entry.PayloadBytes.SetNumUninitialized(static_cast<int32>(Reader.TotalSize() - Reader.Tell()));
		Reader.Serialize(Entry.PayloadBytes.GetData(), Entry.PayloadBytes.Num());
		if (Reader.IsError())
		{
			return false;
		}
		// Legacy Blueprint listeners can bind before the actor's native OnNetDataReceived handler.
		if (bPC) { PCActor->DeserializePayload(); }
		else { Pawn->DeserializePayload(); }
		return true;
	}
	if (Kind == Confirm && !bPC && Revision == LocalPawnRevision)
	{
		uint64 Instance = 0;
		Reader << Instance;
		if (Reader.IsError() || !Reader.AtEnd() || (ConfirmedPawnRevision == Revision && Instance < Peer.PawnInstance))
		{
			return false;
		}
		if (Instance && LocalHMDPawn.IsValid())
		{
			if (AHUICRSyncActor* Endpoint = LocalHMDPawn->GetPawnSyncEndpoint())
			{
				FFieldActorState& State = FieldActors.FindOrAdd(FieldActorKey(*Endpoint));
				if (State.Instance != Instance || State.Actor.Get() != Endpoint)
				{
					Endpoint->ResetConfirmedSyncFields();
					LocalHMDPawn->ActorReceivePayloadData.Reset();
					State = FFieldActorState();
					State.Instance = Instance;
					State.Actor = Endpoint;
				}
			}
		}
		ConfirmedPawnRevision = Revision;
		Peer.PawnInstance = Instance;
		return false;
	}
	if (Kind != Describe || !bPC || Revision < Peer.PawnRevision)
	{
		return false;
	}
	const bool bDetached = Reader.AtEnd();
	if (Revision != Peer.PawnRevision || (!bDetached && !Peer.PawnCounterpart.IsValid()))
	{
		// Destroyed/BeginPlay Blueprint events can stop a connection or replace a participant.
		const FString PeerId = Peer.Info.PeerId;
		const FGuid Session = Peer.RemoteSession;
		auto PairingStillCurrent = [this, &Peer, &PeerId, Session, Revision]()
		{
			const FHUICRSyncPeerRuntimeState* Current = FindPeer(PeerId);
			return Current && Current == &Peer && Current->RemoteSession == Session && Current->PawnRevision == Revision;
		};
		FString HMDClass, PCClass, Schema;
		TArray<uint8> Initial;
		if (!bDetached)
		{
			if (!SerializePath(Reader, HMDClass) || !SerializePath(Reader, PCClass) || !SerializePath(Reader, Schema) || HMDClass.IsEmpty())
			{
				return false;
			}
			Initial.SetNumUninitialized(static_cast<int32>(Reader.TotalSize() - Reader.Tell()));
			if (!Initial.IsEmpty()) { Reader.Serialize(Initial.GetData(), Initial.Num()); }
		}
		const bool bNewRevision = Revision > Peer.PawnRevision;
		Peer.PawnRevision = Revision;
		RemovePCPawn(Peer);
		if (!PairingStillCurrent()) { return false; }
		if (!bDetached)
		{
			// An existing single-player map can supply its placed ID-zero avatar once, then its class for later joins.
			AHUICRSyncActor* Placed = FindSyncActor(EHUICRSyncActorType::HMDPawn, 0);
			if (IsValid(Placed))
			{
				LegacyPawnClass = Placed->GetClass();
			}
			UClass* Class = PCClass.IsEmpty() ? LegacyPawnClass.LoadSynchronous() : LoadSyncActorClass(PCClass);
			const AHUICRSyncActor* Defaults = Class ? Class->GetDefaultObject<AHUICRSyncActor>() : nullptr;
			if (!Defaults || Class->HasAnyClassFlags(CLASS_Abstract) || Defaults->TypeCode != EHUICRSyncActorType::HMDPawn ||
				GetPathNameSafe(Defaults->SharedPayloadStruct) != Schema || (!Defaults->RemotePawnCounterpartClass.IsNull() &&
				 Defaults->RemotePawnCounterpartClass.ToSoftObjectPath().ToString() != HMDClass))
			{
				UE_CLOG(bNewRevision, LogTemp, Warning,
					TEXT("HUICRSync could not pair HUI Pawn for participant %d. Check both counterpart classes, Type Code and shared struct (HMD=%s, PC=%s, schema=%s)."),
					Participant, *HMDClass, *PCClass, *Schema);
				return false;
			}
			AHUICRSyncActor* Actor = nullptr;
			if (IsValid(Placed) && Placed->GetClass() == Class && Initial.IsEmpty())
			{
				UnregisterSyncActor(Placed);
				Placed->SetSyncActorID(Participant);
				RegisterSyncActor(Placed);
				Actor = Placed;
			}
			else
			{
				if (IsValid(Placed))
				{
					DestroyLocalSyncActor(EHUICRSyncActorType::HMDPawn, 0);
					if (!PairingStillCurrent()) { return false; }
				}
				Actor = SpawnSyncActorInternal(Class, Participant, FTransform::Identity, Initial);
			}
			if (!PairingStillCurrent())
			{
				if (IsValid(Actor)) { Actor->Destroy(); }
				return false;
			}
			if (!IsValid(Actor))
			{
				return false;
			}
			Peer.PawnCounterpart = Actor;
			UE_LOG(LogTemp, Log, TEXT("HUICRSync paired HUI Pawn %s with participant %d"), *Actor->GetName(), Participant);
			if (Actor->SharedPayloadStruct)
			{
				FFieldActorState& State = EnsurePCFieldActor(*Actor);
				Peer.PawnInstance = State.Instance;
				for (TFieldIterator<FProperty> It(Actor->SharedPayloadStruct); It; ++It)
				{
					const FName Field = It->GetFName();
					if (HUICRSyncFields::IsSupportedField(**It) && !State.Fields.Contains(Field))
					{
						TArray<uint8> Value;
						Actor->GetInitialSyncField(Field, Value);
						SetFieldWriter(*Actor, Field, State.Fields.Add(Field), Participant, &Value);
					}
				}
			}
			else
			{
				Peer.PawnInstance = ++NextFieldVersion;
			}
			if (bCalibrationPaused) { PauseManagedActor(*Actor); }
		}
	}
	TArray<uint8> Reply;
	FMemoryWriter Writer(Reply);
	Writer << Peer.PawnInstance;
	SendPawnPacket(Peer, Confirm, Revision, Reply);
	return false;
}
