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

/**
 * A soil: the measured properties one kind of dirt has. Every cell of the
 * box belongs to one (a static map built with the terrain), and the shaders
 * read the table through the cell's id. Wet and dry are not soils, they are
 * the moisture channel; a soil is what a moisture does to it.
 *
 * The five float4 rows the GPU sees are laid out by ToRows() exactly as
 * Shaders/Private/DirtCommon.ush documents. The Bekker and Janosi numbers are
 * for the wheel and stay on the CPU. Sources: docs/SoilPhysics.md section 8.
 */
USTRUCT(BlueprintType)
struct FDirtSoil
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soil")
	FString Name = TEXT("loam");

	// --- packing (section 1, 4) ------------------------------------------------
	/** Void fraction when loose. Sand ~0.44, loam ~0.48, silty PNW loam ~0.52. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soil", meta = (ClampMin = "0.2", ClampMax = "0.7"))
	float LoosePorosity = 0.48f;
	/** Void fraction when fully packed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soil", meta = (ClampMin = "0.15", ClampMax = "0.6"))
	float DensePorosity = 0.34f;

	// --- strength (section 2) ----------------------------------------------------
	/** Friction angle loose, deg. Dry sand ~30-32, angular granite ~36, clay ~24. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soil", meta = (ClampMin = "1", ClampMax = "89"))
	float LooseReposeDeg = 32.0f;
	/** Friction angle fully packed, deg. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soil", meta = (ClampMin = "1", ClampMax = "60"))
	float PackedReposeDeg = 42.0f;
	/** Peak apparent cohesion from moisture suction, kPa. Sand 2, loam 3, silty loam 6, clay 8. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soil", meta = (ClampMin = "0", ClampMax = "50"))
	float SuctionCohesionKPa = 3.0f;
	/** Cohesion of fully packed dirt, kPa: interlock and cementation. Sand ~0.5, loam ~8, caliche ~25. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soil", meta = (ClampMin = "0", ClampMax = "100"))
	float PackedCohesionKPa = 8.0f;
	/** Moist unit weight, kN/m3. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soil", meta = (ClampMin = "10", ClampMax = "24"))
	float UnitWeightKNm3 = 17.0f;
	/** Fraction of friction lost when saturated (pore pressure carries the load). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soil", meta = (ClampMin = "0", ClampMax = "0.95"))
	float SaturationFrictionLoss = 0.6f;

	// --- water (section 5) --------------------------------------------------------
	/** Saturated conductivity of the loose soil, cm/s, game-paced. Sand ~1, loam 0.5, silt loam 0.15, clay 0.02. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soil", meta = (ClampMin = "0"))
	float InfiltrationCmPerSec = 0.5f;
	/** Saturation the soil holds against gravity. Sand 0.1, loam 0.3, silt loam 0.45, clay 0.4. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soil", meta = (ClampMin = "0", ClampMax = "1"))
	float FieldCapacity = 0.3f;
	/** Moisture at which it packs best (Proctor optimum). Sand packs best wet, clay well before saturation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soil", meta = (ClampMin = "0.1", ClampMax = "0.9"))
	float ProctorOptimum = 0.55f;
	/** Multiplier on the surface drying rate. Sand and granite dry fast, silty loam slowly. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soil", meta = (ClampMin = "0", ClampMax = "5"))
	float DryingMultiplier = 1.0f;

	// --- looks and dust ------------------------------------------------------------
	/** Multiplier on DustPerLitre: decomposed granite is a dust storm, wet-country loam is not. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soil", meta = (ClampMin = "0", ClampMax = "5"))
	float Dustiness = 1.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soil")
	FLinearColor DryColour = FLinearColor(0.40f, 0.29f, 0.19f);
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soil")
	FLinearColor PackedColour = FLinearColor(0.24f, 0.16f, 0.10f);

	// --- the tyre (section 6): Bekker pressure-sinkage and Janosi shear -------------
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soil") float BekkerNLoose = 0.9f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soil") float BekkerNDense = 0.5f;
	/** kN / m^(n+1) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soil") float BekkerKcLoose = 1.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soil") float BekkerKcDense = 15.0f;
	/** kN / m^(n+2) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soil") float BekkerKphiLoose = 400.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soil") float BekkerKphiDense = 5000.0f;
	/** Janosi-Hanamoto shear modulus, m, loose / dense. Sand 1-2.5 cm, loam 2-5 cm. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soil") float ShearModulusLooseM = 0.02f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soil") float ShearModulusDenseM = 0.045f;
	/** Fraction of Bekker stiffness lost when saturated: mud takes a wheel. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soil") float SaturationStiffnessLoss = 0.8f;

	/** The five float4 rows the shaders read. Out must hold DirtSim::SoilRows entries. */
	void ToRows(FVector4f* Out) const
	{
		Out[0] = FVector4f(LoosePorosity, DensePorosity, LooseReposeDeg, PackedReposeDeg);
		Out[1] = FVector4f(SuctionCohesionKPa, PackedCohesionKPa, UnitWeightKNm3, SaturationFrictionLoss);
		Out[2] = FVector4f(InfiltrationCmPerSec, FieldCapacity, ProctorOptimum, DryingMultiplier);
		Out[3] = FVector4f(DryColour.R, DryColour.G, DryColour.B, Dustiness);
		Out[4] = FVector4f(PackedColour.R, PackedColour.G, PackedColour.B, 0.0f);
	}

	/** The reference soils, docs/SoilPhysics.md section 8. Index 1 (loam) is the default. */
	static void Presets(TArray<FDirtSoil>& Out);
};

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

	// --- soils -------------------------------------------------------------
	//
	// The strength, packing and water numbers all live in the soils now: each
	// cell of the box belongs to one. DaDirt.Soil lists and paints them.

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soil")
	TArray<FDirtSoil> Soils;

	/** The soil a tool, test or painted region uses when none is named. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soil")
	int32 DefaultSoil = 1;

	FDirtSimSettings()
	{
		FDirtSoil::Presets(Soils);
	}

	const FDirtSoil& Soil(int32 Id) const
	{
		static const FDirtSoil Fallback;
		return Soils.IsValidIndex(Id) ? Soils[Id] : (Soils.Num() > 0 ? Soils[0] : Fallback);
	}

	int32 FindSoil(const FString& Name) const
	{
		for (int32 i = 0; i < Soils.Num(); ++i)
		{
			if (Soils[i].Name.Equals(Name, ESearchCase::IgnoreCase))
			{
				return i;
			}
		}
		return -1;
	}

	// --- angle of repose --------------------------------------------------







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


	/** Per second, the share of water above field capacity that drains away downward through loose dirt. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water", meta = (ClampMin = "0"))
	float DrainPerSec = 0.025f;

	/** Surface drying, saturation lost per second. 0.0007 dries a soaked pad in ~20 min of play. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water", meta = (ClampMin = "0"))
	float EvapPerSec = 0.0007f;

	/** Saturation air-dry dirt keeps (hygroscopic water). Drying stops here, so the box never goes bone-dry on its own. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water", meta = (ClampMin = "0", ClampMax = "0.5"))
	float AmbientMoisture = 0.05f;


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
inline void DirtSoilStrength(float Compaction, float Moisture, const FDirtSoil& Soil,
							 float& OutTanPhi, float& OutCohesionKPa)
{
	const float C = FMath::Clamp(Compaction, 0.0f, 1.0f);
	const float M = FMath::Clamp(Moisture, 0.0f, 1.0f);
	const float Sat = FMath::SmoothStep(0.55f, 1.0f, M);                 // pore pressure takes over

	const float PhiDeg = FMath::Lerp(Soil.LooseReposeDeg, Soil.PackedReposeDeg, C);
	OutTanPhi = FMath::Tan(FMath::DegreesToRadians(PhiDeg)) * (1.0f - Soil.SaturationFrictionLoss * Sat);

	const float Suction = 4.0f * M * (1.0f - M);                         // menisci: none dry, none soaked
	OutCohesionKPa = Soil.SuctionCohesionKPa * Suction + Soil.PackedCohesionKPa * C * (1.0f - Sat);
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

/** Solid fraction (1 - porosity) of a soil at a given compaction. */
inline float DirtSolidFraction(float Compaction, const FDirtSoil& Soil)
{
	return 1.0f - FMath::Lerp(Soil.LoosePorosity, Soil.DensePorosity, FMath::Clamp(Compaction, 0.0f, 1.0f));
}

/** Bulk (visible) thickness of a column holding SolidCm of grains, whose top skin is at Compaction. */
inline float DirtBulkCm(float SolidCm, float Compaction, const FDirtSoil& Soil, const FDirtSimSettings& S)
{
	const float SkinFrac = DirtSolidFraction(Compaction, Soil);
	const float DeepFrac = DirtSolidFraction(S.DeepCompaction, Soil);
	const float SkinSolid = SkinFrac * S.CompactionDepthCm;
	return (SolidCm <= SkinSolid)
		? SolidCm / SkinFrac
		: S.CompactionDepthCm + (SolidCm - SkinSolid) / DeepFrac;
}

/** Inverse: the solids in a column BulkCm tall whose top skin is at Compaction. */
inline float DirtSolidCm(float BulkCm, float Compaction, const FDirtSoil& Soil, const FDirtSimSettings& S)
{
	const float SkinFrac = DirtSolidFraction(Compaction, Soil);
	const float DeepFrac = DirtSolidFraction(S.DeepCompaction, Soil);
	return (BulkCm <= S.CompactionDepthCm)
		? BulkCm * SkinFrac
		: S.CompactionDepthCm * SkinFrac + (BulkCm - S.CompactionDepthCm) * DeepFrac;
}

/** Proctor: packing efficiency as a function of moisture, peaking at the soil's optimum. Mirrors DirtPackingEfficiency in DirtCommon.ush. */
inline float DirtPackingEfficiency(float Moisture, const FDirtSoil& Soil)
{
	const float M = FMath::Clamp(Moisture, 0.0f, 1.0f);
	const float Opt = Soil.ProctorOptimum;
	const float Hump = FMath::SmoothStep(Opt - 0.75f, Opt + 0.15f, M);
	const float Mud = 1.0f - FMath::SmoothStep(Opt + 0.3f, 1.0f, M);
	return (0.35f + 0.65f * Hump) * Mud;
}
