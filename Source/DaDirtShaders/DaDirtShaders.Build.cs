using UnrealBuildTool;

// Early-loading module for everything the dirt simulation needs on the GPU side:
//
//   * maps our Shaders/ folder to the virtual shader path "/DaDirt"
//   * declares the dirt compute shaders (DirtSimulation.cpp) and dispatches them
//
// It has to load at PostConfigInit — earlier than the game module — because the
// engine builds its global shader map before Default-phase modules load, and any
// global shader registered after that point asserts. That is also why this module
// must not depend on Engine: at PostConfigInit, Engine has not started yet.
public class DaDirtShaders : ModuleRules
{
	public DaDirtShaders(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"RHI",
			"RenderCore"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Projects"
		});
	}
}
