#pragma once

#if WITH_DEV_AUTOMATION_TESTS
#include "CoreMinimal.h"

class FAutomationTestBase;
class UHUICRSyncSubsystem;
class UWorld;

namespace HUICRSyncSpawnNodeTest
{
	bool Verify(FAutomationTestBase& Test, UWorld& World, UHUICRSyncSubsystem& PC, UHUICRSyncSubsystem& Host,
		UHUICRSyncSubsystem& Guest, TFunctionRef<bool(const TCHAR*, TFunctionRef<bool()>)> WaitFor);
}
#endif
