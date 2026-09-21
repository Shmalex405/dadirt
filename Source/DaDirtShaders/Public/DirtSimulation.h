// DaDirt — render-thread side of the dirt simulation.
//
// The game thread fills in an FDirtSimFrame and hands it to the render thread,
// which builds one render-graph pass list per simulation step:
//
//     [init] -> [deposit] -> [brush] x batches -> [slump] x iterations
//            -> [water] -> [parcel spawn] -> [parcel sim]
//            -> [dust spawn] -> [dust sim] -> [resolve]
//
// The brush, slump and water passes ping-pong between two state textures (the
// water pass also between two pond textures). Resolve reads whichever one ended
// up holding the answer and publishes it to fixed display, normal and debug
// targets, so the ground material always has stable textures to sample.
//
// This lives in the DaDirtShaders module, not the game module, because global
// shader classes must be registered before the engine builds its shader maps —
// which means a module that loads at PostConfigInit. For the same reason this
// module cannot depend on Engine, so it deals only in raw RHI textures; the
// Dirtbox pulls those out of its UTexture resources on the render thread.
#pragma once

#include "CoreMinimal.h"
#include <atomic>
#include "RenderGraphResources.h"

namespace DirtSim
{
	/** Soils in the table and float4 rows per soil. Mirrors DIRT_MAX_SOILS / DIRT_SOIL_ROWS in DirtCommon.ush. */
	constexpr int32 MaxSoils = 8;
	constexpr int32 SoilRows = 5;
}

class FRHICommandListImmediate;
class FRHITexture;

/**
 * One deformation stroke, already converted from world space into grid space.
 * Mode holds an EDirtBrushMode value; it is an int here so this header does not
 * need the game module's reflected types.
 */
struct FDirtBrushStroke
{
	/** Centre in texel coordinates. */
	FVector2f CenterTexel = FVector2f::ZeroVector;

	float CoreRadiusTexels = 8.0f;
	float RimRadiusTexels = 16.0f;

	/**
	 * Kernel amplitude. For the mass modes this is SOLID centimetres, already
	 * scaled so that the peak of the core equals the requested depth — see
	 * ADirtBox::MakeStroke.
	 */
	float Amount = 0.0f;

	/** Discrete sums of the two kernels over the footprint. These are what make
	 *  dig and raise conserve volume exactly on the grid. */
	float CoreNorm = 1.0f;
	float RimNorm = 1.0f;

	float Disturb = 0.0f;

	/** EDirtBrushMode as an integer. Must match DirtSim.usf. */
	int32 Mode = 0;

	/** Pack strokes only: scale by the Proctor moisture curve (a tyre) or not (a tool). */
	bool bProctor = false;

	/**
	 * Giving halves only: index within the step's taking strokes of the stroke
	 * whose shortfall scales this one, or -1. See DirtSim.usf, ApplyStroke.
	 */
	int32 Link = -1;
};

/** Water poured on the surface this step, in grid space. */
struct FDirtWaterSource
{
	FVector2f CenterTexel = FVector2f::ZeroVector;
	float RadiusTexels = 8.0f;
	/** Depth added per unit of kernel weight: litres x 1000 / texel area / kernel sum. Exact litres. */
	float AmountCm = 0.0f;
};

/** One throw of dirt into the air: becomes Count parcels sharing VolumeCm3. */
struct FDirtParcelSpawn
{
	/** Box-relative position, cm. */
	FVector3f PositionCm = FVector3f::ZeroVector;
	FVector3f VelocityCmS = FVector3f::ZeroVector;
	float VolumeCm3 = 0.0f;
	int32 Count = 1;
	float SpreadDeg = 15.0f;
	float SpeedJitter = 0.25f;
	float Moisture = 0.2f;
	float Compaction = 0.1f;
	float DiameterCm = 1.0f;
	float DiameterJitter = 0.4f;
	uint32 Seed = 0;
	/** Index within this step's strokes of the Scoop this throw came from, or -1. */
	int32 ScoopStrokeIndex = -1;
};

/**
 * GPU-side bookkeeping for one parcel pool that has no UTexture equivalent: the
 * free-list stack and its counters. Created lazily on the render thread and kept
 * for the life of the Dirtbox. The counters are read back to the game thread a
 * frame or two late through Readback. The dirt pool also owns the fixed-point
 * deposit accumulators; the dust pool never deposits and shares them.
 */
struct DADIRTSHADERS_API FDirtParcelResources
{
	TRefCountPtr<FRDGPooledBuffer> FreeList;
	TRefCountPtr<FRDGPooledBuffer> Counters;
	TRefCountPtr<IPooledRenderTarget> DepositVol;
	TRefCountPtr<IPooledRenderTarget> DepositMoist;
	int32 ParcelRes = 0;
	FIntPoint DepositRes = FIntPoint::ZeroValue;
	bool bInitialised = false;

	/** Latest counters seen by the render thread; see DirtSim::ParcelCounter*. */
	uint32 Counters_RT[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	/** Copied under CounterLock for the game thread. */
	FCriticalSection CounterLock;
	uint32 Counters_Shared[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

	class FRHIGPUBufferReadback* Readback = nullptr;
	bool bReadbackPending = false;

	~FDirtParcelResources();
};

/**
 * A tile of the window to read back before it slides out, and where to put it.
 * The readbacks land a frame or two later; the Dirtbox harvests them.
 */
struct DADIRTSHADERS_API FDirtTileReadback
{
	FIntPoint Tile = FIntPoint::ZeroValue;      // tile coordinates, for the cache
	FIntPoint OriginTexel = FIntPoint::ZeroValue;   // in the window BEFORE the shift
	int32 SizeTexels = 256;
	class FRHIGPUTextureReadback* State = nullptr;
	class FRHIGPUTextureReadback* Pond = nullptr;
	bool bEnqueued = false;

	/** Filled on the render thread once the copies have landed; bDone last. */
	TArray<FLinearColor> ResultState;
	TArray<float> ResultPond;
	std::atomic<bool> bDone{ false };

	~FDirtTileReadback();
};

/** One simulation step's worth of work and resources. Built on the game thread. */
struct FDirtSimFrame
{
	// --- grid --------------------------------------------------------------
	FIntPoint Resolution = FIntPoint(1024, 1024);
	float TexelSizeCm = 12.5f;
	/** Box-relative xy of the corner of texel (0,0), cm. Parcels live in box space. */
	FVector2f RegionOriginCm = FVector2f::ZeroVector;
	float Dt = 1.0f / 60.0f;
	uint32 FrameSeed = 0;

	// --- tunables ----------------------------------------------------------
	/**
	 * The soil table: DirtSim::MaxSoils soils x DirtSim::SoilRows float4 rows,
	 * laid out exactly as DirtCommon.ush documents (FDirtSoil::ToRows fills it).
	 * Every cell reads its own soil through the SoilIn texture.
	 */
	TArray<FVector4f> SoilTable;
	float MaxCohesiveHeightCm = 400.0f;
	float SlumpRate = 0.1f;
	float LooseningRate = 0.25f;
	float LooseningScaleCm = 2.0f;
	int32 SlumpIterations = 3;

	// solid volume
	float CompactionDepthCm = 15.0f;
	float DeepCompaction = 0.25f;
	float DepositCompaction = 0.05f;

	// water
	bool bWater = true;
	float RunoffRate = 0.5f;
	float RainCmPerSec = 0.0f;
	float DrainPerSec = 0.025f;
	float EvapPerSec = 0.0007f;
	float WetDepthCm = 20.0f;
	float AmbientMoisture = 0.05f;

	// parcels
	bool bParcels = true;
	int32 ParcelRes = 512;
	float GravityCmS2 = 981.0f;
	float ParcelDragK = 2.6e-4f;
	float ParcelRestitutionDry = 0.3f;
	float ParcelRestitutionWet = 0.02f;
	float ParcelRestSpeedCmS = 15.0f;
	float ParcelRestSeconds = 0.12f;

	// grains shedding down a face (spawned by the slump pass)
	bool bShed = true;
	float ShedMinOutCm = 0.05f;
	float ShedChance = 0.15f;
	float ShedFraction = 0.5f;
	float ShedDiameterCm = 0.6f;
	float ShedSpeedCmS = 60.0f;

	// dust
	bool bDust = true;
	int32 DustRes = 256;
	float DustLifetime = 2.5f;
	float DustDragK = 0.0008f;
	float DustBuoyancy = 1.03f;

	// --- debug -------------------------------------------------------------
	int32 DebugMode = 0;
	float DebugLayerRangeCm = 120.0f;

	// --- work --------------------------------------------------------------
	/** Deformation strokes to apply before slumping, in order: the taking halves. */
	TArray<FDirtBrushStroke> Strokes;

	/** The giving halves, dispatched after every taking stroke has reported its shortfall. */
	TArray<FDirtBrushStroke> GivingStrokes;

	/** Dirt thrown into the air this step. */
	TArray<FDirtParcelSpawn> Spawns;

	/** Dust puffed into the air this step (volume is a placeholder, never audited). */
	TArray<FDirtParcelSpawn> DustSpawns;

	/** Water poured this step. */
	TArray<FDirtWaterSource> WaterSources;

	/** Reseed the state from InitialState before doing anything else. */
	bool bReinitialise = false;

	/**
	 * Slide the window by this many texels (old texel = new texel + shift)
	 * before anything else this step. Entering cells come from InitialState and
	 * InitialPond, which the CPU filled for exactly those cells. Parcels over
	 * leaving ground land first, and the leaving tiles are read back for the
	 * cache before the slide.
	 */
	FIntPoint ShiftTexels = FIntPoint::ZeroValue;
	TArray<TSharedPtr<FDirtTileReadback, ESPMode::ThreadSafe>> TileReadbacks;

	// --- resources ---------------------------------------------------------
	// Raw RHI textures. Fill these in ON THE RENDER THREAD, from the texture
	// resources the Dirtbox owns; they are only valid there.
	FRHITexture* BaseHeight = nullptr;     // R32F,    static bedrock
	FRHITexture* SoilIn = nullptr;         // R8_UINT, the soil id of every cell (static, CPU-built)
	FRHITexture* InitialState = nullptr;   // RGBA32F, CPU-built start state, or the entering patch on a shift
	FRHITexture* InitialPond = nullptr;    // R32F, entering pond on a shift
	FRHITexture* StateA = nullptr;         // RGBA32F, ping
	FRHITexture* StateB = nullptr;         // RGBA32F, pong
	FRHITexture* PondA = nullptr;          // R32F, ponded water ping
	FRHITexture* PondB = nullptr;          // R32F, pong
	FRHITexture* Display = nullptr;        // RGBA32F, sampled by the material
	FRHITexture* NormalOut = nullptr;      // RGBA16F, encoded world normal
	FRHITexture* DebugOut = nullptr;       // RGBA16F, base colour / debug view

	FRHITexture* ParcelPos = nullptr;      // RGBA32F, ParcelRes^2
	FRHITexture* ParcelVel = nullptr;
	FRHITexture* ParcelProp = nullptr;

	FRHITexture* DustPos = nullptr;        // RGBA32F, DustRes^2
	FRHITexture* DustVel = nullptr;
	FRHITexture* DustProp = nullptr;

	/** Owned by the Dirtbox, used only on the render thread. */
	FDirtParcelResources* Parcels = nullptr;
	FDirtParcelResources* Dust = nullptr;

	bool IsValid() const
	{
		return BaseHeight && SoilIn && InitialState && InitialPond && StateA && StateB && PondA && PondB && Display && NormalOut && DebugOut
			&& SoilTable.Num() == DirtSim::MaxSoils * DirtSim::SoilRows;
	}

	/** The dirt pool exists (its resources are bound even when parcels are switched off: the slump pass needs them). */
	bool HasParcelPool() const
	{
		return Parcels && ParcelPos && ParcelVel && ParcelProp && ParcelRes > 0;
	}

	bool HasParcels() const
	{
		return bParcels && HasParcelPool();
	}

	bool HasDust() const
	{
		return bDust && HasParcelPool() && Dust && DustPos && DustVel && DustProp && DustRes > 0;
	}
};

namespace DirtSim
{
	/** Strokes applied per brush pass. Must match DIRT_MAX_STROKES in DirtSim.usf. */
	constexpr int32 MaxStrokesPerPass = 16;

	/** Spawn requests per spawn pass. Must match DIRT_MAX_SPAWNS in DirtParcels.usf. */
	constexpr int32 MaxSpawnsPerPass = 16;

	/** Water sources per water pass. Must match DIRT_MAX_WATER_SOURCES in DirtSim.usf. */
	constexpr int32 MaxWaterSourcesPerPass = 16;

	/** Strokes per step that can report a scoop shortfall. Must match DIRT_MAX_STROKES_PER_STEP. */
	constexpr int32 MaxStrokesPerStep = 256;

	/** Fixed-point steps per cm^3 in the deposit textures. Must match DIRT_DEPOSIT_SCALE. */
	constexpr float DepositScale = 65536.0f;

	/** Parcel counter slots. Must match DIRT_PARCEL_COUNTER_* in DirtCommon.ush. */
	constexpr int32 ParcelCounterFree = 0;
	constexpr int32 ParcelCounterSpawned = 1;
	constexpr int32 ParcelCounterLanded = 2;
	constexpr int32 ParcelCounterFallback = 3;
	constexpr int32 ParcelCounterMaxLive = 4;
	constexpr int32 ParcelCounterCount = 8;

	/** Run one simulation step. Render thread only. */
	DADIRTSHADERS_API void Execute_RenderThread(FRHICommandListImmediate& RHICmdList, const FDirtSimFrame& Frame);

	/** Kick or harvest the non-blocking counter readback. Render thread only. */
	DADIRTSHADERS_API void UpdateParcelCounters_RenderThread(FRHICommandListImmediate& RHICmdList, FDirtParcelResources& Parcels);
}
