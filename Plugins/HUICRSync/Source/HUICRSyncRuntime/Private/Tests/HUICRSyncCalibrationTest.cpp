#if WITH_DEV_AUTOMATION_TESTS

#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/PlatformProcess.h"
#include "HUICRSyncActor.h"
#include "HUICRSyncCalibrator_HMD.h"
#include "HUICRSyncCalibrationSaveGame.h"
#include "HUICRSyncHMDManagerBase.h"
#include "HUICRSyncPCManagerBase.h"
#include "HUICRSyncScreen.h"
#include "HUICRSyncSubsystem.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Misc/ScopeExit.h"
#include "Serialization/MemoryWriter.h"
#include "Sockets.h"
#include "UObject/Script.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHUICRSyncCalibrationTest, "HUICRSync.Network.CalibrationTurns",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHUICRSyncCalibrationTest::RunTest(const FString& Parameters)
{
	const FString WorldName = TEXT("HUICRCalibrationTest_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UPackage* TestPackage = CreatePackage(*(TEXT("/Temp/") + WorldName));
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, FName(*WorldName), TestPackage);
	if (!TestNotNull(TEXT("Test world"), World))
	{
		return false;
	}
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
#if WITH_EDITOR
	// The test drives the Blueprint events while the fixture world is outside a normal play session.
	FEditorScriptExecutionGuard ScriptExecution;
#endif
	TArray<UHUICRSyncSubsystem*> Sessions;
	ON_SCOPE_EXIT
	{
		for (UHUICRSyncSubsystem* Session : Sessions)
		{
			Session->StopSync();
		}
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
	};
	auto NewSession = [&Sessions]()
	{
		auto* Session = NewObject<UHUICRSyncSubsystem>(NewObject<UGameInstance>());
		Session->HeartbeatIntervalSeconds = 0.1f;
		Session->PeerTimeoutSeconds = 0.8f;
		Sessions.Add(Session);
		return Session;
	};
	auto WaitFor = [this, &Sessions](const TCHAR* Reason, TFunctionRef<bool()> Predicate,
		UHUICRSyncSubsystem* SkippedSession = nullptr)
	{
		const double Deadline = FPlatformTime::Seconds() + 4.0;
		do
		{
			for (auto* Session : Sessions)
			{
				if (Session != SkippedSession)
				{
					Session->Tick(0.01f);
				}
			}
			if (Predicate())
			{
				return true;
			}
			FPlatformProcess::Sleep(0.005f);
		}
		while (FPlatformTime::Seconds() < Deadline);
		AddError(Reason);
		return false;
	};
	const int32 Port = FMath::RandRange(20000, 43000);
	auto* PC = NewSession();
	auto* PCManager = World->SpawnActor<AHUICRSyncPCManagerBase>();
	PCManager->SyncSubsystem = PC;
	PCManager->LocalSyncPort = Port;
	PCManager->bAutoSpawnPCScreenDissolveBoxes = false;
	PCManager->bAutoSpawnPCCalibrators = false;
	PCManager->bInitializeNDisplayOnBeginPlay = false;
	const FVector NDisplayMainScale(1.0, 10.0, 5.625);
	const FVector PCCalibratorScale(2.0);
	PCManager->SetPCCalibratorScale(PCCalibratorScale);
	for (int32 Id = 0; Id < 3; ++Id)
	{
		auto* Component = NewObject<USceneComponent>(PCManager);
		Component->RegisterComponent();
		Component->SetWorldLocation(FVector(0, Id * 145, 90));
		if (Id == 0)
		{
			Component->SetWorldScale3D(NDisplayMainScale);
		}
		PCManager->RegisterPCScreenComponent(Id, Component, true);
	}
	TestTrue(TEXT("Main PC calibrator spawned"), PCManager->SpawnPCCalibrator(0));
	const FTransform InitialScreen1 = PCManager->PCScreenBindings[1].InitialTransform;
	auto* Viewpoint = NewObject<USceneComponent>(PCManager);
	Viewpoint->RegisterComponent();
	PCManager->DefaultViewportComponent = Viewpoint;
	PCManager->DefaultViewportOffsetFromMainScreen = FVector(-120, 0, 0);
	PCManager->DispatchBeginPlay();
	TestEqual(TEXT("First use starts with no committed layout"), PC->ScreenLayoutVersion, 0);
	const auto SharedActorType = EHUICRSyncActorType::HUIActor;
	TArray<uint8> InitialScenePayload;
	InitialScenePayload.Init(73, 3100);
	AHUICRSyncActor* InitialActor = World->SpawnActor<AHUICRSyncActor>();
	InitialActor->SetSyncActorID(0);
	InitialActor->ActorInitPayloadData = InitialScenePayload;
	PC->RegisterSyncActor(InitialActor);
	const TCHAR* SkeletonPath = TEXT("/HUICRSync/Blueprints/HUICRSyncActor/SkeletonActor/BP_HUICRSkeletonActor_PC.BP_HUICRSkeletonActor_PC_C");
	UClass* SkeletonClass = LoadClass<AActor>(nullptr, SkeletonPath);
	if (!TestNotNull(TEXT("PC Skeleton Actor Blueprint"), SkeletonClass))
	{
		return false;
	}
	const auto* MoveProperty = FindFProperty<FBoolProperty>(SkeletonClass, TEXT("Move?"));
	auto* Skeleton = World->SpawnActor<AActor>(SkeletonClass);
	auto* CalibrationReadyProbe = World->SpawnActor<AActor>(SkeletonClass);
	if (!TestNotNull(TEXT("Skeleton Move? property"), MoveProperty) ||
		!TestNotNull(TEXT("Skeleton Blueprint instance"), Skeleton) ||
		!TestNotNull(TEXT("Calibration-ready probe"), CalibrationReadyProbe))
	{
		return false;
	}
	// Mirror the Blueprint's bindings on this test session, which has no world GameInstance.
	FScriptDelegate CalibrationStateListener;
	CalibrationStateListener.BindUFunction(Skeleton, TEXT("OnCalibrationStateChanged_Event"));
	PC->OnCalibrationStateChanged.Add(CalibrationStateListener);
	FScriptDelegate SceneReadyListener;
	SceneReadyListener.BindUFunction(Skeleton, TEXT("OnSyncSceneReady_Event"));
	PC->OnSyncSceneReady.Add(SceneReadyListener);
	FScriptDelegate CalibrationReadyListener;
	CalibrationReadyListener.BindUFunction(CalibrationReadyProbe, TEXT("OnSyncSceneReady_Event"));
	PC->OnCalibrationReady.Add(CalibrationReadyListener);

	auto CreateHeadset = [&](int32 LocalPort)
	{
		auto* Manager = World->SpawnActor<AHUICRSyncHMDManagerBase>();
		Manager->LocalSyncPort = LocalPort;
		Manager->CalibrationSaveSlotName = TEXT("HUICRSync_Test_") + FGuid::NewGuid().ToString();
		Manager->SyncSubsystem = NewSession();
		Manager->SyncSubsystem->RegisterCalibrationManager(Manager);
		return Manager;
	};
	auto CreateScreen = [&](AHUICRSyncHMDManagerBase* HMD, int32 Id, const FTransform& Transform)
	{
		auto* Screen = World->SpawnActor<AHUICRSyncScreen>(AHUICRSyncScreen::StaticClass(), Transform);
		Screen->bAutoAssignScreenID = false;
		Screen->ChangeScreenID(Id);
		HMD->RegisterScreen(Screen);
		return Screen;
	};
	auto* A = CreateHeadset(Port + 1);
	auto* B = CreateHeadset(Port + 2);
	auto* C = CreateHeadset(Port + 3);
	auto* Host = A->SyncSubsystem;
	auto* Guest = B->SyncSubsystem;
	auto* SecondGuest = C->SyncSubsystem;
	const auto MakeEventProbe = [&](FHUICRSyncSimpleEvent& Event)
	{
		AActor* Probe = World->SpawnActor<AActor>(SkeletonClass);
		FScriptDelegate Listener;
		Listener.BindUFunction(Probe, TEXT("OnSyncSceneReady_Event"));
		Event.Add(Listener);
		return Probe;
	};
	AActor* PCPauseProbe = MakeEventProbe(PC->OnGameplayPauseStateChanged);
	AActor* HostPauseProbe = MakeEventProbe(Host->OnGameplayPauseStateChanged);
	AActor* HostSceneProbe = MakeEventProbe(Host->OnSyncSceneReady);
	AActor* GuestSceneProbe = MakeEventProbe(Guest->OnSyncSceneReady);
	const FTransform HostMain(FRotator(0, 0, 0), FVector(200, -35, 110));
	const FTransform GuestMain(FRotator(0, 40, 0), FVector(-270, 84, 140));
	auto* HostMainScreen = CreateScreen(A, 0, HostMain);
	HostMainScreen->bControlRemoteScreen = false;
	auto* HostSecondary = CreateScreen(A, 1, FTransform(FRotator(0, -30, 0), FVector(183, 270, 103), FVector(1, 2, 3)));
	HostSecondary->bControlRemoteScreen = true;
	CreateScreen(A, 2, FTransform(FRotator(0, 53, 0), FVector(300, -250, 110), FVector(1, 4, 2)))->bControlRemoteScreen = true;
	auto* GuestMainScreen = CreateScreen(B, 0, GuestMain);
	GuestMainScreen->bControlRemoteScreen = false;
	auto AddMainCalibrator = [&](AHUICRSyncHMDManagerBase* Manager, AHUICRSyncScreen* Main, const FVector& Scale)
	{
		auto* Calibrator = World->SpawnActor<AHUICRSyncCalibrator_HMD>();
		Calibrator->BoundScreen = Main;
		Manager->RegisterCalibrator(Calibrator);
		Calibrator->SetActorScale3D(Scale);
		return Calibrator;
	};
	AddMainCalibrator(A, HostMainScreen, FVector(0.4));
	const FVector GuestCalibratorScale(0.72, 0.8, 0.6);
	const FVector GuestScaleRatio = GuestCalibratorScale / PCCalibratorScale;
	AHUICRSyncCalibrator_HMD* GuestCalibrator = AddMainCalibrator(B, GuestMainScreen, GuestCalibratorScale);
	auto* WrongGuestScreen = CreateScreen(B, 1, FTransform(FVector(950, 1230, -100)));
	WrongGuestScreen->bControlRemoteScreen = true;
	CreateScreen(C, 0, FTransform(FRotator(0, -60, 0), FVector(85, 260, 106)));

	A->ConnectToPC(PC->LocalIpAddress, Port, false);
	if (!WaitFor(TEXT("Host received initial calibration permissions"), [&] { return Host->bCanStartCalibration; }))
	{
		return false;
	}
	TestTrue(TEXT("PC can send a GameStart command before the current state permits gameplay"),
		PC->SendReliableCommand(TEXT("/FromPC/GameStart"), {}));
	Host->ReceiveData();
	TestTrue(TEXT("An early GameStart command does not start a managed HMD"), Host->bGameStopped);
	TestFalse(TEXT("An early GameStart command does not notify scene ready"), MoveProperty->GetPropertyValue_InContainer(HostSceneProbe));
	B->ConnectToPC(PC->LocalIpAddress, Port, false);
	if (!WaitFor(TEXT("Guest received connection and current layout"), [&] { return Guest->UsesManagedCalibration(); }))
	{
		return false;
	}
	TestFalse(TEXT("Guest waits for initial Host layout"), Guest->bCanStartCalibration);
	C->ConnectToPC(PC->LocalIpAddress, Port, false);
	if (!WaitFor(TEXT("Second Guest connected"), [&] { return SecondGuest->UsesManagedCalibration(); }))
	{
		return false;
	}
	Host->RequestCalibrationTurn();
	if (!WaitFor(TEXT("Host received the exclusive layout turn"), [&]
		{ return Host->bCanEditPCScreenLayout && Guest->bHostCalibrationActive && SecondGuest->bHostCalibrationActive; }))
	{
		return false;
	}
	TestFalse(TEXT("Guest cannot start before the Host completes layout calibration"), Guest->RequestCalibrationTurn());
	TestFalse(TEXT("Entering first calibration leaves the already-stopped PC unchanged"), MoveProperty->GetPropertyValue_InContainer(PCPauseProbe));
	TestFalse(TEXT("Entering first calibration leaves the already-stopped Host unchanged"), MoveProperty->GetPropertyValue_InContainer(HostPauseProbe));
	const FTransform BeforeHostPreview = Viewpoint->GetComponentTransform();
	TestTrue(TEXT("Host can send a PC preview request"), Host->RequestCalibrationPreview(2));
	if (!WaitFor(TEXT("Host can move the public viewpoint"), [&]
		{ return !Viewpoint->GetComponentTransform().Equals(BeforeHostPreview); }))
	{
		return false;
	}
	A->ChangeCalibrationState();
	if (!WaitFor(TEXT("Host committed the layout and both Guests can request calibration"), [&]
		{ return Guest->bCanStartCalibration && SecondGuest->bCanStartCalibration && PC->ScreenLayoutVersion == 1 &&
			Host->bLocalSceneReady && !Host->bGameStopped; }))
	{
		return false;
	}
	TestTrue(TEXT("First scene ready enables Skeleton movement"), MoveProperty->GetPropertyValue_InContainer(Skeleton));
	TestTrue(TEXT("First Host commit notifies Calibration Ready listeners"), MoveProperty->GetPropertyValue_InContainer(CalibrationReadyProbe));
	TestTrue(TEXT("First start notifies the PC of its gameplay state change"), MoveProperty->GetPropertyValue_InContainer(PCPauseProbe));
	TestTrue(TEXT("First start notifies the Host of its gameplay state change"), MoveProperty->GetPropertyValue_InContainer(HostPauseProbe));
	TestTrue(TEXT("The managed state notifies the initial scene recipient"), MoveProperty->GetPropertyValue_InContainer(HostSceneProbe));
	TestFalse(TEXT("A Guest without the scene does not receive scene ready"), MoveProperty->GetPropertyValue_InContainer(GuestSceneProbe));
	AHUICRSyncActor* HostInitialMirror = Host->FindSyncActor(SharedActorType, 0);
	TestTrue(TEXT("The initial Host gets the whole split init payload"),
		IsValid(HostInitialMirror) && HostInitialMirror->ActorInitPayloadData == InitialScenePayload);
	FHUICRSyncNetEntry InitialActorState;
	InitialActorState.TypeCode = SharedActorType;
	InitialActorState.ActorID = 0;
	InitialActorState.PayloadBytes = { 37, 52, 61 };
	PC->QueueNetEntry(InitialActorState);
	PC->SendAllQueuedStates();
	AHUICRSyncActor* RemovedDuringJoin = PC->SpawnSyncActorAsAuthority(AHUICRSyncActor::StaticClass(), FTransform::Identity, {});
	if (!TestNotNull(TEXT("PC actor present when the Guest snapshot is taken"), RemovedDuringJoin))
	{
		return false;
	}
	const int32 RemovedDuringJoinId = RemovedDuringJoin->ActorID;
	MoveProperty->SetPropertyValue_InContainer(CalibrationReadyProbe, false);
	MoveProperty->SetPropertyValue_InContainer(PCPauseProbe, false);
	MoveProperty->SetPropertyValue_InContainer(HostPauseProbe, false);
	MoveProperty->SetPropertyValue_InContainer(HostSceneProbe, false);
	const FTransform Committed1 = PCManager->PCScreenBindings[1].ScreenComponent->GetComponentTransform();
	const FTransform Committed2 = PCManager->PCScreenBindings[2].ScreenComponent->GetComponentTransform();
	TestTrue(TEXT("Host used the calibrator ratio with a Guest online and main screen control disabled"),
		HostMainScreen->GetActorScale3D().Equals(NDisplayMainScale * 0.2));
	TestTrue(TEXT("Disabling main screen control kept the scale configured in nDisplay"),
		PCManager->PCScreenBindings[0].ScreenComponent->GetComponentScale().Equals(NDisplayMainScale));
	TestFalse(TEXT("Host changed the actual PC secondary screen"), Committed1.Equals(InitialScreen1));
	Guest->RequestCalibrationTurn();
	if (!WaitFor(TEXT("One Guest already calibrating leaves the other Guest's start enabled"), [&]
		{ return Guest->bIsLocalHMDCalibrating && Host->bGameStopped && SecondGuest->ActiveCalibrationCount == 1 &&
			SecondGuest->bCanStartCalibration; }))
	{
		return false;
	}
	TestTrue(TEXT("The first Guest pause notifies the PC"), MoveProperty->GetPropertyValue_InContainer(PCPauseProbe));
	TestTrue(TEXT("The first Guest pause notifies the Host"), MoveProperty->GetPropertyValue_InContainer(HostPauseProbe));
	MoveProperty->SetPropertyValue_InContainer(PCPauseProbe, false);
	MoveProperty->SetPropertyValue_InContainer(HostPauseProbe, false);
	SecondGuest->RequestCalibrationTurn();
	if (!WaitFor(TEXT("Both Guests calibrated in parallel"), [&]
		{ return Guest->bIsLocalHMDCalibrating && SecondGuest->bIsLocalHMDCalibrating && Host->ActiveCalibrationCount == 2; }))
	{
		return false;
	}
	TestTrue(TEXT("Both Guests leave the Host's start enabled"), Host->bCanStartCalibration);
	TestTrue(TEXT("Guest calibration paused the PC and all headsets"),
		PC->bGameStopped && Host->bGameStopped && Guest->bGameStopped && SecondGuest->bGameStopped);
	TestFalse(TEXT("Calibration begin disables the actual Skeleton Blueprint's movement"),
		MoveProperty->GetPropertyValue_InContainer(Skeleton));
	TestFalse(TEXT("Guest cannot alter the screen layout"), Guest->bCanEditPCScreenLayout);
	TestFalse(TEXT("Guest cannot alter the public motion parallax"), Guest->bCanEditGameMotionParallax);
	const bool InitialParallax = PC->bMotionParallaxState;
	B->ChangeMotionParallaxState(!InitialParallax);
	TestEqual(TEXT("Guest left the PC preference alone"), PC->bMotionParallaxState, InitialParallax);
	const FVector ExpectedCDP = PCManager->PCScreenBindings[0].ScreenComponent->GetComponentLocation() +
		PCManager->DefaultViewportOffsetFromMainScreen;
	TestTrue(TEXT("First Guest resets the public viewpoint to the CDP"), Viewpoint->GetComponentLocation().Equals(ExpectedCDP));
	TestFalse(TEXT("Guest cannot send a normal preview request"), Guest->RequestCalibrationPreview(2));
	TArray<FHUICRSyncCommandArg> DirectPreview;
	for (int32 Value : {2, 2, 10})
	{
		auto& Arg = DirectPreview.AddDefaulted_GetRef();
		Arg.Type = EHUICRSyncCommandArgType::Int32;
		Arg.IntValue = Value;
	}
	TestTrue(TEXT("A direct request bypasses the widget"), Guest->SendReliableCommand(TEXT("/HUICRSync/Calibration/Select"), DirectPreview));
	PC->ReceiveData();
	TestTrue(TEXT("PC rejects Guest preview requests and keeps CDP"), Viewpoint->GetComponentLocation().Equals(ExpectedCDP));
	B->ChangeCalibrationState();
	if (!WaitFor(TEXT("Guest completed while the other Guest continued"), [&]
		{ return !Guest->bIsLocalHMDCalibrating && SecondGuest->bIsLocalHMDCalibrating && PC->ActiveCalibrationCount == 1; }))
	{
		return false;
	}
	TestTrue(TEXT("Public view remains at CDP and game stays paused after one Guest finishes"),
		PC->bGameStopped && Viewpoint->GetComponentLocation().Equals(ExpectedCDP));
	TestFalse(TEXT("Skeleton waits while one Guest is still calibrating"), MoveProperty->GetPropertyValue_InContainer(Skeleton));
	TestFalse(TEXT("Intermediate calibration changes do not re-notify the PC pause state"), MoveProperty->GetPropertyValue_InContainer(PCPauseProbe));
	TestFalse(TEXT("Intermediate calibration changes do not re-notify the Host pause state"), MoveProperty->GetPropertyValue_InContainer(HostPauseProbe));
	if (!WaitFor(TEXT("Guest received the beginning of a split actor operation"), [&]
		{ return !Guest->IncomingSceneOperation.IsEmpty() && !Guest->bLocalSceneReady; }))
	{
		return false;
	}
	PC->Tick(0.01f);
	uint32 PendingSize = 0;
	bool bDroppedSceneDatagram = false;
	while (Guest->ReceiverSocket->HasPendingData(PendingSize))
	{
		TArray<uint8> Dropped;
		Dropped.SetNumUninitialized(FMath::Min(PendingSize, 65507u));
		int32 BytesRead = 0;
		if (!Guest->ReceiverSocket->Recv(Dropped.GetData(), Dropped.Num(), BytesRead))
		{
			break;
		}
		// The scene frame contains its routed command name. The next chunk must be retried by the PC.
		const ANSICHAR FrameAddress[] = "/HUICRSync/Scene/Frame";
		for (int32 Offset = 0; Offset + UE_ARRAY_COUNT(FrameAddress) <= BytesRead; ++Offset)
		{
			if (FMemory::Memcmp(Dropped.GetData() + Offset, FrameAddress, UE_ARRAY_COUNT(FrameAddress)) == 0)
			{
				bDroppedSceneDatagram = true;
				break;
			}
		}
	}
	TestTrue(TEXT("A middle scene datagram was dropped deliberately"), bDroppedSceneDatagram);
	TestFalse(TEXT("The Guest cannot be ready while a scene chunk is missing"), Guest->bLocalSceneReady);
	TestTrue(TEXT("PC can delete an actor while one Quest is still loading the scene"), PC->DestroySyncActor(SharedActorType, RemovedDuringJoinId));
	AHUICRSyncActor* AddedDuringJoin = PC->SpawnSyncActorAsAuthority(AHUICRSyncActor::StaticClass(), FTransform::Identity, { 14, 28 });
	if (!TestNotNull(TEXT("PC added a new actor during Guest synchronization"), AddedDuringJoin))
	{
		return false;
	}
	const int32 AddedDuringJoinId = AddedDuringJoin->ActorID;
	C->ChangeCalibrationState();
	if (!WaitFor(TEXT("All calibrated clients resumed only after the last Guest finished"), [&]
		{ return PC->ActiveCalibrationCount == 0 && !PC->bGameStopped && !Host->bGameStopped &&
			!Guest->bGameStopped && !SecondGuest->bGameStopped; }))
	{
		return false;
	}
	TestTrue(TEXT("The last Guest finishing restarts the actual Skeleton Blueprint"), MoveProperty->GetPropertyValue_InContainer(Skeleton));
	TestTrue(TEXT("PC receives the resume event after the last Guest"), MoveProperty->GetPropertyValue_InContainer(PCPauseProbe));
	TestTrue(TEXT("Host receives the resume event after the last Guest"), MoveProperty->GetPropertyValue_InContainer(HostPauseProbe));
	TestFalse(TEXT("A Host only receives initial scene ready once per connection"), MoveProperty->GetPropertyValue_InContainer(HostSceneProbe));
	TestTrue(TEXT("The late Guest notifies scene ready after its mirrors were applied"), MoveProperty->GetPropertyValue_InContainer(GuestSceneProbe));
	TestTrue(TEXT("Guest synchronization keeps the original Host mirror"), Host->FindSyncActor(SharedActorType, 0) == HostInitialMirror);
	TestFalse(TEXT("The Guest snapshot does not retain an actor deleted during synchronization"),
		IsValid(Guest->FindSyncActor(SharedActorType, RemovedDuringJoinId)));
	TestTrue(TEXT("The Guest applies a spawn that happened while synchronizing before reporting ready"),
		IsValid(Guest->FindSyncActor(SharedActorType, AddedDuringJoinId)));
	AHUICRSyncActor* GuestInitialMirror = Guest->FindSyncActor(SharedActorType, 0);
	TestTrue(TEXT("The late Guest gets the split initial payload and current PC state"),
		IsValid(GuestInitialMirror) && GuestInitialMirror->ActorInitPayloadData == InitialScenePayload &&
			GuestInitialMirror->ActorReceivePayloadData == InitialActorState.PayloadBytes);
	const TArray<uint8> RuntimePayload { 5, 7, 11 };
	const FTransform PCSpawnTransform(FRotator(0, 25, 0), FVector(135, 65, 120), FVector(1.5, 2.0, 0.8));
	AHUICRSyncActor* PCSpawned = PC->SpawnSyncActorAsAuthority(SkeletonClass, PCSpawnTransform, RuntimePayload);
	if (!TestNotNull(TEXT("PC spawned a synchronized runtime actor"), PCSpawned))
	{
		return false;
	}
	const int32 PCSpawnId = PCSpawned->ActorID;
	auto IsOnAllDevices = [&](int32 ActorId)
	{
		return IsValid(PC->FindSyncActor(SharedActorType, ActorId)) && IsValid(Host->FindSyncActor(SharedActorType, ActorId)) &&
			IsValid(Guest->FindSyncActor(SharedActorType, ActorId)) && IsValid(SecondGuest->FindSyncActor(SharedActorType, ActorId));
	};
	if (!WaitFor(TEXT("PC spawn is present on all headsets"), [&] { return IsOnAllDevices(PCSpawnId); }))
	{
		return false;
	}
	TestTrue(TEXT("PC and Quest spawn their paired Blueprint classes"),
		PCSpawned->GetClass() == SkeletonClass && Guest->FindSyncActor(SharedActorType, PCSpawnId)->GetClass() == PCSpawned->GetRemoteCounterpartClass());
	AHUICRSyncActor* GuestRuntimeMirror = Guest->FindSyncActor(SharedActorType, PCSpawnId);
	const FVector PCActorOffset = PCSpawnTransform.GetLocation() - PCManager->PCScreenBindings[0].ScreenComponent->GetComponentLocation();
	const auto ExpectedGuestActor = [&](const FVector& Ratio)
	{
		return FTransform(GuestMain.GetRotation() * PCSpawnTransform.GetRotation(),
			GuestMain.TransformPosition(PCActorOffset * Ratio), PCSpawnTransform.GetScale3D() * Ratio);
	};
	const FTransform GuestActorTransform = ExpectedGuestActor(GuestScaleRatio);
	TestTrue(TEXT("Guest spawned with both the center offset and size scaled around the main screen"),
		GuestRuntimeMirror->GetActorTransform().Equals(GuestActorTransform, 0.02));
	TestTrue(TEXT("Running Actor Blueprint conversions use the same scale as spawn"),
		GuestRuntimeMirror->ConvertPCTransformToHMD(PCSpawnTransform).Equals(GuestActorTransform, 0.02));
	TestTrue(TEXT("Guest particle Blueprint scale input sees the calibrator ratio"),
		Guest->PCToHMDTransform.GetScale3D().Equals(GuestScaleRatio));
	TestTrue(TEXT("Host still uses its original calibrator mapping"),
		Host->FindSyncActor(SharedActorType, PCSpawnId)->GetActorTransform().Equals(Host->ConvertPCTransformToHMD(PCSpawnTransform), 0.02));
	TestTrue(TEXT("Guest can convert a rotated and scaled actor back to the original PC transform"),
		GuestRuntimeMirror->ConvertHMDTransformToPC(GuestActorTransform).Equals(PCSpawnTransform, 0.02));
	const FVector UpdatedGuestScale(1.2, 1.0, 0.9);
	AHUICRSyncActor* OtherHeadsetMirror = Host->FindSyncActor(SharedActorType, PCSpawnId);
	for (const FVector& Scale : { UpdatedGuestScale, GuestCalibratorScale })
	{
		const int32 OldGeneration = Guest->NotifiedLocalSceneGeneration;
		Guest->RequestCalibrationTurn();
		if (!WaitFor(TEXT("Guest starts a scale recalibration with existing scene mirrors"), [&] { return Guest->bIsLocalHMDCalibrating; }))
		{
			return false;
		}
		TestNull(TEXT("Guest clears its old actor as soon as recalibration starts"), Guest->FindSyncActor(SharedActorType, PCSpawnId));
		TestFalse(TEXT("Quest scene is unavailable during its own calibration"), Guest->bLocalSceneReady);
		TestTrue(TEXT("Recalibrating a Guest preserves the PC and Host actors"),
			PC->FindSyncActor(SharedActorType, PCSpawnId) == PCSpawned && Host->FindSyncActor(SharedActorType, PCSpawnId) == OtherHeadsetMirror);
		const FVector PCMoveDuringCalibration = Scale.Equals(UpdatedGuestScale) ? FVector(45, 25, -10) : FVector::ZeroVector;
		PCSpawned->SetActorLocation(PCSpawnTransform.GetLocation() + PCMoveDuringCalibration);
		GuestCalibrator->SetActorScale3D(Scale);
		B->ChangeCalibrationState();
		if (!WaitFor(TEXT("Guest committed its new calibrator scale"), [&] { return !Guest->bIsLocalHMDCalibrating && !Guest->bGameStopped; }))
		{
			return false;
		}
		AHUICRSyncActor* RebuiltMirror = Guest->FindSyncActor(SharedActorType, PCSpawnId);
		FTransform ExpectedTransform = ExpectedGuestActor(Scale / PCCalibratorScale);
		ExpectedTransform.AddToTranslation(GuestMain.GetRotation().RotateVector(PCMoveDuringCalibration * (Scale / PCCalibratorScale)));
		TestTrue(TEXT("Guest gets a new actor with the current PC pose and latest calibration scale"),
			IsValid(RebuiltMirror) && RebuiltMirror != GuestRuntimeMirror && RebuiltMirror->GetActorTransform().Equals(ExpectedTransform, 0.02));
		TestTrue(TEXT("The rebuilt Quest scene reports readiness for a fresh generation"), Guest->NotifiedLocalSceneGeneration > OldGeneration);
		GuestRuntimeMirror = RebuiltMirror;

		TArray<uint8> DeletedActor;
		FMemoryWriter Writer(DeletedActor);
		uint8 Type = static_cast<uint8>(SharedActorType);
		int32 Id = PCSpawnId;
		Writer << Type << Id;
		TArray<FHUICRSyncCommandArg> StaleFrame;
		for (int32 Value : { OldGeneration, Guest->IncomingSceneSequence + 1,
			static_cast<int32>(EHUICRSyncSceneOp::Destroy), DeletedActor.Num() })
		{
			FHUICRSyncCommandArg& Arg = StaleFrame.AddDefaulted_GetRef();
			Arg.Type = EHUICRSyncCommandArgType::Int32;
			Arg.IntValue = Value;
		}
		FHUICRSyncCommandArg& Chunk = StaleFrame.AddDefaulted_GetRef();
		Chunk.Type = EHUICRSyncCommandArgType::String;
		Chunk.StringValue = FBase64::Encode(DeletedActor);
		const FHUICRSyncPeerRuntimeState* GuestPeer = PC->FindCalibrationParticipant(Guest->LocalParticipantId);
		TestTrue(TEXT("Simulate an old scene packet arriving after the rebuilt scene"), GuestPeer &&
			PC->SendReliableCommandToPeer(GuestPeer->Info.PeerId, TEXT("/HUICRSync/Scene/Frame"), StaleFrame));
		Guest->ReceiveData();
		TestTrue(TEXT("An old scene packet cannot delete a rebuilt actor"), Guest->FindSyncActor(SharedActorType, PCSpawnId) == RebuiltMirror);
	}
	TestTrue(TEXT("Host can request a spawn"), Host->SpawnSyncActorFromLocalSpace(AHUICRSyncActor::StaticClass(), FTransform::Identity, RuntimePayload));
	const int32 HostSpawnId = PCSpawnId + 1;
	if (!WaitFor(TEXT("Host spawn is present on all devices"), [&] { return IsOnAllDevices(HostSpawnId); }))
	{
		return false;
	}
	TestTrue(TEXT("Guest can request a spawn"), Guest->SpawnSyncActor(AHUICRSyncActor::StaticClass(), FTransform::Identity, RuntimePayload));
	const int32 GuestSpawnId = HostSpawnId + 1;
	if (!WaitFor(TEXT("Guest spawn is present on all devices"), [&] { return IsOnAllDevices(GuestSpawnId); }))
	{
		return false;
	}
	TestTrue(TEXT("Init payload is stored on runtime mirrors"),
		SecondGuest->FindSyncActor(SharedActorType, GuestSpawnId)->ActorInitPayloadData == RuntimePayload);
	TestTrue(TEXT("PC can delete an actor requested by the Guest"), PC->DestroySyncActor(SharedActorType, GuestSpawnId));
	TestTrue(TEXT("Host can request deletion"), Host->DestroySyncActor(SharedActorType, PCSpawnId));
	TestTrue(TEXT("Guest can request deletion"), Guest->DestroySyncActor(SharedActorType, HostSpawnId));
	if (!WaitFor(TEXT("All three deletions are reflected on every device"), [&]
		{
			for (UHUICRSyncSubsystem* Session : Sessions)
			{
				for (int32 Id : { PCSpawnId, HostSpawnId, GuestSpawnId })
				{
					if (IsValid(Session->FindSyncActor(SharedActorType, Id)))
					{
						return false;
					}
				}
			}
			return true;
		}))
	{
		return false;
	}
	TestTrue(TEXT("Guest can request a spawn without a local mirror"),
		Guest->RequestSpawnSyncActor(AHUICRSyncActor::StaticClass(), FTransform::Identity, RuntimePayload, false));
	const int32 ExcludedSpawnId = GuestSpawnId + 1;
	if (!WaitFor(TEXT("A scoped spawn reaches the PC and the other headsets"), [&]
		{
			return IsValid(PC->FindSyncActor(SharedActorType, ExcludedSpawnId)) && IsValid(Host->FindSyncActor(SharedActorType, ExcludedSpawnId)) &&
				IsValid(SecondGuest->FindSyncActor(SharedActorType, ExcludedSpawnId));
		}))
	{
		return false;
	}
	TestNull(TEXT("The requester is excluded from the scoped spawn"), Guest->FindSyncActor(SharedActorType, ExcludedSpawnId));
	const int32 PreviousGuestConnection = SecondGuest->LocalParticipantId;
	const FHUICRSyncGuestActorMapping PreviousGuestMapping = SecondGuest->GetGuestActorMapping();
	SecondGuest->StopSync();
	TestNull(TEXT("Disconnect clears the headset's previous scene mirrors"), SecondGuest->FindSyncActor(SharedActorType, 0));
	C->CalibrationSaveGame->GuestActorMapping = {};
	TestTrue(TEXT("Store a Guest calibration in the previous save format"), UGameplayStatics::SaveGameToSlot(
		C->CalibrationSaveGame, C->CalibrationSaveSlotName, C->CalibrationSaveUserIndex));
	TestTrue(TEXT("Load a Guest save with no actor mapping"), C->LoadCalibrationData());
	TestFalse(TEXT("Previous saves start with no Guest actor mapping"), SecondGuest->GetGuestActorMapping().IsValid());
	TestTrue(TEXT("A returning Guest reconnects without changing the live PC scene"), C->ConnectToPC(PC->LocalIpAddress, Port, false));
	if (!WaitFor(TEXT("The returning Guest applied a new local copy of the current scene"), [&]
		{ return SecondGuest->LocalParticipantId > PreviousGuestConnection && SecondGuest->bLocalSceneReady && !SecondGuest->bGameStopped; }))
	{
		return false;
	}
	TestTrue(TEXT("Reconnecting leaves the Host's original actor intact"), Host->FindSyncActor(SharedActorType, 0) == HostInitialMirror);
	TestTrue(TEXT("The returning Guest sees the current shared actors"),
		IsValid(SecondGuest->FindSyncActor(SharedActorType, AddedDuringJoinId)) && IsValid(SecondGuest->FindSyncActor(SharedActorType, ExcludedSpawnId)));
	TestTrue(TEXT("An older Guest save restores the same actor position and scale after reconnect"),
		SecondGuest->GetGuestActorMapping().IsValid() && SecondGuest->ConvertPCActorTransformToHMD(PCSpawnTransform).Equals(
			PreviousGuestMapping.ToHMD(PCSpawnTransform), 0.02));
	const int32 PreviousSceneGeneration = Host->NotifiedLocalSceneGeneration;
	PC->StartInitialSpawnSync();
	if (!WaitFor(TEXT("An explicit scene refresh reports a new ready generation"), [&]
		{ return Host->NotifiedLocalSceneGeneration > PreviousSceneGeneration && Guest->bLocalSceneReady && SecondGuest->bLocalSceneReady; }))
	{
		return false;
	}
	TestTrue(TEXT("Refreshing headset mirrors preserves the PC's authoritative actor"), PC->FindSyncActor(SharedActorType, 0) == InitialActor);
	TestFalse(TEXT("Guest-only turns do not notify Calibration Ready listeners"), MoveProperty->GetPropertyValue_InContainer(CalibrationReadyProbe));
	TestTrue(TEXT("Guest left PC secondary screen unchanged"), PCManager->PCScreenBindings[1].ScreenComponent->GetComponentTransform().Equals(Committed1));
	const FVector PCMainPosition = PCManager->PCScreenBindings[0].ScreenComponent->GetComponentLocation();
	const auto ExpectedGuestScreen = [&](const FTransform& PCScreen)
	{
		const FVector OffsetFromPCMain = PCScreen.GetLocation() - PCMainPosition;
		const FVector GuestOffset = GuestMain.GetRotation().RotateVector(OffsetFromPCMain * GuestScaleRatio);
		return FTransform(GuestMain.GetRotation() * PCScreen.GetRotation(),
			GuestMain.GetLocation() + GuestOffset, PCScreen.GetScale3D() * GuestScaleRatio);
	};
	const FTransform GuestScreen1 = ExpectedGuestScreen(Committed1);
	TestTrue(TEXT("Existing Guest secondary screen scales its center offset and size around the main screen"),
		WrongGuestScreen->GetActorTransform().Equals(GuestScreen1, 0.01));
	const auto* Added = B->RegisteredScreens.FindByPredicate([](const AHUICRSyncScreen* Screen) { return Screen->ScreenID == 2; });
	const FTransform GuestScreen2 = ExpectedGuestScreen(Committed2);
	TestTrue(TEXT("Missing Guest secondary screen was created with the scaled center offset and size"),
		Added && (*Added)->GetActorTransform().Equals(GuestScreen2, 0.01));
	TestTrue(TEXT("Guest main screen center stays at the measured main screen"),
		GuestMainScreen->GetActorLocation().Equals(GuestMain.GetLocation()));
	TestTrue(TEXT("Guest main screen applies the Guest-to-PC calibrator scale ratio"),
		GuestMainScreen->GetActorScale3D().Equals(NDisplayMainScale * GuestScaleRatio));
	TestTrue(TEXT("Guest calibration reloaded from its save"), B->LoadCalibrationData());
	TestTrue(TEXT("Guest's adjusted secondary screen position survived reload"),
		WrongGuestScreen->GetActorLocation().Equals(GuestScreen1.GetLocation()));
	TestTrue(TEXT("Guest's adjusted secondary screen size survived reload"),
		WrongGuestScreen->GetActorScale3D().Equals(GuestScreen1.GetScale3D()));
	TestEqual(TEXT("Guest did not create a new layout version"), PC->ScreenLayoutVersion, 1);

	const FTransform GameView(FRotator(0, -17, 0), FVector(-400, 73, 145));
	auto* HostPoseSource = World->SpawnActor<AHUICRSyncActor>();
	auto* GuestPoseSource = World->SpawnActor<AHUICRSyncActor>();
	HostPoseSource->TypeCode = GuestPoseSource->TypeCode = EHUICRSyncActorType::HMDPawn;
	HostPoseSource->ActorID = Host->LocalParticipantId;
	GuestPoseSource->ActorID = Guest->LocalParticipantId;
	TestTrue(TEXT("Host followed during gameplay"), PCManager->ApplyHMDTransformToDefaultViewport(GameView, HostPoseSource));
	TestFalse(TEXT("The Guest's PC avatar cannot steer the shared viewpoint"),
		PCManager->ApplyHMDTransformToDefaultViewport(FTransform::Identity, GuestPoseSource));
	TestTrue(TEXT("Guest tracking preserves the last Host viewpoint"), Viewpoint->GetComponentTransform().Equals(GameView));
	AHUICRSyncActor* HostMirrorBeforeCancel = Host->FindSyncActor(SharedActorType, 0);
	AHUICRSyncActor* GuestMirrorBeforeCancel = Guest->FindSyncActor(SharedActorType, 0);
	Host->RequestCalibrationTurn();
	if (!WaitFor(TEXT("Host could recalibrate"), [&] { return Host->bIsLocalHMDCalibrating; }))
	{
		return false;
	}
	TestNull(TEXT("A Host also clears its synchronized objects when entering recalibration"), Host->FindSyncActor(SharedActorType, 0));
	A->ChangeMotionParallaxState(false);
	if (!WaitFor(TEXT("Host turned off the game view while calibrating"), [&] { return !PC->bMotionParallaxState; }))
	{
		return false;
	}
	Host->CancelCalibrationTurn();
	if (!WaitFor(TEXT("Host cancelled calibration and reloaded its scene"), [&]
		{ return PC->ActiveCalibratorId == 0 && !Host->bIsLocalHMDCalibrating && Host->bLocalSceneReady; }))
	{
		return false;
	}
	TestTrue(TEXT("Cancelling calibration rebuilds the Host copy and preserves the Guest copy"),
		IsValid(Host->FindSyncActor(SharedActorType, 0)) && Host->FindSyncActor(SharedActorType, 0) != HostMirrorBeforeCancel &&
		Guest->FindSyncActor(SharedActorType, 0) == GuestMirrorBeforeCancel);
	TestTrue(TEXT("The fixed view is the last game camera, not the calibration preview"), Viewpoint->GetComponentTransform().Equals(GameView));
	FHUICRSyncCalibrationLayout SavedLayout;
	bool bSavedMotion = true;
	Viewpoint->SetWorldTransform(FTransform::Identity);
	TestTrue(TEXT("PC layout reloaded from save"), PCManager->LoadCommittedCalibration(SavedLayout, bSavedMotion));
	TestFalse(TEXT("Saved game motion stayed disabled"), bSavedMotion);
	TestTrue(TEXT("Fixed viewpoint reloaded from save"), Viewpoint->GetComponentTransform().Equals(GameView));

	Guest->RequestCalibrationTurn();
	SecondGuest->RequestCalibrationTurn();
	if (!WaitFor(TEXT("Both Guests began another calibration"), [&]
		{ return Guest->bIsLocalHMDCalibrating && SecondGuest->bIsLocalHMDCalibrating; }))
	{
		return false;
	}
	TestTrue(TEXT("Host can start while both Guests calibrate"), Host->RequestCalibrationTurn());
	if (!WaitFor(TEXT("Host and both Guests kept their separate calibration turns"), [&]
		{ return Host->bCanEditPCScreenLayout && Guest->bHostCalibrationActive && SecondGuest->ActiveCalibrationCount == 3; }))
	{
		return false;
	}
	TestTrue(TEXT("Host can select a preview while Guests calibrate"), Host->RequestCalibrationPreview(2));
	if (!WaitFor(TEXT("Host preview takes over the public camera"), [&]
		{ return !Viewpoint->GetComponentLocation().Equals(ExpectedCDP); }))
	{
		return false;
	}
	const FTransform HostPreview = Viewpoint->GetComponentTransform();
	HostSecondary->AddActorWorldOffset(FVector(0, 70, 0));
	B->ChangeCalibrationState();
	if (!WaitFor(TEXT("Guest completed while the Host and other Guest were still calibrating"), [&]
		{ return !Guest->bIsLocalHMDCalibrating && Host->bCanEditPCScreenLayout && SecondGuest->bIsLocalHMDCalibrating; }))
	{
		return false;
	}
	TestTrue(TEXT("Guest who stopped before Host save used the old layout"),
		WrongGuestScreen->GetActorTransform().Equals(GuestScreen1, 0.01));
	TestTrue(TEXT("Guest completion leaves the Host preview and game pause in place"),
		PC->bGameStopped && Viewpoint->GetComponentTransform().Equals(HostPreview));
	const auto FindSecondGuestScreen = [&]
	{
		return C->RegisteredScreens.FindByPredicate([](const AHUICRSyncScreen* Screen) { return Screen->ScreenID == 1; });
	};
	const auto* SecondGuestScreen = FindSecondGuestScreen();
	if (!TestTrue(TEXT("Second Guest has screen 1 from its previous calibration"), SecondGuestScreen && IsValid(*SecondGuestScreen)))
	{
		return false;
	}
	const FTransform SecondGuestOldScreen = (*SecondGuestScreen)->GetActorTransform();
	A->ChangeCalibrationState();
	if (!WaitFor(TEXT("Host saves new layout and older Guest is prompted while the other Guest continues"), [&]
		{ return !Host->bIsLocalHMDCalibrating && Guest->bLayoutUpdatedSinceMyCalibration &&
			SecondGuest->bIsLocalHMDCalibrating && SecondGuest->ScreenLayoutVersion == 2; }))
	{
		return false;
	}
	TestTrue(TEXT("Host completion returns to CDP while the other Guest is still calibrating"),
		PC->bGameStopped && Viewpoint->GetComponentLocation().Equals(ExpectedCDP));
	TestFalse(TEXT("Saved Host layout waits for the last Guest before notifying Calibration Ready"),
		MoveProperty->GetPropertyValue_InContainer(CalibrationReadyProbe));
	TestTrue(TEXT("Saving a new layout leaves the completed Guest's screens as they were"),
		WrongGuestScreen->GetActorTransform().Equals(GuestScreen1, 0.01));
	const FTransform CommittedSecondLayout = PCManager->PCScreenBindings[1].ScreenComponent->GetComponentTransform();
	C->ChangeCalibrationState();
	if (!WaitFor(TEXT("Guest that finished after the Host returned to gameplay"), [&]
		{ return !SecondGuest->bIsLocalHMDCalibrating && !SecondGuest->bGameStopped && !PC->bGameStopped; }))
	{
		return false;
	}
	SecondGuestScreen = FindSecondGuestScreen();
	TestTrue(TEXT("Guest who stopped after Host save applied the new secondary screen layout"),
		SecondGuestScreen && !(*SecondGuestScreen)->GetActorTransform().Equals(SecondGuestOldScreen));
	TestFalse(TEXT("Guest who finished with the latest layout does not get an update prompt"), SecondGuest->bLayoutUpdatedSinceMyCalibration);
	TestTrue(TEXT("Last Guest completion restores the fixed game viewpoint"), Viewpoint->GetComponentTransform().Equals(GameView));
	TestTrue(TEXT("Host recalibration notifies Calibration Ready listeners when everyone finishes"),
		MoveProperty->GetPropertyValue_InContainer(CalibrationReadyProbe));
	TestTrue(TEXT("The Skeleton moves again after the overlapping Host and Guest turns"), MoveProperty->GetPropertyValue_InContainer(Skeleton));

	Guest->RequestCalibrationTurn();
	Host->RequestCalibrationTurn();
	if (!WaitFor(TEXT("Guest and Host began the retry test"), [&]
		{ return Guest->bIsLocalHMDCalibrating && Host->bCanEditPCScreenLayout; }))
	{
		return false;
	}
	B->ChangeCalibrationState();
	PC->ReceiveData();
	HostSecondary->AddActorWorldOffset(FVector(0, -130, 0));
	A->ChangeCalibrationState();
	if (!WaitFor(TEXT("Host committed a new layout while the Guest result was still in transit"), [&]
		{ return PC->ScreenLayoutVersion == 3 && !Host->bIsLocalHMDCalibrating && PC->ActiveCalibrationCount == 1; }, Guest))
	{
		return false;
	}
	// Simulate a manual retry before the Guest has received the first result.
	Guest->bWaitingForCalibrationResult = false;
	B->ChangeCalibrationState();
	PC->ReceiveData();
	if (!WaitFor(TEXT("Retried guest Stop was confirmed"), [&]
		{ return !Guest->bIsLocalHMDCalibrating && !Guest->bGameStopped; }))
	{
		return false;
	}
	TestTrue(TEXT("Guest retry kept the original submitted layout even after a Host commit"),
		Guest->bLayoutUpdatedSinceMyCalibration && WrongGuestScreen->GetActorTransform().Equals(ExpectedGuestScreen(CommittedSecondLayout), 0.01));
	const FTransform CommittedThirdLayout = PCManager->PCScreenBindings[1].ScreenComponent->GetComponentTransform();

	Guest->RequestCalibrationTurn();
	SecondGuest->RequestCalibrationTurn();
	if (!WaitFor(TEXT("Both Guests began the disconnect test"), [&]
		{ return Guest->bIsLocalHMDCalibrating && SecondGuest->bIsLocalHMDCalibrating; }))
	{
		return false;
	}
	SecondGuest->StopSync();
	if (!WaitFor(TEXT("One Guest disconnect does not end the other Guest's calibration"), [&]
		{ return PC->ActiveCalibrationCount == 1 && PC->bGameStopped && Guest->bIsLocalHMDCalibrating; }))
	{
		return false;
	}
	Host->RequestCalibrationTurn();
	if (!WaitFor(TEXT("Host started a preview before disconnecting"), [&] { return Host->bCanEditPCScreenLayout; }))
	{
		return false;
	}
	Host->RequestCalibrationPreview(2);
	if (!WaitFor(TEXT("Public camera moved to the departing Host's preview"), [&]
		{ return !Viewpoint->GetComponentLocation().Equals(ExpectedCDP); }))
	{
		return false;
	}
	Host->StopSync();
	if (!WaitFor(TEXT("Guest became Host without losing its calibration turn"), [&]
		{ return Guest->bIsLocalHMDHost && Guest->bIsLocalHMDCalibrating && !Guest->bHostCalibrationActive; }))
	{
		return false;
	}
	TestFalse(TEXT("Promotion does not turn an active guest calibration into a layout edit"), Guest->bCanEditPCScreenLayout);
	TestTrue(TEXT("Host disconnect returns the public camera to CDP while the Guest continues"),
		PC->bGameStopped && Viewpoint->GetComponentLocation().Equals(ExpectedCDP));
	B->ChangeCalibrationState();
	if (!WaitFor(TEXT("The newly promoted Host finished the guest turn"), [&]
		{ return PC->ActiveCalibratorId == 0 && !PC->bGameStopped && !Guest->bGameStopped; }))
	{
		return false;
	}
	TestEqual(TEXT("Promotion preserved the committed layout version"), PC->ScreenLayoutVersion, 3);
	TestTrue(TEXT("Promotion preserved the committed PC screen pose"), PCManager->PCScreenBindings[1].ScreenComponent->GetComponentTransform().Equals(CommittedThirdLayout));
	const FTransform InheritedSecondary = WrongGuestScreen->GetActorTransform();
	const FTransform InheritedPCScreen2 = PCManager->PCScreenBindings[2].ScreenComponent->GetComponentTransform();
	WrongGuestScreen->bControlRemoteScreen = false;
	GuestMainScreen->bControlRemoteScreen = true;
	Guest->RequestCalibrationTurn();
	if (!WaitFor(TEXT("A single headset can now start a fresh Host turn"), [&] { return Guest->bCanEditPCScreenLayout; }))
	{
		return false;
	}
	TestTrue(TEXT("Starting as the new Host keeps the inherited HMD secondary screen in place"),
		WrongGuestScreen->GetActorTransform().Equals(InheritedSecondary, 0.01));
	TestTrue(TEXT("Starting as the new Host keeps the inherited PC secondary screen in place"),
		PCManager->PCScreenBindings[1].ScreenComponent->GetComponentTransform().Equals(CommittedThirdLayout));
	TestFalse(TEXT("The new Host inherits the previous Host's unchecked main screen"), GuestMainScreen->bControlRemoteScreen);
	TestTrue(TEXT("The new Host inherits the previous Host's checked secondary screen"), WrongGuestScreen->bControlRemoteScreen);
	B->ChangeCalibrationState();
	if (!WaitFor(TEXT("A single Host can submit and resume"), [&]
		{ return PC->ActiveCalibratorId == 0 && !Guest->bIsLocalHMDCalibrating && !Guest->bGameStopped; }))
	{
		return false;
	}
	TestEqual(TEXT("Submitting inherited screen controls without edits did not change the layout version"), PC->ScreenLayoutVersion, 3);
	TestTrue(TEXT("Submitting as the new Host did not reset either inherited PC secondary screen"),
		PCManager->PCScreenBindings[1].ScreenComponent->GetComponentTransform().Equals(CommittedThirdLayout) &&
		PCManager->PCScreenBindings[2].ScreenComponent->GetComponentTransform().Equals(InheritedPCScreen2));
	TestTrue(TEXT("Submitting as the new Host keeps the inherited HMD secondary pose and scale"),
		WrongGuestScreen->GetActorTransform().Equals(InheritedSecondary, 0.01));
	TestTrue(TEXT("Solo Host uses its own calibrator ratio with the PC-owned main screen"),
		GuestMainScreen->GetActorScale3D().Equals(NDisplayMainScale * GuestScaleRatio));

	Guest->RequestCalibrationTurn();
	if (!WaitFor(TEXT("The new Host started a turn to edit one screen"), [&] { return Guest->bCanEditPCScreenLayout; }))
	{
		return false;
	}
	WrongGuestScreen->AddActorWorldOffset(FVector(35, 70, 20));
	B->ChangeCalibrationState();
	if (!WaitFor(TEXT("The new Host could save its own screen change"), [&]
		{ return PC->ScreenLayoutVersion == 4 && !Guest->bIsLocalHMDCalibrating && !Guest->bGameStopped; }))
	{
		return false;
	}
	const FTransform EditedPCScreen = PCManager->PCScreenBindings[1].ScreenComponent->GetComponentTransform();
	TestFalse(TEXT("The checked PC screen changed"), EditedPCScreen.Equals(CommittedThirdLayout));
	TestTrue(TEXT("Editing one screen keeps the other inherited PC screen in place"),
		PCManager->PCScreenBindings[2].ScreenComponent->GetComponentTransform().Equals(InheritedPCScreen2));

	Guest->RequestCalibrationTurn();
	if (!WaitFor(TEXT("The Host can start a turn to change a checkbox"), [&] { return Guest->bCanEditPCScreenLayout; }))
	{
		return false;
	}
	WrongGuestScreen->ChangeIfControlRemoteScreen(false);
	Guest->CancelCalibrationTurn();
	if (!WaitFor(TEXT("Cancelling restores the saved checkbox"), [&]
		{ return !Guest->bIsLocalHMDCalibrating && WrongGuestScreen->bControlRemoteScreen; }))
	{
		return false;
	}
	Guest->RequestCalibrationTurn();
	if (!WaitFor(TEXT("The Host can start a new turn after cancelling"), [&] { return Guest->bCanEditPCScreenLayout; }))
	{
		return false;
	}
	WrongGuestScreen->ChangeIfControlRemoteScreen(false);
	B->ChangeCalibrationState();
	if (!WaitFor(TEXT("The checkbox-only submission completed"), [&] { return !Guest->bIsLocalHMDCalibrating && !Guest->bGameStopped; }))
	{
		return false;
	}
	TestEqual(TEXT("Changing a checkbox leaves the layout version unchanged"), PC->ScreenLayoutVersion, 4);
	TestTrue(TEXT("Unchecking a controlled screen keeps its current PC pose"),
		PCManager->PCScreenBindings[1].ScreenComponent->GetComponentTransform().Equals(EditedPCScreen));
	TestTrue(TEXT("The checkbox-only change survives loading the PC save"), PCManager->LoadCommittedCalibration(SavedLayout, bSavedMotion));
	const auto* SavedSecondary = SavedLayout.Screens.FindByPredicate([](const auto& Screen) { return Screen.ScreenID == 1; });
	TestTrue(TEXT("The saved screen carries the unchecked control state"), SavedSecondary && !SavedSecondary->bRemoteControlsLocalScreen);

	C->ConnectToPC(PC->LocalIpAddress, Port, false);
	if (!WaitFor(TEXT("Another Guest can calibrate with the new Host online"), [&] { return SecondGuest->bCanStartCalibration; }))
	{
		return false;
	}
	SecondGuest->RequestCalibrationTurn();
	if (!WaitFor(TEXT("The Guest got a calibration turn"), [&] { return SecondGuest->bIsLocalHMDCalibrating; }))
	{
		return false;
	}
	C->ChangeCalibrationState();
	if (!WaitFor(TEXT("The Guest completed before the Host disconnected"), [&]
		{ return !SecondGuest->bIsLocalHMDCalibrating && !SecondGuest->bGameStopped; }))
	{
		return false;
	}
	Guest->StopSync();
	if (!WaitFor(TEXT("The already calibrated Guest became Host"), [&]
		{ return SecondGuest->bIsLocalHMDHost && SecondGuest->bCanStartCalibration; }))
	{
		return false;
	}
	SecondGuest->RequestCalibrationTurn();
	if (!WaitFor(TEXT("The already calibrated Guest can start a Host turn"), [&] { return SecondGuest->bCanEditPCScreenLayout; }))
	{
		return false;
	}
	const auto* SecondHostScreen1 = C->RegisteredScreens.FindByPredicate([](const AHUICRSyncScreen* Screen) { return Screen->ScreenID == 1; });
	const auto* SecondHostScreen2 = C->RegisteredScreens.FindByPredicate([](const AHUICRSyncScreen* Screen) { return Screen->ScreenID == 2; });
	TestTrue(TEXT("The next Host inherits the updated unchecked secondary"), SecondHostScreen1 && !(*SecondHostScreen1)->bControlRemoteScreen);
	TestTrue(TEXT("The next Host still inherits the other checked secondary"), SecondHostScreen2 && (*SecondHostScreen2)->bControlRemoteScreen);
	if (SecondHostScreen1)
	{
		(*SecondHostScreen1)->ChangeIfControlRemoteScreen(true);
	}
	C->ChangeCalibrationState();
	if (!WaitFor(TEXT("The second handoff Host can finish a turn"), [&]
		{ return !SecondGuest->bIsLocalHMDCalibrating && !SecondGuest->bGameStopped; }))
	{
		return false;
	}
	TestEqual(TEXT("Handoff to an already calibrated Guest preserved the layout version"), PC->ScreenLayoutVersion, 4);
	TestTrue(TEXT("Checking a screen without moving it preserved both PC secondary poses"),
		PCManager->PCScreenBindings[1].ScreenComponent->GetComponentTransform().Equals(EditedPCScreen) &&
		PCManager->PCScreenBindings[2].ScreenComponent->GetComponentTransform().Equals(InheritedPCScreen2));
	return !HasAnyErrors();
}

#endif
