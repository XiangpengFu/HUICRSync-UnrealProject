#include "HUICRSyncCalibrationSaveGame.h"

UHUICRSyncCalibrationSaveGame::UHUICRSyncCalibrationSaveGame()
{
	PCIpAddress = TEXT("0.0.0.0");
	TargetPCSyncPort = 4001;
	ScreenOffsetMap.Add(0, FVector::ZeroVector);
	ScreenRotationMap.Add(0, FRotator::ZeroRotator);
	ScreenScaleMap.Add(0, FVector::OneVector);
	ShouldScreenControlRemoteMap.Add(0, true);
}
