#if WITH_DEV_AUTOMATION_TESTS

#include "HUICRSyncSpawnNodeTest.h"

#include "HUICRSyncFieldTestActor.h"
#include "HUICRSyncHMDManagerBase.h"
#include "HUICRSyncPayloadLibrary.h"
#include "HUICRSyncSubsystem.h"
#include "K2Node_HUICRSyncPayload.h"
#include "K2Node_SpawnSyncActor.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EngineUtils.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Misc/AutomationTest.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "UObject/UnrealType.h"

namespace HUICRSyncSpawnNodeTest
{
bool Verify(FAutomationTestBase& Test, UWorld& World, UHUICRSyncSubsystem& PC, UHUICRSyncSubsystem& Host,
	UHUICRSyncSubsystem& Guest, TFunctionRef<bool(const TCHAR*, TFunctionRef<bool()>)> WaitFor)
{
	auto* Blueprint = FKismetEditorUtilities::CreateBlueprint(AHUICRSyncActor::StaticClass(), GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UBlueprint::StaticClass(), TEXT("HUICRSpawnProbe")),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!Blueprint || Blueprint->UbergraphPages.IsEmpty()) { Test.AddError(TEXT("Could not create a spawn probe Blueprint")); return false; }
	const FName RuntimeClass(TEXT("ClassChosenAtRuntime")), RuntimeValues(TEXT("ValuesChosenAtRuntime"));
	FEdGraphPinType ClassType;
	ClassType.PinCategory = UEdGraphSchema_K2::PC_Class;
	ClassType.PinSubCategoryObject = AHUICRSyncActor::StaticClass();
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, RuntimeClass, ClassType);
	FEdGraphPinType StructType;
	StructType.PinCategory = UEdGraphSchema_K2::PC_Struct;
	StructType.PinSubCategoryObject = FInstancedStruct::StaticStruct();
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, RuntimeValues, StructType);
	auto* UserSchema = LoadObject<UScriptStruct>(nullptr, TEXT("/HUICRSync/BlueprintStructs/BPS_TestActor.BPS_TestActor"));
	if (!Test.TestNotNull(TEXT("The example's Blueprint-defined struct loads"), UserSchema)) { return false; }
	Blueprint->GeneratedClass->GetDefaultObject<AHUICRSyncActor>()->SharedPayloadStruct = UserSchema;
	UEdGraph* Graph = Blueprint->UbergraphPages[0];
	const auto* Schema = GetDefault<UEdGraphSchema_K2>();
	auto Add = [Graph](UK2Node* Node)
	{
		Graph->AddNode(Node, false, false);
		Node->CreateNewGuid();
		Node->AllocateDefaultPins();
	};
	auto Event = [&](FName Name)
	{
		auto* Node = NewObject<UK2Node_CustomEvent>(Graph);
		Node->CustomFunctionName = Name;
		Add(Node);
		return Node->FindPin(UEdGraphSchema_K2::PN_Then);
	};
	auto Get = [&](FName Name)
	{
		auto* Node = NewObject<UK2Node_VariableGet>(Graph);
		Node->VariableReference.SetSelfMember(Name);
		Add(Node);
		return Node->FindPin(Name);
	};
	auto Wire = [&](UEdGraphPin* From, UEdGraphPin* To)
	{
		return Test.TestTrue(TEXT("Blueprint pins are compatible"), From && To && Schema->TryCreateConnection(From, To));
	};
	const FName Exec = UEdGraphSchema_K2::PN_Execute, Then = UEdGraphSchema_K2::PN_Then;
	const FName IndexPin = UK2Node_HUICRSyncPayload::MemberPinName(GET_MEMBER_NAME_CHECKED(FHUICRSyncScreenPayload, ScreenID));
	auto* TargetPin = Get(GET_MEMBER_NAME_CHECKED(AHUICRSyncActor, SyncSubsystem));
	auto* InputId = Get(GET_MEMBER_NAME_CHECKED(AHUICRSyncActor, ActorID));
	const FName Omit(TEXT("SpawnWithActorDefaults")), Literal(TEXT("SpawnWithLiteral")), Zero(TEXT("SpawnWithExplicitDefaults")),
		Connected(TEXT("SpawnWithLinkedZero")), Dynamic(TEXT("SpawnWithDynamicClass")), DynamicValues(TEXT("SpawnWithDynamicValues")),
		Split(TEXT("SpawnWithSplitTransform"));
	for (FName Name : { Omit, Literal, Zero, Connected, Dynamic, DynamicValues, Split })
	{
		auto* Spawn = NewObject<UK2Node_SpawnSyncActor>(Graph);
		Add(Spawn);
		auto* ClassPin = Spawn->FindPin(TEXT("SpawnClass"), EGPD_Input);
		if (!Test.TestNotNull(TEXT("Class picker exists"), ClassPin)) { return false; }
		if (Name == Dynamic || Name == DynamicValues)
		{
			if (!Wire(Get(RuntimeClass), ClassPin) || !Wire(TargetPin, Spawn->FindPin(TEXT("Target"))) ||
				!Wire(Event(Name), Spawn->FindPin(Exec))) { return false; }
			if (Name == DynamicValues && !Wire(Get(RuntimeValues), Spawn->FindPin(TEXT("InitialValues")))) { return false; }
			continue;
		}
		if (Name == Literal)
		{
			Schema->TrySetDefaultObject(*ClassPin, Blueprint->GeneratedClass);
			for (TFieldIterator<FProperty> It(UserSchema); It; ++It)
			{
				const auto* Member = Spawn->FindPin(UK2Node_HUICRSyncPayload::MemberPinName(It->GetFName()), EGPD_Input);
				FEdGraphPinType Expected;
				if (Schema->ConvertPropertyToPinType(*It, Expected) &&
					!Test.TestTrue(TEXT("User-defined struct members have correctly typed input pins"), Member && Member->PinType == Expected))
				{
					return false;
				}
			}
		}
		Schema->TrySetDefaultObject(*Spawn->FindPinChecked(TEXT("SpawnClass")), AHUICRSyncFieldTestActor::StaticClass());
		auto* Index = Spawn->FindPin(IndexPin, EGPD_Input);
		if (!Test.TestTrue(TEXT("Selecting the actor exposes the reflected integer field"),
			Index && Index->PinType.PinCategory == UEdGraphSchema_K2::PC_Int)) { return false; }
		if (Name == Literal)
		{
			Schema->TrySetDefaultValue(*Index, TEXT("47"));
		}
		if (Name == Zero)
		{
			auto* Explicit = Spawn->FindPin(TEXT("bUseStructDefaults"), EGPD_Input);
			if (!Test.TestNotNull(TEXT("Intentional zero/default values can be requested"), Explicit)) { return false; }
			Schema->TrySetDefaultValue(*Explicit, TEXT("true"));
		}
		if (Name == Connected && !Wire(InputId, Index)) { return false; }
		if (Name == Omit || Name == Split)
		{
			auto* Pose = Spawn->FindPinChecked(UK2Node_HUICRSyncPayload::MemberPinName(GET_MEMBER_NAME_CHECKED(FHUICRSyncScreenPayload, Transform)));
			Schema->SplitPin(Pose, false);
			UEdGraphPin** Position = Pose->SubPins.FindByPredicate([](const UEdGraphPin* Pin) { return Pin->PinName.ToString().EndsWith(TEXT("_Location")); });
			if (!Test.TestNotNull(TEXT("A transform can split into position, rotation and scale"), Position)) { return false; }
			if (Name == Split) { Schema->TrySetDefaultValue(**Position, TEXT("23,0,0")); }
		}
		if (!Wire(TargetPin, Spawn->FindPin(TEXT("Target"))) || !Wire(Event(Name), Spawn->FindPin(Exec))) { return false; }
	}

	// The sibling array has an authored default which cannot be represented by an unlinked K2 array pin.
	auto* WithArray = FStructureEditorUtils::CreateUserDefinedStruct(GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UUserDefinedStruct::StaticClass(), TEXT("HUICRSpawnDefaults")), RF_Transient);
	FEdGraphPinType IntArray;
	IntArray.PinCategory = UEdGraphSchema_K2::PC_Int;
	IntArray.ContainerType = EPinContainerType::Array;
	if (!Test.TestTrue(TEXT("Can author the initial array schema"), WithArray && FStructureEditorUtils::AddVariable(WithArray, IntArray))) { return false; }
	const FGuid ArrayGuid = FStructureEditorUtils::GetVarDesc(WithArray).Last().VarGuid;
	if (!Test.TestTrue(TEXT("User-defined struct accepts an authored array default"),
		FStructureEditorUtils::ChangeVariableDefaultValue(WithArray, ArrayGuid, TEXT("(13,29)")))) { return false; }
	const FName FlagField = FStructureEditorUtils::GetVarDesc(WithArray)[0].VarName;
	const FName ArrayField = FStructureEditorUtils::GetVarDesc(WithArray).Last().VarName;
	const auto* AuthoredFlag = FindFProperty<FBoolProperty>(WithArray, FlagField);
	const auto* AuthoredArray = FindFProperty<FArrayProperty>(WithArray, ArrayField);
	if (!Test.TestTrue(TEXT("The fixture's own struct constructor supplies the authored array"),
		AuthoredFlag && AuthoredArray && WithArray->GetDefaultInstance() &&
		*AuthoredArray->ContainerPtrToValuePtr<TArray<int32>>(WithArray->GetDefaultInstance()) == TArray<int32>{13, 29})) { return false; }
	FInstancedStruct Reference(WithArray);
	AuthoredFlag->SetPropertyValue_InContainer(Reference.GetMutableMemory(), true);
	TArray<uint8> BaselineBytes;
	FInstancedStruct Baseline;
	if (!Test.TestTrue(TEXT("Initial Values codec preserves the nonempty array in a reflected struct"),
		UHUICRSyncPayloadLibrary::WriteInitialValues(BaselineBytes, WithArray, Reference) &&
		UHUICRSyncPayloadLibrary::ReadInitialValues(BaselineBytes, WithArray, Baseline) &&
		*AuthoredArray->ContainerPtrToValuePtr<TArray<int32>>(Baseline.GetMemory()) == TArray<int32>{13, 29})) { return false; }
	auto* ArrayBP = FKismetEditorUtilities::CreateBlueprint(AHUICRSyncActor::StaticClass(), GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UBlueprint::StaticClass(), TEXT("HUICRSpawnArrayActor")),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!Test.TestNotNull(TEXT("Transient actor for the authored array"), ArrayBP)) { return false; }
	ArrayBP->GeneratedClass->GetDefaultObject<AHUICRSyncActor>()->SharedPayloadStruct = WithArray;
	// A CDO edit made through the Details panel normally refreshes this list for new BP instances.
	CastChecked<UBlueprintGeneratedClass>(ArrayBP->GeneratedClass)->UpdateCustomPropertyListForPostConstruction();
	const FName KeepArray(TEXT("SpawnWithUntouchedArray"));
	auto* ArraySpawn = NewObject<UK2Node_SpawnSyncActor>(Graph);
	Add(ArraySpawn);
	Schema->TrySetDefaultObject(*ArraySpawn->FindPinChecked(TEXT("SpawnClass")), ArrayBP->GeneratedClass);
	auto* FlagInput = ArraySpawn->FindPin(UK2Node_HUICRSyncPayload::MemberPinName(FlagField));
	if (!Test.TestNotNull(TEXT("Authored Boolean pin"), FlagInput)) { return false; }
	Schema->TrySetDefaultValue(*FlagInput, TEXT("true"));
	if (!Wire(TargetPin, ArraySpawn->FindPin(TEXT("Target"))) || !Wire(Event(KeepArray), ArraySpawn->FindPin(Exec))) { return false; }

	// Exercise saved, explicitly placed function call nodes even though new menu entries are hidden.
	auto LegacyCall = [&](FName Name)
	{
		auto* Function = UHUICRSyncPayloadLibrary::StaticClass()->FindFunctionByName(Name);
		if (!Test.TestTrue(TEXT("A legacy function is callable by saved nodes but hidden from the action menu"),
			Function && Function->HasAnyFunctionFlags(FUNC_BlueprintCallable) && !Schema->CanUserKismetCallFunction(Function)))
		{
			return static_cast<UK2Node_CallFunction*>(nullptr);
		}
		auto* Node = NewObject<UK2Node_CallFunction>(Graph);
		Node->SetFromFunction(Function);
		Add(Node);
		Schema->TrySetDefaultObject(*Node->FindPinChecked(TEXT("StructType")), FHUICRSyncScreenPayload::StaticStruct());
		return Node;
	};
	auto* Read = LegacyCall(GET_FUNCTION_NAME_CHECKED(UHUICRSyncPayloadLibrary, ReadInstancedStruct));
	auto* Write = LegacyCall(GET_FUNCTION_NAME_CHECKED(UHUICRSyncPayloadLibrary, WriteInstancedStruct));
	const FName RoundTrip(TEXT("LegacyRoundTrip"));
	auto* Save = NewObject<UK2Node_VariableSet>(Graph);
	const FName SavedBytes = GET_MEMBER_NAME_CHECKED(AHUICRSyncActor, ActorSendPayloadData);
	Save->VariableReference.SetSelfMember(SavedBytes);
	Add(Save);
	if (!Read || !Write || !Wire(Event(RoundTrip), Read->FindPin(Exec)) ||
		!Wire(Get(GET_MEMBER_NAME_CHECKED(AHUICRSyncActor, ActorInitPayloadData)), Read->FindPin(TEXT("InBytes"))) ||
		!Wire(Read->FindPin(Then), Write->FindPin(Exec)) || !Wire(Read->FindPin(TEXT("OutStruct")), Write->FindPin(TEXT("InStruct"))) ||
		!Wire(Write->FindPin(Then), Save->FindPin(Exec)) || !Wire(Write->FindPin(TEXT("OutBytes")), Save->FindPin(SavedBytes)))
	{
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
	if (!Test.TestTrue(TEXT("Modern spawn and already placed legacy read/write nodes compile together"), Blueprint->Status != BS_Error)) { return false; }
	auto* Caller = World.SpawnActor<AHUICRSyncActor>(Blueprint->GeneratedClass);
	if (!Test.TestNotNull(TEXT("Compiled spawner exists"), Caller)) { return false; }
	const auto* ClassProperty = FindFProperty<FClassProperty>(Blueprint->GeneratedClass, RuntimeClass);
	const auto* ValuesProperty = FindFProperty<FStructProperty>(Blueprint->GeneratedClass, RuntimeValues);
	if (!Test.TestNotNull(TEXT("Dynamic spawn class property"), ClassProperty) ||
		!Test.TestNotNull(TEXT("Dynamic spawn struct property"), ValuesProperty)) { return false; }
	ClassProperty->SetObjectPropertyValue_InContainer(Caller, AHUICRSyncFieldTestActor::StaticClass());
	Caller->ActorID = 0;
	auto LegacyValue = FInstancedStruct::Make<FHUICRSyncScreenPayload>();
	LegacyValue.GetMutable<FHUICRSyncScreenPayload>().ScreenID = 83;
	*ValuesProperty->ContainerPtrToValuePtr<FInstancedStruct>(Caller) = LegacyValue;
	if (!UHUICRSyncPayloadLibrary::WriteInstancedStruct(Caller->ActorInitPayloadData, FHUICRSyncScreenPayload::StaticStruct(), LegacyValue)) { return false; }
	Caller->ProcessEvent(Caller->FindFunctionChecked(RoundTrip), nullptr);
	if (!Test.TestTrue(TEXT("Existing Blueprint calls still round-trip the same bytes"),
		!Caller->ActorInitPayloadData.IsEmpty() && Caller->ActorSendPayloadData == Caller->ActorInitPayloadData)) { return false; }
	Caller->SyncSubsystem = &PC;
	Caller->ProcessEvent(Caller->FindFunctionChecked(KeepArray), nullptr);
	int32 LastPCId = INDEX_NONE;
	for (TActorIterator<AHUICRSyncActor> It(&World, ArrayBP->GeneratedClass.Get()); It; ++It)
	{
		LastPCId = It->ActorID;
		FInstancedStruct Payload;
		if (!Test.TestEqual(TEXT("Spawn selected the actor with the test's exact user-defined schema"),
			GetPathNameSafe(It->SharedPayloadStruct), WithArray->GetPathName()) ||
			!Test.TestTrue(TEXT("The authored-array spawn included initial bytes"), !It->ActorInitPayloadData.IsEmpty()) ||
			!Test.TestTrue(TEXT("The actor reads the sent initial bytes under that schema"), It->ReadInitialPayloadInternal(Payload) && Payload.GetScriptStruct() == WithArray) ||
			!Test.TestTrue(TEXT("The Boolean literal was applied"), AuthoredFlag->GetPropertyValue_InContainer(Payload.GetMemory())) ||
			!Test.TestTrue(TEXT("Overriding the Boolean keeps the sibling array's authored nonempty default"),
				*AuthoredArray->ContainerPtrToValuePtr<TArray<int32>>(Payload.GetMemory()) == TArray<int32>{13, 29})) { return false; }
	}
	if (!Test.TestTrue(TEXT("The explicitly selected transient array actor was spawned"), LastPCId != INDEX_NONE)) { return false; }
	Test.AddExpectedMessage(TEXT("with the supplied Initial Values"), ELogVerbosity::Warning);
	Test.TestFalse(TEXT("An explicit runtime struct of the wrong type fails the spawn"),
		PC.SpawnSyncActorWithInitialValues(ArrayBP->GeneratedClass, FTransform::Identity, false, LegacyValue));
	TArray<uint8> Oversized;
	Oversized.Init(0, 65535);
	Test.AddExpectedMessage(TEXT("initial payload exceeds the managed scene operation limit"), ELogVerbosity::Warning);
	Test.TestNull(TEXT("PC rejects a birth payload too large to mirror"),
		PC.SpawnSyncActorAsAuthority(AHUICRSyncFieldTestActor::StaticClass(), FTransform::Identity, Oversized));

	// The author normally leaves the advanced Target pin disconnected. Exercise self resolution in a real HMD manager graph.
	auto* ManagerBP = FKismetEditorUtilities::CreateBlueprint(AHUICRSyncHMDManagerBase::StaticClass(), GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UBlueprint::StaticClass(), TEXT("HUICRSpawnManager")),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!ManagerBP || ManagerBP->UbergraphPages.IsEmpty()) { Test.AddError(TEXT("Could not create the caller manager")); return false; }
	UEdGraph* ManagerGraph = ManagerBP->UbergraphPages[0];
	const FName ManagerEvent(TEXT("SpawnInMyWorld"));
	auto* Begin = NewObject<UK2Node_CustomEvent>(ManagerGraph);
	Begin->CustomFunctionName = ManagerEvent;
	auto* InWorld = NewObject<UK2Node_SpawnSyncActor>(ManagerGraph);
	for (UK2Node* Node : TArray<UK2Node*>{ Begin, InWorld })
	{
		ManagerGraph->AddNode(Node, false, false);
		Node->CreateNewGuid();
		Node->AllocateDefaultPins();
	}
	Schema->TrySetDefaultObject(*InWorld->FindPinChecked(TEXT("SpawnClass")), AHUICRSyncFieldTestActor::StaticClass());
	Schema->TrySetDefaultValue(*InWorld->FindPinChecked(TEXT("bUseLocalSpace")), TEXT("true"));
	if (!Wire(Begin->FindPin(Then), InWorld->FindPin(Exec))) { return false; }
	FKismetEditorUtilities::CompileBlueprint(ManagerBP, EBlueprintCompileOptions::SkipGarbageCollection);
	if (!Test.TestTrue(TEXT("A manager graph with no Target connection compiles"), ManagerBP->Status != BS_Error)) { return false; }
	auto* Manager = World.SpawnActor<AHUICRSyncHMDManagerBase>(ManagerBP->GeneratedClass);
	if (!Test.TestNotNull(TEXT("Calling HMD manager"), Manager)) { return false; }
	Manager->SyncSubsystem = &Host;

	auto VerifySpawn = [&](FName Name, UHUICRSyncSubsystem& Sender, int32 ExpectedId, double ExpectedX, bool bActorDefaults)
	{
		TSet<AHUICRSyncFieldTestActor*> Before;
		for (TActorIterator<AHUICRSyncFieldTestActor> It(&World); It; ++It) { Before.Add(*It); }
		Caller->SyncSubsystem = &Sender;
		AActor* EventOwner = Name == ManagerEvent ? static_cast<AActor*>(Manager) : static_cast<AActor*>(Caller);
		EventOwner->ProcessEvent(EventOwner->FindFunctionChecked(Name), nullptr);
		const FString Description = FString::Printf(TEXT("%s reaches PC and both HMDs with the right birth values"), *Name.ToString());
		int32 SpawnedId = INDEX_NONE;
		const bool bReceived = WaitFor(*Description, [&]
		{
			for (TActorIterator<AHUICRSyncFieldTestActor> It(&World); It; ++It)
			{
				auto* Actor = *It;
				if (Before.Contains(Actor) || PC.FindSyncActor(Actor->TypeCode, Actor->ActorID) != Actor) { continue; }
				SpawnedId = Actor->ActorID;
				if (Name == ManagerEvent && !Actor->SpawnTransformAtConstruction.Equals(Host.ConvertHMDActorTransformToPC(FTransform::Identity))) { return false; }
				for (const auto* Session : { &PC, &Host, &Guest })
				{
					const auto* Copy = Cast<AHUICRSyncFieldTestActor>(Session->FindSyncActor(Actor->TypeCode, Actor->ActorID));
					FInstancedStruct Initial;
					if (!Copy || !Copy->ReadInitialPayloadInternal(Initial)) { return false; }
					const auto* Values = Initial.GetPtr<FHUICRSyncScreenPayload>();
					if (!Values || Values->ScreenID != ExpectedId || !FMath::IsNearlyEqual(Values->Transform.GetLocation().X, ExpectedX) ||
						(Copy == Actor && Copy->bSetConstructionDefault != bActorDefaults) ||
						(Copy != Actor && Copy->InitialBytesAtConstruction != Actor->ActorInitPayloadData)) { return false; }
				}
				return true;
			}
			return false;
		});
		return bReceived && (Name != Omit || Test.TestEqual(TEXT("Rejected spawns do not consume an actor ID"), SpawnedId, LastPCId + 1));
	};
	if (!(VerifySpawn(Omit, PC, 1, 10, true) && VerifySpawn(Literal, PC, 47, 0, false) && VerifySpawn(Split, Host, 0, 23, false) &&
		VerifySpawn(Zero, Guest, 0, 0, false) && VerifySpawn(Connected, Host, 0, 0, false) &&
		VerifySpawn(Dynamic, PC, 1, 10, true) && VerifySpawn(DynamicValues, Guest, 83, 0, false))) { return false; }
	TGuardValue<FTransform> Offset(Host.HMDToPCTransform, FTransform(FVector(523, 18, -7)));
	return VerifySpawn(ManagerEvent, Host, 1, 10, true);
}
}

#endif
