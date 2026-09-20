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
#pragma once

#include "CoreMinimal.h"
#include "DirtSimTypes.h"

class FRHICommandListImmediate;
class FTextureResource;
class FTextureRenderTargetResource;

/** One simulation step's worth of work and resources. Built on the game thread. */
struct FDirtSimFrame
{
	// --- grid --------------------------------------------------------------
	FIntPoint Resolution = FIntPoint(1024, 1024);
	float TexelSizeCm = 12.5f;

	// --- tunables ----------------------------------------------------------
	float LooseReposeDeg = 32.0f;
	float PackedReposeDeg = 70.0f;
	float MoistureCohesionDeg = 12.0f;
	float SaturatedPenaltyDeg = 17.0f;
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
	// Resource pointers are safe to hand across from the game thread because the
	// Dirtbox creates them once and never resizes them. The actual RHI textures
	// are pulled out on the render thread, where they belong.
	FTextureResource* BaseHeight = nullptr;             // R32F,    static bedrock
	FTextureResource* InitialState = nullptr;           // RGBA32F, CPU-built start state
	FTextureRenderTargetResource* StateA = nullptr;     // RGBA32F, ping
	FTextureRenderTargetResource* StateB = nullptr;     // RGBA32F, pong
	FTextureRenderTargetResource* Display = nullptr;    // RGBA32F, sampled by the material
	FTextureRenderTargetResource* NormalOut = nullptr;  // RGBA16F, encoded world normal
	FTextureRenderTargetResource* DebugOut = nullptr;   // RGBA16F, base colour / debug view

	bool IsValid() const
	{
		return BaseHeight && InitialState && StateA && StateB && Display && NormalOut && DebugOut;
	}
};

namespace DirtSim
{
	/** Run one simulation step. Render thread only. */
	void Execute_RenderThread(FRHICommandListImmediate& RHICmdList, const FDirtSimFrame& Frame);
}
