#include "DirtSimulation.h"

#include "GlobalShader.h"
#include "RHIStaticStates.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderCompilerCore.h"
#include "ShaderParameterStruct.h"

namespace
{
	constexpr int32 GDirtThreadGroupSize = 8;

	/** Shared boilerplate: every dirt pass is a compute shader over the whole grid. */
	class FDirtPassCS : public FGlobalShader
	{
	public:
		FDirtPassCS() = default;
		FDirtPassCS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
			: FGlobalShader(Initializer)
		{
		}

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}

		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
												 FShaderCompilerEnvironment& OutEnvironment)
		{
			FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			OutEnvironment.SetDefine(TEXT("DIRT_THREADGROUP_SIZE"), GDirtThreadGroupSize);
		}
	};
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

class FDirtInitCS : public FDirtPassCS
{
public:
	DECLARE_GLOBAL_SHADER(FDirtInitCS);
	SHADER_USE_PARAMETER_STRUCT(FDirtInitCS, FDirtPassCS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntPoint, DirtResolution)
		SHADER_PARAMETER_TEXTURE(Texture2D<float>, DirtBaseHeight)
		SHADER_PARAMETER_TEXTURE(Texture2D<float4>, DirtInitialState)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, DirtStateIn)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DirtStateOut)
	END_SHADER_PARAMETER_STRUCT()

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
											 FShaderCompilerEnvironment& OutEnvironment)
	{
		FDirtPassCS::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("DIRT_INIT_PASS"), 1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FDirtInitCS, "/DaDirt/Private/DirtSim.usf", "MainInitCS", SF_Compute);

// ---------------------------------------------------------------------------
// Brush
// ---------------------------------------------------------------------------

class FDirtBrushCS : public FDirtPassCS
{
public:
	DECLARE_GLOBAL_SHADER(FDirtBrushCS);
	SHADER_USE_PARAMETER_STRUCT(FDirtBrushCS, FDirtPassCS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntPoint, DirtResolution)
		SHADER_PARAMETER(int32, DirtStrokeCount)
		SHADER_PARAMETER_ARRAY(FVector4f, DirtStrokeA, [DirtSim::MaxStrokesPerPass])
		SHADER_PARAMETER_ARRAY(FVector4f, DirtStrokeB, [DirtSim::MaxStrokesPerPass])
		SHADER_PARAMETER_ARRAY(FVector4f, DirtStrokeC, [DirtSim::MaxStrokesPerPass])
		SHADER_PARAMETER_TEXTURE(Texture2D<float>, DirtBaseHeight)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, DirtStateIn)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DirtStateOut)
	END_SHADER_PARAMETER_STRUCT()

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
											 FShaderCompilerEnvironment& OutEnvironment)
	{
		FDirtPassCS::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("DIRT_BRUSH_PASS"), 1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FDirtBrushCS, "/DaDirt/Private/DirtSim.usf", "MainBrushCS", SF_Compute);

// ---------------------------------------------------------------------------
// Slump
// ---------------------------------------------------------------------------

class FDirtSlumpCS : public FDirtPassCS
{
public:
	DECLARE_GLOBAL_SHADER(FDirtSlumpCS);
	SHADER_USE_PARAMETER_STRUCT(FDirtSlumpCS, FDirtPassCS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntPoint, DirtResolution)
		SHADER_PARAMETER(float, DirtTexelSizeCm)
		SHADER_PARAMETER(float, DirtLooseReposeDeg)
		SHADER_PARAMETER(float, DirtPackedReposeDeg)
		SHADER_PARAMETER(float, DirtSuctionCohesionKPa)
		SHADER_PARAMETER(float, DirtPackedCohesionKPa)
		SHADER_PARAMETER(float, DirtUnitWeightKNm3)
		SHADER_PARAMETER(float, DirtSaturationFrictionLoss)
		SHADER_PARAMETER(float, DirtMaxCohesiveHeightCm)
		SHADER_PARAMETER(float, DirtSlumpRate)
		SHADER_PARAMETER(float, DirtLooseningRate)
		SHADER_PARAMETER(float, DirtLooseningScaleCm)
		SHADER_PARAMETER_TEXTURE(Texture2D<float>, DirtBaseHeight)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, DirtStateIn)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DirtStateOut)
	END_SHADER_PARAMETER_STRUCT()

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
											 FShaderCompilerEnvironment& OutEnvironment)
	{
		FDirtPassCS::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("DIRT_SLUMP_PASS"), 1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FDirtSlumpCS, "/DaDirt/Private/DirtSim.usf", "MainSlumpCS", SF_Compute);

// ---------------------------------------------------------------------------
// Resolve
// ---------------------------------------------------------------------------

class FDirtResolveCS : public FDirtPassCS
{
public:
	DECLARE_GLOBAL_SHADER(FDirtResolveCS);
	SHADER_USE_PARAMETER_STRUCT(FDirtResolveCS, FDirtPassCS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntPoint, DirtResolution)
		SHADER_PARAMETER(float, DirtTexelSizeCm)
		SHADER_PARAMETER(float, DirtLooseReposeDeg)
		SHADER_PARAMETER(float, DirtPackedReposeDeg)
		SHADER_PARAMETER(float, DirtSuctionCohesionKPa)
		SHADER_PARAMETER(float, DirtPackedCohesionKPa)
		SHADER_PARAMETER(float, DirtUnitWeightKNm3)
		SHADER_PARAMETER(float, DirtSaturationFrictionLoss)
		SHADER_PARAMETER(float, DirtMaxCohesiveHeightCm)
		SHADER_PARAMETER(int32, DirtDebugMode)
		SHADER_PARAMETER(float, DirtDebugLayerRangeCm)
		SHADER_PARAMETER_TEXTURE(Texture2D<float>, DirtBaseHeight)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, DirtStateIn)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DirtDisplayOut)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DirtNormalOut)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DirtDebugOut)
	END_SHADER_PARAMETER_STRUCT()

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
											 FShaderCompilerEnvironment& OutEnvironment)
	{
		FDirtPassCS::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("DIRT_RESOLVE_PASS"), 1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FDirtResolveCS, "/DaDirt/Private/DirtSim.usf", "MainResolveCS", SF_Compute);

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

void DirtSim::Execute_RenderThread(FRHICommandListImmediate& RHICmdList, const FDirtSimFrame& Frame)
{
	check(IsInRenderingThread());

	if (!Frame.IsValid() || Frame.Resolution.X <= 0 || Frame.Resolution.Y <= 0)
	{
		return;
	}

	// The Dirtbox already resolved these to RHI textures on the render thread.
	FRHITexture* BaseHeightRHI = Frame.BaseHeight;
	FRHITexture* InitialStateRHI = Frame.InitialState;
	FRHITexture* StateARHI = Frame.StateA;
	FRHITexture* StateBRHI = Frame.StateB;
	FRHITexture* DisplayRHI = Frame.Display;
	FRHITexture* NormalRHI = Frame.NormalOut;
	FRHITexture* DebugRHI = Frame.DebugOut;

	FRDGBuilder GraphBuilder(RHICmdList);

	FRDGTextureRef StateA = GraphBuilder.RegisterExternalTexture(
		CreateRenderTarget(StateARHI, TEXT("DirtStateA")));
	FRDGTextureRef StateB = GraphBuilder.RegisterExternalTexture(
		CreateRenderTarget(StateBRHI, TEXT("DirtStateB")));
	FRDGTextureRef DisplayTex = GraphBuilder.RegisterExternalTexture(
		CreateRenderTarget(DisplayRHI, TEXT("DirtDisplay")));
	FRDGTextureRef NormalTex = GraphBuilder.RegisterExternalTexture(
		CreateRenderTarget(NormalRHI, TEXT("DirtNormal")));
	FRDGTextureRef DebugTex = GraphBuilder.RegisterExternalTexture(
		CreateRenderTarget(DebugRHI, TEXT("DirtDebug")));

	const FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(
		FIntPoint(Frame.Resolution.X, Frame.Resolution.Y), GDirtThreadGroupSize);

	// Every pass reads one state texture and writes the other. Track which one
	// currently holds the live state.
	FRDGTextureRef Current = StateA;
	FRDGTextureRef Other = StateB;

	const auto Flip = [&Current, &Other]()
	{
		Swap(Current, Other);
	};

	// --- init --------------------------------------------------------------
	if (Frame.bReinitialise)
	{
		FDirtInitCS::FParameters* Params = GraphBuilder.AllocParameters<FDirtInitCS::FParameters>();
		Params->DirtResolution = Frame.Resolution;
		Params->DirtBaseHeight = BaseHeightRHI;
		Params->DirtInitialState = InitialStateRHI;
		Params->DirtStateIn = Current;          // unused by the pass, but must be bound
		Params->DirtStateOut = GraphBuilder.CreateUAV(Other);

		TShaderMapRef<FDirtInitCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("DirtInit"),
									 Shader, Params, GroupCount);
		Flip();
	}

	// --- brush strokes, batched ------------------------------------------------
	for (int32 First = 0; First < Frame.Strokes.Num(); First += DirtSim::MaxStrokesPerPass)
	{
		const int32 Count = FMath::Min(DirtSim::MaxStrokesPerPass, Frame.Strokes.Num() - First);

		FDirtBrushCS::FParameters* Params = GraphBuilder.AllocParameters<FDirtBrushCS::FParameters>();
		Params->DirtResolution = Frame.Resolution;
		Params->DirtStrokeCount = Count;
		for (int32 K = 0; K < Count; ++K)
		{
			const FDirtBrushStroke& Stroke = Frame.Strokes[First + K];
			Params->DirtStrokeA[K] = FVector4f(Stroke.CenterTexel.X, Stroke.CenterTexel.Y,
											   Stroke.CoreRadiusTexels, Stroke.RimRadiusTexels);
			Params->DirtStrokeB[K] = FVector4f(Stroke.CoreNorm, Stroke.RimNorm, Stroke.Amount, Stroke.Disturb);
			Params->DirtStrokeC[K] = FVector4f(static_cast<float>(Stroke.Mode), 0.0f, 0.0f, 0.0f);
		}
		Params->DirtBaseHeight = BaseHeightRHI;
		Params->DirtStateIn = Current;
		Params->DirtStateOut = GraphBuilder.CreateUAV(Other);

		TShaderMapRef<FDirtBrushCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("DirtBrush"),
									 Shader, Params, GroupCount);
		Flip();
	}

	// --- slump iterations --------------------------------------------------
	for (int32 Iteration = 0; Iteration < Frame.SlumpIterations; ++Iteration)
	{
		FDirtSlumpCS::FParameters* Params = GraphBuilder.AllocParameters<FDirtSlumpCS::FParameters>();
		Params->DirtResolution = Frame.Resolution;
		Params->DirtTexelSizeCm = Frame.TexelSizeCm;
		Params->DirtLooseReposeDeg = Frame.LooseReposeDeg;
		Params->DirtPackedReposeDeg = Frame.PackedReposeDeg;
		Params->DirtSuctionCohesionKPa = Frame.SuctionCohesionKPa;
		Params->DirtPackedCohesionKPa = Frame.PackedCohesionKPa;
		Params->DirtUnitWeightKNm3 = Frame.UnitWeightKNm3;
		Params->DirtSaturationFrictionLoss = Frame.SaturationFrictionLoss;
		Params->DirtMaxCohesiveHeightCm = Frame.MaxCohesiveHeightCm;
		Params->DirtSlumpRate = Frame.SlumpRate;
		Params->DirtLooseningRate = Frame.LooseningRate;
		Params->DirtLooseningScaleCm = Frame.LooseningScaleCm;
		Params->DirtBaseHeight = BaseHeightRHI;
		Params->DirtStateIn = Current;
		Params->DirtStateOut = GraphBuilder.CreateUAV(Other);

		TShaderMapRef<FDirtSlumpCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("DirtSlump"),
									 Shader, Params, GroupCount);
		Flip();
	}

	// --- resolve -----------------------------------------------------------
	{
		FDirtResolveCS::FParameters* Params = GraphBuilder.AllocParameters<FDirtResolveCS::FParameters>();
		Params->DirtResolution = Frame.Resolution;
		Params->DirtTexelSizeCm = Frame.TexelSizeCm;
		Params->DirtLooseReposeDeg = Frame.LooseReposeDeg;
		Params->DirtPackedReposeDeg = Frame.PackedReposeDeg;
		Params->DirtSuctionCohesionKPa = Frame.SuctionCohesionKPa;
		Params->DirtPackedCohesionKPa = Frame.PackedCohesionKPa;
		Params->DirtUnitWeightKNm3 = Frame.UnitWeightKNm3;
		Params->DirtSaturationFrictionLoss = Frame.SaturationFrictionLoss;
		Params->DirtMaxCohesiveHeightCm = Frame.MaxCohesiveHeightCm;
		Params->DirtDebugMode = Frame.DebugMode;
		Params->DirtDebugLayerRangeCm = Frame.DebugLayerRangeCm;
		Params->DirtBaseHeight = BaseHeightRHI;
		Params->DirtStateIn = Current;
		Params->DirtDisplayOut = GraphBuilder.CreateUAV(DisplayTex);
		Params->DirtNormalOut = GraphBuilder.CreateUAV(NormalTex);
		Params->DirtDebugOut = GraphBuilder.CreateUAV(DebugTex);

		TShaderMapRef<FDirtResolveCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("DirtResolve"),
									 Shader, Params, GroupCount);
	}

	// The pass count varies with how many strokes came in this step, so the live
	// state does not always land back in StateA. Copying it back costs one 8 MB
	// blit and means the next step can always assume StateA is the input — much
	// less to get wrong than tracking parity across frames.
	if (Current != StateA)
	{
		AddCopyTexturePass(GraphBuilder, Current, StateA);
	}

	GraphBuilder.Execute();
}
