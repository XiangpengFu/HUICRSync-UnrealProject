#pragma once

#include "Modules/ModuleManager.h"

class FHUICRSyncRuntimeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
