#pragma once

#include "HUICRSyncActor.h"
#include "HUICRSyncHMDPawn.h"
#include "HUICRSyncParticipantPawnFixture.generated.h"

UCLASS(Transient, NotBlueprintable, NotPlaceable)
class AHUICRSyncParticipantPawnFixture : public AHUICRSyncHMDPawn
{
	GENERATED_BODY()
public:
	AHUICRSyncParticipantPawnFixture();

	double HeadX = 0;
	int32 SampleNumber = 0;
	bool bUseLegacyBlueprintWrites = false;
	virtual void OnPrepareSyncFields_Implementation(const TArray<FName>& FieldsToPrepare) override
	{
		if (bUseLegacyBlueprintWrites) { return; }
		auto Sample = FInstancedStruct::Make<FHUICRSyncScreenPayload>();
		auto& Pose = Sample.GetMutable<FHUICRSyncScreenPayload>();
		Pose.ScreenID = SampleNumber;
		Pose.Transform = FTransform(FVector(HeadX, 0, 170));
		PrepareOutgoingPayloadInternal(Sample);
	}
};

UCLASS(Transient, NotBlueprintable, NotPlaceable)
class AHUICRSyncPCPawnFixture : public AHUICRSyncActor
{
	GENERATED_BODY()
public:
	AHUICRSyncPCPawnFixture()
	{
		TypeCode = EHUICRSyncActorType::HMDPawn;
		RemotePawnCounterpartClass = AHUICRSyncParticipantPawnFixture::StaticClass();
		SharedPayloadStruct = FHUICRSyncScreenPayload::StaticStruct();
	}

	virtual void OnSyncFieldsReceived_Implementation() override
	{
		TArray<FName> Mask;
		ReadReceivedPayloadInternal(LastSample, Mask);
	}
	bool Received(double X, int32 Number) const
	{
		const auto* Pose = LastSample.GetPtr<FHUICRSyncScreenPayload>();
		return Pose && Pose->Transform.GetLocation().Equals(FVector(X, 0, 170)) && Pose->ScreenID == Number;
	}
	FInstancedStruct LastSample;
};

inline AHUICRSyncParticipantPawnFixture::AHUICRSyncParticipantPawnFixture()
{
	SharedPayloadStruct = FHUICRSyncScreenPayload::StaticStruct();
	RemoteCounterpartClass = AHUICRSyncPCPawnFixture::StaticClass();
}

// Represents an unmigrated PC map with an ID-zero avatar and no shared schema.
UCLASS(Transient, NotBlueprintable, NotPlaceable)
class AHUICRSyncLegacyPCPawnFixture : public AHUICRSyncActor
{
	GENERATED_BODY()
public:
	AHUICRSyncLegacyPCPawnFixture() { TypeCode = EHUICRSyncActorType::HMDPawn; }
};
