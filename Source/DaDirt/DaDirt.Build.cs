using UnrealBuildTool;

public class DaDirt : ModuleRules
{
	public DaDirt(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"Niagara",
			"ProceduralMeshComponent",
			"DaDirtShaders"
		});

		// Render-graph / compute-shader plumbing for the dirt simulation.
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"RenderCore",
			"RHI",
			"Projects"
		});
	}
}
