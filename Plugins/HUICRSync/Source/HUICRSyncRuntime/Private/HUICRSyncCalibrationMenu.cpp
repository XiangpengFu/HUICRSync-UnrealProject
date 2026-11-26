#include "HUICRSyncCalibrationMenu.h"

#include "Components/Button.h"
#include "Components/CheckBox.h"
#include "Components/TextBlock.h"
#include "Engine/GameInstance.h"
#include "EngineUtils.h"
#include "HUICRSyncHMDManagerBase.h"
#include "HUICRSyncSubsystem.h"

#define LOCTEXT_NAMESPACE "HUICRSyncCalibrationMenu"

void UHUICRSyncCalibrationMenu::NativeConstruct()
{
	Super::NativeConstruct();
	// The connection controls occupy the row below the title on this menu.
	if (GetWidgetFromName(TEXT("ApplyPCIP")))
	{
		if (UWidget* Hint = GetWidgetFromName(TEXT("CalibrationHint")))
		{
			Hint->RemoveFromParent();
		}
	}
	if (const UGameInstance* Game = GetGameInstance())
	{
		Session = Game->GetSubsystem<UHUICRSyncSubsystem>();
	}
	if (Session.IsValid())
	{
		Session->OnCalibrationUIStateChanged.AddUniqueDynamic(this, &ThisClass::RefreshCalibrationControls);
	}
	if (UButton* Cancel = Cast<UButton>(GetWidgetFromName(TEXT("CancelCalibrationRound"))))
	{
		Cancel->OnClicked.AddUniqueDynamic(this, &ThisClass::CancelTurn);
	}
	if (UCheckBox* Motion = Cast<UCheckBox>(GetWidgetFromName(TEXT("GameMotionParallaxCheckBox"))))
	{
		Motion->OnCheckStateChanged.AddUniqueDynamic(this, &ThisClass::SetGameParallax);
	}
	RefreshCalibrationControls();
}

void UHUICRSyncCalibrationMenu::NativeDestruct()
{
	if (Session.IsValid())
	{
		Session->OnCalibrationUIStateChanged.RemoveDynamic(this, &ThisClass::RefreshCalibrationControls);
	}
	Session.Reset();
	Super::NativeDestruct();
}

void UHUICRSyncCalibrationMenu::RefreshCalibrationControls()
{
	if (!Session.IsValid())
	{
		return;
	}
	const bool bManaged = Session->UsesManagedCalibration();
	const bool bCanEditScreen = !bManaged || (Session->bCanEditPCScreenLayout && !Session->bWaitingForCalibrationResult);
	const bool bCanEditMotion = !bManaged || Session->bCanEditGameMotionParallax;
	const FText HostOnly = LOCTEXT("HostOnly", "Host only");
	const FLinearColor DisabledColor(0.65, 0.65, 0.65);
	auto SetPermission = [&](const TCHAR* Box, const TCHAR* Label, bool bCanEdit, const FText& OriginalText)
	{
		if (UCheckBox* CheckBox = Cast<UCheckBox>(GetWidgetFromName(Box)))
		{
			CheckBox->SetIsEnabled(bCanEdit);
			CheckBox->SetToolTipText(bCanEdit ? FText::GetEmpty() : HostOnly);
		}
		if (UTextBlock* Text = Cast<UTextBlock>(GetWidgetFromName(Label)))
		{
			Text->SetColorAndOpacity(FSlateColor(bCanEdit ? FLinearColor::White : DisabledColor));
			Text->SetText(OriginalText);
		}
	};
	SetPermission(TEXT("ShouldControlePCScreenCheckBox"), TEXT("ShouldControlPCScreen?"), bCanEditScreen,
		LOCTEXT("ControlScreen", "Should Control PC Screen?"));
	SetPermission(TEXT("MotionParallaxCheckBox"), TEXT("MotionParallax"), bCanEditMotion,
		LOCTEXT("MotionParallax", "Motion Parallax"));
	SetPermission(TEXT("GameMotionParallaxCheckBox"), TEXT("GameMotionParallaxLabel"), bCanEditMotion,
		LOCTEXT("MotionParallax", "Motion Parallax"));
	if (bManaged)
	{
		for (const TCHAR* Name : {TEXT("MotionParallaxCheckBox"), TEXT("GameMotionParallaxCheckBox")})
		{
			if (UCheckBox* Motion = Cast<UCheckBox>(GetWidgetFromName(Name)))
			{
				Motion->SetIsChecked(Session->bMotionParallaxState);
			}
		}
	}

	const bool bWaitingForHost = bManaged && !Session->bIsLocalHMDHost && Session->ScreenLayoutVersion == 0;
	if (UButton* Start = Cast<UButton>(GetWidgetFromName(TEXT("StartCalibration"))))
	{
		Start->SetIsEnabled(!bManaged || Session->bCanStartCalibration);
	}
	if (UButton* Stop = Cast<UButton>(GetWidgetFromName(TEXT("StopCalibration"))))
	{
		Stop->SetIsEnabled(!Session->bWaitingForCalibrationResult);
	}
	if (UWidget* Cancel = GetWidgetFromName(TEXT("CancelCalibrationRound")))
	{
		Cancel->SetVisibility(Session->bIsLocalHMDCalibrating ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
	if (UWidget* Box = GetWidgetFromName(TEXT("GameMotionParallaxCheckBox")))
	{
		Box->SetVisibility(bManaged ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
	if (UWidget* Label = GetWidgetFromName(TEXT("GameMotionParallaxLabel")))
	{
		Label->SetVisibility(bManaged ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	}
	if (UTextBlock* Hint = Cast<UTextBlock>(GetWidgetFromName(TEXT("CalibrationHint"))))
	{
		FText Message;
		if (!Session->CalibrationMessage.IsEmpty())
		{
			Message = FText::FromString(Session->CalibrationMessage);
		}
		else if (bWaitingForHost || (Session->bHostCalibrationActive && !Session->bIsLocalHMDCalibrating))
		{
			Message = LOCTEXT("WaitForLayout", "Waiting for the Host to confirm the screen layout.");
		}
		else if (Session->bIsLocalHMDCalibrating && !Session->bCanEditPCScreenLayout)
		{
			Message = LOCTEXT("GuestHelp", "Align screen 0. Other screens are placed automatically.");
		}
		else if (bManaged && Session->HasAppliedCalibration() && !Session->bLocalSceneReady)
		{
			Message = LOCTEXT("SyncingScene", "Synchronizing the shared scene...");
		}
		else if (bManaged && !Session->bIsLocalHMDHost)
		{
			Message = LOCTEXT("Guest", "Motion Parallax is Host only.");
		}
		if (Session->bLayoutUpdatedSinceMyCalibration && !Session->bIsLocalHMDCalibrating)
		{
			Message = LOCTEXT("LayoutChanged", "Screen layout updated. Calibrate again when ready.");
		}
		Hint->SetText(Message);
	}
}

void UHUICRSyncCalibrationMenu::CancelTurn()
{
	if (Session.IsValid())
	{
		Session->CancelCalibrationTurn();
	}
}

void UHUICRSyncCalibrationMenu::SetGameParallax(bool bEnabled)
{
	for (TActorIterator<AHUICRSyncHMDManagerBase> It(GetWorld()); It; ++It)
	{
		It->ChangeMotionParallaxState(bEnabled);
		break;
	}
}

#undef LOCTEXT_NAMESPACE
