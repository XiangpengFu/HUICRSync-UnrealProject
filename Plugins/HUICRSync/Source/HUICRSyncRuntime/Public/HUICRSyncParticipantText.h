#pragma once

#include "Components/TextBlock.h"
#include "HUICRSyncParticipantText.generated.h"

class UHUICRSyncSubsystem;

/** Shows this headset's role, participant number and the live headset count. */
UCLASS(meta=(DisplayName="HUICR Headset Status"))
class HUICRSYNCRUNTIME_API UHUICRSyncParticipantText : public UTextBlock
{
	GENERATED_BODY()

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void ReleaseSlateResources(bool bReleaseChildren) override;

private:
	UFUNCTION()
	void RefreshParticipantText();

	TWeakObjectPtr<UHUICRSyncSubsystem> Session;
};
