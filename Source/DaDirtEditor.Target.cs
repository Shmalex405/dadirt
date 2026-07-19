using UnrealBuildTool;
using System.Collections.Generic;

public class DaDirtEditorTarget : TargetRules
{
	public DaDirtEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("DaDirt");
	}
}
