// DaDirt — render-thread side of the dirt simulation.
//
// The game thread fills in an FDirtSimFrame and hands it to the render thread,
// which builds one render-graph pass list per simulation step:
//
//     [init] -> [brush] x strokes -> [slump] x iterations -> [resolve]
//
// The brush and slump passes ping-pong between two state textures. Resolve reads
// whichever one ended up holding the answer and publishes it to fixed display,
// normal and debug targets, so the ground material always has stable textures to
// sample.
//
// This lives in the DaDirtShaders module, not the game module, because global
// shader classes must be registered before the engine builds its shader maps —
// which means a module that loads at PostConfigInit. For the same reason this
// module cannot depend on Engine, so it deals only in raw RHI textures; the
// Dirtbox pulls those out of its UTexture resources on the render thread.
#pragma once

#include "CoreMinimal.h"

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
	 * Kernel amplitude. Already scaled so that the peak of the core equals the
	 * requested depth in cm — see ADirtBox::MakeStroke.
	 */
	float Amount = 0.0f;

	/** Discrete sums of the two kernels over the footprint. These are what make
	 *  dig and raise conserve volume exactly on the grid. */
	float CoreNorm = 1.0f;
	float RimNorm = 1.0f;

	float Disturb = 0.0f;

	/** EDirtBrushMode as an integer. Must match DirtSim.usf. */
	int32 Mode = 0;
};

/** One simulation step's worth of work and resources. Built on the game thread. */
struct FDirtSimFrame
{
	// --- grid --------------------------------------------------------------
	FIntPoint Resolution = FIntPoint(1024, 1024);
	float TexelSizeCm = 12.5f;

	// --- tunables ----------------------------------------------------------
	float LooseReposeDeg = 32.0f;
	float PackedReposeDeg = 42.0f;         // friction angle of dense dirt
	float SuctionCohesionKPa = 3.0f;       // apparent cohesion from moisture, peak
	float PackedCohesionKPa = 8.0f;        // interlock/cementation when fully packed
	float UnitWeightKNm3 = 17.0f;
	float SaturationFrictionLoss = 0.6f;
	float MaxCohesiveHeightCm = 400.0f;
	float SlumpRate = 0.1f;
	float LooseningRate = 0.25f;
	float LooseningScaleCm = 2.0f;
	int32 SlumpIterations = 3;

	// --- debug -------------------------------------------------------------
	int32 DebugMode = 0;
	float DebugLayerRangeCm = 120.0f;

	// --- work --------------------------------------------------------------
	/** Deformation strokes to apply before slumping, in order. */
	TArray<FDirtBrushStroke> Strokes;

	/** Reseed the state from InitialState before doing anything else. */
	bool bReinitialise = false;

	// --- resources ---------------------------------------------------------
	// Raw RHI textures. Fill these in ON THE RENDER THREAD, from the texture
	// resources the Dirtbox owns; they are only valid there.
	FRHITexture* BaseHeight = nullptr;     // R32F,    static bedrock
	FRHITexture* InitialState = nullptr;   // RGBA32F, CPU-built start state
	FRHITexture* StateA = nullptr;         // RGBA32F, ping
	FRHITexture* StateB = nullptr;         // RGBA32F, pong
	FRHITexture* Display = nullptr;        // RGBA32F, sampled by the material
	FRHITexture* NormalOut = nullptr;      // RGBA16F, encoded world normal
	FRHITexture* DebugOut = nullptr;       // RGBA16F, base colour / debug view

	bool IsValid() const
	{
		return BaseHeight && InitialState && StateA && StateB && Display && NormalOut && DebugOut;
	}
};

namespace DirtSim
{
	/** Strokes applied per brush pass. Must match DIRT_MAX_STROKES in DirtSim.usf. */
	constexpr int32 MaxStrokesPerPass = 16;

	/** Run one simulation step. Render thread only. */
	DADIRTSHADERS_API void Execute_RenderThread(FRHICommandListImmediate& RHICmdList, const FDirtSimFrame& Frame);
}
