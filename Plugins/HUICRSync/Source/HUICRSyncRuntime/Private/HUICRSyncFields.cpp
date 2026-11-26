#include "HUICRSyncFields.h"

#include "Containers/StringConv.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/StructuredArchiveAdapters.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/EnumProperty.h"
#include "UObject/PropertyTempVal.h"
#include "UObject/UnrealType.h"

namespace HUICRSyncFields
{
namespace
{
// All field values have to fit into the plugin's bounded scene operations.
constexpr int32 MaxEncodedFieldBytes = 64 * 1024;

bool IsMathField(const FProperty& Property)
{
	const FStructProperty* Member = CastField<FStructProperty>(&Property);
	if (!Member)
	{
		return false;
	}
	const UScriptStruct* Type = Member->Struct;
	return Type == TBaseStructure<FTransform>::Get() || Type == TBaseStructure<FVector>::Get() ||
		Type == TBaseStructure<FQuat>::Get() || Type == TBaseStructure<FRotator>::Get() ||
		Type == TBaseStructure<FVector2D>::Get() || Type == TBaseStructure<FLinearColor>::Get() ||
		Type == TBaseStructure<FColor>::Get();
}

bool IsValueType(const FProperty& Property)
{
	TArray<const FStructProperty*> Seen;
	constexpr EPropertyObjectReferenceType References =
		EPropertyObjectReferenceType::Strong | EPropertyObjectReferenceType::Weak | EPropertyObjectReferenceType::Soft;
	return !Property.IsA<FDelegateProperty>() && !Property.IsA<FMulticastDelegateProperty>() &&
		!Property.ContainsObjectReference(Seen, References);
}

bool HasBinaryFormat(const FProperty& Property)
{
	return Property.IsA<FBoolProperty>() || Property.IsA<FNumericProperty>() ||
		Property.IsA<FEnumProperty>() || IsMathField(Property);
}

void SerializeBinary(FArchive& Archive, const FProperty& Property, void* Value)
{
	if (const FBoolProperty* Bool = CastField<FBoolProperty>(&Property))
	{
		uint8 Bit = Bool->GetPropertyValue(Value) ? 1 : 0;
		Archive << Bit;
		if (Archive.IsLoading())
		{
			if (Bit > 1)
			{
				Archive.SetError();
				return;
			}
			Bool->SetPropertyValue(Value, Bit != 0);
		}
		return;
	}
	if (Property.IsA<FNumericProperty>() || Property.IsA<FEnumProperty>())
	{
		Archive.ByteOrderSerialize(Value, Property.GetElementSize());
		return;
	}
	// Core math structs use UE's native serializers and retain full transform precision.
	Archive.SetWantBinaryPropertySerialization(true);
	FStructuredArchiveFromArchive StructuredArchive(Archive);
	Property.SerializeItem(StructuredArchive.GetSlot(), Value);
}
}

bool IsSupportedField(const FProperty& Property)
{
	return Property.HasAnyPropertyFlags(CPF_BlueprintVisible) &&
		!Property.HasAnyPropertyFlags(CPF_Transient | CPF_EditorOnly | CPF_Deprecated) &&
		Property.ArrayDim == 1 && IsValueType(Property);
}

bool CanUseLegacyBytes(const UScriptStruct* StructType)
{
	if (!StructType)
	{
		return false;
	}
	for (TFieldIterator<FProperty> It(StructType); It; ++It)
	{
		if (!IsValueType(**It))
		{
			return false;
		}
	}
	return true;
}

bool FitsDatagram(FName Name, int32 NumBytes, int32 PacketLimit)
{
	// Make room for the session header, reflected property name and Base64 source command.
	const FString TextName = Name.ToString();
	const int32 Length = StringCast<UTF8CHAR>(*TextName).Length();
	return NumBytes >= 0 && (static_cast<int64>(NumBytes) + Length + 32) * 4 / 3 + 160 <= FMath::Min(PacketLimit, 65507);
}

bool Encode(const FProperty& Property, const FInstancedStruct& Payload, TArray<uint8>& OutBytes)
{
	OutBytes.Reset();
	if (!Payload.IsValid() || !Payload.GetScriptStruct()->IsChildOf(Property.GetOwnerStruct()))
	{
		return false;
	}
	const void* Value = Property.ContainerPtrToValuePtr<void>(Payload.GetMemory());
	if (HasBinaryFormat(Property))
	{
		FMemoryWriter Writer(OutBytes, true);
		SerializeBinary(Writer, Property, const_cast<void*>(Value));
		return !Writer.IsError() && !OutBytes.IsEmpty() && OutBytes.Num() <= MaxEncodedFieldBytes;
	}

	// Bounded property text also supports nested Blueprint value structs and containers.
	// Incoming array/map element counts are derived from actual text, never an unchecked wire count.
	FString Exported;
	Property.ExportTextItem_Direct(Exported, Value, nullptr, nullptr, PPF_Delimited);
	const FTCHARToUTF8 Utf8(*Exported);
	if (Utf8.Length() >= MaxEncodedFieldBytes)
	{
		return false;
	}
	OutBytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	OutBytes.Add(0); // Even an empty array or string has a valid, nonempty wire value.
	return true;
}

bool Decode(FProperty& Property, const TArray<uint8>& Bytes, FInstancedStruct* Destination)
{
	if (Bytes.IsEmpty() || Bytes.Num() > MaxEncodedFieldBytes ||
		(Destination && (!Destination->IsValid() || !Destination->GetScriptStruct()->IsChildOf(Property.GetOwnerStruct()))))
	{
		return false;
	}
	FPropertyTempVal Decoded(&Property);
	if (HasBinaryFormat(Property))
	{
		FMemoryReader Reader(Bytes, true);
		SerializeBinary(Reader, Property, Decoded.Get());
		if (Reader.IsError() || !Reader.AtEnd())
		{
			return false;
		}
	}
	else
	{
		if (Bytes.Find(uint8(0)) != Bytes.Num() - 1)
		{
			return false;
		}
		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()));
		const TCHAR* End = Property.ImportText_Direct(Converted.Get(), Decoded.Get(), nullptr, PPF_Delimited);
		if (!End || *End)
		{
			return false;
		}
	}
	if (Destination)
	{
		Property.CopyCompleteValue(Property.ContainerPtrToValuePtr<void>(Destination->GetMutableMemory()), Decoded.Get());
	}
	return true;
}
}
