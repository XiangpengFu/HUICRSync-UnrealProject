#pragma once

#include "HUICRSyncActor.h"
#include "HUICRSyncPawnEndpoint.generated.h"

/** Uses the actor field protocol on behalf of the locally possessed APawn. */
UCLASS(Transient, NotBlueprintable, NotPlaceable)
class AHUICRSyncPawnEndpoint final : public AHUICRSyncActor
{
	GENERATED_BODY()

public:
	AHUICRSyncPawnEndpoint();
	virtual void OnPrepareSyncFields_Implementation(const TArray<FName>& FieldsToPrepare) override;
	virtual void OnSyncFieldsReceived_Implementation() override;
};
