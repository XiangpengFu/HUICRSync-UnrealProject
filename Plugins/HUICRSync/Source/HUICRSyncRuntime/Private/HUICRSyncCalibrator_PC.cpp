#include "HUICRSyncCalibrator_PC.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/GameInstance.h"
#include "HUICRSyncSubsystem.h"

AHUICRSyncCalibrator_PC::AHUICRSyncCalibrator_PC()
{
	PrimaryActorTick.bCanEverTick = true;

	DefaultSceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("DefaultSceneRoot"));
	RootComponent = DefaultSceneRoot;

	CalibratorMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("CalibratorMesh"));
	CalibratorMesh->SetupAttachment(DefaultSceneRoot);
}

void AHUICRSyncCalibrator_PC::BindScreenComponent(int32 ScreenID, USceneComponent* ScreenComponent)
{
	CalibratorID = ScreenID;
	BoundScreenComponent = ScreenComponent;
	UpdateTransformFromBoundScreen();
}

void AHUICRSyncCalibrator_PC::UpdateTransformFromBoundScreen()
{
	if (!IsValid(BoundScreenComponent))
	{
		return;
	}

	FTransform ScreenTransform = BoundScreenComponent->GetComponentTransform();
	ScreenTransform.SetScale3D(GetActorScale3D());
	SetActorTransform(ScreenTransform);
}

void AHUICRSyncCalibrator_PC::SetCalibratorScale(const FVector& NewScale)
{
	SetActorScale3D(NewScale);
	UpdateTransformFromBoundScreen();
}

void AHUICRSyncCalibrator_PC::BeginPlay()
{
	Super::BeginPlay();

	SetActorHiddenInGame(true);

	if (UGameInstance* GameInstance = GetGameInstance())
	{
		SyncSubsystem = GameInstance->GetSubsystem<UHUICRSyncSubsystem>();
	}

	// if (SyncSubsystem)
	// {
	// 	SyncSubsystem->OnCalibrationStateChanged.AddDynamic(this, &AHUICRSyncCalibrator_PC::HandleCalibrationStateChanged);
	// }
}

void AHUICRSyncCalibrator_PC::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (SyncSubsystem)
	{
		SyncSubsystem->OnCalibrationStateChanged.RemoveDynamic(this, &AHUICRSyncCalibrator_PC::HandleCalibrationStateChanged);
	}

	Super::EndPlay(EndPlayReason);
}

void AHUICRSyncCalibrator_PC::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	UpdateTransformFromBoundScreen();
}

void AHUICRSyncCalibrator_PC::HandleCalibrationStateChanged(bool bNewCalibrationState)
{
	SetActorHiddenInGame(!bNewCalibrationState);
}
