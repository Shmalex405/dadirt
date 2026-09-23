// DaDirt — the powered test wheel. The ancestor of the bike.
//
// One driven tyre on the dirt, with throttle, brake and steer. Like the ball it
// does not use the physics engine: it integrates itself against the simulated
// surface through a height window. What matters is the two-way conversation
// with the dirt, and since Phase D that conversation is terramechanics
// (docs/SoilPhysics.md section 6):
//
//   dirt -> wheel   Bekker pressure-sinkage says how far the tyre sinks for its
//                   load on this soil; Janosi-Hanamoto says how traction builds
//                   with slip up to the Mohr-Coulomb limit c*A + N tan(phi);
//                   the work of pressing the rut is the motion resistance;
//                   ruts steer it, saturation makes it slither
//   wheel -> dirt   the sinkage is pressed as a rut and the line is packed at
//                   the Proctor rate; past the traction limit the lugs shear
//                   the soil and throw it as parcels (roost), which fly, land
//                   and rejoin the ground
//
// All of the dirt side goes through the same brushes and parcels the console
// uses, so every cubic centimetre is still accounted for by DaDirt.Audit.
//
// Units: SI internally (m, s, kg, N). Positions are converted to Unreal cm at
// the edges.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DirtWheel.generated.h"

class ADirtBox;
class UStaticMeshComponent;

UCLASS()
class DADIRT_API ADirtWheel : public AActor
{
	GENERATED_BODY()

public:
	ADirtWheel();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;

	/** Throttle -1..1, steer -1..1, brake 0..1. Held until changed. */
	void SetInputs(float InThrottle, float InSteer, float InBrake);

	// --- the tyre: a real one --------------------------------------------------------
	//
	// A 110/90-19 motocross rear, the size every 250 and 450 races on (Dunlop
	// Geomax MX33, Michelin Starcross 6, Bridgestone Battlecross X30 all come in
	// it): section 110 mm, 90 % aspect ratio on a 19 in rim, so the outside
	// diameter is 482.6 + 2 x 99 = 680 mm; 5.5 kg; run at 12 psi (83 kPa; the
	// tyre patents test at 80 kPa). Its tread is a ROUND crown: the off-road
	// tyre patents put the drop from the centre to the tread edge at 30-40 mm
	// over a half-width of about 52 mm, a circular arc of about 56 mm radius, so
	// the contact is a narrow strip on hard ground and widens as the tyre sinks
	// or deflects, and it takes lean to reach the shoulder blocks. Blocks 7-19 mm
	// tall covering 10-30 % of the tread (the land ratio), tread rubber 75-80
	// Shore A. A complete rear wheel is about 11.7 kg: tyre 5.5, tube 1.0, rim
	// 1.7, hub 1.06, spokes and nipples 0.9, sprocket 0.5, disc 0.4, axle and
	// spacers 0.6. docs/SoilPhysics.md 6b has the sources. DaDirt.Tyre swaps in
	// the front (80/100-21), a sand tyre or a hard-terrain tyre.

	/** Outside diameter, m. 110/90-19: 0.680. 80/100-21 front: 0.693. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tyre")
	float OuterDiameterM = 0.680f;

	/** Section width, m: the widest point of the inflated tyre. Pushes water and dirt ahead of it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tyre")
	float SectionWidthM = 0.110f;

	/** Half the width across the tread blocks, m. The contact can never be wider than twice this. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tyre")
	float TreadHalfWidthM = 0.0525f;

	/** How far the tread edge sits below the crown, m (the patents: 30-40 mm). Sets the crown radius. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tyre")
	float CrownDropM = 0.035f;

	/** Block (knob) height, cm. Soft-terrain rears 18-19 mm, hard-terrain and fronts 12-13 mm. The layer a sliding knob drags; water shallower than this drains between the knobs. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tyre")
	float KnobHeightCm = 1.9f;

	/** Share of the tread area that is block top (the patents: 10-30 %). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tyre")
	float LandRatio = 0.20f;

	/** Inflation pressure, kPa. 12 psi = 83. The carcass carries its load over a patch of about load / pressure. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tyre")
	float PressureKPa = 83.0f;

	/** Mass riding on this wheel. The rear share of a 450 and its rider: 104 kg dry + 5 of fuel + 80 of rider, 52 % on the back. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wheel")
	float MassKg = 100.0f;

	/** The whole rotating wheel, kg: tyre, tube, rim, hub, spokes, sprocket, disc, axle. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wheel")
	float WheelMassKg = 11.7f;

	/** Derived from the tyre above by ApplyTyre(): outside radius and the crown arc's radius, m. */
	float RadiusM = 0.34f;
	float CrownRadiusM = 0.056f;

	/** Recompute the derived sizes and rescale the mesh. Called at BeginPlay and by DaDirt.Tyre. */
	void ApplyTyre();

	/** Swap the whole tyre for a named one: rear (default), front, sand, hard. False if the name is unknown. */
	bool SetTyrePreset(const FString& Name);

	/** The tyre spring: at pressure p a round tyre of radius R and crown r_c carries N over an elliptical patch of area N / p, so its deflection is N / (2 pi p sqrt(R r_c)) and it is linear. About 72 kN/m at 12 psi, next to the 180 kN/m Cossalter quotes for a road tyre at 2.3 bar. */
	float TyreStiffnessNPerM() const;
	float TyreDeflectionM(float LoadN) const { return LoadN / FMath::Max(TyreStiffnessNPerM(), 1.0f); }

	/** Width of the ground contact, m, for a tyre sunk SinkM into the soil and flattened DeflectionM: the round crown's chord, never wider than the tread. */
	float ContactWidthM(float SinkM, float DeflectionM) const;

	/** How far the tyre can flatten before the rim is on the ground, m: most of the sidewall (19 in rim = 0.4826 m). Past it the spring is the rim. */
	float MaxTyreDeflectionM() const { return 0.7f * FMath::Max(0.5f * (OuterDiameterM - 0.4826f), 0.03f); }

	/** Peak drive torque at the axle, N m. A 450 in second gear is in this range. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wheel")
	float MaxDriveTorqueNm = 320.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wheel")
	float MaxBrakeTorqueNm = 500.0f;

	/** Drive torque tapers to nothing at this tyre speed: the gearing's top end. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wheel")
	float MaxWheelSpeedMps = 22.0f;

	/** The tyre counts as in contact this close above the surface, so a rut it has just pressed does not read as flight. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wheel")
	float ContactToleranceM = 0.03f;

	/** Yaw rate at full steer and speed, deg/s. Scales down toward standstill. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wheel")
	float MaxYawRateDeg = 70.0f;

	// --- terramechanics: the soil as the tyre feels it ---------------------------
	//
	// Bekker's (n, k_c, k_phi) and the Janosi shear modulus are properties of the
	// soil under the tyre and live in FDirtSoil (loose and dense values,
	// interpolated by compaction, k in log space, weakened by saturation).

	/** Share of the Bekker sinkage that stays as a rut once the tyre has passed (the rest springs back). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terramechanics")
	float PlasticSinkage = 0.5f;

	/** Slip-sinkage: extra sinkage per unit slip ratio, as a fraction of the static sinkage. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terramechanics")
	float SlipSinkage = 0.5f;

	/** Rolling loss on hardpack that Bekker does not see (bearings, tyre hysteresis). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terramechanics")
	float BaseRollingResistance = 0.02f;

	/** Compaction one pass adds to the line at full load, before the Proctor moisture curve. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terramechanics")
	float PackPerPass = 0.15f;

	// --- heaps: a pile is not a half-space ------------------------------------------
	//
	// Bekker's pressure-sinkage assumes soil confined on every side. Dirt that
	// stands above the ground around it at the scale of the wheel (a spoil pile,
	// a rut shoulder, the mound at the end of a rut) has nothing behind it: the
	// tyre's leading face can either climb it or shove it, and it does whichever
	// takes less. Climbing a heap of height h needs a push of N tan(theta), with
	// cos(theta) = 1 - h / r; shoving it needs the passive earth pressure of the
	// wedge, R_b = b (1/2 gamma h^2 K_p + 2 c h sqrt(K_p)), Rankine's K_p =
	// tan^2(45 + phi/2). The share of the heap that holds is R_b / (N tan theta):
	// a loose dry pile is a few percent (the tyre goes through it and pushes it
	// ahead, where it grows and pushes back with the square of its height); a
	// packed damp lip is most of the way to solid. A slope, whose ground ahead
	// is higher still, is not a heap at all and is climbed like the ground it is.

	/** Radius, as a fraction of the tyre radius, of the ring that defines "the ground around": prominence above its lower half is heap. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terramechanics")
	float HeapRingRadius = 1.0f;

	/** How far ahead of the axle the bulldozed wedge is measured, as a fraction of the tyre radius. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terramechanics")
	float PloughReach = 0.5f;

	// --- standing water ----------------------------------------------------------------
	//
	// A puddle does three things to a tyre. The mud under it is saturated
	// whatever the moisture channel has had time to say, so the soil strength
	// and stiffness the tyre feels are the saturated ones. The tyre has to push
	// the water out of its way: drag 1/2 rho C_d A v^2 on the submerged front,
	// which is what slows a bike through a puddle and throws the water. And once
	// the water is deeper than the knobs it cannot escape between them fast
	// enough: the wedge of water under the patch carries part of the load
	// (1/2 rho v^2 A C_L) and the knobs float off the soil. NASA's hydroplaning
	// speed v = 6.36 sqrt(p [psi]) mph puts a 12 psi tyre at 9.8 m/s, which is
	// C_L = 0.68 on the tyre's own patch.

	/** Drag coefficient of the submerged tyre front. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water")
	float WaterDragCoeff = 1.0f;

	/** Lift coefficient of the water wedge under the patch, full hydroplaning at 9.8 m/s for a 12 psi tyre. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water")
	float HydroLiftCoeff = 0.68f;

	// --- roost: excavation past the traction limit --------------------------------

	/** Depth of soil the lugs shear off per pass of tyre surface, in bulk cm, on loose dirt. Packed dirt gives less. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Roost")
	float LugFailureDepthCm = 0.4f;

	/** Slip speed below which the lugs are not failing the soil yet, m/s. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Roost")
	float RoostSlipThresholdMps = 0.5f;

	/** Slip beyond this throws no more dirt per metre: the tyre is just polishing the hole. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Roost")
	float RoostSlipCapMps = 14.0f;

	/** Ejection speed as a fraction of the slip speed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Roost")
	float RoostSpeedFraction = 0.85f;

	/** Launch angle above the ground, degrees, and the half-angle of the spray cone. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Roost")
	float RoostElevationDeg = 35.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Roost")
	float RoostSpreadDeg = 14.0f;

	// --- cornering: the sheared layer goes to the outside ----------------------------
	//
	// In a corner the tyre runs at a slip angle: it slides sideways at v tan(alpha)
	// while it rolls. Lateral grip builds with that shear displacement along the
	// patch exactly as drive traction does (Janosi-Hanamoto, j = x tan alpha), and
	// the two share one Mohr-Coulomb ceiling, so a spinning rear has little left
	// to hold the side. Where the patch is sliding, the knobs drag the layer they
	// are in (knob height, or the sinkage if shallower) sideways with them: that
	// layer leaves the line and is put down just outside the tyre's outer edge.
	// Lap after lap the line sinks and packs and the outer shoulder grows into a
	// berm; nothing places it. Sliding fast, the flank flings the same dirt as
	// spray instead of leaving it.

	/** Sideways slide above this speed starts flinging the sheared dirt off the flank rather than leaving it as a shoulder, m/s. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Roost")
	float SpraySlipThresholdMps = 0.4f;

	/** Sideways slide speed at which most of the sheared dirt flies, m/s. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Roost")
	float SprayFullSlideMps = 3.0f;

	// --- impact: a landing is Proctor's hammer -------------------------------------------
	//
	// A tyre touching down at v carries 1/2 m v^2 into the ground. The tyre's own
	// spring (its load over its deflection) and the soil (Bekker, the patch
	// growing as it sinks) take it between them at one force, and solving that
	// gives the peak load in g and how far the tyre punched in. The punch packs
	// the ground at that load (Proctor: energy per volume is what compacts), the
	// plastic share of it stays as a crater whose rim is the heaved dirt, and on
	// a hard landing part of the punched dirt squirts out from under the tyre as
	// splash instead of heaving. Landings go hard and hollow by themselves.

	/** Impact speed above which a landing splashes part of the punched dirt out from under the tyre, m/s. Below it the punch only heaves the rim. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Roost")
	float SplashImpactMps = 1.5f;

	/** The rolling dynamic load (g-out in a transition, the bottom of a bowl) is capped at this many g over the static load; a landing is the impact model's job. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terramechanics")
	float MaxDynamicLoadG = 4.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Roost")
	bool bDeformsDirt = true;

	/** Per-part switches for the leak hunt: DaDirt.WheelParts. */
	bool bPartRut = true;
	bool bPartPack = true;
	bool bPartRoost = true;
	bool bPartSpray = true;
	bool bPartSplash = true;
	bool bPartPlough = true;

	/** Test-rig mode: the wheel cannot translate, only spin. Burnouts on a stand. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Roost")
	bool bAnchored = false;

	/** The simplest rider: steer to follow a circle of the given radius about a centre, lap after lap. DaDirt.Orbit. Any DaDirt.Drive clears it. */
	void SetOrbit(FVector2D CentreM, float InRadiusM);
	void ClearOrbit() { bOrbit = false; }
	bool IsOrbiting() const { return bOrbit; }

	/** The same rider on the real track: follow the box's centre line. DaDirt.Lap. */
	void SetLap() { bLap = true; LapIndex = -1; OrbitBias = 0.0f; }

	/** Keep the player's camera behind the wheel. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	bool bFollowCamera = false;

	// --- live state, readable for HUDs and tests ------------------------------

	FVector VelocityMps = FVector::ZeroVector;
	float HeadingDeg = 0.0f;
	float WheelOmega = 0.0f;          // rad/s
	float LastSlipMps = 0.0f;
	float LastSlipRatio = 0.0f;
	float LastFriction = 0.0f;        // c A / N + tan phi: the traction ceiling as a coefficient
	float LastTractionN = 0.0f;
	float LastSinkageCm = 0.0f;
	float LastResistanceN = 0.0f;
	float LastCompaction = 0.0f;
	float LastMoisture = 0.0f;
	float LastHeapCm = 0.0f;          // how far the ground under the tyre stands above the ground around it
	float LastCarried = 1.0f;         // the share of the load that heap can carry (1 = rides over it)
	float LastPloughN = 0.0f;         // bulldozing resistance from the wedge ahead
	float LastPondCm = 0.0f;          // standing water under the tyre
	float LastWaterDragN = 0.0f;
	float LastHydroLiftN = 0.0f;      // load carried by water rather than soil
	float LastLateralN = 0.0f;        // sideways force the ground gave this substep
	float LastSlipAngleDeg = 0.0f;    // angle between heading and travel
	float LastDynamicG = 0.0f;        // rolling load beyond the static, in g
	float LastImpactG = 0.0f;         // peak load of the last landing, in g
	float LastImpactCm = 0.0f;        // how far the last landing punched in
	float LastContactWidthM = 0.08f;  // how wide the tyre touched the ground last substep
	float LastContactOffsetM = 0.0f;  // where along the tyre's circumference the ground held it: + ahead of the axle
	float LastSkinCm = 0.0f;          // the skin under the contact (loose top, or a crust), cm
	double PloughLitresTotal = 0.0;   // solid litres shoved ahead of the tyre
	double ShovedLitresTotal = 0.0;   // solid litres sheared sideways into the outer shoulder
	double AirTimeS = 0.0;            // seconds spent off the ground since placed
	float MaxAirCm = 0.0f;            // highest the tyre has been above the ground since placed
	bool bOnGround = false;
	double OdometerM = 0.0;
	double RoostLitresTotal = 0.0;    // solid litres thrown

private:
	void Step(float Dt);
	void UpdateVisuals(float Dt);
	void UpdateFollowCamera();

	/** Bekker's n, k_c (kN / m^(n+1)) and k_phi (kN / m^(n+2)) for this soil in this state. K = k_c / b + k_phi for a patch b wide. */
	void BekkerConstants(const struct FDirtSoil& Soil, float Compaction, float Moisture, float& OutN, float& OutKcKN, float& OutKphiKN) const;

	/** Bekker static sinkage (m), the patch it makes (length and width, m) and the motion resistance for this state and load. */
	void SoilResponse(const struct FDirtSoil& Soil, float Compaction, float Moisture, float LoadN,
					  float& OutSinkageM, float& OutContactLengthM, float& OutContactWidthM, float& OutResistanceN) const;

	/** A landing at FallSpeed: the peak force the tyre spring and the soil share, and how far the tyre punches in. */
	void ImpactResponse(const struct FDirtSoil& Soil, float Compaction, float Moisture, float InFallSpeedMps,
						float& OutPeakN, float& OutPunchM, float& OutTyreDeflectionM) const;
	float ContactPatchLength(const struct FDirtSoil& Soil, float Compaction, float Moisture, float LoadN) const;

	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> Root;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> Tyre;

	TWeakObjectPtr<ADirtBox> Box;
	int32 WindowId = -1;

	float Throttle = 0.0f;
	float Steer = 0.0f;
	float Brake = 0.0f;

	float Accumulator = 0.0f;
	float SpinAngle = 0.0f;
	float StatusTimer = 0.0f;

	// Deformation is emitted in strokes spaced along the ground rather than
	// every substep; these accumulate between strokes.
	float StrokeDistanceM = 0.0f;
	float StrokeSlipM = 0.0f;
	float StrokeSlipSign = 0.0f;
	float StrokeTimeS = 0.0f;
	float StrokeLoadNs = 0.0f;        // load x time over the stroke, airborne substeps counting nothing:
	float StrokeLoadTimeS = 0.0f;     //   the ground feels the mean pressure, not a hop's landing spike
	float StrokeSlipRatio = 0.0f;
	float StrokeSideSlipM = 0.0f;     // sideways slide, all of it: its mean speed decides shoulder or spray
	float StrokeShoveCm3 = 0.0f;      // bulk cm3 the sliding patch has sheared sideways this stroke, SIGNED by the slide direction: a jiggle cancels, a slide does not
	FVector2D LastPressCm = FVector2D::ZeroVector;   // where the last stroke's strip ended, so the next one continues it without a gap or a lump
	bool bHaveLastPress = false;
	float StrokePloughM2 = 0.0f;      // heap thickness sunk through x distance: the swept area, for the plough
	bool bWasOnGround = false;
	float FallSpeedMps = 0.0f;        // downward speed on the last airborne substep, for the impact
	float SinceImpactS = 100.0f;      // a tyre re-landing in its own fresh crater is not a second landing

	bool bOrbit = false;
	FVector2D OrbitCentreM = FVector2D::ZeroVector;
	float OrbitRadiusM = 8.0f;
	float OrbitSign = 1.0f;           // +1 anticlockwise (heading increasing), -1 clockwise
	float OrbitBias = 0.0f;           // slow correction for running wide at a slip angle
	bool bLap = false;
	int32 LapIndex = -1;              // nearest centre-line point last substep

	static constexpr float SubstepSeconds = 1.0f / 240.0f;
	static constexpr float Gravity = 9.81f;
};
