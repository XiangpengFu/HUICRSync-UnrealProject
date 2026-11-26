#include "HUICRSyncPCManagerBase.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "HUICRSyncActor.h"
#include "HUICRSyncCalibrationSaveGame.h"
#include "HUICRSyncCalibrator_PC.h"
#include "HUICRSyncPCScreenVisual.h"
#include "HUICRSyncSubsystem.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Crc.h"

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
		SyncSubsystem->OnParticipantsChanged.AddDynamic(this, &AHUICRSyncPCManagerBase::OnParticipantsChanged);
		SyncSubsystem->RegisterCalibrationManager(this);
	}
}

void AHUICRSyncPCManagerBase::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	StopSendingPCCalibrationReference();

	if (SyncSubsystem)
	{
		SyncSubsystem->OnCalibrationStateChanged.RemoveDynamic(this, &AHUICRSyncPCManagerBase::OnSubsystemCalibrationStateChanged);
		SyncSubsystem->OnMotionParallaxStateChanged.RemoveDynamic(this, &AHUICRSyncPCManagerBase::OnSubsystemMotionParallaxStateChanged);
		SyncSubsystem->OnSelectedScreenChanged.RemoveDynamic(this, &AHUICRSyncPCManagerBase::OnSubsystemSelectedScreenChanged);
		SyncSubsystem->OnCommandReceived.RemoveDynamic(this, &AHUICRSyncPCManagerBase::OnSubsystemCommandReceived);
		SyncSubsystem->OnParticipantsChanged.RemoveDynamic(this, &AHUICRSyncPCManagerBase::OnParticipantsChanged);
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
		MainBinding->SpawnedCalibrator->UpdateTransformFromBoundScreen();
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

bool AHUICRSyncPCManagerBase::SendPCCalibrationReference()
{
	if (!SyncSubsystem)
	{
		return false;
	}

	if (!RefreshPCCalibratorTransform())
	{
		UE_LOG(LogTemp, Warning, TEXT("HUICRSync PC cannot send calibration reference because PC calibrator transform is unavailable."));
		return false;
	}

	FHUICRSyncCalibratorPayload Payload;
	Payload.ActorID = 0;
	Payload.Transform = PCCalibratorTransform;
	const bool bQueuedPayload = QueueCalibratorPayload(Payload);
	if (bQueuedPayload)
	{
		SyncSubsystem->SendAllQueuedStates();
		++PCCalibrationReferenceSendCount;
	}
	return bQueuedPayload;
}

void AHUICRSyncPCManagerBase::SendPCCalibrationReferenceTick()
{
	if (bWaitingForHMDCalibrationReferenceAck)
	{
		SendPCCalibrationReference();
	}
}

void AHUICRSyncPCManagerBase::StartSendingPCCalibrationReferenceUntilAck()
{
	if (!SyncSubsystem || SyncSubsystem->HostParticipantId == 0)
	{
		return;
	}
	CalibrationReferenceRecipientId = SyncSubsystem->HostParticipantId;
	bWaitingForHMDCalibrationReferenceAck = true;
	PCCalibrationReferenceSendCount = 0;
	SendPCCalibrationReference();

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(PCCalibrationReferenceTimerHandle);
		World->GetTimerManager().SetTimer(
			PCCalibrationReferenceTimerHandle,
			this,
			&AHUICRSyncPCManagerBase::SendPCCalibrationReferenceTick,
			FMath::Max(0.02f, PCCalibrationReferenceSendInterval),
			true);
	}
}

void AHUICRSyncPCManagerBase::StopSendingPCCalibrationReference()
{
	CalibrationReferenceRecipientId = 0;
	bWaitingForHMDCalibrationReferenceAck = false;
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(PCCalibrationReferenceTimerHandle);
	}
}

void AHUICRSyncPCManagerBase::OnParticipantsChanged()
{
	if (bWaitingForHMDCalibrationReferenceAck && CalibrationReferenceRecipientId != SyncSubsystem->HostParticipantId)
	{
		StopSendingPCCalibrationReference();
	}
}

void AHUICRSyncPCManagerBase::CompleteCalibrationAfterHMDReferenceAck()
{
	if (!SyncSubsystem)
	{
		return;
	}

	StopSendingPCCalibrationReference();
	RestoreDefaultViewport();
	SyncSubsystem->bCalibrationState = false;
	SyncSubsystem->OnCalibrationStateChanged.Broadcast(false);
	SyncSubsystem->OnCalibrationReady.Broadcast();
	SyncSubsystem->StartInitialSpawnSync();
}

bool AHUICRSyncPCManagerBase::SendLocalScreenPayload(int32 ScreenID, const FTransform& ScreenTransform, bool bRemoteControlsLocalScreen)
{
	FHUICRSyncScreenPayload Payload;
	Payload.ScreenID = ScreenID;
	Payload.Transform = ScreenTransform;
	Payload.InitialTransform = Payload.Transform;
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
		else if (bAutoSpawnPCScreenDissolveBoxes)
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
	if (bAutoSpawnPCScreenDissolveBoxes)
	{
		SpawnPCScreenVisual(ScreenID);
	}
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

		// HMD sends screen payloads in PC space after applying the PC calibrator reference.
		// If PC owns this screen, send the restored PC-space screen back for HMD-side conversion.
		if (Payload.ScreenID != 0 && (!SyncSubsystem || !SyncSubsystem->UsesManagedCalibration()))
		{
			SendPCScreenPayload(Payload.ScreenID);
		}
		return true;
	}

	FTransform PCScreenTransform = Payload.Transform;
	if (Payload.ScreenID == 0)
	{
		FTransform MainScreenTransform = Binding->InitialTransform;

		const FQuat HMDRotationDelta =
			Payload.InitialTransform.GetRotation().Inverse() * Payload.Transform.GetRotation();
		const FQuat MainScreenRotation =
			Binding->InitialTransform.GetRotation() * HMDRotationDelta;
		MainScreenTransform.SetRotation(MainScreenRotation.GetNormalized());

		// Keep the PC main screen thickness from nDisplay while accepting the HMD-adjusted Y/Z scale in PC space.
		FVector MainScreenScale = PCScreenTransform.GetScale3D();
		MainScreenScale.X = Binding->InitialTransform.GetScale3D().X;
		MainScreenTransform.SetScale3D(MainScreenScale);

		Binding->ScreenComponent->SetWorldTransform(MainScreenTransform);
		UpdatePCScreenVisual(Payload.ScreenID);
		OnPCScreenTransformResolved.Broadcast(Payload.ScreenID, Payload, MainScreenTransform);
		return true;
	}

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

bool AHUICRSyncPCManagerBase::ApplyHMDTransformToDefaultViewport(const FTransform& HMDTransform, const UObject* PoseSource)
{
	const AHUICRSyncActor* Pawn = Cast<AHUICRSyncActor>(PoseSource);
	if (Pawn && Pawn->TypeCode == EHUICRSyncActorType::HMDPawn &&
		(!SyncSubsystem || SyncSubsystem->HostParticipantId <= 0 || Pawn->ActorID != SyncSubsystem->HostParticipantId))
	{
		return false;
	}
	LastHMDTransform = HMDTransform;

	if (!SyncSubsystem || !DefaultViewportComponent || !SyncSubsystem->bMotionParallaxState || SyncSubsystem->bGameStopped)
	{
		return false;
	}

	//OnHMDTransformReceived.Broadcast(HMDTransform);
	LastResolvedDefaultViewportTransform = HMDTransform;
	LastResolvedDefaultViewportTransform.SetScale3D(FVector::OneVector);
	LastTrackedGameViewpoint = LastResolvedDefaultViewportTransform;
	bHasTrackedGameViewpoint = true;
	DefaultViewportComponent->SetWorldTransform(LastResolvedDefaultViewportTransform);
	OnDefaultViewportTransformResolved.Broadcast(LastResolvedDefaultViewportTransform);
	return true;
}

void AHUICRSyncPCManagerBase::OnSubsystemMotionParallaxStateChanged(bool bNewMotionParallaxState)
{
	if (!bNewMotionParallaxState && bRestoreDefaultViewportWhenMotionParallaxDisabled &&
		!SyncSubsystem->UsesManagedCalibration())
	{
		RestoreDefaultViewport();
	}
}

FTransform AHUICRSyncPCManagerBase::GetViewpointReference() const
{
	FTransform Origin = IsValid(NDisplayActor) ? NDisplayActor->GetActorTransform() : GetActorTransform();
	Origin.SetScale3D(FVector::OneVector);
	return Origin;
}

FString AHUICRSyncPCManagerBase::GetPCLayoutSaveSlot() const
{
	const FString Map = GetWorld()->GetOutermost()->GetName();
	const FString DisplayConfig = IsValid(NDisplayActor) ? NDisplayActor->GetClass()->GetPathName() : GetClass()->GetPathName();
	return FString::Printf(TEXT("HUICRSync_PC_%08x"), FCrc::StrCrc32(*(Map + TEXT("|") + DisplayConfig)));
}

FTransform AHUICRSyncPCManagerBase::GetMainCalibrationReference(const FHUICRSyncPCScreenBinding& Main) const
{
	FTransform Reference = Main.ScreenComponent->GetComponentTransform();
	Reference.SetScale3D(IsValid(Main.SpawnedCalibrator) ? Main.SpawnedCalibrator->GetActorScale3D() : PCCalibratorScale);
	return Reference;
}

bool AHUICRSyncPCManagerBase::CaptureCalibrationLayout(FHUICRSyncCalibrationLayout& Layout)
{
	const FHUICRSyncPCScreenBinding* Main = FindPCScreenBinding(0);
	if (!Main || !IsValid(Main->ScreenComponent))
	{
		return false;
	}
	Layout.Screens.Reset();
	for (const FHUICRSyncPCScreenBinding& Binding : PCScreenBindings)
	{
		if (!IsValid(Binding.ScreenComponent))
		{
			return false;
		}
		FHUICRSyncScreenPayload& Screen = Layout.Screens.AddDefaulted_GetRef();
		Screen.ScreenID = Binding.ScreenID;
		Screen.Transform = Binding.ScreenComponent->GetComponentTransform();
		Screen.InitialTransform = Binding.InitialTransform;
	}
	Layout.MainReference = GetMainCalibrationReference(*Main);
	return Layout.MainReference.IsValid() && Layout.MainReference.GetScale3D().GetAbsMin() > UE_SMALL_NUMBER;
}

bool AHUICRSyncPCManagerBase::PreviewHostLayout(const FHUICRSyncCalibrationLayout& Measurements,
	const FHUICRSyncCalibrationLayout& CommittedLayout, FHUICRSyncCalibrationLayout& Result)
{
	FHUICRSyncCalibrationLayout Before;
	if (!CaptureCalibrationLayout(Before) || Before.Screens.Num() > 48 ||
		!Measurements.Screens.ContainsByPredicate([](const FHUICRSyncScreenPayload& Screen) { return Screen.ScreenID == 0; }))
	{
		return false;
	}
	if (Measurements.MainReference.GetScale3D().GetAbsMin() <= UE_SMALL_NUMBER)
	{
		return false;
	}
	const FTransform ToPC = Measurements.MainReference.Inverse() * Before.MainReference;
	const bool bFirstLayout = !CommittedLayout.Id.IsValid();
	TArray<FHUICRSyncScreenPayload> Screens = Measurements.Screens;
	Screens.Sort([](const FHUICRSyncScreenPayload& A, const FHUICRSyncScreenPayload& B) { return A.ScreenID < B.ScreenID; });
	for (FHUICRSyncScreenPayload& Screen : Screens)
	{
		if (!FindPCScreenBinding(Screen.ScreenID))
		{
			continue;
		}
		const bool bPoseEdited = Screen.bRemoteControlsLocalScreen &&
			(bFirstLayout || Measurements.EditedScreenIds.Contains(Screen.ScreenID));
		if (!bFirstLayout && !bPoseEdited)
		{
			continue;
		}
		Screen.Transform = Screen.Transform * ToPC;
		Screen.InitialTransform = Screen.InitialTransform * ToPC;
		if (ApplyHMDScreenPayload(Screen) && bPoseEdited)
		{
			Result.EditedScreenIds.Add(Screen.ScreenID);
		}
		OnScreenPayloadReceived.Broadcast(Screen);
	}
	if (!CaptureCalibrationLayout(Result))
	{
		RestoreCalibrationLayout(Before);
		return false;
	}
	for (auto& Screen : Result.Screens)
	{
		const auto HasSameId = [&Screen](const FHUICRSyncScreenPayload& Candidate) { return Screen.ScreenID == Candidate.ScreenID; };
		const FHUICRSyncScreenPayload* Input = Screens.FindByPredicate(HasSameId);
		if (!Input)
		{
			Input = CommittedLayout.Screens.FindByPredicate(HasSameId);
		}
		Screen.bRemoteControlsLocalScreen = Input && Input->bRemoteControlsLocalScreen;
	}
	return true;
}

void AHUICRSyncPCManagerBase::RestoreCalibrationLayout(const FHUICRSyncCalibrationLayout& Layout)
{
	for (const FHUICRSyncPCScreenBinding& Binding : PCScreenBindings)
	{
		if (!IsValid(Binding.ScreenComponent))
		{
			continue;
		}
		const FHUICRSyncScreenPayload* Saved = Layout.Screens.FindByPredicate(
			[&Binding](const FHUICRSyncScreenPayload& Screen) { return Screen.ScreenID == Binding.ScreenID; });
		Binding.ScreenComponent->SetWorldTransform(Saved ? Saved->Transform : Binding.InitialTransform);
		UpdatePCScreenVisual(Binding.ScreenID);
	}
	RefreshPCCalibratorTransform();
}

bool AHUICRSyncPCManagerBase::SaveCommittedCalibration(const FHUICRSyncCalibrationLayout& Layout, bool bGameMotionParallax)
{
	auto* Save = NewObject<UHUICRSyncCalibrationSaveGame>();
	Save->PCLayoutVersion = Layout.Version;
	Save->PCLayoutId = Layout.Id;
	Save->PCLayoutScreens = Layout.Screens;
	const FTransform Origin = GetViewpointReference();
	for (FHUICRSyncScreenPayload& Screen : Save->PCLayoutScreens)
	{
		Screen.Transform = Screen.Transform.GetRelativeTransform(Origin);
		Screen.InitialTransform = Screen.InitialTransform.GetRelativeTransform(Origin);
	}
	Save->PCMainReference = Layout.MainReference.GetRelativeTransform(Origin);
	Save->bMotionParallaxState = bGameMotionParallax;
	Save->bHasFixedGameViewpoint = bHasFixedViewpoint;
	Save->FixedGameViewpointRelative = FixedViewpointRelative;
	return UGameplayStatics::SaveGameToSlot(Save, GetPCLayoutSaveSlot(), 0);
}

bool AHUICRSyncPCManagerBase::LoadCommittedCalibration(FHUICRSyncCalibrationLayout& Layout, bool& bGameMotionParallax)
{
	const FString Slot = GetPCLayoutSaveSlot();
	if (!UGameplayStatics::DoesSaveGameExist(Slot, 0))
	{
		return false;
	}
	auto* Save = Cast<UHUICRSyncCalibrationSaveGame>(UGameplayStatics::LoadGameFromSlot(Slot, 0));
	if (!Save)
	{
		return false;
	}
	bGameMotionParallax = Save->bMotionParallaxState;
	FixedViewpointRelative = Save->FixedGameViewpointRelative;
	bHasFixedViewpoint = Save->bHasFixedGameViewpoint;
	const FTransform Origin = GetViewpointReference();
	const bool bMatchingScreens = Save->PCLayoutScreens.Num() == PCScreenBindings.Num() &&
		Save->PCLayoutScreens.ContainsByPredicate([](const FHUICRSyncScreenPayload& Screen) { return Screen.ScreenID == 0; }) &&
		!Save->PCLayoutScreens.ContainsByPredicate([this](const FHUICRSyncScreenPayload& Screen)
		{
			return !FindPCScreenBinding(Screen.ScreenID) || !Screen.Transform.IsValid();
		});
	if (Save->PCLayoutId.IsValid() && bMatchingScreens)
	{
		Layout.Version = Save->PCLayoutVersion;
		Layout.Id = Save->PCLayoutId;
		Layout.Screens = Save->PCLayoutScreens;
		Layout.MainReference = Save->PCMainReference * Origin;
		for (FHUICRSyncScreenPayload& Screen : Layout.Screens)
		{
			Screen.Transform = Screen.Transform * Origin;
			Screen.InitialTransform = Screen.InitialTransform * Origin;
		}
		RestoreCalibrationLayout(Layout);
		const FHUICRSyncPCScreenBinding* Main = FindPCScreenBinding(0);
		if (Main && IsValid(Main->ScreenComponent))
		{
			Layout.MainReference = GetMainCalibrationReference(*Main);
		}
	}
	RestoreFixedGameViewpoint();
	return Layout.Id.IsValid();
}

void AHUICRSyncPCManagerBase::ApplyGameMotionParallax(bool bEnabled, const FHUICRSyncCalibrationLayout& Layout)
{
	if (!bEnabled && bHasTrackedGameViewpoint)
	{
		FixedViewpointRelative = LastTrackedGameViewpoint.GetRelativeTransform(GetViewpointReference());
		bHasFixedViewpoint = true;
	}
	ResetGameViewpointTracking();
	if (!SaveCommittedCalibration(Layout, bEnabled))
	{
		UE_LOG(LogTemp, Warning, TEXT("HUICRSync could not save the game viewpoint."));
	}
	if (!SyncSubsystem->bCalibrationState)
	{
		RestoreFixedGameViewpoint();
	}
}

void AHUICRSyncPCManagerBase::ResetGameViewpointTracking()
{
	bHasTrackedGameViewpoint = false;
}

bool AHUICRSyncPCManagerBase::RestoreFixedGameViewpoint()
{
	if (DefaultViewportComponent && bHasFixedViewpoint)
	{
		DefaultViewportComponent->SetWorldTransform(FixedViewpointRelative * GetViewpointReference());
		return true;
	}
	return SelectPCScreen(0) || RestoreDefaultViewport();
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
	if (!SyncSubsystem->UsesManagedCalibration())
	{
		SelectPCScreen(ScreenID);
	}
}

void AHUICRSyncPCManagerBase::OnSubsystemCommandReceived(const FString& Address, const TArray<FHUICRSyncCommandArg>& Args)
{
	const bool bIsCalibrationReferenceRequest =
		Address.Equals(TEXT("/FromHMD/Calibration/RequestPCReference"), ESearchCase::IgnoreCase) ||
		Address.Equals(TEXT("FromHMD/Calibration/RequestPCReference"), ESearchCase::IgnoreCase);
	if (bIsCalibrationReferenceRequest)
	{
		StartSendingPCCalibrationReferenceUntilAck();
		return;
	}

	const bool bIsCalibrationReferenceApplied =
		Address.Equals(TEXT("/FromHMD/Calibration/ReferenceApplied"), ESearchCase::IgnoreCase) ||
		Address.Equals(TEXT("FromHMD/Calibration/ReferenceApplied"), ESearchCase::IgnoreCase);
	if (bIsCalibrationReferenceApplied && bWaitingForHMDCalibrationReferenceAck)
	{
		CompleteCalibrationAfterHMDReferenceAck();
		return;
	}

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
	if (!SyncSubsystem)
	{
		return;
	}

	if (const FHUICRSyncNetInnerMapEntry* ScreenMap = SyncSubsystem->ReceivedDataMap.Find(EHUICRSyncActorType::HUIScreen))
	{
		TArray<FHUICRSyncScreenPayload> DecodedScreenPayloads;
		DecodedScreenPayloads.Reserve(ScreenMap->InnerMap.Num());

		for (const TPair<int32, FHUICRSyncNetEntry>& Pair : ScreenMap->InnerMap)
		{
			FHUICRSyncScreenPayload ScreenPayload;
			if (DecodeScreenEntry(Pair.Value, ScreenPayload))
			{
				LastHMDScreenPayloads.Add(ScreenPayload.ScreenID, ScreenPayload);
				DecodedScreenPayloads.Add(ScreenPayload);
			}
		}

		const FHUICRSyncScreenPayload* MainScreenPayload = DecodedScreenPayloads.FindByPredicate(
			[](const FHUICRSyncScreenPayload& ScreenPayload)
			{
				return ScreenPayload.ScreenID == 0;
			});

		if (MainScreenPayload)
		{
			ApplyHMDScreenPayload(*MainScreenPayload);
			OnScreenPayloadReceived.Broadcast(*MainScreenPayload);

			if (!MainScreenPayload->bRemoteControlsLocalScreen)
			{
				SendPCScreenPayload(MainScreenPayload->ScreenID);
			}
		}

		for (const FHUICRSyncScreenPayload& ScreenPayload : DecodedScreenPayloads)
		{
			if (ScreenPayload.ScreenID == 0)
			{
				continue;
			}

			ApplyHMDScreenPayload(ScreenPayload);
			OnScreenPayloadReceived.Broadcast(ScreenPayload);
		}
	}
}
