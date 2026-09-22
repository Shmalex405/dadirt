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
	constexpr int32 GSlumpTileSize = 16;            // must match SLUMP_TILE in DirtSim.usf
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
	SHADER_PARAMETER(float, DirtCompactionDepthCm) \
	SHADER_PARAMETER(float, DirtDeepCompaction) \
	SHADER_PARAMETER_TEXTURE(Texture2D<float>, DirtBaseHeight) \
	SHADER_PARAMETER_TEXTURE(Texture2D<uint>, DirtSoilIn) \
	SHADER_PARAMETER_ARRAY(FVector4f, DirtSoilTable, [DirtSim::MaxSoils * DirtSim::SoilRows]) \
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, DirtStateIn)

#define DIRT_SOIL_PARAMETERS() \
	SHADER_PARAMETER(float, DirtTexelSizeCm) \
	SHADER_PARAMETER(float, DirtMaxCohesiveHeightCm)

// The bindings DirtParcelCommon.ush declares. DirtTexelSizeCm is deliberately
// not here: the slump pass has it from the soil set, the parcel passes add it.
#define DIRT_PARCEL_PARAMETERS() \
	SHADER_PARAMETER(int32, DirtParcelRes) \
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DirtParcelPos) \
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DirtParcelVel) \
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DirtParcelProp) \
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, DirtParcelFree) \
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, DirtParcelCounters) \
	SHADER_PARAMETER(FVector2f, DirtRegionOriginCm) \
	SHADER_PARAMETER(float, DirtTexelAreaCm2) \
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, DirtDepositVol) \
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, DirtDepositMoist)

// The parcel textures are read AND written as UAVs (RGBA32F loads), which D3D
// only promises on hardware that reports it; the Arc does, and this opts in.
#define DIRT_ALLOW_TYPED_UAV_LOADS() OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads)

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
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, DirtPondOut)
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
// Shift (the window slides over the world)
// ---------------------------------------------------------------------------

class FDirtShiftCS : public FDirtPassCS
{
public:
	DECLARE_GLOBAL_SHADER(FDirtShiftCS);
	SHADER_USE_PARAMETER_STRUCT(FDirtShiftCS, FDirtPassCS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		DIRT_SHARED_PARAMETERS()
		SHADER_PARAMETER(FIntPoint, DirtShiftTexels)
		SHADER_PARAMETER_TEXTURE(Texture2D<float4>, DirtPatchState)
		SHADER_PARAMETER_TEXTURE(Texture2D<float2>, DirtPatchPond)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, DirtPondIn)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DirtStateOut)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, DirtPondOut)
	END_SHADER_PARAMETER_STRUCT()

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
											 FShaderCompilerEnvironment& OutEnvironment)
	{
		FDirtPassCS::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("DIRT_SHIFT_PASS"), 1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FDirtShiftCS, "/DaDirt/Private/DirtSim.usf", "MainShiftCS", SF_Compute);

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
		DIRT_PARCEL_PARAMETERS()
		SHADER_PARAMETER(float, DirtDepositCompaction)
		SHADER_PARAMETER(int32, DirtResetLiveMax)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DirtStateOut)
	END_SHADER_PARAMETER_STRUCT()

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
											 FShaderCompilerEnvironment& OutEnvironment)
	{
		FDirtPassCS::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("DIRT_DEPOSIT_PASS"), 1);
		DIRT_ALLOW_TYPED_UAV_LOADS();
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
		SHADER_PARAMETER(int32, DirtStrokeBase)
		SHADER_PARAMETER(float, DirtTexelAreaCm2)
		SHADER_PARAMETER_ARRAY(FVector4f, DirtStrokeA, [DirtSim::MaxStrokesPerPass])
		SHADER_PARAMETER_ARRAY(FVector4f, DirtStrokeB, [DirtSim::MaxStrokesPerPass])
		SHADER_PARAMETER_ARRAY(FVector4f, DirtStrokeC, [DirtSim::MaxStrokesPerPass])
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DirtStateOut)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, DirtScoopShortfall)
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
		DIRT_PARCEL_PARAMETERS()
		SHADER_PARAMETER(float, DirtSlumpRate)
		SHADER_PARAMETER(float, DirtLooseningRate)
		SHADER_PARAMETER(float, DirtLooseningScaleCm)
		SHADER_PARAMETER(int32, DirtShedEnabled)
		SHADER_PARAMETER(float, DirtShedMinOutCm)
		SHADER_PARAMETER(float, DirtShedChance)
		SHADER_PARAMETER(float, DirtShedFraction)
		SHADER_PARAMETER(float, DirtShedDiameterCm)
		SHADER_PARAMETER(float, DirtShedSpeedCmS)
		SHADER_PARAMETER(uint32, DirtFrameSeed)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DirtStateOut)
	END_SHADER_PARAMETER_STRUCT()

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
											 FShaderCompilerEnvironment& OutEnvironment)
	{
		FDirtPassCS::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("DIRT_SLUMP_PASS"), 1);
		DIRT_ALLOW_TYPED_UAV_LOADS();
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
		SHADER_PARAMETER(float, DirtTexelSizeCm)
		SHADER_PARAMETER(int32, DirtErosionEnabled)
		SHADER_PARAMETER(float, DirtErosionPace)
		SHADER_PARAMETER(float, DirtRunoffRate)
		SHADER_PARAMETER(float, DirtRainCmPerSec)
		SHADER_PARAMETER(float, DirtDrainPerSec)
		SHADER_PARAMETER(float, DirtEvapPerSec)
		SHADER_PARAMETER(float, DirtWetDepthCm)
		SHADER_PARAMETER(float, DirtAmbientMoisture)
		SHADER_PARAMETER(int32, DirtWaterSourceCount)
		SHADER_PARAMETER_ARRAY(FVector4f, DirtWaterSource, [DirtSim::MaxWaterSourcesPerPass])
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, DirtPondIn)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, DirtPondOut)
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
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, DirtPondIn)
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
// Parcels (and dust: the same shaders with DirtIsDust = 1)
// ---------------------------------------------------------------------------

class FDirtParcelInitCS : public FDirtPassCS
{
public:
	DECLARE_GLOBAL_SHADER(FDirtParcelInitCS);
	SHADER_USE_PARAMETER_STRUCT(FDirtParcelInitCS, FDirtPassCS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		DIRT_SHARED_PARAMETERS()
		DIRT_PARCEL_PARAMETERS()
		SHADER_PARAMETER(float, DirtTexelSizeCm)
		SHADER_PARAMETER(int32, DirtIsDust)
	END_SHADER_PARAMETER_STRUCT()

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
											 FShaderCompilerEnvironment& OutEnvironment)
	{
		FDirtPassCS::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("DIRT_PARCEL_INIT_PASS"), 1);
		DIRT_ALLOW_TYPED_UAV_LOADS();
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
		SHADER_PARAMETER(float, DirtTexelSizeCm)
		SHADER_PARAMETER(int32, DirtIsDust)
		SHADER_PARAMETER(int32, DirtSpawnCount)
		SHADER_PARAMETER(int32, DirtSpawnTotal)
		SHADER_PARAMETER_ARRAY(FVector4f, DirtSpawnA, [DirtSim::MaxSpawnsPerPass])
		SHADER_PARAMETER_ARRAY(FVector4f, DirtSpawnB, [DirtSim::MaxSpawnsPerPass])
		SHADER_PARAMETER_ARRAY(FVector4f, DirtSpawnC, [DirtSim::MaxSpawnsPerPass])
		SHADER_PARAMETER_ARRAY(FVector4f, DirtSpawnD, [DirtSim::MaxSpawnsPerPass])
		SHADER_PARAMETER_ARRAY(FVector4f, DirtSpawnE, [DirtSim::MaxSpawnsPerPass])
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, DirtScoopShortfall)
		SHADER_PARAMETER(int32, DirtDepositOnly)
	END_SHADER_PARAMETER_STRUCT()

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
											 FShaderCompilerEnvironment& OutEnvironment)
	{
		FDirtPassCS::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("DIRT_PARCEL_SPAWN_PASS"), 1);
		DIRT_ALLOW_TYPED_UAV_LOADS();
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
		SHADER_PARAMETER(float, DirtTexelSizeCm)
		SHADER_PARAMETER(int32, DirtIsDust)
		SHADER_PARAMETER(float, DirtDt)
		SHADER_PARAMETER(float, DirtGravityCmS2)
		SHADER_PARAMETER(float, DirtParcelDragK)
		SHADER_PARAMETER(float, DirtParcelRestitutionDry)
		SHADER_PARAMETER(float, DirtParcelRestitutionWet)
		SHADER_PARAMETER(float, DirtParcelRestSpeedCmS)
		SHADER_PARAMETER(float, DirtParcelRestSeconds)
		SHADER_PARAMETER(float, DirtDustLifetime)
		SHADER_PARAMETER(float, DirtDustDragK)
		SHADER_PARAMETER(float, DirtDustBuoyancy)
	END_SHADER_PARAMETER_STRUCT()

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
											 FShaderCompilerEnvironment& OutEnvironment)
	{
		FDirtPassCS::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("DIRT_PARCEL_SIM_PASS"), 1);
		DIRT_ALLOW_TYPED_UAV_LOADS();
	}
};

IMPLEMENT_GLOBAL_SHADER(FDirtParcelSimCS, "/DaDirt/Private/DirtParcels.usf", "MainParcelSimCS", SF_Compute);

class FDirtParcelFlushCS : public FDirtPassCS
{
public:
	DECLARE_GLOBAL_SHADER(FDirtParcelFlushCS);
	SHADER_USE_PARAMETER_STRUCT(FDirtParcelFlushCS, FDirtPassCS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		DIRT_SHARED_PARAMETERS()
		DIRT_PARCEL_PARAMETERS()
		SHADER_PARAMETER(float, DirtTexelSizeCm)
		SHADER_PARAMETER(int32, DirtIsDust)
		SHADER_PARAMETER(FIntPoint, DirtShiftTexels)
	END_SHADER_PARAMETER_STRUCT()

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
											 FShaderCompilerEnvironment& OutEnvironment)
	{
		FDirtPassCS::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("DIRT_PARCEL_FLUSH_PASS"), 1);
		DIRT_ALLOW_TYPED_UAV_LOADS();
	}
};

IMPLEMENT_GLOBAL_SHADER(FDirtParcelFlushCS, "/DaDirt/Private/DirtParcels.usf", "MainParcelFlushCS", SF_Compute);

// ---------------------------------------------------------------------------
// Parcel resources
// ---------------------------------------------------------------------------

FDirtParcelResources::~FDirtParcelResources()
{
	delete Readback;
	Readback = nullptr;
}

FDirtTileReadback::~FDirtTileReadback()
{
	delete State;
	delete Pond;
}

namespace
{
	/** Everything the parcel passes bind for one pool, resolved for this frame's graph. */
	struct FParcelGraphResources
	{
		FRDGTextureRef Pos = nullptr;
		FRDGTextureRef Vel = nullptr;
		FRDGTextureRef Prop = nullptr;
		FRDGBufferRef FreeList = nullptr;
		FRDGBufferRef Counters = nullptr;
		FRDGTextureRef DepositVol = nullptr;
		FRDGTextureRef DepositMoist = nullptr;
		int32 Res = 0;
		bool bFresh = false;
	};

	/**
	 * Register a pool's textures and create or register its free list and counters.
	 * The dirt pool owns the deposit textures; the dust pool borrows the dirt
	 * pool's (it never deposits, but the shaders bind them).
	 */
	FParcelGraphResources AcquirePool(FRDGBuilder& GraphBuilder, FDirtParcelResources& R, FRHITexture* Pos, FRHITexture* Vel,
									  FRHITexture* Prop, int32 Res, const FIntPoint& DepositRes, const FParcelGraphResources* DepositOwner,
									  const TCHAR* Name)
	{
		FParcelGraphResources G;
		G.Res = Res;
		G.Pos = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Pos, Name));
		G.Vel = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Vel, Name));
		G.Prop = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Prop, Name));

		const int32 Count = Res * Res;
		const bool bOwnsDeposits = DepositOwner == nullptr;
		const bool bNeedRebuild = !R.FreeList || !R.Counters || R.ParcelRes != Res
			|| (bOwnsDeposits && (!R.DepositVol || !R.DepositMoist || R.DepositRes != DepositRes));

		if (bNeedRebuild)
		{
			G.FreeList = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), Count), TEXT("DirtParcelFree"));
			G.Counters = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), DirtSim::ParcelCounterCount), TEXT("DirtParcelCounters"));
			if (bOwnsDeposits)
			{
				const FRDGTextureDesc DepositDesc = FRDGTextureDesc::Create2D(DepositRes, PF_R32_UINT, FClearValueBinding::Black,
																			  TexCreate_ShaderResource | TexCreate_UAV);
				G.DepositVol = GraphBuilder.CreateTexture(DepositDesc, TEXT("DirtDepositVol"));
				G.DepositMoist = GraphBuilder.CreateTexture(DepositDesc, TEXT("DirtDepositMoist"));
				R.DepositRes = DepositRes;
			}
			// Extraction to the pooled references happens at the end of the frame,
			// once the init passes have produced them: RDG refuses to extract a
			// resource no pass has written.
			R.ParcelRes = Res;
			R.bInitialised = false;
			G.bFresh = true;
		}
		else
		{
			G.FreeList = GraphBuilder.RegisterExternalBuffer(R.FreeList);
			G.Counters = GraphBuilder.RegisterExternalBuffer(R.Counters);
			if (bOwnsDeposits)
			{
				G.DepositVol = GraphBuilder.RegisterExternalTexture(R.DepositVol);
				G.DepositMoist = GraphBuilder.RegisterExternalTexture(R.DepositMoist);
			}
		}

		if (!bOwnsDeposits)
		{
			G.DepositVol = DepositOwner->DepositVol;
			G.DepositMoist = DepositOwner->DepositMoist;
		}
		return G;
	}

	template <typename TParams>
	void BindParcelParameters(FRDGBuilder& GraphBuilder, TParams* Params, const FDirtSimFrame& Frame,
							  const FParcelGraphResources& G)
	{
		Params->DirtParcelRes = G.Res;
		Params->DirtParcelPos = GraphBuilder.CreateUAV(G.Pos);
		Params->DirtParcelVel = GraphBuilder.CreateUAV(G.Vel);
		Params->DirtParcelProp = GraphBuilder.CreateUAV(G.Prop);
		Params->DirtParcelFree = GraphBuilder.CreateUAV(G.FreeList);
		Params->DirtParcelCounters = GraphBuilder.CreateUAV(G.Counters);
		Params->DirtRegionOriginCm = Frame.RegionOriginCm;
		Params->DirtTexelAreaCm2 = Frame.TexelSizeCm * Frame.TexelSizeCm;
		Params->DirtDepositVol = GraphBuilder.CreateUAV(G.DepositVol);
		Params->DirtDepositMoist = GraphBuilder.CreateUAV(G.DepositMoist);
	}

	template <typename TParams>
	void BindSharedParameters(TParams* Params, const FDirtSimFrame& Frame, FRDGTextureRef StateIn)
	{
		Params->DirtResolution = Frame.Resolution;
		Params->DirtCompactionDepthCm = Frame.CompactionDepthCm;
		Params->DirtDeepCompaction = Frame.DeepCompaction;
		Params->DirtBaseHeight = Frame.BaseHeight;
		Params->DirtSoilIn = Frame.SoilIn;
		for (int32 i = 0; i < DirtSim::MaxSoils * DirtSim::SoilRows; ++i)
		{
			Params->DirtSoilTable[i] = Frame.SoilTable.IsValidIndex(i) ? Frame.SoilTable[i] : FVector4f::Zero();
		}
		Params->DirtStateIn = StateIn;
	}

	template <typename TParams>
	void BindSoilParameters(TParams* Params, const FDirtSimFrame& Frame)
	{
		Params->DirtTexelSizeCm = Frame.TexelSizeCm;
		Params->DirtMaxCohesiveHeightCm = Frame.MaxCohesiveHeightCm;
	}

	void AddPoolInit(FRDGBuilder& GraphBuilder, const FDirtSimFrame& Frame, FDirtParcelResources& R,
					 const FParcelGraphResources& G, FRDGTextureRef Current, bool bIsDust, bool bClearDeposits)
	{
		FDirtParcelInitCS::FParameters* Params = GraphBuilder.AllocParameters<FDirtParcelInitCS::FParameters>();
		BindSharedParameters(Params, Frame, Current);
		BindParcelParameters(GraphBuilder, Params, Frame, G);
		Params->DirtTexelSizeCm = Frame.TexelSizeCm;
		Params->DirtIsDust = bIsDust ? 1 : 0;

		TShaderMapRef<FDirtParcelInitCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, bIsDust ? RDG_EVENT_NAME("DirtDustInit") : RDG_EVENT_NAME("DirtParcelInit"),
									 Shader, Params, FComputeShaderUtils::GetGroupCount(G.Res * G.Res, GParcelThreadGroupSize));
		if (bClearDeposits)
		{
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(G.DepositVol), 0u);
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(G.DepositMoist), 0u);
		}
		R.bInitialised = true;
	}

	/** The spawn and sim passes for one pool, plus its counter readback. */
	void AddPoolPasses(FRDGBuilder& GraphBuilder, const FDirtSimFrame& Frame, FDirtParcelResources& R,
					   const FParcelGraphResources& G, FRDGTextureRef Current, const TArray<FDirtParcelSpawn>& Spawns, bool bIsDust,
					   FRDGBufferRef Shortfall, bool bFly)
	{
		for (int32 First = 0; First < Spawns.Num(); First += DirtSim::MaxSpawnsPerPass)
		{
			const int32 Count = FMath::Min(DirtSim::MaxSpawnsPerPass, Spawns.Num() - First);

			FDirtParcelSpawnCS::FParameters* Params = GraphBuilder.AllocParameters<FDirtParcelSpawnCS::FParameters>();
			BindSharedParameters(Params, Frame, Current);
			BindParcelParameters(GraphBuilder, Params, Frame, G);
			Params->DirtTexelSizeCm = Frame.TexelSizeCm;
			Params->DirtIsDust = bIsDust ? 1 : 0;

			int32 Total = 0;
			for (int32 K = 0; K < Count; ++K)
			{
				const FDirtParcelSpawn& S = Spawns[First + K];
				const int32 N = FMath::Max(S.Count, 1);
				Params->DirtSpawnA[K] = FVector4f(S.PositionCm.X, S.PositionCm.Y, S.PositionCm.Z, S.VolumeCm3);
				Params->DirtSpawnB[K] = FVector4f(S.VelocityCmS.X, S.VelocityCmS.Y, S.VelocityCmS.Z, static_cast<float>(N));
				Params->DirtSpawnC[K] = FVector4f(S.SpreadDeg, S.Moisture, S.Compaction, S.DiameterCm);
				Params->DirtSpawnD[K] = FVector4f(static_cast<float>(S.Seed & 0xFFFFFFu), static_cast<float>(Total), S.SpeedJitter, S.DiameterJitter);
				Params->DirtSpawnE[K] = FVector4f(static_cast<float>(bIsDust ? -1 : S.ScoopStrokeIndex), 0.0f, 0.0f, 0.0f);
				Total += N;
			}
			Params->DirtSpawnCount = Count;
			Params->DirtSpawnTotal = Total;
			Params->DirtScoopShortfall = GraphBuilder.CreateUAV(Shortfall);
			Params->DirtDepositOnly = bFly ? 0 : 1;

			TShaderMapRef<FDirtParcelSpawnCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder, bIsDust ? RDG_EVENT_NAME("DirtDustSpawn") : RDG_EVENT_NAME("DirtParcelSpawn"),
										 Shader, Params, FComputeShaderUtils::GetGroupCount(FMath::Max(Total, 1), GParcelThreadGroupSize));
		}

		if (bFly && Frame.SlumpIterations > 0)     // paused freezes the air as well as the ground
		{
			FDirtParcelSimCS::FParameters* Params = GraphBuilder.AllocParameters<FDirtParcelSimCS::FParameters>();
			BindSharedParameters(Params, Frame, Current);
			BindParcelParameters(GraphBuilder, Params, Frame, G);
			Params->DirtTexelSizeCm = Frame.TexelSizeCm;
			Params->DirtIsDust = bIsDust ? 1 : 0;
			Params->DirtDt = Frame.Dt;
			Params->DirtGravityCmS2 = Frame.GravityCmS2;
			Params->DirtParcelDragK = Frame.ParcelDragK;
			Params->DirtParcelRestitutionDry = Frame.ParcelRestitutionDry;
			Params->DirtParcelRestitutionWet = Frame.ParcelRestitutionWet;
			Params->DirtParcelRestSpeedCmS = Frame.ParcelRestSpeedCmS;
			Params->DirtParcelRestSeconds = Frame.ParcelRestSeconds;
			Params->DirtDustLifetime = Frame.DustLifetime;
			Params->DirtDustDragK = Frame.DustDragK;
			Params->DirtDustBuoyancy = Frame.DustBuoyancy;

			TShaderMapRef<FDirtParcelSimCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder, bIsDust ? RDG_EVENT_NAME("DirtDustSim") : RDG_EVENT_NAME("DirtParcelSim"),
										 Shader, Params, FComputeShaderUtils::GetGroupCount(G.Res * G.Res, GParcelThreadGroupSize));
		}

		// Non-blocking readback of the counters, harvested a frame or two later.
		if (!R.Readback)
		{
			R.Readback = new FRHIGPUBufferReadback(TEXT("DirtParcelCounters"));
		}
		if (!R.bReadbackPending)
		{
			AddEnqueueCopyPass(GraphBuilder, R.Readback, G.Counters, DirtSim::ParcelCounterCount * sizeof(uint32));
			R.bReadbackPending = true;
		}
	}

	void ExtractPool(FRDGBuilder& GraphBuilder, FDirtParcelResources& R, const FParcelGraphResources& G, bool bOwnsDeposits)
	{
		if (!G.bFresh)
		{
			return;
		}
		GraphBuilder.QueueBufferExtraction(G.FreeList, &R.FreeList);
		GraphBuilder.QueueBufferExtraction(G.Counters, &R.Counters);
		if (bOwnsDeposits)
		{
			GraphBuilder.QueueTextureExtraction(G.DepositVol, &R.DepositVol);
			GraphBuilder.QueueTextureExtraction(G.DepositMoist, &R.DepositMoist);
		}
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
	const FIntVector SlumpGroupCount = FComputeShaderUtils::GetGroupCount(
		FIntPoint(Frame.Resolution.X, Frame.Resolution.Y), GSlumpTileSize);

	// Every pass reads one state texture and writes the other. Track which one
	// currently holds the live state.
	FRDGTextureRef Current = StateA;
	FRDGTextureRef Other = StateB;
	FRDGTextureRef PondCur = PondA;
	FRDGTextureRef PondOther = PondB;

	const auto Flip = [&Current, &Other]() { Swap(Current, Other); };
	const auto FlipPond = [&PondCur, &PondOther]() { Swap(PondCur, PondOther); };

	// Per-step scoop shortfalls, written by the brush passes and read by the
	// parcel spawn passes of the same step.
	FRDGBufferRef Shortfall = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), DirtSim::MaxStrokesPerStep), TEXT("DirtScoopShortfall"));
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Shortfall), 0u);

	// The dirt pool's resources are bound whenever the pool exists, because the
	// slump pass binds them (it can shed grains); bParcels only decides whether
	// anything spawns, flies or sheds.
	const bool bPool = Frame.HasParcelPool();
	const bool bParcels = Frame.HasParcels();
	const bool bDust = Frame.HasDust();
	FParcelGraphResources Parcels;
	FParcelGraphResources Dust;
	if (bPool)
	{
		Parcels = AcquirePool(GraphBuilder, *Frame.Parcels, Frame.ParcelPos, Frame.ParcelVel, Frame.ParcelProp,
							  Frame.ParcelRes, Frame.Resolution, nullptr, TEXT("DirtParcel"));
	}
	if (bDust)
	{
		Dust = AcquirePool(GraphBuilder, *Frame.Dust, Frame.DustPos, Frame.DustVel, Frame.DustProp,
						   Frame.DustRes, Frame.Resolution, &Parcels, TEXT("DirtDust"));
	}

	const auto AddDepositPass = [&]()
	{
		FDirtDepositCS::FParameters* Params = GraphBuilder.AllocParameters<FDirtDepositCS::FParameters>();
		BindSharedParameters(Params, Frame, Current);
		BindParcelParameters(GraphBuilder, Params, Frame, Parcels);
		Params->DirtDepositCompaction = Frame.DepositCompaction;
		Params->DirtResetLiveMax = (Frame.SlumpIterations > 0) ? 1 : 0;
		Params->DirtStateOut = GraphBuilder.CreateUAV(Other);

		TShaderMapRef<FDirtDepositCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("DirtDeposit"), Shader, Params, GroupCount);
		Flip();
	};

	// --- slide the window ----------------------------------------------------
	// Parcels over leaving ground land first and are folded in, the leaving
	// tiles are read back for the cache, then everything shifts.
	if (Frame.ShiftTexels != FIntPoint::ZeroValue && !Frame.bReinitialise)
	{
		if (bPool && Frame.Parcels->bInitialised)
		{
			const auto AddFlush = [&](FDirtParcelResources& R, const FParcelGraphResources& G, bool bIsDust)
			{
				FDirtParcelFlushCS::FParameters* Params = GraphBuilder.AllocParameters<FDirtParcelFlushCS::FParameters>();
				BindSharedParameters(Params, Frame, Current);
				BindParcelParameters(GraphBuilder, Params, Frame, G);
				Params->DirtTexelSizeCm = Frame.TexelSizeCm;
				Params->DirtIsDust = bIsDust ? 1 : 0;
				Params->DirtShiftTexels = Frame.ShiftTexels;
				TShaderMapRef<FDirtParcelFlushCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FComputeShaderUtils::AddPass(GraphBuilder, bIsDust ? RDG_EVENT_NAME("DirtDustFlush") : RDG_EVENT_NAME("DirtParcelFlush"),
											 Shader, Params, FComputeShaderUtils::GetGroupCount(G.Res * G.Res, GParcelThreadGroupSize));
			};
			AddFlush(*Frame.Parcels, Parcels, false);
			if (bDust && Frame.Dust->bInitialised)
			{
				AddFlush(*Frame.Dust, Dust, true);
			}
			AddDepositPass();
		}

		for (const TSharedPtr<FDirtTileReadback, ESPMode::ThreadSafe>& RB : Frame.TileReadbacks)
		{
			if (!RB.IsValid() || RB->bEnqueued)
			{
				continue;
			}
			if (!RB->State)
			{
				RB->State = new FRHIGPUTextureReadback(TEXT("DirtTileState"));
				RB->Pond = new FRHIGPUTextureReadback(TEXT("DirtTilePond"));
			}
			const FResolveRect Rect(RB->OriginTexel.X, RB->OriginTexel.Y,
									RB->OriginTexel.X + RB->SizeTexels, RB->OriginTexel.Y + RB->SizeTexels);
			AddEnqueueCopyPass(GraphBuilder, RB->State, Current, Rect);
			AddEnqueueCopyPass(GraphBuilder, RB->Pond, PondCur, Rect);
			RB->bEnqueued = true;
		}

		FDirtShiftCS::FParameters* Params = GraphBuilder.AllocParameters<FDirtShiftCS::FParameters>();
		BindSharedParameters(Params, Frame, Current);
		Params->DirtShiftTexels = Frame.ShiftTexels;
		Params->DirtPatchState = Frame.InitialState;
		Params->DirtPatchPond = Frame.InitialPond;
		Params->DirtPondIn = PondCur;
		Params->DirtStateOut = GraphBuilder.CreateUAV(Other);
		Params->DirtPondOut = GraphBuilder.CreateUAV(PondOther);

		TShaderMapRef<FDirtShiftCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("DirtShift"), Shader, Params, GroupCount);
		Flip();
		FlipPond();
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

	// The pools are (re)initialised here, before anything reads their counters.
	if (bPool && (Frame.bReinitialise || Parcels.bFresh || !Frame.Parcels->bInitialised))
	{
		AddPoolInit(GraphBuilder, Frame, *Frame.Parcels, Parcels, Current, /*bIsDust*/ false, /*bClearDeposits*/ true);
	}
	if (bDust && (Frame.bReinitialise || Dust.bFresh || !Frame.Dust->bInitialised))
	{
		AddPoolInit(GraphBuilder, Frame, *Frame.Dust, Dust, Current, /*bIsDust*/ true, /*bClearDeposits*/ false);
	}

	// --- brush strokes, batched: the taking halves, then the giving halves ---------
	const auto AddBrushBatches = [&](const TArray<FDirtBrushStroke>& Strokes, const TCHAR* Name)
	{
		for (int32 First = 0; First < Strokes.Num(); First += DirtSim::MaxStrokesPerPass)
		{
			const int32 Count = FMath::Min(DirtSim::MaxStrokesPerPass, Strokes.Num() - First);

			FDirtBrushCS::FParameters* Params = GraphBuilder.AllocParameters<FDirtBrushCS::FParameters>();
			BindSharedParameters(Params, Frame, Current);
			Params->DirtStrokeCount = Count;
			Params->DirtStrokeBase = First;
			Params->DirtTexelAreaCm2 = Frame.TexelSizeCm * Frame.TexelSizeCm;
			Params->DirtScoopShortfall = GraphBuilder.CreateUAV(Shortfall);
			for (int32 K = 0; K < Count; ++K)
			{
				const FDirtBrushStroke& Stroke = Strokes[First + K];
				Params->DirtStrokeA[K] = FVector4f(Stroke.CenterTexel.X, Stroke.CenterTexel.Y,
												   Stroke.CoreRadiusTexels, Stroke.RimRadiusTexels);
				Params->DirtStrokeB[K] = FVector4f(Stroke.CoreNorm, Stroke.RimNorm, Stroke.Amount, Stroke.Disturb);
				Params->DirtStrokeC[K] = FVector4f(static_cast<float>(Stroke.Mode), Stroke.bProctor ? 1.0f : 0.0f,
												   static_cast<float>(Stroke.Link), 0.0f);
			}
			Params->DirtStateOut = GraphBuilder.CreateUAV(Other);

			TShaderMapRef<FDirtBrushCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("%s", Name), Shader, Params, GroupCount);
			Flip();
		}
	};
	AddBrushBatches(Frame.Strokes, TEXT("DirtBrushTake"));
	AddBrushBatches(Frame.GivingStrokes, TEXT("DirtBrushGive"));

	// --- slump iterations --------------------------------------------------
	if (bPool)
	{
		for (int32 Iteration = 0; Iteration < Frame.SlumpIterations; ++Iteration)
		{
			FDirtSlumpCS::FParameters* Params = GraphBuilder.AllocParameters<FDirtSlumpCS::FParameters>();
			BindSharedParameters(Params, Frame, Current);
			BindSoilParameters(Params, Frame);
			BindParcelParameters(GraphBuilder, Params, Frame, Parcels);
			Params->DirtSlumpRate = Frame.SlumpRate;
			Params->DirtLooseningRate = Frame.LooseningRate;
			Params->DirtLooseningScaleCm = Frame.LooseningScaleCm;
			// Shed only on the last iteration of a step: one chance per cell per step.
			Params->DirtShedEnabled = (bParcels && Frame.bShed && Iteration == Frame.SlumpIterations - 1) ? 1 : 0;
			Params->DirtShedMinOutCm = Frame.ShedMinOutCm;
			Params->DirtShedChance = Frame.ShedChance;
			Params->DirtShedFraction = Frame.ShedFraction;
			Params->DirtShedDiameterCm = Frame.ShedDiameterCm;
			Params->DirtShedSpeedCmS = Frame.ShedSpeedCmS;
			Params->DirtFrameSeed = Frame.FrameSeed * 977u + static_cast<uint32>(Iteration);
			Params->DirtStateOut = GraphBuilder.CreateUAV(Other);

			TShaderMapRef<FDirtSlumpCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("DirtSlump"), Shader, Params, SlumpGroupCount);
			Flip();
		}
	}

	// --- water -------------------------------------------------------------
	if (Frame.bWater && Frame.SlumpIterations > 0)
	{
		FDirtWaterCS::FParameters* Params = GraphBuilder.AllocParameters<FDirtWaterCS::FParameters>();
		BindSharedParameters(Params, Frame, Current);
		Params->DirtDt = Frame.Dt;
		Params->DirtRunoffRate = Frame.RunoffRate;
		Params->DirtRainCmPerSec = Frame.RainCmPerSec;
		Params->DirtTexelSizeCm = Frame.TexelSizeCm;
		Params->DirtErosionEnabled = Frame.bErosion ? 1 : 0;
		Params->DirtErosionPace = Frame.ErosionPace;
		Params->DirtDrainPerSec = Frame.DrainPerSec;
		Params->DirtEvapPerSec = Frame.EvapPerSec;
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

	// --- parcels and dust ----------------------------------------------------
	// With parcels switched off the spawn pass still runs, in deposit-only mode:
	// a throw lands where it started, through the same shortfall-corrected books.
	if (bPool)
	{
		AddPoolPasses(GraphBuilder, Frame, *Frame.Parcels, Parcels, Current, Frame.Spawns, /*bIsDust*/ false, Shortfall, /*bFly*/ bParcels);
	}
	if (bDust)
	{
		AddPoolPasses(GraphBuilder, Frame, *Frame.Dust, Dust, Current, Frame.DustSpawns, /*bIsDust*/ true, Shortfall, /*bFly*/ true);
	}

	// --- landed parcels join the ground ----------------------------------------
	// After the parcel sim, so what landed this step is in the ground before the
	// resolve: the audit reads the display texture and the live parcels, and
	// anything still sitting in the deposit accumulators would be in neither.
	// The pass also zeroes the highest-live-slot counter for the next step; the
	// readback copy of the counters was queued before it, so this step's value
	// still reaches the game thread.
	if (bPool)
	{
		AddDepositPass();
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

	// Keep freshly created pool bookkeeping alive across frames.
	if (bPool)
	{
		ExtractPool(GraphBuilder, *Frame.Parcels, Parcels, /*bOwnsDeposits*/ true);
	}
	if (bDust)
	{
		ExtractPool(GraphBuilder, *Frame.Dust, Dust, /*bOwnsDeposits*/ false);
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
