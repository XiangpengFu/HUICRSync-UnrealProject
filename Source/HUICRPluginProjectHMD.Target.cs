// HUICRPluginProjectHMD.Target.cs

using UnrealBuildTool;
using System.Collections.Generic;

public class HUICRPluginProjectHMDTarget : TargetRules
{
	public HUICRPluginProjectHMDTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V5;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_6;

		ExtraModuleNames.Add("HUICRPluginProject");

		EnablePlugins.Add("OculusXR");
		EnablePlugins.Add("OculusInteraction");
		EnablePlugins.Add("OpenXRHandTracking");

		DisablePlugins.Add("nDisplay");
		DisablePlugins.Add("Switchboard");
	}
}