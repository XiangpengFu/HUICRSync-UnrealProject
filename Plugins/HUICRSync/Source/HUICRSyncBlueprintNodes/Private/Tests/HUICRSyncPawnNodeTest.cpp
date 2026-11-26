#if WITH_DEV_AUTOMATION_TESTS

#include "HUICRSyncActor.h"
#include "HUICRSyncHMDPawn.h"
#include "K2Node_HUICRSyncPayload.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/World.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UObject/Script.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHUICRSyncPawnNodeTest, "HUICRSync.Blueprint.PawnPayloadNodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHUICRSyncPawnNodeTest::RunTest(const FString& Parameters)
{
	UClass* PawnClass = AHUICRSyncHMDPawn::StaticClass();
	for (FName Name : { TEXT("ActorInitPayloadData"), TEXT("ActorSendPayloadData"), TEXT("ActorReceivePayloadData") })
	{
		const FProperty* Property = PawnClass->FindPropertyByName(Name);
		TestTrue(*FString::Printf(TEXT("Existing Blueprint byte access survives hiding %s in Details"), *Name.ToString()),
			Property && Property->HasAnyPropertyFlags(CPF_BlueprintVisible) && !Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintReadOnly));
	}
	auto* PayloadType = LoadObject<UScriptStruct>(nullptr, TEXT("/HUICRSync/BlueprintStructs/BPS_HUICRPawn.BPS_HUICRPawn"));
	if (!TestNotNull(TEXT("The example's real tracking schema loads"), PayloadType)) { return false; }
	auto* HMDExample = LoadClass<AHUICRSyncHMDPawn>(nullptr,
		TEXT("/HUICRSync/Blueprints/HUICRSyncActor/Pawn/BP_HUICRPawn_HMD.BP_HUICRPawn_HMD_C"));
	auto* PCExample = LoadClass<AHUICRSyncActor>(nullptr,
		TEXT("/HUICRSync/Blueprints/HUICRSyncActor/Pawn/BP_HUICRPawnSyncActor_PC.BP_HUICRPawnSyncActor_PC_C"));
	if (!TestNotNull(TEXT("Saved headset Pawn class"), HMDExample) || !TestNotNull(TEXT("Saved PC counterpart class"), PCExample)) { return false; }
	const auto* HMDDefaults = HMDExample->GetDefaultObject<AHUICRSyncHMDPawn>();
	const auto* PCDefaults = PCExample->GetDefaultObject<AHUICRSyncActor>();
	TestTrue(TEXT("Saved example Pawn classes select each other and share the tracking schema"),
		HMDDefaults->RemoteCounterpartClass.Get() == PCExample && PCDefaults->RemotePawnCounterpartClass.Get() == HMDExample &&
		HMDDefaults->SharedPayloadStruct == PayloadType && PCDefaults->SharedPayloadStruct == PayloadType &&
		PCDefaults->TypeCode == EHUICRSyncActorType::HMDPawn);
	auto* Blueprint = FKismetEditorUtilities::CreateBlueprint(PawnClass, GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UBlueprint::StaticClass(), TEXT("HUICRPawnPayloadProbe")),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!Blueprint || Blueprint->UbergraphPages.IsEmpty()) { AddError(TEXT("Could not create the test Pawn Blueprint")); return false; }
	const FName SamplePose(TEXT("SamplePose")), AppliedPose(TEXT("AppliedPose"));
	FEdGraphPinType PosePin;
	PosePin.PinCategory = UEdGraphSchema_K2::PC_Struct;
	PosePin.PinSubCategoryObject = TBaseStructure<FTransform>::Get();
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, SamplePose, PosePin);
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, AppliedPose, PosePin);
	Blueprint->GeneratedClass->GetDefaultObject<AHUICRSyncHMDPawn>()->SharedPayloadStruct = PayloadType;
	UEdGraph* Graph = Blueprint->UbergraphPages[0];
	auto Add = [Graph](UK2Node* Node)
	{
		Graph->AddNode(Node, false, false);
		Node->CreateNewGuid();
		Node->AllocateDefaultPins();
	};
	auto* Writer = NewObject<UK2Node_WriteSyncPayload>(Graph);
	auto* Reader = NewObject<UK2Node_ReadSyncPayload>(Graph);
	auto* Source = NewObject<UK2Node_SetSyncFieldSource>(Graph);
	for (auto* Node : TArray<UK2Node_HUICRSyncPayload*>{ Writer, Reader, Source })
	{
		if (!TestTrue(TEXT("Pawn graph admits the payload node"), Node->IsCompatibleWithGraph(Graph))) { return false; }
		Add(Node);
	}
	FProperty* Camera = nullptr;
	for (TFieldIterator<FProperty> It(PayloadType); It; ++It)
	{
		if (!HUICRSyncFields::IsSupportedField(**It)) { continue; }
		const FName Pin = UK2Node_HUICRSyncPayload::MemberPinName(It->GetFName());
		if (!TestTrue(TEXT("Every supported tracking field has write, read and apply pins"),
			Writer->FindPin(Pin, EGPD_Input) && Reader->FindPin(Pin, EGPD_Output) &&
			Reader->FindPin(UK2Node_HUICRSyncPayload::FieldExecPinName(It->GetFName()), EGPD_Output))) { return false; }
		if (It->GetAuthoredName().StartsWith(TEXT("Camera"))) { Camera = *It; }
	}
	if (!TestNotNull(TEXT("Tracking struct contains its camera transform"), Camera)) { return false; }
	const auto* Schema = GetDefault<UEdGraphSchema_K2>();
	Schema->TrySetDefaultValue(*Source->FindPinChecked(TEXT("Field")), Camera->GetAuthoredName());
	Schema->TrySetDefaultValue(*Source->FindPinChecked(TEXT("Source")), TEXT("UseSpecifiedValue"));
	const FName CameraPin = UK2Node_HUICRSyncPayload::MemberPinName(Camera->GetFName());
	const FName ApplyPin = UK2Node_HUICRSyncPayload::FieldExecPinName(Camera->GetFName());
	auto* GetPose = NewObject<UK2Node_VariableGet>(Graph);
	GetPose->VariableReference.SetSelfMember(SamplePose);
	Add(GetPose);
	auto* SetPose = NewObject<UK2Node_VariableSet>(Graph);
	SetPose->VariableReference.SetSelfMember(AppliedPose);
	Add(SetPose);
	int32 EventY = 0;
	auto* Prepare = FKismetEditorUtilities::AddDefaultEventNode(Blueprint, Graph, TEXT("OnPrepareSyncFields"), PawnClass, EventY);
	auto* Receive = FKismetEditorUtilities::AddDefaultEventNode(Blueprint, Graph, TEXT("OnSyncFieldsReceived"), PawnClass, EventY);
	auto* Publish = NewObject<UK2Node_CustomEvent>(Graph);
	Publish->CustomFunctionName = TEXT("SpecifyPose");
	Add(Publish);
	auto Wire = [this, Schema](UEdGraphPin* From, UEdGraphPin* To)
	{
		return TestTrue(TEXT("The Pawn graph accepts this connection"), From && To && Schema->TryCreateConnection(From, To));
	};
	const FName Then = UEdGraphSchema_K2::PN_Then, Exec = UEdGraphSchema_K2::PN_Execute;
	if (!TestNotNull(TEXT("Pawn prepare event"), Prepare) || !TestNotNull(TEXT("Pawn receive event"), Receive) ||
		!Wire(Prepare->FindPin(Then), Writer->FindPin(Exec)) || !Wire(GetPose->FindPin(SamplePose), Writer->FindPin(CameraPin)) ||
		!Wire(Receive->FindPin(Then), Reader->FindPin(Exec)) || !Wire(Reader->FindPin(ApplyPin), SetPose->FindPin(Exec)) ||
		!Wire(Reader->FindPin(CameraPin), SetPose->FindPin(AppliedPose)) ||
		!Wire(Publish->FindPin(Then), Source->FindPin(Exec)) || !Wire(GetPose->FindPin(SamplePose), Source->FindPin(TEXT("Value"))))
	{
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
	if (!TestTrue(TEXT("Pawn self-targets compile for write, read and field source"), Blueprint->Status != BS_Error)) { return false; }
	auto* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("Blueprint test world"), World)) { return false; }
	ON_SCOPE_EXIT { World->DestroyWorld(false); };
	FEditorScriptExecutionGuard AllowScript;
	auto* Pawn = World->SpawnActor<AHUICRSyncHMDPawn>(Blueprint->GeneratedClass);
	if (!TestNotNull(TEXT("Compiled pawn"), Pawn)) { return false; }
	auto* Endpoint = Pawn->EnsurePawnSyncEndpoint();
	if (!TestNotNull(TEXT("The Pawn's local payload endpoint"), Endpoint)) { return false; }
	const FTransform Expected(FRotator(10, 45, 0), FVector(15, 22, 105));
	const auto* PreparedProperty = FindFProperty<FStructProperty>(Blueprint->GeneratedClass, SamplePose);
	const auto* AppliedProperty = FindFProperty<FStructProperty>(Blueprint->GeneratedClass, AppliedPose);
	if (!TestNotNull(TEXT("Compiled pose input"), PreparedProperty) || !TestNotNull(TEXT("Compiled pose output"), AppliedProperty)) { return false; }
	*PreparedProperty->ContainerPtrToValuePtr<FTransform>(Pawn) = Expected;
	Endpoint->OnPrepareSyncFields({ FName(*Camera->GetAuthoredName()) });
	TArray<uint8> Bytes;
	if (!TestTrue(TEXT("The Pawn prepare event writes its camera through the compiled node"),
		Endpoint->GetPreparedSyncField(Camera->GetFName(), Bytes))) { return false; }
	if (!TestTrue(TEXT("Confirmed camera reaches the Pawn endpoint"), Endpoint->ApplyConfirmedSyncField(Camera->GetFName(), Bytes, true))) { return false; }
	Endpoint->OnSyncFieldsReceived();
	const auto* Applied = AppliedProperty->ContainerPtrToValuePtr<FTransform>(Pawn);
	TestTrue(TEXT("The actual Apply Camera Transform exec pin updates the Pawn"), Applied->Equals(Expected));
	return !HasAnyErrors();
}

#endif
