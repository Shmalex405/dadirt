// DaDirt — the powered test wheel. The ancestor of the bike.
//
// One driven tyre on the dirt, with throttle, brake and steer. Like the ball it
// does not use the physics engine: it integrates itself against the simulated
// surface through a height window. What matters is the two-way conversation
// with the dirt:
//
//   dirt -> wheel   height and slope under the contact patch (ruts steer it),
//                   compaction sets grip, saturation makes it slither,
//                   loose dirt drags harder
//   wheel -> dirt   wheelspin scoops dirt out from under the tyre and throws it
//                   backwards (roost), a locked brake shoves it forwards, rolling
//                   packs a line and presses a rut with shoulders on both sides
//
// All of the dirt side goes through the same brushes the console uses, so every
// cubic centimetre is still accounted for by DaDirt.Audit.
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

	// --- the tyre-dirt contact ---------------------------------------------

	/**
	 * Grip comes from the soil itself (docs/SoilPhysics.md section 6):
	 * grip = tan(phi_eff) + c_eff * A / N, with A the contact patch. Cohesion
	 * gives grip even under a light wheel; friction scales with load; saturation
	 * takes both away. Contact patch length along the tyre, m.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grip")
	float ContactPatchLengthM = 0.15f;

	/** Slip speed (m/s) at which traction is at ~76% of its peak. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grip")
	float SlipScaleMps = 1.2f;

	/** Rolling resistance coefficient on loose / packed dirt. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grip")
	float RollingResistanceLoose = 0.10f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grip")
	float RollingResistancePacked = 0.02f;

	// --- what the wheel does to the dirt --------------------------------------

	/** Dirt thrown per metre of slip at full load, in litres. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Deform")
	float RoostLitresPerSlipMetre = 0.12f;

	/** Slip beyond this throws no more dirt per metre: the tyre is just polishing the hole. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Deform")
	float RoostSlipCapMps = 12.0f;

	/** How far behind the tyre roost lands, in radii, at zero slip; more slip throws further. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Deform")
	float RoostDistanceRadii = 1.8f;

	/** Rut pressed by one pass at full load on fully loose dirt, cm. Packed dirt takes far less. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Deform")
	float RutCmPerPass = 1.5f;

	/** Compaction one pass adds to the line at full load. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Deform")
	float PackPerPass = 0.15f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Deform")
	bool bDeformsDirt = true;

	/** Test-rig mode: the wheel cannot translate, only spin. Burnouts on a stand. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Deform")
	bool bAnchored = false;

	/** Keep the player's camera behind the wheel. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	bool bFollowCamera = false;

	// --- live state, readable for HUDs and tests ------------------------------

	FVector VelocityMps = FVector::ZeroVector;
	float HeadingDeg = 0.0f;
	float WheelOmega = 0.0f;          // rad/s
	float LastSlipMps = 0.0f;
	float LastFriction = 0.0f;
	float LastCompaction = 0.0f;
	float LastMoisture = 0.0f;
	bool bOnGround = false;
	double OdometerM = 0.0;
	double RoostLitresTotal = 0.0;

private:
	void Step(float Dt);
	void UpdateVisuals(float Dt);
	void UpdateFollowCamera();

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

	static constexpr float SubstepSeconds = 1.0f / 240.0f;
	static constexpr float Gravity = 9.81f;
};
