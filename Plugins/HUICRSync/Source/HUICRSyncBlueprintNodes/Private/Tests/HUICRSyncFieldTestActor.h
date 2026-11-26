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

	// Exercise the same default assignment a Blueprint can make in its construction script.
	virtual void OnConstruction(const FTransform& Transform) override
	{
		Super::OnConstruction(Transform);
		SpawnTransformAtConstruction = Transform;
		InitialBytesAtConstruction = ActorInitPayloadData;
		FInstancedStruct Initial = FInstancedStruct::Make<FHUICRSyncScreenPayload>();
		auto& Value = Initial.GetMutable<FHUICRSyncScreenPayload>();
		Value.Transform = FTransform(FVector(10, 0, 0));
		Value.ScreenID = 1;
		bSetConstructionDefault = SetInitialPayloadInternal(Initial);
	}

	TArray<uint8> InitialBytesAtConstruction;
	FTransform SpawnTransformAtConstruction;
	bool bSetConstructionDefault = false;

	virtual void OnPrepareSyncFields_Implementation(const TArray<FName>& FieldsToPrepare) override
	{
		++PrepareEventCount;
		LastPrepareFields = FieldsToPrepare;
		if (PayloadAtPrepare.IsValid())
		{
			PrepareOutgoingPayloadInternal(PayloadAtPrepare);
		}
		if (PrepareAction)
		{
			PrepareAction();
		}
	}

	int32 PrepareEventCount = 0;
	TArray<FName> LastPrepareFields;
	FInstancedStruct PayloadAtPrepare;
	TFunction<void()> PrepareAction;

	virtual void OnSyncFieldsReceived_Implementation() override
	{
		++ReceiveEventCount;
		if (ReadReceivedPayloadInternal(LastEventPayload, LastEventFields) &&
			LastEventFields.Contains(GET_MEMBER_NAME_CHECKED(FHUICRSyncScreenPayload, ScreenID)))
		{
			AppliedColorIndex = LastEventPayload.Get<FHUICRSyncScreenPayload>().ScreenID;
		}
	}

	bool EventReadState(double X, int32 ColorIndex) const
	{
		const auto* Value = LastEventPayload.GetPtr<FHUICRSyncScreenPayload>();
		return Value && FMath::IsNearlyEqual(Value->Transform.GetLocation().X, X) && Value->ScreenID == ColorIndex;
	}

	int32 ReceiveEventCount = 0;
	int32 AppliedColorIndex = INDEX_NONE;
	FInstancedStruct LastEventPayload;
	TArray<FName> LastEventFields;
};
