using UnrealBuildTool;

// Tiny module whose only job is to map our Shaders/ folder to a virtual shader
// path ("/DaDirt") before the engine starts compiling shaders. It has to load at
// PostConfigInit — earlier than the game module — or the engine will not know
// where our .usf files live.
public class DaDirtShaders : ModuleRules
{
	public DaDirtShaders(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"Projects",
			"RenderCore"
		});
	}
}
