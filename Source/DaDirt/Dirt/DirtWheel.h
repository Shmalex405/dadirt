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
	// Bekker: p = (k_c / b + k_phi) z^n. Loose and dense values are interpolated
	// by compaction (k in log space), and saturation weakens both. Loose ~ dry
	// sand (sinks 3-4 cm under this wheel), dense ~ hardpack (under 1 mm).

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terramechanics")
	float BekkerNLoose = 0.9f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terramechanics")
	float BekkerNDense = 0.5f;

	/** kN / m^(n+1) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terramechanics")
	float BekkerKcLoose = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terramechanics")
	float BekkerKcDense = 15.0f;

	/** kN / m^(n+2) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terramechanics")
	float BekkerKphiLoose = 400.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terramechanics")
	float BekkerKphiDense = 5000.0f;

	/** Fraction of soil stiffness lost when saturated: mud takes a wheel. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terramechanics")
	float SaturationStiffnessLoss = 0.8f;

	/** Janosi-Hanamoto shear deformation modulus, m: sand 1-2.5 cm, loam 2-5 cm. Loose / dense. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terramechanics")
	float ShearModulusLooseM = 0.02f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terramechanics")
	float ShearModulusDenseM = 0.045f;

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

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Roost")
	bool bDeformsDirt = true;

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
	bool bOnGround = false;
	double OdometerM = 0.0;
	double RoostLitresTotal = 0.0;    // solid litres thrown

private:
	void Step(float Dt);
	void UpdateVisuals(float Dt);
	void UpdateFollowCamera();

	/** Bekker static sinkage (m) and the soil constants for this state and load. */
	void SoilResponse(float Compaction, float Moisture, float LoadN,
					  float& OutSinkageM, float& OutContactLengthM, float& OutResistanceN) const;

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

	static constexpr float SubstepSeconds = 1.0f / 240.0f;
	static constexpr float Gravity = 9.81f;
};
