#pragma once

#include "CoreMinimal.h"
#include "HUICRSyncFields.generated.h"

class FProperty;
struct FInstancedStruct;

UENUM(BlueprintType)
enum class EHUICRSyncFieldSource : uint8
{
	FollowMyPayload UMETA(DisplayName = "Follow My Payload"),
	UseSpecifiedValue UMETA(DisplayName = "Use Specified Value")
};

namespace HUICRSyncFields
{
	/** The editor and the runtime use the same eligibility rules for shared members. */
	HUICRSYNCRUNTIME_API bool IsSupportedField(const FProperty& Property);
	bool CanUseLegacyBytes(const UScriptStruct* StructType);
	bool Encode(const FProperty& Property, const FInstancedStruct& Payload, TArray<uint8>& OutBytes);
	bool Decode(FProperty& Property, const TArray<uint8>& Bytes, FInstancedStruct* Destination);
}
