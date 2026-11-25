#if WITH_DEV_AUTOMATION_TESTS

#include "K2Node_HUICRSyncPayload.h"
#include "HUICRSyncActor.h"
#include "HUICRSyncPayloadLibrary.h"
#include "HUICRSyncSubsystem.h"
#include "BlueprintActionDatabase.h"
#include "BlueprintNodeSpawner.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "NodeFactory.h"
#include "SGraphNode.h"
#include "UObject/Script.h"
#include "UObject/UnrealType.h"
#include "Widgets/SToolTip.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHUICRSyncFieldNodeTest, "HUICRSync.Blueprint.SharedPayloadNodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHUICRSyncFieldNodeTest::RunTest(const FString& Parameters)
{
	auto HasPaletteEntry = [](UClass* NodeClass)
	{
		FBlueprintActionDatabase& Database = FBlueprintActionDatabase::Get();
		Database.RefreshClassActions(NodeClass);
		const auto* Actions = Database.GetAllActions().Find(NodeClass);
		return Actions && Actions->ContainsByPredicate([NodeClass](const UBlueprintNodeSpawner* Action)
			{ return Action && Action->NodeClass.Get() == NodeClass; });
	};
	for (UClass* Node : { UK2Node_SetInitialPayload::StaticClass(), UK2Node_PrepareOutgoingPayload::StaticClass(), UK2Node_ReadReceivedPayload::StaticClass() })
	{
		TestFalse(*FString::Printf(TEXT("Legacy %s is absent from the node menu"), *Node->GetName()), HasPaletteEntry(Node));
	}
	for (UClass* Node : { UK2Node_WriteSyncPayload::StaticClass(), UK2Node_ReadSyncPayload::StaticClass(), UK2Node_SetSyncFieldSource::StaticClass() })
	{
		TestTrue(*FString::Printf(TEXT("%s is available in the node menu"), *Node->GetName()), HasPaletteEntry(Node));
	}
	const FProperty* SubsystemProperty = AHUICRSyncActor::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(AHUICRSyncActor, SyncSubsystem));
	TestTrue(TEXT("Sync Subsystem is readable in Blueprint graphs and hidden in Details"),
		SubsystemProperty && SubsystemProperty->HasAllPropertyFlags(CPF_BlueprintVisible | CPF_BlueprintReadOnly) &&
		!SubsystemProperty->HasAnyPropertyFlags(CPF_Edit));
	for (const FName Name : { GET_MEMBER_NAME_CHECKED(AHUICRSyncActor, ActorInitPayloadData),
		GET_MEMBER_NAME_CHECKED(AHUICRSyncActor, ActorSendPayloadData), GET_MEMBER_NAME_CHECKED(AHUICRSyncActor, ActorReceivePayloadData) })
	{
		const FProperty* Buffer = AHUICRSyncActor::StaticClass()->FindPropertyByName(Name);
		TestTrue(*FString::Printf(TEXT("%s stays available for Blueprint Get and Set without a Details row"), *Name.ToString()),
			Buffer && Buffer->HasAnyPropertyFlags(CPF_BlueprintVisible) && !Buffer->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintReadOnly));
	}
	auto* PayloadType = LoadObject<UScriptStruct>(nullptr, TEXT("/HUICRSync/BlueprintStructs/BPS_TestActor.BPS_TestActor"));
	if (!TestNotNull(TEXT("Load the actual Blueprint-defined payload"), PayloadType))
	{
		return false;
	}
	// The test edits a transient child Blueprint only. The example assets are read-only fixtures.
	auto* Blueprint = FKismetEditorUtilities::CreateBlueprint(AHUICRSyncActor::StaticClass(), GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UBlueprint::StaticClass(), TEXT("HUICRFieldNodeTest")),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!TestNotNull(TEXT("Temporary Sync Actor Blueprint"), Blueprint) || Blueprint->UbergraphPages.IsEmpty())
	{
		return false;
	}
	const FName ObservedColor(TEXT("ObservedPayloadColor"));
	const FName ObservedFields(TEXT("ObservedPayloadFields"));
	const FName AppliedColor(TEXT("ColorAppliedByFieldPin"));
	const FName AppliedTransform(TEXT("TransformAppliedByFieldPin"));
	FEdGraphPinType VectorPin, MaskPin, TransformPin;
	VectorPin.PinCategory = UEdGraphSchema_K2::PC_Struct;
	VectorPin.PinSubCategoryObject = TBaseStructure<FVector>::Get();
	TransformPin.PinCategory = UEdGraphSchema_K2::PC_Struct;
	TransformPin.PinSubCategoryObject = TBaseStructure<FTransform>::Get();
	MaskPin.PinCategory = UEdGraphSchema_K2::PC_Name;
	MaskPin.ContainerType = EPinContainerType::Array;
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, ObservedColor, VectorPin);
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, ObservedFields, MaskPin);
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, AppliedColor, VectorPin);
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, AppliedTransform, TransformPin);
	Blueprint->GeneratedClass->GetDefaultObject<AHUICRSyncActor>()->SharedPayloadStruct = PayloadType;
	UEdGraph* Graph = Blueprint->UbergraphPages[0];
	const auto* Schema = GetDefault<UEdGraphSchema_K2>();
	auto Add = [Graph](UK2Node* Node)
	{
		Graph->AddNode(Node, false, false);
		Node->CreateNewGuid();
		Node->AllocateDefaultPins();
	};
	// Names are intentionally literal: existing Blueprint bindings store these identifiers.
	for (const FName SavedName : { FName(TEXT("OnPrepareNetDataSend")), FName(TEXT("OnNetDataReceived")) })
	{
		UClass* Owner = UHUICRSyncSubsystem::StaticClass();
		const FProperty* Delegate = Owner->FindPropertyByName(SavedName);
		if (!TestNotNull(*FString::Printf(TEXT("Saved binding %s resolves"), *SavedName.ToString()), Delegate))
		{
			return false;
		}
		if (SavedName == TEXT("OnPrepareNetDataSend"))
		{
			TestEqual(TEXT("Prepare event has a clear pre-send display name"),
				Delegate->GetMetaData(TEXT("DisplayName")), FString(TEXT("On Prepare Net Data to Send")));
		}
		auto* Bind = NewObject<UK2Node_AddDelegate>(Graph);
		Bind->SetFromProperty(Delegate, false, Owner);
		Add(Bind);
		TestEqual(TEXT("Bind node keeps its saved member name"), Bind->GetPropertyName(), SavedName);
		TestTrue(TEXT("Bind title includes the delegate display name"),
			Bind->GetNodeTitle(ENodeTitleType::FullTitle).ToString().Contains(Delegate->GetDisplayNameText().ToString()));
		const TSharedPtr<SGraphNode> Widget = FNodeFactory::CreateNodeWidget(Bind);
		const TSharedPtr<IToolTip> Hover = Widget ? Widget->GetToolTip() : nullptr;
		if (TestTrue(TEXT("Placed bind node supplies a text tooltip"), Hover && Hover->AsWidget()->GetType() == TEXT("SToolTip")))
		{
			const FText& Displayed = StaticCastSharedRef<SToolTip>(Hover->AsWidget())->GetTextTooltip();
			TestTrue(TEXT("Bind hover explains the selected delegate"),
				!Displayed.IsEmpty() && Displayed.EqualTo(Delegate->GetToolTipText()));
		}
		Bind->DestroyNode();
	}
	auto Wire = [this, Schema](UEdGraphPin* From, UEdGraphPin* To)
	{
		return TestTrue(TEXT("Graph pins can be connected"), From && To && Schema->TryCreateConnection(From, To));
	};

	auto* Event = NewObject<UK2Node_CustomEvent>(Graph);
	Event->CustomFunctionName = TEXT("ProbeSharedPayload");
	Add(Event);
	auto* Prepare = NewObject<UK2Node_PrepareOutgoingPayload>(Graph);
	Add(Prepare);
	auto* InitEvent = NewObject<UK2Node_CustomEvent>(Graph);
	InitEvent->CustomFunctionName = TEXT("SetBirthValues");
	Add(InitEvent);
	auto* Initial = NewObject<UK2Node_SetInitialPayload>(Graph);
	Add(Initial);
	auto* Select = NewObject<UK2Node_SetSyncFieldSource>(Graph);
	Add(Select);
	auto* Read = NewObject<UK2Node_ReadReceivedPayload>(Graph);
	Add(Read);
	FProperty* Color = nullptr;
	FProperty* Motion = nullptr;
	for (TFieldIterator<FProperty> It(PayloadType); It; ++It)
	{
		const FName PinName = UK2Node_HUICRSyncPayload::MemberPinName(It->GetFName());
		auto* Input = Prepare->FindPin(PinName, EGPD_Input);
		auto* BirthInput = Initial->FindPin(PinName, EGPD_Input);
		auto* Output = Read->FindPin(PinName, EGPD_Output);
		if (!TestTrue(TEXT("Each user-defined member has matching typed input and output pins"),
			Input && BirthInput && Output && Input->PinType == Output->PinType && BirthInput->PinType == Output->PinType))
		{
			return false;
		}
		if (It->GetAuthoredName().Contains(TEXT("Color")))
		{
			Color = *It;
		}
		if (Output->PinType.PinSubCategoryObject == TBaseStructure<FTransform>::Get())
		{
			Motion = *It;
		}
	}
	if (!TestNotNull(TEXT("Actual payload has a color field"), Color) || !TestNotNull(TEXT("Actual payload has a transform field"), Motion))
	{
		return false;
	}
	Schema->TrySetDefaultValue(*Select->FindPinChecked(TEXT("Field")), Color->GetAuthoredName());
	Schema->TrySetDefaultValue(*Select->FindPinChecked(TEXT("Source")), TEXT("UseSpecifiedValue"));
	auto* ColorInput = Select->FindPin(TEXT("Value"), EGPD_Input);
	if (!TestTrue(TEXT("Selecting a specified color produces a typed input"),
		ColorInput && ColorInput->PinType == Prepare->FindPinChecked(UK2Node_HUICRSyncPayload::MemberPinName(Color->GetFName()))->PinType))
	{
		return false;
	}
	// The project's Initial Color is a Vector. Distinct node literals catch a silently dropped default during expansion.
	if (!TestTrue(TEXT("Example stores color as a vector"), ColorInput->PinType.PinSubCategoryObject == TBaseStructure<FVector>::Get()))
	{
		return false;
	}
	Schema->TrySetDefaultValue(*ColorInput, TEXT("0.25,0.5,1.0"));
	Schema->TrySetDefaultValue(*Initial->FindPinChecked(UK2Node_HUICRSyncPayload::MemberPinName(Color->GetFName())),
		TEXT("1.0,0.0,0.0"));
	auto* ApplyMask = Read->FindPin(TEXT("FieldsToApply"), EGPD_Output);
	TestTrue(TEXT("Read exposes a name array for selective application"),
		ApplyMask && ApplyMask->PinType.IsArray() && ApplyMask->PinType.PinCategory == UEdGraphSchema_K2::PC_Name);
	auto AddBranchProbe = [&](const TCHAR* PinName, bool bValue)
	{
		auto* Set = NewObject<UK2Node_VariableSet>(Graph);
		Set->VariableReference.SetSelfMember(GET_MEMBER_NAME_CHECKED(AHUICRSyncActor, bHideDuringCalibration));
		Add(Set);
		Schema->TrySetDefaultValue(*Set->FindPinChecked(TEXT("bHideDuringCalibration")), bValue ? TEXT("true") : TEXT("false"));
		return Wire(Read->FindPin(PinName), Set->FindPin(UEdGraphSchema_K2::PN_Execute));
	};
	if (!Wire(InitEvent->FindPin(UEdGraphSchema_K2::PN_Then), Initial->FindPin(UEdGraphSchema_K2::PN_Execute)) ||
		!Wire(Event->FindPin(UEdGraphSchema_K2::PN_Then), Prepare->FindPin(UEdGraphSchema_K2::PN_Execute)) ||
		!Wire(Prepare->FindPin(UEdGraphSchema_K2::PN_Then), Select->FindPin(UEdGraphSchema_K2::PN_Execute)) ||
		!Wire(Select->FindPin(UEdGraphSchema_K2::PN_Then), Read->FindPin(UEdGraphSchema_K2::PN_Execute)) ||
		!AddBranchProbe(TEXT("Valid"), true) || !AddBranchProbe(TEXT("NoValidPayload"), false))
	{
		return false;
	}

	// Build the two public nodes in both modes alongside the original, still loadable nodes.
	auto NewEvent = [&](FName Name)
	{
		auto* Node = NewObject<UK2Node_CustomEvent>(Graph);
		Node->CustomFunctionName = Name;
		Add(Node);
		return Node;
	};
	auto SetObserved = [&](FName Name)
	{
		auto* Node = NewObject<UK2Node_VariableSet>(Graph);
		Node->VariableReference.SetSelfMember(Name);
		Add(Node);
		return Node;
	};
	const FName ColorPin = UK2Node_HUICRSyncPayload::MemberPinName(Color->GetFName());
	const FName MotionPin = UK2Node_HUICRSyncPayload::MemberPinName(Motion->GetFName());
	const FName ApplyColorPin = UK2Node_HUICRSyncPayload::FieldExecPinName(Color->GetFName());
	const FName ApplyMotionPin = UK2Node_HUICRSyncPayload::FieldExecPinName(Motion->GetFName());
	auto* WriteBirth = NewObject<UK2Node_WriteSyncPayload>(Graph);
	auto* WriteOngoing = NewObject<UK2Node_WriteSyncPayload>(Graph);
	auto* ReadBirth = NewObject<UK2Node_ReadSyncPayload>(Graph);
	auto* ReadOngoing = NewObject<UK2Node_ReadSyncPayload>(Graph);
	auto* ReadFieldsOnly = NewObject<UK2Node_ReadSyncPayload>(Graph);
	Add(WriteBirth);
	Add(WriteOngoing);
	Add(ReadBirth);
	Add(ReadOngoing);
	Add(ReadFieldsOnly);
	for (TFieldIterator<FProperty> It(PayloadType); It; ++It)
	{
		const FName ApplyPinName = UK2Node_HUICRSyncPayload::FieldExecPinName(It->GetFName());
		const UEdGraphPin* ApplyPin = ReadOngoing->FindPin(ApplyPinName, EGPD_Output);
		if (!TestTrue(TEXT("Read Received generates a separate exec output for each user-defined field"),
			ApplyPin && ApplyPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec &&
			ApplyPin->PinFriendlyName.ToString().Contains(It->GetDisplayNameText().ToString()) && !WriteOngoing->FindPin(ApplyPinName)))
		{
			return false;
		}
	}
	TestEqual(TEXT("Write initially prepares outgoing data"), WriteBirth->FindPinChecked(TEXT("Mode"))->DefaultValue, FString(TEXT("Outgoing")));
	TestEqual(TEXT("Read initially selects received state"), ReadBirth->FindPinChecked(TEXT("Mode"))->DefaultValue, FString(TEXT("Received")));
	const UEnum* ReadModes = CastChecked<UEnum>(ReadBirth->FindPinChecked(TEXT("Mode"))->PinType.PinSubCategoryObject.Get());
	TestEqual(TEXT("Saved Initial mode displays Initial Values"),
		ReadModes->GetMetaData(TEXT("DisplayName"), ReadModes->GetIndexByNameString(TEXT("Initial"))), FString(TEXT("Initial Values")));
	TestEqual(TEXT("Saved Received mode displays Received Values"),
		ReadModes->GetMetaData(TEXT("DisplayName"), ReadModes->GetIndexByNameString(TEXT("Received"))), FString(TEXT("Received Values")));
	auto* ReadBirthEvent = NewEvent(TEXT("ReadBirth"));
	auto* WriteBothEvent = NewEvent(TEXT("WriteBoth"));
	auto* ReadOngoingEvent = NewEvent(TEXT("ReadOngoing"));
	auto* ReadFieldsOnlyEvent = NewEvent(TEXT("ReadFieldsWithoutValid"));
	auto* BirthColor = SetObserved(ObservedColor);
	auto* OngoingColor = SetObserved(ObservedColor);
	auto* EmptyColor = SetObserved(ObservedColor);
	auto* FieldsProbe = SetObserved(ObservedFields);
	auto* ColorBranch = SetObserved(AppliedColor);
	auto* MotionBranch = SetObserved(AppliedTransform);
	auto* FieldsOnlyColor = SetObserved(AppliedColor);
	auto* FieldsOnlyMotion = SetObserved(AppliedTransform);
	const FName Exec = UEdGraphSchema_K2::PN_Execute;
	const FName Then = UEdGraphSchema_K2::PN_Then;
	// Connect pins *before* mode switching, as a Blueprint developer can do.
	if (!Wire(ReadBirthEvent->FindPin(Then), ReadBirth->FindPin(Exec)) ||
		!Wire(ReadBirth->FindPin(TEXT("Valid")), BirthColor->FindPin(Exec)) ||
		!Wire(ReadBirth->FindPin(ColorPin), BirthColor->FindPin(ObservedColor)) ||
		!Wire(ReadBirth->FindPin(TEXT("FieldsToApply")), FieldsProbe->FindPin(ObservedFields)) ||
		!Wire(ReadBirth->FindPin(ApplyColorPin), ColorBranch->FindPin(Exec)) ||
		!Wire(WriteBothEvent->FindPin(Then), WriteBirth->FindPin(Exec)) ||
		!Wire(WriteBirth->FindPin(Then), WriteOngoing->FindPin(Exec)))
	{
		return false;
	}
	Schema->TrySetDefaultValue(*WriteBirth->FindPinChecked(ColorPin), TEXT("1,0,0"));
	Schema->TrySetDefaultValue(*WriteBirth->FindPinChecked(TEXT("Mode")), TEXT("Initial"));
	Schema->TrySetDefaultValue(*WriteOngoing->FindPinChecked(ColorPin), TEXT("0,0,1"));
	Schema->TrySetDefaultValue(*ReadBirth->FindPinChecked(TEXT("Mode")), TEXT("Initial"));
	TestTrue(TEXT("Initial keeps its typed value and execution links"),
		BirthColor->FindPinChecked(ObservedColor)->LinkedTo.Contains(ReadBirth->FindPinChecked(ColorPin)) &&
		ReadBirth->FindPinChecked(Exec)->LinkedTo.Contains(ReadBirthEvent->FindPinChecked(Then)));
	TestTrue(TEXT("Switching to Initial removes the received-only mask and its wire"),
		!ReadBirth->FindPin(TEXT("FieldsToApply")) && FieldsProbe->FindPinChecked(ObservedFields)->LinkedTo.IsEmpty());
	TestTrue(TEXT("Initial removes field exec outputs, including a previously wired one"),
		!ReadBirth->FindPin(ApplyColorPin) && !ReadBirth->FindPin(ApplyMotionPin) && ColorBranch->FindPinChecked(Exec)->LinkedTo.IsEmpty());
	Schema->TrySetDefaultValue(*EmptyColor->FindPinChecked(ObservedColor), TEXT("-1,-1,-1"));
	auto* BeginPlay = FBlueprintEditorUtils::FindOverrideForFunction(Blueprint, AActor::StaticClass(), TEXT("ReceiveBeginPlay"));
	if (!BeginPlay)
	{
		int32 Y = 0;
		BeginPlay = FKismetEditorUtilities::AddDefaultEventNode(Blueprint, Graph, TEXT("ReceiveBeginPlay"), AActor::StaticClass(), Y);
	}
	int32 EventY = 0;
	auto* FieldsReceived = FKismetEditorUtilities::AddDefaultEventNode(Blueprint, Graph,
		GET_FUNCTION_NAME_CHECKED(AHUICRSyncActor, OnSyncFieldsReceived), AHUICRSyncActor::StaticClass(), EventY);
	auto* PrepareFields = FKismetEditorUtilities::AddDefaultEventNode(Blueprint, Graph,
		GET_FUNCTION_NAME_CHECKED(AHUICRSyncActor, OnPrepareSyncFields), AHUICRSyncActor::StaticClass(), EventY);
	auto* PrepareMask = PrepareFields ? PrepareFields->FindPin(TEXT("FieldsToPrepare"), EGPD_Output) : nullptr;
	if (!TestNotNull(TEXT("Actual Blueprint BeginPlay"), BeginPlay) ||
		!TestNotNull(TEXT("Sync Actor exposes its own receive event to the graph"), FieldsReceived) ||
		!TestTrue(TEXT("Sync Actor's prepare event exposes a field-name array to Blueprint"),
			PrepareMask && PrepareMask->PinType.IsArray() && PrepareMask->PinType.PinCategory == UEdGraphSchema_K2::PC_Name) ||
		!Wire(PrepareFields->FindPin(Then), WriteOngoing->FindPin(Exec)) ||
		!Wire(FieldsReceived->FindPin(Then), ReadOngoing->FindPin(Exec)) ||
		!Wire(BeginPlay->FindPin(Then), ReadBirth->FindPin(Exec)) ||
		!Wire(ReadBirth->FindPin(TEXT("NoValidPayload")), EmptyColor->FindPin(Exec)) ||
		!Wire(ReadOngoingEvent->FindPin(Then), ReadOngoing->FindPin(Exec)) ||
		!Wire(ReadOngoing->FindPin(TEXT("NoValidPayload")), EmptyColor->FindPin(Exec)) ||
		!Wire(ReadOngoing->FindPin(TEXT("Valid")), OngoingColor->FindPin(Exec)) ||
		!Wire(ReadOngoing->FindPin(ColorPin), OngoingColor->FindPin(ObservedColor)) ||
		!Wire(ReadOngoing->FindPin(ApplyColorPin), ColorBranch->FindPin(Exec)) ||
		!Wire(ReadOngoing->FindPin(ColorPin), ColorBranch->FindPin(AppliedColor)) ||
		!Wire(ReadOngoing->FindPin(ApplyMotionPin), MotionBranch->FindPin(Exec)) ||
		!Wire(ReadOngoing->FindPin(MotionPin), MotionBranch->FindPin(AppliedTransform)) ||
		!Wire(ReadFieldsOnlyEvent->FindPin(Then), ReadFieldsOnly->FindPin(Exec)) ||
		!Wire(ReadFieldsOnly->FindPin(ApplyColorPin), FieldsOnlyColor->FindPin(Exec)) ||
		!Wire(ReadFieldsOnly->FindPin(ColorPin), FieldsOnlyColor->FindPin(AppliedColor)) ||
		!Wire(ReadFieldsOnly->FindPin(ApplyMotionPin), FieldsOnlyMotion->FindPin(Exec)) ||
		!Wire(ReadFieldsOnly->FindPin(MotionPin), FieldsOnlyMotion->FindPin(AppliedTransform)) ||
		!Wire(OngoingColor->FindPin(Then), FieldsProbe->FindPin(Exec)) ||
		!Wire(ReadOngoing->FindPin(TEXT("FieldsToApply")), FieldsProbe->FindPin(ObservedFields)))
	{
		return false;
	}
	// Exercise the same safe conversions used when a previously saved graph opens.
	auto Upgrade = [&](UK2Node_HUICRSyncPayload* Legacy, UClass* ExpectedClass, const TCHAR* ExpectedMode)
	{
		const FGuid Guid = Legacy->NodeGuid;
		Legacy->ConvertDeprecatedNode(Graph, true);
		for (UEdGraphNode* Candidate : Graph->Nodes)
		{
			if (Candidate->NodeGuid == Guid)
			{
				if (ExpectedClass == UK2Node_ReadSyncPayload::StaticClass() &&
					!TestTrue(TEXT("Upgrading a saved legacy read adds the field execution pins"),
						Candidate->FindPin(ApplyColorPin, EGPD_Output) && Candidate->FindPin(ApplyMotionPin, EGPD_Output)))
				{
					return false;
				}
				const auto* Mode = Candidate->FindPin(TEXT("Mode"));
				return TestTrue(*FString::Printf(TEXT("Retired node upgrades to %s / %s"), *ExpectedClass->GetName(), ExpectedMode),
					Candidate->GetClass() == ExpectedClass && Mode && Mode->DefaultValue == ExpectedMode);
			}
		}
		AddError(TEXT("The upgraded node lost its identity in the graph"));
		return false;
	};
	if (!Upgrade(Initial, UK2Node_WriteSyncPayload::StaticClass(), TEXT("Initial")) ||
		!Upgrade(Prepare, UK2Node_WriteSyncPayload::StaticClass(), TEXT("Outgoing")) ||
		!Upgrade(Read, UK2Node_ReadSyncPayload::StaticClass(), TEXT("Received")))
	{
		return false;
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
	if (!TestTrue(TEXT("Combined and legacy nodes compiled to runnable Blueprint bytecode"), Blueprint->Status != BS_Error))
	{
		return false;
	}
	auto* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("Fixture world"), World)) { return false; }
	bool bFinishNextFrame = false;
	ON_SCOPE_EXIT { if (!bFinishNextFrame) { World->DestroyWorld(false); } };
	auto* Actor = World->SpawnActor<AHUICRSyncActor>(Blueprint->GeneratedClass);
	if (!TestNotNull(TEXT("Spawn compiled actor"), Actor)) { return false; }
	auto* Probe = Actor->FindFunction(TEXT("ProbeSharedPayload"));
	if (!TestNotNull(TEXT("Compiled event"), Probe) ||
		!TestTrue(TEXT("Class Defaults kept the selected shared structure"), Actor->SharedPayloadStruct == PayloadType))
	{
		return false;
	}
	FEditorScriptExecutionGuard AllowScript;
	Actor->ProcessEvent(Actor->FindFunctionChecked(TEXT("SetBirthValues")), nullptr);
	FInstancedStruct Birth;
	if (!TestTrue(TEXT("The compiled initialization node writes an ordinary spawn payload"),
		UHUICRSyncPayloadLibrary::ReadInstancedStruct(Actor->ActorInitPayloadData, PayloadType, Birth)))
	{
		return false;
	}
	TestTrue(TEXT("The spawn payload contains the initial node's own color literal"),
		Color->ContainerPtrToValuePtr<FVector>(Birth.GetMemory())->Equals(FVector(1, 0, 0)));
	TArray<uint8> Prepared;
	TestFalse(TEXT("Setting initial values does not prepare an outgoing frame"), Actor->GetPreparedSyncField(Color->GetFName(), Prepared));
	const TArray<uint8> BirthBytes = Actor->ActorInitPayloadData;
	Actor->bHideDuringCalibration = true;
	Actor->ProcessEvent(Probe, nullptr);
	TestFalse(TEXT("An empty receive buffer takes No Valid Payload"), Actor->bHideDuringCalibration);
	TArray<uint8> ColorBytes;
	const FName ColorName = Color->GetFName();
	if (!TestTrue(TEXT("Prepare emitted an actual typed color sample"), Actor->GetPreparedSyncField(ColorName, ColorBytes)) ||
		!TestTrue(TEXT("A field can be delivered to the compiled actor"), Actor->ApplyConfirmedSyncField(ColorName, ColorBytes, true)))
	{
		return false;
	}
	Actor->ProcessEvent(Probe, nullptr);
	TestTrue(TEXT("A received color takes Valid"), Actor->bHideDuringCalibration);
	Actor->SyncSubsystem = NewObject<UHUICRSyncSubsystem>(NewObject<UGameInstance>());
	Actor->SyncSubsystem->Role = EHUICRSyncRole::PC;
	Actor->SetSyncActorID(5);
	Actor->SyncSubsystem->RegisterSyncActor(Actor);
	Actor->ProcessEvent(Probe, nullptr);
	FInstancedStruct Confirmed;
	TArray<FName> Fields;
	if (!TestTrue(TEXT("The executed graph published a field"), Actor->ReadReceivedPayloadInternal(Confirmed, Fields)))
	{
		return false;
	}
	TestTrue(TEXT("Specified color literal survived Blueprint compilation and field publication"),
		Color->ContainerPtrToValuePtr<FVector>(Confirmed.GetMemory())->Equals(FVector(.25, .5, 1.0)));
	TestTrue(TEXT("Ongoing updates leave the original spawn payload intact"), Actor->ActorInitPayloadData == BirthBytes);

	auto* Mirror = World->SpawnActor<AHUICRSyncActor>(Blueprint->GeneratedClass);
	if (!TestNotNull(TEXT("Temporary mirror"), Mirror)) { return false; }
	const auto* ResultColor = FindFProperty<FStructProperty>(Blueprint->GeneratedClass, ObservedColor);
	const auto* ResultFields = FindFProperty<FArrayProperty>(Blueprint->GeneratedClass, ObservedFields);
	const auto* ColorBranchProperty = FindFProperty<FStructProperty>(Blueprint->GeneratedClass, AppliedColor);
	const auto* MotionBranchProperty = FindFProperty<FStructProperty>(Blueprint->GeneratedClass, AppliedTransform);
	if (!TestNotNull(TEXT("Compiled color probe"), ResultColor) ||
		!TestNotNull(TEXT("Compiled field mask probe"), ResultFields) ||
		!TestNotNull(TEXT("Compiled color branch probe"), ColorBranchProperty) ||
		!TestNotNull(TEXT("Compiled motion branch probe"), MotionBranchProperty)) { return false; }
	FVector* AppliedMirrorColor = ColorBranchProperty->ContainerPtrToValuePtr<FVector>(Mirror);
	FTransform* AppliedMirrorMotion = MotionBranchProperty->ContainerPtrToValuePtr<FTransform>(Mirror);
	const FVector UnappliedColor(-7);
	const FTransform LocalHeldMotion(FVector(97, 0, 0));
	*AppliedMirrorColor = UnappliedColor;
	*AppliedMirrorMotion = LocalHeldMotion;
	auto ReadColor = [Mirror, ResultColor]() { return *ResultColor->ContainerPtrToValuePtr<FVector>(Mirror); };
	Mirror->ProcessEvent(Mirror->FindFunctionChecked(TEXT("ReadBirth")), nullptr);
	TestTrue(TEXT("Missing birth data uses No Valid Payload"), ReadColor().Equals(FVector(-1)));
	Actor->ProcessEvent(Actor->FindFunctionChecked(TEXT("WriteBoth")), nullptr);
	TArray<uint8> LatestBytes;
	if (!TestTrue(TEXT("Write Outgoing prepared a field"), Actor->GetPreparedSyncField(ColorName, LatestBytes)))
	{
		return false;
	}
	Mirror->InitializeSpawnPayload(Actor->ActorInitPayloadData);
	Mirror->bReceiveDataFromPeer = false;
	Mirror->DispatchBeginPlay();
	TestTrue(TEXT("Blueprint BeginPlay reads the red birth value with live receive disabled"), ReadColor().Equals(FVector(1, 0, 0)));
	Mirror->bReceiveDataFromPeer = true;
	Mirror->ProcessEvent(Mirror->FindFunctionChecked(TEXT("ReadOngoing")), nullptr);
	TestTrue(TEXT("Received has no value before confirmation"), ReadColor().Equals(FVector(-1)));
	TestTrue(TEXT("No Valid Payload does not run any field execution path"),
		AppliedMirrorColor->Equals(UnappliedColor) && AppliedMirrorMotion->Equals(LocalHeldMotion));
	if (!TestTrue(TEXT("Deliver blue to the mirror"), Mirror->ApplyConfirmedSyncField(ColorName, LatestBytes, true))) { return false; }
	Mirror->ProcessEvent(Mirror->FindFunctionChecked(TEXT("ReadBirth")), nullptr);
	TestTrue(TEXT("Reading Initial again still returns red after live blue arrives"), ReadColor().Equals(FVector(1, 0, 0)));

	// The received mask can be reread in the same frame. A later frame proves Initial never consumed it.
	const uint64 ReadFrame = GFrameCounter;
	const FName AuthoredColor(*Color->GetAuthoredName());
	const FName MotionName = Motion->GetFName();
	const FTransform NetworkMotion(FVector(12, 0, 0));
	FInstancedStruct Sample;
	Sample.InitializeAs(PayloadType);
	*Motion->ContainerPtrToValuePtr<FTransform>(Sample.GetMutableMemory()) = NetworkMotion;
	TArray<uint8> MotionBytes;
	if (!TestTrue(TEXT("Prepare a distinct network transform for the echo and remote update cases"),
		Actor->PrepareOutgoingPayloadInternal(Sample) && Actor->GetPreparedSyncField(MotionName, MotionBytes))) { return false; }
	bFinishNextFrame = true;
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([=, this]()
	{
		if (GFrameCounter == ReadFrame) { return false; }
		FEditorScriptExecutionGuard Script;
		Mirror->ProcessEvent(Mirror->FindFunctionChecked(TEXT("ReadOngoing")), nullptr);
		TestTrue(TEXT("Read Received returns the newer blue"), ReadColor().Equals(FVector(0, 0, 1)));
		TestTrue(TEXT("Read Initial did not consume the pending live color"),
			ResultFields->ContainerPtrToValuePtr<TArray<FName>>(Mirror)->Contains(AuthoredColor));
		Mirror->SyncSubsystem = Actor->SyncSubsystem;
		Mirror->SetSyncActorID(6);
		Mirror->SyncSubsystem->RegisterSyncActor(Mirror);
		ResultFields->ContainerPtrToValuePtr<TArray<FName>>(Mirror)->Reset();
		*AppliedMirrorColor = UnappliedColor;
		TArray<uint8> Red;
		if (TestTrue(TEXT("Confirm remote color and acknowledge locally controlled motion"),
			Mirror->GetInitialSyncField(ColorName, Red) && Mirror->ApplyConfirmedSyncField(ColorName, Red, true) &&
			Mirror->ApplyConfirmedSyncField(MotionName, MotionBytes, false)))
		{
			TestTrue(TEXT("The mirror waits for its notification before changing color"), ReadColor().Equals(FVector(0, 0, 1)));
			// Dispatch through the subsystem. Only the compiled receive event can update this Blueprint variable.
			Mirror->SyncSubsystem->Tick(0.f);
			TestTrue(TEXT("The actor's receive event read the new confirmed color"), ReadColor().Equals(FVector(1, 0, 0)));
			TestTrue(TEXT("Fields To Apply is available inside the actor's receive event"),
				ResultFields->ContainerPtrToValuePtr<TArray<FName>>(Mirror)->Contains(AuthoredColor));
			TestTrue(TEXT("Only Apply Color runs while this device holds the transform"),
				AppliedMirrorColor->Equals(FVector(1, 0, 0)) && AppliedMirrorMotion->Equals(LocalHeldMotion));

			*AppliedMirrorColor = UnappliedColor;
			Mirror->ApplyConfirmedSyncField(MotionName, MotionBytes, true);
			Mirror->SyncSubsystem->Tick(0.f);
			TestTrue(TEXT("Only Apply Transform runs for a remote transform in a new batch"),
				AppliedMirrorMotion->Equals(NetworkMotion) && AppliedMirrorColor->Equals(UnappliedColor));

			*AppliedMirrorMotion = LocalHeldMotion;
			Mirror->ApplyConfirmedSyncField(MotionName, MotionBytes, true);
			Mirror->ApplyConfirmedSyncField(ColorName, Red, true);
			Mirror->SyncSubsystem->Tick(0.f);
			TestTrue(TEXT("One receive event applies both typed values when both fields are in its mask"),
				AppliedMirrorMotion->Equals(NetworkMotion) && AppliedMirrorColor->Equals(FVector(1, 0, 0)));

			Mirror->ClearSyncFieldToApply(MotionName);
			Mirror->ClearSyncFieldToApply(ColorName);
			*ResultColor->ContainerPtrToValuePtr<FVector>(Mirror) = UnappliedColor;
			*AppliedMirrorColor = UnappliedColor;
			*AppliedMirrorMotion = LocalHeldMotion;
			Mirror->ProcessEvent(Mirror->FindFunctionChecked(TEXT("ReadOngoing")), nullptr);
			TestTrue(TEXT("Valid still runs for a confirmed payload with an empty mask, with neither field branch"),
				ReadColor().Equals(FVector(1, 0, 0)) && AppliedMirrorColor->Equals(UnappliedColor) && AppliedMirrorMotion->Equals(LocalHeldMotion));

			Mirror->ApplyConfirmedSyncField(MotionName, MotionBytes, true);
			Mirror->ApplyConfirmedSyncField(ColorName, Red, true);
			Mirror->ProcessEvent(Mirror->FindFunctionChecked(TEXT("ReadFieldsWithoutValid")), nullptr);
			TestTrue(TEXT("Both Apply outputs execute on a read node with no Valid wire"),
				AppliedMirrorMotion->Equals(NetworkMotion) && AppliedMirrorColor->Equals(FVector(1, 0, 0)));
		}
		World->DestroyWorld(false);
		return true;
	}));
	return !HasAnyErrors();
}
#endif
