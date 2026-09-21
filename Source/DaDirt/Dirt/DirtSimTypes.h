// DaDirt — dirt simulation types and tunables.
#pragma once

#include "CoreMinimal.h"
#include "DirtSimulation.h"
#include "DirtSimTypes.generated.h"

/** What a deformation stroke does to the dirt. Values must match DirtSim.usf. */
UENUM(BlueprintType)
enum class EDirtBrushMode : uint8
{
	/** Scoop dirt out of the core and heap it on the rim. Zero-sum. */
	Dig     = 0,
	/** Pile dirt up in the core, borrowing it from the rim. Zero-sum. */
	Raise   = 1,
	/** Pull the surface toward its local average. Roughly zero-sum. */
	Smooth  = 2,
	/** Add moisture. Moves no dirt. Past saturation the water pass ponds the excess. */
	Wet     = 3,
	/** Compact the dirt. Moves no dirt, but shrinks it: the surface drops. */
	Pack    = 4,
	/** Break up hardpack. Moves no dirt, but fluffs it: the surface rises. */
	Loosen  = 5,
	/**
	 * Remove dirt from the core with NO rim: it goes somewhere else, via a paired
	 * Dump of the same volume. Amount is volume in cm x texel^2 (see
	 * ADirtBox::TransferDirt). This is how a spinning tyre throws dirt backwards.
	 */
	Scoop   = 6,
	/** Add dirt to the core with no rim; the other half of a Scoop. */
	Dump    = 7
	// The shader also knows 8 (the rim half of a Dig) and 10 (the core half of a
	// Raise): giving halves the Dirtbox emits itself, scaled by what the taking
	// half found. Not requestable from outside.
};

/** What terrain the Dirtbox builds. */
UENUM(BlueprintType)
enum class EDirtTerrainMode : uint8
{
	/**
	 * The 128 m measuring instrument: an angle spectrum, jump-lip comparison,
	 * calibration pad and hills. 12.5 cm cells. This is where dirt gets tuned.
	 */
	Testbed = 0,
	/**
	 * A real FIM-legal motocross circuit at true scale: 1,520 m lap, 8 m wide,
	 * on a 384 m site. 37.5 cm cells, so this is for judging layout, scale and
	 * flow - not for close-up dirt behaviour. See docs/MXTrackReference.md.
	 */
	Track = 1
};

/** Which channel the ground is coloured by. Values must match DirtSim.usf. */
UENUM(BlueprintType)
enum class EDirtDebugView : uint8
{
	/** Plain dirt, shaded by compaction and moisture. */
	Dirt        = 0,
	/** Heatmap of how much loose dirt sits above bedrock. */
	LayerDepth  = 1,
	/** Pale = loose, dark red = packed. */
	Compaction  = 2,
	/** Pale = dry, blue = saturated. */
	Moisture    = 3,
	/** Green = stable, red = steeper than this dirt can hold. The angle-fan view. */
	Stability   = 4,
	/** Red wherever the dirt layer has been scraped down to bedrock. */
	Bedrock     = 5,
	/** Absolute surface slope, 0 to 90 degrees. */
	SlopeAngle  = 6
};

/**
 * Everything tunable about the dirt. Dirt feel is the whole game, so all of this
 * is exposed on the DirtBox actor and most of it is reachable from the console.
 */
USTRUCT(BlueprintType)
struct FDirtSimSettings
{
	GENERATED_BODY()

	// --- grid -------------------------------------------------------------

	/**
	 * Side length of the box in centimetres. 12800 = 128 m, which is big enough
	 * for real hills and a rhythm section. Cell size is this divided by
	 * SimResolution, so 128 m at 1024 gives 12.5 cm cells.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Grid", meta = (ClampMin = "1000"))
	float WorldSizeCm = 12800.0f;

	/**
	 * Simulation grid resolution per side. 1024 is the budgeted ceiling on the
	 * Arc iGPU. Drop to 512 if the sim is slow; raise to 2048 only after
	 * measuring, and expect roughly 4x the sim cost.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Grid", meta = (ClampMin = "64", ClampMax = "4096"))
	int32 SimResolution = 1024;

	/**
	 * Vertices per side of the display mesh. 512 gives 25 cm vertex spacing at a
	 * 128 m box — coarser than the 12.5 cm sim, so very sharp lips will look
	 * slightly stepped until this goes up. 512x512 is ~523k triangles.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Grid", meta = (ClampMin = "16", ClampMax = "2048"))
	int32 MeshVertsPerSide = 512;

	/**
	 * Side length of the region actually simulated, in centimetres. 0 means the
	 * whole box.
	 *
	 * This is the knob that makes ruts possible. The grid is fixed at
	 * SimResolution, so cell size is this divided by it — point the same 1024
	 * cells at a smaller piece of the world and the cells get finer:
	 *
	 *     whole 384 m track   -> 37.5 cm cells, a 12 cm rut is a third of a cell
	 *     51.2 m region       ->  5.0 cm cells, a 12 cm rut is ~2.5 cells
	 *
	 * The terrain under the region is regenerated at full resolution for wherever
	 * it is pointed, so nothing is upsampled or smeared.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Grid", meta = (ClampMin = "0"))
	float SimRegionSizeCm = 0.0f;

	/** Centre of the simulated region, in cm relative to the Dirtbox origin. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Grid")
	FVector2D SimRegionCentreCm = FVector2D::ZeroVector;

	// --- dirt layer -------------------------------------------------------

	/**
	 * How much loose dirt sits on bedrock everywhere at startup, in cm. This is
	 * the dig budget: scrape away 60 cm and you are down to hardpack.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Dirt", meta = (ClampMin = "0"))
	float RestLayerCm = 60.0f;

	// --- angle of repose --------------------------------------------------

	/** Steepest slope fully loose dirt can hold. Dry sand is about 32 degrees. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Repose", meta = (ClampMin = "1", ClampMax = "89"))
	float LooseReposeDeg = 32.0f;

	/**
	 * Friction angle of fully packed (dense) dirt. Dense sand is 38-42 deg: grains
	 * interlock. Hardpack standing at 70 deg is not friction, it is cohesion —
	 * see PackedCohesionKPa. (docs/SoilPhysics.md section 2.)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Repose", meta = (ClampMin = "1", ClampMax = "60"))
	float PackedReposeDeg = 42.0f;

	/**
	 * Peak apparent cohesion from moisture, kPa. Water menisci between grains pull
	 * them together (matric suction); it is zero when dry, peaks around half
	 * saturation and vanishes again when saturated. Damp sand: 2-4 kPa. This is
	 * what lets a sandcastle wall stand vertical up to ~1 m.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Repose", meta = (ClampMin = "0", ClampMax = "50"))
	float SuctionCohesionKPa = 3.0f;

	/**
	 * Cohesion of fully packed dirt, kPa: interlock and cementation of fines. Sand
	 * ~2, loam ~8, hardpack clay ~25. Washed out as the dirt saturates.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Repose", meta = (ClampMin = "0", ClampMax = "100"))
	float PackedCohesionKPa = 8.0f;

	/** Moist unit weight, kN/m3. Sets how tall a cohesive face can stand: H ~ 4c/gamma. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Repose", meta = (ClampMin = "10", ClampMax = "24"))
	float UnitWeightKNm3 = 17.0f;

	/**
	 * Fraction of friction lost when saturated: pore pressure carries the load
	 * instead of the grains (effective stress). 0.6 takes 32 deg dry sand to about
	 * 14 deg mud.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Repose", meta = (ClampMin = "0", ClampMax = "0.95"))
	float SaturationFrictionLoss = 0.6f;

	/** Cap on the cohesive standing height, cm, so near-repose faces do not read as infinitely strong. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Repose", meta = (ClampMin = "10"))
	float MaxCohesiveHeightCm = 400.0f;

	// --- slumping ---------------------------------------------------------

	/**
	 * Fraction of the excess height moved per slump iteration. Keep at or below
	 * 0.125 — four neighbours each moving more than that oscillates.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slump", meta = (ClampMin = "0.001", ClampMax = "0.125"))
	float SlumpRate = 0.1f;

	/** Slump iterations per fixed sim step. More = dirt settles faster. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slump", meta = (ClampMin = "0", ClampMax = "16"))
	int32 SlumpIterations = 3;

	/** How much compaction avalanching dirt loses. Tumbling dirt arrives loose. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slump", meta = (ClampMin = "0", ClampMax = "1"))
	float LooseningRate = 0.25f;

	/** Movement in cm that counts as "fully disturbed" for the loosening above. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slump", meta = (ClampMin = "0.01"))
	float LooseningScaleCm = 2.0f;

	// --- brushes ----------------------------------------------------------

	/** Compaction lost where a dig/raise/smooth stroke lands. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Brush", meta = (ClampMin = "0", ClampMax = "1"))
	float BrushDisturb = 0.3f;

	/** Rim radius as a multiple of core radius. The spoil heap around a hole. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Brush", meta = (ClampMin = "1.05", ClampMax = "4"))
	float BrushRimScale = 2.0f;

	// --- solid volume (docs/SoilPhysics.md sections 1 and 4) -------------------
	//
	// The layer channel stores SOLID centimetres: the grains alone, voids
	// squeezed out. Bulk (visible) thickness follows from porosity, so packing
	// shrinks the ground and loosening fluffs it up, and nothing is created or
	// destroyed either way. The audit sums solids.

	/** Void fraction of fully loose dirt. Loam ~0.48, sand ~0.46, clay ~0.50. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Solid", meta = (ClampMin = "0.2", ClampMax = "0.7"))
	float LoosePorosity = 0.48f;

	/** Void fraction of fully packed dirt. Loam ~0.34. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Solid", meta = (ClampMin = "0.1", ClampMax = "0.6"))
	float DensePorosity = 0.34f;

	/**
	 * How deep packing reaches, cm. A tyre or a tool packs a skin, not the whole
	 * column; below it the ground stays at DeepCompaction. This is what makes a
	 * rut from packing a few centimetres rather than a fifth of the layer.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Solid", meta = (ClampMin = "1", ClampMax = "100"))
	float CompactionDepthCm = 15.0f;

	/** Compaction of natural ground below the packed skin. Matches the builders' default. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Solid", meta = (ClampMin = "0", ClampMax = "1"))
	float DeepCompaction = 0.25f;

	/** Compaction landed parcels arrive with. Thrown dirt is the loosest dirt there is. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Solid", meta = (ClampMin = "0", ClampMax = "1"))
	float DepositCompaction = 0.05f;

	// --- water (docs/SoilPhysics.md section 5) ----------------------------------
	//
	// Real time scales are hours; these are game-paced, roughly 50x faster, and
	// every one of them is a knob.

	/** Run the water pass at all. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water")
	bool bWater = true;

	/** Fraction of a head difference that ponded water crosses per step. Keep under 1. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water", meta = (ClampMin = "0", ClampMax = "1"))
	float RunoffRate = 0.5f;

	/** Rain falling on the whole box, cm per second of game time. 0 = dry. DaDirt.Rain sets it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water", meta = (ClampMin = "0"))
	float RainCmPerSec = 0.0f;

	/** Seconds of rain left, or a negative number for rain that never stops. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water")
	float RainSecondsLeft = 0.0f;

	/** How fast ponded water soaks into LOOSE dirt, cm/s. Packing cuts it a hundredfold. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water", meta = (ClampMin = "0"))
	float InfiltrationCmPerSec = 0.5f;

	/** Per second, the share of water above field capacity that drains away downward through loose dirt. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water", meta = (ClampMin = "0"))
	float DrainPerSec = 0.025f;

	/** Surface drying, saturation lost per second. 0.0007 dries a soaked pad in ~20 min of play. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water", meta = (ClampMin = "0"))
	float EvapPerSec = 0.0007f;

	/** Saturation air-dry dirt keeps (hygroscopic water). Drying stops here, so the box never goes bone-dry on its own. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water", meta = (ClampMin = "0", ClampMax = "0.5"))
	float AmbientMoisture = 0.05f;

	/** Saturation the dirt holds against gravity by suction. ~0.3 for loam. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water", meta = (ClampMin = "0", ClampMax = "1"))
	float FieldCapacity = 0.3f;

	/** Depth of ground the moisture channel describes, cm. Sets how much water a cell can drink. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water", meta = (ClampMin = "1"))
	float WetDepthCm = 20.0f;

	// --- parcels (docs/SoilPhysics.md section 7) ---------------------------------
	//
	// Dirt in the air. A parcel carries a real volume, lands, and hands it back.

	/** Simulate and draw parcels at all. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parcels")
	bool bParcels = true;

	/** Parcels per side of the pool: 512 = 262,144 parcels. Set at startup only. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Parcels", meta = (ClampMin = "64", ClampMax = "1024"))
	int32 ParcelPoolSide = 512;

	/**
	 * Rendered diameter of a parcel, cm. THE knob for how fine the airborne dirt
	 * is: the volume a throw carries is split into parcels of this size, so
	 * halving it means eight times as many parcels for the same roost.
	 * 0 = choose from the soil: clods the size cohesion can hold together.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parcels", meta = (ClampMin = "0", ClampMax = "20"))
	float ParcelDiameterCm = 1.0f;

	/** Smallest parcel the soil rule may pick, cm. Below ~3 mm nothing survives the budget. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parcels", meta = (ClampMin = "0.05", ClampMax = "20"))
	float ParcelMinDiameterCm = 0.4f;

	/** Largest parcel the soil rule may pick, cm. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parcels", meta = (ClampMin = "0.05", ClampMax = "50"))
	float ParcelMaxDiameterCm = 5.0f;

	/** +/- fraction of size jitter within one throw. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parcels", meta = (ClampMin = "0", ClampMax = "0.9"))
	float ParcelDiameterJitter = 0.45f;

	/** Most parcels one throw may create; a bigger throw gets bigger parcels instead. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parcels", meta = (ClampMin = "1", ClampMax = "65536"))
	int32 ParcelMaxPerThrow = 4096;

	/** Air drag: deceleration = K v^2 / diameter (cm units). 2.6e-4 is a dirt clod. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parcels", meta = (ClampMin = "0"))
	float ParcelDragK = 0.00026f;

	/** Bounce kept by a dry clod hitting the ground. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parcels", meta = (ClampMin = "0", ClampMax = "1"))
	float ParcelRestitutionDry = 0.3f;

	/** Bounce kept by a lump of mud: it splats. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parcels", meta = (ClampMin = "0", ClampMax = "1"))
	float ParcelRestitutionWet = 0.02f;

	/** Below this speed on ground it can stand on, a parcel is settling, cm/s. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parcels", meta = (ClampMin = "0"))
	float ParcelRestSpeedCmS = 15.0f;

	/** Seconds of settling before a parcel deposits and disappears. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parcels", meta = (ClampMin = "0"))
	float ParcelRestSeconds = 0.12f;

	/** Smallest parcel on screen, as a fraction of its distance (0.003 = ~1.5 px at 1080p). Stops grains flickering. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parcels", meta = (ClampMin = "0", ClampMax = "0.05"))
	float ParcelMinScreenSize = 0.003f;

	// --- grains shedding down a face (spawned by the slump pass) ------------------

	/** Cells avalanching hard enough turn part of what they shed into parcels that roll off. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parcels")
	bool bShed = true;

	/** A cell must be shedding at least this many solid cm in one step to qualify. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parcels", meta = (ClampMin = "0"))
	float ShedMinOutCm = 0.05f;

	/** Chance per qualifying cell per step. Density of the trickle. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parcels", meta = (ClampMin = "0", ClampMax = "1"))
	float ShedChance = 0.15f;

	/** Share of the cell's outflow that leaves as a parcel instead of flowing to the neighbours. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parcels", meta = (ClampMin = "0", ClampMax = "1"))
	float ShedFraction = 0.5f;

	/** Rendered size of a shed grain, cm. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parcels", meta = (ClampMin = "0.05", ClampMax = "10"))
	float ShedDiameterCm = 0.6f;

	/** How fast a shed grain leaves its cell, cm/s, along the fall line. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parcels", meta = (ClampMin = "0"))
	float ShedSpeedCmS = 60.0f;

	// --- dust: the sub-millimetre tail, an effect on purpose ---------------------

	/** Puff dust with roost and throws. Dust carries no audited volume and fades out. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dust")
	bool bDust = true;

	/** Dust motes per side of the pool: 256 = 65,536. Set at startup only. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Dust", meta = (ClampMin = "32", ClampMax = "1024"))
	int32 DustPoolSide = 256;

	/** Motes puffed per litre of dirt thrown. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dust", meta = (ClampMin = "0"))
	float DustPerLitre = 400.0f;

	/** Seconds a mote lasts before it has faded. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dust", meta = (ClampMin = "0.1"))
	float DustLifetime = 2.5f;

	/** Air drag for dust, on a 1 mm speck: a = K v^2 / 0.1 cm. 0.0008 carries a plume a metre or two. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dust", meta = (ClampMin = "0"))
	float DustDragK = 0.0008f;

	/**
	 * Share of a mote's weight the air carries. 1 = it hangs where it stops; a
	 * little over 1 = it rises, the way a plume does on the warm turbulent air
	 * behind a tyre. Lifetime, not gravity, ends it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dust", meta = (ClampMin = "0", ClampMax = "1.5"))
	float DustBuoyancy = 1.03f;

	/** Rendered size of a mote, cm. A soft puff rather than a grain. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dust", meta = (ClampMin = "0.5"))
	float DustDiameterCm = 6.0f;

	// --- stepping ---------------------------------------------------------

	/**
	 * Fixed simulation rate. Stepping on a fixed clock instead of per-frame is
	 * what makes the test suite give the same answer at 30 fps and 120 fps.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sim", meta = (ClampMin = "10", ClampMax = "240"))
	float SimHz = 60.0f;

	/** Max sim steps per frame, so a hitch cannot spiral into a slideshow. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sim", meta = (ClampMin = "1", ClampMax = "8"))
	int32 MaxStepsPerFrame = 2;

	/** Side length of the region actually simulated, in cm. */
	float RegionSizeCm() const
	{
		return (SimRegionSizeCm > 0.0f) ? FMath::Min(SimRegionSizeCm, WorldSizeCm) : WorldSizeCm;
	}

	/** True when the simulation is pointed at part of the box rather than all of it. */
	bool IsFocused() const
	{
		return RegionSizeCm() < WorldSizeCm - 1.0f;
	}

	/** Centimetres per simulation cell. Derived from the REGION, not the box. */
	float TexelSizeCm() const
	{
		return RegionSizeCm() / FMath::Max(SimResolution, 1);
	}

	/** Ground area one cell covers, in square centimetres. */
	float TexelAreaCm2() const
	{
		const float T = TexelSizeCm();
		return T * T;
	}
};


// ---------------------------------------------------------------------------
// Brush kernel shapes.
//
// These MUST stay identical to DirtBrushCoreWeight / DirtBrushRimWeight in
// Shaders/Private/DirtCommon.ush. The CPU sums them over the brush footprint to
// produce CoreNorm and RimNorm; if the shapes drift apart, dig stops being
// zero-sum and the volume audit will start reporting drift.
// ---------------------------------------------------------------------------

inline float DirtBrushCoreWeight(float R, float CoreRadius)
{
	if (R >= CoreRadius || CoreRadius <= 0.0f)
	{
		return 0.0f;
	}
	const float T = R / CoreRadius;
	const float F = 1.0f - T * T;
	return F * F;
}

inline float DirtBrushRimWeight(float R, float CoreRadius, float RimRadius)
{
	if (R <= CoreRadius || R >= RimRadius || RimRadius <= CoreRadius)
	{
		return 0.0f;
	}
	const float T = (R - CoreRadius) / (RimRadius - CoreRadius);
	const float S = FMath::Sin(PI * T);
	return S * S;
}

// ---------------------------------------------------------------------------
// Soil strength (Mohr-Coulomb). Mirrors DirtSoilStrength / DirtAllowedDrop in
// Shaders/Private/DirtCommon.ush — keep them identical. docs/SoilPhysics.md.
// ---------------------------------------------------------------------------

/** Friction (as tan phi) and cohesion (kPa) of dirt in a given state. */
inline void DirtSoilStrength(float Compaction, float Moisture, const FDirtSimSettings& S,
							 float& OutTanPhi, float& OutCohesionKPa)
{
	const float C = FMath::Clamp(Compaction, 0.0f, 1.0f);
	const float M = FMath::Clamp(Moisture, 0.0f, 1.0f);
	const float Sat = FMath::SmoothStep(0.55f, 1.0f, M);                 // pore pressure takes over

	const float PhiDeg = FMath::Lerp(S.LooseReposeDeg, S.PackedReposeDeg, C);
	OutTanPhi = FMath::Tan(FMath::DegreesToRadians(PhiDeg)) * (1.0f - S.SaturationFrictionLoss * Sat);

	const float Suction = 4.0f * M * (1.0f - M);                         // menisci: none dry, none soaked
	OutCohesionKPa = S.SuctionCohesionKPa * Suction + S.PackedCohesionKPa * C * (1.0f - Sat);
}

/**
 * Largest drop (cm) a cell may stand above a neighbour RunCm away. Friction
 * allows Run * tan(phi); cohesion lets a face up to the Culmann height H_c(beta)
 * stand whole, and a taller face keep H_c of it.
 */
inline float DirtAllowedDropCm(float DropCm, float RunCm, float TanPhi, float CohesionKPa,
							   float UnitWeightKNm3, float MaxHcCm)
{
	const float FrictionDrop = RunCm * TanPhi;
	if (DropCm <= FrictionDrop || CohesionKPa <= 0.0f)
	{
		return FrictionDrop;
	}

	const float Beta = FMath::Atan2(DropCm, RunCm);
	const float Phi = FMath::Atan(TanPhi);
	const float Denom = FMath::Max(1.0f - FMath::Cos(Beta - Phi), 1e-4f);
	// 4c/gamma is metres; the grid is centimetres.
	const float Hc = FMath::Min(400.0f * CohesionKPa / FMath::Max(UnitWeightKNm3, 1.0f)
								* FMath::Sin(Beta) * FMath::Cos(Phi) / Denom, MaxHcCm);

	return FMath::Max(FrictionDrop, (DropCm <= Hc) ? DropCm : Hc);
}

// ---------------------------------------------------------------------------
// Solid volume <-> bulk thickness. Mirrors DirtSolidFraction / DirtBulkCm /
// DirtSolidCm in Shaders/Private/DirtCommon.ush — keep them identical.
// ---------------------------------------------------------------------------

/** Solid fraction (1 - porosity) of dirt at a given compaction. */
inline float DirtSolidFraction(float Compaction, const FDirtSimSettings& S)
{
	return 1.0f - FMath::Lerp(S.LoosePorosity, S.DensePorosity, FMath::Clamp(Compaction, 0.0f, 1.0f));
}

/** Bulk (visible) thickness of a column holding SolidCm of grains, whose top skin is at Compaction. */
inline float DirtBulkCm(float SolidCm, float Compaction, const FDirtSimSettings& S)
{
	const float SkinFrac = DirtSolidFraction(Compaction, S);
	const float DeepFrac = DirtSolidFraction(S.DeepCompaction, S);
	const float SkinSolid = SkinFrac * S.CompactionDepthCm;
	return (SolidCm <= SkinSolid)
		? SolidCm / SkinFrac
		: S.CompactionDepthCm + (SolidCm - SkinSolid) / DeepFrac;
}

/** Inverse: the solids in a column BulkCm tall whose top skin is at Compaction. */
inline float DirtSolidCm(float BulkCm, float Compaction, const FDirtSimSettings& S)
{
	const float SkinFrac = DirtSolidFraction(Compaction, S);
	const float DeepFrac = DirtSolidFraction(S.DeepCompaction, S);
	return (BulkCm <= S.CompactionDepthCm)
		? BulkCm * SkinFrac
		: S.CompactionDepthCm * SkinFrac + (BulkCm - S.CompactionDepthCm) * DeepFrac;
}

/** Proctor: packing efficiency as a function of moisture. Mirrors DirtPackingEfficiency in DirtCommon.ush. */
inline float DirtPackingEfficiency(float Moisture)
{
	const float M = FMath::Clamp(Moisture, 0.0f, 1.0f);
	const float Hump = FMath::SmoothStep(-0.2f, 0.7f, M);
	const float Mud = 1.0f - FMath::SmoothStep(0.85f, 1.0f, M);
	return (0.35f + 0.65f * Hump) * Mud;
}
