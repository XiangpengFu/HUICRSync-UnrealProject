#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "HUICRSyncTypes.h"
#include "HUICRSyncScreen.generated.h"

class UHUICRSyncCalibrationSaveGame;
class UHUICRSyncSubsystem;
class UTextRenderComponent;

UCLASS(Blueprintable)
class HUICRSYNCRUNTIME_API AHUICRSyncScreen : public AActor
{
	GENERATED_BODY()

public:
	AHUICRSyncScreen();

	UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "HUICR Sync")
	UHUICRSyncSubsystem* SyncSubsystem = nullptr;

	UPROPERTY(BlueprintReadWrite, VisibleAnywhere, Category = "HUICR Sync|Components")
	USceneComponent* DefaultSceneRoot = nullptr;

	UPROPERTY(BlueprintReadWrite, VisibleAnywhere, Category = "HUICR Sync|Components")
	UStaticMeshComponent* ScreenMesh = nullptr;

	UPROPERTY(BlueprintReadWrite, VisibleAnywhere, Category = "HUICR Sync|Components")
	UTextRenderComponent* TextComponent = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Screen")
	int32 ScreenID = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Screen")
	bool bAutoAssignScreenID = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Screen")
	bool bControlRemoteScreen = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Screen", meta = (DisplayName = "Enable Dissolve Box"))
	bool bSpawnDissolveBoxOnInit = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Screen")
	TSubclassOf<AActor> ScreenDissolveBoxClass;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Screen")
	AActor* SpawnedDissolveBox = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Screen")
	FTransform InitialTransform;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Screen")
	FString PersistentScreenKey;

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Screen")
	void ChangeScreenID(int32 NewScreenID);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Screen")
	void ChangeScreenScaleY(float ScaleY);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Screen")
	void ChangeScreenScaleZ(float ScaleZ);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Screen")
	void ChangeIfControlRemoteScreen(bool bChecked);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Screen")
	FString ResolvePersistentScreenKey();

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Screen")
	void InitScreen(UHUICRSyncCalibrationSaveGame* SaveGame);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Screen")
	void SaveScreenCalibration(UHUICRSyncCalibrationSaveGame* SaveGame) const;

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Screen")
	FHUICRSyncScreenPayload BuildScreenPayload() const;

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Screen")
	void ApplyScreenPayload(const FHUICRSyncScreenPayload& Payload);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Screen")
	void SpawnDissolveBoxActor();

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Screen")
	void DestroyDissolveBoxActor();

protected:
	UPROPERTY(Transient)
	bool bHasInitializedScreen = false;

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UFUNCTION()
	void HandleCalibrationStateChanged(bool bNewCalibrationState);
};
