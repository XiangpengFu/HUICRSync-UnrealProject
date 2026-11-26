#include "HUICRSyncSubsystem.h"

#include "Components/PrimitiveComponent.h"
#include "Engine/GameInstance.h"
#include "HUICRSyncActor.h"
#include "HUICRSyncHMDManagerBase.h"
#include "HUICRSyncPCManagerBase.h"
#include "HUICRSyncSpaceSetupSubsystem.h"
#include "Misc/Base64.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

namespace HUICRSyncCalibrationProtocol
{
const TCHAR* Prefix = TEXT("/HUICRSync/Calibration/");
const TCHAR* Join = TEXT("/HUICRSync/Calibration/Join");
const TCHAR* Cancel = TEXT("/HUICRSync/Calibration/Cancel");
const TCHAR* Select = TEXT("/HUICRSync/Calibration/Select");
const TCHAR* Submit = TEXT("/HUICRSync/Calibration/Submit");
const TCHAR* CalibrationResult = TEXT("/HUICRSync/Calibration/Result");
const TCHAR* Applied = TEXT("/HUICRSync/Calibration/Applied");
const TCHAR* Restore = TEXT("/HUICRSync/Calibration/Restore");
const TCHAR* CalibrationError = TEXT("/HUICRSync/Calibration/Error");
constexpr int32 ScreenStatusBytes = sizeof(int32) + sizeof(uint8);

FHUICRSyncCommandArg Integer(int32 Value)
{
	FHUICRSyncCommandArg Arg;
	Arg.Type = EHUICRSyncCommandArgType::Int32;
	Arg.IntValue = Value;
	return Arg;
}

FHUICRSyncCommandArg String(const FString& Value)
{
	FHUICRSyncCommandArg Arg;
	Arg.Type = EHUICRSyncCommandArgType::String;
	Arg.StringValue = Value;
	return Arg;
}

FString EncodeLayout(FHUICRSyncCalibrationLayout Layout)
{
	TArray<uint8> Bytes;
	FMemoryWriter Writer(Bytes);
	int32 Count = Layout.Screens.Num();
	Writer << Layout.Version << Layout.Id << Layout.MainReference << Count;
	for (FHUICRSyncScreenPayload& Screen : Layout.Screens)
	{
		uint8 bEdited = Layout.EditedScreenIds.Contains(Screen.ScreenID);
		Writer << Screen << Screen.InitialTransform << bEdited;
	}
	return FBase64::Encode(Bytes);
}

bool DecodeLayout(const FString& Encoded, FHUICRSyncCalibrationLayout& Layout)
{
	TArray<uint8> Bytes;
	if (Encoded.Len() > 12000 || !FBase64::Decode(Encoded, Bytes))
	{
		return false;
	}
	FMemoryReader Reader(Bytes);
	int32 Count = 0;
	Reader << Layout.Version << Layout.Id << Layout.MainReference << Count;
	if (Reader.IsError() || Count < 0 || Count > 48 || !Layout.MainReference.IsValid())
	{
		return false;
	}
	TSet<int32> ScreenIDs;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		FHUICRSyncScreenPayload Screen;
		uint8 bEdited = 0;
		Reader << Screen << Screen.InitialTransform << bEdited;
		if (Reader.IsError() || bEdited > 1 || Screen.ScreenID < 0 || ScreenIDs.Contains(Screen.ScreenID) ||
			!Screen.Transform.IsValid() || !Screen.InitialTransform.IsValid())
		{
			return false;
		}
		ScreenIDs.Add(Screen.ScreenID);
		if (bEdited)
		{
			Layout.EditedScreenIds.Add(Screen.ScreenID);
		}
		Layout.Screens.Add(Screen);
	}
	return Reader.AtEnd();
}

bool IsSameScreenLayout(const FHUICRSyncCalibrationLayout& A, const FHUICRSyncCalibrationLayout& B)
{
	return A.Id.IsValid() && A.Screens.Num() == B.Screens.Num() && A.MainReference.Equals(B.MainReference) &&
		!A.Screens.ContainsByPredicate([&B](const FHUICRSyncScreenPayload& Entry)
		{
			return !B.Screens.ContainsByPredicate([&Entry](const FHUICRSyncScreenPayload& Other)
			{
				return Entry.ScreenID == Other.ScreenID && Entry.Transform.Equals(Other.Transform);
			});
		});
}
}

void UHUICRSyncSubsystem::RegisterCalibrationManager(AHUICRSyncPCManagerBase* Manager)
{
	CalibrationPC = Manager;
	bManagedCalibration = true;
	CommittedCalibration = {};
	CalibrationScreenIds.Reset();
	for (const FHUICRSyncPCScreenBinding& Screen : Manager->PCScreenBindings)
	{
		CalibrationScreenIds.AddUnique(Screen.ScreenID);
	}
	CalibrationScreenIds.Sort();
	Manager->LoadCommittedCalibration(CommittedCalibration, bMotionParallaxState);
	ScreenLayoutVersion = CommittedCalibration.Version;
	PublishCalibrationStatus();
}

void UHUICRSyncSubsystem::RegisterCalibrationManager(AHUICRSyncHMDManagerBase* Manager)
{
	CalibrationHMD = Manager;
}

bool UHUICRSyncSubsystem::CanStartLocalCalibration() const
{
	if (Role == EHUICRSyncRole::PC || (UsesManagedCalibration() && !bCanStartCalibration))
	{
		return false;
	}
	const auto* SpaceSetup = UGameInstance::GetSubsystem<UHUICRSyncSpaceSetupSubsystem>(GetGameInstance());
	return !SpaceSetup || SpaceSetup->IsCalibrationScreenReady();
}

bool UHUICRSyncSubsystem::RequestCalibrationTurn()
{
	using namespace HUICRSyncCalibrationProtocol;
	return Role == EHUICRSyncRole::HMD && bCanStartCalibration && CanStartLocalCalibration() &&
		SendReliableCommand(Join, {Integer(++CalibrationRequestSequence)});
}

bool UHUICRSyncSubsystem::CancelCalibrationTurn()
{
	using namespace HUICRSyncCalibrationProtocol;
	return Role == EHUICRSyncRole::HMD && bIsLocalHMDCalibrating &&
		SendReliableCommand(Cancel, {Integer(++CalibrationRequestSequence), Integer(CalibrationRound)});
}

bool UHUICRSyncSubsystem::RequestCalibrationPreview(int32 ScreenID)
{
	using namespace HUICRSyncCalibrationProtocol;
	return bCanEditPCScreenLayout && !bWaitingForCalibrationResult &&
		SendReliableCommand(Select, {Integer(CalibrationRound), Integer(ScreenID), Integer(++CalibrationPreviewSequence)});
}

bool UHUICRSyncSubsystem::SubmitCalibration(const FHUICRSyncCalibrationLayout& Measurements)
{
	using namespace HUICRSyncCalibrationProtocol;
	if (!bIsLocalHMDCalibrating || bWaitingForCalibrationResult || Measurements.Screens.Num() > 48)
	{
		return false;
	}
	const FString Encoded = EncodeLayout(Measurements);
	if (Encoded.Len() + 256 > MaxPacketBytes ||
		!SendReliableCommand(Submit, {Integer(CalibrationRound), String(Encoded)}))
	{
		SetCalibrationMessage(TEXT("Could not submit calibration. Try again."));
		return false;
	}
	bWaitingForCalibrationResult = true;
	ClientSubmitDeadline = FPlatformTime::Seconds() + 5.0;
	SetCalibrationMessage(TEXT("Applying screen layout..."));
	return true;
}

void UHUICRSyncSubsystem::SetCalibrationMessage(const FString& Message)
{
	CalibrationMessage = Message;
	OnCalibrationUIStateChanged.Broadcast();
}

bool UHUICRSyncSubsystem::HasAppliedCalibration() const
{
	return LocalAppliedLayoutId.IsValid();
}

void UHUICRSyncSubsystem::RestoreLocalCalibrationId(const FGuid& LayoutId)
{
	using namespace HUICRSyncCalibrationProtocol;
	LocalAppliedLayoutId = LayoutId;
	if (bManagedCalibration && bConnectionState && LayoutId.IsValid())
	{
		FHUICRSyncCalibrationLayout Restored;
		Restored.Id = LayoutId;
		Restored.MainReference = HMDToPCTransform;
		SendReliableCommand(Restore, {String(EncodeLayout(Restored))});
	}
}

FHUICRSyncPeerRuntimeState* UHUICRSyncSubsystem::FindCalibrationParticipant(int32 ParticipantId)
{
	for (auto& Pair : Peers)
	{
		if (Pair.Value.Info.bConnected && Pair.Value.Info.ParticipantId == ParticipantId)
		{
			return &Pair.Value;
		}
	}
	return nullptr;
}

bool UHUICRSyncSubsystem::HandleCalibrationCommand(FHUICRSyncPeerRuntimeState& Peer, const FHUICRSyncCommandPayload& Command)
{
	using namespace HUICRSyncCalibrationProtocol;
	const FString& Address = Command.Address;
	if (!Address.StartsWith(Prefix, ESearchCase::CaseSensitive))
	{
		return false;
	}
	const auto& Args = Command.Args;
	auto IsInt = [&Args](int32 Index)
	{
		return Args.IsValidIndex(Index) && Args[Index].Type == EHUICRSyncCommandArgType::Int32;
	};
	auto IsString = [&Args](int32 Index)
	{
		return Args.IsValidIndex(Index) && Args[Index].Type == EHUICRSyncCommandArgType::String;
	};
	if (!bManagedCalibration)
	{
		return true;
	}
	if (Role == EHUICRSyncRole::HMD)
	{
		if (!bIsLocalHMDCalibrating || !IsInt(0) || Args[0].IntValue != CalibrationRound)
		{
			return true;
		}
		if (Address == CalibrationError && IsString(1))
		{
			bWaitingForCalibrationResult = false;
			SetCalibrationMessage(Args[1].StringValue);
		}
		else if (Address == CalibrationResult && IsString(1) && CalibrationHMD.IsValid())
		{
			FHUICRSyncCalibrationLayout Layout;
			if (DecodeLayout(Args[1].StringValue, Layout) && Layout.Id.IsValid() &&
				(PendingLocalLayoutId == Layout.Id || CalibrationHMD->ApplyCalibrationLayout(Layout, bCanEditPCScreenLayout)))
			{
				PendingLocalLayoutId = Layout.Id;
				SendReliableCommand(Applied, {Integer(CalibrationRound), String(Layout.Id.ToString())});
				ClientSubmitDeadline = FPlatformTime::Seconds() + 5.0;
			}
			else
			{
				SetCalibrationMessage(TEXT("Could not apply all screens. Check the main screen and try again."));
			}
		}
		return true;
	}
	if (!CalibrationPC.IsValid())
	{
		return true;
	}

	const int32 ParticipantId = Peer.Info.ParticipantId;
	FCalibrationTurn* Turn = CalibrationTurns.Find(ParticipantId);
	if (Address == Restore && IsString(0))
	{
		FHUICRSyncCalibrationLayout Restored;
		if (DecodeLayout(Args[0].StringValue, Restored) && Restored.Id.IsValid() && Restored.Id == CommittedCalibration.Id)
		{
			Peer.AppliedLayout = Restored.Id;
			Peer.HMDToPC = Restored.MainReference;
			Peer.bHasCalibrated = true;
			if (ParticipantId == HostParticipantId && ActiveCalibrationCount == 0)
			{
				HMDToPCTransform = Peer.HMDToPC;
				PCToHMDTransform = HMDToPCTransform.Inverse();
				bStartGameAfterCalibration = true;
				BeginManagedGameplay();
			}
			PublishCalibrationStatus();
		}
		return true;
	}
	if (Address == Join || Address == Cancel)
	{
		if (!IsInt(0) || Args[0].IntValue <= Peer.LastCalibrationRequest)
		{
			return true;
		}
		Peer.LastCalibrationRequest = Args[0].IntValue;
		if (Address == Join && !Turn)
		{
			TryGrantCalibrationTurn(ParticipantId);
		}
		else if (Address == Cancel && Turn && IsInt(1) && Args[1].IntValue == Turn->Round)
		{
			EndCalibrationTurn(ParticipantId, false);
			return true;
		}
		PublishCalibrationStatus();
		return true;
	}
	if (!Turn || !IsInt(0) || Args[0].IntValue != Turn->Round)
	{
		return true;
	}
	const bool bHostTurn = Turn->bCanEditLayout && ParticipantId == HostParticipantId;
	if (Address == Select && bHostTurn && IsInt(1) && IsInt(2) &&
		Args[2].IntValue > Turn->LastPreviewSequence && !Turn->SubmittedLayout.Id.IsValid())
	{
		Turn->LastPreviewSequence = Args[2].IntValue;
		if (CalibrationPC->SelectPCScreen(Args[1].IntValue))
		{
			UE_LOG(LogTemp, Log, TEXT("HUICRSync calibration turn %d selected PC screen %d"), Turn->Round, Args[1].IntValue);
			OnSelectedScreenChanged.Broadcast(Args[1].IntValue);
		}
	}
	else if (Address == Submit && IsString(1))
	{
		const FString Recipient = Peer.Info.PeerId;
		const int32 Round = Turn->Round;
		if (!Turn->SubmittedLayout.Id.IsValid())
		{
			FHUICRSyncCalibrationLayout Measurements;
			if (!DecodeLayout(Args[1].StringValue, Measurements))
			{
				return true;
			}
			if (bHostTurn)
			{
				FHUICRSyncCalibrationLayout CandidateLayout;
				if (!CalibrationPC->PreviewHostLayout(Measurements, CommittedCalibration, CandidateLayout))
				{
					SendReliableCommandToPeer(Recipient, CalibrationError, {Integer(Round), String(TEXT("The PC could not apply these screens."))});
					return true;
				}
				Turn = CalibrationTurns.Find(ParticipantId);
				if (!Turn || Turn->Round != Round)
				{
					if (CalibrationPC.IsValid())
					{
						CalibrationPC->RestoreCalibrationLayout(CommittedCalibration);
					}
					return true;
				}
				const bool bSameLayout = IsSameScreenLayout(CommittedCalibration, CandidateLayout);
				CandidateLayout.Id = bSameLayout ? CommittedCalibration.Id : FGuid::NewGuid();
				CandidateLayout.Version = CommittedCalibration.Version + (bSameLayout ? 0 : 1);
				Turn->SubmittedLayout = MoveTemp(CandidateLayout);
			}
			else
			{
				// Keep the layout chosen on the first Stop request, including across retries.
				Turn->SubmittedLayout = CommittedCalibration;
			}
			Turn->HMDToPC = Measurements.GetMainReferenceForTurn(bHostTurn).Inverse() *
				Turn->SubmittedLayout.GetMainReferenceForTurn(bHostTurn);
		}
		Turn->ReplyDeadline = FPlatformTime::Seconds() + 12.0;
		const FString Encoded = EncodeLayout(Turn->SubmittedLayout);
		if (Encoded.Len() + 256 > MaxPacketBytes ||
			!SendReliableCommandToPeer(Recipient, CalibrationResult, {Integer(Round), String(Encoded)}))
		{
			EndCalibrationTurn(ParticipantId, false);
		}
	}
	else if (Address == Applied && IsString(1) && Turn->SubmittedLayout.Id.IsValid() &&
		Args[1].StringValue == Turn->SubmittedLayout.Id.ToString())
	{
		if (bHostTurn)
		{
			if (!CalibrationPC->SaveCommittedCalibration(Turn->SubmittedLayout, bMotionParallaxState))
			{
				SendReliableCommandToPeer(Peer.Info.PeerId, CalibrationError, {Integer(Turn->Round), String(TEXT("PC could not save the layout."))});
				EndCalibrationTurn(ParticipantId, false);
				return true;
			}
			CommittedCalibration = Turn->SubmittedLayout;
			CommittedCalibration.EditedScreenIds.Reset();
			ScreenLayoutVersion = CommittedCalibration.Version;
			bPendingHostCalibrationReady = true;
		}
		Peer.AppliedLayout = Turn->SubmittedLayout.Id;
		Peer.HMDToPC = Turn->HMDToPC;
		Peer.bHasCalibrated = true;
		Peer.LastCompletedCalibrationRound = Turn->Round;
		UE_LOG(LogTemp, Log, TEXT("HUICRSync calibration turn %d completed by participant %d using layout %d (%d screens)"),
			Turn->Round, ParticipantId, Turn->SubmittedLayout.Version, Turn->SubmittedLayout.Screens.Num());
		if (ParticipantId == HostParticipantId)
		{
			HMDToPCTransform = Peer.HMDToPC;
			PCToHMDTransform = HMDToPCTransform.Inverse();
		}
		bStartGameAfterCalibration = true;
		EndCalibrationTurn(ParticipantId, true);
	}
	return true;
}

void UHUICRSyncSubsystem::TryGrantCalibrationTurn(int32 ParticipantId)
{
	const bool bHostTurn = ParticipantId == HostParticipantId;
	if (!CalibrationPC.IsValid() || CalibrationTurns.Contains(ParticipantId) ||
		bHostCalibrationActive || (!bHostTurn && !CommittedCalibration.Id.IsValid()))
	{
		return;
	}
	const bool bFirstCalibrator = CalibrationTurns.IsEmpty();
	FCalibrationTurn& Turn = CalibrationTurns.Add(ParticipantId);
	const int32 Round = ++CalibrationRound;
	Turn.Round = Round;
	Turn.bCanEditLayout = bHostTurn;
	if (FHUICRSyncPeerRuntimeState* Peer = FindCalibrationParticipant(ParticipantId))
	{
		RevokeParticipantFields(ParticipantId);
		DiscardManagedSceneTransfer(*Peer);
	}
	RefreshActiveCalibrations();
	const bool bWasStopped = bGameStopped;
	bGameStopped = true;
	SetCalibrationPaused(true);
	if (bFirstCalibrator)
	{
		bCalibrationState = true;
		CalibrationPC->SelectPCScreen(0);
		OnCalibrationStateChanged.Broadcast(true);
		OnSelectedScreenChanged.Broadcast(0);
	}
	NotifyGameplayPauseIfChanged(bWasStopped);
	UE_LOG(LogTemp, Log, TEXT("HUICRSync calibration turn %d granted to participant %d (layout edit=%d)"),
		Round, ParticipantId, bHostTurn);
}

void UHUICRSyncSubsystem::RefreshActiveCalibrations()
{
	ActiveCalibrationCount = CalibrationTurns.Num();
	bHostCalibrationActive = false;
	ActiveCalibratorId = 0;
	for (int32 ParticipantId : ConnectedParticipantIds)
	{
		if (const FCalibrationTurn* Turn = CalibrationTurns.Find(ParticipantId))
		{
			if (ActiveCalibratorId == 0)
			{
				ActiveCalibratorId = ParticipantId;
			}
			bHostCalibrationActive |= Turn->bCanEditLayout;
		}
	}
}

void UHUICRSyncSubsystem::EndCalibrationTurn(int32 ParticipantId, bool bCommitted)
{
	FCalibrationTurn Finished;
	if (!CalibrationTurns.RemoveAndCopyValue(ParticipantId, Finished))
	{
		return;
	}
	if (Finished.bCanEditLayout && !bCommitted && Finished.SubmittedLayout.Id.IsValid() && CalibrationPC.IsValid())
	{
		CalibrationPC->RestoreCalibrationLayout(CommittedCalibration);
	}
	if (FHUICRSyncPeerRuntimeState* Peer = FindCalibrationParticipant(ParticipantId))
	{
		if (Peer->bHasCalibrated && bManagedScenePrepared)
		{
			StartManagedSceneForPeer(*Peer);
		}
	}
	RefreshActiveCalibrations();
	if (ActiveCalibrationCount == 0)
	{
		bCalibrationState = false;
		if (CalibrationPC.IsValid())
		{
			CalibrationPC->RestoreFixedGameViewpoint();
		}
		OnCalibrationStateChanged.Broadcast(false);
		BeginManagedGameplay();
	}
	else if (Finished.bCanEditLayout && CalibrationPC.IsValid())
	{
		CalibrationPC->SelectPCScreen(0);
		OnSelectedScreenChanged.Broadcast(0);
	}
	PublishCalibrationStatus();
}

void UHUICRSyncSubsystem::UpdateCalibrationParticipants()
{
	if (Role != EHUICRSyncRole::PC || !bManagedCalibration)
	{
		return;
	}
	if (CalibrationKnownHost != HostParticipantId)
	{
		CalibrationKnownHost = HostParticipantId;
		if (CalibrationPC.IsValid())
		{
			CalibrationPC->ResetGameViewpointTracking();
			if (CalibrationTurns.IsEmpty())
			{
				CalibrationPC->RestoreFixedGameViewpoint();
			}
		}
		if (const FHUICRSyncPeerRuntimeState* Host = FindCalibrationParticipant(HostParticipantId))
		{
			if (Host->bHasCalibrated)
			{
				HMDToPCTransform = Host->HMDToPC;
				PCToHMDTransform = HMDToPCTransform.Inverse();
			}
		}
	}
	TArray<int32> DepartedCalibrators;
	for (const auto& Pair : CalibrationTurns)
	{
		if (!ConnectedParticipantIds.Contains(Pair.Key))
		{
			DepartedCalibrators.Add(Pair.Key);
		}
	}
	for (int32 ParticipantId : DepartedCalibrators)
	{
		EndCalibrationTurn(ParticipantId, false);
	}
	BeginManagedGameplay();
	PublishCalibrationStatus();
}

void UHUICRSyncSubsystem::PublishCalibrationStatus()
{
	++CalibrationRevision;
	for (const auto& Pair : Peers)
	{
		if (Pair.Value.Info.bConnected)
		{
			SendPresence(Pair.Value, false);
		}
	}
	OnCalibrationUIStateChanged.Broadcast();
}

void UHUICRSyncSubsystem::WriteCalibrationStatus(FArchive& Writer, int32 RecipientId)
{
	bool bManaged = bManagedCalibration;
	Writer << bManaged;
	if (!bManaged)
	{
		return;
	}
	const FHUICRSyncPeerRuntimeState* Peer = FindCalibrationParticipant(RecipientId);
	const FCalibrationTurn* Turn = CalibrationTurns.Find(RecipientId);
	int32 LocalRound = Turn ? Turn->Round : 0;
	bool bCalibrated = Peer && Peer->bHasCalibrated;
	int32 SceneGeneration = Peer ? Peer->SceneGeneration : 0;
	int32 ReadySceneGeneration = Peer ? Peer->ReadySceneGeneration : 0;
	bool bLocalCanEdit = Turn && Turn->bCanEditLayout && RecipientId == HostParticipantId;
	bool bRunning = !bGameStopped;
	int32 CompletedRound = Peer ? Peer->LastCompletedCalibrationRound : 0;
	Writer << CalibrationRevision << LocalRound << ActiveCalibratorId << ActiveCalibrationCount << ScreenLayoutVersion
		<< CommittedCalibration.Id << CommittedCalibration.MainReference << bHostCalibrationActive << bLocalCanEdit << bMotionParallaxState << bCalibrationPaused
		<< bRunning << bCalibrated << SceneGeneration << ReadySceneGeneration << CompletedRound;
	int32 ScreenCount = FMath::Min(CalibrationScreenIds.Num(), 48);
	Writer << ScreenCount;
	for (int32 Index = 0; Index < ScreenCount; ++Index)
	{
		int32 ScreenId = CalibrationScreenIds[Index];
		const auto* SavedScreen = CommittedCalibration.Screens.FindByPredicate(
			[ScreenId](const FHUICRSyncScreenPayload& Screen) { return Screen.ScreenID == ScreenId; });
		uint8 bControlsPC = SavedScreen && SavedScreen->bRemoteControlsLocalScreen;
		Writer << ScreenId << bControlsPC;
	}
}

void UHUICRSyncSubsystem::ReadCalibrationStatus(FArchive& Reader)
{
	bool bManaged = false;
	Reader << bManaged;
	if (Reader.IsError() || !bManaged || !bConnectionState)
	{
		return;
	}
	int32 Revision = 0, Round = 0, Active = 0, Count = 0, Version = 0, CompletedRound = 0;
	FGuid LayoutId;
	FTransform PCMainReference;
	bool bHostActive = false, bLocalCanEdit = false, bParallax = false, bPaused = false, bRunning = false, bCalibrated = false;
	int32 SceneGeneration = 0, ReadySceneGeneration = 0;
	Reader << Revision << Round << Active << Count << Version << LayoutId << PCMainReference << bHostActive << bLocalCanEdit << bParallax << bPaused << bRunning
		<< bCalibrated << SceneGeneration << ReadySceneGeneration << CompletedRound;
	int32 ScreenCount = 0;
	Reader << ScreenCount;
	if (Reader.IsError() || Revision <= CalibrationRevision || ScreenCount < 0 || ScreenCount > 48 ||
		Reader.TotalSize() - Reader.Tell() != ScreenCount * HUICRSyncCalibrationProtocol::ScreenStatusBytes)
	{
		return;
	}
	CalibrationScreenIds.SetNumUninitialized(ScreenCount);
	TMap<int32, bool> SavedScreenControls;
	for (int32& ScreenId : CalibrationScreenIds)
	{
		uint8 bControlsPC = 0;
		Reader << ScreenId << bControlsPC;
		if (bLocalCanEdit && LayoutId.IsValid())
		{
			SavedScreenControls.Add(ScreenId, bControlsPC != 0);
		}
	}
	const bool bFirstStatus = !bManagedCalibration;
	const bool bOfflineMenuWasOpen = bFirstStatus && bCalibrationState;
	const bool bWasCalibrating = bIsLocalHMDCalibrating;
	const bool bWasStopped = bGameStopped;
	const bool bParallaxChanged = bMotionParallaxState != bParallax;
	const int32 PreviousRound = CalibrationRound;
	bManagedCalibration = true;
	CalibrationRevision = Revision;
	CalibrationRound = Round;
	ActiveCalibratorId = Active;
	ActiveCalibrationCount = Count;
	bHostCalibrationActive = bHostActive;
	ScreenLayoutVersion = Version;
	bIsLocalHMDCalibrating = Round != 0;
	ExpectedSceneGeneration = SceneGeneration;
	bCanEditPCScreenLayout = bIsLocalHMDCalibrating && bLocalCanEdit;
	bCanEditGameMotionParallax = bIsLocalHMDHost;
	bCanStartCalibration = !bIsLocalHMDCalibrating &&
		!bHostActive && (bIsLocalHMDHost || LayoutId.IsValid());
	bLayoutUpdatedSinceMyCalibration = LocalAppliedLayoutId.IsValid() && LayoutId.IsValid() && LayoutId != LocalAppliedLayoutId;
	if (!bIsLocalHMDCalibrating && LayoutId.IsValid() && LayoutId == LocalAppliedLayoutId && CalibrationHMD.IsValid())
	{
		CalibrationHMD->RestoreGuestActorMapping(PCMainReference);
	}
	bMotionParallaxState = bParallax;
	bLocalSceneReady = !bIsLocalHMDCalibrating && ReadySceneGeneration > 0 && ReadySceneGeneration == LocalSceneGeneration;
	bGameStopped = bPaused || !bRunning || !bCalibrated || !bLocalSceneReady;
	SetCalibrationPaused(bPaused || (bCalibrated && !bLocalSceneReady));
	if (PreviousRound != Round || bOfflineMenuWasOpen)
	{
		bWaitingForCalibrationResult = false;
		CalibrationMessage.Reset();
		if (bIsLocalHMDCalibrating)
		{
			ClearLocalSceneMirrors();
		}
		if (CalibrationHMD.IsValid())
		{
			if (bWasCalibrating)
			{
				const bool bAccepted = PreviousRound == CompletedRound && PendingLocalLayoutId.IsValid();
				CalibrationHMD->EndLocalCalibrationTurn(bAccepted);
				if (bAccepted)
				{
					LocalAppliedLayoutId = PendingLocalLayoutId;
					bLayoutUpdatedSinceMyCalibration = LayoutId.IsValid() && LocalAppliedLayoutId != LayoutId;
				}
				PendingLocalLayoutId.Invalidate();
			}
			if (bIsLocalHMDCalibrating)
			{
				CalibrationHMD->BeginLocalCalibrationTurn(SavedScreenControls);
			}
		}
		bCalibrationState = bIsLocalHMDCalibrating;
		OnCalibrationStateChanged.Broadcast(bCalibrationState);
	}
	if (bFirstStatus)
	{
		if (LocalAppliedLayoutId.IsValid())
		{
			RestoreLocalCalibrationId(LocalAppliedLayoutId);
		}
		if (bOfflineMenuWasOpen && bCanStartCalibration)
		{
			RequestCalibrationTurn();
		}
	}
	if (bParallaxChanged)
	{
		OnMotionParallaxStateChanged.Broadcast(bParallax);
	}
	NotifyGameplayPauseIfChanged(bWasStopped);
	if (bLocalSceneReady && !bGameStopped && ReadySceneGeneration != NotifiedLocalSceneGeneration)
	{
		NotifiedLocalSceneGeneration = ReadySceneGeneration;
		OnSyncSceneReady.Broadcast();
	}
	OnCalibrationUIStateChanged.Broadcast();
}

void UHUICRSyncSubsystem::TickCalibration()
{
	const double Now = FPlatformTime::Seconds();
	if (Role == EHUICRSyncRole::PC)
	{
		TArray<int32> ExpiredCalibrators;
		for (const auto& Pair : CalibrationTurns)
		{
			if (Pair.Value.ReplyDeadline > 0 && Now > Pair.Value.ReplyDeadline)
			{
				ExpiredCalibrators.Add(Pair.Key);
			}
		}
		for (int32 ParticipantId : ExpiredCalibrators)
		{
			EndCalibrationTurn(ParticipantId, false);
		}
	}
	if (Role == EHUICRSyncRole::HMD && bWaitingForCalibrationResult && Now > ClientSubmitDeadline)
	{
		bWaitingForCalibrationResult = false;
		SetCalibrationMessage(TEXT("No calibration confirmation. Try Stop Calibration again."));
	}
}

void UHUICRSyncSubsystem::ResetCalibrationSession()
{
	const bool bWasStopped = bGameStopped;
	bGameStopped = true;
	if (bIsLocalHMDCalibrating && CalibrationHMD.IsValid())
	{
		CalibrationHMD->EndLocalCalibrationTurn(false);
	}
	if (CalibrationPC.IsValid())
	{
		for (const auto& Pair : CalibrationTurns)
		{
			if (Pair.Value.bCanEditLayout && Pair.Value.SubmittedLayout.Id.IsValid())
			{
				CalibrationPC->RestoreCalibrationLayout(CommittedCalibration);
				break;
			}
		}
	}
	if (bCalibrationState && bManagedCalibration)
	{
		bCalibrationState = false;
		OnCalibrationStateChanged.Broadcast(false);
	}
	SetCalibrationPaused(false);
	bManagedCalibration = Role == EHUICRSyncRole::PC && CalibrationPC.IsValid();
	bIsLocalHMDCalibrating = bCanEditPCScreenLayout = bCanEditGameMotionParallax = bCanStartCalibration = false;
	bHostCalibrationActive = bWaitingForCalibrationResult = bLayoutUpdatedSinceMyCalibration = false;
	bAwaitingFirstSpawn = bStartGameAfterCalibration = bHasStartedGame = false;
	bPendingHostCalibrationReady = false;
	bInitialCalibrationSceneReady = false;
	ResetManagedSceneConnection();
	CalibrationKnownHost = 0;
	ActiveCalibratorId = ActiveCalibrationCount = CalibrationRevision = CalibrationRound = 0;
	CalibrationRequestSequence = CalibrationPreviewSequence = 0;
	CalibrationTurns.Reset();
	PendingLocalLayoutId.Invalidate();
	CalibrationMessage.Reset();
	NotifyGameplayPauseIfChanged(bWasStopped);
	OnCalibrationUIStateChanged.Broadcast();
}

void UHUICRSyncSubsystem::SetCalibrationPaused(bool bPaused)
{
	if (bPaused == bCalibrationPaused)
	{
		return;
	}
	bCalibrationPaused = bPaused;
	if (bPaused)
	{
		RegisteredStates.Reset();
		ScreenRegisteredStates.Reset();
		for (const auto& Pair : SpawnedSyncActors)
		{
			AHUICRSyncActor* Actor = Pair.Value.Get();
			if (IsValid(Actor))
			{
				PauseManagedActor(*Actor);
			}
		}
	}
	else
	{
		for (const FPausedPhysics& State : PausedPhysics)
		{
			if (UPrimitiveComponent* Component = State.Component.Get())
			{
				Component->SetSimulatePhysics(true);
				Component->SetPhysicsLinearVelocity(State.LinearVelocity);
				Component->SetPhysicsAngularVelocityInRadians(State.AngularVelocity);
				if (!State.bWasAwake)
				{
					Component->PutRigidBodyToSleep();
				}
			}
		}
		for (const TWeakObjectPtr<AHUICRSyncActor>& Actor : PausedActorTicks)
		{
			if (Actor.IsValid())
			{
				Actor->SetActorTickEnabled(true);
			}
		}
		PausedPhysics.Reset();
		PausedActorTicks.Reset();
	}
}

void UHUICRSyncSubsystem::PauseManagedActor(AHUICRSyncActor& Actor)
{
	if (Actor.IsActorTickEnabled())
	{
		PausedActorTicks.Add(&Actor);
		Actor.SetActorTickEnabled(false);
	}
	TInlineComponentArray<UPrimitiveComponent*> Components(&Actor);
	for (UPrimitiveComponent* Component : Components)
	{
		if (Component->IsSimulatingPhysics())
		{
			PausedPhysics.Add({Component, Component->GetPhysicsLinearVelocity(),
				Component->GetPhysicsAngularVelocityInRadians(), Component->IsAnyRigidBodyAwake()});
			Component->SetSimulatePhysics(false);
		}
	}
}

void UHUICRSyncSubsystem::NotifyGameplayPauseIfChanged(bool bWasStopped)
{
	if (bWasStopped != bGameStopped)
	{
		OnGameplayPauseStateChanged.Broadcast();
	}
}

void UHUICRSyncSubsystem::BeginManagedGameplay()
{
	if (!CalibrationTurns.IsEmpty())
	{
		return;
	}
	if (!bStartGameAfterCalibration && !bHasStartedGame)
	{
		SetCalibrationPaused(false);
		return;
	}
	const FHUICRSyncPeerRuntimeState* Host = FindCalibrationParticipant(HostParticipantId);
	if (!Host || !Host->bHasCalibrated)
	{
		const bool bWasStopped = bGameStopped;
		bGameStopped = true;
		SetCalibrationPaused(false);
		NotifyGameplayPauseIfChanged(bWasStopped);
		return;
	}
	if (!bHasStartedGame && !bInitialCalibrationSceneReady && Host->ReadySceneGeneration == 0)
	{
		if (!bAwaitingFirstSpawn)
		{
			bAwaitingFirstSpawn = true;
			bPendingHostCalibrationReady = false;
			OnCalibrationReady.Broadcast();
			StartInitialSpawnSync();
		}
		return;
	}
	const bool bWasStopped = bGameStopped;
	const bool bFirstStart = !bHasStartedGame;
	if (bPendingHostCalibrationReady)
	{
		bPendingHostCalibrationReady = false;
		OnCalibrationReady.Broadcast();
	}
	bHasStartedGame = true;
	bGameStopped = false;
	SetCalibrationPaused(false);
	bStartGameAfterCalibration = false;
	NotifyGameplayPauseIfChanged(bWasStopped);
	if (bWasStopped || bFirstStart)
	{
		// Existing PC Blueprints use this event to re-enable movement after a calibration pause.
		UE_LOG(LogTemp, Log, TEXT("HUICRSync PC resumed gameplay with the scene ready (first start=%d)"), bFirstStart);
		OnSyncSceneReady.Broadcast();
		for (const auto& Pair : Peers)
		{
			if (Pair.Value.bHasCalibrated)
			{
				SendReliableCommandToPeer(Pair.Key, TEXT("/FromPC/GameStart"), {});
			}
		}
	}
}

void UHUICRSyncSubsystem::HandleManagedParallax(bool bEnabled)
{
	if (bMotionParallaxState == bEnabled || !CalibrationPC.IsValid())
	{
		return;
	}
	CalibrationPC->ApplyGameMotionParallax(bEnabled, CommittedCalibration);
	bMotionParallaxState = bEnabled;
	OnMotionParallaxStateChanged.Broadcast(bEnabled);
	PublishCalibrationStatus();
}
