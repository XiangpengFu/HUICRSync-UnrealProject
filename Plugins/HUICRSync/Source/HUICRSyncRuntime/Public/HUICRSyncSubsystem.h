#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "HUICRSyncTypes.h"
#include "HUICRSyncSubsystem.generated.h"

class FInternetAddr;
class FSocket;
class AHUICRSyncActor;
class AHUICRSyncPCManagerBase;
class AHUICRSyncHMDManagerBase;
class UPrimitiveComponent;
struct FIPv4Endpoint;
enum class EHUICRSyncFieldSource : uint8;

enum class EHUICRSyncSceneOp : uint8
{
	Reset,
	Spawn,
	Destroy,
	Ready,
	Field
};

struct FHUICRSyncSceneOperation
{
	EHUICRSyncSceneOp Kind = EHUICRSyncSceneOp::Reset;
	TArray<uint8> Bytes;
};

struct FHUICRSyncPeerRuntimeState
{
	FHUICRSyncPeerInfo Info;
	TSharedPtr<FInternetAddr> UDPAddr;
	TMap<uint32, FHUICRSyncPendingCommand> PendingReliableCommands;
	TSet<uint32> ReceivedCommandIds;
	FGuid RemoteSession;
	int32 SenderPort = 0;
	uint32 LastPoseSequence = 0;
	int32 LastCalibrationRequest = 0;
	int32 LastCompletedCalibrationRound = 0;
	FGuid AppliedLayout;
	FTransform HMDToPC = FTransform::Identity;
	bool bHasCalibrated = false;
	int32 SceneGeneration = 0;
	int32 ReadySceneGeneration = 0;
	int32 SceneSequence = 1;
	int32 SceneSendOffset = 0;
	bool bNeedsSceneSnapshot = false;
	double NextSceneSendTime = 0;
	TArray<FHUICRSyncSceneOperation> SceneOperations;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FHUICRSyncConnectionStateChanged, bool, bConnected);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FHUICRSyncCalibrationStateChanged, bool, bCalibrationState);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FHUICRSyncMotionParallaxStateChanged, bool, bMotionParallaxState);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FHUICRSyncSimpleEvent);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FHUICRSyncCommandReceived, const FString&, Address, const TArray<FHUICRSyncCommandArg>&, Args);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FHUICRSyncWorldChanged, int32, WorldID);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FHUICRSyncSelectedScreenChanged, int32, ScreenID);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FHUICRSyncTaskCounterReceived, int32, TaskCounter);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FiveParams(FHUICRSyncSceneRequestCompleted, int32, ParticipantId, int32, RequestId,
	bool, bSpawnRequest, bool, bSucceeded, int32, ActorId);

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

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Participants")
	int32 LocalParticipantId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Participants")
	int32 HostParticipantId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Participants")
	int32 ConnectedHMDCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Participants")
	bool bIsLocalHMDHost = false;

	/** Oldest connected headset first. Zero is reserved for no participant. */
	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Participants")
	TArray<int32> ConnectedParticipantIds;

	UPROPERTY(BlueprintAssignable, Category = "HUICR Sync|Events")
	FHUICRSyncSimpleEvent OnParticipantsChanged;

	UPROPERTY(BlueprintAssignable, Category = "HUICR Sync|Calibration")
	FHUICRSyncSimpleEvent OnCalibrationUIStateChanged;

	/** Broadcast after the managed session changes bGameStopped and updates local Tick and physics. */
	UPROPERTY(BlueprintAssignable, Category = "HUICR Sync|Calibration")
	FHUICRSyncSimpleEvent OnGameplayPauseStateChanged;

	/** First active participant in join order. Use ActiveCalibrationCount for parallel calibration. */
	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Calibration")
	int32 ActiveCalibratorId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Calibration", meta=(DeprecatedProperty, DeprecationMessage="Guest calibration is parallel. Use ActiveCalibrationCount."))
	int32 LocalQueuePosition = 0;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Calibration")
	int32 ActiveCalibrationCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Calibration")
	bool bHostCalibrationActive = false;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Calibration")
	int32 ScreenLayoutVersion = 0;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Calibration")
	TArray<int32> CalibrationScreenIds;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Calibration")
	bool bCanStartCalibration = false;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Calibration")
	bool bCanEditPCScreenLayout = false;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Calibration")
	bool bCanEditGameMotionParallax = false;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Calibration")
	bool bIsLocalHMDCalibrating = false;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Calibration")
	bool bLayoutUpdatedSinceMyCalibration = false;

	/** True once this headset's current scene has been applied and confirmed by the PC. */
	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Spawn")
	bool bLocalSceneReady = false;

	UPROPERTY(BlueprintAssignable, Category = "HUICR Sync|Spawn")
	FHUICRSyncSceneRequestCompleted OnSceneActorRequestCompleted;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Spawn")
	int32 LastSceneActorRequestId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Calibration")
	bool bWaitingForCalibrationResult = false;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Calibration")
	FString CalibrationMessage;

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Calibration")
	bool RequestCalibrationTurn();

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Calibration")
	bool CancelCalibrationTurn();

	void RegisterCalibrationManager(AHUICRSyncPCManagerBase* Manager);
	void RegisterCalibrationManager(AHUICRSyncHMDManagerBase* Manager);
	bool SubmitCalibration(const FHUICRSyncCalibrationLayout& LocalMeasurements);
	bool RequestCalibrationPreview(int32 ScreenID);
	bool UsesManagedCalibration() const { return bManagedCalibration; }
	bool RequestSyncFieldSource(AHUICRSyncActor* Actor, FName Field, EHUICRSyncFieldSource Source, const TArray<uint8>& SpecifiedValue);
	bool HasAppliedCalibration() const;
	void RestoreLocalCalibrationId(const FGuid& LayoutId);
	void SetCalibrationMessage(const FString& Message);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Network", meta=(ClampMin="0.1"))
	float HeartbeatIntervalSeconds = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Network", meta=(ClampMin="0.5"))
	float PeerTimeoutSeconds = 6.0f;

	void SendLocalHeadPose(const FTransform& HeadPoseInPCSpace);

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
	FString LocalSessionId;

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

	/** Supply a transform in the calling device's local world. The original Spawn Sync Actor accepts PC-space transforms. */
	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Spawn", meta = (AutoCreateRefTerm = "InitPayloadBytes"))
	bool SpawnSyncActorFromLocalSpace(UClass* SpawnClass, const FTransform& SpawnTransform, const TArray<uint8>& InitPayloadBytes);

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

	void BeginConnectionAttempt();

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

	FTransform ConvertHMDActorTransformToPC(const FTransform& Transform) const;
	FTransform ConvertPCActorTransformToHMD(const FTransform& Transform) const;
	const FHUICRSyncGuestActorMapping& GetGuestActorMapping() const { return GuestActorMapping; }
	void SetLocalCalibrationMapping(const FTransform& ToHMD, const FTransform& ToPC,
		const FHUICRSyncGuestActorMapping& GuestMapping);

private:
	friend class AHUICRSyncActor;
#if WITH_DEV_AUTOMATION_TESTS
	friend class FHUICRSyncCalibrationTest;
#endif
	TWeakObjectPtr<AHUICRSyncPCManagerBase> CalibrationPC;
	TWeakObjectPtr<AHUICRSyncHMDManagerBase> CalibrationHMD;
	FHUICRSyncCalibrationLayout CommittedCalibration;
	FHUICRSyncGuestActorMapping GuestActorMapping;
	struct FCalibrationTurn
	{
		int32 Round = 0;
		int32 LastPreviewSequence = 0;
		bool bCanEditLayout = false;
		FHUICRSyncCalibrationLayout SubmittedLayout;
		FTransform HMDToPC = FTransform::Identity;
		double ReplyDeadline = 0;
	};
	TMap<int32, FCalibrationTurn> CalibrationTurns;
	FGuid LocalAppliedLayoutId;
	int32 CalibrationRound = 0;
	int32 CalibrationRevision = 0;
	int32 CalibrationRequestSequence = 0;
	int32 CalibrationPreviewSequence = 0;
	double ClientSubmitDeadline = 0;
	bool bManagedCalibration = false;
	bool bCalibrationPaused = false;
	bool bHasStartedGame = false;
	bool bStartGameAfterCalibration = false;
	bool bPendingHostCalibrationReady = false;
	bool bAwaitingFirstSpawn = false;
	bool bInitialCalibrationSceneReady = false;
	int32 NotifiedLocalSceneGeneration = 0;
	int32 CalibrationKnownHost = 0;
	FGuid PendingLocalLayoutId;

	struct FPausedPhysics
	{
		TWeakObjectPtr<UPrimitiveComponent> Component;
		FVector LinearVelocity;
		FVector AngularVelocity;
		bool bWasAwake = false;
	};
	TArray<FPausedPhysics> PausedPhysics;
	TArray<TWeakObjectPtr<AHUICRSyncActor>> PausedActorTicks;

	bool HandleCalibrationCommand(FHUICRSyncPeerRuntimeState& Peer, const FHUICRSyncCommandPayload& Command);
	void WriteCalibrationStatus(FArchive& Writer, int32 RecipientId);
	void ReadCalibrationStatus(FArchive& Reader);
	void PublishCalibrationStatus();
	void TryGrantCalibrationTurn(int32 ParticipantId);
	void EndCalibrationTurn(int32 ParticipantId, bool bCommitted);
	void RefreshActiveCalibrations();
	void UpdateCalibrationParticipants();
	void TickCalibration();
	void ResetCalibrationSession();
	void SetCalibrationPaused(bool bPaused);
	void PauseManagedActor(AHUICRSyncActor& Actor);
	void NotifyGameplayPauseIfChanged(bool bWasStopped);
	void BeginManagedGameplay();
	void HandleManagedParallax(bool bEnabled);
	FHUICRSyncPeerRuntimeState* FindCalibrationParticipant(int32 ParticipantId);

	struct FManagedSceneActor
	{
		TSoftClassPtr<AHUICRSyncActor> RemoteClass;
		TArray<uint8> InitialPayload;
		int32 ExcludedParticipantId = 0;
		bool bSpawnedAtRuntime = false;
	};
	TMap<FString, FManagedSceneActor> ManagedSceneActors;
	struct FFieldDelivery
	{
		uint64 SentEpoch = 0;
		uint32 SentSequence = 0;
		double RetryAt = 0;
		bool bAcknowledged = false;
	};
	struct FSharedField
	{
		uint64 Epoch = 0;
		uint32 Sequence = 0;
		int32 Writer = INDEX_NONE;
		TArray<uint8> Value;
		TMap<int32, uint32> LastRequests;
		TMap<int32, FFieldDelivery> Deliveries;
		// On the writing HMD, the last sample remains pending until echoed by the PC.
		uint32 UploadSequence = 0;
		TArray<uint8> UploadValue;
		double RetryUploadAt = 0;
	};
	struct FFieldActorState
	{
		uint64 Instance = 0;
		TWeakObjectPtr<AHUICRSyncActor> Actor;
		TMap<FName, FSharedField> Fields;
	};
	struct FEarlyFieldRequest
	{
		TWeakObjectPtr<AHUICRSyncActor> Actor;
		FName Field;
		EHUICRSyncFieldSource Source;
		TArray<uint8> Value;
	};
	TArray<FEarlyFieldRequest> EarlyFieldRequests;
	TMap<int32, FFieldActorState> FieldActors;
	bool bFieldReceivePending = false;
	TMap<TWeakObjectPtr<AHUICRSyncActor>, TArray<FName>> PendingActorFieldEvents;
	bool bDispatchingActorFieldEvents = false;
	void DispatchActorFieldEvents();
	uint64 NextFieldVersion = 0;
	uint32 NextFieldRequest = 0;
	static constexpr uint32 FieldPacketMagic = 0x48554644; // HUFD
	FFieldActorState& EnsurePCFieldActor(AHUICRSyncActor& Actor);
	void SeedInitialFields(AHUICRSyncActor& Actor);
	void SetFieldWriter(AHUICRSyncActor& Actor, FName Field, FSharedField& State, int32 Writer, const TArray<uint8>* Value);
	void RevokeParticipantFields(int32 ParticipantId);
	void TickSyncFields();
	bool HandleFieldCommand(FHUICRSyncPeerRuntimeState& Peer, const FHUICRSyncCommandPayload& Command);
	bool ReceiveFieldPacket(FHUICRSyncPeerRuntimeState& Peer, FArchive& Reader);
	bool ReceiveFieldState(FArchive& Reader, bool bSnapshot, FHUICRSyncPeerRuntimeState* Peer);
	void QueueFieldSnapshot(FHUICRSyncPeerRuntimeState& Peer, const FFieldActorState& ActorState);
	void SendFieldPacket(FHUICRSyncPeerRuntimeState& Peer, uint8 Kind, const TArray<uint8>& Payload);
	TSet<FString> LocalOnlySceneActors;
	struct FManagedActorState
	{
		FHUICRSyncNetEntry Entry;
		uint64 Sequence = 0;
	};
	TMap<FString, FManagedActorState> LatestManagedActorStates;
	uint64 ManagedStateSequence = 0;
	TMap<int32, FManagedActorState> ReceivedManagedActorStates;
	TSet<int32> RemovedSceneActorIds;
	TMap<FString, TWeakObjectPtr<AHUICRSyncActor>> LocalSceneMirrors;
	TArray<uint8> IncomingSceneOperation;
	int32 IncomingSceneSequence = 0;
	int32 LastAppliedSceneOffset = 0;
	int32 LocalSceneGeneration = 0;
	int32 ExpectedSceneGeneration = 0;
	bool bManagedScenePrepared = false;
	bool bSpawningManagedActor = false;

	void PrepareManagedScene();
	void StartManagedSceneForPeer(FHUICRSyncPeerRuntimeState& Peer);
	void QueueManagedSceneSnapshot(FHUICRSyncPeerRuntimeState& Peer);
	void DiscardManagedSceneTransfer(FHUICRSyncPeerRuntimeState& Peer);
	void TickManagedScene();
	bool HandleManagedSceneCommand(FHUICRSyncPeerRuntimeState& Peer, const FHUICRSyncCommandPayload& Command);
	bool ApplyManagedSceneOperation(EHUICRSyncSceneOp Kind, const TArray<uint8>& Bytes);
	void ClearLocalSceneMirrors();
	void ResetManagedSceneConnection();
	bool RegisterManagedSceneMirror(AHUICRSyncActor* Actor, UClass* RemoteClass, const TArray<uint8>& InitialPayload, int32 ExcludedParticipantId = 0);
	void QueueManagedSceneMirror(FHUICRSyncPeerRuntimeState& Peer, AHUICRSyncActor& Actor, const FManagedSceneActor& Record);
	void UnregisterManagedSceneMirror(AHUICRSyncActor& Actor);
	bool SendManagedSpawnRequest(const TArray<uint8>& Payload);
	bool SendManagedDestroyRequest(EHUICRSyncActorType TypeCode, int32 ActorId);
	AHUICRSyncActor* SpawnSyncActorForPeers(UClass* SpawnClass, const FTransform& SpawnTransform,
		const TArray<uint8>& InitPayloadBytes, bool bSpawnOnPeer, int32 ExcludedParticipantId);

	FSocket* SenderSocket = nullptr;
	FSocket* ReceiverSocket = nullptr;
	TMap<FString, FHUICRSyncPeerRuntimeState> Peers;
	TSet<FGuid> RetiredSessions;
	FGuid LocalSession;
	int32 NextParticipantId = 1;
	int32 RosterRevision = 0;
	double LastHeartbeatTime = 0;
	uint32 PoseSequence = 0;

	bool EnsureSenderSocket();
	void ClearSessionTraffic();
	void ResetLocalSession();
	void WriteSessionHeader(FArchive& Writer, const FHUICRSyncPeerRuntimeState& Peer) const;
	void TickPresence();
	void SendPresence(const FHUICRSyncPeerRuntimeState& Peer, bool bLeaving);
	void RemoveParticipant(const FString& PeerId);
	void UpdatePCParticipants();
	void ApplyHMDRoster(int32 Revision, const TArray<int32>& Participants);
	void SetConnected(bool bConnected);
	FHUICRSyncPeerRuntimeState* AcceptHandshake(const FIPv4Endpoint& Sender, const FHUICRSyncCommandPayload& Hello);

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
	bool SendQueuedEntries(FHUICRSyncPeerRuntimeState& Peer, const TArray<FHUICRSyncNetEntry>& Entries, uint32 Magic, uint64 StateSequence = 0);
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
