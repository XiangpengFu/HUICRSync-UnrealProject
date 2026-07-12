#include "HUICRSyncManagerBase.h"

#include "Engine/GameInstance.h"
#include "HUICRSyncSubsystem.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

namespace
{
const TCHAR* ScreenInitialTransformAddressPrefix = TEXT("HUICRScreenInitialTransform=");
}

AHUICRSyncManagerBase::AHUICRSyncManagerBase()
{
	PrimaryActorTick.bCanEverTick = false;
}

bool AHUICRSyncManagerBase::InitializeManager()
{
	if (!SyncSubsystem)
	{
		if (UGameInstance* GameInstance = GetGameInstance())
		{
			SyncSubsystem = GameInstance->GetSubsystem<UHUICRSyncSubsystem>();
		}
	}

	if (!SyncSubsystem)
	{
		return false;
	}

	if (bConfigureSyncOnBeginPlay)
	{
		return SyncSubsystem->ConfigureSync(LocalRole, LocalSyncPort, RemoteSyncPort, RemoteIpAddress);
	}

	return true;
}

bool AHUICRSyncManagerBase::QueueCalibratorPayload(const FHUICRSyncCalibratorPayload& Payload)
{
	if (!EnsureSyncSubsystem())
	{
		return false;
	}

	TArray<uint8> PayloadBytes;
	FMemoryWriter Writer(PayloadBytes, true);
	FHUICRSyncCalibratorPayload MutablePayload = Payload;
	Writer << MutablePayload;

	FHUICRSyncNetEntry Entry;
	Entry.TypeCode = EHUICRSyncActorType::HUICalibrator;
	Entry.ActorID = Payload.ActorID;
	Entry.PayloadBytes = PayloadBytes;
	SyncSubsystem->QueueNetEntry(Entry);
	return !Writer.IsError();
}

bool AHUICRSyncManagerBase::QueueScreenPayload(const FHUICRSyncScreenPayload& Payload)
{
	if (!EnsureSyncSubsystem())
	{
		return false;
	}

	TArray<uint8> PayloadBytes;
	FMemoryWriter Writer(PayloadBytes, true);
	FHUICRSyncScreenPayload MutablePayload = Payload;
	Writer << MutablePayload;

	FHUICRSyncNetEntry Entry;
	Entry.TypeCode = EHUICRSyncActorType::HUIScreen;
	Entry.ActorID = Payload.ScreenID;
	Entry.Address = FString::Printf(TEXT("%s%s"), ScreenInitialTransformAddressPrefix, *Payload.InitialTransform.ToString());
	Entry.PayloadBytes = PayloadBytes;
	SyncSubsystem->QueueNetEntry(Entry);
	return !Writer.IsError();
}

bool AHUICRSyncManagerBase::SpawnSyncActor(UClass* SpawnClass, const FTransform& SpawnTransform, const TArray<uint8>& InitPayloadBytes)
{
	return EnsureSyncSubsystem() ? SyncSubsystem->SpawnSyncActor(SpawnClass, SpawnTransform, InitPayloadBytes) : false;
}

bool AHUICRSyncManagerBase::SpawnSyncActorSimple(UClass* SpawnClass, const FTransform& SpawnTransform)
{
	return EnsureSyncSubsystem() ? SyncSubsystem->SpawnSyncActorSimple(SpawnClass, SpawnTransform) : false;
}

bool AHUICRSyncManagerBase::DestroySyncActor(EHUICRSyncActorType TypeCode, int32 ActorID)
{
	return EnsureSyncSubsystem() ? SyncSubsystem->DestroySyncActor(TypeCode, ActorID) : false;
}

bool AHUICRSyncManagerBase::SendReliableCommand(const FString& Address, const TArray<FHUICRSyncCommandArg>& Args)
{
	return EnsureSyncSubsystem() ? SyncSubsystem->SendReliableCommand(Address, Args) : false;
}

bool AHUICRSyncManagerBase::HasSyncSubsystem() const
{
	return SyncSubsystem != nullptr;
}

void AHUICRSyncManagerBase::BeginPlay()
{
	Super::BeginPlay();

	InitializeManager();

	if (SyncSubsystem)
	{
		SyncSubsystem->OnNetDataReceived.AddDynamic(this, &AHUICRSyncManagerBase::OnSubsystemNetDataReceived);
	}
}

void AHUICRSyncManagerBase::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (SyncSubsystem)
	{
		SyncSubsystem->OnNetDataReceived.RemoveDynamic(this, &AHUICRSyncManagerBase::OnSubsystemNetDataReceived);
	}

	Super::EndPlay(EndPlayReason);
}

void AHUICRSyncManagerBase::HandleNetDataReceived()
{
	FHUICRSyncNetEntry Entry;
	FHUICRSyncCalibratorPayload CalibratorPayload;
	if (GetFirstPayload(EHUICRSyncActorType::HUICalibrator, Entry) && DecodeCalibratorEntry(Entry, CalibratorPayload))
	{
		OnCalibratorPayloadReceived.Broadcast(CalibratorPayload);
	}

	if (!SyncSubsystem)
	{
		return;
	}

	if (const FHUICRSyncNetInnerMapEntry* ScreenMap = SyncSubsystem->ReceivedDataMap.Find(EHUICRSyncActorType::HUIScreen))
	{
		for (const TPair<int32, FHUICRSyncNetEntry>& Pair : ScreenMap->InnerMap)
		{
			FHUICRSyncScreenPayload ScreenPayload;
			if (DecodeScreenEntry(Pair.Value, ScreenPayload))
			{
				OnScreenPayloadReceived.Broadcast(ScreenPayload);
			}
		}
	}
}

void AHUICRSyncManagerBase::OnSubsystemNetDataReceived()
{
	HandleNetDataReceived();
}

bool AHUICRSyncManagerBase::EnsureSyncSubsystem()
{
	if (SyncSubsystem)
	{
		return true;
	}

	if (UGameInstance* GameInstance = GetGameInstance())
	{
		SyncSubsystem = GameInstance->GetSubsystem<UHUICRSyncSubsystem>();
	}

	return SyncSubsystem != nullptr;
}

bool AHUICRSyncManagerBase::GetFirstPayload(EHUICRSyncActorType TypeCode, FHUICRSyncNetEntry& OutEntry) const
{
	if (!SyncSubsystem)
	{
		return false;
	}

	const FHUICRSyncNetInnerMapEntry* InnerMapEntry = SyncSubsystem->ReceivedDataMap.Find(TypeCode);
	if (!InnerMapEntry || InnerMapEntry->InnerMap.IsEmpty())
	{
		return false;
	}

	for (const TPair<int32, FHUICRSyncNetEntry>& Pair : InnerMapEntry->InnerMap)
	{
		OutEntry = Pair.Value;
		return true;
	}

	return false;
}

bool AHUICRSyncManagerBase::DecodeCalibratorEntry(const FHUICRSyncNetEntry& Entry, FHUICRSyncCalibratorPayload& OutPayload) const
{
	if (Entry.PayloadBytes.IsEmpty())
	{
		return false;
	}

	FMemoryReader Reader(Entry.PayloadBytes, true);
	Reader << OutPayload;
	return !Reader.IsError();
}

bool AHUICRSyncManagerBase::DecodeScreenEntry(const FHUICRSyncNetEntry& Entry, FHUICRSyncScreenPayload& OutPayload) const
{
	if (Entry.PayloadBytes.IsEmpty())
	{
		return false;
	}

	FMemoryReader Reader(Entry.PayloadBytes, true);
	Reader << OutPayload;
	OutPayload.InitialTransform = OutPayload.Transform;
	const FString Prefix(ScreenInitialTransformAddressPrefix);
	if (Entry.Address.StartsWith(Prefix))
	{
		FTransform ParsedInitialTransform;
		if (ParsedInitialTransform.InitFromString(Entry.Address.RightChop(Prefix.Len())))
		{
			OutPayload.InitialTransform = ParsedInitialTransform;
		}
	}
	return !Reader.IsError();
}

bool AHUICRSyncManagerBase::DecodeTransformEntry(const FHUICRSyncNetEntry& Entry, FTransform& OutTransform) const
{
	if (Entry.PayloadBytes.IsEmpty())
	{
		return false;
	}

	FHUICRSyncTransformPayload Payload;
	FMemoryReader Reader(Entry.PayloadBytes, true);
	Reader << Payload;
	OutTransform = Payload.Transform;
	return !Reader.IsError();
}
