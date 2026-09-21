#include "DirtSimulation.h"

#include "GlobalShader.h"
#include "RHIGPUReadback.h"
#include "RHIStaticStates.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderCompilerCore.h"
#include "ShaderParameterStruct.h"

namespace
{
	constexpr int32 GDirtThreadGroupSize = 8;
	constexpr int32 GParcelThreadGroupSize = 64;

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

// Every pass reads these (DirtCommon.ush declares them), so every FParameters
// struct carries them. Bind-by-name means a missing one is a silent zero.
#define DIRT_SHARED_PARAMETERS() \
	SHADER_PARAMETER(FIntPoint, DirtResolution) \
	SHADER_PARAMETER(float, DirtLoosePorosity) \
	SHADER_PARAMETER(float, DirtDensePorosity) \
	SHADER_PARAMETER(float, DirtCompactionDepthCm) \
	SHADER_PARAMETER(float, DirtDeepCompaction) \
	SHADER_PARAMETER_TEXTURE(Texture2D<float>, DirtBaseHeight) \
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, DirtStateIn)

#define DIRT_SOIL_PARAMETERS() \
	SHADER_PARAMETER(float, DirtTexelSizeCm) \
	SHADER_PARAMETER(float, DirtLooseReposeDeg) \
	SHADER_PARAMETER(float, DirtPackedReposeDeg) \
	SHADER_PARAMETER(float, DirtSuctionCohesionKPa) \
	SHADER_PARAMETER(float, DirtPackedCohesionKPa) \
	SHADER_PARAMETER(float, DirtUnitWeightKNm3) \
	SHADER_PARAMETER(float, DirtSaturationFrictionLoss) \
	SHADER_PARAMETER(float, DirtMaxCohesiveHeightCm)

#define DIRT_PARCEL_PARAMETERS() \
	SHADER_PARAMETER(int32, DirtParcelRes) \
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DirtParcelPos) \
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DirtParcelVel) \
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DirtParcelProp) \
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, DirtParcelFree) \
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, DirtParcelCounters) \
	SHADER_PARAMETER(FVector2f, DirtRegionOriginCm) \
	SHADER_PARAMETER(float, DirtTexelSizeCm) \
	SHADER_PARAMETER(float, DirtTexelAreaCm2) \
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, DirtDepositVol) \
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, DirtDepositMoist)

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

class FDirtInitCS : public FDirtPassCS
{
public:
	DECLARE_GLOBAL_SHADER(FDirtInitCS);
	SHADER_USE_PARAMETER_STRUCT(FDirtInitCS, FDirtPassCS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		DIRT_SHARED_PARAMETERS()
		SHADER_PARAMETER_TEXTURE(Texture2D<float4>, DirtInitialState)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DirtStateOut)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, DirtPondOut)
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
// Deposit
// ---------------------------------------------------------------------------

class FDirtDepositCS : public FDirtPassCS
{
public:
	DECLARE_GLOBAL_SHADER(FDirtDepositCS);
	SHADER_USE_PARAMETER_STRUCT(FDirtDepositCS, FDirtPassCS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		DIRT_SHARED_PARAMETERS()
		SHADER_PARAMETER(float, DirtTexelAreaCm2)
		SHADER_PARAMETER(float, DirtDepositCompaction)
		SHADER_PARAMETER(int32, DirtResetLiveMax)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, DirtDepositVol)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, DirtDepositMoist)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, DirtParcelCounters)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DirtStateOut)
	END_SHADER_PARAMETER_STRUCT()

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
											 FShaderCompilerEnvironment& OutEnvironment)
	{
		FDirtPassCS::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("DIRT_DEPOSIT_PASS"), 1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FDirtDepositCS, "/DaDirt/Private/DirtSim.usf", "MainDepositCS", SF_Compute);

// ---------------------------------------------------------------------------
// Brush
// ---------------------------------------------------------------------------

class FDirtBrushCS : public FDirtPassCS
{
public:
	DECLARE_GLOBAL_SHADER(FDirtBrushCS);
	SHADER_USE_PARAMETER_STRUCT(FDirtBrushCS, FDirtPassCS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		DIRT_SHARED_PARAMETERS()
		SHADER_PARAMETER(int32, DirtStrokeCount)
		SHADER_PARAMETER_ARRAY(FVector4f, DirtStrokeA, [DirtSim::MaxStrokesPerPass])
		SHADER_PARAMETER_ARRAY(FVector4f, DirtStrokeB, [DirtSim::MaxStrokesPerPass])
		SHADER_PARAMETER_ARRAY(FVector4f, DirtStrokeC, [DirtSim::MaxStrokesPerPass])
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
		DIRT_SHARED_PARAMETERS()
		DIRT_SOIL_PARAMETERS()
		SHADER_PARAMETER(float, DirtSlumpRate)
		SHADER_PARAMETER(float, DirtLooseningRate)
		SHADER_PARAMETER(float, DirtLooseningScaleCm)
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
// Water
// ---------------------------------------------------------------------------

class FDirtWaterCS : public FDirtPassCS
{
public:
	DECLARE_GLOBAL_SHADER(FDirtWaterCS);
	SHADER_USE_PARAMETER_STRUCT(FDirtWaterCS, FDirtPassCS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		DIRT_SHARED_PARAMETERS()
		SHADER_PARAMETER(float, DirtDt)
		SHADER_PARAMETER(float, DirtRunoffRate)
		SHADER_PARAMETER(float, DirtRainCmPerSec)
		SHADER_PARAMETER(float, DirtInfiltrationCmPerSec)
		SHADER_PARAMETER(float, DirtDrainPerSec)
		SHADER_PARAMETER(float, DirtEvapPerSec)
		SHADER_PARAMETER(float, DirtFieldCapacity)
		SHADER_PARAMETER(float, DirtWetDepthCm)
		SHADER_PARAMETER(float, DirtAmbientMoisture)
		SHADER_PARAMETER(int32, DirtWaterSourceCount)
		SHADER_PARAMETER_ARRAY(FVector4f, DirtWaterSource, [DirtSim::MaxWaterSourcesPerPass])
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, DirtPondIn)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, DirtPondOut)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DirtStateOut)
	END_SHADER_PARAMETER_STRUCT()

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
											 FShaderCompilerEnvironment& OutEnvironment)
	{
		FDirtPassCS::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("DIRT_WATER_PASS"), 1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FDirtWaterCS, "/DaDirt/Private/DirtSim.usf", "MainWaterCS", SF_Compute);

// ---------------------------------------------------------------------------
// Resolve
// ---------------------------------------------------------------------------

class FDirtResolveCS : public FDirtPassCS
{
public:
	DECLARE_GLOBAL_SHADER(FDirtResolveCS);
	SHADER_USE_PARAMETER_STRUCT(FDirtResolveCS, FDirtPassCS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		DIRT_SHARED_PARAMETERS()
		DIRT_SOIL_PARAMETERS()
		SHADER_PARAMETER(int32, DirtDebugMode)
		SHADER_PARAMETER(float, DirtDebugLayerRangeCm)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, DirtPondIn)
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
// Parcels
// ---------------------------------------------------------------------------

class FDirtParcelInitCS : public FDirtPassCS
{
public:
	DECLARE_GLOBAL_SHADER(FDirtParcelInitCS);
	SHADER_USE_PARAMETER_STRUCT(FDirtParcelInitCS, FDirtPassCS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		DIRT_SHARED_PARAMETERS()
		DIRT_PARCEL_PARAMETERS()
	END_SHADER_PARAMETER_STRUCT()

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
											 FShaderCompilerEnvironment& OutEnvironment)
	{
		FDirtPassCS::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("DIRT_PARCEL_INIT_PASS"), 1);
		// The parcel textures are read AND written as UAVs (RGBA32F loads), which D3D
		// only promises on hardware that reports it; the Arc does, and this opts in.
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
	}
};

IMPLEMENT_GLOBAL_SHADER(FDirtParcelInitCS, "/DaDirt/Private/DirtParcels.usf", "MainParcelInitCS", SF_Compute);

class FDirtParcelSpawnCS : public FDirtPassCS
{
public:
	DECLARE_GLOBAL_SHADER(FDirtParcelSpawnCS);
	SHADER_USE_PARAMETER_STRUCT(FDirtParcelSpawnCS, FDirtPassCS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		DIRT_SHARED_PARAMETERS()
		DIRT_PARCEL_PARAMETERS()
		SHADER_PARAMETER(int32, DirtSpawnCount)
		SHADER_PARAMETER(int32, DirtSpawnTotal)
		SHADER_PARAMETER_ARRAY(FVector4f, DirtSpawnA, [DirtSim::MaxSpawnsPerPass])
		SHADER_PARAMETER_ARRAY(FVector4f, DirtSpawnB, [DirtSim::MaxSpawnsPerPass])
		SHADER_PARAMETER_ARRAY(FVector4f, DirtSpawnC, [DirtSim::MaxSpawnsPerPass])
		SHADER_PARAMETER_ARRAY(FVector4f, DirtSpawnD, [DirtSim::MaxSpawnsPerPass])
	END_SHADER_PARAMETER_STRUCT()

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
											 FShaderCompilerEnvironment& OutEnvironment)
	{
		FDirtPassCS::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("DIRT_PARCEL_SPAWN_PASS"), 1);
		// The parcel textures are read AND written as UAVs (RGBA32F loads), which D3D
		// only promises on hardware that reports it; the Arc does, and this opts in.
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
	}
};

IMPLEMENT_GLOBAL_SHADER(FDirtParcelSpawnCS, "/DaDirt/Private/DirtParcels.usf", "MainParcelSpawnCS", SF_Compute);

class FDirtParcelSimCS : public FDirtPassCS
{
public:
	DECLARE_GLOBAL_SHADER(FDirtParcelSimCS);
	SHADER_USE_PARAMETER_STRUCT(FDirtParcelSimCS, FDirtPassCS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		DIRT_SHARED_PARAMETERS()
		DIRT_PARCEL_PARAMETERS()
		SHADER_PARAMETER(float, DirtDt)
		SHADER_PARAMETER(float, DirtGravityCmS2)
		SHADER_PARAMETER(float, DirtParcelDragK)
		SHADER_PARAMETER(float, DirtParcelRestitutionDry)
		SHADER_PARAMETER(float, DirtParcelRestitutionWet)
		SHADER_PARAMETER(float, DirtParcelRestSpeedCmS)
		SHADER_PARAMETER(float, DirtParcelRestSeconds)
		SHADER_PARAMETER(float, DirtLooseReposeDeg)
		SHADER_PARAMETER(float, DirtPackedReposeDeg)
		SHADER_PARAMETER(float, DirtSaturationFrictionLoss)
	END_SHADER_PARAMETER_STRUCT()

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
											 FShaderCompilerEnvironment& OutEnvironment)
	{
		FDirtPassCS::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("DIRT_PARCEL_SIM_PASS"), 1);
		// The parcel textures are read AND written as UAVs (RGBA32F loads), which D3D
		// only promises on hardware that reports it; the Arc does, and this opts in.
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
	}
};

IMPLEMENT_GLOBAL_SHADER(FDirtParcelSimCS, "/DaDirt/Private/DirtParcels.usf", "MainParcelSimCS", SF_Compute);

// ---------------------------------------------------------------------------
// Parcel resources
// ---------------------------------------------------------------------------

FDirtParcelResources::~FDirtParcelResources()
{
	delete Readback;
	Readback = nullptr;
}

namespace
{
	/** Everything the parcel passes bind, resolved for this frame's graph. */
	struct FParcelGraphResources
	{
		FRDGTextureRef Pos = nullptr;
		FRDGTextureRef Vel = nullptr;
		FRDGTextureRef Prop = nullptr;
		FRDGBufferRef FreeList = nullptr;
		FRDGBufferRef Counters = nullptr;
		FRDGTextureRef DepositVol = nullptr;
		FRDGTextureRef DepositMoist = nullptr;
		bool bFresh = false;
	};

	FParcelGraphResources AcquireParcelResources(FRDGBuilder& GraphBuilder, const FDirtSimFrame& Frame)
	{
		FDirtParcelResources& R = *Frame.Parcels;
		FParcelGraphResources G;

		G.Pos = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Frame.ParcelPos, TEXT("DirtParcelPos")));
		G.Vel = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Frame.ParcelVel, TEXT("DirtParcelVel")));
		G.Prop = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Frame.ParcelProp, TEXT("DirtParcelProp")));

		const int32 Count = Frame.ParcelRes * Frame.ParcelRes;
		const bool bNeedRebuild = !R.FreeList || !R.Counters || !R.DepositVol || !R.DepositMoist
			|| R.ParcelRes != Frame.ParcelRes || R.DepositRes != Frame.Resolution;

		if (bNeedRebuild)
		{
			G.FreeList = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), Count), TEXT("DirtParcelFree"));
			G.Counters = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), DirtSim::ParcelCounterCount), TEXT("DirtParcelCounters"));

			const FRDGTextureDesc DepositDesc = FRDGTextureDesc::Create2D(Frame.Resolution, PF_R32_UINT, FClearValueBinding::Black,
																		  TexCreate_ShaderResource | TexCreate_UAV);
			G.DepositVol = GraphBuilder.CreateTexture(DepositDesc, TEXT("DirtDepositVol"));
			G.DepositMoist = GraphBuilder.CreateTexture(DepositDesc, TEXT("DirtDepositMoist"));

			// Extraction to the pooled references happens at the end of the frame,
			// once the init passes have produced them: RDG refuses to extract a
			// resource no pass has written.
			R.ParcelRes = Frame.ParcelRes;
			R.DepositRes = Frame.Resolution;
			R.bInitialised = false;
			G.bFresh = true;
		}
		else
		{
			G.FreeList = GraphBuilder.RegisterExternalBuffer(R.FreeList);
			G.Counters = GraphBuilder.RegisterExternalBuffer(R.Counters);
			G.DepositVol = GraphBuilder.RegisterExternalTexture(R.DepositVol);
			G.DepositMoist = GraphBuilder.RegisterExternalTexture(R.DepositMoist);
		}

		return G;
	}

	template <typename TParams>
	void BindParcelParameters(FRDGBuilder& GraphBuilder, TParams* Params, const FDirtSimFrame& Frame,
							  const FParcelGraphResources& G)
	{
		Params->DirtParcelRes = Frame.ParcelRes;
		Params->DirtParcelPos = GraphBuilder.CreateUAV(G.Pos);
		Params->DirtParcelVel = GraphBuilder.CreateUAV(G.Vel);
		Params->DirtParcelProp = GraphBuilder.CreateUAV(G.Prop);
		Params->DirtParcelFree = GraphBuilder.CreateUAV(G.FreeList);
		Params->DirtParcelCounters = GraphBuilder.CreateUAV(G.Counters);
		Params->DirtRegionOriginCm = Frame.RegionOriginCm;
		Params->DirtTexelSizeCm = Frame.TexelSizeCm;
		Params->DirtTexelAreaCm2 = Frame.TexelSizeCm * Frame.TexelSizeCm;
		Params->DirtDepositVol = GraphBuilder.CreateUAV(G.DepositVol);
		Params->DirtDepositMoist = GraphBuilder.CreateUAV(G.DepositMoist);
	}

	template <typename TParams>
	void BindSharedParameters(TParams* Params, const FDirtSimFrame& Frame, FRDGTextureRef StateIn)
	{
		Params->DirtResolution = Frame.Resolution;
		Params->DirtLoosePorosity = Frame.LoosePorosity;
		Params->DirtDensePorosity = Frame.DensePorosity;
		Params->DirtCompactionDepthCm = Frame.CompactionDepthCm;
		Params->DirtDeepCompaction = Frame.DeepCompaction;
		Params->DirtBaseHeight = Frame.BaseHeight;
		Params->DirtStateIn = StateIn;
	}

	template <typename TParams>
	void BindSoilParameters(TParams* Params, const FDirtSimFrame& Frame)
	{
		Params->DirtTexelSizeCm = Frame.TexelSizeCm;
		Params->DirtLooseReposeDeg = Frame.LooseReposeDeg;
		Params->DirtPackedReposeDeg = Frame.PackedReposeDeg;
		Params->DirtSuctionCohesionKPa = Frame.SuctionCohesionKPa;
		Params->DirtPackedCohesionKPa = Frame.PackedCohesionKPa;
		Params->DirtUnitWeightKNm3 = Frame.UnitWeightKNm3;
		Params->DirtSaturationFrictionLoss = Frame.SaturationFrictionLoss;
		Params->DirtMaxCohesiveHeightCm = Frame.MaxCohesiveHeightCm;
	}
}

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

	FRDGBuilder GraphBuilder(RHICmdList);

	FRDGTextureRef StateA = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Frame.StateA, TEXT("DirtStateA")));
	FRDGTextureRef StateB = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Frame.StateB, TEXT("DirtStateB")));
	FRDGTextureRef PondA = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Frame.PondA, TEXT("DirtPondA")));
	FRDGTextureRef PondB = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Frame.PondB, TEXT("DirtPondB")));
	FRDGTextureRef DisplayTex = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Frame.Display, TEXT("DirtDisplay")));
	FRDGTextureRef NormalTex = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Frame.NormalOut, TEXT("DirtNormal")));
	FRDGTextureRef DebugTex = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Frame.DebugOut, TEXT("DirtDebug")));

	const FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(
		FIntPoint(Frame.Resolution.X, Frame.Resolution.Y), GDirtThreadGroupSize);

	// Every pass reads one state texture and writes the other. Track which one
	// currently holds the live state.
	FRDGTextureRef Current = StateA;
	FRDGTextureRef Other = StateB;
	FRDGTextureRef PondCur = PondA;
	FRDGTextureRef PondOther = PondB;

	const auto Flip = [&Current, &Other]() { Swap(Current, Other); };
	const auto FlipPond = [&PondCur, &PondOther]() { Swap(PondCur, PondOther); };

	const bool bParcels = Frame.HasParcels();
	FParcelGraphResources Parcels;
	if (bParcels)
	{
		Parcels = AcquireParcelResources(GraphBuilder, Frame);
	}

	// --- init --------------------------------------------------------------
	if (Frame.bReinitialise)
	{
		FDirtInitCS::FParameters* Params = GraphBuilder.AllocParameters<FDirtInitCS::FParameters>();
		BindSharedParameters(Params, Frame, Current);          // StateIn unused by the pass, but must be bound
		Params->DirtInitialState = Frame.InitialState;
		Params->DirtStateOut = GraphBuilder.CreateUAV(Other);
		Params->DirtPondOut = GraphBuilder.CreateUAV(PondOther);

		TShaderMapRef<FDirtInitCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("DirtInit"), Shader, Params, GroupCount);
		Flip();
		FlipPond();
	}

	if (bParcels && (Frame.bReinitialise || Parcels.bFresh || !Frame.Parcels->bInitialised))
	{
		FDirtParcelInitCS::FParameters* Params = GraphBuilder.AllocParameters<FDirtParcelInitCS::FParameters>();
		BindSharedParameters(Params, Frame, Current);
		BindParcelParameters(GraphBuilder, Params, Frame, Parcels);

		TShaderMapRef<FDirtParcelInitCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("DirtParcelInit"), Shader, Params,
									 FComputeShaderUtils::GetGroupCount(Frame.ParcelRes * Frame.ParcelRes, GParcelThreadGroupSize));

		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Parcels.DepositVol), 0u);
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Parcels.DepositMoist), 0u);
		Frame.Parcels->bInitialised = true;
	}

	// --- landed parcels join the ground ----------------------------------------
	if (bParcels)
	{
		FDirtDepositCS::FParameters* Params = GraphBuilder.AllocParameters<FDirtDepositCS::FParameters>();
		BindSharedParameters(Params, Frame, Current);
		Params->DirtTexelAreaCm2 = Frame.TexelSizeCm * Frame.TexelSizeCm;
		Params->DirtDepositCompaction = Frame.DepositCompaction;
		Params->DirtResetLiveMax = (Frame.SlumpIterations > 0) ? 1 : 0;
		Params->DirtDepositVol = GraphBuilder.CreateUAV(Parcels.DepositVol);
		Params->DirtDepositMoist = GraphBuilder.CreateUAV(Parcels.DepositMoist);
		Params->DirtParcelCounters = GraphBuilder.CreateUAV(Parcels.Counters);
		Params->DirtStateOut = GraphBuilder.CreateUAV(Other);

		TShaderMapRef<FDirtDepositCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("DirtDeposit"), Shader, Params, GroupCount);
		Flip();
	}

	// --- brush strokes, batched ------------------------------------------------
	for (int32 First = 0; First < Frame.Strokes.Num(); First += DirtSim::MaxStrokesPerPass)
	{
		const int32 Count = FMath::Min(DirtSim::MaxStrokesPerPass, Frame.Strokes.Num() - First);

		FDirtBrushCS::FParameters* Params = GraphBuilder.AllocParameters<FDirtBrushCS::FParameters>();
		BindSharedParameters(Params, Frame, Current);
		Params->DirtStrokeCount = Count;
		for (int32 K = 0; K < Count; ++K)
		{
			const FDirtBrushStroke& Stroke = Frame.Strokes[First + K];
			Params->DirtStrokeA[K] = FVector4f(Stroke.CenterTexel.X, Stroke.CenterTexel.Y,
											   Stroke.CoreRadiusTexels, Stroke.RimRadiusTexels);
			Params->DirtStrokeB[K] = FVector4f(Stroke.CoreNorm, Stroke.RimNorm, Stroke.Amount, Stroke.Disturb);
			Params->DirtStrokeC[K] = FVector4f(static_cast<float>(Stroke.Mode), Stroke.bProctor ? 1.0f : 0.0f, 0.0f, 0.0f);
		}
		Params->DirtStateOut = GraphBuilder.CreateUAV(Other);

		TShaderMapRef<FDirtBrushCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("DirtBrush"), Shader, Params, GroupCount);
		Flip();
	}

	// --- slump iterations --------------------------------------------------
	for (int32 Iteration = 0; Iteration < Frame.SlumpIterations; ++Iteration)
	{
		FDirtSlumpCS::FParameters* Params = GraphBuilder.AllocParameters<FDirtSlumpCS::FParameters>();
		BindSharedParameters(Params, Frame, Current);
		BindSoilParameters(Params, Frame);
		Params->DirtSlumpRate = Frame.SlumpRate;
		Params->DirtLooseningRate = Frame.LooseningRate;
		Params->DirtLooseningScaleCm = Frame.LooseningScaleCm;
		Params->DirtStateOut = GraphBuilder.CreateUAV(Other);

		TShaderMapRef<FDirtSlumpCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("DirtSlump"), Shader, Params, GroupCount);
		Flip();
	}

	// --- water -------------------------------------------------------------
	if (Frame.bWater && Frame.SlumpIterations > 0)
	{
		FDirtWaterCS::FParameters* Params = GraphBuilder.AllocParameters<FDirtWaterCS::FParameters>();
		BindSharedParameters(Params, Frame, Current);
		Params->DirtDt = Frame.Dt;
		Params->DirtRunoffRate = Frame.RunoffRate;
		Params->DirtRainCmPerSec = Frame.RainCmPerSec;
		Params->DirtInfiltrationCmPerSec = Frame.InfiltrationCmPerSec;
		Params->DirtDrainPerSec = Frame.DrainPerSec;
		Params->DirtEvapPerSec = Frame.EvapPerSec;
		Params->DirtFieldCapacity = Frame.FieldCapacity;
		Params->DirtWetDepthCm = Frame.WetDepthCm;
		Params->DirtAmbientMoisture = Frame.AmbientMoisture;
		const int32 SourceCount = FMath::Min(Frame.WaterSources.Num(), DirtSim::MaxWaterSourcesPerPass);
		Params->DirtWaterSourceCount = SourceCount;
		for (int32 K = 0; K < SourceCount; ++K)
		{
			const FDirtWaterSource& W = Frame.WaterSources[K];
			Params->DirtWaterSource[K] = FVector4f(W.CenterTexel.X, W.CenterTexel.Y, W.RadiusTexels, W.AmountCm);
		}
		Params->DirtPondIn = PondCur;
		Params->DirtPondOut = GraphBuilder.CreateUAV(PondOther);
		Params->DirtStateOut = GraphBuilder.CreateUAV(Other);

		TShaderMapRef<FDirtWaterCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("DirtWater"), Shader, Params, GroupCount);
		Flip();
		FlipPond();
	}

	// --- parcels -----------------------------------------------------------
	if (bParcels)
	{
		for (int32 First = 0; First < Frame.Spawns.Num(); First += DirtSim::MaxSpawnsPerPass)
		{
			const int32 Count = FMath::Min(DirtSim::MaxSpawnsPerPass, Frame.Spawns.Num() - First);

			FDirtParcelSpawnCS::FParameters* Params = GraphBuilder.AllocParameters<FDirtParcelSpawnCS::FParameters>();
			BindSharedParameters(Params, Frame, Current);
			BindParcelParameters(GraphBuilder, Params, Frame, Parcels);

			int32 Total = 0;
			for (int32 K = 0; K < Count; ++K)
			{
				const FDirtParcelSpawn& S = Frame.Spawns[First + K];
				const int32 N = FMath::Max(S.Count, 1);
				Params->DirtSpawnA[K] = FVector4f(S.PositionCm.X, S.PositionCm.Y, S.PositionCm.Z, S.VolumeCm3);
				Params->DirtSpawnB[K] = FVector4f(S.VelocityCmS.X, S.VelocityCmS.Y, S.VelocityCmS.Z, static_cast<float>(N));
				Params->DirtSpawnC[K] = FVector4f(S.SpreadDeg, S.Moisture, S.Compaction, S.DiameterCm);
				Params->DirtSpawnD[K] = FVector4f(static_cast<float>(S.Seed & 0xFFFFFFu), static_cast<float>(Total), S.SpeedJitter, S.DiameterJitter);
				Total += N;
			}
			Params->DirtSpawnCount = Count;
			Params->DirtSpawnTotal = Total;

			TShaderMapRef<FDirtParcelSpawnCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("DirtParcelSpawn"), Shader, Params,
										 FComputeShaderUtils::GetGroupCount(FMath::Max(Total, 1), GParcelThreadGroupSize));
		}

		if (Frame.SlumpIterations > 0)     // paused freezes the air as well as the ground
		{
			FDirtParcelSimCS::FParameters* Params = GraphBuilder.AllocParameters<FDirtParcelSimCS::FParameters>();
			BindSharedParameters(Params, Frame, Current);
			BindParcelParameters(GraphBuilder, Params, Frame, Parcels);
			Params->DirtDt = Frame.Dt;
			Params->DirtGravityCmS2 = Frame.GravityCmS2;
			Params->DirtParcelDragK = Frame.ParcelDragK;
			Params->DirtParcelRestitutionDry = Frame.ParcelRestitutionDry;
			Params->DirtParcelRestitutionWet = Frame.ParcelRestitutionWet;
			Params->DirtParcelRestSpeedCmS = Frame.ParcelRestSpeedCmS;
			Params->DirtParcelRestSeconds = Frame.ParcelRestSeconds;
			Params->DirtLooseReposeDeg = Frame.LooseReposeDeg;
			Params->DirtPackedReposeDeg = Frame.PackedReposeDeg;
			Params->DirtSaturationFrictionLoss = Frame.SaturationFrictionLoss;

			TShaderMapRef<FDirtParcelSimCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("DirtParcelSim"), Shader, Params,
										 FComputeShaderUtils::GetGroupCount(Frame.ParcelRes * Frame.ParcelRes, GParcelThreadGroupSize));
		}

		// Non-blocking readback of the four counters, harvested a frame or two later.
		FDirtParcelResources& R = *Frame.Parcels;
		if (!R.Readback)
		{
			R.Readback = new FRHIGPUBufferReadback(TEXT("DirtParcelCounters"));
		}
		if (!R.bReadbackPending)
		{
			AddEnqueueCopyPass(GraphBuilder, R.Readback, Parcels.Counters, DirtSim::ParcelCounterCount * sizeof(uint32));
			R.bReadbackPending = true;
		}
	}

	// --- resolve -----------------------------------------------------------
	{
		FDirtResolveCS::FParameters* Params = GraphBuilder.AllocParameters<FDirtResolveCS::FParameters>();
		BindSharedParameters(Params, Frame, Current);
		BindSoilParameters(Params, Frame);
		Params->DirtDebugMode = Frame.DebugMode;
		Params->DirtDebugLayerRangeCm = Frame.DebugLayerRangeCm;
		Params->DirtPondIn = PondCur;
		Params->DirtDisplayOut = GraphBuilder.CreateUAV(DisplayTex);
		Params->DirtNormalOut = GraphBuilder.CreateUAV(NormalTex);
		Params->DirtDebugOut = GraphBuilder.CreateUAV(DebugTex);

		TShaderMapRef<FDirtResolveCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("DirtResolve"), Shader, Params, GroupCount);
	}

	// The pass count varies with how many strokes came in this step, so the live
	// state does not always land back in StateA. Copying it back costs one 16 MB
	// blit and means the next step can always assume StateA is the input — much
	// less to get wrong than tracking parity across frames.
	if (Current != StateA)
	{
		AddCopyTexturePass(GraphBuilder, Current, StateA);
	}
	if (PondCur != PondA)
	{
		AddCopyTexturePass(GraphBuilder, PondCur, PondA);
	}

	// Keep freshly created parcel bookkeeping alive across frames.
	if (bParcels && Parcels.bFresh)
	{
		FDirtParcelResources& R = *Frame.Parcels;
		GraphBuilder.QueueBufferExtraction(Parcels.FreeList, &R.FreeList);
		GraphBuilder.QueueBufferExtraction(Parcels.Counters, &R.Counters);
		GraphBuilder.QueueTextureExtraction(Parcels.DepositVol, &R.DepositVol);
		GraphBuilder.QueueTextureExtraction(Parcels.DepositMoist, &R.DepositMoist);
	}

	GraphBuilder.Execute();
}

void DirtSim::UpdateParcelCounters_RenderThread(FRHICommandListImmediate& RHICmdList, FDirtParcelResources& Parcels)
{
	check(IsInRenderingThread());

	if (!Parcels.Readback || !Parcels.bReadbackPending || !Parcels.Readback->IsReady())
	{
		return;
	}

	if (const uint32* Data = static_cast<const uint32*>(Parcels.Readback->Lock(DirtSim::ParcelCounterCount * sizeof(uint32))))
	{
		for (int32 i = 0; i < DirtSim::ParcelCounterCount; ++i)
		{
			Parcels.Counters_RT[i] = Data[i];
		}
		Parcels.Readback->Unlock();

		FScopeLock L(&Parcels.CounterLock);
		for (int32 i = 0; i < DirtSim::ParcelCounterCount; ++i)
		{
			Parcels.Counters_Shared[i] = Parcels.Counters_RT[i];
		}
	}
	Parcels.bReadbackPending = false;
}
