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
	bool bRemoteControlsLocalScreen = false;

	friend FArchive& operator<<(FArchive& Ar, FHUICRSyncScreenPayload& Data)
	{
		Ar << Data.ScreenID;
		Ar << Data.Transform;
		Ar << Data.bRemoteControlsLocalScreen;
		return Ar;
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
