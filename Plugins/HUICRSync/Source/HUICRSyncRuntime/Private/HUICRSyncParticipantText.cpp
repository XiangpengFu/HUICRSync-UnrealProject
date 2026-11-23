#include "HUICRSyncParticipantText.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HUICRSyncSubsystem.h"

#define LOCTEXT_NAMESPACE "HUICRSyncParticipantText"

TSharedRef<SWidget> UHUICRSyncParticipantText::RebuildWidget()
{
	TSharedRef<SWidget> Widget = Super::RebuildWidget();
	if (!IsDesignTime())
	{
		const UWorld* World = GetWorld();
		UGameInstance* Game = World ? World->GetGameInstance() : nullptr;
		Session = Game ? Game->GetSubsystem<UHUICRSyncSubsystem>() : nullptr;
		if (Session.IsValid())
		{
			Session->OnParticipantsChanged.AddUniqueDynamic(this, &ThisClass::RefreshParticipantText);
		}
		RefreshParticipantText();
	}
	return Widget;
}

void UHUICRSyncParticipantText::ReleaseSlateResources(bool bReleaseChildren)
{
	if (Session.IsValid())
	{
		Session->OnParticipantsChanged.RemoveDynamic(this, &ThisClass::RefreshParticipantText);
		Session.Reset();
	}
	Super::ReleaseSlateResources(bReleaseChildren);
}

void UHUICRSyncParticipantText::RefreshParticipantText()
{
	if (!Session.IsValid() || !Session->bConnectionState || Session->LocalParticipantId <= 0)
	{
		SetText(LOCTEXT("Offline", "Offline"));
		return;
	}
	const FText Role = Session->bIsLocalHMDHost ? LOCTEXT("Host", "Host") : LOCTEXT("Guest", "Guest");
	SetText(FText::Format(LOCTEXT("Status", "{0} #{1} | {2} online"),
		Role, Session->LocalParticipantId, Session->ConnectedHMDCount));
}

#undef LOCTEXT_NAMESPACE
