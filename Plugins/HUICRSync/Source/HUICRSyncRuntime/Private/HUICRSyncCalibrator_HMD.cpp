#include "HUICRSyncCalibrator_HMD.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/GameInstance.h"
#include "HUICRSyncCalibrationSaveGame.h"
#include "HUICRSyncScreen.h"
#include "HUICRSyncSubsystem.h"

AHUICRSyncCalibrator_HMD::AHUICRSyncCalibrator_HMD()
{
	PrimaryActorTick.bCanEverTick = true;

	DefaultSceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("DefaultSceneRoot"));
	RootComponent = DefaultSceneRoot;

	CalibratorMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("CalibratorMesh"));
	CalibratorMesh->SetupAttachment(DefaultSceneRoot);
}

void AHUICRSyncCalibrator_HMD::InitCalibrator(UHUICRSyncCalibrationSaveGame* SaveGame)
{
	CalibratorID = GetEffectiveCalibratorID();

	if (SaveGame)
	{
		SetActorScale3D(SaveGame->HUIScale);
	}

	SetupTransformWithScreen();
}

void AHUICRSyncCalibrator_HMD::SetupTransformWithScreen()
{
	if (!BoundScreen)
	{
		return;
	}

	SetActorLocation(BoundScreen->GetActorLocation());
	SetActorRotation(BoundScreen->GetActorRotation());
}

FHUICRSyncCalibratorPayload AHUICRSyncCalibrator_HMD::BuildCalibratorPayload() const
{
	FHUICRSyncCalibratorPayload Payload;
	Payload.ActorID = GetEffectiveCalibratorID();
	Payload.Transform = GetActorTransform();
	return Payload;
}

int32 AHUICRSyncCalibrator_HMD::GetEffectiveCalibratorID() const
{
	return BoundScreen ? BoundScreen->ScreenID : CalibratorID;
}

void AHUICRSyncCalibrator_HMD::BeginPlay()
{
	Super::BeginPlay();

	SetActorHiddenInGame(true);

	if (UGameInstance* GameInstance = GetGameInstance())
	{
		SyncSubsystem = GameInstance->GetSubsystem<UHUICRSyncSubsystem>();
	}

	if (SyncSubsystem)
	{
		SyncSubsystem->OnCalibrationStateChanged.AddDynamic(this, &AHUICRSyncCalibrator_HMD::HandleCalibrationStateChanged);
	}
}

void AHUICRSyncCalibrator_HMD::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (SyncSubsystem)
	{
		SyncSubsystem->OnCalibrationStateChanged.RemoveDynamic(this, &AHUICRSyncCalibrator_HMD::HandleCalibrationStateChanged);
	}

	Super::EndPlay(EndPlayReason);
}

void AHUICRSyncCalibrator_HMD::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (bFollowBoundScreenEveryTick)
	{
		SetupTransformWithScreen();
	}
}

void AHUICRSyncCalibrator_HMD::HandleCalibrationStateChanged(bool bNewCalibrationState)
{
	SetActorHiddenInGame(!bNewCalibrationState);
}
