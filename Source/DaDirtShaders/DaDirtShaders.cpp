#include "DaDirtShaders.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

IMPLEMENT_MODULE(FDaDirtShadersModule, DaDirtShaders);

void FDaDirtShadersModule::StartupModule()
{
	// Everything under <Project>/Shaders/ becomes reachable as "/DaDirt/...".
	// So Shaders/Private/DirtSim.usf is included from C++ as "/DaDirt/DirtSim.usf".
	const FString ShaderDir = FPaths::Combine(FPaths::ProjectDir(), TEXT("Shaders"));
	AddShaderSourceDirectoryMapping(TEXT("/DaDirt"), ShaderDir);
}

void FDaDirtShadersModule::ShutdownModule()
{
}
