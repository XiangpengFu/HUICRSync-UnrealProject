#include "HUICRSyncPCScreenVisual.h"

#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"

AHUICRSyncPCScreenVisual::AHUICRSyncPCScreenVisual()
{
	PrimaryActorTick.bCanEverTick = false;

	DefaultSceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("DefaultSceneRoot"));
	RootComponent = DefaultSceneRoot;

	ScreenMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ScreenMesh"));
	ScreenMesh->SetupAttachment(DefaultSceneRoot);

	TextComponent = CreateDefaultSubobject<UTextRenderComponent>(TEXT("TextComponent"));
	TextComponent->SetupAttachment(ScreenMesh);

	ActorID = -1;
	TypeCode = EHUICRSyncActorType::HUIScreen;
}

void AHUICRSyncPCScreenVisual::BindScreenComponent(int32 ScreenID, USceneComponent* ScreenComponent)
{
	ActorID = ScreenID;
	ReferencedScreenComponent = ScreenComponent;

	if (TextComponent)
	{
		TextComponent->SetText(FText::AsNumber(ActorID));
	}

	UpdateTransformFromReferencedScreen();
}

void AHUICRSyncPCScreenVisual::UpdateTransformFromReferencedScreen()
{
	if (!IsValid(ReferencedScreenComponent))
	{
		return;
	}

	SetActorLocation(ReferencedScreenComponent->GetComponentLocation());
	SetActorRotation(ReferencedScreenComponent->GetComponentRotation());

	const FVector RefScale = ReferencedScreenComponent->GetComponentScale();
	SetActorScale3D(FVector(RefScale.X * XScaleMultiplier, RefScale.Y, RefScale.Z));
}

void AHUICRSyncPCScreenVisual::SetCalibrationVisible(bool bVisible)
{
	if (TextComponent)
	{
		TextComponent->SetHiddenInGame(!bVisible);
	}
}

void AHUICRSyncPCScreenVisual::BeginPlay()
{
	Super::BeginPlay();

	SetCalibrationVisible(false);
}
