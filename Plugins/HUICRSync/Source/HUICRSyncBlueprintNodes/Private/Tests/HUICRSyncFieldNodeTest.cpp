#if WITH_DEV_AUTOMATION_TESTS

#include "K2Node_HUICRSyncPayload.h"
#include "HUICRSyncActor.h"
#include "HUICRSyncSubsystem.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UObject/Script.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHUICRSyncFieldNodeTest, "HUICRSync.Blueprint.SharedPayloadNodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHUICRSyncFieldNodeTest::RunTest(const FString& Parameters)
{
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
	Blueprint->GeneratedClass->GetDefaultObject<AHUICRSyncActor>()->SharedPayloadStruct = PayloadType;
	UEdGraph* Graph = Blueprint->UbergraphPages[0];
	const auto* Schema = GetDefault<UEdGraphSchema_K2>();
	auto Add = [Graph](UK2Node* Node)
	{
		Graph->AddNode(Node, false, false);
		Node->CreateNewGuid();
		Node->AllocateDefaultPins();
	};
	auto Wire = [this, Schema](UEdGraphPin* From, UEdGraphPin* To)
	{
		return TestTrue(TEXT("Graph pins can be connected"), From && To && Schema->TryCreateConnection(From, To));
	};

	auto* Event = NewObject<UK2Node_CustomEvent>(Graph);
	Event->CustomFunctionName = TEXT("ProbeSharedPayload");
	Add(Event);
	auto* Prepare = NewObject<UK2Node_PrepareOutgoingPayload>(Graph);
	Add(Prepare);
	auto* Select = NewObject<UK2Node_SetSyncFieldSource>(Graph);
	Add(Select);
	auto* Read = NewObject<UK2Node_ReadReceivedPayload>(Graph);
	Add(Read);
	FProperty* Color = nullptr;
	for (TFieldIterator<FProperty> It(PayloadType); It; ++It)
	{
		const FName PinName = UK2Node_HUICRSyncPayload::MemberPinName(It->GetFName());
		auto* Input = Prepare->FindPin(PinName, EGPD_Input);
		auto* Output = Read->FindPin(PinName, EGPD_Output);
		if (!TestTrue(TEXT("Each user-defined member has matching typed input and output pins"),
			Input && Output && Input->PinType == Output->PinType))
		{
			return false;
		}
		if (It->GetAuthoredName().Contains(TEXT("Color")))
		{
			Color = *It;
		}
	}
	if (!TestNotNull(TEXT("Actual payload has a color field"), Color))
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
	if (!Wire(Event->FindPin(UEdGraphSchema_K2::PN_Then), Prepare->FindPin(UEdGraphSchema_K2::PN_Execute)) ||
		!Wire(Prepare->FindPin(UEdGraphSchema_K2::PN_Then), Select->FindPin(UEdGraphSchema_K2::PN_Execute)) ||
		!Wire(Select->FindPin(UEdGraphSchema_K2::PN_Then), Read->FindPin(UEdGraphSchema_K2::PN_Execute)) ||
		!AddBranchProbe(TEXT("Valid"), true) || !AddBranchProbe(TEXT("NoValidPayload"), false))
	{
		return false;
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
	if (!TestTrue(TEXT("All three nodes compiled to runnable Blueprint bytecode"), Blueprint->Status != BS_Error))
	{
		return false;
	}
	auto* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("Fixture world"), World)) { return false; }
	ON_SCOPE_EXIT { World->DestroyWorld(false); };
	auto* Actor = World->SpawnActor<AHUICRSyncActor>(Blueprint->GeneratedClass);
	if (!TestNotNull(TEXT("Spawn compiled actor"), Actor)) { return false; }
	auto* Probe = Actor->FindFunction(TEXT("ProbeSharedPayload"));
	if (!TestNotNull(TEXT("Compiled event"), Probe) ||
		!TestTrue(TEXT("Class Defaults kept the selected shared structure"), Actor->SharedPayloadStruct == PayloadType))
	{
		return false;
	}
	FEditorScriptExecutionGuard AllowScript;
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
	return !HasAnyErrors();
}
#endif
