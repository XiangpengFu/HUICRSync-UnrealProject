using UnrealBuildTool;

public class HUICRSyncBlueprintNodes : ModuleRules
{
    public HUICRSyncBlueprintNodes(ReadOnlyTargetRules Target) : base(Target)
    {
        PrivateDependencyModuleNames.AddRange(new[]
        {
            "Core", "CoreUObject", "Engine", "HUICRSyncRuntime", "UnrealEd",
            "BlueprintGraph", "KismetCompiler", "GraphEditor", "Slate", "SlateCore"
        });
    }
}
