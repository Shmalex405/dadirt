using UnrealBuildTool;
using System.Collections.Generic;

public class DaDirtTarget : TargetRules
{
	public DaDirtTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.AddRange(new string[] { "DaDirt", "DaDirtShaders" });
	}
}
