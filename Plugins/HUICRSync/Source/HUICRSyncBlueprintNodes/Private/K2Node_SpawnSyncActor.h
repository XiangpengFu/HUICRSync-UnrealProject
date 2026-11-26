#pragma once

#include "K2Node.h"
#include "K2Node_SpawnSyncActor.generated.h"

/** Exposes the selected sync actor's shared struct as optional spawn inputs. */
UCLASS()
class UK2Node_SpawnSyncActor : public UK2Node
{
	GENERATED_BODY()

public:
	virtual void AllocateDefaultPins() override;
	virtual void ReallocatePinsDuringReconstruction(TArray<UEdGraphPin*>& OldPins) override;
	virtual void PinDefaultValueChanged(UEdGraphPin* Pin) override;
	virtual void PinConnectionListChanged(UEdGraphPin* Pin) override;
	virtual void ExpandNode(FKismetCompilerContext& Compiler, UEdGraph* SourceGraph) override;
	virtual bool HasExternalDependencies(TArray<UStruct*>* OptionalOutput = nullptr) const override;
	virtual void GetMenuActions(FBlueprintActionDatabaseRegistrar& Registrar) const override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetMenuCategory() const override;
	virtual FText GetTooltipText() const override;

	UClass* GetSpawnClass() const;

private:
	void AddInitialPins(const UEdGraphPin* ClassPin);
	void RefreshClassPins();

	// While a Blueprint recompiles its new class defaults may not have been copied yet.
	UPROPERTY()
	TObjectPtr<UScriptStruct> CachedSchema;
};
