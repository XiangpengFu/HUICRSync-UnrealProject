#include "HUICRSyncSubsystem.h"

#include "HUICRSyncActor.h"
#include "HUICRSyncPayloadLibrary.h"
#include "Misc/Base64.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

namespace HUICRSyncScene
{
const TCHAR* Prefix = TEXT("/HUICRSync/Scene/");
const TCHAR* Frame = TEXT("/HUICRSync/Scene/Frame");
const TCHAR* Applied = TEXT("/HUICRSync/Scene/Applied");
const TCHAR* SpawnRequest = TEXT("/HUICRSync/Scene/Spawn");
const TCHAR* DestroyRequest = TEXT("/HUICRSync/Scene/Destroy");
const TCHAR* RequestResult = TEXT("/HUICRSync/Scene/RequestResult");
constexpr int32 MaxOperationBytes = 64 * 1024;
constexpr int32 MaxChunkBytes = 900;
constexpr int32 GenerationPayloadBytes = sizeof(int32);
constexpr int32 MaxSpawnRequestBytes = 48000;
constexpr double RetryInterval = 0.75;

FHUICRSyncCommandArg Integer(int32 Value)
{
	FHUICRSyncCommandArg Arg;
	Arg.Type = EHUICRSyncCommandArgType::Int32;
	Arg.IntValue = Value;
	return Arg;
}

FHUICRSyncCommandArg String(FString Value)
{
	FHUICRSyncCommandArg Arg;
	Arg.Type = EHUICRSyncCommandArgType::String;
	Arg.StringValue = MoveTemp(Value);
	return Arg;
}

bool IsManagedActorType(EHUICRSyncActorType Type)
{
	return Type == EHUICRSyncActorType::HUIActor;
}
}

FTransform UHUICRSyncSubsystem::ConvertHMDActorTransformToPC(const FTransform& Transform) const
{
	return Role == EHUICRSyncRole::HMD && GuestActorMapping.IsValid()
		? GuestActorMapping.ToPC(Transform) : ConvertHMDTransformToPC(Transform);
}

FTransform UHUICRSyncSubsystem::ConvertPCActorTransformToHMD(const FTransform& Transform) const
{
	return Role == EHUICRSyncRole::HMD && GuestActorMapping.IsValid()
		? GuestActorMapping.ToHMD(Transform) : ConvertPCTransformToHMD(Transform);
}

void UHUICRSyncSubsystem::SetLocalCalibrationMapping(const FTransform& ToHMD, const FTransform& ToPC,
	const FHUICRSyncGuestActorMapping& GuestMapping)
{
	const FTransform PreviousToPC = HMDToPCTransform;
	const FHUICRSyncGuestActorMapping PreviousGuestMapping = GuestActorMapping;
	const bool bActorsNeedRemapping = Role == EHUICRSyncRole::HMD &&
		(PreviousGuestMapping.IsValid() || GuestMapping.IsValid()) &&
		(!PreviousGuestMapping.ScaleRatio.Equals(GuestMapping.ScaleRatio) ||
			!PreviousGuestMapping.PCMain.Equals(GuestMapping.PCMain) || !PreviousGuestMapping.HMDMain.Equals(GuestMapping.HMDMain));
	PCToHMDTransform = ToHMD;
	HMDToPCTransform = ToPC;
	GuestActorMapping = GuestMapping;
	if (GuestActorMapping.IsValid())
	{
		// Existing effect Blueprints read this scale directly for scalar dimensions.
		PCToHMDTransform.SetScale3D(GuestActorMapping.ScaleRatio);
	}
	if (!bActorsNeedRemapping)
	{
		return;
	}
	for (const auto& Pair : LocalSceneMirrors)
	{
		if (AHUICRSyncActor* Actor = Pair.Value.Get())
		{
			const FTransform LocalTransform = Actor->GetActorTransform();
			const FTransform PCTransform = PreviousGuestMapping.IsValid()
				? PreviousGuestMapping.ToPC(LocalTransform) : LocalTransform * PreviousToPC;
			Actor->SetActorTransform(ConvertPCActorTransformToHMD(PCTransform), false, nullptr, ETeleportType::TeleportPhysics);
		}
	}
}

void UHUICRSyncSubsystem::PrepareManagedScene()
{
	bManagedScenePrepared = true;
	for (const auto& Pair : SpawnedSyncActors)
	{
		AHUICRSyncActor* Actor = Pair.Value.Get();
		if (!IsValid(Actor) || !Actor->bParticipateInSyncAtStart)
		{
			continue;
		}
		if (FManagedSceneActor* Existing = ManagedSceneActors.Find(Pair.Key))
		{
			if (!Existing->bSpawnedAtRuntime)
			{
				Existing->InitialPayload = Actor->ActorInitPayloadData;
			}
		}
		else
		{
			RegisterManagedSceneMirror(Actor, Actor->GetRemoteCounterpartClass(), Actor->ActorInitPayloadData);
		}
	}
}

bool UHUICRSyncSubsystem::RegisterManagedSceneMirror(AHUICRSyncActor* Actor, UClass* RemoteClass,
	const TArray<uint8>& InitialPayload, int32 ExcludedParticipantId)
{
	using namespace HUICRSyncScene;
	if (!IsValid(Actor) || !RemoteClass || !RemoteClass->IsChildOf(AHUICRSyncActor::StaticClass()) || Actor->ActorID < 0 ||
		!IsManagedActorType(Actor->TypeCode) ||
		RemoteClass->GetDefaultObject<AHUICRSyncActor>()->TypeCode != Actor->TypeCode)
	{
		return false;
	}

	const FString Key = MakeActorKey(Actor->TypeCode, Actor->ActorID);
	if (LocalOnlySceneActors.Contains(Key))
	{
		return false;
	}
	if (ManagedSceneActors.Contains(Key))
	{
		return true;
	}

	FManagedSceneActor& Record = ManagedSceneActors.Add(Key);
	Record.RemoteClass = RemoteClass;
	Record.InitialPayload = InitialPayload;
	Record.ExcludedParticipantId = ExcludedParticipantId;
	SeedInitialFields(*Actor);
	for (auto& Pair : Peers)
	{
		if (Pair.Value.SceneGeneration > 0 && !Pair.Value.bNeedsSceneSnapshot &&
			!CalibrationTurns.Contains(Pair.Value.Info.ParticipantId))
		{
			QueueManagedSceneMirror(Pair.Value, *Actor, Record);
		}
	}
	return true;
}

void UHUICRSyncSubsystem::QueueManagedSceneMirror(FHUICRSyncPeerRuntimeState& Peer, AHUICRSyncActor& Actor,
	const FManagedSceneActor& Record)
{
	if (Record.ExcludedParticipantId == Peer.Info.ParticipantId)
	{
		return;
	}

	UClass* RemoteClass = Record.RemoteClass.LoadSynchronous();
	if (!RemoteClass)
	{
		UE_LOG(LogTemp, Warning, TEXT("HUICRSync missing scene counterpart for %s"), *Actor.GetPathName());
		return;
	}

	TArray<uint8> MirrorBytes;
	UHUICRSyncPayloadLibrary::WriteSpawnMirrorPayload(MirrorBytes, RemoteClass, Actor.ActorID,
		Actor.GetActorTransform(), Record.InitialPayload);
	FHUICRSyncSceneOperation Operation;
	Operation.Kind = EHUICRSyncSceneOp::Spawn;
	FMemoryWriter Writer(Operation.Bytes);
	Writer << MirrorBytes;
	const FString Key = MakeActorKey(Actor.TypeCode, Actor.ActorID);
	const FManagedActorState* LatestState = LatestManagedActorStates.Find(Key);
	bool bHasState = LatestState != nullptr;
	Writer << bHasState;
	if (LatestState)
	{
		FManagedActorState State = *LatestState;
		Writer << State.Sequence << State.Entry;
	}
	SeedInitialFields(Actor);
	const FFieldActorState* FieldState = FieldActors.Find(Actor.ActorID);
	uint64 FieldInstance = FieldState ? FieldState->Instance : 0;
	FString SchemaPath = FieldState ? GetPathNameSafe(Actor.SharedPayloadStruct) : FString();
	Writer << FieldInstance << SchemaPath;
	if (Operation.Bytes.Num() <= HUICRSyncScene::MaxOperationBytes)
	{
		Peer.SceneOperations.Add(MoveTemp(Operation));
		if (FieldState)
		{
			QueueFieldSnapshot(Peer, *FieldState);
		}
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("HUICRSync scene spawn exceeds the payload limit for actor %d"), Actor.ActorID);
	}
}

void UHUICRSyncSubsystem::UnregisterManagedSceneMirror(AHUICRSyncActor& Actor)
{
	const int32 FieldKey = FieldActorKey(Actor);
	if (const FFieldActorState* State = FieldActors.Find(FieldKey); State && State->Actor.Get() == &Actor)
	{
		FieldActors.Remove(FieldKey);
	}
	const FString Key = MakeActorKey(Actor.TypeCode, Actor.ActorID);
	LocalOnlySceneActors.Remove(Key);
	LatestManagedActorStates.Remove(Key);
	FManagedSceneActor Record;
	if (!ManagedSceneActors.RemoveAndCopyValue(Key, Record))
	{
		return;
	}

	FHUICRSyncSceneOperation Operation;
	Operation.Kind = EHUICRSyncSceneOp::Destroy;
	FMemoryWriter Writer(Operation.Bytes);
	uint8 Type = static_cast<uint8>(Actor.TypeCode);
	int32 ActorId = Actor.ActorID;
	Writer << Type << ActorId;
	for (auto& Pair : Peers)
	{
		FHUICRSyncPeerRuntimeState& Peer = Pair.Value;
		if (Peer.SceneGeneration > 0 && !Peer.bNeedsSceneSnapshot && Peer.Info.ParticipantId != Record.ExcludedParticipantId &&
			!CalibrationTurns.Contains(Peer.Info.ParticipantId))
		{
			Peer.SceneOperations.Add(Operation);
		}
	}
}

void UHUICRSyncSubsystem::StartManagedSceneForPeer(FHUICRSyncPeerRuntimeState& Peer)
{
	DiscardManagedSceneTransfer(Peer);
	++Peer.SceneGeneration;
	// Capture the scene on Tick after calibration callbacks have updated the PC actors.
	Peer.bNeedsSceneSnapshot = true;
}

void UHUICRSyncSubsystem::QueueManagedSceneSnapshot(FHUICRSyncPeerRuntimeState& Peer)
{
	Peer.bNeedsSceneSnapshot = false;
	FHUICRSyncSceneOperation& Reset = Peer.SceneOperations.AddDefaulted_GetRef();
	FMemoryWriter Writer(Reset.Bytes);
	Writer << Peer.SceneGeneration;
	for (const auto& Pair : ManagedSceneActors)
	{
		if (AHUICRSyncActor* Actor = SpawnedSyncActors.FindRef(Pair.Key))
		{
			if (IsValid(Actor))
			{
				QueueManagedSceneMirror(Peer, *Actor, Pair.Value);
			}
		}
	}
}

void UHUICRSyncSubsystem::DiscardManagedSceneTransfer(FHUICRSyncPeerRuntimeState& Peer)
{
	Peer.ReadySceneGeneration = 0;
	Peer.SceneSequence = 1;
	Peer.SceneSendOffset = 0;
	Peer.bNeedsSceneSnapshot = false;
	Peer.NextSceneSendTime = 0;
	Peer.SceneOperations.Reset();
	for (auto It = Peer.PendingReliableCommands.CreateIterator(); It; ++It)
	{
		if (It.Value().Address == HUICRSyncScene::Frame)
		{
			It.RemoveCurrent();
		}
	}
}

void UHUICRSyncSubsystem::TickManagedScene()
{
	using namespace HUICRSyncScene;
	if (Role != EHUICRSyncRole::PC || !bManagedCalibration || !bManagedScenePrepared)
	{
		return;
	}

	const double Now = FPlatformTime::Seconds();
	for (auto& Pair : Peers)
	{
		FHUICRSyncPeerRuntimeState& Peer = Pair.Value;
		if (!Peer.Info.bConnected || !Peer.bHasCalibrated || CalibrationTurns.Contains(Peer.Info.ParticipantId))
		{
			continue;
		}
		if (Peer.SceneGeneration == 0)
		{
			StartManagedSceneForPeer(Peer);
		}
		if (Peer.bNeedsSceneSnapshot)
		{
			QueueManagedSceneSnapshot(Peer);
		}
		if (Peer.SceneOperations.IsEmpty() && Peer.ReadySceneGeneration != Peer.SceneGeneration)
		{
			FHUICRSyncSceneOperation& Ready = Peer.SceneOperations.AddDefaulted_GetRef();
			Ready.Kind = EHUICRSyncSceneOp::Ready;
			FMemoryWriter Writer(Ready.Bytes);
			Writer << Peer.SceneGeneration;
		}
		if (Peer.SceneOperations.IsEmpty() || Now < Peer.NextSceneSendTime)
		{
			continue;
		}

		const FHUICRSyncSceneOperation& Operation = Peer.SceneOperations[0];
		const int32 ChunkLimit = FMath::Clamp((MaxPacketBytes - 256) * 3 / 4, GenerationPayloadBytes, MaxChunkBytes);
		const int32 ChunkSize = FMath::Min(ChunkLimit, Operation.Bytes.Num() - Peer.SceneSendOffset);
		const FString Chunk = FBase64::Encode(Operation.Bytes.GetData() + Peer.SceneSendOffset, ChunkSize);
		Peer.NextSceneSendTime = Now + RetryInterval;
		SendReliableCommandToPeer(Pair.Key, Frame, { Integer(Peer.SceneGeneration), Integer(Peer.SceneSequence), Integer(static_cast<int32>(Operation.Kind)),
			Integer(Operation.Bytes.Num()), String(Chunk) });
	}
}

bool UHUICRSyncSubsystem::SendManagedSpawnRequest(const TArray<uint8>& Payload)
{
	using namespace HUICRSyncScene;
	const FString Encoded = FBase64::Encode(Payload);
	if (bSpawningManagedActor || !bLocalSceneReady || bGameStopped || Payload.IsEmpty() ||
		Payload.Num() > FMath::Min(MaxPacketBytes, MaxSpawnRequestBytes))
	{
		return false;
	}
	return SendReliableCommand(SpawnRequest, { Integer(++LastSceneActorRequestId), String(Encoded) });
}

bool UHUICRSyncSubsystem::SendManagedDestroyRequest(EHUICRSyncActorType TypeCode, int32 ActorId)
{
	using namespace HUICRSyncScene;
	return bLocalSceneReady && !bGameStopped && IsManagedActorType(TypeCode) &&
		SendReliableCommand(DestroyRequest, { Integer(++LastSceneActorRequestId), Integer(ActorId) });
}

bool UHUICRSyncSubsystem::HandleManagedSceneCommand(FHUICRSyncPeerRuntimeState& Peer, const FHUICRSyncCommandPayload& Command)
{
	using namespace HUICRSyncScene;
	if (!Command.Address.StartsWith(Prefix, ESearchCase::CaseSensitive))
	{
		return false;
	}
	if (!bManagedCalibration || !Peer.Info.bConnected)
	{
		return true;
	}

	const auto& Args = Command.Args;
	const auto IsInt = [&Args](int32 Index)
	{
		return Args.IsValidIndex(Index) && Args[Index].Type == EHUICRSyncCommandArgType::Int32;
	};
	const FString PeerId = Peer.Info.PeerId;
	if (Role == EHUICRSyncRole::PC)
	{
		if (Command.Address == Applied && IsInt(0) && IsInt(1) && IsInt(2) && !Peer.SceneOperations.IsEmpty() &&
			!CalibrationTurns.Contains(Peer.Info.ParticipantId) && Args[0].IntValue == Peer.SceneGeneration &&
			Args[1].IntValue == Peer.SceneSequence)
		{
			const FHUICRSyncSceneOperation& Sent = Peer.SceneOperations[0];
			const int32 Offset = Args[2].IntValue;
			const int32 SentSize = FMath::Min(FMath::Clamp((MaxPacketBytes - 256) * 3 / 4, GenerationPayloadBytes, MaxChunkBytes),
				Sent.Bytes.Num() - Peer.SceneSendOffset);
			if (Offset != Peer.SceneSendOffset + SentSize)
			{
				return true;
			}
			++Peer.SceneSequence;
			Peer.NextSceneSendTime = 0;
			Peer.SceneSendOffset = Offset;
			if (Offset == Sent.Bytes.Num())
			{
				int32 ReadyGeneration = 0;
				if (Sent.Kind == EHUICRSyncSceneOp::Ready)
				{
					FMemoryReader Reader(Sent.Bytes);
					Reader << ReadyGeneration;
				}
				Peer.SceneOperations.RemoveAt(0, 1, EAllowShrinking::No);
				Peer.SceneSendOffset = 0;
				if (ReadyGeneration == Peer.SceneGeneration && Peer.SceneOperations.IsEmpty())
				{
					Peer.ReadySceneGeneration = ReadyGeneration;
					UE_LOG(LogTemp, Log, TEXT("HUICRSync participant %d confirmed scene generation %d"),
						Peer.Info.ParticipantId, ReadyGeneration);
					if (!bInitialCalibrationSceneReady && Peer.Info.ParticipantId == HostParticipantId)
					{
						StartGameFromAuthority();
					}
					else
					{
						PublishCalibrationStatus();
					}
				}
			}
			return true;
		}

		const bool bSpawn = Command.Address == SpawnRequest;
		const bool bDestroy = Command.Address == DestroyRequest;
		if ((!bSpawn && !bDestroy) || !IsInt(0) || Args[0].IntValue <= 0)
		{
			return true;
		}
		const int32 RequestId = Args[0].IntValue;
		const int32 ParticipantId = Peer.Info.ParticipantId;
		int32 ActorId = -1;
		bool bSucceeded = false;
		const bool bCanRequest = Peer.bHasCalibrated && Peer.ReadySceneGeneration > 0 && !bGameStopped;
		if (bCanRequest && bSpawn && Args.Num() == 2 && Args[1].Type == EHUICRSyncCommandArgType::String)
		{
			TArray<uint8> Bytes;
			FString ClassPath;
			FTransform Transform;
			bool bSpawnBackOnRequester = true;
			TArray<uint8> InitialPayload;
			const int32 RequestLimit = FMath::Min(MaxPacketBytes, MaxSpawnRequestBytes);
			if (Args[1].StringValue.Len() <= 4 * ((RequestLimit + 2) / 3) &&
				FBase64::Decode(Args[1].StringValue, Bytes) &&
				Bytes.Num() <= RequestLimit &&
				UHUICRSyncPayloadLibrary::ReadSpawnRequestPayload(Bytes, ClassPath, Transform, bSpawnBackOnRequester, InitialPayload) &&
				Transform.IsValid())
			{
				if (UClass* SpawnClass = LoadSyncActorClass(ClassPath))
				{
					if (AHUICRSyncActor* Actor = SpawnSyncActorForPeers(SpawnClass, Transform, InitialPayload, true,
						bSpawnBackOnRequester ? 0 : ParticipantId))
					{
						ActorId = Actor->ActorID;
						bSucceeded = true;
					}
				}
			}
		}
		else if (bCanRequest && bDestroy && IsInt(1))
		{
			ActorId = Args[1].IntValue;
			const auto Type = EHUICRSyncActorType::HUIActor;
			const FManagedSceneActor* Actor = ManagedSceneActors.Find(MakeActorKey(Type, ActorId));
			if (Actor && Actor->ExcludedParticipantId != ParticipantId)
			{
				bSucceeded = DestroyLocalSyncActor(Type, ActorId);
			}
		}
		SendReliableCommandToPeer(PeerId, RequestResult,
			{ Integer(RequestId), Integer(bSpawn ? 1 : 0), Integer(bSucceeded ? 1 : 0), Integer(ActorId) });
		OnSceneActorRequestCompleted.Broadcast(ParticipantId, RequestId, bSpawn, bSucceeded, ActorId);
		return true;
	}
	if (Role == EHUICRSyncRole::HMD && Command.Address == RequestResult && IsInt(0) && IsInt(1) && IsInt(2) && IsInt(3))
	{
		OnSceneActorRequestCompleted.Broadcast(LocalParticipantId, Args[0].IntValue, Args[1].IntValue != 0,
			Args[2].IntValue != 0, Args[3].IntValue);
		return true;
	}

	if (Role != EHUICRSyncRole::HMD || Command.Address != Frame || bIsLocalHMDCalibrating || Args.Num() != 5 ||
		!IsInt(0) || !IsInt(1) || !IsInt(2) || !IsInt(3) || Args[4].Type != EHUICRSyncCommandArgType::String)
	{
		return true;
	}
	const int32 Generation = Args[0].IntValue;
	const int32 Sequence = Args[1].IntValue;
	const int32 Kind = Args[2].IntValue;
	const int32 TotalSize = Args[3].IntValue;
	if (Generation <= 0 || Generation < ExpectedSceneGeneration || Generation < LocalSceneGeneration || Sequence <= 0 ||
		Kind < 0 || Kind > static_cast<int32>(EHUICRSyncSceneOp::Field) ||
		TotalSize <= 0 || TotalSize > MaxOperationBytes || Args[4].StringValue.Len() > MaxChunkBytes * 2)
	{
		return true;
	}
	const auto Operation = static_cast<EHUICRSyncSceneOp>(Kind);
	if (Generation > LocalSceneGeneration)
	{
		TArray<uint8> Chunk;
		if (Operation != EHUICRSyncSceneOp::Reset || Sequence != 1 || TotalSize != GenerationPayloadBytes ||
			!FBase64::Decode(Args[4].StringValue, Chunk) || Chunk.Num() != TotalSize)
		{
			return true;
		}
		int32 ResetGeneration = 0;
		FMemoryReader Reader(Chunk);
		Reader << ResetGeneration;
		if (ResetGeneration == Generation && ApplyManagedSceneOperation(Operation, Chunk))
		{
			IncomingSceneOperation.Reset();
			IncomingSceneSequence = Sequence;
			LastAppliedSceneOffset = TotalSize;
			SendReliableCommandToPeer(PeerId, Applied, { Integer(Generation), Integer(Sequence), Integer(TotalSize) });
		}
		return true;
	}
	if (Sequence == IncomingSceneSequence)
	{
		SendReliableCommandToPeer(PeerId, Applied, { Integer(Generation), Integer(Sequence), Integer(LastAppliedSceneOffset) });
		return true;
	}
	if (Sequence != IncomingSceneSequence + 1)
	{
		return true;
	}

	TArray<uint8> Chunk;
	if (!FBase64::Decode(Args[4].StringValue, Chunk) || Chunk.IsEmpty() ||
		Chunk.Num() > MaxChunkBytes || IncomingSceneOperation.Num() + Chunk.Num() > TotalSize)
	{
		return true;
	}
	const int32 StartOffset = IncomingSceneOperation.Num();
	IncomingSceneOperation.Append(Chunk);
	const int32 EndOffset = IncomingSceneOperation.Num();
	if (EndOffset == TotalSize)
	{
		if (!ApplyManagedSceneOperation(Operation, IncomingSceneOperation))
		{
			IncomingSceneOperation.SetNum(StartOffset, EAllowShrinking::No);
			return true;
		}
		IncomingSceneOperation.Reset();
	}
	IncomingSceneSequence = Sequence;
	LastAppliedSceneOffset = EndOffset;
	SendReliableCommandToPeer(PeerId, Applied, { Integer(Generation), Integer(Sequence), Integer(EndOffset) });
	return true;
}

bool UHUICRSyncSubsystem::ApplyManagedSceneOperation(EHUICRSyncSceneOp Kind, const TArray<uint8>& Bytes)
{
	FMemoryReader Reader(Bytes);
	switch (Kind)
	{
	case EHUICRSyncSceneOp::Reset:
	{
		int32 Generation = 0;
		Reader << Generation;
		if (Reader.IsError() || !Reader.AtEnd() || Generation <= LocalSceneGeneration)
		{
			return false;
		}
		const bool bWasStopped = bGameStopped;
		bGameStopped = true;
		bLocalSceneReady = false;
		LocalSceneGeneration = Generation;
		SetCalibrationPaused(true);
		ClearLocalSceneMirrors();
		NotifyGameplayPauseIfChanged(bWasStopped);
		return true;
	}
	case EHUICRSyncSceneOp::Spawn:
	{
		FHUICRSyncNetEntry Mirror;
		Reader << Mirror.PayloadBytes;
		bool bHasState = false;
		Reader << bHasState;
		FHUICRSyncNetEntry State;
		uint64 SnapshotStateSequence = 0;
		if (bHasState)
		{
			Reader << SnapshotStateSequence << State;
		}
		uint64 FieldInstance = 0;
		FString SchemaPath;
		Reader << FieldInstance << SchemaPath;
		FString ClassPath;
		int32 Id = -1;
		FTransform Transform;
		TArray<uint8> InitialPayload;
		if (Reader.IsError() || !Reader.AtEnd() ||
			!UHUICRSyncPayloadLibrary::ReadSpawnMirrorPayload(Mirror.PayloadBytes, ClassPath, Id, Transform, InitialPayload) ||
			Id < 0 || !Transform.IsValid())
		{
			return false;
		}
		UClass* SpawnClass = LoadSyncActorClass(ClassPath);
		if (!SpawnClass)
		{
			SetCalibrationMessage(TEXT("Could not load a synchronized scene actor."));
			return false;
		}
		const auto Type = SpawnClass->GetDefaultObject<AHUICRSyncActor>()->TypeCode;
		if (!HUICRSyncScene::IsManagedActorType(Type))
		{
			return false;
		}
		const FString Key = MakeActorKey(Type, Id);
		AHUICRSyncActor* Existing = FindSyncActor(Type, Id);
		if (IsValid(Existing) && LocalSceneMirrors.FindRef(Key).Get() != Existing)
		{
			DestroyLocalSyncActor(Type, Id);
		}
		TGuardValue<bool> ApplyingAuthoritativeSpawn(bSpawningManagedActor, true);
		AHUICRSyncActor* Actor = SpawnSyncActorInternal(SpawnClass, Id, ConvertPCActorTransformToHMD(Transform), InitialPayload);
		if (!IsValid(Actor))
		{
			return false;
		}
		LocalSceneMirrors.Add(Key, Actor);
		if (FieldInstance != 0 && Actor->SharedPayloadStruct && SchemaPath == Actor->SharedPayloadStruct->GetPathName())
		{
			FFieldActorState& FieldActor = FieldActors.Add(Id);
			FieldActor.Instance = FieldInstance;
			FieldActor.Actor = Actor;
		}
		else if (FieldInstance != 0 || Actor->SharedPayloadStruct)
		{
			UE_LOG(LogTemp, Warning, TEXT("HUICRSync shared payload type differs for %s (PC %s, HMD %s)"),
				*Actor->GetPathName(), *SchemaPath, *GetPathNameSafe(Actor->SharedPayloadStruct));
		}
		if (bCalibrationPaused)
		{
			PauseManagedActor(*Actor);
		}
		RemovedSceneActorIds.Remove(Id);
		FManagedActorState& Latest = ReceivedManagedActorStates.FindOrAdd(Id);
		if (bHasState && SnapshotStateSequence > Latest.Sequence && State.TypeCode == Type && State.ActorID == Id)
		{
			Latest = { MoveTemp(State), SnapshotStateSequence };
		}
		if (Latest.Sequence > 0)
		{
			ReceivedDataMap.FindOrAdd(Type).InnerMap.Add(Id, Latest.Entry);
			Actor->DeserializePayload();
		}
		Mirror.TypeCode = EHUICRSyncActorType::HUISpawnInGame;
		Mirror.ActorID = Id;
		SpawnActorPackets.Add(MoveTemp(Mirror));
		OnSpawnDataReceived.Broadcast();
		return true;
	}
	case EHUICRSyncSceneOp::Destroy:
	{
		uint8 TypeValue = 0;
		int32 Id = -1;
		Reader << TypeValue << Id;
		const auto Type = static_cast<EHUICRSyncActorType>(TypeValue);
		if (Reader.IsError() || !Reader.AtEnd() || Id < 0 || !HUICRSyncScene::IsManagedActorType(Type))
		{
			return false;
		}
		if (LocalSceneMirrors.Remove(MakeActorKey(Type, Id)))
		{
			DestroyLocalSyncActor(Type, Id);
		}
		if (auto* States = ReceivedDataMap.Find(Type))
		{
			States->InnerMap.Remove(Id);
		}
		RemovedSceneActorIds.Add(Id);
		ReceivedManagedActorStates.Remove(Id);
		FieldActors.Remove(Id);
		return true;
	}
	case EHUICRSyncSceneOp::Field:
		// A mismatched/incompatible Blueprint field cannot block the rest of the scene from spawning.
		ReceiveFieldState(Reader, true, nullptr);
		return true;
	case EHUICRSyncSceneOp::Ready:
	{
		int32 Generation = 0;
		Reader << Generation;
		return !Reader.IsError() && Reader.AtEnd() && Generation == LocalSceneGeneration && Generation > 0;
	}
	}
	return false;
}

void UHUICRSyncSubsystem::ClearLocalSceneMirrors()
{
	if (Role == EHUICRSyncRole::HMD)
	{
		// Pawn pairing belongs to the connection, not to a particular calibration or scene snapshot.
		for (auto It = FieldActors.CreateIterator(); It; ++It)
		{
			if (It.Key() >= 0) { It.RemoveCurrent(); }
		}
	}
	auto Mirrors = MoveTemp(LocalSceneMirrors);
	LocalSceneMirrors.Reset();
	for (const auto& Pair : Mirrors)
	{
		if (AHUICRSyncActor* Mirror = Pair.Value.Get())
		{
			DestroyLocalSyncActor(Mirror->TypeCode, Mirror->ActorID);
		}
	}
}

void UHUICRSyncSubsystem::ResetManagedSceneConnection()
{
	if (Role == EHUICRSyncRole::HMD)
	{
		ClearLocalSceneMirrors();
	}
	bLocalSceneReady = false;
	IncomingSceneOperation.Reset();
	ReceivedManagedActorStates.Reset();
	RemovedSceneActorIds.Reset();
	IncomingSceneSequence = LastAppliedSceneOffset = LocalSceneGeneration = ExpectedSceneGeneration = NotifiedLocalSceneGeneration = 0;
	LastSceneActorRequestId = 0;
}
