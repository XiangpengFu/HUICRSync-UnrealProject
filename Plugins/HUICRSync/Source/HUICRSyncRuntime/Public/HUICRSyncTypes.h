#pragma once

#include "CoreMinimal.h"
#include "HUICRSyncTypes.generated.h"

UENUM(BlueprintType)
enum class EHUICRSyncRole : uint8
{
	None UMETA(DisplayName = "None"),
	PC UMETA(DisplayName = "PC"),
	HMD UMETA(DisplayName = "HMD")
};

UENUM(BlueprintType)
enum class EHUICRSyncActorType : uint8
{
	HMDPawn UMETA(DisplayName = "HMD Pawn"),
	HUICalibrator UMETA(DisplayName = "HUI Calibrator"),
	HUIScreen UMETA(DisplayName = "HUI Screen"),
	HUISpawnInGame UMETA(DisplayName = "HUI Spawn In Game"),
	HUIActor UMETA(DisplayName = "HUI Actor")
};

UENUM(BlueprintType)
enum class EHUICRSyncActorClassRole : uint8
{
	Shared UMETA(DisplayName = "Shared"),
	PC UMETA(DisplayName = "PC"),
	HMD UMETA(DisplayName = "HMD")
};

FORCEINLINE uint32 GetTypeHash(const EHUICRSyncActorType Type)
{
	return ::GetTypeHash(static_cast<uint8>(Type));
}

USTRUCT(BlueprintType)
struct HUICRSYNCRUNTIME_API FHUICRSyncPeerInfo
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	FString PeerId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	EHUICRSyncRole Role = EHUICRSyncRole::None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	FString IpAddress;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	int32 SyncPort = 4001;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	int32 UDPPort = 4002;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync")
	bool bConnected = false;

	// Assigned by the PC for this connection. Zero means no participant has been assigned yet.
	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Participants")
	int32 ParticipantId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Participants")
	bool bHasHeadPose = false;

	// Uses this participant's current HMD-to-PC transform. Spatial alignment requires calibration.
	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync|Participants")
	FTransform HeadPoseInPCSpace = FTransform::Identity;

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync")
	double LastReceiveTime = 0.0;
};

USTRUCT(BlueprintType)
struct HUICRSYNCRUNTIME_API FHUICRSyncCalibratorPayload
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	int32 ActorID = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	FTransform Transform;

	friend FArchive& operator<<(FArchive& Ar, FHUICRSyncCalibratorPayload& Data)
	{
		Ar << Data.ActorID;
		Ar << Data.Transform;
		return Ar;
	}
};

USTRUCT(BlueprintType)
struct HUICRSYNCRUNTIME_API FHUICRSyncScreenPayload
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	int32 ScreenID = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	FTransform Transform;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	FTransform InitialTransform;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	bool bRemoteControlsLocalScreen = false;

	friend FArchive& operator<<(FArchive& Ar, FHUICRSyncScreenPayload& Data)
	{
		Ar << Data.ScreenID;
		Ar << Data.Transform;
		Ar << Data.bRemoteControlsLocalScreen;
		return Ar;
	}
};

// A complete layout expressed in the PC's coordinates.
struct FHUICRSyncCalibrationLayout
{
	int32 Version = 0;
	FGuid Id;
	FTransform MainReference = FTransform::Identity;
	TArray<FHUICRSyncScreenPayload> Screens;
	// Per-round pose edits. The saved layout only contains the accepted poses and control preferences.
	TSet<int32> EditedScreenIds;

	FTransform GetMainReferenceForTurn(bool bHostTurn) const
	{
		FTransform Reference = MainReference;
		if (!bHostTurn)
		{
			Reference.SetScale3D(FVector::OneVector);
		}
		return Reference;
	}
};

USTRUCT(BlueprintType)
struct HUICRSYNCRUNTIME_API FHUICRSyncTransformPayload
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	FTransform Transform;

	friend FArchive& operator<<(FArchive& Ar, FHUICRSyncTransformPayload& Data)
	{
		Ar << Data.Transform;
		return Ar;
	}
};

USTRUCT()
struct FHUICRSyncGuestActorMapping
{
	GENERATED_BODY()

	UPROPERTY()
	FTransform PCMain = FTransform::Identity;

	UPROPERTY()
	FTransform HMDMain = FTransform::Identity;

	UPROPERTY()
	FVector ScaleRatio = FVector::ZeroVector;

	bool IsValid() const
	{
		return !ScaleRatio.ContainsNaN() && ScaleRatio.GetAbsMin() > UE_SMALL_NUMBER;
	}

	FTransform ToHMD(const FTransform& Pose) const
	{
		return FTransform(HMDMain.GetRotation() * PCMain.GetRotation().Inverse() * Pose.GetRotation(),
			HMDMain.TransformPosition(PCMain.InverseTransformPosition(Pose.GetLocation()) * ScaleRatio),
			Pose.GetScale3D() * ScaleRatio);
	}

	FTransform ToPC(const FTransform& Pose) const
	{
		return FTransform(PCMain.GetRotation() * HMDMain.GetRotation().Inverse() * Pose.GetRotation(),
			PCMain.TransformPosition(HMDMain.InverseTransformPosition(Pose.GetLocation()) / ScaleRatio),
			Pose.GetScale3D() / ScaleRatio);
	}
};

USTRUCT(BlueprintType)
struct HUICRSYNCRUNTIME_API FHUICRSyncNetEntry
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	EHUICRSyncActorType TypeCode = EHUICRSyncActorType::HUIActor;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	int32 ActorID = -1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	FString Address;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	TArray<uint8> PayloadBytes;

	friend FArchive& operator<<(FArchive& Ar, FHUICRSyncNetEntry& Data)
	{
		Ar << Data.TypeCode;
		Ar << Data.ActorID;
		Ar << Data.Address;
		Ar << Data.PayloadBytes;
		return Ar;
	}
};

USTRUCT(BlueprintType)
struct HUICRSYNCRUNTIME_API FHUICRSyncNetInnerMapEntry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "HUICR Sync")
	TMap<int32, FHUICRSyncNetEntry> InnerMap;
};

UENUM(BlueprintType)
enum class EHUICRSyncCommandArgType : uint8
{
	Bool,
	Int32,
	Float,
	String
};

USTRUCT(BlueprintType)
struct HUICRSYNCRUNTIME_API FHUICRSyncCommandArg
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	EHUICRSyncCommandArgType Type = EHUICRSyncCommandArgType::Bool;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	bool BoolValue = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	int32 IntValue = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	float FloatValue = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	FString StringValue;

	friend FArchive& operator<<(FArchive& Ar, FHUICRSyncCommandArg& Arg)
	{
		Ar << Arg.Type;
		Ar << Arg.BoolValue;
		Ar << Arg.IntValue;
		Ar << Arg.FloatValue;
		Ar << Arg.StringValue;
		return Ar;
	}
};

USTRUCT(BlueprintType)
struct HUICRSYNCRUNTIME_API FHUICRSyncCommandPayload
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	FString Address;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUICR Sync")
	TArray<FHUICRSyncCommandArg> Args;

	friend FArchive& operator<<(FArchive& Ar, FHUICRSyncCommandPayload& Payload)
	{
		Ar << Payload.Address;
		Ar << Payload.Args;
		return Ar;
	}
};

USTRUCT()
struct HUICRSYNCRUNTIME_API FHUICRSyncPendingCommand
{
	GENERATED_BODY()

	uint32 CommandId = 0;
	FString Address;
	TArray<FHUICRSyncCommandArg> Args;
	double LastSendTime = 0.0;
	int32 RetryCount = 0;
};
