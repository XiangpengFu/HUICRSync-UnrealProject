using UnrealBuildTool;

public class HUICRSyncRuntime : ModuleRules
{
	public HUICRSyncRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Sockets",
			"Networking",
			"StructUtils"
		});

		if (Target.Platform == UnrealTargetPlatform.Win64 || Target.Platform == UnrealTargetPlatform.Linux)
		{
			PublicDependencyModuleNames.Add("DisplayCluster");
			PublicDefinitions.Add("HUICR_WITH_NDISPLAY=1");
		}
		else
		{
			PublicDefinitions.Add("HUICR_WITH_NDISPLAY=0");
		}

		PrivateDependencyModuleNames.AddRange(new string[] {});
	}
}
