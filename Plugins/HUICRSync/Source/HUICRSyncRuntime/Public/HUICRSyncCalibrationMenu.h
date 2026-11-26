#pragma once

#include "Blueprint/UserWidget.h"
#include "HUICRSyncCalibrationMenu.generated.h"

class UHUICRSyncSubsystem;
class UHUICRSyncSpaceSetupSubsystem;

UCLASS()
class HUICRSYNCRUNTIME_API UHUICRSyncCalibrationMenu : public UUserWidget
{
	GENERATED_BODY()

protected:
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;

private:
	TWeakObjectPtr<UHUICRSyncSubsystem> Session;
	TWeakObjectPtr<UHUICRSyncSpaceSetupSubsystem> SpaceSetup;

	UFUNCTION()
	void RefreshCalibrationControls();

	UFUNCTION()
	void OpenSpaceSetup();

	UFUNCTION()
	void CancelTurn();

	UFUNCTION()
	void SetGameParallax(bool bEnabled);
};
