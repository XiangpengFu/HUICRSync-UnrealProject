#include "HUICRSyncPCManagerBase.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "HUICRSyncCalibrator_PC.h"
#include "HUICRSyncPCScreenVisual.h"
#include "HUICRSyncSubsystem.h"
#include "Kismet/GameplayStatics.h"

#if HUICR_WITH_NDISPLAY
#include "Components/DisplayClusterCameraComponent.h"
#include "DisplayClusterRootActor.h"
#endif

AHUICRSyncPCManagerBase::AHUICRSyncPCManagerBase()
{
	LocalRole = EHUICRSyncRole::PC;
	PCScreenVisualClass = AHUICRSyncPCScreenVisual::StaticClass();
	PCCalibratorClass = AHUICRSyncCalibrator_PC::StaticClass();
}

void AHUICRSyncPCManagerBase::BeginPlay()
{
	Super::BeginPlay();

	if (bInitializeNDisplayOnBeginPlay)
	{
		InitializeNDisplayScreens();
	}

	if (SyncSubsystem)
	{
		SyncSubsystem->OnCalibrationStateChanged.AddDynamic(this, &AHUICRSyncPCManagerBase::OnSubsystemCalibrationStateChanged);
		SyncSubsystem->OnMotionParallaxStateChanged.AddDynamic(this, &AHUICRSyncPCManagerBase::OnSubsystemMotionParallaxStateChanged);
		SyncSubsystem->OnSelectedScreenChanged.AddDynamic(this, &AHUICRSyncPCManagerBase::OnSubsystemSelectedScreenChanged);
		SyncSubsystem->OnCommandReceived.AddDynamic(this, &AHUICRSyncPCManagerBase::OnSubsystemCommandReceived);
	}
}

void AHUICRSyncPCManagerBase::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (SyncSubsystem)
	{
		SyncSubsystem->OnCalibrationStateChanged.RemoveDynamic(this, &AHUICRSyncPCManagerBase::OnSubsystemCalibrationStateChanged);
		SyncSubsystem->OnMotionParallaxStateChanged.RemoveDynamic(this, &AHUICRSyncPCManagerBase::OnSubsystemMotionParallaxStateChanged);
		SyncSubsystem->OnSelectedScreenChanged.RemoveDynamic(this, &AHUICRSyncPCManagerBase::OnSubsystemSelectedScreenChanged);
		SyncSubsystem->OnCommandReceived.RemoveDynamic(this, &AHUICRSyncPCManagerBase::OnSubsystemCommandReceived);
	}

	Super::EndPlay(EndPlayReason);
}

void AHUICRSyncPCManagerBase::SetPCCalibratorTransform(const FTransform& InPCCalibratorTransform)
{
	PCCalibratorTransform = InPCCalibratorTransform;
}

bool AHUICRSyncPCManagerBase::RecalculateHMDPCTransforms()
{
	if (!SyncSubsystem)
	{
		return false;
	}

	if (!RefreshPCCalibratorTransform())
	{
		return false;
	}

	SyncSubsystem->HMDToPCTransform = LastHMDCalibratorPayload.Transform.Inverse() * PCCalibratorTransform;
	SyncSubsystem->PCToHMDTransform = PCCalibratorTransform.Inverse() * LastHMDCalibratorPayload.Transform;
	return true;
}

bool AHUICRSyncPCManagerBase::RefreshPCCalibratorTransform()
{
	const FHUICRSyncPCScreenBinding* MainBinding = FindPCScreenBinding(0);
	if (MainBinding && IsValid(MainBinding->SpawnedCalibrator))
	{
		SetPCCalibratorTransform(MainBinding->SpawnedCalibrator->GetActorTransform());
		return true;
	}

	return false;
}

bool AHUICRSyncPCManagerBase::SpawnPCCalibrator(int32 ScreenID)
{
	FHUICRSyncPCScreenBinding* Binding = FindPCScreenBinding(ScreenID);
	if (!Binding || !IsValid(Binding->ScreenComponent) || !PCCalibratorClass || !GetWorld())
	{
		return false;
	}

	if (IsValid(Binding->SpawnedCalibrator))
	{
		Binding->SpawnedCalibrator->BindScreenComponent(ScreenID, Binding->ScreenComponent);
		Binding->SpawnedCalibrator->SetCalibratorScale(PCCalibratorScale);
		return true;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.bNoFail = true;
	SpawnParams.Owner = this;

	AHUICRSyncCalibrator_PC* NewCalibrator = GetWorld()->SpawnActor<AHUICRSyncCalibrator_PC>(
		PCCalibratorClass,
		Binding->ScreenComponent->GetComponentTransform(),
		SpawnParams);

	if (!IsValid(NewCalibrator))
	{
		return false;
	}

	NewCalibrator->BindScreenComponent(ScreenID, Binding->ScreenComponent);
	NewCalibrator->SetCalibratorScale(PCCalibratorScale);
	Binding->SpawnedCalibrator = NewCalibrator;
	return true;
}

int32 AHUICRSyncPCManagerBase::SpawnAllPCCalibrators()
{
	int32 SpawnedOrUpdatedCount = 0;
	for (const FHUICRSyncPCScreenBinding& Binding : PCScreenBindings)
	{
		SpawnedOrUpdatedCount += SpawnPCCalibrator(Binding.ScreenID) ? 1 : 0;
	}

	RefreshPCCalibratorTransform();
	return SpawnedOrUpdatedCount;
}

void AHUICRSyncPCManagerBase::SetPCCalibratorScale(const FVector& NewScale)
{
	PCCalibratorScale = NewScale;
	ApplyPCCalibratorScale();
}

void AHUICRSyncPCManagerBase::ApplyPCCalibratorScale()
{
	for (FHUICRSyncPCScreenBinding& Binding : PCScreenBindings)
	{
		if (IsValid(Binding.SpawnedCalibrator))
		{
			Binding.SpawnedCalibrator->SetCalibratorScale(PCCalibratorScale);
		}
	}

	RefreshPCCalibratorTransform();
}

bool AHUICRSyncPCManagerBase::SendLocalScreenPayload(int32 ScreenID, const FTransform& ScreenTransform, bool bRemoteControlsLocalScreen)
{
	FHUICRSyncScreenPayload Payload;
	Payload.ScreenID = ScreenID;
	Payload.Transform = SyncSubsystem ? SyncSubsystem->ConvertPCTransformToHMD(ScreenTransform) : ScreenTransform;
	Payload.bRemoteControlsLocalScreen = bRemoteControlsLocalScreen;
	return QueueScreenPayload(Payload);
}

bool AHUICRSyncPCManagerBase::InitializeNDisplayScreens()
{
#if HUICR_WITH_NDISPLAY
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	ADisplayClusterRootActor* DisplayClusterActor =
		Cast<ADisplayClusterRootActor>(UGameplayStatics::GetActorOfClass(World, ADisplayClusterRootActor::StaticClass()));
	NDisplayActor = DisplayClusterActor;
	if (!DisplayClusterActor)
	{
		return false;
	}

	DefaultViewportComponent = DisplayClusterActor->GetDefaultCamera();
	if (!DefaultViewportComponent && !DefaultViewportComponentName.IsNone())
	{
		DefaultViewportComponent = DisplayClusterActor->GetComponentByName<USceneComponent>(
			DefaultViewportComponentName.ToString());
	}
	if (!DefaultViewportComponent)
	{
		DefaultViewportComponent = Cast<USceneComponent>(
			DisplayClusterActor->GetComponentByClass(UDisplayClusterCameraComponent::StaticClass()));
	}
	if (DefaultViewportComponent)
	{
		DefaultViewportTransform = DefaultViewportComponent->GetComponentTransform();
	}

	PCMainScreen = DisplayClusterActor->GetComponentByName<UStaticMeshComponent>(MainScreenComponentName.ToString());
	if (PCMainScreen && DefaultViewportComponent)
	{
		DefaultViewportOffsetFromMainScreen =
			DefaultViewportTransform.GetLocation() - PCMainScreen->GetComponentTransform().GetLocation();
	}

	for (FHUICRSyncPCScreenBinding& Binding : PCScreenBindings)
	{
		if (IsValid(Binding.SpawnedVisualActor))
		{
			Binding.SpawnedVisualActor->Destroy();
		}

		if (IsValid(Binding.SpawnedCalibrator))
		{
			Binding.SpawnedCalibrator->Destroy();
		}
	}
	PCScreenBindings.Reset();

	int32 NextScreenID = 0;
	if (PCMainScreen)
	{
		RegisterPCScreenComponent(0, PCMainScreen, bApplyMainScreenTransformAutomatically);
		NextScreenID = 1;
	}

	TArray<UActorComponent*> TaggedScreenComponents =
		DisplayClusterActor->GetComponentsByTag(UStaticMeshComponent::StaticClass(), ScreenComponentTag);
	for (UActorComponent* Component : TaggedScreenComponents)
	{
		UStaticMeshComponent* ScreenComponent = Cast<UStaticMeshComponent>(Component);
		if (!ScreenComponent || ScreenComponent == PCMainScreen)
		{
			continue;
		}

		RegisterPCScreenComponent(NextScreenID, ScreenComponent, true);
		++NextScreenID;
	}

	if (bAutoSpawnPCCalibrators)
	{
		SpawnAllPCCalibrators();
	}

	return PCScreenBindings.Num() > 0;
#else
	return false;
#endif
}

bool AHUICRSyncPCManagerBase::RegisterPCScreenComponent(int32 ScreenID, USceneComponent* ScreenComponent, bool bApplyTransformAutomatically)
{
	if (!IsValid(ScreenComponent))
	{
		return false;
	}

	if (FHUICRSyncPCScreenBinding* ExistingBinding = FindPCScreenBinding(ScreenID))
	{
		ExistingBinding->ScreenComponent = ScreenComponent;
		ExistingBinding->InitialTransform = ScreenComponent->GetComponentTransform();
		ExistingBinding->bApplyTransformAutomatically = bApplyTransformAutomatically;
		if (IsValid(ExistingBinding->SpawnedVisualActor))
		{
			ExistingBinding->SpawnedVisualActor->BindScreenComponent(ScreenID, ScreenComponent);
		}
		else
		{
			SpawnPCScreenVisual(ScreenID);
		}

		if (bAutoSpawnPCCalibrators)
		{
			SpawnPCCalibrator(ScreenID);
		}
		return true;
	}

	FHUICRSyncPCScreenBinding NewBinding;
	NewBinding.ScreenID = ScreenID;
	NewBinding.ScreenComponent = ScreenComponent;
	NewBinding.InitialTransform = ScreenComponent->GetComponentTransform();
	NewBinding.bApplyTransformAutomatically = bApplyTransformAutomatically;
	PCScreenBindings.Add(NewBinding);
	SpawnPCScreenVisual(ScreenID);
	if (bAutoSpawnPCCalibrators)
	{
		SpawnPCCalibrator(ScreenID);
	}
	return true;
}

bool AHUICRSyncPCManagerBase::SpawnPCScreenVisual(int32 ScreenID)
{
	FHUICRSyncPCScreenBinding* Binding = FindPCScreenBinding(ScreenID);
	if (!Binding || !IsValid(Binding->ScreenComponent) || !PCScreenVisualClass || !GetWorld())
	{
		return false;
	}

	if (IsValid(Binding->SpawnedVisualActor))
	{
		Binding->SpawnedVisualActor->Destroy();
		Binding->SpawnedVisualActor = nullptr;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.Owner = this;

	AHUICRSyncPCScreenVisual* NewVisual = GetWorld()->SpawnActor<AHUICRSyncPCScreenVisual>(
		PCScreenVisualClass,
		Binding->ScreenComponent->GetComponentTransform(),
		SpawnParams);

	if (!NewVisual)
	{
		return false;
	}

	NewVisual->BindScreenComponent(ScreenID, Binding->ScreenComponent);
	NewVisual->SetCalibrationVisible(SyncSubsystem ? SyncSubsystem->bCalibrationState : false);
	Binding->SpawnedVisualActor = NewVisual;
	return true;
}

bool AHUICRSyncPCManagerBase::UpdatePCScreenVisual(int32 ScreenID)
{
	FHUICRSyncPCScreenBinding* Binding = FindPCScreenBinding(ScreenID);
	if (!Binding || !IsValid(Binding->SpawnedVisualActor))
	{
		return false;
	}

	Binding->SpawnedVisualActor->UpdateTransformFromReferencedScreen();
	return true;
}

bool AHUICRSyncPCManagerBase::RestorePCScreen(int32 ScreenID)
{
	FHUICRSyncPCScreenBinding* Binding = FindPCScreenBinding(ScreenID);
	if (!Binding || !IsValid(Binding->ScreenComponent))
	{
		return false;
	}

	Binding->ScreenComponent->SetWorldTransform(Binding->InitialTransform);
	UpdatePCScreenVisual(ScreenID);
	return true;
}

bool AHUICRSyncPCManagerBase::ApplyHMDScreenPayload(const FHUICRSyncScreenPayload& Payload)
{
	FHUICRSyncPCScreenBinding* Binding = FindPCScreenBinding(Payload.ScreenID);
	if (!Binding || !IsValid(Binding->ScreenComponent))
	{
		return false;
	}

	if (!Payload.bRemoteControlsLocalScreen)
	{
		RestorePCScreen(Payload.ScreenID);
		SendPCScreenPayload(Payload.ScreenID);
		return true;
	}

	FTransform PCScreenTransform = SyncSubsystem ? SyncSubsystem->ConvertHMDTransformToPC(Payload.Transform) : Payload.Transform;
	if (Payload.ScreenID != 0 && PCMainScreen)
	{
		FVector PCScale = PCScreenTransform.GetScale3D();
		PCScale.X = PCMainScreen->GetComponentScale().X;
		PCScreenTransform.SetScale3D(PCScale);
	}

	if (Binding->bApplyTransformAutomatically)
	{
		Binding->ScreenComponent->SetWorldTransform(PCScreenTransform);
		UpdatePCScreenVisual(Payload.ScreenID);
	}

	OnPCScreenTransformResolved.Broadcast(Payload.ScreenID, Payload, PCScreenTransform);
	return true;
}

bool AHUICRSyncPCManagerBase::SendPCScreenPayload(int32 ScreenID)
{
	const FHUICRSyncPCScreenBinding* Binding = FindPCScreenBinding(ScreenID);
	if (!Binding || !IsValid(Binding->ScreenComponent))
	{
		return false;
	}

	const bool bQueuedPayload = SendLocalScreenPayload(ScreenID, Binding->ScreenComponent->GetComponentTransform(), false);
	if (bQueuedPayload && SyncSubsystem)
	{
		SyncSubsystem->SendAllQueuedStates();
	}

	return bQueuedPayload;
}

bool AHUICRSyncPCManagerBase::SelectPCScreen(int32 ScreenID)
{
	const FHUICRSyncPCScreenBinding* Binding = FindPCScreenBinding(ScreenID);
	if (!Binding || !IsValid(Binding->ScreenComponent) || !DefaultViewportComponent)
	{
		return false;
	}

	const float OffsetLength = DefaultViewportOffsetFromMainScreen.Length();
	const FVector NewLocation =
		Binding->ScreenComponent->GetComponentLocation() +
		(Binding->ScreenComponent->GetForwardVector() * -1.0f) * OffsetLength;

	DefaultViewportComponent->SetWorldLocation(NewLocation);
	DefaultViewportComponent->SetWorldRotation(Binding->ScreenComponent->GetComponentRotation());
	return true;
}

bool AHUICRSyncPCManagerBase::RestoreDefaultViewport()
{
	bool bRestored = false;

	if (DefaultViewportComponent)
	{
		DefaultViewportComponent->SetWorldTransform(DefaultViewportTransform);
		bRestored = true;
	}

	return bRestored;
}

bool AHUICRSyncPCManagerBase::ApplyHMDTransformToDefaultViewport(const FTransform& HMDTransform)
{
	LastHMDTransform = HMDTransform;

	if (!SyncSubsystem || !DefaultViewportComponent || !SyncSubsystem->bMotionParallaxState || SyncSubsystem->bGameStopped)
	{
		return false;
	}

	//OnHMDTransformReceived.Broadcast(HMDTransform);
	LastResolvedDefaultViewportTransform = SyncSubsystem->ConvertHMDTransformToPC(HMDTransform);
	LastResolvedDefaultViewportTransform.SetScale3D(FVector::OneVector);
	OnDefaultViewportTransformResolved.Broadcast(LastResolvedDefaultViewportTransform);

	DefaultViewportComponent->SetWorldTransform(LastResolvedDefaultViewportTransform);
	return true;
}

void AHUICRSyncPCManagerBase::OnSubsystemMotionParallaxStateChanged(bool bNewMotionParallaxState)
{
	if (!bNewMotionParallaxState && bRestoreDefaultViewportWhenMotionParallaxDisabled)
	{
		RestoreDefaultViewport();
	}
}

void AHUICRSyncPCManagerBase::OnSubsystemCalibrationStateChanged(bool bNewCalibrationState)
{
	ApplyPCCalibratorScale();

	for (FHUICRSyncPCScreenBinding& Binding : PCScreenBindings)
	{
		if (IsValid(Binding.SpawnedCalibrator))
		{
			Binding.SpawnedCalibrator->HandleCalibrationStateChanged(bNewCalibrationState);
		}

		if (IsValid(Binding.SpawnedVisualActor))
		{
			Binding.SpawnedVisualActor->SetCalibrationVisible(bNewCalibrationState);
			Binding.SpawnedVisualActor->UpdateTransformFromReferencedScreen();
		}
	}
}

#if WITH_EDITOR
void AHUICRSyncPCManagerBase::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	if (PropertyChangedEvent.GetPropertyName() == GET_MEMBER_NAME_CHECKED(AHUICRSyncPCManagerBase, PCCalibratorScale))
	{
		ApplyPCCalibratorScale();
	}
}
#endif

void AHUICRSyncPCManagerBase::OnSubsystemSelectedScreenChanged(int32 ScreenID)
{
	SelectPCScreen(ScreenID);
}

void AHUICRSyncPCManagerBase::OnSubsystemCommandReceived(const FString& Address, const TArray<FHUICRSyncCommandArg>& Args)
{
	if (Args.IsEmpty())
	{
		return;
	}

	const bool bIsSelectScreenCommand =
		Address.Equals(TEXT("/FromHMD/Calibration/SelectedScreenID"), ESearchCase::IgnoreCase) ||
		Address.Equals(TEXT("FromHMD/Calibration/SelectedScreenID"), ESearchCase::IgnoreCase) ||
		Address.Equals(TEXT("/SelectScreen"), ESearchCase::IgnoreCase) ||
		Address.Equals(TEXT("SelectScreen"), ESearchCase::IgnoreCase);

	if (!bIsSelectScreenCommand || Args[0].Type != EHUICRSyncCommandArgType::Int32)
	{
		return;
	}

	SelectPCScreen(Args[0].IntValue);
}

FHUICRSyncPCScreenBinding* AHUICRSyncPCManagerBase::FindPCScreenBinding(int32 ScreenID)
{
	for (FHUICRSyncPCScreenBinding& Binding : PCScreenBindings)
	{
		if (Binding.ScreenID == ScreenID)
		{
			return &Binding;
		}
	}

	return nullptr;
}

const FHUICRSyncPCScreenBinding* AHUICRSyncPCManagerBase::FindPCScreenBinding(int32 ScreenID) const
{
	for (const FHUICRSyncPCScreenBinding& Binding : PCScreenBindings)
	{
		if (Binding.ScreenID == ScreenID)
		{
			return &Binding;
		}
	}

	return nullptr;
}

void AHUICRSyncPCManagerBase::HandleNetDataReceived()
{
	bool bCalibrationDataProcessed = false;
	FHUICRSyncNetEntry Entry;
	FHUICRSyncCalibratorPayload CalibratorPayload;
	if (GetFirstPayload(EHUICRSyncActorType::HUICalibrator, Entry) && DecodeCalibratorEntry(Entry, CalibratorPayload))
	{
		LastHMDCalibratorPayload = CalibratorPayload;
		OnCalibratorPayloadReceived.Broadcast(CalibratorPayload);
		bCalibrationDataProcessed = RecalculateHMDPCTransforms();
	}

	if (!SyncSubsystem)
	{
		return;
	}

	if (const FHUICRSyncNetInnerMapEntry* ScreenMap = SyncSubsystem->ReceivedDataMap.Find(EHUICRSyncActorType::HUIScreen))
	{
		for (const TPair<int32, FHUICRSyncNetEntry>& Pair : ScreenMap->InnerMap)
		{
			FHUICRSyncScreenPayload ScreenPayload;
			if (DecodeScreenEntry(Pair.Value, ScreenPayload))
			{
				LastHMDScreenPayloads.Add(ScreenPayload.ScreenID, ScreenPayload);
				ApplyHMDScreenPayload(ScreenPayload);
				OnScreenPayloadReceived.Broadcast(ScreenPayload);
			}
		}
	}

	if (bCalibrationDataProcessed)
	{
		RestoreDefaultViewport();
		SyncSubsystem->bCalibrationState = false;
		SyncSubsystem->OnCalibrationStateChanged.Broadcast(false);
		SyncSubsystem->OnCalibrationReady.Broadcast();
		SyncSubsystem->StartInitialSpawnSync();
	}
}
