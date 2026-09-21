// DaDirt — real-scale motocross track generator.
//
// Builds a full FIM-legal outdoor motocross circuit as bedrock: 1,520 m along the
// centre line, 8 m wide, on a 384 m site with about 22 m of elevation change.
// Every number traces back to docs/MXTrackReference.md.
//
// How it works:
//   1. A closed Catmull-Rom spline through 36 hand-placed control points gives the
//      centre line. It is a serpentine — four passes across the site linked by
//      U-turns, plus a long outer return — because that is how a real track folds
//      1.5 km onto a site that is only 330 m across.
//   2. The spline is sampled into a polyline carrying arc length (s), tangent,
//      normal and curvature.
//   3. Every cell finds its nearest point on that polyline, giving (s, t): how far
//      round the lap it is, and how far sideways off the centre line.
//   4. Height is then a function of (s, t): the elevation profile at s, plus the
//      corridor cross-section at t, plus whatever feature occupies that stretch of s.
//
// Berms are not placed by hand. They are generated from the curvature of the centre
// line and appear on the OUTSIDE of every corner, sized by how tight the corner is —
// which is exactly where and why they form on a real track.
#pragma once

#include "CoreMinimal.h"
#include "DirtSimTypes.h"
#include "DirtTestbed.h"

/** The obstacle types the track generator knows how to build. */
enum class EDirtTrackFeature : uint8
{
	/** Up-face, flat table, down-face. The forgiving one. */
	Tabletop,
	/** Take-off, gap, landing. Miss it and you case the landing. */
	Double,
	/** Take-off, gap, mound, gap, landing — cleared in one 20-23 m leap. */
	Triple,
	/** Steep face onto a plateau the track stays up on. */
	StepUp,
	/** Drop off a plateau the track stays down on. */
	StepDown,
	/** FIM 047.3.7 rolling waves: ~10 m peak to peak, <= 80 cm tall. Legal outdoors. */
	RollingWaves,
	/** Supercross whoops: ~4.3 m peak to peak, ~90 cm tall. Illegal outdoors. */
	Whoops,
	/** No shape change — swaps the soil to deep, dry, loose sand. */
	SandSection,
};

/** One obstacle, placed by distance along the lap. */
struct FDirtTrackFeature
{
	EDirtTrackFeature Type = EDirtTrackFeature::Tabletop;

	/** Distance along the centre line where it starts, in metres. */
	float StartS = 0.0f;

	/** How much of the lap it occupies, in metres. */
	float LengthM = 0.0f;

	/** Peak height above the track surface, in metres. */
	float HeightM = 2.0f;

	/** Take-off face angle in degrees. 45 gives maximum distance. */
	float FaceAngleDeg = 32.0f;

	/** How much the lip is rounded off, in metres. 0 is a sharp kicker. */
	float LipRoundM = 0.5f;

	/** Gap for doubles and triples, or wave spacing for the repeating features. */
	float GapM = 9.0f;

	const TCHAR* Name = TEXT("");
};

/** Generates the track into the same four arrays the testbed uses. */
class FDirtTrack
{
public:
	explicit FDirtTrack(const FDirtSimSettings& InSettings);

	/** Borrow-pit radius settled by the last whole-site build, reused by partial builds. */
	static float SitePitRadiusM;

	void Build();

	TArray<float> Bedrock;      // cm
	TArray<float> Layer;        // cm
	TArray<float> Compaction;   // 0-1
	TArray<float> Moisture;     // 0-1

	double BaselineVolumeM3 = 0.0;
	TArray<FString> FeatureLog;

	/** Total lap distance in metres, measured along the centre line. */
	float LapLengthM = 0.0f;

	// --- earthmoving ledger, in cubic metres ------------------------------
	double CutM3 = 0.0;        // taken out of the corridor
	double FillM3 = 0.0;       // put back into the corridor
	double BuiltM3 = 0.0;      // placed into jumps and berms
	double BorrowedM3 = 0.0;   // dug out of the borrow pits
	double ImportedM3 = 0.0;   // trucked in because the site could not supply it

	/** World position and heading of the start gate, for placing the camera. */
	FVector2f StartPositionM = FVector2f::ZeroVector;
	float StartHeadingRad = 0.0f;

	/** The smallest corner radius anywhere on the lap, in metres. */
	float MinCornerRadiusM = 0.0f;

	float MinElevationM = 0.0f;
	float MaxElevationM = 0.0f;

	/** Recommended world box for this track, in centimetres. */
	static float RecommendedWorldSizeCm() { return 38400.0f; }   // 384 m

private:
	/** One sample of the centre line. */
	struct FSample
	{
		FVector2f Pos = FVector2f::ZeroVector;   // metres
		FVector2f Normal = FVector2f::ZeroVector; // unit, pointing left of travel
		float S = 0.0f;                           // metres along the lap
		float Curvature = 0.0f;                   // 1/m, signed: + turns left
	};

	void BuildCentreline();
	void BuildFeatureList();
	void StampTrack();

	/** The site before anyone touched it, in metres. A function of world position
	 *  only — it does not know where the track is. */
	static float NaturalGroundM(const FVector2f& PosM);

	/** Smooth the natural ground along the centre line into a rideable grade, and
	 *  clamp it to a gradient a bike can actually climb. */
	void BuildDesignProfile();

	/** The graded design height at a distance along the lap, in metres. */
	float DesignAtS(float S) const;

	void GradeCorridor();
	void BuildFeaturesAndBerms();
	void DigBorrowPits();
	void Finalise();

	/** Cross-section of the corridor: track surface, shoulder, neutral-zone banking. */
	float CorridorProfile(float LateralM, float CurvatureAtS, FDirtMaterial& OutMat) const;

	/** Height a feature adds at a given distance into it, across the track. */
	float FeatureProfile(const FDirtTrackFeature& F, float IntoS, float LateralM,
						 FDirtMaterial& OutMat, bool& bOutOverridesMaterial) const;

	int32 Index(int32 X, int32 Y) const { return Y * Resolution + X; }
	FVector2f CellToMetres(int32 X, int32 Y) const;

	/** Cell containing a world position. May fall outside the grid. */
	FIntPoint MetresToCell(const FVector2f& M) const;

	void Log(const FString& Line) { FeatureLog.Add(Line); }

	FDirtSimSettings Settings;
	int32 Resolution = 0;
	/** Centre of the simulated region in world metres, and half its side length.
	 *  The track layout stays in world coordinates; only this mapping changes when
	 *  the simulation is pointed at part of the site. */
	FVector2f RegionCentreM = FVector2f::ZeroVector;
	float HalfRegionM = 0.0f;
	float MetresPerCell = 0.0f;

	TArray<FSample> Centreline;
	TArray<FDirtTrackFeature> Features;

	/** Design grade per centre-line sample, metres. */
	TArray<float> DesignM;

	/** The ground surface as the machines leave it, metres. */
	TArray<float> SurfaceM;

	/** Material claimed per cell while building. */
	TArray<FDirtMaterial> CellMaterial;

	/** Per-cell nearest-centreline results, filled by StampTrack. */
	TArray<float> CellS;
	TArray<float> CellT;
	TArray<float> CellDist;
	TArray<float> CellCurvature;
};
