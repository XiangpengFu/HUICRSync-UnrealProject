// HUICRPluginProjectPC.Target.cs

using UnrealBuildTool;
using System.Collections.Generic;

public class HUICRPluginProjectPCTarget : TargetRules
{
	public HUICRPluginProjectPCTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V5;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_6;

		ExtraModuleNames.Add("HUICRPluginProject");

		DisablePlugins.Add("OculusXR");
		DisablePlugins.Add("OculusInteraction");
		DisablePlugins.Add("OpenXRHandTracking");
	}
}