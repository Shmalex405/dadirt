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

	// --- the machine ---------------------------------------------------------

	/** MX rear tyre: ~0.35 m radius, 0.12 m wide. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wheel")
	float RadiusM = 0.35f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wheel")
	float WidthM = 0.12f;

	/** Mass riding on this wheel. Half a bike and rider. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wheel")
	float MassKg = 100.0f;

	/** Wheel + tyre rotating mass, for spin-up. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wheel")
	float WheelMassKg = 9.0f;

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

	/**
	 * A pneumatic tyre flattens under its load whatever the ground does: an MX
	 * tyre at 12 psi deflects 2-3 cm, which is a contact patch 2 sqrt(2 r d) ~
	 * 25 cm long even on concrete. Bekker's rigid wheel alone gave a 7 cm patch
	 * on hardpack, the shear could not build over it, and the tyre spun on every
	 * packed face it met.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terramechanics")
	float TyreDeflectionM = 0.022f;

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

	/** Knob height, cm. Water shallower than this drains between the knobs and only wets the soil. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water")
	float KnobHeightCm = 1.8f;

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

	/** Sideways slither above this speed shears dirt off the tyre's flank: spray off a berm, m/s. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Roost")
	float SpraySlipThresholdMps = 0.4f;

	/** Bulk cm of soil the flank shears per metre of sideways slide, on loose dirt. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Roost")
	float SprayFailureDepthCm = 0.25f;

	/** Impact speed above which a landing splashes dirt out from under the tyre, m/s. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Roost")
	float SplashImpactMps = 1.5f;

	/** Litres of solid dirt splashed per m/s of impact above the threshold, on loose dirt. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Roost")
	float SplashLitresPerMps = 0.25f;

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
	double PloughLitresTotal = 0.0;   // solid litres shoved ahead of the tyre
	double AirTimeS = 0.0;            // seconds spent off the ground since placed
	float MaxAirCm = 0.0f;            // highest the tyre has been above the ground since placed
	bool bOnGround = false;
	double OdometerM = 0.0;
	double RoostLitresTotal = 0.0;    // solid litres thrown

private:
	void Step(float Dt);
	void UpdateVisuals(float Dt);
	void UpdateFollowCamera();

	/** Bekker static sinkage (m) and the soil constants for this state and load. */
	void SoilResponse(const struct FDirtSoil& Soil, float Compaction, float Moisture, float LoadN,
					  float& OutSinkageM, float& OutContactLengthM, float& OutResistanceN) const;
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
	float StrokeSinkageM = 0.0f;      // sinkage-weighted by distance, for the rut
	float StrokeSlipRatio = 0.0f;
	float StrokeSideSlipM = 0.0f;     // sideways slide, for spray
	float StrokeSideSign = 0.0f;
	float StrokePloughM2 = 0.0f;      // heap thickness sunk through x distance: the swept area, for the plough
	bool bWasOnGround = false;
	float FallSpeedMps = 0.0f;        // downward speed on the last airborne substep, for the splash

	static constexpr float SubstepSeconds = 1.0f / 240.0f;
	static constexpr float Gravity = 9.81f;
};
