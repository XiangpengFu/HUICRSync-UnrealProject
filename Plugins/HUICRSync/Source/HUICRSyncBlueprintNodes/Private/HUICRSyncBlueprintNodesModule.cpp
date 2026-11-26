#include "K2Node_HUICRSyncPayload.h"
#include "HUICRSyncActor.h"
#include "HUICRSyncHMDPawn.h"
#include "HUICRSyncSubsystem.h"

#include "EdGraphUtilities.h"
#include "Engine/Blueprint.h"
#include "K2Node_AddDelegate.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "KismetNodes/SGraphNodeK2Default.h"
#include "Modules/ModuleManager.h"
#include "SGraphPinNameList.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

class FHUICRSyncDelegateNodeFactory final : public FGraphPanelNodeFactory
{
public:
	virtual TSharedPtr<SGraphNode> CreateNode(UEdGraphNode* Node) const override
	{
		auto* Bind = Cast<UK2Node_AddDelegate>(Node);
		if (!Bind)
		{
			return nullptr;
		}
		const FName Name = Bind->GetPropertyName();
		if (Name != GET_MEMBER_NAME_CHECKED(UHUICRSyncSubsystem, OnPrepareNetDataSend) &&
			Name != GET_MEMBER_NAME_CHECKED(UHUICRSyncSubsystem, OnNetDataReceived))
		{
			return nullptr;
		}
		const FProperty* Property = Bind->GetProperty();
		if (!Property || Property->GetOwnerClass() != UHUICRSyncSubsystem::StaticClass())
		{
			return nullptr;
		}
		// The standard Bind node uses its class tooltip. Show the selected dispatcher's timing and scope.
		TSharedRef<SGraphNodeK2Default> Widget = SNew(SGraphNodeK2Default, Bind);
		Widget->SetToolTipText(Property->GetToolTipText());
		return Widget;
	}
};

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
		DelegateNodeFactory = MakeShared<FHUICRSyncDelegateNodeFactory>();
		FEdGraphUtilities::RegisterVisualNodeFactory(DelegateNodeFactory);
		PropertyChanged = FCoreUObjectDelegates::OnObjectPropertyChanged.AddStatic(&RefreshPayloadNodes);
	}

	virtual void ShutdownModule() override
	{
		FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(PropertyChanged);
		FEdGraphUtilities::UnregisterVisualNodeFactory(DelegateNodeFactory);
		DelegateNodeFactory.Reset();
		FEdGraphUtilities::UnregisterVisualPinFactory(FieldPinFactory);
		FieldPinFactory.Reset();
	}

private:
	static void RefreshPayloadNodes(UObject* Object, FPropertyChangedEvent& Event)
	{
		if (!Object || !Object->HasAnyFlags(RF_ClassDefaultObject) || (!Object->IsA<AHUICRSyncActor>() && !Object->IsA<AHUICRSyncHMDPawn>())
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
	TSharedPtr<FHUICRSyncDelegateNodeFactory> DelegateNodeFactory;
	FDelegateHandle PropertyChanged;
};

IMPLEMENT_MODULE(FHUICRSyncBlueprintNodesModule, HUICRSyncBlueprintNodes)
