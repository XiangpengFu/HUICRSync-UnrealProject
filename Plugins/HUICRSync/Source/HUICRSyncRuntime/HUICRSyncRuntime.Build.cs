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
			"UMG",
			"SlateCore",
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

		if (Target.Platform == UnrealTargetPlatform.Android)
		{
			// MRUK's public anchor and subsystem headers include types from these private SDK dependencies.
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"MRUtilityKit", "OculusXRAnchors", "ProceduralMeshComponent", "HeadMountedDisplay", "Json", "AndroidPermission"
			});
		}
	}
}
