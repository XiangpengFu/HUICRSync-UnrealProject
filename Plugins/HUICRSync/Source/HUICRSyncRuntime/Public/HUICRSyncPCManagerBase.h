#pragma once

#include "CoreMinimal.h"
#include "HUICRSyncManagerBase.h"
#include "HUICRSyncPCManagerBase.generated.h"

class UStaticMeshComponent;
class AHUICRSyncPCScreenVisual;
class AHUICRSyncCalibrator_PC;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FHUICRSyncTransformPayloadReceived, const FTransform&, Transform);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FHUICRSyncPCScreenTransformResolved, int32, ScreenID, const FHUICRSyncScreenPayload&, HMDPayload, const FTransform&, PCTransform);

USTRUCT(BlueprintType)
struct HUICRSYNCRUNTIME_API FHUICRSyncPCScreenBinding
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	int32 ScreenID = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	USceneComponent* ScreenComponent = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	FTransform InitialTransform;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	bool bApplyTransformAutomatically = true;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync")
	AHUICRSyncPCScreenVisual* SpawnedVisualActor = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync")
	AHUICRSyncCalibrator_PC* SpawnedCalibrator = nullptr;
};

UCLASS(Blueprintable)
class HUICRSYNCRUNTIME_API AHUICRSyncPCManagerBase : public AHUICRSyncManagerBase
{
	GENERATED_BODY()

public:
	AHUICRSyncPCManagerBase();

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Calibration")
	FHUICRSyncCalibratorPayload LastHMDCalibratorPayload;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Calibration")
	TMap<int32, FHUICRSyncScreenPayload> LastHMDScreenPayloads;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|nDisplay")
	bool bInitializeNDisplayOnBeginPlay = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|nDisplay")
	FName ScreenComponentTag = TEXT("Screen");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|nDisplay")
	FName MainScreenComponentName = TEXT("nDisplayScreenMain");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|nDisplay")
	FName DefaultViewportComponentName = TEXT("DefaultViewPoint");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|nDisplay")
	bool bApplyMainScreenTransformAutomatically = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Calibration")
	bool bAutoSpawnPCCalibrators = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Calibration")
	TSubclassOf<AHUICRSyncCalibrator_PC> PCCalibratorClass;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Calibration")
	FVector PCCalibratorScale = FVector::OneVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|nDisplay")
	bool bRestoreDefaultViewportWhenMotionParallaxDisabled = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|Screen")
	bool bScreenDissolveBoxHidden = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync|nDisplay")
	TSubclassOf<AHUICRSyncPCScreenVisual> PCScreenVisualClass;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|nDisplay")
	AActor* NDisplayActor = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|nDisplay")
	USceneComponent* DefaultViewportComponent = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|nDisplay")
	UStaticMeshComponent* PCMainScreen = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|nDisplay")
	FTransform DefaultViewportTransform;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|nDisplay")
	FVector DefaultViewportOffsetFromMainScreen = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|nDisplay")
	TArray<FHUICRSyncPCScreenBinding> PCScreenBindings;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Motion Parallax")
	FTransform LastHMDTransform;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Motion Parallax")
	FTransform LastResolvedDefaultViewportTransform;

	UPROPERTY(BlueprintAssignable, Category = "HUICR Sync|nDisplay")
	FHUICRSyncPCScreenTransformResolved OnPCScreenTransformResolved;

	UPROPERTY(BlueprintAssignable, Category = "HUICR Sync|Motion Parallax")
	FHUICRSyncTransformPayloadReceived OnHMDTransformReceived;

	UPROPERTY(BlueprintAssignable, Category = "HUICR Sync|Motion Parallax")
	FHUICRSyncTransformPayloadReceived OnDefaultViewportTransformResolved;

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Calibration")
	void SetPCCalibratorTransform(const FTransform& InPCCalibratorTransform);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Calibration")
	bool RecalculateHMDPCTransforms();

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Calibration")
	bool RefreshPCCalibratorTransform();

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Calibration")
	bool SpawnPCCalibrator(int32 ScreenID);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Calibration")
	int32 SpawnAllPCCalibrators();

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Calibration")
	void SetPCCalibratorScale(const FVector& NewScale);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Calibration")
	void ApplyPCCalibratorScale();

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|nDisplay")
	bool InitializeNDisplayScreens();

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|nDisplay")
	bool RegisterPCScreenComponent(int32 ScreenID, USceneComponent* ScreenComponent, bool bApplyTransformAutomatically);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|nDisplay")
	bool SpawnPCScreenVisual(int32 ScreenID);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|nDisplay")
	bool UpdatePCScreenVisual(int32 ScreenID);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|nDisplay")
	bool RestorePCScreen(int32 ScreenID);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|nDisplay")
	bool ApplyHMDScreenPayload(const FHUICRSyncScreenPayload& Payload);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|nDisplay")
	bool SendPCScreenPayload(int32 ScreenID);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|nDisplay")
	bool SelectPCScreen(int32 ScreenID);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|nDisplay")
	bool RestoreDefaultViewport();

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Motion Parallax")
	bool ApplyHMDTransformToDefaultViewport(const FTransform& HMDTransform);

	UFUNCTION(BlueprintCallable, Category = "HUICR Sync|Payload")
	bool SendLocalScreenPayload(int32 ScreenID, const FTransform& ScreenTransform, bool bRemoteControlsLocalScreen);

protected:
	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Calibration")
	FTransform PCCalibratorTransform;

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void HandleNetDataReceived() override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	UFUNCTION()
	void OnSubsystemMotionParallaxStateChanged(bool bNewMotionParallaxState);

	UFUNCTION()
	void OnSubsystemCalibrationStateChanged(bool bNewCalibrationState);

	UFUNCTION()
	void OnSubsystemSelectedScreenChanged(int32 ScreenID);

	UFUNCTION()
	void OnSubsystemCommandReceived(const FString& Address, const TArray<FHUICRSyncCommandArg>& Args);

	FHUICRSyncPCScreenBinding* FindPCScreenBinding(int32 ScreenID);
	const FHUICRSyncPCScreenBinding* FindPCScreenBinding(int32 ScreenID) const;
};
