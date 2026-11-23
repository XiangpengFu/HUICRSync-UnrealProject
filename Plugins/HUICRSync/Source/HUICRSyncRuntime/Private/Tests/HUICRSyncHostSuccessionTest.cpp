#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Components/SceneComponent.h"
#include "HAL/PlatformProcess.h"
#include "HUICRSyncHMDManagerBase.h"
#include "HUICRSyncPCManagerBase.h"
#include "HUICRSyncSubsystem.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHUICRSyncHostSuccessionTest, "HUICRSync.Network.HostSuccession",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHUICRSyncHostSuccessionTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("Test world"), World))
	{
		return false;
	}

	TArray<UHUICRSyncSubsystem*> Endpoints;
	ON_SCOPE_EXIT
	{
		for (UHUICRSyncSubsystem* Endpoint : Endpoints)
		{
			Endpoint->StopSync();
		}
		World->DestroyWorld(false);
	};

	auto NewEndpoint = [&Endpoints]()
	{
		auto* Endpoint = NewObject<UHUICRSyncSubsystem>(NewObject<UGameInstance>());
		Endpoint->HeartbeatIntervalSeconds = 0.1f;
		Endpoint->PeerTimeoutSeconds = 1.0f;
		Endpoints.Add(Endpoint);
		return Endpoint;
	};
	UHUICRSyncSubsystem* PC = NewEndpoint();
	// Separate receive ports on one machine also catch lookups that only compare IP addresses.
	const int32 BasePort = FMath::RandRange(20000, 43000);
	if (!TestTrue(TEXT("PC socket bound"), PC->ConfigureSync(EHUICRSyncRole::PC, BasePort, BasePort, TEXT(""))))
	{
		return false;
	}
	auto NewHeadset = [&](int32 Port)
	{
		auto* Manager = World->SpawnActor<AHUICRSyncHMDManagerBase>();
		if (Manager)
		{
			Manager->SyncSubsystem = NewEndpoint();
			Manager->LocalSyncPort = Port;
		}
		return Manager;
	};
	AHUICRSyncHMDManagerBase* A = NewHeadset(BasePort + 1);
	AHUICRSyncHMDManagerBase* B = NewHeadset(BasePort + 2);
	AHUICRSyncHMDManagerBase* C = NewHeadset(BasePort + 3);
	if (!TestTrue(TEXT("Created three independent HMD clients"), A && B && C))
	{
		return false;
	}

	// Withhold ticks from one endpoint later to simulate power loss without delivering a leave packet.
	UHUICRSyncSubsystem* SilentEndpoint = nullptr;
	auto WaitFor = [&](const TCHAR* Description, TFunctionRef<bool()> IsReady)
	{
		const double Deadline = FPlatformTime::Seconds() + 5.0;
		do
		{
			for (UHUICRSyncSubsystem* Endpoint : Endpoints)
			{
				if (Endpoint != SilentEndpoint)
				{
					Endpoint->Tick(0.01f);
				}
			}
			if (IsReady())
			{
				return TestTrue(Description, true);
			}
			FPlatformProcess::Sleep(0.005f);
		} while (FPlatformTime::Seconds() < Deadline);
		return TestTrue(Description, false);
	};
	auto Connect = [&](AHUICRSyncHMDManagerBase* Manager)
	{
		return Manager->ConnectToPC(PC->LocalIpAddress, PC->LocalSyncPort, false);
	};

	if (!TestTrue(TEXT("A sent Connect"), Connect(A)) ||
		!WaitFor(TEXT("A connected first and is Host"), [&] { return A->SyncSubsystem->bIsLocalHMDHost && PC->ConnectedHMDCount == 1; }))
	{
		return false;
	}
	const int32 IdA = A->SyncSubsystem->LocalParticipantId;
	if (!TestTrue(TEXT("A has a participant ID"), IdA > 0) || !TestTrue(TEXT("B sent Connect"), Connect(B)) ||
		!WaitFor(TEXT("Both clients see two participants"), [&]
		{
			return A->SyncSubsystem->ConnectedHMDCount == 2 && B->SyncSubsystem->ConnectedHMDCount == 2;
		}))
	{
		return false;
	}
	const int32 IdB = B->SyncSubsystem->LocalParticipantId;
	TestTrue(TEXT("Separate identities on the same IP"), IdB > 0 && IdB != IdA);
	TestFalse(TEXT("B received guest status"), B->SyncSubsystem->bIsLocalHMDHost);
	TestEqual(TEXT("PC remembers A as Host"), PC->HostParticipantId, IdA);
	TestEqual(TEXT("B knows A is Host"), B->SyncSubsystem->HostParticipantId, IdA);

	if (!TestTrue(TEXT("Repeat Connect on A"), Connect(A)) || !TestTrue(TEXT("C sent Connect"), Connect(C)) ||
		!WaitFor(TEXT("Repeat Connect preserved A's identity, C joined as third"), [&]
		{
			return PC->ConnectedHMDCount == 3 && C->SyncSubsystem->ConnectedHMDCount == 3 &&
				A->SyncSubsystem->LocalParticipantId == IdA && A->SyncSubsystem->bIsLocalHMDHost;
		}))
	{
		return false;
	}
	const int32 IdC = C->SyncSubsystem->LocalParticipantId;
	TestTrue(TEXT("C has a distinct identity"), IdC > 0 && IdC != IdA && IdC != IdB);

	const FTransform PoseA(FRotator(2.0, 8.0, 5.0), FVector(101.0, -20.0, 165.0));
	const FTransform PoseB(FRotator(10.0, -73.0, 2.0), FVector(-32.0, 207.0, 178.0));
	A->SyncSubsystem->SendLocalHeadPose(PoseA);
	B->SyncSubsystem->SendLocalHeadPose(PoseB);
	if (!WaitFor(TEXT("PC received both HMD poses before gameplay started"), [&]
	{
		bool bFoundA = false, bFoundB = false;
		for (const auto& Pair : PC->PeerInfos)
		{
			const FHUICRSyncPeerInfo& Info = Pair.Value;
			bFoundA |= Info.ParticipantId == IdA && Info.bHasHeadPose && Info.HeadPoseInPCSpace.Equals(PoseA);
			bFoundB |= Info.ParticipantId == IdB && Info.bHasHeadPose && Info.HeadPoseInPCSpace.Equals(PoseB);
		}
		return bFoundA && bFoundB;
	}))
	{
		return false;
	}

	auto* PCManager = World->SpawnActor<AHUICRSyncPCManagerBase>();
	if (!TestNotNull(TEXT("PC manager"), PCManager))
	{
		return false;
	}
	PCManager->SyncSubsystem = PC;
	PCManager->bAutoSpawnPCCalibrators = false;
	PCManager->bAutoSpawnPCScreenDissolveBoxes = false;
	auto* Screen = NewObject<USceneComponent>(PCManager);
	Screen->RegisterComponent();
	const FTransform ScreenPose(FRotator(0, 47, 0), FVector(25, 40, 100));
	Screen->SetWorldTransform(ScreenPose);
	PCManager->RegisterPCScreenComponent(1, Screen, true);
	const FTransform MappingB(FRotator(0, 17, 0), FVector(-50, 39, 8));
	B->SyncSubsystem->HMDToPCTransform = MappingB;
	PC->WorldID = 17;
	B->SyncSubsystem->SendReliableCommandInt(TEXT("/FromHMD/ChangeToWorldID"), 99);
	PC->ReceiveData();
	TestEqual(TEXT("A guest cannot change the Host's shared world"), PC->WorldID, 17);
	A->SyncSubsystem->StopSync();
	if (!WaitFor(TEXT("A left, oldest remaining participant B was promoted"), [&]
	{
		return PC->ConnectedHMDCount == 2 && PC->HostParticipantId == IdB && B->SyncSubsystem->bIsLocalHMDHost &&
			C->SyncSubsystem->HostParticipantId == IdB && !C->SyncSubsystem->bIsLocalHMDHost;
	}))
	{
		return false;
	}
	TestTrue(TEXT("PC screen placement survived handoff"), Screen->GetComponentTransform().Equals(ScreenPose));
	TestTrue(TEXT("B kept its calibrated mapping when promoted"), B->SyncSubsystem->HMDToPCTransform.Equals(MappingB));

	if (!TestTrue(TEXT("A reconnected using its original port"), Connect(A)) ||
		!WaitFor(TEXT("Returning A joined behind B and C"), [&]
		{
			return A->SyncSubsystem->ConnectedHMDCount == 3 && A->SyncSubsystem->HostParticipantId == IdB &&
				!A->SyncSubsystem->bIsLocalHMDHost && PC->ConnectedHMDCount == 3;
		}))
	{
		return false;
	}

	SilentEndpoint = B->SyncSubsystem;
	if (!WaitFor(TEXT("B timed out, C promoted before returning A"), [&]
	{
		return PC->ConnectedHMDCount == 2 && PC->HostParticipantId == IdC &&
			C->SyncSubsystem->bIsLocalHMDHost && A->SyncSubsystem->HostParticipantId == IdC;
	}))
	{
		return false;
	}
	C->SyncSubsystem->StopSync();
	if (!WaitFor(TEXT("Last online HMD became Host"), [&]
	{
		return PC->ConnectedHMDCount == 1 && A->SyncSubsystem->bIsLocalHMDHost;
	}))
	{
		return false;
	}
	A->SyncSubsystem->StopSync();
	WaitFor(TEXT("No HMD online, no Host"), [&] { return PC->ConnectedHMDCount == 0 && PC->HostParticipantId == 0; });
	TestEqual(TEXT("Host changes preserved shared world state"), PC->WorldID, 17);
	return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
