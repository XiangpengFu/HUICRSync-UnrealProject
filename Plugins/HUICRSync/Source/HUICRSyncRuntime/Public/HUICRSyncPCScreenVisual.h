#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "HUICRSyncTypes.h"
#include "HUICRSyncPCScreenVisual.generated.h"

class UStaticMeshComponent;
class UTextRenderComponent;

UCLASS(Blueprintable)
class HUICRSYNCRUNTIME_API AHUICRSyncPCScreenVisual : public AActor
{
	GENERATED_BODY()

public:
	AHUICRSyncPCScreenVisual();

	UPROPERTY(BlueprintReadWrite, VisibleAnywhere, Category = "HUICR Sync|Components")
	USceneComponent* DefaultSceneRoot = nullptr;

	UPROPERTY(BlueprintReadWrite, VisibleAnywhere, Category = "HUICR Sync|Components")
	UStaticMeshComponent* ScreenMesh = nullptr;

	UPROPERTY(BlueprintReadWrite, VisibleAnywhere, Category = "HUICR Sync|Components")
	UTextRenderComponent* TextComponent = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	int32 ActorID = -1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	EHUICRSyncActorType TypeCode = EHUICRSyncActorType::HUIScreen;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Screen")
	USceneComponent* ReferencedScreenComponent = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Screen")
	float XScaleMultiplier = 2.0f;

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Screen")
	void BindScreenComponent(int32 ScreenID, USceneComponent* ScreenComponent);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Screen")
	void UpdateTransformFromReferencedScreen();

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Screen")
	void SetCalibrationVisible(bool bVisible);

protected:
	virtual void BeginPlay() override;
};
