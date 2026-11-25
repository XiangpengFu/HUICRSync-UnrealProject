#include "HUICRSyncSubsystem.h"

#include "Common/UdpSocketBuilder.h"
#include "Engine/World.h"
#include "HUICRSyncActor.h"
#include "HUICRSyncHMDManagerBase.h"
#include "HUICRSyncPCManagerBase.h"
#include "HUICRSyncPayloadLibrary.h"
#include "IPAddress.h"
#include "Kismet/GameplayStatics.h"
#include "Networking.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Stats/Stats.h"

namespace HUICRSyncProtocol
{
	static constexpr uint32 SessionMagic = 0x48555332; // HUS2
	static constexpr uint32 PresenceMagic = 0x48555032;
	static constexpr uint32 LeaveMagic = 0x48554C32;
	static constexpr uint32 HeadPoseMagic = 0x48554832;
	static constexpr int32 ProtocolVersion = 12;

	static FHUICRSyncCommandArg IntArg(int32 Value)
	{
		FHUICRSyncCommandArg Arg;
		Arg.Type = EHUICRSyncCommandArgType::Int32;
		Arg.IntValue = Value;
		return Arg;
	}

	static FHUICRSyncCommandArg StringArg(const FString& Value)
	{
		FHUICRSyncCommandArg Arg;
		Arg.Type = EHUICRSyncCommandArgType::String;
		Arg.StringValue = Value;
		return Arg;
	}

	static constexpr uint32 RealtimeMagic = 0x48554952; // HUIR
	static constexpr uint32 SpawnMagic = 0x48554950; // HUIP
	static constexpr uint32 ManagedRealtimeMagic = 0x4855494D; // HUIM
	static constexpr uint32 CommandMagic = 0x48554943; // HUIC
	static constexpr uint32 CommandAckMagic = 0x48554941; // HUIA
	static constexpr uint32 PCCommandIdPrefix = 0x10000000;
	static constexpr uint32 HMDCommandIdPrefix = 0x20000000;
	static constexpr uint32 UnknownCommandIdPrefix = 0x30000000;

	static bool IsConnectionHandshakeAddress(const FString& Address)
	{
		return Address.Equals(TEXT("FromHMD/HMDConnectionRequest"), ESearchCase::IgnoreCase) ||
			Address.Equals(TEXT("/FromHMD/HMDConnectionRequest"), ESearchCase::IgnoreCase) ||
			Address.Equals(TEXT("FromPC/PCConnectionRequest"), ESearchCase::IgnoreCase) ||
			Address.Equals(TEXT("/FromPC/PCConnectionRequest"), ESearchCase::IgnoreCase);
	}
}

UHUICRSyncSubsystem::UHUICRSyncSubsystem()
{
	HMDToPCTransform = FTransform::Identity;
	PCToHMDTransform = FTransform::Identity;
}

void UHUICRSyncSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	ResetLocalSession();
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

	if (!bGameStopped && bConnectionState &&
		(Role == EHUICRSyncRole::PC || bIsLocalHMDHost || (bManagedCalibration && bLocalSceneReady)))
	{
		OnPrepareNetDataSend.Broadcast();
		SendAllQueuedStates();
	}

	TickReliableCommands();
	TickPresence();
	TickCalibration();
	TickManagedScene();
	TickSyncFields();
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
	if (InLocalSyncPort <= 0 || InLocalSyncPort >= 65535 ||
		(!InRemoteIpAddress.IsEmpty() && (InRemoteSyncPort <= 0 || InRemoteSyncPort >= 65535)))
	{
		return false;
	}
	if (ReceiverSocket && (InLocalSyncPort != LocalSyncPort || InRole != Role ||
		(InRole == EHUICRSyncRole::HMD && (RemoteIpAddress != InRemoteIpAddress || RemoteSyncPort != InRemoteSyncPort))))
	{
		StopSync();
	}

	if (LocalSessionId.IsEmpty())
	{
		ResetLocalSession();
	}

	Role = InRole;
	LocalSyncPort = InLocalSyncPort;
	RemoteSyncPort = InRemoteSyncPort;
	RemoteIpAddress = InRemoteIpAddress;
	UDPReceiverPort = LocalSyncPort + 1;
	InitialReliableCommandSendCount = Role == EHUICRSyncRole::HMD ? 3 : 1;
	LocalIpAddress = ResolveLocalIpAddress();

	const bool bReceiverReady = ReceiverSocket || InitializeReceiverSocket();
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

bool UHUICRSyncSubsystem::EnsureSenderSocket()
{
	if (!SenderSocket)
	{
		FIPv4Address LocalAddress;
		if (!FIPv4Address::Parse(LocalIpAddress.IsEmpty() ? ResolveLocalIpAddress() : LocalIpAddress, LocalAddress))
		{
			return false;
		}
		// The reply must leave through the address the headset actually connected to on a multi-NIC PC.
		SenderSocket = FUdpSocketBuilder(TEXT("HUICRSync_UDP_Sender"))
			.AsReusable()
			.BoundToEndpoint(FIPv4Endpoint(LocalAddress, 0))
			.WithBroadcast()
			.WithSendBufferSize(2 * 1024 * 1024);
	}

	return SenderSocket != nullptr;
}

bool UHUICRSyncSubsystem::InitializeSenderSocket(const FString& InRemoteIpAddress, int32 InRemoteSyncPort)
{
	if (!EnsureSenderSocket())
	{
		return false;
	}
	RemoteIpAddress = InRemoteIpAddress;
	RemoteSyncPort = InRemoteSyncPort;

	const FString PeerId = RegisterOrUpdatePeer(FString(), Role == EHUICRSyncRole::PC ? EHUICRSyncRole::HMD : EHUICRSyncRole::PC, RemoteIpAddress, RemoteSyncPort);
	FHUICRSyncPeerRuntimeState* Peer = FindPeer(PeerId);
	return Peer && Peer->UDPAddr.IsValid();
}

void UHUICRSyncSubsystem::ReceiveData()
{
	// A receive-event Blueprint may call ReceiveData. Finish this batch before draining the socket again.
	if (bDispatchingActorFieldEvents)
	{
		return;
	}
	if (!ReceiverSocket)
	{
		DispatchActorFieldEvents();
		return;
	}

	ReceivedDataMap.Empty();
	SpawnActorPackets.Empty();

	bool bReceivedAnyData = false;
	bool bReceivedSpawnData = false;
	bool bReceivedLastSpawnPacket = false;

	TArray<uint8> Data;
	uint32 Size = 0;

	while (ReceiverSocket && ReceiverSocket->HasPendingData(Size))
	{
		Data.SetNumUninitialized(static_cast<int32>(FMath::Min(Size, 65507u)));

		int32 BytesRead = 0;
		const TSharedRef<FInternetAddr> SourceAddr = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
		const bool bReceived = ReceiverSocket->RecvFrom(Data.GetData(), Data.Num(), BytesRead, *SourceAddr);

		if (!bReceived || BytesRead <= 0)
		{
			continue;
		}

		const FIPv4Endpoint Sender(SourceAddr);
		Data.SetNum(BytesRead);
		FMemoryReader Reader(Data, true);

		uint32 Magic = 0;
		Reader << Magic;

		FHUICRSyncPeerRuntimeState* Peer = nullptr;
		if (Magic == HUICRSyncProtocol::SessionMagic)
		{
			FGuid SourceSession, TargetSession;
			Reader << SourceSession << TargetSession << Magic;
			Peer = FindPeerByEndpoint(Sender);
			if (Reader.IsError() || !Peer || Peer->RemoteSession != SourceSession || TargetSession != LocalSession)
			{
				continue;
			}
			Peer->Info.LastReceiveTime = FPlatformTime::Seconds();
			if (FHUICRSyncPeerInfo* Snapshot = PeerInfos.Find(Peer->Info.PeerId))
			{
				Snapshot->LastReceiveTime = Peer->Info.LastReceiveTime;
			}
		}
		else if (Magic != HUICRSyncProtocol::CommandMagic)
		{
			continue;
		}

		if (Magic == HUICRSyncProtocol::LeaveMagic)
		{
			const FString DepartingId = Peer->Info.PeerId;
			RemoveParticipant(DepartingId);
			continue;
		}
		if (Magic == FieldPacketMagic && Peer)
		{
			bReceivedAnyData |= ReceiveFieldPacket(*Peer, Reader);
			continue;
		}
		if (Magic == HUICRSyncProtocol::PresenceMagic)
		{
			if (Role == EHUICRSyncRole::HMD)
			{
				int32 Revision = 0;
				int32 Count = 0;
				Reader << Revision << Count;
				if (!Reader.IsError() && Count > 0 && static_cast<int64>(Count) * 4 < Reader.TotalSize() - Reader.Tell())
				{
					TArray<int32> Participants;
					Participants.SetNumUninitialized(Count);
					for (int32& Id : Participants)
					{
						Reader << Id;
					}
					ApplyHMDRoster(Revision, Participants);
					ReadCalibrationStatus(Reader);
				}
			}
			continue;
		}
		if (Magic == HUICRSyncProtocol::HeadPoseMagic)
		{
			uint32 Sequence = 0;
			FTransform Pose;
			Reader << Sequence << Pose;
			if (Role == EHUICRSyncRole::PC && !Reader.IsError() && Pose.IsValid() &&
				static_cast<int32>(Sequence - Peer->LastPoseSequence) > 0)
			{
				if (!Peer->Info.bHasHeadPose)
				{
					UE_LOG(LogTemp, Log, TEXT("HUICRSync first head pose: participant %d, PC position (cm) %s"),
						Peer->Info.ParticipantId, *Pose.GetLocation().ToString());
				}
				Peer->LastPoseSequence = Sequence;
				Peer->Info.bHasHeadPose = true;
				Peer->Info.HeadPoseInPCSpace = Pose;
				PeerInfos.Add(Peer->Info.PeerId, Peer->Info);
			}
			continue;
		}
		if (Magic == HUICRSyncProtocol::CommandAckMagic)
		{
			uint32 AckCommandId = 0;
			Reader << AckCommandId;
			if (!Reader.IsError())
			{
				Peer->PendingReliableCommands.Remove(AckCommandId);
			}
			continue;
		}
		if (Magic == HUICRSyncProtocol::CommandMagic)
		{
			uint32 CommandId = 0;
			FHUICRSyncCommandPayload CommandPayload;
			Reader << CommandId << CommandPayload;
			if (Reader.IsError())
			{
				continue;
			}
			const bool bHandshake = HUICRSyncProtocol::IsConnectionHandshakeAddress(CommandPayload.Address);
			if (bHandshake && Peer)
			{
				continue;
			}
			if (bHandshake)
			{
				Peer = AcceptHandshake(Sender, CommandPayload);
			}
			if (!Peer)
			{
				continue;
			}
			SendCommandAck(*Peer, CommandId);
			if (Peer->ReceivedCommandIds.Contains(CommandId))
			{
				continue;
			}
			Peer->ReceivedCommandIds.Add(CommandId);
			Peer->Info.LastReceiveTime = FPlatformTime::Seconds();
			if (!bHandshake && HandleCalibrationCommand(*Peer, CommandPayload))
			{
				continue;
			}
			if (!bHandshake && HandleManagedSceneCommand(*Peer, CommandPayload))
			{
				continue;
			}
			if (!bHandshake && HandleFieldCommand(*Peer, CommandPayload))
			{
				continue;
			}
			if (!bHandshake && Role == EHUICRSyncRole::PC && Peer->Info.ParticipantId != HostParticipantId)
			{
				// Existing single-user Blueprint commands affect shared state.
				continue;
			}
			if (bManagedCalibration && Role == EHUICRSyncRole::PC && !bHandshake)
			{
				const FString Address = CommandPayload.Address.StartsWith(TEXT("/")) ?
					CommandPayload.Address.Mid(1) : CommandPayload.Address;
				if (Address.StartsWith(TEXT("FromHMD/Calibration/")) || Address.Equals(TEXT("SelectScreen"), ESearchCase::IgnoreCase))
				{
					continue;
				}
				if (Address.Equals(TEXT("FromHMD/MotionParallaxState"), ESearchCase::IgnoreCase))
				{
					if (!CommandPayload.Args.IsEmpty() && CommandPayload.Args[0].Type == EHUICRSyncCommandArgType::Bool)
					{
						HandleManagedParallax(CommandPayload.Args[0].BoolValue);
					}
					continue;
				}
				if (bCalibrationPaused && !Address.Equals(TEXT("FromHMD/SyncSpawnReady"), ESearchCase::IgnoreCase) &&
					!Address.Equals(TEXT("FromHMD/HMDSyncReady"), ESearchCase::IgnoreCase))
				{
					continue;
				}
			}
			if (!bHandshake)
			{
				bReceivedAnyData = true;
			}
			const FString CommandPeerId = Peer->Info.PeerId;
			HandleCommandPayload(*Peer, CommandPayload);
			if (Role != EHUICRSyncRole::PC || CommandPeerId == DefaultPeerId)
			{
				OnCommandReceived.Broadcast(CommandPayload.Address, CommandPayload.Args);
			}
			continue;
		}
		// Until queued calibration and multiplayer gameplay ship, only the Host drives the
		// existing single-user gameplay path. All registered headsets publish independent head poses.
		if (Role == EHUICRSyncRole::PC && Peer->Info.ParticipantId != HostParticipantId)
		{
			continue;
		}

		int32 NumEntries = 0;
		uint64 StateSequence = 0;
		if (Magic == HUICRSyncProtocol::ManagedRealtimeMagic && Role == EHUICRSyncRole::HMD && bManagedCalibration)
		{
			Reader << StateSequence << NumEntries;
		}
		else if (Magic == HUICRSyncProtocol::RealtimeMagic)
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

			if (bManagedCalibration &&
				(ReceivedEntry.TypeCode == EHUICRSyncActorType::HUIScreen || ReceivedEntry.TypeCode == EHUICRSyncActorType::HUICalibrator ||
					(bCalibrationPaused && !(Role == EHUICRSyncRole::HMD && Magic == HUICRSyncProtocol::ManagedRealtimeMagic))))
			{
				continue;
			}
			bReceivedAnyData = true;

			if (ReceivedEntry.TypeCode == EHUICRSyncActorType::HUISpawnInGame)
			{
				if (bManagedCalibration)
				{
					continue;
				}
				SpawnActorPackets.Add(ReceivedEntry);
				bReceivedSpawnData = true;
				continue;
			}
			if (StateSequence > 0 && ReceivedEntry.TypeCode == EHUICRSyncActorType::HUIActor)
			{
				if (RemovedSceneActorIds.Contains(ReceivedEntry.ActorID))
				{
					continue;
				}
				FManagedActorState& Latest = ReceivedManagedActorStates.FindOrAdd(ReceivedEntry.ActorID);
				if (StateSequence <= Latest.Sequence)
				{
					continue;
				}
				Latest = { ReceivedEntry, StateSequence };
			}

			ReceivedDataMap.FindOrAdd(ReceivedEntry.TypeCode).InnerMap.Add(ReceivedEntry.ActorID, ReceivedEntry);
		}
	}

	DispatchActorFieldEvents();
	if (bReceivedAnyData || bFieldReceivePending)
	{
		bFieldReceivePending = false;
		OnNetDataReceived.Broadcast();
	}

	if (bReceivedSpawnData && !SpawnActorPackets.IsEmpty())
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
	if (Role == EHUICRSyncRole::HMD && bConnectionState && !bIsLocalHMDHost)
	{
		RegisteredStates.Reset();
		ScreenRegisteredStates.Reset();
		return;
	}
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

	if (Role == EHUICRSyncRole::PC && bManagedCalibration)
	{
		const uint64 StateSequence = ++ManagedStateSequence;
		for (const auto& Entry : RealtimeEntries)
		{
			if (Entry.TypeCode == EHUICRSyncActorType::HUIActor)
			{
				const FString Key = MakeActorKey(Entry.TypeCode, Entry.ActorID);
				if (ManagedSceneActors.Contains(Key))
				{
					LatestManagedActorStates.Add(Key, { Entry, StateSequence });
				}
			}
		}
		for (auto& Pair : Peers)
		{
			if (Pair.Value.Info.bConnected && Pair.Value.bHasCalibrated)
			{
				TArray<FHUICRSyncNetEntry> VisibleEntries;
				VisibleEntries.Reserve(RealtimeEntries.Num());
				for (const auto& Entry : RealtimeEntries)
				{
					const FManagedSceneActor* SceneActor = ManagedSceneActors.Find(MakeActorKey(Entry.TypeCode, Entry.ActorID));
					if (Entry.TypeCode != EHUICRSyncActorType::HUIActor ||
						(SceneActor && SceneActor->ExcludedParticipantId != Pair.Value.Info.ParticipantId))
					{
						VisibleEntries.Add(Entry);
					}
				}
				SendQueuedEntries(Pair.Value, VisibleEntries, HUICRSyncProtocol::ManagedRealtimeMagic, StateSequence);
			}
		}
	}
	else
	{
		SendQueuedEntries(*Peer, RealtimeEntries, HUICRSyncProtocol::RealtimeMagic);
	}

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

		WriteSessionHeader(Writer, *Peer);
		Writer << Magic;
		Writer << BatchId;
		Writer << PacketIndex;

		const int64 LastPacketPos = Writer.Tell();
		Writer << bLastPacket;

		const int64 NumEntriesPos = Writer.Tell();
		Writer << NumEntries;
		const int64 HeaderBytes = Writer.Tell();

		int32 TestIndex = PendingSpawnSendIndex;
		while (TestIndex < PendingSpawnPackets.Num())
		{
			TArray<uint8> EntryBytes;
			FMemoryWriter EntryWriter(EntryBytes, true);
			FHUICRSyncNetEntry Entry = PendingSpawnPackets[TestIndex];
			EntryWriter << Entry;

			if (HeaderBytes + EntryBytes.Num() > MaxPacketBytes)
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
	if (Role == EHUICRSyncRole::HMD && bManagedCalibration)
	{
		return RequestSpawnSyncActor(SpawnClass, SpawnTransform, {}, bSpawnBackOnSender);
	}

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
	if (Role == EHUICRSyncRole::PC && bManagedCalibration)
	{
		const auto ActorType = SpawnClass->GetDefaultObject<AHUICRSyncActor>()->TypeCode;
		return bSpawningManagedActor || RegisterManagedSceneMirror(FindSyncActor(ActorType, ActorID), SpawnClass, InitPayloadBytes);
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
	if (Role == EHUICRSyncRole::HMD && bManagedCalibration)
	{
		return SendManagedSpawnRequest(Entry.PayloadBytes);
	}
	RegisteredStates.Add(Entry);
	return !Entry.PayloadBytes.IsEmpty();
}

AHUICRSyncActor* UHUICRSyncSubsystem::SpawnSyncActorAsAuthority(UClass* SpawnClass, const FTransform& SpawnTransform, const TArray<uint8>& InitPayloadBytes, bool bSpawnOnPeer)
{
	return SpawnSyncActorForPeers(SpawnClass, SpawnTransform, InitPayloadBytes, bSpawnOnPeer, 0);
}

AHUICRSyncActor* UHUICRSyncSubsystem::SpawnSyncActorForPeers(UClass* SpawnClass, const FTransform& SpawnTransform,
	const TArray<uint8>& InitPayloadBytes, bool bSpawnOnPeer, int32 ExcludedParticipantId)
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
	if (bManagedCalibration && (ActorType != EHUICRSyncActorType::HUIActor || !SpawnTransform.IsValid() ||
		(bSpawnOnPeer && (!RemoteSpawnClass || RemoteSpawnClass->GetDefaultObject<AHUICRSyncActor>()->TypeCode != ActorType))))
	{
		return nullptr;
	}
	const int32 NewActorID = AllocateActorID(ActorType);
	TGuardValue<bool> SpawningActor(bSpawningManagedActor, bManagedCalibration);
	AHUICRSyncActor* NewActor = SpawnSyncActorInternal(LocalSpawnClass, NewActorID, SpawnTransform, InitPayloadBytes);
	if (!IsValid(NewActor))
	{
		return nullptr;
	}
	if (bManagedCalibration)
	{
		if (bSpawnOnPeer)
		{
			const TArray<uint8>& InitialPayload = !InitPayloadBytes.IsEmpty() ? InitPayloadBytes : NewActor->ActorInitPayloadData;
			if (RegisterManagedSceneMirror(NewActor, RemoteSpawnClass, InitialPayload, ExcludedParticipantId))
			{
				ManagedSceneActors.FindChecked(MakeActorKey(ActorType, NewActorID)).bSpawnedAtRuntime = true;
			}
		}
		else
		{
			LocalOnlySceneActors.Add(MakeActorKey(ActorType, NewActorID));
		}
		return NewActor;
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
		UHUICRSyncPayloadLibrary::WriteSpawnMirrorPayload(Entry.PayloadBytes, RemoteSpawnClass, NewActorID, NewActor->GetActorTransform(), MirrorInitPayloadBytes);
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

bool UHUICRSyncSubsystem::SpawnSyncActorFromLocalSpace(UClass* SpawnClass, const FTransform& SpawnTransform,
	const TArray<uint8>& InitPayloadBytes)
{
	return SpawnSyncActor(SpawnClass, Role == EHUICRSyncRole::HMD ? ConvertHMDActorTransformToPC(SpawnTransform) : SpawnTransform,
		InitPayloadBytes);
}

int32 UHUICRSyncSubsystem::StartInitialSpawnSync()
{
	if (Role != EHUICRSyncRole::PC)
	{
		return 0;
	}

	if (bManagedCalibration)
	{
		PrepareManagedScene();
		if (bHasStartedGame)
		{
			for (auto& Pair : Peers)
			{
				if (Pair.Value.Info.bConnected && Pair.Value.bHasCalibrated &&
					!CalibrationTurns.Contains(Pair.Value.Info.ParticipantId))
				{
					StartManagedSceneForPeer(Pair.Value);
				}
			}
			PublishCalibrationStatus();
		}
		TickManagedScene();
		return ManagedSceneActors.Num();
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
			SyncActor->GetActorTransform(),
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
	PendingActorFieldEvents.Remove(SyncActor);
	if (!IsValid(SyncActor) || SyncActor->ActorID < 0)
	{
		return;
	}

	const FString ActorKey = MakeActorKey(SyncActor->TypeCode, SyncActor->ActorID);
	if (SpawnedSyncActors.FindRef(ActorKey) == SyncActor)
	{
		SpawnedSyncActors.Remove(ActorKey);
		if (Role == EHUICRSyncRole::PC && bManagedCalibration)
		{
			UnregisterManagedSceneMirror(*SyncActor);
		}
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
		if (bManagedCalibration)
		{
			return bDestroyedLocalActor;
		}
		const bool bSentDestroyCommand = SendReliableCommand(TEXT("/FromPC/DestroySyncActor"), { TypeArg, ActorIdArg });
		return bDestroyedLocalActor || bSentDestroyCommand;
	}

	if (Role == EHUICRSyncRole::HMD)
	{
		if (bManagedCalibration)
		{
			return SendManagedDestroyRequest(TypeCode, ActorID);
		}
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
			if (Role == EHUICRSyncRole::PC && bManagedCalibration)
			{
				UnregisterManagedSceneMirror(*SyncActor);
			}
		}
	}

	SpawnedSyncActors.Reset();
	for (const EHUICRSyncActorType TypeCode : ClearedTypes)
	{
		ReceivedDataMap.Remove(TypeCode);
		if (!bManagedCalibration)
		{
			ActorIdCounters.Remove(TypeCode);
		}
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
	const bool bParticipantCommand = Address == TEXT("/HUICRSync/Fields/Source") ||
		Address.StartsWith(TEXT("/HUICRSync/Calibration/"), ESearchCase::CaseSensitive) ||
		Address.StartsWith(TEXT("/HUICRSync/Scene/"), ESearchCase::CaseSensitive);
	if (!HUICRSyncProtocol::IsConnectionHandshakeAddress(Address) &&
		(!bConnectionState || (Role == EHUICRSyncRole::HMD && !bIsLocalHMDHost && !bParticipantCommand)))
	{
		return false;
	}
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
	if (Role == EHUICRSyncRole::HMD && HUICRSyncProtocol::IsConnectionHandshakeAddress(Address) && Pending.Args.Num() == 3)
	{
		Pending.Args.Add(HUICRSyncProtocol::IntArg(HUICRSyncProtocol::ProtocolVersion));
	}
	Pending.LastSendTime = FPlatformTime::Seconds();
	Pending.RetryCount = 0;

	Peer->PendingReliableCommands.Add(CommandId, Pending);

	bool bAnySent = false;
	const int32 SendCount = FMath::Max(1, InitialReliableCommandSendCount);
	for (int32 Index = 0; Index < SendCount; ++Index)
	{
		bAnySent |= SendReliableCommandPacket(*Peer, CommandId, Address, Pending.Args);
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
	const FHUICRSyncPeerRuntimeState* Peer = FindPeer(PeerId);
	if (!Peer || (Role == EHUICRSyncRole::PC && HostParticipantId != 0 &&
		Peer->Info.ParticipantId != HostParticipantId))
	{
		return false;
	}

	DefaultPeerId = PeerId;
	RemoteIpAddress = Peer->Info.IpAddress;
	RemoteSyncPort = Peer->Info.SyncPort;
	return true;
}

void UHUICRSyncSubsystem::BeginConnectionAttempt()
{
	FHUICRSyncPeerRuntimeState* Peer = FindDefaultPeer();
	if (!Peer || bConnectionState)
	{
		return;
	}
	for (const TPair<uint32, FHUICRSyncPendingCommand>& Command : Peer->PendingReliableCommands)
	{
		if (HUICRSyncProtocol::IsConnectionHandshakeAddress(Command.Value.Address))
		{
			return;
		}
	}
	ResetLocalSession();
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

void UHUICRSyncSubsystem::ReserveActorID(EHUICRSyncActorType TypeCode, int32 ActorID)
{
	if (ActorID < 0)
	{
		return;
	}

	int32& Counter = ActorIdCounters.FindOrAdd(TypeCode);
	Counter = FMath::Max(Counter, ActorID + 1);
}

void UHUICRSyncSubsystem::StopSync()
{
	for (const TPair<FString, FHUICRSyncPeerRuntimeState>& Pair : Peers)
	{
		if (Pair.Value.Info.bConnected)
		{
			SendPresence(Pair.Value, true);
			if (Role == EHUICRSyncRole::PC)
			{
				RevokeParticipantFields(Pair.Value.Info.ParticipantId);
			}
		}
	}
	DestroySockets();
	EarlyFieldRequests.Reset();
	bFieldReceivePending = false;
	PendingActorFieldEvents.Reset();

	ResetCalibrationSession();
	Peers.Empty();
	RetiredSessions.Reset();
	ConnectedParticipantIds.Reset();
	HostParticipantId = LocalParticipantId = ConnectedHMDCount = 0;
	bIsLocalHMDHost = false;
	RosterRevision = 0;
	ResetLocalSession();
	PeerInfos.Empty();
	DefaultPeerId.Empty();
	ClearSessionTraffic();
	if (Role != EHUICRSyncRole::PC || !bManagedCalibration)
	{
		SpawnedSyncActors.Empty();
		ActorIdCounters.Empty();
	}
	SetConnected(false);
	OnParticipantsChanged.Broadcast();
}

void UHUICRSyncSubsystem::ClearSessionTraffic()
{
	RegisteredStates.Reset();
	ScreenRegisteredStates.Reset();
	PendingSpawnPackets.Reset();
	PendingSpawnSendIndex = 0;
	CurrentSpawnPacketIndex = 0;
	ReceivedDataMap.Reset();
	SpawnActorPackets.Reset();
	bWaitingForInitialSpawnSync = false;
}

FTransform UHUICRSyncSubsystem::ConvertHMDTransformToPC(const FTransform& InHMDTransform) const
{
	return InHMDTransform * HMDToPCTransform;
}

FTransform UHUICRSyncSubsystem::ConvertPCTransformToHMD(const FTransform& InPCTransform) const
{
	FTransform Mapping = PCToHMDTransform;
	if (Role == EHUICRSyncRole::HMD && GuestActorMapping.IsValid())
	{
		Mapping.SetScale3D(FVector::OneVector);
	}
	return InPCTransform * Mapping;
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
		if (Peer.RemoteSession.IsValid() && Peer.Info.IpAddress == SenderIp && Peer.SenderPort == SenderUDPPort)
		{
			return &Peer;
		}
	}
	return nullptr;
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
	if (!HUICRSyncProtocol::IsConnectionHandshakeAddress(Address))
	{
		WriteSessionHeader(Writer, Peer);
	}
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
	WriteSessionHeader(Writer, Peer);
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

bool UHUICRSyncSubsystem::SendQueuedEntries(FHUICRSyncPeerRuntimeState& Peer, const TArray<FHUICRSyncNetEntry>& Entries, uint32 Magic,
	uint64 StateSequence)
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

		WriteSessionHeader(Writer, Peer);
		Writer << Magic;
		if (Magic == HUICRSyncProtocol::ManagedRealtimeMagic)
		{
			Writer << StateSequence;
		}
		const int64 NumEntriesPos = Writer.Tell();
		Writer << NumEntries;
		const int64 HeaderBytes = Writer.Tell();

		while (RealtimeIndex < Entries.Num())
		{
			TArray<uint8> EntryBytes;
			FMemoryWriter EntryWriter(EntryBytes, true);
			FHUICRSyncNetEntry Entry = Entries[RealtimeIndex];
			EntryWriter << Entry;

			if (HeaderBytes + EntryBytes.Num() > MaxPacketBytes)
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

	AHUICRSyncActor* SpawnedActor = SpawnSyncActorAsAuthority(SpawnClass, RequestedHMDTransform, InitPayloadBytes, bSpawnBackOnSender);
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

	const FTransform LocalSpawnTransform = Role == EHUICRSyncRole::HMD ? ConvertPCTransformToHMD(SpawnTransform) : SpawnTransform;
	return IsValid(SpawnSyncActorInternal(SpawnClass, ActorID, LocalSpawnTransform, InitPayloadBytes));
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
	if (!World && bManagedCalibration)
	{
		if (Role == EHUICRSyncRole::PC && CalibrationPC.IsValid())
		{
			World = CalibrationPC->GetWorld();
		}
		else if (Role == EHUICRSyncRole::HMD && CalibrationHMD.IsValid())
		{
			World = CalibrationHMD->GetWorld();
		}
	}
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
	NewActor->InitializeSpawnPayload(InitPayloadBytes);
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
	if (bManagedCalibration)
	{
		bAwaitingFirstSpawn = false;
		bInitialCalibrationSceneReady = true;
		BeginManagedGameplay();
		PublishCalibrationStatus();
		return;
	}
	bGameStopped = false;
	OnSyncSceneReady.Broadcast();
	SendReliableCommand(TEXT("/FromPC/GameStart"), {});
}

void UHUICRSyncSubsystem::HandleCommandPayload(FHUICRSyncPeerRuntimeState& Peer, const FHUICRSyncCommandPayload& CommandPayload)
{
	const FString& Address = CommandPayload.Address;
	const TArray<FHUICRSyncCommandArg>& Args = CommandPayload.Args;

	if (HUICRSyncProtocol::IsConnectionHandshakeAddress(Address))
	{
		Peer.Info.bConnected = true;
		if (Role == EHUICRSyncRole::PC)
		{
			const bool bJoined = Peer.Info.ParticipantId == 0;
			if (bJoined)
			{
				Peer.Info.ParticipantId = NextParticipantId++;
				ConnectedParticipantIds.Add(Peer.Info.ParticipantId);
				UE_LOG(LogTemp, Log, TEXT("HUICRSync participant %d joined (%s)"), Peer.Info.ParticipantId, *Peer.Info.PeerId);
			}
			const FString ReplyPeerId = Peer.Info.PeerId;
			const FString Echo = Peer.RemoteSession.ToString();
			const int32 ParticipantId = Peer.Info.ParticipantId;
			if (bJoined)
			{
				UpdatePCParticipants();
			}
			TArray<FHUICRSyncCommandArg> Reply {
				HUICRSyncProtocol::StringArg(LocalIpAddress), HUICRSyncProtocol::IntArg(LocalSyncPort),
				HUICRSyncProtocol::StringArg(LocalSessionId), HUICRSyncProtocol::StringArg(Echo),
				HUICRSyncProtocol::IntArg(ParticipantId), HUICRSyncProtocol::IntArg(RosterRevision)
			};
			for (int32 Id : ConnectedParticipantIds)
			{
				Reply.Add(HUICRSyncProtocol::IntArg(Id));
			}
			SendReliableCommandToPeer(ReplyPeerId, TEXT("FromPC/PCConnectionRequest"), Reply);
		}
		else
		{
			LocalParticipantId = Args[4].IntValue;
			TArray<int32> Roster;
			for (int32 Index = 6; Index < Args.Num(); ++Index)
			{
				Roster.Add(Args[Index].IntValue);
			}
			RefreshPeerInfoSnapshot();
			ApplyHMDRoster(Args[5].IntValue, Roster);
		}
		return;
	}

	if (Address.Equals(TEXT("FromPC/SyncSpawnBegin"), ESearchCase::IgnoreCase) ||
		Address.Equals(TEXT("/FromPC/SyncSpawnBegin"), ESearchCase::IgnoreCase))
	{
		if (Role == EHUICRSyncRole::HMD && !bManagedCalibration)
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
		if (Role == EHUICRSyncRole::PC && !bManagedCalibration)
		{
			CompleteSyncSceneAsAuthority();
		}
		return;
	}

	if (Address.Equals(TEXT("FromPC/GameStart"), ESearchCase::IgnoreCase) ||
		Address.Equals(TEXT("/FromPC/GameStart"), ESearchCase::IgnoreCase))
	{
		if (Role == EHUICRSyncRole::HMD && !bManagedCalibration)
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

void UHUICRSyncSubsystem::ResetLocalSession()
{
	LocalSession = FGuid::NewGuid();
	LocalSessionId = LocalSession.ToString();
	PoseSequence = 0;
}

void UHUICRSyncSubsystem::WriteSessionHeader(FArchive& Writer, const FHUICRSyncPeerRuntimeState& Peer) const
{
	uint32 Magic = HUICRSyncProtocol::SessionMagic;
	FGuid Source = LocalSession;
	FGuid Target = Peer.RemoteSession;
	Writer << Magic << Source << Target;
}

FHUICRSyncPeerRuntimeState* UHUICRSyncSubsystem::AcceptHandshake(const FIPv4Endpoint& Sender, const FHUICRSyncCommandPayload& Hello)
{
	const auto& Args = Hello.Args;
	if (Args.Num() < 4 || Args[0].Type != EHUICRSyncCommandArgType::String ||
		Args[1].Type != EHUICRSyncCommandArgType::Int32 || Args[1].IntValue <= 0 || Args[1].IntValue >= 65535 ||
		Args[2].Type != EHUICRSyncCommandArgType::String)
	{
		return nullptr;
	}
	FGuid Session;
	if (!FGuid::Parse(Args[2].StringValue, Session) || !Session.IsValid() || RetiredSessions.Contains(Session))
	{
		return nullptr;
	}
	const FString Ip = Sender.Address.ToString();
	const int32 SyncPort = Args[1].IntValue;
	const FString PeerId = MakePeerId(Ip, SyncPort);
	const FString Address = Hello.Address.StartsWith(TEXT("/")) ? Hello.Address.Mid(1) : Hello.Address;

	if (Role == EHUICRSyncRole::PC)
	{
		if (!Address.Equals(TEXT("FromHMD/HMDConnectionRequest"), ESearchCase::IgnoreCase) ||
			Args[3].Type != EHUICRSyncCommandArgType::Int32 || Args[3].IntValue != HUICRSyncProtocol::ProtocolVersion)
		{
			return nullptr;
		}
	}
	else if (Role == EHUICRSyncRole::HMD)
	{
		if (!Address.Equals(TEXT("FromPC/PCConnectionRequest"), ESearchCase::IgnoreCase) ||
			PeerId != DefaultPeerId || Args.Num() < 7 || Args[3].Type != EHUICRSyncCommandArgType::String ||
			Args[3].StringValue != LocalSessionId || Args[4].Type != EHUICRSyncCommandArgType::Int32 ||
			Args[4].IntValue <= 0 || Args[5].Type != EHUICRSyncCommandArgType::Int32 || Args[5].IntValue <= 0)
		{
			return nullptr;
		}
		bool bContainsLocalId = false;
		int32 PreviousId = 0;
		for (int32 Index = 6; Index < Args.Num(); ++Index)
		{
			if (Args[Index].Type != EHUICRSyncCommandArgType::Int32 || Args[Index].IntValue <= PreviousId)
			{
				return nullptr;
			}
			PreviousId = Args[Index].IntValue;
			bContainsLocalId |= PreviousId == Args[4].IntValue;
		}
		if (!bContainsLocalId || (LocalParticipantId != 0 && LocalParticipantId != Args[4].IntValue))
		{
			return nullptr;
		}
	}
	else
	{
		return nullptr;
	}
	if (!EnsureSenderSocket())
	{
		return nullptr;
	}
	for (const TPair<FString, FHUICRSyncPeerRuntimeState>& Pair : Peers)
	{
		const FHUICRSyncPeerRuntimeState& Other = Pair.Value;
		if (Pair.Key != PeerId && (Other.RemoteSession == Session ||
			(Other.Info.IpAddress == Ip && Other.SenderPort == Sender.Port)))
		{
			return nullptr;
		}
	}
	FHUICRSyncPeerRuntimeState* Peer = FindPeer(PeerId);
	if (Peer && Peer->RemoteSession.IsValid() && Peer->RemoteSession != Session)
	{
		if (Role == EHUICRSyncRole::HMD)
		{
			// A new PC process needs a fresh connect attempt; don't let stale replies replace it.
			return nullptr;
		}
		RemoveParticipant(PeerId);
		Peer = nullptr;
	}
	if (!Peer)
	{
		const auto PeerRole = Role == EHUICRSyncRole::PC ? EHUICRSyncRole::HMD : EHUICRSyncRole::PC;
		RegisterOrUpdatePeer(PeerId, PeerRole, Ip, SyncPort);
		Peer = FindPeer(PeerId);
	}
	if (!Peer || (Peer->RemoteSession.IsValid() && Peer->SenderPort != Sender.Port))
	{
		return nullptr;
	}
	Peer->RemoteSession = Session;
	Peer->SenderPort = Sender.Port;
	return Peer;
}

void UHUICRSyncSubsystem::SetConnected(bool bConnected)
{
	if (bConnectionState != bConnected)
	{
		bConnectionState = bConnected;
		OnConnectionStateChanged.Broadcast(bConnected);
	}
}

void UHUICRSyncSubsystem::UpdatePCParticipants()
{
	++RosterRevision;
	ConnectedHMDCount = ConnectedParticipantIds.Num();
	HostParticipantId = ConnectedParticipantIds.IsEmpty() ? 0 : ConnectedParticipantIds[0];
	DefaultPeerId.Reset();
	for (const TPair<FString, FHUICRSyncPeerRuntimeState>& Pair : Peers)
	{
		if (Pair.Value.Info.bConnected)
		{
			if (Pair.Value.Info.ParticipantId == HostParticipantId)
			{
				SetDefaultPeer(Pair.Key);
			}
			SendPresence(Pair.Value, false);
		}
	}
	RefreshPeerInfoSnapshot();
	SetConnected(ConnectedHMDCount > 0);
	UE_LOG(LogTemp, Log, TEXT("HUICRSync roster revision %d, Host=%d, connected HMDs=%d"), RosterRevision, HostParticipantId, ConnectedHMDCount);
	UpdateCalibrationParticipants();
	OnParticipantsChanged.Broadcast();
}

void UHUICRSyncSubsystem::ApplyHMDRoster(int32 Revision, const TArray<int32>& Participants)
{
	if (Revision <= RosterRevision || !Participants.Contains(LocalParticipantId) || Participants.IsEmpty())
	{
		return;
	}
	int32 PreviousId = 0;
	for (int32 Id : Participants)
	{
		if (Id <= PreviousId)
		{
			return;
		}
		PreviousId = Id;
	}
	RosterRevision = Revision;
	if (LocalParticipantId == Participants[0] && !bIsLocalHMDHost && bConnectionState)
	{
		// Do not transmit work queued by a Guest just because the headset is now Host.
		ClearSessionTraffic();
	}
	ConnectedParticipantIds = Participants;
	ConnectedHMDCount = Participants.Num();
	HostParticipantId = Participants[0];
	bIsLocalHMDHost = HostParticipantId == LocalParticipantId;
	SetConnected(true);
	UE_LOG(LogTemp, Log, TEXT("HUICRSync local participant=%d Host=%d connected HMDs=%d"), LocalParticipantId, HostParticipantId, ConnectedHMDCount);
	OnParticipantsChanged.Broadcast();
}

void UHUICRSyncSubsystem::RemoveParticipant(const FString& PeerId)
{
	FHUICRSyncPeerRuntimeState Removed;
	if (!Peers.RemoveAndCopyValue(PeerId, Removed))
	{
		return;
	}
	if (Role == EHUICRSyncRole::PC)
	{
		RetiredSessions.Add(Removed.RemoteSession);
		RevokeParticipantFields(Removed.Info.ParticipantId);
		if (HostParticipantId == Removed.Info.ParticipantId)
		{
			// A partially transmitted scene or calibration batch belongs only to the departing Host.
			ClearSessionTraffic();
			RemoteIpAddress.Reset();
		}
		ConnectedParticipantIds.Remove(Removed.Info.ParticipantId);
		UpdatePCParticipants();
	}
	else
	{
		ClearSessionTraffic();
		ResetCalibrationSession();
		DefaultPeerId.Reset();
		ConnectedParticipantIds.Reset();
		LocalParticipantId = HostParticipantId = ConnectedHMDCount = 0;
		bIsLocalHMDHost = false;
		RosterRevision = 0;
		RefreshPeerInfoSnapshot();
		ResetLocalSession();
		SetConnected(false);
		OnParticipantsChanged.Broadcast();
	}
}

void UHUICRSyncSubsystem::SendPresence(const FHUICRSyncPeerRuntimeState& Peer, bool bLeaving)
{
	if (!SenderSocket || !Peer.UDPAddr || !Peer.RemoteSession.IsValid())
	{
		return;
	}
	TArray<uint8> Buffer;
	FMemoryWriter Writer(Buffer, true);
	WriteSessionHeader(Writer, Peer);
	uint32 Magic = bLeaving ? HUICRSyncProtocol::LeaveMagic : HUICRSyncProtocol::PresenceMagic;
	Writer << Magic;
	if (!bLeaving && Role == EHUICRSyncRole::PC)
	{
		Writer << RosterRevision << ConnectedParticipantIds;
		WriteCalibrationStatus(Writer, Peer.Info.ParticipantId);
	}
	int32 Sent = 0;
	SenderSocket->SendTo(Buffer.GetData(), Buffer.Num(), Sent, *Peer.UDPAddr);
}

void UHUICRSyncSubsystem::TickPresence()
{
	const double Now = FPlatformTime::Seconds();
	const float Interval = FMath::Max(0.1f, HeartbeatIntervalSeconds);
	const float Timeout = FMath::Max(2.0f * Interval, PeerTimeoutSeconds);
	const bool bSendHeartbeat = Now - LastHeartbeatTime >= Interval;
	TArray<FString> ExpiredPeers;
	for (const TPair<FString, FHUICRSyncPeerRuntimeState>& Pair : Peers)
	{
		const FHUICRSyncPeerRuntimeState& Peer = Pair.Value;
		if (!Peer.Info.bConnected)
		{
			continue;
		}
		if (Now - Peer.Info.LastReceiveTime > Timeout)
		{
			ExpiredPeers.Add(Pair.Key);
		}
		else if (bSendHeartbeat)
		{
			SendPresence(Peer, false);
		}
	}
	if (bSendHeartbeat)
	{
		LastHeartbeatTime = Now;
	}
	for (const FString& Id : ExpiredPeers)
	{
		RemoveParticipant(Id);
	}
}

void UHUICRSyncSubsystem::SendLocalHeadPose(const FTransform& HeadPoseInPCSpace)
{
	FHUICRSyncPeerRuntimeState* Peer = FindDefaultPeer();
	if (Role != EHUICRSyncRole::HMD || !bConnectionState || !Peer || !Peer->UDPAddr || !SenderSocket || !HeadPoseInPCSpace.IsValid())
	{
		return;
	}
	TArray<uint8> Buffer;
	FMemoryWriter Writer(Buffer, true);
	WriteSessionHeader(Writer, *Peer);
	uint32 Magic = HUICRSyncProtocol::HeadPoseMagic;
	uint32 Sequence = ++PoseSequence;
	FTransform Pose = HeadPoseInPCSpace;
	Writer << Magic << Sequence << Pose;
	int32 Sent = 0;
	SenderSocket->SendTo(Buffer.GetData(), Buffer.Num(), Sent, *Peer->UDPAddr);
}
