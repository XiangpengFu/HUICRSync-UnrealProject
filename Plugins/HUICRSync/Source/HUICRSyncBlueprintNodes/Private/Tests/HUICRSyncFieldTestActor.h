#pragma once

#include "HUICRSyncActor.h"
#include "HUICRSyncFieldTestActor.generated.h"

// A cook-excluded fixture whose schema is available on both sides of a real scene spawn.
UCLASS(Transient, NotBlueprintable, NotPlaceable)
class AHUICRSyncFieldTestActor : public AHUICRSyncActor
{
	GENERATED_BODY()
public:
	AHUICRSyncFieldTestActor() { SharedPayloadStruct = FHUICRSyncScreenPayload::StaticStruct(); }
};
