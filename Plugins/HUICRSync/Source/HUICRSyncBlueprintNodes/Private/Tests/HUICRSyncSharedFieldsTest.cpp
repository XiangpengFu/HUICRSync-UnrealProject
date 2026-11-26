#if WITH_DEV_AUTOMATION_TESTS

#include "HUICRSyncFieldTestActor.h"
#include "HUICRSyncSpawnNodeTest.h"
#include "HUICRSyncFields.h"
#include "HUICRSyncHMDManagerBase.h"
#include "HUICRSyncPayloadLibrary.h"
#include "HUICRSyncPCManagerBase.h"
#include "HUICRSyncScreen.h"
#include "HUICRSyncSubsystem.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/PlatformProcess.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/AutomationTest.h"
#include "Misc/Crc.h"
#include "Misc/ScopeExit.h"
#include "UObject/Script.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHUICRSyncSharedFieldsTest, "HUICRSync.Network.SharedFields",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHUICRSyncSharedFieldsTest::RunTest(const FString& Parameters)
{
	const FString MapName = TEXT("HUICRFields_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	auto* Package = CreatePackage(*(TEXT("/Temp/") + MapName));
	auto* World = UWorld::CreateWorld(EWorldType::Game, false, *MapName, Package);
	if (!TestNotNull(TEXT("Field test world"), World)) { return false; }
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	FEditorScriptExecutionGuard AllowActorEvents;
	auto* PayloadType = FHUICRSyncScreenPayload::StaticStruct();
	TArray<UHUICRSyncSubsystem*> Sessions;
	TArray<FString> SaveSlots;
	ON_SCOPE_EXIT
	{
		for (auto* Session : Sessions) { Session->StopSync(); }
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
		for (const FString& Slot : SaveSlots)
		{
			if (UGameplayStatics::DoesSaveGameExist(Slot, 0)) { UGameplayStatics::DeleteGameInSlot(Slot, 0); }
		}
	};
	auto NewSession = [&]()
	{
		auto* Result = NewObject<UHUICRSyncSubsystem>(NewObject<UGameInstance>());
		Result->HeartbeatIntervalSeconds = 0.1f;
		Sessions.Add(Result);
		return Result;
	};
	auto WaitFor = [&](const TCHAR* Description, TFunctionRef<bool()> IsReady)
	{
		const double End = FPlatformTime::Seconds() + 5.0;
		do
		{
			for (auto* Session : Sessions) { Session->Tick(.01f); }
			if (IsReady()) { return true; }
			FPlatformProcess::Sleep(.005f);
		} while (FPlatformTime::Seconds() < End);
		AddError(Description);
		return false;
	};
	const int32 Port = FMath::RandRange(21000, 40500);
	auto* PC = NewSession();
	auto* PCManager = World->SpawnActor<AHUICRSyncPCManagerBase>();
	PCManager->SyncSubsystem = PC;
	PCManager->LocalSyncPort = Port;
	PCManager->bInitializeNDisplayOnBeginPlay = false;
	PCManager->bAutoSpawnPCScreenDissolveBoxes = false;
	PCManager->bAutoSpawnPCCalibrators = false;
	auto* Main = NewObject<USceneComponent>(PCManager);
	Main->RegisterComponent();
	PCManager->RegisterPCScreenComponent(0, Main, true);
	PCManager->DispatchBeginPlay();
	SaveSlots.Add(FString::Printf(TEXT("HUICRSync_PC_%08x"), FCrc::StrCrc32(*(Package->GetName() + TEXT("|") + PCManager->GetClass()->GetPathName()))));
	auto NewHMD = [&](int32 Offset)
	{
		auto* Manager = World->SpawnActor<AHUICRSyncHMDManagerBase>();
		Manager->LocalSyncPort = Port + Offset;
		Manager->CalibrationSaveSlotName = MapName + FString::FromInt(Offset);
		SaveSlots.Add(Manager->CalibrationSaveSlotName);
		Manager->SyncSubsystem = NewSession();
		Manager->SyncSubsystem->RegisterCalibrationManager(Manager);
		auto* Screen = World->SpawnActor<AHUICRSyncScreen>();
		Screen->bAutoAssignScreenID = false;
		Screen->ChangeScreenID(0);
		Manager->RegisterScreen(Screen);
		return Manager;
	};
	auto Calibrate = [&](AHUICRSyncHMDManagerBase* Manager)
	{
		auto* Session = Manager->SyncSubsystem;
		if (!WaitFor(TEXT("Can start physical alignment"), [&] { return Session->bCanStartCalibration; }) ||
			!Session->RequestCalibrationTurn() ||
			!WaitFor(TEXT("Received alignment turn"), [&] { return Session->bIsLocalHMDCalibrating; }))
		{
			return false;
		}
		Manager->ChangeCalibrationState();
		return WaitFor(TEXT("Headset finished alignment and received its scene"), [&]
			{ return !Session->bGameStopped && Session->bLocalSceneReady && !Session->bIsLocalHMDCalibrating; });
	};
	const FName Motion = GET_MEMBER_NAME_CHECKED(FHUICRSyncScreenPayload, Transform);
	const FName Color = GET_MEMBER_NAME_CHECKED(FHUICRSyncScreenPayload, ScreenID); // A separate scalar, equivalent to a color selection.
	auto Sample = [&](double X, int32 ColorIndex)
	{
		auto SampleValue = FInstancedStruct::Make<FHUICRSyncScreenPayload>();
		auto& Value = SampleValue.GetMutable<FHUICRSyncScreenPayload>();
		Value.Transform = FTransform(FVector(X, 0, 0));
		Value.ScreenID = ColorIndex;
		return SampleValue;
	};
	auto IsState = [](AHUICRSyncActor* Actor, double X, int32 ColorIndex)
	{
		FInstancedStruct Payload;
		TArray<FName> Mask;
		if (!IsValid(Actor) || !Actor->ReadReceivedPayloadInternal(Payload, Mask)) { return false; }
		const auto& Value = Payload.Get<FHUICRSyncScreenPayload>();
		return FMath::IsNearlyEqual(Value.Transform.GetLocation().X, X) && Value.ScreenID == ColorIndex;
	};
	const auto Follow = EHUICRSyncFieldSource::FollowMyPayload;
	const auto Fixed = EHUICRSyncFieldSource::UseSpecifiedValue;
	auto* Ball = World->SpawnActor<AHUICRSyncFieldTestActor>();
	Ball->SyncSubsystem = PC;
	Ball->SetSyncActorID(14);
	if (!TestTrue(TEXT("The placed actor can set its initial payload in construction"), Ball->bSetConstructionDefault))
	{
		return false;
	}
	PC->RegisterSyncActor(Ball);
	auto Mirror = [Ball](AHUICRSyncHMDManagerBase* Manager)
	{
		return Cast<AHUICRSyncFieldTestActor>(Manager->SyncSubsystem->FindSyncActor(Ball->TypeCode, Ball->ActorID));
	};
	auto Connect = [&](AHUICRSyncHMDManagerBase* Manager)
	{
		return Manager->ConnectToPC(PC->LocalIpAddress, Port, false);
	};

	auto* A = NewHMD(1);
	auto* B = NewHMD(2);
	if (!TestTrue(TEXT("PC chooses an autonomous motion source before the first player joins"),
		Ball->SetSyncFieldSourceInternal(Motion, Follow, {})) ||
		!Connect(A) || !Calibrate(A))
	{
		return false;
	}
	if (!TestTrue(TEXT("First mirror has the initial values before any outgoing sample"), IsState(Mirror(A), 10, 1)))
	{
		return false;
	}

	TArray<uint8> ExplicitBytes;
	if (!TestTrue(TEXT("The caller can still supply byte-based spawn values"),
		UHUICRSyncPayloadLibrary::WriteInstancedStruct(ExplicitBytes, PayloadType, Sample(41, 9))))
	{
		return false;
	}
	auto* Spawned = Cast<AHUICRSyncFieldTestActor>(PC->SpawnSyncActorAsAuthority(
		AHUICRSyncFieldTestActor::StaticClass(), FTransform::Identity, ExplicitBytes));
	if (!TestNotNull(TEXT("Runtime actor spawned with caller-supplied initial values"), Spawned) ||
		!TestTrue(TEXT("PC construction sees and preserves the caller's initial values"),
			Spawned->InitialBytesAtConstruction == ExplicitBytes && Spawned->ActorInitPayloadData == ExplicitBytes &&
			!Spawned->bSetConstructionDefault && IsState(Spawned, 41, 9)) ||
		!WaitFor(TEXT("First mirror construction and initial shared fields match caller-supplied values"), [&]
		{
			auto* Other = Cast<AHUICRSyncFieldTestActor>(A->SyncSubsystem->FindSyncActor(Spawned->TypeCode, Spawned->ActorID));
			return Other && Other->InitialBytesAtConstruction == ExplicitBytes && !Other->bSetConstructionDefault &&
				IsState(Other, 41, 9);
		}))
	{
		return false;
	}
	auto* FromDefaults = Cast<AHUICRSyncFieldTestActor>(PC->SpawnSyncActorAsAuthority(
		AHUICRSyncFieldTestActor::StaticClass(), FTransform::Identity, {}));
	if (!TestNotNull(TEXT("Runtime actor spawned without explicit init"), FromDefaults) ||
		!TestTrue(TEXT("Empty spawn input uses the runtime actor's construction default"), FromDefaults->bSetConstructionDefault) ||
		!WaitFor(TEXT("Runtime actor's authored default reaches the first mirror"), [&]
		{ return IsState(A->SyncSubsystem->FindSyncActor(FromDefaults->TypeCode, FromDefaults->ActorID), 10, 1); }))
	{
		return false;
	}
	const int32 PCEventsBeforeLocalMotion = Ball->ReceiveEventCount;
	const int32 PreparesBeforeLocalMotion = Ball->PrepareEventCount;
	Ball->PayloadAtPrepare = Sample(12, 99);
	PC->Tick(.01f);
	TestEqual(TEXT("PC prepares once when it is the motion source"), Ball->PrepareEventCount, PreparesBeforeLocalMotion + 1);
	TestTrue(TEXT("PC samples the newly prepared motion in that same tick, preserving fixed color"),
		Ball->LastPrepareFields.Num() == 1 && Ball->LastPrepareFields.Contains(Motion) && IsState(Ball, 12, 1));
	if (!WaitFor(TEXT("PC drives motion; initial scalar is preserved on A"), [&] { return IsState(Mirror(A), 12, 1); }))
	{
		return false;
	}
	TestEqual(TEXT("PC does not notify its own continuous motion source"), Ball->ReceiveEventCount, PCEventsBeforeLocalMotion);
	auto* BallA = Mirror(A);
	TestEqual(TEXT("A does not prepare while the PC drives the ball"), BallA->PrepareEventCount, 0);
	BallA->PayloadAtPrepare = Sample(22, 8);
	if (!TestTrue(TEXT("First headset takes motion"), BallA->SetSyncFieldSourceInternal(Motion, Follow, {})) ||
		!WaitFor(TEXT("PC receives only A's motion"), [&] { return IsState(Ball, 22, 1); }) ||
		!Connect(B) || !Calibrate(B) ||
		!WaitFor(TEXT("Late guest restores the two independently confirmed fields"), [&] { return IsState(Mirror(B), 22, 1); }))
	{
		return false;
	}
	auto* BallB = Mirror(B);
	if (!TestTrue(TEXT("Late guest keeps birth payload separate from the newer motion snapshot"),
		BallB->ActorInitPayloadData == Ball->ActorInitPayloadData &&
		BallB->InitialBytesAtConstruction == Ball->ActorInitPayloadData))
	{
		return false;
	}
	auto* OtherB = Cast<AHUICRSyncFieldTestActor>(B->SyncSubsystem->FindSyncActor(Spawned->TypeCode, Spawned->ActorID));
	if (!TestNotNull(TEXT("A second independent actor exists on B"), OtherB) ||
		!WaitFor(TEXT("Both late-join mirrors can read their snapshot inside their own events"), [&]
		{ return BallB->EventReadState(22, 1) && OtherB->EventReadState(41, 9); }))
	{
		return false;
	}
	if (!HUICRSyncSpawnNodeTest::Verify(*this, *World, *PC, *A->SyncSubsystem, *B->SyncSubsystem, WaitFor))
	{
		return false;
	}
	const int32 LocalEvents = BallA->ReceiveEventCount;
	const int32 RemoteEvents = BallB->ReceiveEventCount;
	const int32 UnrelatedEvents = OtherB->ReceiveEventCount;
	const int32 PCPreparesWhileRelaying = Ball->PrepareEventCount;
	BallA->PayloadAtPrepare = Sample(24, 4);
	if (!WaitFor(TEXT("Only PC and B need to apply A's new motion"), [&]
		{ return Ball->EventReadState(24, 1) && BallB->EventReadState(24, 1) && IsState(BallA, 24, 1); }))
	{
		return false;
	}
	TestEqual(TEXT("A's own confirmed motion does not fire its receive event"), BallA->ReceiveEventCount, LocalEvents);
	TestEqual(TEXT("One motion update notifies B's ball once"), BallB->ReceiveEventCount, RemoteEvents + 1);
	TestEqual(TEXT("B's other actor did not receive the ball's event"), OtherB->ReceiveEventCount, UnrelatedEvents);
	TestTrue(TEXT("A's ball prepares only motion"), BallA->LastPrepareFields.Num() == 1 && BallA->LastPrepareFields.Contains(Motion));
	TestEqual(TEXT("PC can relay A's motion without preparing that ball"), Ball->PrepareEventCount, PCPreparesWhileRelaying);
	TestEqual(TEXT("The unrelated PC actor never prepared"), Spawned->PrepareEventCount, 0);
	TestEqual(TEXT("An unrelated mirror never prepared"), OtherB->PrepareEventCount, 0);
	const int32 APreparesBeforeDisable = BallA->PrepareEventCount;
	BallA->bSendDataToPeer = false;
	A->SyncSubsystem->Tick(.01f);
	TestEqual(TEXT("Disabling this actor's send option skips its prepare event"), BallA->PrepareEventCount, APreparesBeforeDisable);
	BallA->bSendDataToPeer = true;
	BallB->PayloadAtPrepare = Sample(30, 7);
	BallB->SetSyncFieldSourceInternal(Color, Fixed, Sample(900, 2));
	BallA->PayloadAtPrepare = Sample(25, 4);
	if (!WaitFor(TEXT("A moves while guest B independently sets color"), [&]
		{ return IsState(Ball, 25, 2) && IsState(BallA, 25, 2) && BallB->EventReadState(25, 2) &&
			BallA->AppliedColorIndex == 2 && BallB->AppliedColorIndex == 2; }))
	{
		return false;
	}
	TestEqual(TEXT("Setting color once while A moves the ball never triggers preparation on B"), BallB->PrepareEventCount, 0);
	FInstancedStruct State;
	TArray<FName> Mask;
	BallA->ReadReceivedPayloadInternal(State, Mask);
	TestFalse(TEXT("An echo cannot move the local motion writer"), Mask.Contains(Motion));
	const int32 EventsBeforeBatch = Spawned->ReceiveEventCount;
	Spawned->SetSyncFieldSourceInternal(Motion, Fixed, Sample(52, 0));
	Spawned->SetSyncFieldSourceInternal(Color, Fixed, Sample(0, 3));
	if (!WaitFor(TEXT("A local fixed-value batch is readable inside the PC actor's event"), [&]
		{ return Spawned->EventReadState(52, 3) && Spawned->LastEventFields.Contains(Motion) &&
			Spawned->LastEventFields.Contains(Color); }))
	{
		return false;
	}
	TestEqual(TEXT("Two fields ready together notify their actor once"), Spawned->ReceiveEventCount, EventsBeforeBatch + 1);
	// Multiple subsystem ticks in this synchronous test share the engine frame.
	Spawned->SetSyncFieldSourceInternal(Color, Fixed, Sample(0, 8));
	PC->Tick(.01f);
	TestEqual(TEXT("A second batch in the same engine frame fires its own event"), Spawned->ReceiveEventCount, EventsBeforeBatch + 2);
	TestTrue(TEXT("The second event exposes only its color, without the previous batch's motion"),
		Spawned->EventReadState(52, 8) && Spawned->LastEventFields.Num() == 1 && Spawned->LastEventFields.Contains(Color));
	Spawned->bReceiveDataFromPeer = false;
	Spawned->SetSyncFieldSourceInternal(Color, Fixed, Sample(0, 4));
	if (!WaitFor(TEXT("Other receivers still apply an update while PC actor reception is off"), [&]
		{ return OtherB->EventReadState(52, 4); }))
	{
		return false;
	}
	TestEqual(TEXT("A receive-disabled actor gets no local event"), Spawned->ReceiveEventCount, EventsBeforeBatch + 2);
	BallB->SetSyncFieldSourceInternal(Motion, Follow, {});
	if (!WaitFor(TEXT("B takes motion without A releasing"), [&] { return IsState(BallA, 30, 2); }))
	{
		return false;
	}
	const int32 APreparesAfterTakeover = BallA->PrepareEventCount;
	BallA->PayloadAtPrepare = Sample(999, 999);
	BallB->PayloadAtPrepare = Sample(34, 7);
	if (!WaitFor(TEXT("Previous writer never overwrites motion or fixed color"), [&] { return IsState(Ball, 34, 2); }))
	{
		return false;
	}
	TestEqual(TEXT("A stops preparing once B's takeover reaches A"), BallA->PrepareEventCount, APreparesAfterTakeover);
	const int32 PCReceivesBeforeTakeover = Ball->ReceiveEventCount;
	if (!TestTrue(TEXT("PC takes motion from B while both headsets are playing"),
		Ball->SetSyncFieldSourceInternal(Motion, Follow, {})))
	{
		return false;
	}
	const FTransform PCPoses[] = {
		FTransform(FRotator(5, 30, -10), FVector(48, -16, 21), FVector(1.25, 0.75, 2)),
		FTransform(FRotator(-14, 85, 7), FVector(-73, 42, 8), FVector(0.8, 1.4, 0.6))
	};
	for (const FTransform& Pose : PCPoses)
	{
		Ball->PayloadAtPrepare = Sample(0, 99);
		Ball->PayloadAtPrepare.GetMutable<FHUICRSyncScreenPayload>().Transform = Pose;
		auto ReceivedMotionOnly = [&](const AHUICRSyncFieldTestActor* Receiver)
		{
			const auto* Payload = Receiver->LastEventPayload.GetPtr<FHUICRSyncScreenPayload>();
			return Payload && Payload->Transform.Equals(Pose) && Payload->ScreenID == 2 &&
				Receiver->LastEventFields.Num() == 1 && Receiver->LastEventFields.Contains(Motion);
		};
		if (!WaitFor(TEXT("Both live headsets receive the PC pose via their motion-only actor events"),
			[&] { return ReceivedMotionOnly(BallA) && ReceivedMotionOnly(BallB); }))
		{
			return false;
		}
	}
	TestEqual(TEXT("PC does not receive its own motion back from either headset"), Ball->ReceiveEventCount, PCReceivesBeforeTakeover);
	if (!TestTrue(TEXT("B takes motion back from PC"), BallB->SetSyncFieldSourceInternal(Motion, Follow, {})) ||
		!WaitFor(TEXT("B's motion reaches both the former PC writer and A"), [&]
			{ return Ball->ReceiveEventCount > PCReceivesBeforeTakeover && Ball->EventReadState(34, 2) && BallA->EventReadState(34, 2); }))
	{
		return false;
	}
	auto AppliedOnAllThree = [&](FName Field, double X, int32 ColorIndex)
	{
		for (const AHUICRSyncFieldTestActor* Receiver : { Ball, BallA, BallB })
		{
			if (!Receiver->EventReadState(X, ColorIndex) || Receiver->AppliedColorIndex != ColorIndex ||
				Receiver->LastEventFields.Num() != 1 || !Receiver->LastEventFields.Contains(Field))
			{
				return false;
			}
		}
		return true;
	};
	if (!TestTrue(TEXT("PC specifies a color while B controls motion"),
			Ball->SetSyncFieldSourceInternal(Color, Fixed, Sample(900, 5))) ||
		!WaitFor(TEXT("PC and both headsets apply only the PC's specified color"), [&] { return AppliedOnAllThree(Color, 34, 5); }))
	{
		return false;
	}
	BallB->PayloadAtPrepare = Sample(35, 777);
	if (!WaitFor(TEXT("B continues moving the ball without replacing the PC's color"),
			[&] { return Ball->EventReadState(35, 5) && BallA->EventReadState(35, 5) && IsState(BallB, 35, 5); }) ||
		!TestTrue(TEXT("PC replaces B's continuous motion with one specified transform"),
			Ball->SetSyncFieldSourceInternal(Motion, Fixed, Sample(71, 888))) ||
		!WaitFor(TEXT("All three apply the fixed motion and preserve the independent color"), [&] { return AppliedOnAllThree(Motion, 71, 5); }))
	{
		return false;
	}
	const int32 BPreparesAfterFixedMotion = BallB->PrepareEventCount;
	BallB->PayloadAtPrepare = Sample(901, 999);
	BallB->PrepareOutgoingPayloadInternal(BallB->PayloadAtPrepare);
	B->SyncSubsystem->Tick(.01f);
	for (auto* Session : Sessions) { Session->Tick(.01f); }
	TestEqual(TEXT("Fixing motion stops the previous writer's prepare event"), BallB->PrepareEventCount, BPreparesAfterFixedMotion);
	TestTrue(TEXT("The former writer's new local sample cannot overwrite the fixed pose"),
		IsState(Ball, 71, 5) && IsState(BallA, 71, 5) && IsState(BallB, 71, 5));
	BallB->PayloadAtPrepare = Sample(34, 7);
	if (!TestTrue(TEXT("B can explicitly resume continuous motion after the PC's fixed pose"),
		BallB->SetSyncFieldSourceInternal(Motion, Follow, {}))) { return false; }
	BallB->SetSyncFieldSourceInternal(Color, Follow, {});
	if (!WaitFor(TEXT("The same color field switches from fixed to continuous"), [&] { return IsState(BallA, 34, 7); }))
	{
		return false;
	}
	const int32 BPreparesWithTwoFields = BallB->PrepareEventCount;
	B->SyncSubsystem->Tick(.01f);
	TestEqual(TEXT("Two continuous fields on B's ball produce one preparation per tick"), BallB->PrepareEventCount, BPreparesWithTwoFields + 1);
	TestTrue(TEXT("Both locally driven fields are listed together"), BallB->LastPrepareFields.Num() == 2 &&
		BallB->LastPrepareFields.Contains(Motion) && BallB->LastPrepareFields.Contains(Color));
	BallB->SetSyncFieldSourceInternal(Color, Fixed, Sample(900, 0));
	if (!WaitFor(TEXT("An explicit zero is preserved"), [&] { return IsState(BallA, 34, 0); }) ||
		!Calibrate(B) ||
		!WaitFor(TEXT("Recalibrating rebuilds the mirror with the latest merged state"), [&]
		{ return Mirror(B) != BallB && IsState(Mirror(B), 34, 0); }))
	{
		return false;
	}
	BallB = Mirror(B);
	BallB->PrepareOutgoingPayloadInternal(Sample(38, 4));
	BallB->SetSyncFieldSourceInternal(Motion, Follow, {});
	if (!WaitFor(TEXT("Rebuilt guest can explicitly retake motion"), [&] { return IsState(BallA, 38, 0); }))
	{
		return false;
	}
	B->SyncSubsystem->StopSync();
	if (!WaitFor(TEXT("Disconnect keeps last confirmed motion and color"), [&]
		{ return PC->ConnectedHMDCount == 1 && IsState(Ball, 38, 0); }) ||
		!Connect(B) || !WaitFor(TEXT("Reconnection restores the last state"), [&]
		{ return B->SyncSubsystem->bLocalSceneReady && IsState(Mirror(B), 38, 0); }))
	{
		return false;
	}
	const int32 BeforeCancel = FromDefaults->ReceiveEventCount;
	if (!TestTrue(TEXT("Schedule a fixed color then retake it before delivery"),
		FromDefaults->SetSyncFieldSourceInternal(Color, Fixed, Sample(0, 5)) &&
		FromDefaults->SetSyncFieldSourceInternal(Color, Follow, {}))) { return false; }
	PC->Tick(.01f);
	TestEqual(TEXT("Retaking the only pending field cancels the actor event"), FromDefaults->ReceiveEventCount, BeforeCancel);

	// Another actor's prepare callback grants a previously fixed field while this tick is in progress.
	Spawned->bReceiveDataFromPeer = true;
	Spawned->PrepareOutgoingPayloadInternal(Sample(500, 0));
	Spawned->PayloadAtPrepare = Sample(61, 0);
	const int32 BeforeGrant = Spawned->PrepareEventCount;
	FromDefaults->PrepareAction = [this, Spawned, Motion, Follow]
	{
		TestTrue(TEXT("A prepare callback can grant a field on another actor"), Spawned->SetSyncFieldSourceInternal(Motion, Follow, {}));
	};
	PC->Tick(.01f);
	FromDefaults->PrepareAction = nullptr;
	TestEqual(TEXT("An actor newly granted a field waits until its next sync tick to prepare"), Spawned->PrepareEventCount, BeforeGrant);
	TestTrue(TEXT("The grant does not publish the actor's stale cached sample"), IsState(Spawned, 52, 4));
	PC->Tick(.01f);
	TestEqual(TEXT("The newly active actor prepares once on the next tick"), Spawned->PrepareEventCount, BeforeGrant + 1);
	TestTrue(TEXT("Its first published sample comes from its own prepare callback"), IsState(Spawned, 61, 4));

	if (!TestTrue(TEXT("Queue color before unregistering"), FromDefaults->SetSyncFieldSourceInternal(Color, Fixed, Sample(0, 6))))
	{
		return false;
	}
	PC->UnregisterSyncActor(FromDefaults);
	auto* Replacement = World->SpawnActor<AHUICRSyncFieldTestActor>();
	if (!TestNotNull(TEXT("Replacement for the old actor ID"), Replacement)) { return false; }
	Replacement->SyncSubsystem = PC;
	Replacement->SetSyncActorID(FromDefaults->ActorID);
	if (!TestTrue(TEXT("Register new actor under the old ID"), PC->RegisterSyncActor(Replacement))) { return false; }
	PC->Tick(.01f);
	TestEqual(TEXT("An unregistered object gets no deferred event"), FromDefaults->ReceiveEventCount, BeforeCancel);
	TestEqual(TEXT("An actor reusing its ID does not inherit its pending event"), Replacement->ReceiveEventCount, 0);
	return !HasAnyErrors();
}
#endif
