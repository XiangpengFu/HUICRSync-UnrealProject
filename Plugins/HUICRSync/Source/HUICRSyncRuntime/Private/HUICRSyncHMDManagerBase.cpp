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
	if (PCIpAddress.IsEmpty() || TargetPCSyncPort <= 0)
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

	SyncSubsystem->ConfigureSync(EHUICRSyncRole::HMD, LocalSyncPort, TargetPCSyncPort, PCIpAddress);
	SyncSubsystem->bConnectionState = false;
	
	FHUICRSyncCommandArg IpArg;
	IpArg.Type = EHUICRSyncCommandArgType::String;
	IpArg.StringValue = GetLocalIpAddress();

	FHUICRSyncCommandArg PortArg;
	PortArg.Type = EHUICRSyncCommandArgType::Int32;
	PortArg.IntValue = LocalSyncPort;

	const bool bConnectionRequestSent = SyncSubsystem->SendReliableCommand(TEXT("FromHMD/HMDConnectionRequest"), { IpArg, PortArg });

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
		SyncSubsystem->bMotionParallaxState = bMotionParallaxState;
	}

	for (AHUICRSyncScreen* Screen : RegisteredScreens)
	{
		if (IsValid(Screen))
		{
			Screen->InitScreen(CalibrationSaveGame);
		}
	}

	for (AHUICRSyncCalibrator_HMD* Calibrator : RegisteredCalibrators)
	{
		if (IsValid(Calibrator))
		{
			Calibrator->InitCalibrator(CalibrationSaveGame);
		}
	}

	return true;
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

	RegisteredScreens.AddUnique(Screen);

	if (CalibrationSaveGame)
	{
		Screen->InitScreen(CalibrationSaveGame);
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

bool AHUICRSyncHMDManagerBase::SendCalibrationAndScreens()
{
	if (!SyncSubsystem)
	{
		return false;
	}

	bool bQueuedAnyPayload = false;

	for (AHUICRSyncCalibrator_HMD* Calibrator : RegisteredCalibrators)
	{
		if (IsValid(Calibrator) && Calibrator->GetEffectiveCalibratorID() == 0)
		{
			const FHUICRSyncCalibratorPayload Payload = Calibrator->BuildCalibratorPayload();
			bQueuedAnyPayload |= QueueCalibratorPayload(Payload);
			break;
		}
	}

	for (AHUICRSyncScreen* Screen : RegisteredScreens)
	{
		if (IsValid(Screen))
		{
			bQueuedAnyPayload |= QueueScreenPayload(Screen->BuildScreenPayload());
		}
	}

	if (bQueuedAnyPayload)
	{
		SyncSubsystem->SendAllQueuedStates();
	}

	return bQueuedAnyPayload;
}

int32 AHUICRSyncHMDManagerBase::SpawnDissolveBoxesForRegisteredScreens()
{
	int32 SpawnedCount = 0;
	for (AHUICRSyncScreen* Screen : RegisteredScreens)
	{
		if (IsValid(Screen) && Screen->bSpawnDissolveBoxOnInit)
		{
			Screen->SpawnDissolveBoxActor();
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

	SyncSubsystem->bCalibrationState = !SyncSubsystem->bCalibrationState;

	if (SyncSubsystem->bCalibrationState)
	{
		SyncSubsystem->bGameStopped = true;
		SyncSubsystem->ClearLocalSyncActors();
		SyncSubsystem->SendReliableCommandBool(TEXT("/FromHMD/Calibration/CalibrationState"), true);
		ApplyMotionParallaxState(false, false, false);
	}

	SyncSubsystem->OnCalibrationStateChanged.Broadcast(SyncSubsystem->bCalibrationState);

	if (!SyncSubsystem->bCalibrationState)
	{
		SaveCalibrationData();
		SendCalibrationAndScreens();
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

	SyncSubsystem->bMotionParallaxState = bNewState;
	SyncSubsystem->OnMotionParallaxStateChanged.Broadcast(bNewState);
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
	}
}

bool AHUICRSyncHMDManagerBase::SelectScreenByID(int32 ScreenID)
{
	for (AHUICRSyncScreen* Screen : RegisteredScreens)
	{
		if (IsValid(Screen) && Screen->ScreenID == ScreenID)
		{
			ManipulatedTargetScreen = Screen;
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

				for (AHUICRSyncScreen* Screen : RegisteredScreens)
				{
					if (IsValid(Screen) && Screen->ScreenID == ScreenPayload.ScreenID)
					{
						Screen->ApplyScreenPayload(ScreenPayload);
						bAppliedScreenPayload = true;
						break;
					}
				}

				OnScreenPayloadReceived.Broadcast(ScreenPayload);
			}
		}
	}

	if (bAppliedScreenPayload)
	{
		SaveScreenData();
	}
}
