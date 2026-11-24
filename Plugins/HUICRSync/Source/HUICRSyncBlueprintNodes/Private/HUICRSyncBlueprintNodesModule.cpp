#include "K2Node_HUICRSyncPayload.h"
#include "HUICRSyncActor.h"

#include "EdGraphUtilities.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Modules/ModuleManager.h"
#include "SGraphPinNameList.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

class FHUICRSyncFieldPinFactory final : public FGraphPanelPinFactory
{
public:
	virtual TSharedPtr<SGraphPin> CreatePin(UEdGraphPin* Pin) const override
	{
		if (Pin && Pin->PinName == TEXT("Field"))
		{
			if (const auto* Node = Cast<UK2Node_SetSyncFieldSource>(Pin->GetOwningNode()))
			{
				TArray<TSharedPtr<FName>> Fields;
				Node->GetFieldChoices(Fields);
				return SNew(SGraphPinNameList, Pin, Fields);
			}
		}
		return nullptr;
	}
};

class FHUICRSyncBlueprintNodesModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FieldPinFactory = MakeShared<FHUICRSyncFieldPinFactory>();
		FEdGraphUtilities::RegisterVisualPinFactory(FieldPinFactory);
		PropertyChanged = FCoreUObjectDelegates::OnObjectPropertyChanged.AddStatic(&RefreshPayloadNodes);
	}

	virtual void ShutdownModule() override
	{
		FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(PropertyChanged);
		FEdGraphUtilities::UnregisterVisualPinFactory(FieldPinFactory);
		FieldPinFactory.Reset();
	}

private:
	static void RefreshPayloadNodes(UObject* Object, FPropertyChangedEvent& Event)
	{
		if (!Object || !Object->HasAnyFlags(RF_ClassDefaultObject) || !Object->IsA<AHUICRSyncActor>()
			|| Event.GetPropertyName() != GET_MEMBER_NAME_CHECKED(AHUICRSyncActor, SharedPayloadStruct))
		{
			return;
		}
		TArray<UK2Node_HUICRSyncPayload*> Nodes;
		for (TObjectIterator<UK2Node_HUICRSyncPayload> It; It; ++It)
		{
			const UBlueprint* BP = FBlueprintEditorUtils::FindBlueprintForNode(*It);
			if (BP && !BP->bBeingCompiled && BP->GeneratedClass && BP->GeneratedClass->IsChildOf(Object->GetClass()))
			{
				Nodes.Add(*It);
			}
		}
		for (UK2Node_HUICRSyncPayload* Node : Nodes)
		{
			Node->ReconstructNode();
			Node->GetGraph()->NotifyGraphChanged();
		}
	}

	TSharedPtr<FHUICRSyncFieldPinFactory> FieldPinFactory;
	FDelegateHandle PropertyChanged;
};

IMPLEMENT_MODULE(FHUICRSyncBlueprintNodesModule, HUICRSyncBlueprintNodes)
