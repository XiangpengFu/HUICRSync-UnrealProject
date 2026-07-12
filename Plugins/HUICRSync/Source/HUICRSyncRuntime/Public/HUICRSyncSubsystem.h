#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "HUICRSyncTypes.h"
#include "HUICRSyncSubsystem.generated.h"

class FInternetAddr;
class FSocket;
class AHUICRSyncActor;
struct FIPv4Endpoint;

struct FHUICRSyncPeerRuntimeState
{
	FHUICRSyncPeerInfo Info;
	TSharedPtr<FInternetAddr> UDPAddr;
	TMap<uint32, FHUICRSyncPendingCommand> PendingReliableCommands;
	TSet<uint32> ReceivedCommandIds;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FHUICRSyncConnectionStateChanged, bool, bConnected);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FHUICRSyncCalibrationStateChanged, bool, bCalibrationState);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FHUICRSyncMotionParallaxStateChanged, bool, bMotionParallaxState);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FHUICRSyncSimpleEvent);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FHUICRSyncCommandReceived, const FString&, Address, const TArray<FHUICRSyncCommandArg>&, Args);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FHUICRSyncWorldChanged, int32, WorldID);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FHUICRSyncSelectedScreenChanged, int32, ScreenID);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FHUICRSyncTaskCounterReceived, int32, TaskCounter);

UCLASS(BlueprintType) 
class HUICRSYNCRUNTIME_API UHUICRSyncSubsystem : public UGameInstanceSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	UHUICRSyncSubsystem();

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync")
	EHUICRSyncRole Role = EHUICRSyncRole::None;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync")
	bool bConnectionState = false;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync")
	bool bCalibrationState = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	bool bMotionParallaxState = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	bool bGameStopped = true;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync")
	int32 WorldID = 0;

	UPROPERTY(BlueprintReadWrite, Category = "HUICR Sync")
	float UDPLatency = 0.0f;

	UPROPERTY(BlueprintReadWrite, Category = "HUICR Sync|Transform")
	FTransform HMDToPCTransform;

	UPROPERTY(BlueprintReadWrite, Category = "HUICR Sync|Transform")
	FTransform PCToHMDTransform;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Network")
	FString LocalIpAddress;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Network")
	FString RemoteIpAddress;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Network")
	FString DefaultPeerId;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Network")
	TMap<FString, FHUICRSyncPeerInfo> PeerInfos;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Network")
	int32 LocalSyncPort = 4001;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Network")
	int32 RemoteSyncPort = 4001;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Network")
	int32 UDPReceiverPort = 4002;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Network")
	int32 MaxPacketBytes = 12000;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Network")
	int32 MaxSpawnPacketsPerTick = 4;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Command")
	int32 MaxCommandRetryCount = 5;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Command")
	float CommandAckTimeout = 0.08f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Command")
	int32 InitialReliableCommandSendCount = 1;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Data")
	TMap<EHUICRSyncActorType, FHUICRSyncNetInnerMapEntry> ReceivedDataMap;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Data")
	TArray<FHUICRSyncNetEntry> SpawnActorPackets;

	UPROPERTY(BlueprintAssignable, Category = "HUICR Sync|Events")
	FHUICRSyncConnectionStateChanged OnConnectionStateChanged;

	UPROPERTY(BlueprintAssignable, Category = "HUICR Sync|Events")
	FHUICRSyncCalibrationStateChanged OnCalibrationStateChanged;

	UPROPERTY(BlueprintAssignable, Category = "HUICR Sync|Events")
	FHUICRSyncMotionParallaxStateChanged OnMotionParallaxStateChanged;

	UPROPERTY(BlueprintAssignable, Category = "HUICR Sync|Events")
	FHUICRSyncSimpleEvent OnCalibrationReady;

	UPROPERTY(BlueprintAssignable, Category = "HUICR Sync|Events")
	FHUICRSyncSimpleEvent OnSyncSceneReady;

	UPROPERTY(BlueprintAssignable, Category = "HUICR Sync|Events")
	FHUICRSyncSimpleEvent OnPrepareNetDataSend;

	UPROPERTY(BlueprintAssignable, Category = "HUICR Sync|Events")
	FHUICRSyncSimpleEvent OnNetDataReceived;

	UPROPERTY(BlueprintAssignable, Category = "HUICR Sync|Events")
	FHUICRSyncSimpleEvent OnSpawnDataReceived;

	UPROPERTY(BlueprintAssignable, Category = "HUICR Sync|Events")
	FHUICRSyncCommandReceived OnCommandReceived;

	UPROPERTY(BlueprintAssignable, Category = "HUICR Sync|Events")
	FHUICRSyncWorldChanged OnWorldChanged;

	UPROPERTY(BlueprintAssignable, Category = "HUICR Sync|Events")
	FHUICRSyncSelectedScreenChanged OnSelectedScreenChanged;

	UPROPERTY(BlueprintAssignable, Category = "HUICR Sync|Events")
	FHUICRSyncTaskCounterReceived OnTaskCounterReceived;

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync")
	bool ConfigureSync(EHUICRSyncRole InRole, int32 InLocalSyncPort, int32 InRemoteSyncPort, const FString& InRemoteIpAddress);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|UDP")
	bool InitializeReceiverSocket();

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|UDP")
	bool InitializeSenderSocket(const FString& InRemoteIpAddress, int32 InRemoteSyncPort);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|UDP")
	void ReceiveData();

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|UDP")
	void QueueNetEntry(const FHUICRSyncNetEntry& Entry);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|UDP")
	void QueueScreenNetEntry(const FHUICRSyncNetEntry& Entry);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|UDP")
	void SendAllQueuedStates();

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Spawn")
	bool QueueSpawnRequest(UClass* SpawnClass, const FTransform& SpawnTransform, bool bSpawnBackOnSender);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Spawn")
	bool QueueSpawnMirror(UClass* SpawnClass, int32 ActorID, const FTransform& SpawnTransform, const TArray<uint8>& InitPayloadBytes);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Spawn", meta = (AutoCreateRefTerm = "InitPayloadBytes"))
	bool RequestSpawnSyncActor(UClass* SpawnClass, const FTransform& SpawnTransform, const TArray<uint8>& InitPayloadBytes, bool bSpawnBackOnRequester = true);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Spawn", meta = (AutoCreateRefTerm = "InitPayloadBytes"))
	AHUICRSyncActor* SpawnSyncActorAsAuthority(UClass* SpawnClass, const FTransform& SpawnTransform, const TArray<uint8>& InitPayloadBytes, bool bSpawnOnPeer = true);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Spawn", meta = (AutoCreateRefTerm = "InitPayloadBytes"))
	bool SpawnSyncActor(UClass* SpawnClass, const FTransform& SpawnTransform, const TArray<uint8>& InitPayloadBytes);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Spawn", meta = (DisplayName = "Spawn Sync Actor"))
	bool SpawnSyncActorSimple(UClass* SpawnClass, const FTransform& SpawnTransform);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Spawn")
	int32 StartInitialSpawnSync();

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Actor")
	bool RegisterSyncActor(AHUICRSyncActor* SyncActor);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Actor")
	void UnregisterSyncActor(AHUICRSyncActor* SyncActor);

	UFUNCTION(BlueprintPure, Category = "HUICR Sync|Actor")
	AHUICRSyncActor* FindSyncActor(EHUICRSyncActorType TypeCode, int32 ActorID) const;

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Actor")
	bool DestroySyncActor(EHUICRSyncActorType TypeCode, int32 ActorID);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Actor")
	int32 ClearLocalSyncActors();

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Command")
	bool SendReliableCommand(const FString& Address, const TArray<FHUICRSyncCommandArg>& Args);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Command")
	bool SendReliableCommandBool(const FString& Address, bool Value);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Command")
	bool SendReliableCommandInt(const FString& Address, int32 Value);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Command")
	bool SendReliableCommandFloat(const FString& Address, float Value);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Command")
	bool SendReliableCommandString(const FString& Address, const FString& Value);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Command")
	bool SendReliableCommandToPeer(const FString& PeerId, const FString& Address, const TArray<FHUICRSyncCommandArg>& Args);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Network")
	FString RegisterOrUpdatePeer(const FString& PeerId, EHUICRSyncRole PeerRole, const FString& IpAddress, int32 PeerSyncPort);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Network")
	bool SetDefaultPeer(const FString& PeerId);

	UFUNCTION(BlueprintPure, Category = "HUICR Sync|Network")
	TArray<FString> GetKnownPeerIds() const;

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Actor")
	int32 AllocateActorID(EHUICRSyncActorType TypeCode);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Actor")
	void ReserveActorID(EHUICRSyncActorType TypeCode, int32 ActorID);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync")
	void StopSync();

	UFUNCTION(BlueprintPure, Category = "HUICR Sync|Transform")
	FTransform ConvertHMDTransformToPC(const FTransform& InHMDTransform) const;

	UFUNCTION(BlueprintPure, Category = "HUICR Sync|Transform")
	FTransform ConvertPCTransformToHMD(const FTransform& InPCTransform) const;

private:
	FSocket* SenderSocket = nullptr;
	FSocket* ReceiverSocket = nullptr;
	TMap<FString, FHUICRSyncPeerRuntimeState> Peers;

	TArray<FHUICRSyncNetEntry> RegisteredStates;
	TArray<FHUICRSyncNetEntry> ScreenRegisteredStates;
	TArray<FHUICRSyncNetEntry> PendingSpawnPackets;
	int32 PendingSpawnSendIndex = 0;
	uint32 CurrentSpawnBatchId = 1;
	int32 CurrentSpawnPacketIndex = 0;
	bool bWaitingForInitialSpawnSync = false;

	uint32 NextCommandId = 1;
	TMap<EHUICRSyncActorType, int32> ActorIdCounters;

	UPROPERTY(Transient)
	TMap<FString, TObjectPtr<AHUICRSyncActor>> SpawnedSyncActors;

	FString ResolveLocalIpAddress() const;
	FString MakeActorKey(EHUICRSyncActorType TypeCode, int32 ActorID) const;
	FString MakePeerId(const FString& IpAddress, int32 PeerSyncPort) const;
	FHUICRSyncPeerRuntimeState* FindPeer(const FString& PeerId);
	const FHUICRSyncPeerRuntimeState* FindPeer(const FString& PeerId) const;
	FHUICRSyncPeerRuntimeState* FindDefaultPeer();
	FHUICRSyncPeerRuntimeState* FindPeerByEndpoint(const FIPv4Endpoint& Endpoint);
	bool BuildPeerUdpAddress(FHUICRSyncPeerRuntimeState& Peer);
	void RefreshPeerInfoSnapshot();
	uint32 GetCommandIdPrefix() const;
	bool SendReliableCommandPacket(FHUICRSyncPeerRuntimeState& Peer, uint32 CommandId, const FString& Address, const TArray<FHUICRSyncCommandArg>& Args);
	void SendCommandAck(FHUICRSyncPeerRuntimeState& Peer, uint32 CommandId);
	void TickReliableCommands();
	void DestroySockets();
	bool SendQueuedEntries(FHUICRSyncPeerRuntimeState& Peer, const TArray<FHUICRSyncNetEntry>& Entries, uint32 Magic);
	void ProcessSpawnActorPackets();
	bool HandleSpawnRequest(const FHUICRSyncNetEntry& Entry);
	bool HandleSpawnMirror(const FHUICRSyncNetEntry& Entry);
	bool DestroyLocalSyncActor(EHUICRSyncActorType TypeCode, int32 ActorID);
	UClass* LoadSyncActorClass(const FString& ClassPath) const;
	bool ResolveSpawnClasses(UClass* RequestedClass, UClass*& OutLocalSpawnClass, UClass*& OutRemoteSpawnClass) const;
	TSubclassOf<AHUICRSyncActor> GetRemoteCounterpartClass(UClass* LocalSyncActorClass) const;
	AHUICRSyncActor* SpawnSyncActorInternal(UClass* SpawnClass, int32 ActorID, const FTransform& SpawnTransform, const TArray<uint8>& InitPayloadBytes);
	void CompleteSyncSceneAsAuthority();
	void StartGameFromAuthority();
	void HandleCommandPayload(FHUICRSyncPeerRuntimeState& Peer, const FHUICRSyncCommandPayload& CommandPayload);
};
