#pragma once

#include "K2Node.h"
#include "HUICRSyncFields.h"
#include "K2Node_HUICRSyncPayload.generated.h"

UENUM()
enum class EHUICRSyncPayloadWriteMode : uint8
{
	Initial UMETA(DisplayName = "Initial Values", ToolTip = "Set the starting values before the PC captures the actor for spawning."),
	Outgoing UMETA(DisplayName = "Current Local Values", ToolTip = "Prepare this device's latest local values for fields it controls.")
};

UENUM()
enum class EHUICRSyncPayloadReadMode : uint8
{
	Initial UMETA(ToolTip = "Read the starting values supplied when this actor was spawned."),
	Received UMETA(ToolTip = "Read the current PC-confirmed values and the fields to apply.")
};

/** Payload nodes use the shared payload type configured on the owning Sync Actor Blueprint. */
UCLASS(Abstract)
class UK2Node_HUICRSyncPayload : public UK2Node
{
	GENERATED_BODY()

public:
	virtual void AllocateDefaultPins() override;
	virtual void PostLoad() override;
	virtual void ConvertDeprecatedNode(UEdGraph* Graph, bool bOnlySafeChanges) override;
	virtual void ExpandNode(FKismetCompilerContext& Compiler, UEdGraph* SourceGraph) override;
	virtual void PinDefaultValueChanged(UEdGraphPin* Pin) override;
	virtual bool HasExternalDependencies(TArray<UStruct*>* OptionalOutput = nullptr) const override;
	virtual bool IsCompatibleWithGraph(const UEdGraph* TargetGraph) const override;
	virtual void GetMenuActions(FBlueprintActionDatabaseRegistrar& Registrar) const override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetTooltipText() const override;
	virtual FText GetMenuCategory() const override;
	UScriptStruct* ResolvePayloadStruct() const;
	void GetFieldChoices(TArray<TSharedPtr<FName>>& Choices) const;
	static FName MemberPinName(FName PropertyName);
	static FName FieldExecPinName(FName PropertyName);

protected:
	bool IsReadNode() const;
	bool UsesInitialPayload() const;
	void AddModePin();
	void AddMissingFieldExecPins();

	UPROPERTY()
	bool bInitialMode = false;

	// A Blueprint's new generated CDO is not populated yet when its graph is expanded.
	UPROPERTY()
	TObjectPtr<UScriptStruct> CachedPayloadStruct;

	UPROPERTY()
	FName SelectedField;

	UPROPERTY()
	EHUICRSyncFieldSource SelectedSource = EHUICRSyncFieldSource::FollowMyPayload;
};

UCLASS()
class UK2Node_WriteSyncPayload : public UK2Node_HUICRSyncPayload
{
	GENERATED_BODY()
};

UCLASS()
class UK2Node_ReadSyncPayload : public UK2Node_HUICRSyncPayload
{
	GENERATED_BODY()
};

UCLASS()
class UK2Node_SetInitialPayload : public UK2Node_HUICRSyncPayload
{
	GENERATED_BODY()
};

UCLASS()
class UK2Node_PrepareOutgoingPayload : public UK2Node_HUICRSyncPayload
{
	GENERATED_BODY()
};

UCLASS()
class UK2Node_ReadReceivedPayload : public UK2Node_HUICRSyncPayload
{
	GENERATED_BODY()
};

UCLASS()
class UK2Node_SetSyncFieldSource : public UK2Node_HUICRSyncPayload
{
	GENERATED_BODY()
};
