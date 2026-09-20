// DaDirt — procedural testbed terrain.
//
// Builds the bedrock shape and the initial dirt state on the CPU, once, at
// startup. Doing it on the CPU costs nothing (it happens once) and it means the
// whole test suite is plain readable code rather than shader math.
//
// The testbed is deliberately a measuring instrument, not a pretty landscape.
// It lays out a full spectrum of angles, a range of jump-lip sharpnesses, big
// dramatic hills, a flat calibration pad, and concave/convex pairs — so that
// every dirt behaviour we care about has something in the box that provokes it.
#pragma once

#include "CoreMinimal.h"
#include "DirtSimTypes.h"

/** Dirt properties stamped along with a terrain feature. */
struct FDirtMaterial
{
	/** 0 = loose fluffy, 1 = packed hardpack. */
	float Compaction = 0.25f;

	/** 0 = bone dry, 1 = saturated. */
	float Moisture = 0.2f;

	/** How much loose dirt sits on this feature's bedrock, in cm. */
	float LayerCm = 60.0f;
};

/** One wedge of the angle spectrum, in metres relative to the box centre. */
struct FDirtAngleWedge
{
	float AngleDeg = 0.0f;
	float CentreY = 0.0f;
	float WidthM = 0.0f;
	float ToeX = 0.0f;
	float CrestX = 0.0f;
	/** Halfway up the face — where a test drops loose dirt to see if it holds. */
	float FaceMidX = 0.0f;
	float PeakHeightM = 0.0f;
};

/**
 * Generates the testbed. Output is three flat arrays, row-major, SimResolution
 * per side, all in centimetres / 0-1 units ready to upload to the GPU.
 */
class FDirtTestbed
{
public:
	explicit FDirtTestbed(const FDirtSimSettings& InSettings);

	/** Build the whole testbed into the output arrays. */
	void Build();

	/** Bedrock height per cell, cm relative to the DirtBox origin. */
	TArray<float> Bedrock;

	/** Dirt layer thickness per cell, cm above bedrock. */
	TArray<float> Layer;

	/** Compaction per cell, 0-1. */
	TArray<float> Compaction;

	/** Moisture per cell, 0-1. */
	TArray<float> Moisture;

	/** Total dirt volume in the layer at build time, cubic metres. The volume
	 *  audit compares against this. */
	double BaselineVolumeM3 = 0.0;

	/** Human-readable description of every feature, for the console. */
	TArray<FString> FeatureLog;

	/**
	 * Layout of the angle-spectrum wedges. Single source of truth: the builder
	 * stamps them from this, and the scripted tests aim at them using the same
	 * numbers, so the two can never drift apart.
	 */
	static void GetAngleSpectrum(const FDirtSimSettings& Settings, TArray<FDirtAngleWedge>& Out);

private:
	// --- grid helpers ------------------------------------------------------

	/** World position in metres of a cell centre, relative to the box centre. */
	FVector2f CellToMetres(int32 X, int32 Y) const;

	/** Cell containing a world position. May fall outside the grid. */
	FIntPoint MetresToCell(const FVector2f& M) const;

	/** Cell index range covering a metres-space axis-aligned box, clamped. */
	void MetresToCellRect(FVector2f MinM, FVector2f MaxM, FIntPoint& OutMin, FIntPoint& OutMax) const;

	int32 Index(int32 X, int32 Y) const { return Y * Resolution + X; }

	/** Raise bedrock to NewHeight if that is higher, and claim the cell's material. */
	void RaiseTo(int32 X, int32 Y, float NewHeightM, const FDirtMaterial& Mat);

	/** Lower bedrock to NewHeight if that is lower, and claim the cell's material. */
	void LowerTo(int32 X, int32 Y, float NewHeightM, const FDirtMaterial& Mat);

	// --- feature primitives -----------------------------------------------

	/**
	 * A ridge running across Y: rises from the toe at FrontAngleDeg to a crest,
	 * then falls away at BackAngleDeg. Used for the angle spectrum, jump takeoffs
	 * and jump landings alike — only the angles and lip change.
	 */
	void AddWedge(float CentreY, float WidthM, float ToeX, float FrontAngleDeg,
				  float MaxHeightM, float FrontRunLimitM, float BackAngleDeg,
				  float LipRoundM, const FDirtMaterial& Mat);

	/** Smooth cosine dome. The big hills. */
	void AddDome(FVector2f CentreM, float RadiusM, float HeightM, const FDirtMaterial& Mat);

	/** Smooth ridge running along the Y axis. */
	void AddLongRidge(float CentreX, float HalfWidthM, float HeightM, float FromY, float ToY,
					  const FDirtMaterial& Mat);

	/** Flat-topped pad. Bedrock only — use Mat.LayerCm for the dirt on it. */
	void AddPad(FVector2f CentreM, FVector2f HalfExtentM, float HeightM, float EdgeFalloffM,
				const FDirtMaterial& Mat);

	/** Cone of loose dirt, stamped into the dirt layer rather than bedrock, so it
	 *  is free to collapse. Steeper than repose on purpose. */
	void AddLooseCone(FVector2f CentreM, float RadiusM, float HeightM, const FDirtMaterial& Mat);

	/** Bowl dug down into bedrock. Tests dirt collecting in a hollow. */
	void AddBowl(FVector2f CentreM, float RadiusM, float DepthM, const FDirtMaterial& Mat);

	/**
	 * Repeating cosine bumps across a strip. Covers both FIM 047.3.7 rolling waves
	 * (~10 m apart, <= 80 cm) and Supercross whoops (~4.3 m apart, ~90 cm) — which
	 * are two genuinely different obstacles, and whoops are banned outdoors.
	 */
	void AddWaveStrip(float CentreY, float WidthM, float FromX, float ToX,
					  float SpacingM, float HeightM, const FDirtMaterial& Mat);

	/** Banked arc — the outside wall of a corner. */
	void AddBermArc(FVector2f CentreM, float RadiusM, float FromAngleDeg, float ToAngleDeg,
					float BankAngleDeg, float HeightM, const FDirtMaterial& Mat);

	// --- the composed testbed ---------------------------------------------

	void BuildAngleSpectrum();
	void BuildJumpLineup();
	void BuildCalibrationPad();
	void BuildHills();
	void BuildBermAndBowls();
	void BuildWaveSections();

	void Log(const FString& Line) { FeatureLog.Add(Line); }

	FDirtSimSettings Settings;
	int32 Resolution = 0;
	/** Centre of the simulated region in world metres, and half its side length.
	 *  Feature layout stays in world coordinates; only this mapping changes when
	 *  the simulation is pointed at part of the box. */
	FVector2f RegionCentreM = FVector2f::ZeroVector;
	float HalfRegionM = 0.0f;
	float MetresPerCell = 0.0f;
};
