#if WITH_DEV_AUTOMATION_TESTS

#include "HUICRSyncFieldTestActor.h"
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHUICRSyncSharedFieldsTest, "HUICRSync.Network.SharedFields",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHUICRSyncSharedFieldsTest::RunTest(const FString& Parameters)
{
	const FString MapName = TEXT("HUICRFields_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	auto* Package = CreatePackage(*(TEXT("/Temp/") + MapName));
	auto* World = UWorld::CreateWorld(EWorldType::Game, false, *MapName, Package);
	if (!TestNotNull(TEXT("Field test world"), World)) { return false; }
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
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
	UHUICRSyncPayloadLibrary::WriteInstancedStruct(Ball->ActorInitPayloadData, PayloadType, Sample(10, 1));
	PC->RegisterSyncActor(Ball);
	auto Mirror = [Ball](AHUICRSyncHMDManagerBase* Manager)
	{
		return Manager->SyncSubsystem->FindSyncActor(Ball->TypeCode, Ball->ActorID);
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
	Ball->PrepareOutgoingPayloadInternal(Sample(12, 99));
	if (!WaitFor(TEXT("PC drives motion; initial scalar is preserved on A"), [&] { return IsState(Mirror(A), 12, 1); }))
	{
		return false;
	}
	auto* BallA = Mirror(A);
	BallA->PrepareOutgoingPayloadInternal(Sample(22, 8));
	if (!TestTrue(TEXT("First headset takes motion"), BallA->SetSyncFieldSourceInternal(Motion, Follow, {})) ||
		!WaitFor(TEXT("PC receives only A's motion"), [&] { return IsState(Ball, 22, 1); }) ||
		!Connect(B) || !Calibrate(B) ||
		!WaitFor(TEXT("Late guest restores the two independently confirmed fields"), [&] { return IsState(Mirror(B), 22, 1); }))
	{
		return false;
	}
	auto* BallB = Mirror(B);
	BallB->PrepareOutgoingPayloadInternal(Sample(30, 7));
	BallB->SetSyncFieldSourceInternal(Color, Fixed, Sample(900, 2));
	BallA->PrepareOutgoingPayloadInternal(Sample(25, 4));
	if (!WaitFor(TEXT("A moves while guest B independently sets color"), [&]
		{ return IsState(Ball, 25, 2) && IsState(BallA, 25, 2) && IsState(BallB, 25, 2); }))
	{
		return false;
	}
	FInstancedStruct State;
	TArray<FName> Mask;
	BallA->ReadReceivedPayloadInternal(State, Mask);
	TestFalse(TEXT("An echo cannot move the local motion writer"), Mask.Contains(Motion));
	BallB->SetSyncFieldSourceInternal(Motion, Follow, {});
	if (!WaitFor(TEXT("B takes motion without A releasing"), [&] { return IsState(BallA, 30, 2); }))
	{
		return false;
	}
	BallA->PrepareOutgoingPayloadInternal(Sample(999, 999));
	BallB->PrepareOutgoingPayloadInternal(Sample(34, 7));
	if (!WaitFor(TEXT("Previous writer never overwrites motion or fixed color"), [&] { return IsState(Ball, 34, 2); }))
	{
		return false;
	}
	BallB->SetSyncFieldSourceInternal(Color, Follow, {});
	if (!WaitFor(TEXT("The same color field switches from fixed to continuous"), [&] { return IsState(BallA, 34, 7); }))
	{
		return false;
	}
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
	return !HasAnyErrors();
}
#endif
