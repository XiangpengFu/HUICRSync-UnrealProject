#if WITH_DEV_AUTOMATION_TESTS

#include "HUICRSyncParticipantPawnFixture.h"
#include "HUICRSyncHMDManagerBase.h"
#include "HUICRSyncPCManagerBase.h"
#include "HUICRSyncPayloadLibrary.h"
#include "HUICRSyncScreen.h"
#include "HUICRSyncSubsystem.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformProcess.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/AutomationTest.h"
#include "Misc/Crc.h"
#include "Misc/ScopeExit.h"
#include "UObject/Script.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHUICRSyncParticipantPawnTest, "HUICRSync.Network.ParticipantPawns",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHUICRSyncParticipantPawnTest::RunTest(const FString& Parameters)
{
	const FString Name = TEXT("HUICRPawns_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	auto* Package = CreatePackage(*(TEXT("/Temp/") + Name));
	auto* World = UWorld::CreateWorld(EWorldType::Game, false, *Name, Package);
	if (!TestNotNull(TEXT("Game world for participant pawns"), World)) { return false; }
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	FEditorScriptExecutionGuard AllowActorEvents;
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
	auto MakeSession = [&]
	{
		auto* Session = NewObject<UHUICRSyncSubsystem>(NewObject<UGameInstance>());
		Session->HeartbeatIntervalSeconds = .1f;
		Sessions.Add(Session);
		return Session;
	};
	auto WaitFor = [&](const TCHAR* Description, TFunctionRef<bool()> Predicate)
	{
		const double End = FPlatformTime::Seconds() + 6.;
		do
		{
			for (auto* Session : Sessions) { Session->Tick(.01f); }
			if (Predicate()) { return true; }
			FPlatformProcess::Sleep(.005f);
		} while (FPlatformTime::Seconds() < End);
		AddError(Description);
		return false;
	};
	const int32 Port = FMath::RandRange(22000, 43000);
	auto* PC = MakeSession();
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
	struct FHeadset
	{
		UHUICRSyncSubsystem* Session;
		AHUICRSyncHMDManagerBase* Manager;
		AHUICRSyncParticipantPawnFixture* Pawn;
	};
	auto Headset = [&](int32 Offset, double X)
	{
		auto* Session = MakeSession();
		auto* Manager = World->SpawnActor<AHUICRSyncHMDManagerBase>();
		Manager->LocalSyncPort = Port + Offset;
		Manager->CalibrationSaveSlotName = Name + FString::FromInt(Offset);
		SaveSlots.Add(Manager->CalibrationSaveSlotName);
		Manager->SyncSubsystem = Session;
		Session->RegisterCalibrationManager(Manager);
		auto* Screen = World->SpawnActor<AHUICRSyncScreen>();
		Screen->bAutoAssignScreenID = false;
		Screen->ChangeScreenID(0);
		Manager->RegisterScreen(Screen);
		auto* Pawn = World->SpawnActor<AHUICRSyncParticipantPawnFixture>();
		Pawn->SyncSubsystem = Session;
		Pawn->HeadX = X;
		Pawn->SampleNumber = Offset;
		auto* Controller = World->SpawnActor<APlayerController>();
		Controller->Possess(Pawn);
		Pawn->DispatchBeginPlay();
		return FHeadset{ Session, Manager, Pawn };
	};
	auto Connect = [&](const FHeadset& Device)
	{
		return TestTrue(TEXT("Headset starts its connection"), Device.Manager->ConnectToPC(PC->LocalIpAddress, Port, false));
	};
	auto Counterpart = [&](const FHeadset& Device)
	{
		return Cast<AHUICRSyncPCPawnFixture>(PC->FindSyncActor(EHUICRSyncActorType::HMDPawn, Device.Session->LocalParticipantId));
	};
	auto Calibrate = [&](const FHeadset& Device)
	{
		if (!WaitFor(TEXT("Can start alignment"), [&] { return Device.Session->bCanStartCalibration; }) ||
			!TestTrue(TEXT("Request alignment"), Device.Session->RequestCalibrationTurn()) ||
			!WaitFor(TEXT("Alignment granted"), [&] { return Device.Session->bIsLocalHMDCalibrating; })) { return false; }
		Device.Manager->ChangeCalibrationState();
		return WaitFor(TEXT("Alignment completed"), [&] { return !Device.Session->bGameStopped && Device.Session->bLocalSceneReady; });
	};
	FHeadset A = Headset(1, 13), B = Headset(2, -53);
	auto Birth = FInstancedStruct::Make<FHUICRSyncScreenPayload>();
	Birth.GetMutable<FHUICRSyncScreenPayload>().ScreenID = 107;
	if (!TestTrue(TEXT("A can set its birth payload before connecting"), A.Pawn->SetInitialPayloadInternal(Birth)) ||
		!Connect(A) || !WaitFor(TEXT("PC automatically spawns A's private avatar on joining, before calibration"), [&]
		{ return A.Session->LocalParticipantId > 0 && IsValid(Counterpart(A)); })) { return false; }
	auto* AvatarA = Counterpart(A);
	FInstancedStruct BirthOnPC;
	if (!TestTrue(TEXT("The PC avatar received A's birth data on initial pairing"),
		AvatarA->ReadInitialPayloadInternal(BirthOnPC) && BirthOnPC.Get<FHUICRSyncScreenPayload>().ScreenID == 107)) { return false; }
	if (!Calibrate(A) || !WaitFor(TEXT("Host's tracking fields automatically follow its Pawn"), [&] { return AvatarA->Received(13, 1); }) ||
		!Connect(B) || !WaitFor(TEXT("PC creates a separate guest avatar immediately"), [&]
			{ return B.Session->LocalParticipantId > 0 && IsValid(Counterpart(B)); }) || !Calibrate(B)) { return false; }
	auto* AvatarB = Counterpart(B);
	const int32 IdA = A.Session->LocalParticipantId, IdB = B.Session->LocalParticipantId;
	if (!TestTrue(TEXT("Each local pawn shares only its own PC avatar's unique participant ID"),
		IdA != IdB && AvatarA != AvatarB && AvatarA->ActorID == IdA && AvatarB->ActorID == IdB &&
		A.Pawn->ActorID == IdA && B.Pawn->ActorID == IdB) ||
		!WaitFor(TEXT("Concurrent tracking updates reach their own avatar"), [&]
			{ return AvatarA->Received(13, 1) && AvatarB->Received(-53, 2); })) { return false; }
	TestNull(TEXT("A has no network mirror of B's private pawn"), A.Session->FindSyncActor(EHUICRSyncActorType::HMDPawn, IdB));
	TestNull(TEXT("B has no network mirror of A's private pawn"), B.Session->FindSyncActor(EHUICRSyncActorType::HMDPawn, IdA));
	// The original example packs the pose on the HMD and unpacks the received bytes on the PC.
	A.Pawn->bUseLegacyBlueprintWrites = true;
	auto RawPose = FInstancedStruct::Make<FHUICRSyncScreenPayload>();
	RawPose.GetMutable<FHUICRSyncScreenPayload>().Transform = FTransform(FVector(47, 0, 170));
	RawPose.GetMutable<FHUICRSyncScreenPayload>().ScreenID = 17;
	if (!TestTrue(TEXT("The existing HMD graph can prepare its byte payload"), UHUICRSyncPayloadLibrary::WriteInstancedStruct(
		A.Pawn->ActorSendPayloadData, FHUICRSyncScreenPayload::StaticStruct(), RawPose)) ||
		!WaitFor(TEXT("Both old byte graphs work after selecting a shared schema"), [&]
		{
			FInstancedStruct Unpacked;
			return AvatarA->Received(47, 17) && UHUICRSyncPayloadLibrary::ReadInstancedStruct(
				AvatarA->ActorReceivePayloadData, FHUICRSyncScreenPayload::StaticStruct(), Unpacked) &&
				Unpacked.Get<FHUICRSyncScreenPayload>().Transform.Equals(RawPose.Get<FHUICRSyncScreenPayload>().Transform) &&
				AvatarB->Received(-53, 2);
		})) { return false; }
	A.Pawn->bUseLegacyBlueprintWrites = false;
	auto Fixed = FInstancedStruct::Make<FHUICRSyncScreenPayload>();
	Fixed.GetMutable<FHUICRSyncScreenPayload>().ScreenID = 91;
	const FName NumberField = GET_MEMBER_NAME_CHECKED(FHUICRSyncScreenPayload, ScreenID);
	if (!TestTrue(TEXT("PC writes a private feedback field to B"), AvatarB->SetSyncFieldSourceInternal(NumberField,
		EHUICRSyncFieldSource::UseSpecifiedValue, Fixed))) { return false; }
	A.Pawn->HeadX = 82;
	B.Pawn->HeadX = 32;
	auto LocalNumber = [](const FHeadset& Device)
	{
		FInstancedStruct Payload;
		TArray<FName> Fields;
		return Device.Pawn->ReadReceivedPayloadInternal(Payload, Fields) ? Payload.Get<FHUICRSyncScreenPayload>().ScreenID : INDEX_NONE;
	};
	if (!WaitFor(TEXT("B receives the PC override while A continues to send its own tracking"), [&]
		{ return LocalNumber(B) == 91 && AvatarA->Received(82, 1) && AvatarB->Received(32, 91); })) { return false; }
	TestNotEqual(TEXT("B's private feedback was never applied on A"), LocalNumber(A), 91);
	A.Session->StopSync();
	if (!WaitFor(TEXT("Host leaving removes only its avatar; successor keeps the same actor and ID"), [&]
		{ return !IsValid(AvatarA) && !PC->FindSyncActor(EHUICRSyncActorType::HMDPawn, IdA) &&
			PC->ConnectedHMDCount == 1 && B.Session->bIsLocalHMDHost && B.Session->LocalParticipantId == IdB && Counterpart(B) == AvatarB; }))
	{
		return false;
	}
	B.Pawn->HeadX = 65;
	if (!WaitFor(TEXT("A promoted guest continues driving its original avatar"), [&] { return AvatarB->Received(65, 91); }) ||
		!Connect(A) || !WaitFor(TEXT("Rejoining A gets a fresh PC avatar without recreating B"), [&]
			{ return A.Session->LocalParticipantId > 0 && IsValid(Counterpart(A)) && Counterpart(A) != AvatarA &&
				A.Session->LocalParticipantId != IdB && Counterpart(B) == AvatarB; })) { return false; }
	// Changing the possessed Pawn exercises replacement in the same connected session and adoption of an old placed avatar.
	auto* PreviousA = Counterpart(A);
	auto* Placed = World->SpawnActor<AHUICRSyncLegacyPCPawnFixture>();
	Placed->SyncSubsystem = PC;
	Placed->SetSyncActorID(0);
	Placed->DispatchBeginPlay();
	auto* RawPawn = World->SpawnActor<AHUICRSyncHMDPawn>();
	RawPawn->SyncSubsystem = A.Session;
	RawPawn->DispatchBeginPlay();
	auto* ControllerA = Cast<APlayerController>(A.Pawn->GetController());
	if (!TestNotNull(TEXT("Rejoining A still has a player controller"), ControllerA)) { return false; }
	ControllerA->Possess(RawPawn);
	const int32 RejoinedId = A.Session->LocalParticipantId;
	if (!WaitFor(TEXT("Possession automatically moves the old placed avatar to the owning ID without leaving a duplicate"), [&]
		{ return RawPawn->ActorID == RejoinedId && !IsValid(PreviousA) &&
			PC->FindSyncActor(EHUICRSyncActorType::HMDPawn, RejoinedId) == Placed && !PC->FindSyncActor(EHUICRSyncActorType::HMDPawn, 0); }) ||
		!WaitFor(TEXT("Reconnected headset can exchange legacy bytes"), [&]
			{ return A.Session->bLocalSceneReady && !A.Session->bGameStopped; })) { return false; }
	const TArray<uint8> Tracking{ 42, 1, 9 }, Feedback{ 7, 12 };
	if (!WaitFor(TEXT("Schema-less legacy tracking and feedback work for only their own HMD/PC pair"), [&]
	{
		RawPawn->ActorSendPayloadData = Tracking;
		Placed->ActorSendPayloadData = Feedback;
		return Placed->ActorReceivePayloadData == Tracking && RawPawn->ActorReceivePayloadData == Feedback && AvatarB->Received(65, 91);
	})) { return false; }
	FHUICRSyncNetEntry OtherParticipantsSample;
	OtherParticipantsSample.TypeCode = EHUICRSyncActorType::HMDPawn;
	OtherParticipantsSample.ActorID = RejoinedId;
	OtherParticipantsSample.PayloadBytes = { 99, 99 };
	B.Session->QueueNetEntry(OtherParticipantsSample);
	B.Pawn->HeadX = 66;
	if (!WaitFor(TEXT("B's tracking still updates normally"), [&] { return AvatarB->Received(66, 91); })) { return false; }
	TestTrue(TEXT("An entry submitted by B for A's ID never changes A's raw pose"), Placed->ActorReceivePayloadData == Tracking);
	ControllerA->UnPossess();
	if (!WaitFor(TEXT("Unpossessing the local pawn removes only its PC avatar while the headset stays connected"), [&]
		{ return !IsValid(Placed) && PC->ConnectedHMDCount == 2 && Counterpart(B) == AvatarB; })) { return false; }
	PC->StopSync();
	TestFalse(TEXT("Stopping the PC session removes the remaining guest avatar"), IsValid(AvatarB));
	return !HasAnyErrors();
}

#endif
