#include "HUICRSyncPayloadLibrary.h"

#include "HUICRSyncFields.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/StructuredArchiveAdapters.h"

bool UHUICRSyncPayloadLibrary::WriteInstancedStruct(TArray<uint8>& OutBytes, UScriptStruct* StructType, const FInstancedStruct& InStruct)
{
	OutBytes.Reset();

	if (!StructType || !InStruct.IsValid() || InStruct.GetScriptStruct() != StructType)
	{
		return false;
	}

	FMemoryWriter Ar(OutBytes, true);
	FStructuredArchiveFromArchive StructuredArchive(Ar);
	FStructuredArchive::FSlot Root = StructuredArchive.GetSlot();

	const void* SourceMemory = InStruct.GetMemory();
	StructType->SerializeItem(Root, const_cast<void*>(SourceMemory), nullptr);
	return !Ar.IsError();
}

bool UHUICRSyncPayloadLibrary::ReadInstancedStruct(const TArray<uint8>& InBytes, UScriptStruct* StructType, FInstancedStruct& OutStruct)
{
	OutStruct.Reset();

	if (!StructType || InBytes.IsEmpty())
	{
		return false;
	}

	FMemoryReader Ar(InBytes, true);
	Ar.Seek(0);

	FStructuredArchiveFromArchive StructuredArchive(Ar);
	FStructuredArchive::FSlot Root = StructuredArchive.GetSlot();

	const int32 StructSize = StructType->GetStructureSize();
	void* TempMemory = FMemory::Malloc(StructSize);
	StructType->InitializeStruct(TempMemory);

	StructType->SerializeItem(Root, TempMemory, nullptr);

	if (!Ar.IsError())
	{
		OutStruct.InitializeAs(StructType);
		StructType->CopyScriptStruct(OutStruct.GetMutableMemory(), TempMemory);
	}

	StructType->DestroyStruct(TempMemory);
	FMemory::Free(TempMemory);

	if (Ar.IsError())
	{
		OutStruct.Reset();
		return false;
	}

	return true;
}

bool UHUICRSyncPayloadLibrary::WriteInitialValues(TArray<uint8>& Bytes, UScriptStruct* Schema, const FInstancedStruct& Payload)
{
	TArray<uint8> Encoded;
	if (!HUICRSyncFields::CanUseLegacyBytes(Schema) || !WriteInstancedStruct(Encoded, Schema, Payload))
	{
		return false;
	}
	Bytes = MoveTemp(Encoded);
	return true;
}

bool UHUICRSyncPayloadLibrary::ReadInitialValues(const TArray<uint8>& Bytes, UScriptStruct* Schema, FInstancedStruct& Payload)
{
	// Spawn bytes have no schema tag. Accept only a complete round trip with this endpoint's schema.
	TArray<uint8> RoundTrip;
	if (!Bytes.IsEmpty() && HUICRSyncFields::CanUseLegacyBytes(Schema) && ReadInstancedStruct(Bytes, Schema, Payload) &&
		WriteInstancedStruct(RoundTrip, Schema, Payload) && RoundTrip == Bytes)
	{
		return true;
	}
	Payload.Reset();
	return false;
}

void UHUICRSyncPayloadLibrary::WriteSpawnRequestPayload(TArray<uint8>& OutBytes, UClass* SpawnClass, const FTransform& SpawnTransform, bool bSpawnBackOnSender, const TArray<uint8>& InitPayloadBytes)
{
	OutBytes.Reset();
	if (!SpawnClass)
	{
		return;
	}

	FMemoryWriter Writer(OutBytes, true);
	FString ClassPath = SpawnClass ? SpawnClass->GetPathName() : FString();
	FTransform Transform = SpawnTransform;
	Writer << ClassPath;
	Writer << Transform;
	Writer << bSpawnBackOnSender;
	OutBytes.Append(InitPayloadBytes);
}

bool UHUICRSyncPayloadLibrary::ReadSpawnRequestPayload(const TArray<uint8>& InBytes, FString& OutClassPath, FTransform& OutTransform, bool& bOutSpawnBackOnSender, TArray<uint8>& OutInitPayloadBytes)
{
	OutInitPayloadBytes.Reset();

	if (InBytes.IsEmpty())
	{
		return false;
	}

	FMemoryReader Reader(InBytes, true);
	Reader << OutClassPath;
	Reader << OutTransform;
	Reader << bOutSpawnBackOnSender;

	if (Reader.IsError())
	{
		return false;
	}

	const int32 Offset = static_cast<int32>(Reader.Tell());
	if (Offset < InBytes.Num())
	{
		OutInitPayloadBytes.Append(InBytes.GetData() + Offset, InBytes.Num() - Offset);
	}

	return !Reader.IsError();
}

void UHUICRSyncPayloadLibrary::WriteSpawnMirrorPayload(TArray<uint8>& OutBytes, UClass* SpawnClass, int32 ActorID, const FTransform& SpawnTransform, const TArray<uint8>& InitPayloadBytes)
{
	OutBytes.Reset();
	if (!SpawnClass)
	{
		return;
	}

	FMemoryWriter Writer(OutBytes, true);
	FString ClassPath = SpawnClass ? SpawnClass->GetPathName() : FString();
	FTransform Transform = SpawnTransform;
	Writer << ClassPath;
	Writer << ActorID;
	Writer << Transform;
	OutBytes.Append(InitPayloadBytes);
}

bool UHUICRSyncPayloadLibrary::ReadSpawnMirrorPayload(const TArray<uint8>& InBytes, FString& OutClassPath, int32& OutActorID, FTransform& OutTransform, TArray<uint8>& OutInitPayloadBytes)
{
	OutInitPayloadBytes.Reset();

	if (InBytes.IsEmpty())
	{
		return false;
	}

	FMemoryReader Reader(InBytes, true);
	Reader << OutClassPath;
	Reader << OutActorID;
	Reader << OutTransform;

	if (Reader.IsError())
	{
		return false;
	}

	const int32 Offset = static_cast<int32>(Reader.Tell());
	if (Offset < InBytes.Num())
	{
		OutInitPayloadBytes.Append(InBytes.GetData() + Offset, InBytes.Num() - Offset);
	}

	return true;
}
