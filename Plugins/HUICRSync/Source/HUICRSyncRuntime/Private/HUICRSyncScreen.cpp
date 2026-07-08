#include "HUICRSyncScreen.h"

#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/GameInstance.h"
#include "HUICRSyncCalibrationSaveGame.h"
#include "HUICRSyncSubsystem.h"

AHUICRSyncScreen::AHUICRSyncScreen()
{
	PrimaryActorTick.bCanEverTick = false;

	DefaultSceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("DefaultSceneRoot"));
	RootComponent = DefaultSceneRoot;

	ScreenMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ScreenMesh"));
	ScreenMesh->SetupAttachment(DefaultSceneRoot);

	TextComponent = CreateDefaultSubobject<UTextRenderComponent>(TEXT("TextComponent"));
	TextComponent->SetupAttachment(ScreenMesh);
}

void AHUICRSyncScreen::ChangeScreenID(int32 NewScreenID)
{
	ScreenID = NewScreenID;

	if (TextComponent)
	{
		TextComponent->SetText(FText::AsNumber(ScreenID));
	}
}

void AHUICRSyncScreen::ChangeScreenScaleY(float ScaleY)
{
	const FVector CurrentScale = GetActorScale3D();
	SetActorScale3D(FVector(CurrentScale.X, ScaleY, CurrentScale.Z));
}

void AHUICRSyncScreen::ChangeScreenScaleZ(float ScaleZ)
{
	const FVector CurrentScale = GetActorScale3D();
	SetActorScale3D(FVector(CurrentScale.X, CurrentScale.Y, ScaleZ));
}

void AHUICRSyncScreen::ChangeIfControlRemoteScreen(bool bChecked)
{
	bControlRemoteScreen = bChecked;
}

void AHUICRSyncScreen::InitScreen(UHUICRSyncCalibrationSaveGame* SaveGame)
{
	InitialTransform = GetActorTransform();

	if (bAutoAssignScreenID && SyncSubsystem)
	{
		ChangeScreenID(SyncSubsystem->AllocateActorID(EHUICRSyncActorType::HUIScreen));
	}
	else
	{
		ChangeScreenID(ScreenID);
	}

	if (SaveGame)
	{
		const FVector SavedScale = SaveGame->ScreenScaleMap.FindRef(ScreenID);
		SetActorScale3D(SavedScale.IsNearlyZero() ? GetActorScale3D() : SavedScale);

		if (const FRotator* SavedRotation = SaveGame->ScreenRotationMap.Find(ScreenID))
		{
			AddActorLocalRotation(*SavedRotation);
		}

		if (const FVector* SavedOffset = SaveGame->ScreenOffsetMap.Find(ScreenID))
		{
			AddActorWorldOffset(*SavedOffset);
		}

		if (const bool* SavedControlState = SaveGame->ShouldScreenControlRemoteMap.Find(ScreenID))
		{
			bControlRemoteScreen = *SavedControlState;
		}
		else
		{
			SaveGame->ShouldScreenControlRemoteMap.Add(ScreenID, bControlRemoteScreen);
		}
	}

}

void AHUICRSyncScreen::SaveScreenCalibration(UHUICRSyncCalibrationSaveGame* SaveGame) const
{
	if (!SaveGame)
	{
		return;
	}

	SaveGame->ScreenOffsetMap.Add(ScreenID, GetActorLocation() - InitialTransform.GetLocation());
	SaveGame->ScreenRotationMap.Add(ScreenID, GetActorRotation() - InitialTransform.Rotator());
	SaveGame->ScreenScaleMap.Add(ScreenID, GetActorScale3D());
	SaveGame->ShouldScreenControlRemoteMap.Add(ScreenID, bControlRemoteScreen);
}

FHUICRSyncScreenPayload AHUICRSyncScreen::BuildScreenPayload() const
{
	FHUICRSyncScreenPayload Payload;
	Payload.ScreenID = ScreenID;
	Payload.Transform = GetActorTransform();
	Payload.bRemoteControlsLocalScreen = bControlRemoteScreen;
	return Payload;
}

void AHUICRSyncScreen::ApplyScreenPayload(const FHUICRSyncScreenPayload& Payload)
{
	ChangeScreenID(Payload.ScreenID);
	SetActorTransform(Payload.Transform);
	bControlRemoteScreen = Payload.bRemoteControlsLocalScreen;
}

void AHUICRSyncScreen::SpawnDissolveBoxActor()
{
	if (!ScreenDissolveBoxClass)
	{
		return;
	}

	DestroyDissolveBoxActor();

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.bNoFail = true;
	SpawnParams.Owner = this;

	SpawnedDissolveBox = GetWorld()->SpawnActor<AActor>(ScreenDissolveBoxClass, GetActorTransform(), SpawnParams);
	if (SpawnedDissolveBox)
	{
		SpawnedDissolveBox->SetActorScale3D(FVector(1.0f, GetActorScale3D().Y, GetActorScale3D().Z));
	}
}

void AHUICRSyncScreen::DestroyDissolveBoxActor()
{
	if (IsValid(SpawnedDissolveBox))
	{
		SpawnedDissolveBox->Destroy();
	}

	SpawnedDissolveBox = nullptr;
}

void AHUICRSyncScreen::BeginPlay()
{
	Super::BeginPlay();

	SetActorHiddenInGame(true);

	if (UGameInstance* GameInstance = GetGameInstance())
	{
		SyncSubsystem = GameInstance->GetSubsystem<UHUICRSyncSubsystem>();
	}

	if (SyncSubsystem)
	{
		SyncSubsystem->OnCalibrationStateChanged.AddDynamic(this, &AHUICRSyncScreen::HandleCalibrationStateChanged);
	}

	ChangeScreenID(ScreenID);
}

void AHUICRSyncScreen::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (SyncSubsystem)
	{
		SyncSubsystem->OnCalibrationStateChanged.RemoveDynamic(this, &AHUICRSyncScreen::HandleCalibrationStateChanged);
	}

	Super::EndPlay(EndPlayReason);
}

void AHUICRSyncScreen::HandleCalibrationStateChanged(bool bNewCalibrationState)
{
	SetActorHiddenInGame(!bNewCalibrationState);

	if (bNewCalibrationState)
	{
		DestroyDissolveBoxActor();
	}
}
