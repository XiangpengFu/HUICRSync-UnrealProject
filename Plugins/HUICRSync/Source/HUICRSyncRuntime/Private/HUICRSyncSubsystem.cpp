#include "HUICRSyncSubsystem.h"

#include "Common/UdpSocketBuilder.h"
#include "Engine/World.h"
#include "HUICRSyncActor.h"
#include "HUICRSyncPayloadLibrary.h"
#include "IPAddress.h"
#include "Kismet/GameplayStatics.h"
#include "Networking.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Stats/Stats.h"

namespace HUICRSyncProtocol
{
	static constexpr uint32 RealtimeMagic = 0x48554952; // HUIR
	static constexpr uint32 SpawnMagic = 0x48554950; // HUIP
	static constexpr uint32 CommandMagic = 0x48554943; // HUIC
	static constexpr uint32 CommandAckMagic = 0x48554941; // HUIA
	static constexpr uint32 PCCommandIdPrefix = 0x10000000;
	static constexpr uint32 HMDCommandIdPrefix = 0x20000000;
	static constexpr uint32 UnknownCommandIdPrefix = 0x30000000;
}

UHUICRSyncSubsystem::UHUICRSyncSubsystem()
{
	HMDToPCTransform = FTransform::Identity;
	PCToHMDTransform = FTransform::Identity;
}

void UHUICRSyncSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	LocalIpAddress = ResolveLocalIpAddress();
}

void UHUICRSyncSubsystem::Deinitialize()
{
	StopSync();
	Super::Deinitialize();
}

void UHUICRSyncSubsystem::Tick(float DeltaTime)
{
	ReceiveData();

	if (!bGameStopped)
	{
		OnPrepareNetDataSend.Broadcast();
		SendAllQueuedStates();
	}

	TickReliableCommands();
}

TStatId UHUICRSyncSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UHUICRSyncSubsystem, STATGROUP_Tickables);
}

bool UHUICRSyncSubsystem::IsTickable() const
{
	return !HasAnyFlags(RF_ClassDefaultObject);
}

bool UHUICRSyncSubsystem::ConfigureSync(EHUICRSyncRole InRole, int32 InLocalSyncPort, int32 InRemoteSyncPort, const FString& InRemoteIpAddress)
{
	Role = InRole;
	LocalSyncPort = InLocalSyncPort;
	RemoteSyncPort = InRemoteSyncPort;
	RemoteIpAddress = InRemoteIpAddress;
	UDPReceiverPort = LocalSyncPort + 1;
	InitialReliableCommandSendCount = Role == EHUICRSyncRole::HMD ? 3 : 1;
	LocalIpAddress = ResolveLocalIpAddress();

	const bool bReceiverReady = InitializeReceiverSocket();
	bool bSenderReady = true;

	if (!RemoteIpAddress.IsEmpty())
	{
		bSenderReady = InitializeSenderSocket(RemoteIpAddress, RemoteSyncPort);
	}

	if (!bReceiverReady)
	{
		UE_LOG(LogTemp, Warning, TEXT("HUICRSync failed to initialize UDP receiver."));
	}

	return bReceiverReady && bSenderReady;
}

bool UHUICRSyncSubsystem::InitializeReceiverSocket()
{
	if (ReceiverSocket)
	{
		ReceiverSocket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ReceiverSocket);
		ReceiverSocket = nullptr;
	}

	LocalIpAddress = ResolveLocalIpAddress();
	FIPv4Address ReceiverAddress;
	if (!FIPv4Address::Parse(LocalIpAddress, ReceiverAddress))
	{
		return false;
	}

	UDPReceiverPort = LocalSyncPort + 1;
	const FIPv4Endpoint ListenEndpoint(ReceiverAddress, UDPReceiverPort);

	ReceiverSocket = FUdpSocketBuilder(TEXT("HUICRSync_UDP_Receiver"))
		.AsNonBlocking()
		.AsReusable()
		.BoundToEndpoint(ListenEndpoint)
		.WithReceiveBufferSize(2 * 1024 * 1024);

	return ReceiverSocket != nullptr;
}

bool UHUICRSyncSubsystem::InitializeSenderSocket(const FString& InRemoteIpAddress, int32 InRemoteSyncPort)
{
	if (!SenderSocket)
	{
		SenderSocket = FUdpSocketBuilder(TEXT("HUICRSync_UDP_Sender"))
			.AsReusable()
			.WithBroadcast()
			.WithSendBufferSize(2 * 1024 * 1024);
	}

	if (!SenderSocket)
	{
		return false;
	}

	RemoteIpAddress = InRemoteIpAddress;
	RemoteSyncPort = InRemoteSyncPort;

	const FString PeerId = RegisterOrUpdatePeer(DefaultPeerId, Role == EHUICRSyncRole::PC ? EHUICRSyncRole::HMD : EHUICRSyncRole::PC, RemoteIpAddress, RemoteSyncPort);
	FHUICRSyncPeerRuntimeState* Peer = FindPeer(PeerId);
	return Peer && Peer->UDPAddr.IsValid();
}

void UHUICRSyncSubsystem::ReceiveData()
{
	if (!ReceiverSocket)
	{
		return;
	}

	ReceivedDataMap.Empty();
	SpawnActorPackets.Empty();

	bool bReceivedAnyData = false;
	bool bReceivedSpawnData = false;
	bool bReceivedLastSpawnPacket = false;

	TArray<uint8> Data;
	uint32 Size = 0;

	while (ReceiverSocket->HasPendingData(Size))
	{
		Data.SetNumUninitialized(static_cast<int32>(FMath::Min(Size, 65507u)));

		int32 BytesRead = 0;
		FIPv4Endpoint Sender;
		const bool bReceived = ReceiverSocket->RecvFrom(Data.GetData(), Data.Num(), BytesRead, Sender.ToInternetAddr().Get());

		if (!bReceived || BytesRead <= 0)
		{
			continue;
		}

		Data.SetNum(BytesRead);
		FMemoryReader Reader(Data, true);

		uint32 Magic = 0;
		Reader << Magic;

		if (Magic == HUICRSyncProtocol::CommandAckMagic)
		{
			uint32 AckCommandId = 0;
			Reader << AckCommandId;
			if (FHUICRSyncPeerRuntimeState* Peer = FindPeerByEndpoint(Sender))
			{
				Peer->PendingReliableCommands.Remove(AckCommandId);
				Peer->Info.LastReceiveTime = FPlatformTime::Seconds();
				RefreshPeerInfoSnapshot();
			}
			continue;
		}

		if (Magic == HUICRSyncProtocol::CommandMagic)
		{
			uint32 CommandId = 0;
			FHUICRSyncCommandPayload CommandPayload;

			Reader << CommandId;
			Reader << CommandPayload;

			if (Reader.IsError())
			{
				UE_LOG(LogTemp, Error, TEXT("HUICRSync command deserialize error."));
				continue;
			}

			bReceivedAnyData = true;
			FHUICRSyncPeerRuntimeState* Peer = FindPeerByEndpoint(Sender);
			if (!Peer)
			{
				const FString SenderIp = Sender.Address.ToString();
				const int32 SenderSyncPort = FMath::Max(0, Sender.Port - 1);
				const FString PeerId = RegisterOrUpdatePeer(TEXT(""), EHUICRSyncRole::None, SenderIp, SenderSyncPort);
				Peer = FindPeer(PeerId);
			}

			if (!Peer)
			{
				continue;
			}

			Peer->Info.LastReceiveTime = FPlatformTime::Seconds();
			SendCommandAck(*Peer, CommandId);

			if (Peer->ReceivedCommandIds.Contains(CommandId))
			{
				continue;
			}

			Peer->ReceivedCommandIds.Add(CommandId);
			RefreshPeerInfoSnapshot();
			HandleCommandPayload(*Peer, CommandPayload);
			OnCommandReceived.Broadcast(CommandPayload.Address, CommandPayload.Args);
			continue;
		}

		int32 NumEntries = 0;
		if (Magic == HUICRSyncProtocol::RealtimeMagic)
		{
			Reader << NumEntries;
		}
		else if (Magic == HUICRSyncProtocol::SpawnMagic)
		{
			uint32 BatchId = 0;
			int32 PacketIndex = 0;
			bool bLastPacket = false;
			Reader << BatchId;
			Reader << PacketIndex;
			Reader << bLastPacket;
			Reader << NumEntries;
			bReceivedLastSpawnPacket |= bLastPacket;
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("HUICRSync invalid UDP magic: %u"), Magic);
			continue;
		}

		for (int32 EntryIndex = 0; EntryIndex < NumEntries; ++EntryIndex)
		{
			FHUICRSyncNetEntry ReceivedEntry;
			Reader << ReceivedEntry;

			if (Reader.IsError())
			{
				UE_LOG(LogTemp, Error, TEXT("HUICRSync net entry deserialize error."));
				break;
			}

			bReceivedAnyData = true;

			if (ReceivedEntry.TypeCode == EHUICRSyncActorType::HUISpawnInGame)
			{
				SpawnActorPackets.Add(ReceivedEntry);
				bReceivedSpawnData = true;
				continue;
			}

			ReceivedDataMap.FindOrAdd(ReceivedEntry.TypeCode).InnerMap.Add(ReceivedEntry.ActorID, ReceivedEntry);
		}
	}

	if (bReceivedAnyData)
	{
		OnNetDataReceived.Broadcast();
	}

	if (bReceivedSpawnData)
	{
		ProcessSpawnActorPackets();
		OnSpawnDataReceived.Broadcast();

		if (Role == EHUICRSyncRole::HMD && bGameStopped && bReceivedLastSpawnPacket)
		{
			SendReliableCommand(TEXT("/FromHMD/SyncSpawnReady"), {});
		}
	}
}

void UHUICRSyncSubsystem::QueueNetEntry(const FHUICRSyncNetEntry& Entry)
{
	RegisteredStates.Add(Entry);
}

void UHUICRSyncSubsystem::QueueScreenNetEntry(const FHUICRSyncNetEntry& Entry)
{
	ScreenRegisteredStates.Add(Entry);
}

void UHUICRSyncSubsystem::SendAllQueuedStates()
{
	FHUICRSyncPeerRuntimeState* Peer = FindDefaultPeer();
	if (!SenderSocket || !Peer || !Peer->UDPAddr.IsValid())
	{
		return;
	}

	TArray<FHUICRSyncNetEntry> RealtimeEntries;
	TArray<FHUICRSyncNetEntry> SpawnEntries;

	auto SplitEntry = [&RealtimeEntries, &SpawnEntries](const FHUICRSyncNetEntry& Entry)
	{
		if (Entry.TypeCode == EHUICRSyncActorType::HUISpawnInGame)
		{
			SpawnEntries.Add(Entry);
		}
		else
		{
			RealtimeEntries.Add(Entry);
		}
	};

	for (const FHUICRSyncNetEntry& Entry : RegisteredStates)
	{
		SplitEntry(Entry);
	}

	for (const FHUICRSyncNetEntry& Entry : ScreenRegisteredStates)
	{
		SplitEntry(Entry);
	}

	RegisteredStates.Empty();
	ScreenRegisteredStates.Empty();

	SendQueuedEntries(*Peer, RealtimeEntries, HUICRSyncProtocol::RealtimeMagic);

	if (!SpawnEntries.IsEmpty())
	{
		PendingSpawnPackets.Append(SpawnEntries);
	}

	int32 PacketsSentThisTick = 0;
	while (PendingSpawnSendIndex < PendingSpawnPackets.Num() && PacketsSentThisTick < MaxSpawnPacketsPerTick)
	{
		TArray<uint8> Buffer;
		FMemoryWriter Writer(Buffer, true);

		uint32 Magic = HUICRSyncProtocol::SpawnMagic;
		uint32 BatchId = CurrentSpawnBatchId;
		int32 PacketIndex = CurrentSpawnPacketIndex;
		bool bLastPacket = false;
		int32 NumEntries = 0;

		Writer << Magic;
		Writer << BatchId;
		Writer << PacketIndex;

		const int64 LastPacketPos = Writer.Tell();
		Writer << bLastPacket;

		const int64 NumEntriesPos = Writer.Tell();
		Writer << NumEntries;

		int32 TestIndex = PendingSpawnSendIndex;
		while (TestIndex < PendingSpawnPackets.Num())
		{
			TArray<uint8> EntryBytes;
			FMemoryWriter EntryWriter(EntryBytes, true);
			FHUICRSyncNetEntry Entry = PendingSpawnPackets[TestIndex];
			EntryWriter << Entry;

			if (EntryBytes.Num() > MaxPacketBytes)
			{
				UE_LOG(LogTemp, Error, TEXT("HUICRSync spawn entry too large. Size=%d ActorID=%d Address=%s"), EntryBytes.Num(), Entry.ActorID, *Entry.Address);
				TestIndex++;
				PendingSpawnSendIndex++;
				continue;
			}

			if (Buffer.Num() + EntryBytes.Num() > MaxPacketBytes)
			{
				break;
			}

			Writer.Serialize(EntryBytes.GetData(), EntryBytes.Num());
			NumEntries++;
			TestIndex++;
		}

		if (NumEntries == 0)
		{
			break;
		}

		bLastPacket = TestIndex >= PendingSpawnPackets.Num();
		Writer.Seek(LastPacketPos);
		Writer << bLastPacket;
		Writer.Seek(NumEntriesPos);
		Writer << NumEntries;

		int32 BytesSent = 0;
		const bool bSent = SenderSocket->SendTo(Buffer.GetData(), Buffer.Num(), BytesSent, *Peer->UDPAddr);
		if (!bSent)
		{
			const ESocketErrors Err = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->GetLastErrorCode();
			UE_LOG(LogTemp, Error, TEXT("HUICRSync spawn send failed. Buffer=%d Sent=%d Err=%d"), Buffer.Num(), BytesSent, static_cast<int32>(Err));
			break;
		}

		PendingSpawnSendIndex = TestIndex;
		CurrentSpawnPacketIndex++;
		PacketsSentThisTick++;
	}

	if (PendingSpawnSendIndex >= PendingSpawnPackets.Num())
	{
		PendingSpawnPackets.Reset();
		PendingSpawnSendIndex = 0;
		CurrentSpawnPacketIndex = 0;
		CurrentSpawnBatchId++;
	}
}

bool UHUICRSyncSubsystem::QueueSpawnRequest(UClass* SpawnClass, const FTransform& SpawnTransform, bool bSpawnBackOnSender)
{
	UClass* LocalSpawnClass = nullptr;
	UClass* RemoteSpawnClass = nullptr;
	if (!ResolveSpawnClasses(SpawnClass, LocalSpawnClass, RemoteSpawnClass))
	{
		return false;
	}

	if (!RemoteSpawnClass)
	{
		return false;
	}

	FHUICRSyncNetEntry Entry;
	Entry.TypeCode = EHUICRSyncActorType::HUISpawnInGame;
	Entry.ActorID = -1;
	UHUICRSyncPayloadLibrary::WriteSpawnRequestPayload(Entry.PayloadBytes, RemoteSpawnClass, SpawnTransform, bSpawnBackOnSender);
	RegisteredStates.Add(Entry);
	return !Entry.PayloadBytes.IsEmpty();
}

bool UHUICRSyncSubsystem::QueueSpawnMirror(UClass* SpawnClass, int32 ActorID, const FTransform& SpawnTransform, const TArray<uint8>& InitPayloadBytes)
{
	if (!SpawnClass || !SpawnClass->IsChildOf(AHUICRSyncActor::StaticClass()))
	{
		return false;
	}

	FHUICRSyncNetEntry Entry;
	Entry.TypeCode = EHUICRSyncActorType::HUISpawnInGame;
	Entry.ActorID = ActorID;
	UHUICRSyncPayloadLibrary::WriteSpawnMirrorPayload(Entry.PayloadBytes, SpawnClass, ActorID, SpawnTransform, InitPayloadBytes);
	RegisteredStates.Add(Entry);
	return !Entry.PayloadBytes.IsEmpty();
}

bool UHUICRSyncSubsystem::RequestSpawnSyncActor(UClass* SpawnClass, const FTransform& SpawnTransform, const TArray<uint8>& InitPayloadBytes, bool bSpawnBackOnRequester)
{
	UClass* LocalSpawnClass = nullptr;
	UClass* RemoteSpawnClass = nullptr;
	if (!ResolveSpawnClasses(SpawnClass, LocalSpawnClass, RemoteSpawnClass))
	{
		return false;
	}

	if (!RemoteSpawnClass)
	{
		return false;
	}

	FHUICRSyncNetEntry Entry;
	Entry.TypeCode = EHUICRSyncActorType::HUISpawnInGame;
	Entry.ActorID = -1;
	UHUICRSyncPayloadLibrary::WriteSpawnRequestPayload(Entry.PayloadBytes, RemoteSpawnClass, SpawnTransform, bSpawnBackOnRequester, InitPayloadBytes);
	RegisteredStates.Add(Entry);
	return !Entry.PayloadBytes.IsEmpty();
}

AHUICRSyncActor* UHUICRSyncSubsystem::SpawnSyncActorAsAuthority(UClass* SpawnClass, const FTransform& SpawnTransform, const TArray<uint8>& InitPayloadBytes, bool bSpawnOnPeer)
{
	if (Role != EHUICRSyncRole::PC)
	{
		return nullptr;
	}

	UClass* LocalSpawnClass = nullptr;
	UClass* RemoteSpawnClass = nullptr;
	if (!ResolveSpawnClasses(SpawnClass, LocalSpawnClass, RemoteSpawnClass))
	{
		return nullptr;
	}

	if (!LocalSpawnClass || !LocalSpawnClass->IsChildOf(AHUICRSyncActor::StaticClass()))
	{
		return nullptr;
	}

	const AHUICRSyncActor* DefaultSyncActor = LocalSpawnClass->GetDefaultObject<AHUICRSyncActor>();
	const EHUICRSyncActorType ActorType = DefaultSyncActor ? DefaultSyncActor->TypeCode : EHUICRSyncActorType::HUIActor;
	const int32 NewActorID = AllocateActorID(ActorType);
	AHUICRSyncActor* NewActor = SpawnSyncActorInternal(LocalSpawnClass, NewActorID, SpawnTransform, InitPayloadBytes);
	if (!IsValid(NewActor))
	{
		return nullptr;
	}

	if (bSpawnOnPeer)
	{
		if (!RemoteSpawnClass)
		{
			return NewActor;
		}

		const TArray<uint8>& MirrorInitPayloadBytes = InitPayloadBytes.Num() > 0 ? InitPayloadBytes : NewActor->ActorInitPayloadData;
		FHUICRSyncNetEntry Entry;
		Entry.TypeCode = EHUICRSyncActorType::HUISpawnInGame;
		Entry.ActorID = NewActorID;
		UHUICRSyncPayloadLibrary::WriteSpawnMirrorPayload(Entry.PayloadBytes, RemoteSpawnClass, NewActorID, ConvertPCTransformToHMD(NewActor->GetActorTransform()), MirrorInitPayloadBytes);
		RegisteredStates.Add(Entry);
	}

	return NewActor;
}

bool UHUICRSyncSubsystem::SpawnSyncActor(UClass* SpawnClass, const FTransform& SpawnTransform, const TArray<uint8>& InitPayloadBytes)
{
	if (Role == EHUICRSyncRole::PC)
	{
		return IsValid(SpawnSyncActorAsAuthority(SpawnClass, SpawnTransform, InitPayloadBytes, true));
	}

	if (Role == EHUICRSyncRole::HMD)
	{
		return RequestSpawnSyncActor(SpawnClass, SpawnTransform, InitPayloadBytes, true);
	}

	return false;
}

bool UHUICRSyncSubsystem::SpawnSyncActorSimple(UClass* SpawnClass, const FTransform& SpawnTransform)
{
	return SpawnSyncActor(SpawnClass, SpawnTransform, {});
}

int32 UHUICRSyncSubsystem::StartInitialSpawnSync()
{
	if (Role != EHUICRSyncRole::PC)
	{
		return 0;
	}

	bWaitingForInitialSpawnSync = true;
	SendReliableCommand(TEXT("/FromPC/SyncSpawnBegin"), {});

	int32 QueuedCount = 0;
	for (const TPair<FString, TObjectPtr<AHUICRSyncActor>>& Pair : SpawnedSyncActors)
	{
		AHUICRSyncActor* SyncActor = Pair.Value.Get();
		if (!IsValid(SyncActor) || !SyncActor->bParticipateInSyncAtStart || SyncActor->ActorID < 0)
		{
			continue;
		}

		const bool bQueuedMirror = QueueSpawnMirror(
			SyncActor->GetRemoteCounterpartClass(),
			SyncActor->ActorID,
			ConvertPCTransformToHMD(SyncActor->GetActorTransform()),
			SyncActor->ActorInitPayloadData);
		if (bQueuedMirror)
		{
			QueuedCount++;
		}
	}

	if (QueuedCount > 0)
	{
		int32 FlushGuard = 0;
		do
		{
			SendAllQueuedStates();
			FlushGuard++;
		}
		while (!PendingSpawnPackets.IsEmpty() && FlushGuard < 1024);
	}
	else
	{
		CompleteSyncSceneAsAuthority();
	}

	return QueuedCount;
}

bool UHUICRSyncSubsystem::RegisterSyncActor(AHUICRSyncActor* SyncActor)
{
	if (!IsValid(SyncActor) || SyncActor->ActorID < 0)
	{
		return false;
	}

	SpawnedSyncActors.Add(MakeActorKey(SyncActor->TypeCode, SyncActor->ActorID), SyncActor);
	int32& Counter = ActorIdCounters.FindOrAdd(SyncActor->TypeCode);
	Counter = FMath::Max(Counter, SyncActor->ActorID + 1);
	return true;
}

void UHUICRSyncSubsystem::UnregisterSyncActor(AHUICRSyncActor* SyncActor)
{
	if (!IsValid(SyncActor) || SyncActor->ActorID < 0)
	{
		return;
	}

	const FString ActorKey = MakeActorKey(SyncActor->TypeCode, SyncActor->ActorID);
	if (SpawnedSyncActors.FindRef(ActorKey) == SyncActor)
	{
		SpawnedSyncActors.Remove(ActorKey);
	}
}

AHUICRSyncActor* UHUICRSyncSubsystem::FindSyncActor(EHUICRSyncActorType TypeCode, int32 ActorID) const
{
	return SpawnedSyncActors.FindRef(MakeActorKey(TypeCode, ActorID));
}

bool UHUICRSyncSubsystem::DestroySyncActor(EHUICRSyncActorType TypeCode, int32 ActorID)
{
	if (ActorID < 0)
	{
		return false;
	}

	FHUICRSyncCommandArg TypeArg;
	TypeArg.Type = EHUICRSyncCommandArgType::Int32;
	TypeArg.IntValue = static_cast<int32>(TypeCode);

	FHUICRSyncCommandArg ActorIdArg;
	ActorIdArg.Type = EHUICRSyncCommandArgType::Int32;
	ActorIdArg.IntValue = ActorID;

	if (Role == EHUICRSyncRole::PC)
	{
		const bool bDestroyedLocalActor = DestroyLocalSyncActor(TypeCode, ActorID);
		const bool bSentDestroyCommand = SendReliableCommand(TEXT("/FromPC/DestroySyncActor"), { TypeArg, ActorIdArg });
		return bDestroyedLocalActor || bSentDestroyCommand;
	}

	if (Role == EHUICRSyncRole::HMD)
	{
		return SendReliableCommand(TEXT("/FromHMD/DestroySyncActorRequest"), { TypeArg, ActorIdArg });
	}

	return false;
}

int32 UHUICRSyncSubsystem::ClearLocalSyncActors()
{
	TArray<TWeakObjectPtr<AHUICRSyncActor>> ActorsToDestroy;
	TSet<EHUICRSyncActorType> ClearedTypes;
	ActorsToDestroy.Reserve(SpawnedSyncActors.Num());

	for (const TPair<FString, TObjectPtr<AHUICRSyncActor>>& Pair : SpawnedSyncActors)
	{
		AHUICRSyncActor* SyncActor = Pair.Value.Get();
		if (IsValid(SyncActor))
		{
			ActorsToDestroy.Add(SyncActor);
			ClearedTypes.Add(SyncActor->TypeCode);
		}
	}

	SpawnedSyncActors.Reset();
	for (const EHUICRSyncActorType TypeCode : ClearedTypes)
	{
		ReceivedDataMap.Remove(TypeCode);
		ActorIdCounters.Remove(TypeCode);
	}

	RegisteredStates.RemoveAll([&ClearedTypes](const FHUICRSyncNetEntry& Entry)
	{
		return ClearedTypes.Contains(Entry.TypeCode) || Entry.TypeCode == EHUICRSyncActorType::HUISpawnInGame;
	});
	SpawnActorPackets.RemoveAll([&ClearedTypes](const FHUICRSyncNetEntry& Entry)
	{
		return ClearedTypes.Contains(Entry.TypeCode) || Entry.TypeCode == EHUICRSyncActorType::HUISpawnInGame;
	});
	PendingSpawnPackets.Reset();
	PendingSpawnSendIndex = 0;

	int32 DestroyedCount = 0;
	for (const TWeakObjectPtr<AHUICRSyncActor>& ActorPtr : ActorsToDestroy)
	{
		if (AHUICRSyncActor* SyncActor = ActorPtr.Get())
		{
			SyncActor->Destroy();
			DestroyedCount++;
		}
	}

	return DestroyedCount;
}

bool UHUICRSyncSubsystem::SendReliableCommand(const FString& Address, const TArray<FHUICRSyncCommandArg>& Args)
{
	return SendReliableCommandToPeer(DefaultPeerId, Address, Args);
}

bool UHUICRSyncSubsystem::SendReliableCommandBool(const FString& Address, bool Value)
{
	FHUICRSyncCommandArg Arg;
	Arg.Type = EHUICRSyncCommandArgType::Bool;
	Arg.BoolValue = Value;
	return SendReliableCommand(Address, { Arg });
}

bool UHUICRSyncSubsystem::SendReliableCommandInt(const FString& Address, int32 Value)
{
	FHUICRSyncCommandArg Arg;
	Arg.Type = EHUICRSyncCommandArgType::Int32;
	Arg.IntValue = Value;
	return SendReliableCommand(Address, { Arg });
}

bool UHUICRSyncSubsystem::SendReliableCommandFloat(const FString& Address, float Value)
{
	FHUICRSyncCommandArg Arg;
	Arg.Type = EHUICRSyncCommandArgType::Float;
	Arg.FloatValue = Value;
	return SendReliableCommand(Address, { Arg });
}

bool UHUICRSyncSubsystem::SendReliableCommandString(const FString& Address, const FString& Value)
{
	FHUICRSyncCommandArg Arg;
	Arg.Type = EHUICRSyncCommandArgType::String;
	Arg.StringValue = Value;
	return SendReliableCommand(Address, { Arg });
}

bool UHUICRSyncSubsystem::SendReliableCommandToPeer(const FString& PeerId, const FString& Address, const TArray<FHUICRSyncCommandArg>& Args)
{
	FHUICRSyncPeerRuntimeState* Peer = PeerId.IsEmpty() ? FindDefaultPeer() : FindPeer(PeerId);
	if (!SenderSocket || !Peer || !Peer->UDPAddr.IsValid())
	{
		return false;
	}

	uint32 LocalId = NextCommandId++;
	if (NextCommandId > 0x0FFFFFFF)
	{
		NextCommandId = 1;
	}

	const uint32 CommandId = GetCommandIdPrefix() | LocalId;

	FHUICRSyncPendingCommand Pending;
	Pending.CommandId = CommandId;
	Pending.Address = Address;
	Pending.Args = Args;
	Pending.LastSendTime = FPlatformTime::Seconds();
	Pending.RetryCount = 0;

	Peer->PendingReliableCommands.Add(CommandId, Pending);

	bool bAnySent = false;
	const int32 SendCount = FMath::Max(1, InitialReliableCommandSendCount);
	for (int32 Index = 0; Index < SendCount; ++Index)
	{
		bAnySent |= SendReliableCommandPacket(*Peer, CommandId, Address, Args);
	}

	return bAnySent;
}

FString UHUICRSyncSubsystem::RegisterOrUpdatePeer(const FString& PeerId, EHUICRSyncRole PeerRole, const FString& IpAddress, int32 PeerSyncPort)
{
	if (IpAddress.IsEmpty())
	{
		return FString();
	}

	const FString ResolvedPeerId = PeerId.IsEmpty() ? MakePeerId(IpAddress, PeerSyncPort) : PeerId;
	FHUICRSyncPeerRuntimeState& Peer = Peers.FindOrAdd(ResolvedPeerId);
	Peer.Info.PeerId = ResolvedPeerId;
	Peer.Info.Role = PeerRole;
	Peer.Info.IpAddress = IpAddress;
	Peer.Info.SyncPort = PeerSyncPort;
	Peer.Info.UDPPort = PeerSyncPort + 1;
	Peer.Info.bConnected = true;

	BuildPeerUdpAddress(Peer);

	if (DefaultPeerId.IsEmpty())
	{
		DefaultPeerId = ResolvedPeerId;
	}

	RefreshPeerInfoSnapshot();
	return ResolvedPeerId;
}

bool UHUICRSyncSubsystem::SetDefaultPeer(const FString& PeerId)
{
	if (!Peers.Contains(PeerId))
	{
		return false;
	}

	DefaultPeerId = PeerId;
	if (const FHUICRSyncPeerRuntimeState* Peer = FindPeer(PeerId))
	{
		RemoteIpAddress = Peer->Info.IpAddress;
		RemoteSyncPort = Peer->Info.SyncPort;
	}
	return true;
}

TArray<FString> UHUICRSyncSubsystem::GetKnownPeerIds() const
{
	TArray<FString> PeerIds;
	Peers.GetKeys(PeerIds);
	return PeerIds;
}

int32 UHUICRSyncSubsystem::AllocateActorID(EHUICRSyncActorType TypeCode)
{
	return ActorIdCounters.FindOrAdd(TypeCode)++;
}

void UHUICRSyncSubsystem::StopSync()
{
	DestroySockets();

	Peers.Empty();
	PeerInfos.Empty();
	DefaultPeerId.Empty();
	RegisteredStates.Empty();
	ScreenRegisteredStates.Empty();
	PendingSpawnPackets.Empty();
	ReceivedDataMap.Empty();
	SpawnActorPackets.Empty();
	SpawnedSyncActors.Empty();
	ActorIdCounters.Empty();
	bWaitingForInitialSpawnSync = false;
}

FTransform UHUICRSyncSubsystem::ConvertHMDTransformToPC(const FTransform& InHMDTransform) const
{
	return InHMDTransform * HMDToPCTransform;
}

FTransform UHUICRSyncSubsystem::ConvertPCTransformToHMD(const FTransform& InPCTransform) const
{
	return InPCTransform * PCToHMDTransform;
}

FString UHUICRSyncSubsystem::ResolveLocalIpAddress() const
{
	bool bCanBindAll = false;
	TSharedRef<FInternetAddr> LocalHostAddress = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->GetLocalHostAddr(*GLog, bCanBindAll);
	return LocalHostAddress->ToString(false);
}

FString UHUICRSyncSubsystem::MakeActorKey(EHUICRSyncActorType TypeCode, int32 ActorID) const
{
	return FString::Printf(TEXT("%d:%d"), static_cast<int32>(TypeCode), ActorID);
}

TSubclassOf<AHUICRSyncActor> UHUICRSyncSubsystem::GetRemoteCounterpartClass(UClass* LocalSyncActorClass) const
{
	if (!LocalSyncActorClass || !LocalSyncActorClass->IsChildOf(AHUICRSyncActor::StaticClass()))
	{
		return nullptr;
	}

	const AHUICRSyncActor* DefaultSyncActor = LocalSyncActorClass->GetDefaultObject<AHUICRSyncActor>();
	return DefaultSyncActor ? DefaultSyncActor->GetRemoteCounterpartClass() : nullptr;
}

FString UHUICRSyncSubsystem::MakePeerId(const FString& IpAddress, int32 PeerSyncPort) const
{
	return FString::Printf(TEXT("%s:%d"), *IpAddress, PeerSyncPort);
}

FHUICRSyncPeerRuntimeState* UHUICRSyncSubsystem::FindPeer(const FString& PeerId)
{
	return Peers.Find(PeerId);
}

const FHUICRSyncPeerRuntimeState* UHUICRSyncSubsystem::FindPeer(const FString& PeerId) const
{
	return Peers.Find(PeerId);
}

FHUICRSyncPeerRuntimeState* UHUICRSyncSubsystem::FindDefaultPeer()
{
	return DefaultPeerId.IsEmpty() ? nullptr : Peers.Find(DefaultPeerId);
}

FHUICRSyncPeerRuntimeState* UHUICRSyncSubsystem::FindPeerByEndpoint(const FIPv4Endpoint& Endpoint)
{
	const FString SenderIp = Endpoint.Address.ToString();
	const int32 SenderUDPPort = Endpoint.Port;

	for (TPair<FString, FHUICRSyncPeerRuntimeState>& Pair : Peers)
	{
		FHUICRSyncPeerRuntimeState& Peer = Pair.Value;
		if (Peer.Info.IpAddress == SenderIp && Peer.Info.UDPPort == SenderUDPPort)
		{
			return &Peer;
		}
	}

	for (TPair<FString, FHUICRSyncPeerRuntimeState>& Pair : Peers)
	{
		if (Pair.Value.Info.IpAddress == SenderIp)
		{
			return &Pair.Value;
		}
	}

	return FindDefaultPeer();
}

bool UHUICRSyncSubsystem::BuildPeerUdpAddress(FHUICRSyncPeerRuntimeState& Peer)
{
	Peer.UDPAddr = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();

	if (!Peer.UDPAddr.IsValid())
	{
		return false;
	}

	bool bIsValid = false;
	Peer.UDPAddr->SetIp(*Peer.Info.IpAddress, bIsValid);
	Peer.UDPAddr->SetPort(Peer.Info.UDPPort);
	return bIsValid;
}

void UHUICRSyncSubsystem::RefreshPeerInfoSnapshot()
{
	PeerInfos.Empty();
	for (const TPair<FString, FHUICRSyncPeerRuntimeState>& Pair : Peers)
	{
		PeerInfos.Add(Pair.Key, Pair.Value.Info);
	}
}

uint32 UHUICRSyncSubsystem::GetCommandIdPrefix() const
{
	if (Role == EHUICRSyncRole::PC)
	{
		return HUICRSyncProtocol::PCCommandIdPrefix;
	}

	if (Role == EHUICRSyncRole::HMD)
	{
		return HUICRSyncProtocol::HMDCommandIdPrefix;
	}

	return HUICRSyncProtocol::UnknownCommandIdPrefix;
}

bool UHUICRSyncSubsystem::SendReliableCommandPacket(FHUICRSyncPeerRuntimeState& Peer, uint32 CommandId, const FString& Address, const TArray<FHUICRSyncCommandArg>& Args)
{
	if (!SenderSocket || !Peer.UDPAddr.IsValid())
	{
		return false;
	}

	FHUICRSyncCommandPayload CommandPayload;
	CommandPayload.Address = Address;
	CommandPayload.Args = Args;

	TArray<uint8> Buffer;
	FMemoryWriter Writer(Buffer, true);
	uint32 Magic = HUICRSyncProtocol::CommandMagic;
	Writer << Magic;
	Writer << CommandId;
	Writer << CommandPayload;

	int32 BytesSent = 0;
	const bool bSent = SenderSocket->SendTo(Buffer.GetData(), Buffer.Num(), BytesSent, *Peer.UDPAddr);
	if (!bSent)
	{
		const ESocketErrors Err = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->GetLastErrorCode();
		UE_LOG(LogTemp, Error, TEXT("HUICRSync reliable command send failed. Peer=%s Id=%u Address=%s Bytes=%d Sent=%d Err=%d"), *Peer.Info.PeerId, CommandId, *Address, Buffer.Num(), BytesSent, static_cast<int32>(Err));
		return false;
	}

	return true;
}

void UHUICRSyncSubsystem::SendCommandAck(FHUICRSyncPeerRuntimeState& Peer, uint32 CommandId)
{
	if (!SenderSocket || !Peer.UDPAddr.IsValid())
	{
		return;
	}

	TArray<uint8> Buffer;
	FMemoryWriter Writer(Buffer, true);
	uint32 Magic = HUICRSyncProtocol::CommandAckMagic;
	Writer << Magic;
	Writer << CommandId;

	int32 BytesSent = 0;
	SenderSocket->SendTo(Buffer.GetData(), Buffer.Num(), BytesSent, *Peer.UDPAddr);
}

void UHUICRSyncSubsystem::TickReliableCommands()
{
	const double Now = FPlatformTime::Seconds();

	for (TPair<FString, FHUICRSyncPeerRuntimeState>& PeerPair : Peers)
	{
		FHUICRSyncPeerRuntimeState& Peer = PeerPair.Value;
		TArray<uint32> CommandsToRemove;

		for (TPair<uint32, FHUICRSyncPendingCommand>& CommandPair : Peer.PendingReliableCommands)
		{
			FHUICRSyncPendingCommand& Pending = CommandPair.Value;

			if (Now - Pending.LastSendTime < CommandAckTimeout)
			{
				continue;
			}

			if (Pending.RetryCount >= MaxCommandRetryCount)
			{
				UE_LOG(LogTemp, Error, TEXT("HUICRSync reliable command failed after retries. Peer=%s Id=%u Address=%s"), *Peer.Info.PeerId, Pending.CommandId, *Pending.Address);
				CommandsToRemove.Add(Pending.CommandId);
				continue;
			}

			Pending.RetryCount++;
			Pending.LastSendTime = Now;
			SendReliableCommandPacket(Peer, Pending.CommandId, Pending.Address, Pending.Args);
		}

		for (uint32 CommandId : CommandsToRemove)
		{
			Peer.PendingReliableCommands.Remove(CommandId);
		}
	}
}

void UHUICRSyncSubsystem::DestroySockets()
{
	if (ReceiverSocket)
	{
		ReceiverSocket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ReceiverSocket);
		ReceiverSocket = nullptr;
	}

	if (SenderSocket)
	{
		SenderSocket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(SenderSocket);
		SenderSocket = nullptr;
	}

	for (TPair<FString, FHUICRSyncPeerRuntimeState>& Pair : Peers)
	{
		Pair.Value.UDPAddr.Reset();
	}
}

bool UHUICRSyncSubsystem::SendQueuedEntries(FHUICRSyncPeerRuntimeState& Peer, const TArray<FHUICRSyncNetEntry>& Entries, uint32 Magic)
{
	if (Entries.IsEmpty() || !SenderSocket || !Peer.UDPAddr.IsValid())
	{
		return false;
	}

	bool bSentAny = false;
	int32 RealtimeIndex = 0;

	while (RealtimeIndex < Entries.Num())
	{
		TArray<uint8> Buffer;
		FMemoryWriter Writer(Buffer, true);
		int32 NumEntries = 0;

		Writer << Magic;
		const int64 NumEntriesPos = Writer.Tell();
		Writer << NumEntries;

		while (RealtimeIndex < Entries.Num())
		{
			TArray<uint8> EntryBytes;
			FMemoryWriter EntryWriter(EntryBytes, true);
			FHUICRSyncNetEntry Entry = Entries[RealtimeIndex];
			EntryWriter << Entry;

			if (EntryBytes.Num() > MaxPacketBytes)
			{
				UE_LOG(LogTemp, Error, TEXT("HUICRSync realtime entry too large. Size=%d Type=%d ActorID=%d Address=%s"), EntryBytes.Num(), static_cast<int32>(Entry.TypeCode), Entry.ActorID, *Entry.Address);
				RealtimeIndex++;
				continue;
			}

			if (Buffer.Num() + EntryBytes.Num() > MaxPacketBytes)
			{
				break;
			}

			Writer.Serialize(EntryBytes.GetData(), EntryBytes.Num());
			NumEntries++;
			RealtimeIndex++;
		}

		if (NumEntries == 0)
		{
			break;
		}

		Writer.Seek(NumEntriesPos);
		Writer << NumEntries;

		int32 BytesSent = 0;
		const bool bSent = SenderSocket->SendTo(Buffer.GetData(), Buffer.Num(), BytesSent, *Peer.UDPAddr);
		if (!bSent)
		{
			const ESocketErrors Err = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->GetLastErrorCode();
			UE_LOG(LogTemp, Error, TEXT("HUICRSync realtime send failed. Peer=%s Buffer=%d Sent=%d Err=%d"), *Peer.Info.PeerId, Buffer.Num(), BytesSent, static_cast<int32>(Err));
		}

		bSentAny |= bSent;
	}

	return bSentAny;
}

void UHUICRSyncSubsystem::ProcessSpawnActorPackets()
{
	for (const FHUICRSyncNetEntry& Entry : SpawnActorPackets)
	{
		if (Entry.ActorID < 0)
		{
			HandleSpawnRequest(Entry);
		}
		else
		{
			HandleSpawnMirror(Entry);
		}
	}
}

bool UHUICRSyncSubsystem::HandleSpawnRequest(const FHUICRSyncNetEntry& Entry)
{
	if (Role != EHUICRSyncRole::PC)
	{
		return false;
	}

	FString ClassPath;
	FTransform RequestedHMDTransform;
	bool bSpawnBackOnSender = true;
	TArray<uint8> InitPayloadBytes;
	if (!UHUICRSyncPayloadLibrary::ReadSpawnRequestPayload(Entry.PayloadBytes, ClassPath, RequestedHMDTransform, bSpawnBackOnSender, InitPayloadBytes))
	{
		return false;
	}

	UClass* SpawnClass = LoadSyncActorClass(ClassPath);
	if (!SpawnClass)
	{
		UE_LOG(LogTemp, Warning, TEXT("HUICRSync spawn request class could not be loaded. Class=%s"), *ClassPath);
		return false;
	}

	const FTransform PCTransform = ConvertHMDTransformToPC(RequestedHMDTransform);
	AHUICRSyncActor* SpawnedActor = SpawnSyncActorAsAuthority(SpawnClass, PCTransform, InitPayloadBytes, bSpawnBackOnSender);
	return IsValid(SpawnedActor);
}

bool UHUICRSyncSubsystem::HandleSpawnMirror(const FHUICRSyncNetEntry& Entry)
{
	if (Role == EHUICRSyncRole::PC)
	{
		return false;
	}

	FString ClassPath;
	int32 ActorID = -1;
	FTransform SpawnTransform;
	TArray<uint8> InitPayloadBytes;
	if (!UHUICRSyncPayloadLibrary::ReadSpawnMirrorPayload(Entry.PayloadBytes, ClassPath, ActorID, SpawnTransform, InitPayloadBytes))
	{
		return false;
	}

	UClass* SpawnClass = LoadSyncActorClass(ClassPath);
	if (!SpawnClass)
	{
		UE_LOG(LogTemp, Warning, TEXT("HUICRSync spawn mirror class could not be loaded. Class=%s"), *ClassPath);
		return false;
	}

	const AHUICRSyncActor* DefaultSyncActor = SpawnClass->GetDefaultObject<AHUICRSyncActor>();
	const EHUICRSyncActorType ActorType = DefaultSyncActor ? DefaultSyncActor->TypeCode : EHUICRSyncActorType::HUIActor;
	if (FindSyncActor(ActorType, ActorID))
	{
		return true;
	}

	return IsValid(SpawnSyncActorInternal(SpawnClass, ActorID, SpawnTransform, InitPayloadBytes));
}

bool UHUICRSyncSubsystem::DestroyLocalSyncActor(EHUICRSyncActorType TypeCode, int32 ActorID)
{
	AHUICRSyncActor* SyncActor = FindSyncActor(TypeCode, ActorID);
	if (!IsValid(SyncActor))
	{
		return false;
	}

	UnregisterSyncActor(SyncActor);
	SyncActor->Destroy();
	return true;
}

UClass* UHUICRSyncSubsystem::LoadSyncActorClass(const FString& ClassPath) const
{
	if (ClassPath.IsEmpty())
	{
		return nullptr;
	}

	UClass* LoadedClass = StaticLoadClass(AHUICRSyncActor::StaticClass(), nullptr, *ClassPath);
	if (!LoadedClass)
	{
		LoadedClass = LoadObject<UClass>(nullptr, *ClassPath);
	}

	if (!LoadedClass || !LoadedClass->IsChildOf(AHUICRSyncActor::StaticClass()))
	{
		return nullptr;
	}

	return LoadedClass;
}

bool UHUICRSyncSubsystem::ResolveSpawnClasses(UClass* RequestedClass, UClass*& OutLocalSpawnClass, UClass*& OutRemoteSpawnClass) const
{
	OutLocalSpawnClass = nullptr;
	OutRemoteSpawnClass = nullptr;

	if (!RequestedClass || !RequestedClass->IsChildOf(AHUICRSyncActor::StaticClass()))
	{
		return false;
	}

	const AHUICRSyncActor* DefaultSyncActor = RequestedClass->GetDefaultObject<AHUICRSyncActor>();
	if (!DefaultSyncActor)
	{
		return false;
	}

	UClass* ExplicitCounterpartClass = nullptr;
	if (!DefaultSyncActor->RemoteCounterpartClass.IsNull())
	{
		ExplicitCounterpartClass = DefaultSyncActor->RemoteCounterpartClass.LoadSynchronous();
		if (ExplicitCounterpartClass && !ExplicitCounterpartClass->IsChildOf(AHUICRSyncActor::StaticClass()))
		{
			ExplicitCounterpartClass = nullptr;
		}
	}

	UClass* CounterpartClass = ExplicitCounterpartClass ? ExplicitCounterpartClass : RequestedClass;

	switch (DefaultSyncActor->SyncActorClassRole)
	{
	case EHUICRSyncActorClassRole::Shared:
		OutLocalSpawnClass = RequestedClass;
		OutRemoteSpawnClass = RequestedClass;
		return true;

	case EHUICRSyncActorClassRole::PC:
		if (Role == EHUICRSyncRole::PC)
		{
			OutLocalSpawnClass = RequestedClass;
			OutRemoteSpawnClass = CounterpartClass;
			return true;
		}
		if (Role == EHUICRSyncRole::HMD && ExplicitCounterpartClass)
		{
			OutLocalSpawnClass = ExplicitCounterpartClass;
			OutRemoteSpawnClass = RequestedClass;
			return true;
		}
		break;

	case EHUICRSyncActorClassRole::HMD:
		if (Role == EHUICRSyncRole::HMD)
		{
			OutLocalSpawnClass = RequestedClass;
			OutRemoteSpawnClass = CounterpartClass;
			return true;
		}
		if (Role == EHUICRSyncRole::PC && ExplicitCounterpartClass)
		{
			OutLocalSpawnClass = ExplicitCounterpartClass;
			OutRemoteSpawnClass = RequestedClass;
			return true;
		}
		break;

	default:
		break;
	}

	UE_LOG(LogTemp, Warning, TEXT("HUICRSync could not resolve spawn classes. Role=%d Class=%s ClassRole=%d Counterpart=%s"),
		static_cast<int32>(Role),
		*RequestedClass->GetPathName(),
		static_cast<int32>(DefaultSyncActor->SyncActorClassRole),
		ExplicitCounterpartClass ? *ExplicitCounterpartClass->GetPathName() : TEXT("None"));
	return false;
}

AHUICRSyncActor* UHUICRSyncSubsystem::SpawnSyncActorInternal(UClass* SpawnClass, int32 ActorID, const FTransform& SpawnTransform, const TArray<uint8>& InitPayloadBytes)
{
	if (!SpawnClass || !SpawnClass->IsChildOf(AHUICRSyncActor::StaticClass()) || ActorID < 0)
	{
		return nullptr;
	}

	const AHUICRSyncActor* DefaultSyncActor = SpawnClass->GetDefaultObject<AHUICRSyncActor>();
	const EHUICRSyncActorType ActorType = DefaultSyncActor ? DefaultSyncActor->TypeCode : EHUICRSyncActorType::HUIActor;
	if (AHUICRSyncActor* ExistingActor = FindSyncActor(ActorType, ActorID))
	{
		return ExistingActor;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	AHUICRSyncActor* NewActor = World->SpawnActorDeferred<AHUICRSyncActor>(
		SpawnClass,
		SpawnTransform,
		nullptr,
		nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);

	if (!IsValid(NewActor))
	{
		return nullptr;
	}

	NewActor->bAutoAssignActorID = false;
	NewActor->SetSyncActorID(ActorID);
	NewActor->SyncSubsystem = this;
	NewActor->ActorInitPayloadData = InitPayloadBytes;
	UGameplayStatics::FinishSpawningActor(NewActor, SpawnTransform);
	RegisterSyncActor(NewActor);
	return NewActor;
}

void UHUICRSyncSubsystem::CompleteSyncSceneAsAuthority()
{
	if (Role != EHUICRSyncRole::PC || !bWaitingForInitialSpawnSync)
	{
		return;
	}

	bWaitingForInitialSpawnSync = false;
	StartGameFromAuthority();
}

void UHUICRSyncSubsystem::StartGameFromAuthority()
{
	bGameStopped = false;
	OnSyncSceneReady.Broadcast();
	SendReliableCommand(TEXT("/FromPC/GameStart"), {});
}

void UHUICRSyncSubsystem::HandleCommandPayload(FHUICRSyncPeerRuntimeState& Peer, const FHUICRSyncCommandPayload& CommandPayload)
{
	const FString& Address = CommandPayload.Address;
	const TArray<FHUICRSyncCommandArg>& Args = CommandPayload.Args;

	if (Address.Equals(TEXT("FromHMD/HMDConnectionRequest"), ESearchCase::IgnoreCase) ||
		Address.Equals(TEXT("/FromHMD/HMDConnectionRequest"), ESearchCase::IgnoreCase))
	{
		if (Role != EHUICRSyncRole::PC || Args.Num() < 2 ||
			Args[0].Type != EHUICRSyncCommandArgType::String ||
			Args[1].Type != EHUICRSyncCommandArgType::Int32)
		{
			return;
		}

		RemoteIpAddress = Args[0].StringValue;
		RemoteSyncPort = Args[1].IntValue;
		const FString PeerId = RegisterOrUpdatePeer(Peer.Info.PeerId, EHUICRSyncRole::HMD, RemoteIpAddress, RemoteSyncPort);
		SetDefaultPeer(PeerId);
		InitializeSenderSocket(RemoteIpAddress, RemoteSyncPort);

		bConnectionState = true;
		OnConnectionStateChanged.Broadcast(bConnectionState);

		FHUICRSyncCommandArg IpArg;
		IpArg.Type = EHUICRSyncCommandArgType::String;
		IpArg.StringValue = ResolveLocalIpAddress();

		FHUICRSyncCommandArg PortArg;
		PortArg.Type = EHUICRSyncCommandArgType::Int32;
		PortArg.IntValue = LocalSyncPort;

		SendReliableCommand(TEXT("FromPC/PCConnectionRequest"), { IpArg, PortArg });
		return;
	}

	if (Address.Equals(TEXT("FromPC/PCConnectionRequest"), ESearchCase::IgnoreCase) ||
		Address.Equals(TEXT("/FromPC/PCConnectionRequest"), ESearchCase::IgnoreCase))
	{
		if (Role != EHUICRSyncRole::HMD || Args.Num() < 2 ||
			Args[0].Type != EHUICRSyncCommandArgType::String ||
			Args[1].Type != EHUICRSyncCommandArgType::Int32)
		{
			return;
		}

		RemoteIpAddress = Args[0].StringValue;
		RemoteSyncPort = Args[1].IntValue;
		const FString PeerId = RegisterOrUpdatePeer(Peer.Info.PeerId, EHUICRSyncRole::PC, RemoteIpAddress, RemoteSyncPort);
		SetDefaultPeer(PeerId);
		InitializeSenderSocket(RemoteIpAddress, RemoteSyncPort);

		bConnectionState = true;
		OnConnectionStateChanged.Broadcast(bConnectionState);
		return;
	}

	if (Address.Equals(TEXT("FromPC/SyncSpawnBegin"), ESearchCase::IgnoreCase) ||
		Address.Equals(TEXT("/FromPC/SyncSpawnBegin"), ESearchCase::IgnoreCase))
	{
		if (Role == EHUICRSyncRole::HMD)
		{
			bGameStopped = true;
			ClearLocalSyncActors();
		}
		return;
	}

	if (Address.Equals(TEXT("FromHMD/HMDSyncReady"), ESearchCase::IgnoreCase) ||
		Address.Equals(TEXT("/FromHMD/HMDSyncReady"), ESearchCase::IgnoreCase) ||
		Address.Equals(TEXT("FromHMD/SyncSpawnReady"), ESearchCase::IgnoreCase) ||
		Address.Equals(TEXT("/FromHMD/SyncSpawnReady"), ESearchCase::IgnoreCase))
	{
		if (Role == EHUICRSyncRole::PC)
		{
			CompleteSyncSceneAsAuthority();
		}
		return;
	}

	if (Address.Equals(TEXT("FromPC/GameStart"), ESearchCase::IgnoreCase) ||
		Address.Equals(TEXT("/FromPC/GameStart"), ESearchCase::IgnoreCase))
	{
		if (Role == EHUICRSyncRole::HMD)
		{
			bGameStopped = false;
			OnSyncSceneReady.Broadcast();
		}
		return;
	}

	if (Address.Equals(TEXT("FromHMD/DestroySyncActorRequest"), ESearchCase::IgnoreCase) ||
		Address.Equals(TEXT("/FromHMD/DestroySyncActorRequest"), ESearchCase::IgnoreCase))
	{
		if (Role == EHUICRSyncRole::PC && Args.Num() >= 2 &&
			Args[0].Type == EHUICRSyncCommandArgType::Int32 &&
			Args[1].Type == EHUICRSyncCommandArgType::Int32)
		{
			const EHUICRSyncActorType TypeCode = static_cast<EHUICRSyncActorType>(Args[0].IntValue);
			const int32 ActorID = Args[1].IntValue;
			DestroySyncActor(TypeCode, ActorID);
		}
		return;
	}

	if (Address.Equals(TEXT("FromPC/DestroySyncActor"), ESearchCase::IgnoreCase) ||
		Address.Equals(TEXT("/FromPC/DestroySyncActor"), ESearchCase::IgnoreCase))
	{
		if (Role == EHUICRSyncRole::HMD && Args.Num() >= 2 &&
			Args[0].Type == EHUICRSyncCommandArgType::Int32 &&
			Args[1].Type == EHUICRSyncCommandArgType::Int32)
		{
			DestroyLocalSyncActor(static_cast<EHUICRSyncActorType>(Args[0].IntValue), Args[1].IntValue);
		}
		return;
	}

	if (Address.Equals(TEXT("/FromHMD/Calibration/CalibrationState"), ESearchCase::IgnoreCase) ||
		Address.Equals(TEXT("FromHMD/Calibration/CalibrationState"), ESearchCase::IgnoreCase))
	{
		if (!Args.IsEmpty() && Args[0].Type == EHUICRSyncCommandArgType::Bool)
		{
			bCalibrationState = Args[0].BoolValue;
			if (bCalibrationState)
			{
				bGameStopped = true;
			}
			OnCalibrationStateChanged.Broadcast(bCalibrationState);
		}
		return;
	}

	if (Address.Equals(TEXT("/FromHMD/MotionParallaxState"), ESearchCase::IgnoreCase) ||
		Address.Equals(TEXT("FromHMD/MotionParallaxState"), ESearchCase::IgnoreCase))
	{
		if (!Args.IsEmpty() && Args[0].Type == EHUICRSyncCommandArgType::Bool)
		{
			bMotionParallaxState = Args[0].BoolValue;
			OnMotionParallaxStateChanged.Broadcast(bMotionParallaxState);
		}
		return;
	}

	if (Address.Equals(TEXT("/FromHMD/Calibration/SelectedScreenID"), ESearchCase::IgnoreCase) ||
		Address.Equals(TEXT("FromHMD/Calibration/SelectedScreenID"), ESearchCase::IgnoreCase) ||
		Address.Equals(TEXT("/SelectScreen"), ESearchCase::IgnoreCase) ||
		Address.Equals(TEXT("SelectScreen"), ESearchCase::IgnoreCase))
	{
		if (!Args.IsEmpty() && Args[0].Type == EHUICRSyncCommandArgType::Int32)
		{
			OnSelectedScreenChanged.Broadcast(Args[0].IntValue);
		}
		return;
	}

	if (Address.Equals(TEXT("/FromHMD/ChangeToWorldID"), ESearchCase::IgnoreCase) ||
		Address.Equals(TEXT("FromHMD/ChangeToWorldID"), ESearchCase::IgnoreCase))
	{
		if (!Args.IsEmpty() && Args[0].Type == EHUICRSyncCommandArgType::Int32)
		{
			WorldID = Args[0].IntValue;
			OnWorldChanged.Broadcast(WorldID);
		}
		return;
	}

}
