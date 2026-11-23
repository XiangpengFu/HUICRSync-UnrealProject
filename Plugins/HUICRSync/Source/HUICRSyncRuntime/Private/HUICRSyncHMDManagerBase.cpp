#include "HUICRSyncHMDManagerBase.h"

#include "HUICRSyncCalibrationSaveGame.h"
#include "HUICRSyncCalibrator_HMD.h"
#include "HUICRSyncConnectionSaveGame.h"
#include "HUICRSyncScreen.h"
#include "HUICRSyncSubsystem.h"
#include "Kismet/GameplayStatics.h"

AHUICRSyncHMDManagerBase::AHUICRSyncHMDManagerBase()
{
	LocalRole = EHUICRSyncRole::HMD;
}

void AHUICRSyncHMDManagerBase::BeginPlay()
{
	Super::BeginPlay();

	if (SyncSubsystem)
	{
		SyncSubsystem->RegisterCalibrationManager(this);
		SyncSubsystem->OnSyncSceneReady.AddDynamic(this, &AHUICRSyncHMDManagerBase::HandleSyncSceneReady);
	}

	// if (bAutoRegisterCalibrationActorsOnBeginPlay)
	// {
	// 	RegisterCalibrationActorsInWorld();
	// }

	// if (bLoadCalibrationDataOnBeginPlay)
	// {
	// 	LoadCalibrationData();
	// }

	// if (bAutoConnectToSavedPCOnBeginPlay)
	// {
	// 	ConnectToSavedPC();
	// }
}

void AHUICRSyncHMDManagerBase::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (SyncSubsystem)
	{
		SyncSubsystem->OnSyncSceneReady.RemoveDynamic(this, &AHUICRSyncHMDManagerBase::HandleSyncSceneReady);
	}

	Super::EndPlay(EndPlayReason);
}

bool AHUICRSyncHMDManagerBase::LoadSavedPCConnection()
{
	USaveGame* LoadedSave = UGameplayStatics::LoadGameFromSlot(ConnectionSaveSlotName, ConnectionSaveUserIndex);
	UHUICRSyncConnectionSaveGame* ConnectionSave = Cast<UHUICRSyncConnectionSaveGame>(LoadedSave);

	if (!ConnectionSave)
	{
		return false;
	}

	SavedPCIpAddress = ConnectionSave->PCIpAddress;
	SavedTargetPCSyncPort = ConnectionSave->TargetPCSyncPort;
	RemoteIpAddress = SavedPCIpAddress;
	RemoteSyncPort = SavedTargetPCSyncPort;
	return !SavedPCIpAddress.IsEmpty();
}

bool AHUICRSyncHMDManagerBase::SavePCConnection(const FString& PCIpAddress, int32 TargetPCSyncPort)
{
	if (PCIpAddress.IsEmpty() || TargetPCSyncPort <= 0)
	{
		return false;
	}

	UHUICRSyncConnectionSaveGame* ConnectionSave = Cast<UHUICRSyncConnectionSaveGame>(
		UGameplayStatics::CreateSaveGameObject(UHUICRSyncConnectionSaveGame::StaticClass()));

	if (!ConnectionSave)
	{
		return false;
	}

	ConnectionSave->PCIpAddress = PCIpAddress;
	ConnectionSave->TargetPCSyncPort = TargetPCSyncPort;

	const bool bSaved = UGameplayStatics::SaveGameToSlot(ConnectionSave, ConnectionSaveSlotName, ConnectionSaveUserIndex);
	if (bSaved)
	{
		SavedPCIpAddress = PCIpAddress;
		SavedTargetPCSyncPort = TargetPCSyncPort;
	}

	return bSaved;
}

bool AHUICRSyncHMDManagerBase::ConnectToPC(const FString& PCIpAddress, int32 TargetPCSyncPort, bool bSaveConnection)
{
	if (PCIpAddress.IsEmpty() || TargetPCSyncPort <= 0 || TargetPCSyncPort >= 65535 || LocalSyncPort <= 0 || LocalSyncPort >= 65535)
	{
		return false;
	}

	RemoteIpAddress = PCIpAddress;
	RemoteSyncPort = TargetPCSyncPort;

	if (!SyncSubsystem && !InitializeManager())
	{
		return false;
	}

	if (!SyncSubsystem)
	{
		return false;
	}

	if (SyncSubsystem->Role == EHUICRSyncRole::HMD && SyncSubsystem->bConnectionState &&
		SyncSubsystem->LocalSyncPort == LocalSyncPort && SyncSubsystem->RemoteSyncPort == TargetPCSyncPort &&
		SyncSubsystem->RemoteIpAddress == PCIpAddress)
	{
		if (bSaveConnection)
		{
			SavePCConnection(PCIpAddress, TargetPCSyncPort);
		}
		return true;
	}

	if (!SyncSubsystem->ConfigureSync(EHUICRSyncRole::HMD, LocalSyncPort, TargetPCSyncPort, PCIpAddress))
	{
		return false;
	}
	SyncSubsystem->BeginConnectionAttempt();
	
	FHUICRSyncCommandArg IpArg;
	IpArg.Type = EHUICRSyncCommandArgType::String;
	IpArg.StringValue = GetLocalIpAddress();

	FHUICRSyncCommandArg PortArg;
	PortArg.Type = EHUICRSyncCommandArgType::Int32;
	PortArg.IntValue = LocalSyncPort;

	FHUICRSyncCommandArg SessionArg;
	SessionArg.Type = EHUICRSyncCommandArgType::String;
	SessionArg.StringValue = SyncSubsystem->LocalSessionId;

	const bool bConnectionRequestSent = SyncSubsystem->SendReliableCommand(
		TEXT("FromHMD/HMDConnectionRequest"), { IpArg, PortArg, SessionArg });

	if (bSaveConnection)
	{
		SavePCConnection(PCIpAddress, TargetPCSyncPort);
	}

	return bConnectionRequestSent;
}

bool AHUICRSyncHMDManagerBase::ConnectToSavedPC()
{
	if (!LoadSavedPCConnection())
	{
		return false;
	}

	return ConnectToPC(SavedPCIpAddress, SavedTargetPCSyncPort, false);
}

FString AHUICRSyncHMDManagerBase::GetLocalIpAddress() const
{
	return SyncSubsystem ? SyncSubsystem->LocalIpAddress : FString();
}

TArray<int32> AHUICRSyncHMDManagerBase::BuildSavedScreenIDFallbackOrder() const
{
	TArray<int32> Result;
	if (!CalibrationSaveGame)
	{
		return Result;
	}

	TSet<int32> AddedIDs;
	auto AddID = [&Result, &AddedIDs](int32 ScreenID)
	{
		if (ScreenID >= 0 && !AddedIDs.Contains(ScreenID))
		{
			AddedIDs.Add(ScreenID);
			Result.Add(ScreenID);
		}
	};

	for (int32 ScreenID : CalibrationSaveGame->SavedScreenIDOrder)
	{
		AddID(ScreenID);
	}

	TArray<int32> AdditionalIDs;
	auto CollectMapKeys = [&AdditionalIDs](const auto& Map)
	{
		for (const auto& Pair : Map)
		{
			AdditionalIDs.Add(Pair.Key);
		}
	};

	CollectMapKeys(CalibrationSaveGame->ScreenOffsetMap);
	CollectMapKeys(CalibrationSaveGame->ScreenRotationMap);
	CollectMapKeys(CalibrationSaveGame->ScreenScaleMap);
	CollectMapKeys(CalibrationSaveGame->ShouldScreenControlRemoteMap);
	for (const TPair<FString, int32>& Pair : CalibrationSaveGame->ScreenIDByPersistentKey)
	{
		AdditionalIDs.Add(Pair.Value);
	}

	AdditionalIDs.Sort();
	for (int32 ScreenID : AdditionalIDs)
	{
		AddID(ScreenID);
	}

	return Result;
}

void AHUICRSyncHMDManagerBase::RestoreRegisteredScreenIDsAndCalibration()
{
	if (!CalibrationSaveGame)
	{
		return;
	}

	RegisteredScreens.RemoveAll([](const AHUICRSyncScreen* Screen)
	{
		return !IsValid(Screen);
	});

	for (AHUICRSyncScreen* Screen : RegisteredScreens)
	{
		Screen->InitScreen(nullptr);
		Screen->ResolvePersistentScreenKey();
	}

	TSet<int32> UsedIDs;
	TArray<AHUICRSyncScreen*> ScreensWithoutMatchedKey;
	for (AHUICRSyncScreen* Screen : RegisteredScreens)
	{
		const int32* SavedScreenID = Screen->PersistentScreenKey.IsEmpty()
			? nullptr
			: CalibrationSaveGame->ScreenIDByPersistentKey.Find(Screen->PersistentScreenKey);

		if (SavedScreenID && *SavedScreenID >= 0 && !UsedIDs.Contains(*SavedScreenID))
		{
			Screen->ChangeScreenID(*SavedScreenID);
			UsedIDs.Add(*SavedScreenID);
		}
		else
		{
			ScreensWithoutMatchedKey.Add(Screen);
		}
	}

	const TArray<int32> FallbackIDs = BuildSavedScreenIDFallbackOrder();
	int32 NextNewScreenID = 0;
	for (int32 SavedScreenID : FallbackIDs)
	{
		NextNewScreenID = FMath::Max(NextNewScreenID, SavedScreenID + 1);
	}

	int32 FallbackIndex = 0;
	for (AHUICRSyncScreen* Screen : ScreensWithoutMatchedKey)
	{
		while (FallbackIndex < FallbackIDs.Num() && UsedIDs.Contains(FallbackIDs[FallbackIndex]))
		{
			++FallbackIndex;
		}

		int32 ResolvedScreenID = INDEX_NONE;
		if (FallbackIndex < FallbackIDs.Num())
		{
			ResolvedScreenID = FallbackIDs[FallbackIndex++];
		}
		else
		{
			while (UsedIDs.Contains(NextNewScreenID))
			{
				++NextNewScreenID;
			}
			ResolvedScreenID = NextNewScreenID++;
		}

		Screen->ChangeScreenID(ResolvedScreenID);
		UsedIDs.Add(ResolvedScreenID);
	}

	for (AHUICRSyncScreen* Screen : RegisteredScreens)
	{
		if (SyncSubsystem)
		{
			SyncSubsystem->ReserveActorID(EHUICRSyncActorType::HUIScreen, Screen->ScreenID);
		}
		Screen->InitScreen(CalibrationSaveGame);
	}

	ManipulatedTargetScreen = nullptr;
	for (AHUICRSyncScreen* Screen : RegisteredScreens)
	{
		if (Screen->ScreenID == 0)
		{
			ManipulatedTargetScreen = Screen;
			break;
		}
	}
	if (!IsValid(ManipulatedTargetScreen) && RegisteredScreens.Num() > 0)
	{
		ManipulatedTargetScreen = RegisteredScreens[0];
	}

	SelectCalibrator();
}

void AHUICRSyncHMDManagerBase::PrepareRegisteredScreensForSave()
{
	if (!CalibrationSaveGame)
	{
		return;
	}

	RegisteredScreens.RemoveAll([](const AHUICRSyncScreen* Screen)
	{
		return !IsValid(Screen);
	});

	const TArray<int32> PreviousSavedOrder = BuildSavedScreenIDFallbackOrder();
	int32 NextNewScreenID = 0;
	for (int32 SavedScreenID : PreviousSavedOrder)
	{
		NextNewScreenID = FMath::Max(NextNewScreenID, SavedScreenID + 1);
	}
	for (const AHUICRSyncScreen* Screen : RegisteredScreens)
	{
		NextNewScreenID = FMath::Max(NextNewScreenID, Screen->ScreenID + 1);
	}

	TSet<int32> UsedIDs;
	TArray<int32> CurrentScreenOrder;
	for (AHUICRSyncScreen* Screen : RegisteredScreens)
	{
		if (Screen->ScreenID < 0 || UsedIDs.Contains(Screen->ScreenID))
		{
			while (UsedIDs.Contains(NextNewScreenID))
			{
				++NextNewScreenID;
			}
			Screen->ChangeScreenID(NextNewScreenID++);
		}

		UsedIDs.Add(Screen->ScreenID);
		CurrentScreenOrder.Add(Screen->ScreenID);
		Screen->ResolvePersistentScreenKey();
		if (SyncSubsystem)
		{
			SyncSubsystem->ReserveActorID(EHUICRSyncActorType::HUIScreen, Screen->ScreenID);
		}
	}

	CalibrationSaveGame->SavedScreenIDOrder = CurrentScreenOrder;
	for (int32 SavedScreenID : PreviousSavedOrder)
	{
		if (!CalibrationSaveGame->SavedScreenIDOrder.Contains(SavedScreenID))
		{
			CalibrationSaveGame->SavedScreenIDOrder.Add(SavedScreenID);
		}
	}

	CalibrationSaveGame->ScreenIDByPersistentKey.Reset();
	for (const AHUICRSyncScreen* Screen : RegisteredScreens)
	{
		if (!Screen->PersistentScreenKey.IsEmpty())
		{
			CalibrationSaveGame->ScreenIDByPersistentKey.Add(Screen->PersistentScreenKey, Screen->ScreenID);
		}
	}
}

bool AHUICRSyncHMDManagerBase::LoadCalibrationData()
{
	USaveGame* LoadedSave = UGameplayStatics::LoadGameFromSlot(CalibrationSaveSlotName, CalibrationSaveUserIndex);
	CalibrationSaveGame = Cast<UHUICRSyncCalibrationSaveGame>(LoadedSave);

	if (!CalibrationSaveGame)
	{
		CalibrationSaveGame = Cast<UHUICRSyncCalibrationSaveGame>(
			UGameplayStatics::CreateSaveGameObject(UHUICRSyncCalibrationSaveGame::StaticClass()));
	}

	if (!CalibrationSaveGame)
	{
		return false;
	}

	SavedPCIpAddress = CalibrationSaveGame->PCIpAddress;
	SavedTargetPCSyncPort = CalibrationSaveGame->TargetPCSyncPort;
	RemoteIpAddress = SavedPCIpAddress;
	RemoteSyncPort = SavedTargetPCSyncPort;
	RetargetPawnScale = CalibrationSaveGame->RetargetPawnScale;
	bMotionParallaxState = CalibrationSaveGame->bMotionParallaxState;
	if (SyncSubsystem)
	{
		if (!SyncSubsystem->UsesManagedCalibration())
		{
			SyncSubsystem->bMotionParallaxState = bMotionParallaxState;
		}
		const bool bNeedsScaleRecalibration = CalibrationSaveGame->AppliedLayoutId.IsValid() &&
			!CalibrationSaveGame->bUsedCalibratorScaleForHost && !CalibrationSaveGame->bScaledGuestScreenLayout;
		const bool bWasGuest = CalibrationSaveGame->bUsesGuestScreenLayout || !CalibrationSaveGame->GuestScreenTransforms.IsEmpty();
		if (CalibrationSaveGame->bHasSavedHMDPCTransforms && (!bNeedsScaleRecalibration || bWasGuest))
		{
			SyncSubsystem->SetLocalCalibrationMapping(CalibrationSaveGame->PCToHMDTransform,
				CalibrationSaveGame->HMDToPCTransform, CalibrationSaveGame->GuestActorMapping);
			bHasLocalCalibrationReference = true;
		}
		SyncSubsystem->RestoreLocalCalibrationId(bNeedsScaleRecalibration ? FGuid() : CalibrationSaveGame->AppliedLayoutId);
	}

	RestoreRegisteredScreenIDsAndCalibration();

	for (AHUICRSyncCalibrator_HMD* Calibrator : RegisteredCalibrators)
	{
		if (IsValid(Calibrator))
		{
			Calibrator->InitCalibrator(CalibrationSaveGame);
		}
	}

	return true;
}

void AHUICRSyncHMDManagerBase::RestoreGuestActorMapping(const FTransform& SavedPCMain)
{
	if (!SyncSubsystem || !CalibrationSaveGame || !CalibrationSaveGame->bHasSavedHMDPCTransforms ||
		!CalibrationSaveGame->bScaledGuestScreenLayout || SyncSubsystem->GetGuestActorMapping().IsValid() ||
		!SavedPCMain.IsValid() || SavedPCMain.GetScale3D().GetAbsMin() <= UE_SMALL_NUMBER)
	{
		return;
	}
	FHUICRSyncGuestActorMapping Mapping;
	Mapping.PCMain = SavedPCMain;
	Mapping.PCMain.SetScale3D(FVector::OneVector);
	Mapping.HMDMain = Mapping.PCMain * CalibrationSaveGame->PCToHMDTransform;
	Mapping.HMDMain.SetScale3D(FVector::OneVector);
	Mapping.ScaleRatio = CalibrationSaveGame->HUIScale / SavedPCMain.GetScale3D();
	if (Mapping.IsValid())
	{
		SyncSubsystem->SetLocalCalibrationMapping(CalibrationSaveGame->PCToHMDTransform,
			CalibrationSaveGame->HMDToPCTransform, Mapping);
	}
}

bool AHUICRSyncHMDManagerBase::SaveCalibrationData()
{
	if (!CalibrationSaveGame && !LoadCalibrationData())
	{
		return false;
	}

	if (!CalibrationSaveGame)
	{
		return false;
	}

	CalibrationSaveGame->PCIpAddress = RemoteIpAddress;
	CalibrationSaveGame->TargetPCSyncPort = RemoteSyncPort;
	CalibrationSaveGame->RetargetPawnScale = RetargetPawnScale;
	CalibrationSaveGame->bMotionParallaxState = bMotionParallaxState;
	if (SyncSubsystem && bHasLocalCalibrationReference)
	{
		CalibrationSaveGame->bHasSavedHMDPCTransforms = true;
		CalibrationSaveGame->HMDToPCTransform = SyncSubsystem->HMDToPCTransform;
		CalibrationSaveGame->PCToHMDTransform = SyncSubsystem->PCToHMDTransform;
		CalibrationSaveGame->GuestActorMapping = SyncSubsystem->GetGuestActorMapping();
		if (CalibrationSaveGame->GuestActorMapping.IsValid())
		{
			CalibrationSaveGame->PCToHMDTransform.SetScale3D(FVector::OneVector);
		}
	}
	PrepareRegisteredScreensForSave();

	AHUICRSyncCalibrator_HMD* MainCalibrator = nullptr;
	for (AHUICRSyncCalibrator_HMD* Calibrator : RegisteredCalibrators)
	{
		if (IsValid(Calibrator) && Calibrator->GetEffectiveCalibratorID() == 0)
		{
			MainCalibrator = Calibrator;
			break;
		}
	}

	if (!MainCalibrator && RegisteredCalibrators.Num() > 0)
	{
		MainCalibrator = RegisteredCalibrators[0];
	}

	if (IsValid(MainCalibrator))
	{
		CalibrationSaveGame->HUIScale = MainCalibrator->GetActorScale3D();
	}

	for (AHUICRSyncScreen* Screen : RegisteredScreens)
	{
		if (IsValid(Screen))
		{
			Screen->SaveScreenCalibration(CalibrationSaveGame);
		}
	}

	return UGameplayStatics::SaveGameToSlot(CalibrationSaveGame, CalibrationSaveSlotName, CalibrationSaveUserIndex);
}

bool AHUICRSyncHMDManagerBase::SaveScreenData()
{
	if (!CalibrationSaveGame && !LoadCalibrationData())
	{
		return false;
	}

	if (!CalibrationSaveGame)
	{
		return false;
	}

	PrepareRegisteredScreensForSave();

	for (AHUICRSyncScreen* Screen : RegisteredScreens)
	{
		if (IsValid(Screen))
		{
			Screen->SaveScreenCalibration(CalibrationSaveGame);
		}
	}

	return UGameplayStatics::SaveGameToSlot(CalibrationSaveGame, CalibrationSaveSlotName, CalibrationSaveUserIndex);
}

void AHUICRSyncHMDManagerBase::RegisterScreen(AHUICRSyncScreen* Screen)
{
	if (!IsValid(Screen))
	{
		return;
	}

	const bool bAlreadyRegistered = RegisteredScreens.Contains(Screen);
	RegisteredScreens.AddUnique(Screen);
	Screen->InitScreen(nullptr);

	if (CalibrationSaveGame && !bAlreadyRegistered)
	{
		RestoreRegisteredScreenIDsAndCalibration();
	}

	if (!IsValid(ManipulatedTargetScreen) || Screen->ScreenID == 0)
	{
		ManipulatedTargetScreen = Screen;
	}
}

void AHUICRSyncHMDManagerBase::RegisterCalibrator(AHUICRSyncCalibrator_HMD* Calibrator)
{
	if (!IsValid(Calibrator))
	{
		return;
	}

	RegisteredCalibrators.AddUnique(Calibrator);

	if (CalibrationSaveGame)
	{
		Calibrator->InitCalibrator(CalibrationSaveGame);
	}

	if (!IsValid(ManipulatedTargetCalibrator) || Calibrator->GetEffectiveCalibratorID() == 0)
	{
		ManipulatedTargetCalibrator = Calibrator;
	}
}

void AHUICRSyncHMDManagerBase::RegisterCalibrationActorsInWorld()
{
	TArray<AActor*> FoundScreens;
	UGameplayStatics::GetAllActorsOfClass(GetWorld(), AHUICRSyncScreen::StaticClass(), FoundScreens);
	for (AActor* Actor : FoundScreens)
	{
		RegisterScreen(Cast<AHUICRSyncScreen>(Actor));
	}

	TArray<AActor*> FoundCalibrators;
	UGameplayStatics::GetAllActorsOfClass(GetWorld(), AHUICRSyncCalibrator_HMD::StaticClass(), FoundCalibrators);
	for (AActor* Actor : FoundCalibrators)
	{
		RegisterCalibrator(Cast<AHUICRSyncCalibrator_HMD>(Actor));
	}

	if (bAutoSpawnCalibratorsForRegisteredScreens)
	{
		SpawnCalibrators();
	}

	if (!SyncSubsystem || !SyncSubsystem->bIsLocalHMDCalibrating)
	{
		RestoreSavedGuestScreens();
	}

	if (!IsValid(ManipulatedTargetScreen) && RegisteredScreens.Num() > 0)
	{
		ManipulatedTargetScreen = RegisteredScreens[0];
	}

	if (!IsValid(ManipulatedTargetCalibrator))
	{
		SelectCalibrator();
	}
}

int32 AHUICRSyncHMDManagerBase::SpawnCalibrators()
{
	if (!GetWorld() || !CalibratorClass)
	{
		UE_LOG(LogTemp, Warning, TEXT("HUICRSync cannot spawn calibrators because CalibratorClass is not configured."));
		return 0;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.bNoFail = true;
	SpawnParams.Owner = this;

	int32 SpawnedCount = 0;

	for (AHUICRSyncScreen* Screen : RegisteredScreens)
	{
		if (!IsValid(Screen))
		{
			continue;
		}

		const bool bAlreadyHasBoundCalibrator = RegisteredCalibrators.ContainsByPredicate(
			[Screen](const AHUICRSyncCalibrator_HMD* Calibrator)
			{
				return IsValid(Calibrator) && Calibrator->BoundScreen == Screen;
			});

		if (bAlreadyHasBoundCalibrator)
		{
			continue;
		}

		AHUICRSyncCalibrator_HMD* NewCalibrator = GetWorld()->SpawnActor<AHUICRSyncCalibrator_HMD>(
			CalibratorClass,
			Screen->GetActorLocation(),
			Screen->GetActorRotation(),
			SpawnParams);

		if (!IsValid(NewCalibrator))
		{
			continue;
		}

		NewCalibrator->BoundScreen = Screen;
		NewCalibrator->CalibratorID = Screen->ScreenID;
		RegisterCalibrator(NewCalibrator);
		SpawnedCount++;

	}

	SelectCalibrator();
	return SpawnedCount;
}

AHUICRSyncCalibrator_HMD* AHUICRSyncHMDManagerBase::FindRegisteredCalibratorByID(int32 CalibratorID) const
{
	for (AHUICRSyncCalibrator_HMD* Calibrator : RegisteredCalibrators)
	{
		if (IsValid(Calibrator) && Calibrator->GetEffectiveCalibratorID() == CalibratorID)
		{
			return Calibrator;
		}
	}

	return nullptr;
}

FHUICRSyncScreenPayload AHUICRSyncHMDManagerBase::ConvertScreenPayloadHMDToPC(const FHUICRSyncScreenPayload& Payload) const
{
	FHUICRSyncScreenPayload ConvertedPayload = Payload;
	if (SyncSubsystem)
	{
		ConvertedPayload.Transform = SyncSubsystem->ConvertHMDTransformToPC(Payload.Transform);
		ConvertedPayload.InitialTransform = SyncSubsystem->ConvertHMDTransformToPC(Payload.InitialTransform);
	}
	return ConvertedPayload;
}

FHUICRSyncScreenPayload AHUICRSyncHMDManagerBase::ConvertScreenPayloadPCToHMD(const FHUICRSyncScreenPayload& Payload) const
{
	FHUICRSyncScreenPayload ConvertedPayload = Payload;
	if (SyncSubsystem)
	{
		ConvertedPayload.Transform = SyncSubsystem->ConvertPCTransformToHMD(Payload.Transform);
		ConvertedPayload.InitialTransform = SyncSubsystem->ConvertPCTransformToHMD(Payload.InitialTransform);
	}
	return ConvertedPayload;
}
bool AHUICRSyncHMDManagerBase::SendCalibrationAndScreens()
{
	if (!SyncSubsystem)
	{
		return false;
	}

	if (!bHasLocalCalibrationReference)
	{
		UE_LOG(LogTemp, Warning, TEXT("HUICRSync HMD cannot send calibration screens before PC calibration reference is applied."));
		return false;
	}

	bool bQueuedAnyPayload = false;
	for (AHUICRSyncScreen* Screen : RegisteredScreens)
	{
		if (IsValid(Screen))
		{
			bQueuedAnyPayload |= QueueScreenPayload(ConvertScreenPayloadHMDToPC(Screen->BuildScreenPayload()));
		}
	}

	if (bQueuedAnyPayload)
	{
		SyncSubsystem->SendAllQueuedStates();
	}

	return bQueuedAnyPayload;
}

bool AHUICRSyncHMDManagerBase::RequestPCCalibrationReference()
{
	if (!SyncSubsystem && !InitializeManager())
	{
		return false;
	}

	if (!SyncSubsystem)
	{
		return false;
	}

	bWaitingForPCCalibrationReference = true;
	const bool bRequestSent = SyncSubsystem->SendReliableCommand(TEXT("/FromHMD/Calibration/RequestPCReference"), {});
	if (!bRequestSent)
	{
		UE_LOG(LogTemp, Warning, TEXT("HUICRSync failed to request PC calibration reference."));
	}
	return bRequestSent;
}

bool AHUICRSyncHMDManagerBase::RecalculateLocalHMDPCTransforms(const FHUICRSyncCalibratorPayload& PCCalibratorPayload)
{
	if (!SyncSubsystem)
	{
		return false;
	}

	AHUICRSyncCalibrator_HMD* HMDCalibrator = FindRegisteredCalibratorByID(PCCalibratorPayload.ActorID);
	if (!IsValid(HMDCalibrator))
	{
		HMDCalibrator = FindRegisteredCalibratorByID(0);
	}
	if (!IsValid(HMDCalibrator) && RegisteredCalibrators.Num() > 0)
	{
		HMDCalibrator = RegisteredCalibrators[0];
	}
	if (!IsValid(HMDCalibrator))
	{
		UE_LOG(LogTemp, Warning, TEXT("HUICRSync HMD cannot calculate local transforms because no HMD calibrator is registered."));
		return false;
	}

	LastPCCalibratorPayload = PCCalibratorPayload;
	const FTransform HMDCalibratorTransform = HMDCalibrator->GetActorTransform();
	SyncSubsystem->HMDToPCTransform = HMDCalibratorTransform.Inverse() * PCCalibratorPayload.Transform;
	SyncSubsystem->PCToHMDTransform = PCCalibratorPayload.Transform.Inverse() * HMDCalibratorTransform;
	bHasLocalCalibrationReference = true;

	if (CalibrationSaveGame || LoadCalibrationData())
	{
		CalibrationSaveGame->bHasSavedHMDPCTransforms = true;
		CalibrationSaveGame->HMDToPCTransform = SyncSubsystem->HMDToPCTransform;
		CalibrationSaveGame->PCToHMDTransform = SyncSubsystem->PCToHMDTransform;
		UGameplayStatics::SaveGameToSlot(CalibrationSaveGame, CalibrationSaveSlotName, CalibrationSaveUserIndex);
	}

	return true;
}

bool AHUICRSyncHMDManagerBase::SendCalibrationReferenceAck()
{
	if (!SyncSubsystem && !InitializeManager())
	{
		return false;
	}

	return SyncSubsystem ? SyncSubsystem->SendReliableCommand(TEXT("/FromHMD/Calibration/ReferenceApplied"), {}) : false;
}

AHUICRSyncScreen* AHUICRSyncHMDManagerBase::FindUniqueMainScreen() const
{
	AHUICRSyncScreen* Main = nullptr;
	for (AHUICRSyncScreen* Screen : RegisteredScreens)
	{
		if (IsValid(Screen) && Screen->ScreenID == 0)
		{
			if (Main)
			{
				return nullptr;
			}
			Main = Screen;
		}
	}
	return Main;
}

void AHUICRSyncHMDManagerBase::BeginLocalCalibrationTurn(const TMap<int32, bool>& SavedScreenControls)
{
	PreCalibrationTransform = SyncSubsystem->PCToHMDTransform;
	PreCalibrationInverse = SyncSubsystem->HMDToPCTransform;
	PreCalibrationGuestActorMapping = SyncSubsystem->GetGuestActorMapping();
	bHadLocalCalibrationReference = bHasLocalCalibrationReference;
	PreCalibrationScreens.Reset();
	NewScreensThisTurn.Reset();
	bAppliedCurrentTurn = false;
	PendingAppliedLayoutId.Invalidate();
	PendingGuestScreens.Reset();
	for (AHUICRSyncScreen* Screen : RegisteredScreens)
	{
		if (IsValid(Screen))
		{
			if (const bool* bControlsPC = SavedScreenControls.Find(Screen->ScreenID))
			{
				Screen->bControlRemoteScreen = *bControlsPC;
			}
			PreCalibrationScreens.Add(Screen, {Screen->GetActorTransform(), Screen->ScreenID, Screen->bControlRemoteScreen});
		}
	}
	SelectScreenByID(0);
	SelectCalibratorByID(0);
}

void AHUICRSyncHMDManagerBase::EndLocalCalibrationTurn(bool bAccepted)
{
	if (bAccepted && bAppliedCurrentTurn)
	{
		if (CalibrationSaveGame)
		{
			CalibrationSaveGame->AppliedLayoutId = PendingAppliedLayoutId;
			CalibrationSaveGame->GuestScreenTransforms = PendingGuestScreens;
			CalibrationSaveGame->bUsesGuestScreenLayout = bPendingGuestLayout;
			CalibrationSaveGame->bUsedCalibratorScaleForHost = !bPendingGuestLayout;
			CalibrationSaveGame->bScaledGuestScreenLayout = bPendingGuestLayout;
		}
		if (!SaveCalibrationData())
		{
			SyncSubsystem->SetCalibrationMessage(TEXT("Could not save this headset's calibration."));
		}
	}
	else
	{
		SyncSubsystem->SetLocalCalibrationMapping(PreCalibrationTransform, PreCalibrationInverse,
			PreCalibrationGuestActorMapping);
		bHasLocalCalibrationReference = bHadLocalCalibrationReference;
		for (const auto& Pair : PreCalibrationScreens)
		{
			if (AHUICRSyncScreen* Screen = Pair.Key.Get())
			{
				Screen->ChangeScreenID(Pair.Value.ScreenID);
				Screen->SetActorTransform(Pair.Value.Transform);
				Screen->bControlRemoteScreen = Pair.Value.bControlsPC;
			}
		}
		for (const auto& Screen : NewScreensThisTurn)
		{
			if (Screen.IsValid())
			{
				RegisteredScreens.Remove(Screen.Get());
				Screen->Destroy();
			}
		}
	}
	bAppliedCurrentTurn = false;
	PreCalibrationScreens.Reset();
	NewScreensThisTurn.Reset();
	SpawnDissolveBoxesForRegisteredScreens();
}

bool AHUICRSyncHMDManagerBase::FinishCalibrationTurn()
{
	AHUICRSyncScreen* Main = FindUniqueMainScreen();
	if (!Main)
	{
		SyncSubsystem->SetCalibrationMessage(TEXT("Set exactly one main screen to Screen ID 0."));
		return false;
	}
	FHUICRSyncCalibrationLayout Measurements;
	const FTransform MainScreenTransform = Main->GetActorTransform();
	const bool bHostTurn = SyncSubsystem->bCanEditPCScreenLayout;
	FTransform LocalReference = MainScreenTransform;
	if (AHUICRSyncCalibrator_HMD* Calibrator = FindRegisteredCalibratorByID(0))
	{
		Calibrator->SetupTransformWithScreen();
		if (bHostTurn)
		{
			LocalReference = Calibrator->GetActorTransform();
		}
		else
		{
			LocalReference.SetScale3D(Calibrator->GetActorScale3D());
		}
	}
	else
	{
		LocalReference.SetScale3D(CalibrationSaveGame ? CalibrationSaveGame->HUIScale : FVector::OneVector);
	}
	if (!LocalReference.IsValid() || LocalReference.GetScale3D().GetAbsMin() <= UE_SMALL_NUMBER)
	{
		SyncSubsystem->SetCalibrationMessage(TEXT("Set a valid main calibrator scale before stopping."));
		return false;
	}
	Measurements.MainReference = LocalReference;
	if (!bHostTurn)
	{
		Measurements.MainReference.SetScale3D(FVector::OneVector);
	}
	if (bHostTurn)
	{
		TSet<int32> UsedIDs;
		for (AHUICRSyncScreen* Screen : RegisteredScreens)
		{
			if (!IsValid(Screen))
			{
				continue;
			}
			if (Screen->ScreenID < 0 || UsedIDs.Contains(Screen->ScreenID))
			{
				SyncSubsystem->SetCalibrationMessage(TEXT("Give each screen a different Screen ID."));
				return false;
			}
			UsedIDs.Add(Screen->ScreenID);
			const FHUICRSyncScreenPayload& Payload = Measurements.Screens.Add_GetRef(Screen->BuildScreenPayload());
			const FCalibrationScreenState* Before = PreCalibrationScreens.Find(Screen);
			if (Payload.bRemoteControlsLocalScreen &&
				(!Before || Before->ScreenID != Payload.ScreenID || !Before->Transform.Equals(Payload.Transform)))
			{
				Measurements.EditedScreenIds.Add(Payload.ScreenID);
			}
		}
	}
	if (!SyncSubsystem->SubmitCalibration(Measurements))
	{
		return false;
	}
	SubmittedMainReference = LocalReference;
	SubmittedMainScreenTransform = MainScreenTransform;
	return true;
}

AHUICRSyncScreen* AHUICRSyncHMDManagerBase::SpawnLayoutScreen(int32 ScreenID, const FTransform& Transform)
{
	AHUICRSyncScreen* Main = FindUniqueMainScreen();
	if (!Main)
	{
		return nullptr;
	}
	AHUICRSyncScreen* NewScreen = GetWorld()->SpawnActorDeferred<AHUICRSyncScreen>(
		Main->GetClass(), Transform, this, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!NewScreen)
	{
		return nullptr;
	}
	NewScreen->ScreenID = ScreenID;
	NewScreen->bAutoAssignScreenID = false;
	NewScreen->PersistentScreenKey = FString::Printf(TEXT("PCLayout:%d"), ScreenID);
	NewScreen->FinishSpawning(Transform);
	NewScreen->InitScreen(nullptr);
	RegisteredScreens.AddUnique(NewScreen);
	SyncSubsystem->ReserveActorID(EHUICRSyncActorType::HUIScreen, ScreenID);
	return NewScreen;
}

bool AHUICRSyncHMDManagerBase::ApplyCalibrationLayout(const FHUICRSyncCalibrationLayout& Layout, bool bHostTurn)
{
	AHUICRSyncScreen* Main = FindUniqueMainScreen();
	if (!Main || !Layout.Screens.ContainsByPredicate([](const FHUICRSyncScreenPayload& Screen) { return Screen.ScreenID == 0; }))
	{
		return false;
	}
	if (!CalibrationSaveGame)
	{
		// Loading here would discard the adjustments made during this turn.
		CalibrationSaveGame = NewObject<UHUICRSyncCalibrationSaveGame>(this);
	}
	const FVector PCCalibratorScale = Layout.MainReference.GetScale3D();
	if (PCCalibratorScale.GetAbsMin() <= UE_SMALL_NUMBER)
	{
		return false;
	}
	FTransform LocalReference = SubmittedMainReference;
	const FTransform PCReference = Layout.GetMainReferenceForTurn(bHostTurn);
	const FVector GuestScaleRatio = bHostTurn ? FVector::OneVector : LocalReference.GetScale3D() / PCCalibratorScale;
	if (!bHostTurn)
	{
		// Convert screen offsets in the main screen's axes so rotation and non-uniform scale stay separate.
		LocalReference.SetScale3D(FVector::OneVector);
	}
	const FTransform ToHMD = PCReference.Inverse() * LocalReference;
	const auto ResolveScreenTransform = [&](const FTransform& Transform)
	{
		FTransform Resolved = Transform * ToHMD;
		if (!bHostTurn)
		{
			const FVector OffsetFromPCMain = PCReference.InverseTransformPosition(Transform.GetLocation());
			Resolved.SetLocation(LocalReference.TransformPosition(OffsetFromPCMain * GuestScaleRatio));
			Resolved.SetScale3D(Resolved.GetScale3D() * GuestScaleRatio);
		}
		return Resolved;
	};
	TMap<int32, AHUICRSyncScreen*> ExistingScreens;
	for (AHUICRSyncScreen* Screen : RegisteredScreens)
	{
		if (IsValid(Screen))
		{
			ExistingScreens.FindOrAdd(Screen->ScreenID, Screen);
		}
	}
	for (const FHUICRSyncScreenPayload& Screen : Layout.Screens)
	{
		if (!ExistingScreens.Contains(Screen.ScreenID))
		{
			AHUICRSyncScreen* NewScreen = SpawnLayoutScreen(Screen.ScreenID, ResolveScreenTransform(Screen.Transform));
			if (!NewScreen)
			{
				for (const auto& Created : NewScreensThisTurn)
				{
					if (Created.IsValid())
					{
						RegisteredScreens.Remove(Created.Get());
						Created->Destroy();
					}
				}
				NewScreensThisTurn.Reset();
				return false;
			}
			NewScreensThisTurn.Add(NewScreen);
			ExistingScreens.Add(Screen.ScreenID, NewScreen);
		}
	}
	FHUICRSyncGuestActorMapping ActorMapping;
	if (!bHostTurn)
	{
		ActorMapping.PCMain = PCReference;
		ActorMapping.HMDMain = LocalReference;
		ActorMapping.ScaleRatio = GuestScaleRatio;
	}
	SyncSubsystem->SetLocalCalibrationMapping(ToHMD, LocalReference.Inverse() * PCReference, ActorMapping);
	Main->SetActorTransform(SubmittedMainScreenTransform);
	LastPCCalibratorPayload.Transform = PCReference;
	bHasLocalCalibrationReference = true;
	PendingGuestScreens.Reset();
	for (const FHUICRSyncScreenPayload& Screen : Layout.Screens)
	{
		AHUICRSyncScreen* LocalScreen = ExistingScreens.FindRef(Screen.ScreenID);
		if (bHostTurn)
		{
			LocalScreen->bControlRemoteScreen = Screen.bRemoteControlsLocalScreen;
			if (Layout.EditedScreenIds.Contains(Screen.ScreenID))
			{
				continue;
			}
		}
		FHUICRSyncScreenPayload Resolved = Screen;
		Resolved.Transform = ResolveScreenTransform(Screen.Transform);
		Resolved.InitialTransform = ResolveScreenTransform(Screen.InitialTransform);
		Resolved.bRemoteControlsLocalScreen = false;
		const bool bPrivateControlPreference = LocalScreen->bControlRemoteScreen;
		LocalScreen->ApplyScreenPayload(Resolved);
		LocalScreen->bControlRemoteScreen = bPrivateControlPreference;
		if (!bHostTurn && Screen.ScreenID != 0)
		{
			PendingGuestScreens.Add(Screen.ScreenID, Resolved.Transform.GetRelativeTransform(LocalReference));
		}
		OnScreenPayloadReceived.Broadcast(Resolved);
	}
	PendingAppliedLayoutId = Layout.Id;
	bPendingGuestLayout = !bHostTurn;
	bAppliedCurrentTurn = true;
	return true;
}

void AHUICRSyncHMDManagerBase::RestoreSavedGuestScreens()
{
	const AHUICRSyncScreen* Main = FindUniqueMainScreen();
	if (!Main || !CalibrationSaveGame || CalibrationSaveGame->GuestScreenTransforms.IsEmpty())
	{
		return;
	}
	FTransform MainReference = Main->GetActorTransform();
	MainReference.SetScale3D(FVector::OneVector);
	for (const auto& Pair : CalibrationSaveGame->GuestScreenTransforms)
	{
		const FTransform Pose = Pair.Value * MainReference;
		AHUICRSyncScreen** Found = RegisteredScreens.FindByPredicate([&Pair](const AHUICRSyncScreen* Screen)
		{
			return IsValid(Screen) && Screen->ScreenID == Pair.Key;
		});
		if (Found)
		{
			(*Found)->SetActorTransform(Pose);
		}
		else
		{
			SpawnLayoutScreen(Pair.Key, Pose);
		}
	}
}

int32 AHUICRSyncHMDManagerBase::SpawnDissolveBoxesForRegisteredScreens()
{
	int32 SpawnedCount = 0;
	TSet<int32> SpawnedIDs;
	for (AHUICRSyncScreen* Screen : RegisteredScreens)
	{
		if (IsValid(Screen) && Screen->bSpawnDissolveBoxOnInit)
		{
			if (CalibrationSaveGame && CalibrationSaveGame->bUsesGuestScreenLayout && Screen->ScreenID != 0 &&
				(!CalibrationSaveGame->GuestScreenTransforms.Contains(Screen->ScreenID) || SpawnedIDs.Contains(Screen->ScreenID)))
			{
				Screen->DestroyDissolveBoxActor();
				continue;
			}
			Screen->SpawnDissolveBoxActor();
			SpawnedIDs.Add(Screen->ScreenID);
			SpawnedCount++;
		}
	}

	return SpawnedCount;
}

void AHUICRSyncHMDManagerBase::ChangeCalibrationState()
{
	if (!SyncSubsystem && !InitializeManager())
	{
		return;
	}

	if (!SyncSubsystem)
	{
		return;
	}

	if (SyncSubsystem->UsesManagedCalibration())
	{
		if (SyncSubsystem->bIsLocalHMDCalibrating)
		{
			FinishCalibrationTurn();
		}
		else
		{
			SyncSubsystem->RequestCalibrationTurn();
		}
		return;
	}

	SyncSubsystem->bCalibrationState = !SyncSubsystem->bCalibrationState;

	if (SyncSubsystem->bCalibrationState)
	{
		SyncSubsystem->bGameStopped = true;
		bHasLocalCalibrationReference = false;
		bWaitingForPCCalibrationReference = false;
		SyncSubsystem->ClearLocalSyncActors();
		SyncSubsystem->SendReliableCommandBool(TEXT("/FromHMD/Calibration/CalibrationState"), true);
		ApplyMotionParallaxState(false, false, false);
	}

	SyncSubsystem->OnCalibrationStateChanged.Broadcast(SyncSubsystem->bCalibrationState);

	if (!SyncSubsystem->bCalibrationState)
	{
		SaveCalibrationData();
		RequestPCCalibrationReference();
		ApplyMotionParallaxState(bMotionParallaxState, false, false);
	}
}

void AHUICRSyncHMDManagerBase::HandleSyncSceneReady()
{
	SpawnDissolveBoxesForRegisteredScreens();
}

void AHUICRSyncHMDManagerBase::ChangeMotionParallaxState(bool bNewState)
{
	ApplyMotionParallaxState(bNewState, true, true);
}

bool AHUICRSyncHMDManagerBase::ApplyMotionParallaxState(bool bNewState, bool bUpdatePreference, bool bSavePreference)
{
	if (!SyncSubsystem && !InitializeManager())
	{
		return false;
	}

	if (!SyncSubsystem)
	{
		return false;
	}

	if (SyncSubsystem->UsesManagedCalibration() && !SyncSubsystem->bCanEditGameMotionParallax)
	{
		return false;
	}

	if (bUpdatePreference)
	{
		bMotionParallaxState = bNewState;
		if (!CalibrationSaveGame)
		{
			LoadCalibrationData();
		}

		if (CalibrationSaveGame)
		{
			CalibrationSaveGame->bMotionParallaxState = bNewState;
			if (bSavePreference)
			{
				UGameplayStatics::SaveGameToSlot(CalibrationSaveGame, CalibrationSaveSlotName, CalibrationSaveUserIndex);
			}
		}
	}

	if (!SyncSubsystem->UsesManagedCalibration())
	{
		SyncSubsystem->bMotionParallaxState = bNewState;
		SyncSubsystem->OnMotionParallaxStateChanged.Broadcast(bNewState);
	}
	const bool bCommandSent = SyncSubsystem->SendReliableCommandBool(TEXT("/FromHMD/MotionParallaxState"), bNewState);
	if (!bCommandSent)
	{
		UE_LOG(LogTemp, Warning, TEXT("HUICRSync failed to send MotionParallaxState=%s to PC. Check connection/default peer."), bNewState ? TEXT("true") : TEXT("false"));
	}

	return bCommandSent;
}

bool AHUICRSyncHMDManagerBase::SendSelectedScreenID(int32 ScreenID)
{
	if (!SyncSubsystem && !InitializeManager())
	{
		return false;
	}

	if (!SyncSubsystem)
	{
		return false;
	}
	if (SyncSubsystem->UsesManagedCalibration())
	{
		return SyncSubsystem->RequestCalibrationPreview(ScreenID);
	}

	FHUICRSyncCommandArg ScreenIDArg;
	ScreenIDArg.Type = EHUICRSyncCommandArgType::Int32;
	ScreenIDArg.IntValue = ScreenID;

	TArray<FHUICRSyncCommandArg> Args;
	Args.Add(ScreenIDArg);
	const bool bQueuedCommand = SyncSubsystem->SendReliableCommand(TEXT("/FromHMD/Calibration/SelectedScreenID"), Args);

	return bQueuedCommand;
}

void AHUICRSyncHMDManagerBase::SelectScreen(AHUICRSyncScreen* SelectedScreen)
{
	if (IsValid(SelectedScreen))
	{
		ManipulatedTargetScreen = SelectedScreen;
		RegisteredScreens.AddUnique(SelectedScreen);
		if (SyncSubsystem && SyncSubsystem->UsesManagedCalibration())
		{
			SyncSubsystem->RequestCalibrationPreview(SelectedScreen->ScreenID);
		}
	}
}

bool AHUICRSyncHMDManagerBase::SelectScreenByID(int32 ScreenID)
{
	for (AHUICRSyncScreen* Screen : RegisteredScreens)
	{
		if (IsValid(Screen) && Screen->ScreenID == ScreenID)
		{
			ManipulatedTargetScreen = Screen;
			if (SyncSubsystem && SyncSubsystem->UsesManagedCalibration())
			{
				SyncSubsystem->RequestCalibrationPreview(ScreenID);
			}
			return true;
		}
	}

	return false;
}

void AHUICRSyncHMDManagerBase::SelectCalibrator()
{
	if (SelectCalibratorByID(0))
	{
		return;
	}

	for (AHUICRSyncCalibrator_HMD* Calibrator : RegisteredCalibrators)
	{
		if (IsValid(Calibrator))
		{
			ManipulatedTargetCalibrator = Calibrator;
			return;
		}
	}
}

bool AHUICRSyncHMDManagerBase::SelectCalibratorByID(int32 CalibratorID)
{
	for (AHUICRSyncCalibrator_HMD* Calibrator : RegisteredCalibrators)
	{
		if (IsValid(Calibrator) && Calibrator->GetEffectiveCalibratorID() == CalibratorID)
		{
			ManipulatedTargetCalibrator = Calibrator;
			return true;
		}
	}

	return false;
}

void AHUICRSyncHMDManagerBase::MoveScreen(FVector DeltaVector)
{
	if (IsValid(ManipulatedTargetScreen))
	{
		ManipulatedTargetScreen->AddActorLocalOffset(DeltaVector * ManipulateAmplitude);
	}
}

void AHUICRSyncHMDManagerBase::RotateScreen(FRotator DeltaRotator)
{
	if (IsValid(ManipulatedTargetScreen))
	{
		ManipulatedTargetScreen->AddActorLocalRotation(DeltaRotator * ManipulateAmplitude);
	}
}

void AHUICRSyncHMDManagerBase::ScaleScreen(FVector DeltaVector)
{
	if (IsValid(ManipulatedTargetScreen))
	{
		ManipulatedTargetScreen->SetActorScale3D(
			(ManipulatedTargetScreen->GetActorScale3D() * 100.0 + DeltaVector * ManipulateAmplitude) / 100.0);
	}
}

void AHUICRSyncHMDManagerBase::ScaleCalibrator(FVector DeltaVector)
{
	if (!IsValid(ManipulatedTargetCalibrator))
	{
		return;
	}

	const FVector NewScale = ManipulatedTargetCalibrator->GetActorScale3D() + DeltaVector * ManipulateAmplitude;
	for (AHUICRSyncCalibrator_HMD* Calibrator : RegisteredCalibrators)
	{
		if (IsValid(Calibrator))
		{
			Calibrator->SetActorScale3D(NewScale);
		}
	}

	if (CalibrationSaveGame)
	{
		CalibrationSaveGame->HUIScale = NewScale;
	}
}

void AHUICRSyncHMDManagerBase::ResetScreenPosition()
{
	if (IsValid(ManipulatedTargetScreen))
	{
		ManipulatedTargetScreen->SetActorLocation(ManipulatedTargetScreen->InitialTransform.GetLocation());
	}
}

void AHUICRSyncHMDManagerBase::ResetScreenRotation()
{
	if (IsValid(ManipulatedTargetScreen))
	{
		ManipulatedTargetScreen->SetActorRotation(ManipulatedTargetScreen->InitialTransform.Rotator());
	}
}

void AHUICRSyncHMDManagerBase::ResetCalibratorScale()
{
	for (AHUICRSyncCalibrator_HMD* Calibrator : RegisteredCalibrators)
	{
		if (IsValid(Calibrator))
		{
			Calibrator->SetActorScale3D(FVector::OneVector);
		}
	}

	if (CalibrationSaveGame)
	{
		CalibrationSaveGame->HUIScale = FVector::OneVector;
	}
}

void AHUICRSyncHMDManagerBase::SetScreenHidden()
{
	if (IsValid(ManipulatedTargetScreen))
	{
		ManipulatedTargetScreen->SetActorHiddenInGame(!ManipulatedTargetScreen->IsHidden());
	}
}

void AHUICRSyncHMDManagerBase::SetCalibratorHidden()
{
	if (IsValid(ManipulatedTargetCalibrator))
	{
		ManipulatedTargetCalibrator->SetActorHiddenInGame(!ManipulatedTargetCalibrator->IsHidden());
	}
}

void AHUICRSyncHMDManagerBase::SetMotionParallaxStatePreference(bool bNewState)
{
	if (SyncSubsystem && SyncSubsystem->UsesManagedCalibration() && !SyncSubsystem->bCanEditGameMotionParallax)
	{
		return;
	}
	bMotionParallaxState = bNewState;

	if (!CalibrationSaveGame)
	{
		LoadCalibrationData();
	}

	if (CalibrationSaveGame)
	{
		CalibrationSaveGame->bMotionParallaxState = bNewState;
		UGameplayStatics::SaveGameToSlot(CalibrationSaveGame, CalibrationSaveSlotName, CalibrationSaveUserIndex);
	}
}

void AHUICRSyncHMDManagerBase::UpdateRetargetPawnScale(const FVector& ActorScale)
{
	RetargetPawnScale = ActorScale;

	if (CalibrationSaveGame)
	{
		CalibrationSaveGame->RetargetPawnScale = ActorScale;
	}
}

bool AHUICRSyncHMDManagerBase::SendCalibratorPayload(int32 ActorID, const FTransform& CalibratorTransform)
{
	FHUICRSyncCalibratorPayload Payload;
	Payload.ActorID = ActorID;
	Payload.Transform = CalibratorTransform;
	return QueueCalibratorPayload(Payload);
}

bool AHUICRSyncHMDManagerBase::SendScreenPayload(int32 ScreenID, const FTransform& ScreenTransform, bool bHmdControlsPCScreen)
{
	FHUICRSyncScreenPayload Payload;
	Payload.ScreenID = ScreenID;
	Payload.Transform = ScreenTransform;
	Payload.InitialTransform = ScreenTransform;
	Payload.bRemoteControlsLocalScreen = bHmdControlsPCScreen;
	return QueueScreenPayload(Payload);
}

void AHUICRSyncHMDManagerBase::HandleNetDataReceived()
{
	FHUICRSyncNetEntry Entry;
	FHUICRSyncCalibratorPayload CalibratorPayload;
	if (GetFirstPayload(EHUICRSyncActorType::HUICalibrator, Entry) && DecodeCalibratorEntry(Entry, CalibratorPayload))
	{
		OnCalibratorPayloadReceived.Broadcast(CalibratorPayload);

		if (bWaitingForPCCalibrationReference && RecalculateLocalHMDPCTransforms(CalibratorPayload))
		{
			bWaitingForPCCalibrationReference = false;
			SendCalibrationAndScreens();
			SendCalibrationReferenceAck();
		}
	}

	bool bAppliedScreenPayload = false;

	if (SyncSubsystem)
	{
		if (const FHUICRSyncNetInnerMapEntry* ScreenMap = SyncSubsystem->ReceivedDataMap.Find(EHUICRSyncActorType::HUIScreen))
		{
			for (const TPair<int32, FHUICRSyncNetEntry>& Pair : ScreenMap->InnerMap)
			{
				FHUICRSyncScreenPayload ScreenPayload;
				if (!DecodeScreenEntry(Pair.Value, ScreenPayload))
				{
					continue;
				}

				const FHUICRSyncScreenPayload LocalScreenPayload = ConvertScreenPayloadPCToHMD(ScreenPayload);
				for (AHUICRSyncScreen* Screen : RegisteredScreens)
				{
					if (IsValid(Screen) && Screen->ScreenID == LocalScreenPayload.ScreenID)
					{
						Screen->ApplyScreenPayload(LocalScreenPayload);
						bAppliedScreenPayload = true;
						break;
					}
				}

				OnScreenPayloadReceived.Broadcast(LocalScreenPayload);
			}
		}
	}

	if (bAppliedScreenPayload)
	{
		SaveScreenData();
	}
}
