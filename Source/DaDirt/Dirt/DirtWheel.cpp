#include "DirtWheel.h"

#include "DirtBox.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "UObject/ConstructorHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogDirtWheel, Log, All);

namespace
{
	constexpr float CmPerM = 100.0f;

	/** Grip and drag are the dirt's answer to the tyre; this is that answer. */
	float SaturationOf(float Moisture)
	{
		return FMath::SmoothStep(0.55f, 1.0f, FMath::Clamp(Moisture, 0.0f, 1.0f));
	}
}

ADirtWheel::ADirtWheel()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostPhysics;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	Tyre = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Tyre"));
	Tyre->SetupAttachment(Root);
	Tyre->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Tyre->SetCastShadow(true);

	// The engine cylinder is 100 cm across and 100 cm tall along its local Z.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cylinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (Cylinder.Succeeded())
	{
		Tyre->SetStaticMesh(Cylinder.Object);
	}
}

void ADirtWheel::BeginPlay()
{
	Super::BeginPlay();

	HeadingDeg = GetActorRotation().Yaw;
	Tyre->SetRelativeScale3D(FVector(RadiusM * 2.0f, RadiusM * 2.0f, WidthM));

	Box = ADirtBox::GetActive();
	if (ADirtBox* B = Box.Get())
	{
		const int32 Texels = FMath::Clamp(FMath::CeilToInt(RadiusM * CmPerM * 8.0f / B->GetTexelSizeCm()), 16, 128);
		WindowId = B->CreateHeightWindow(Texels);
		B->SetHeightWindowCentre(WindowId, FVector2D(GetActorLocation()));
	}
	else
	{
		UE_LOG(LogDirtWheel, Warning, TEXT("No Dirtbox: the wheel has nothing to drive on."));
	}
}

void ADirtWheel::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (ADirtBox* B = Box.Get())
	{
		B->ReleaseHeightWindow(WindowId);
	}
	Super::EndPlay(EndPlayReason);
}

void ADirtWheel::SetInputs(float InThrottle, float InSteer, float InBrake)
{
	Throttle = InThrottle;
	Steer = InSteer;
	Brake = InBrake;
	UE_LOG(LogDirtWheel, Log, TEXT("Wheel inputs: throttle %.2f steer %.2f brake %.2f"), Throttle, Steer, Brake);
}

void ADirtWheel::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	Accumulator += FMath::Min(DeltaSeconds, 0.1f);
	while (Accumulator >= SubstepSeconds)
	{
		Accumulator -= SubstepSeconds;
		Step(SubstepSeconds);
	}

	UpdateVisuals(DeltaSeconds);

	if (ADirtBox* B = Box.Get())
	{
		B->SetHeightWindowCentre(WindowId, FVector2D(GetActorLocation()));
	}

	if (bFollowCamera)
	{
		UpdateFollowCamera();
	}

	// A status line twice a second while anything is happening.
	StatusTimer += DeltaSeconds;
	const bool bBusy = VelocityMps.SizeSquared() > 0.01f || FMath::Abs(WheelOmega) > 0.1f || Throttle != 0.0f;
	if (bBusy && StatusTimer >= 0.5f)
	{
		StatusTimer = 0.0f;
		const FVector P = GetActorLocation();
		UE_LOG(LogDirtWheel, Log,
			TEXT("Wheel at (%.1f, %.1f) m hdg %.0f: %.1f m/s (%.0f km/h), wheel %.1f m/s, slip %+.1f m/s, grip %.2f, ")
			TEXT("compaction %.2f, moisture %.2f, %s, odometer %.1f m, roost %.1f L"),
			P.X / CmPerM, P.Y / CmPerM, HeadingDeg, VelocityMps.Size(), VelocityMps.Size() * 3.6f,
			WheelOmega * RadiusM, LastSlipMps, LastFriction, LastCompaction, LastMoisture,
			bOnGround ? TEXT("on ground") : TEXT("airborne"), OdometerM, RoostLitresTotal);
	}
}

void ADirtWheel::Step(float Dt)
{
	ADirtBox* B = Box.Get();
	FVector PosM = GetActorLocation() / CmPerM;

	// --- steer: heading turns with speed, like a bike, not on the spot -------------
	const float Speed = static_cast<float>(FVector2D(VelocityMps).Size());
	const float SpeedFactor = FMath::Clamp(Speed / 3.0f, 0.0f, 1.0f);
	HeadingDeg += Steer * MaxYawRateDeg * SpeedFactor * Dt;

	const float HeadingRad = FMath::DegreesToRadians(HeadingDeg);
	const FVector Forward(FMath::Cos(HeadingRad), FMath::Sin(HeadingRad), 0.0f);

	// --- gravity, then find the ground -------------------------------------------
	VelocityMps.Z -= Gravity * Dt;
	if (bAnchored)
	{
		VelocityMps.X = VelocityMps.Y = 0.0f;
	}
	PosM += VelocityMps * Dt;

	// The tyre is 12 cm wide, not a point. It rests on the highest ground under
	// its width, and the difference between its two edges is what a rut uses to
	// steer it: the tyre is pushed toward the lower edge, back into the trough.
	float GroundCm = 0.0f;
	FVector Normal = FVector::UpVector;
	FLinearColor State(0, 0, 0, 0);
	float LateralSlope = 0.0f;
	bool bHaveGround = B && B->SampleHeightWindow(WindowId, FVector2D(PosM) * CmPerM, GroundCm, Normal, &State);
	if (bHaveGround)
	{
		const FVector Right(-Forward.Y, Forward.X, 0.0f);
		const FVector2D EdgeOffsetCm = FVector2D(Right.X, Right.Y) * (WidthM * 0.5f * CmPerM);
		float LeftCm = GroundCm, RightCm = GroundCm;
		FVector Unused;
		B->SampleHeightWindow(WindowId, FVector2D(PosM) * CmPerM - EdgeOffsetCm, LeftCm, Unused);
		B->SampleHeightWindow(WindowId, FVector2D(PosM) * CmPerM + EdgeOffsetCm, RightCm, Unused);
		LateralSlope = (RightCm - LeftCm) / (WidthM * CmPerM);
		GroundCm = FMath::Max3(GroundCm, LeftCm, RightCm);
	}
	if (!bHaveGround)
	{
		// Off the simulated ground (outside the box or a focused region): park
		// rather than coast off into nothing.
		VelocityMps.X = VelocityMps.Y = 0.0f;
		if (B && PosM.Z - RadiusM < B->GetActorLocation().Z / CmPerM)
		{
			PosM.Z = B->GetActorLocation().Z / CmPerM + RadiusM;
			VelocityMps.Z = 0.0f;
		}
		SetActorLocation(PosM * CmPerM);
		bOnGround = false;
		return;
	}

	const float GroundM = GroundCm / CmPerM;
	const float Bottom = PosM.Z - RadiusM;
	bOnGround = Bottom <= GroundM + ContactToleranceM;

	// --- what the dirt under the tyre is like --------------------------------------
	const float Compaction = FMath::Clamp(State.G, 0.0f, 1.0f);
	const float Moisture = FMath::Clamp(State.B, 0.0f, 1.0f);
	const float Friction = FMath::Lerp(FrictionLoose, FrictionPacked, Compaction)
						 * FMath::Lerp(1.0f, FrictionMud, SaturationOf(Moisture));
	const float RollingResistance = FMath::Lerp(RollingResistanceLoose, RollingResistancePacked, Compaction);
	LastCompaction = Compaction;
	LastMoisture = Moisture;
	LastFriction = Friction;

	// --- wheel spin from the engine and brake --------------------------------------
	const float Inertia = 0.5f * WheelMassKg * RadiusM * RadiusM;

	// Torque falls off toward the top of the gearing, so a free-spinning tyre
	// settles at MaxWheelSpeed instead of climbing forever.
	const float TyreSpeed = WheelOmega * RadiusM;
	const float TopEnd = (Throttle * TyreSpeed > 0.0f)
		? FMath::Clamp(1.0f - FMath::Abs(TyreSpeed) / FMath::Max(MaxWheelSpeedMps, 1.0f), 0.0f, 1.0f)
		: 1.0f;
	float AxleTorque = Throttle * MaxDriveTorqueNm * TopEnd;
	if (Brake > 0.0f)
	{
		// Brakes oppose rotation and can hold the wheel at zero.
		const float BrakeTorque = Brake * MaxBrakeTorqueNm;
		const float StopTorque = FMath::Abs(WheelOmega) * Inertia / Dt;
		AxleTorque -= FMath::Sign(WheelOmega) * FMath::Min(BrakeTorque, StopTorque);
	}

	float SlipMps = 0.0f;

	if (bOnGround)
	{
		// Out of the ground; kill velocity into it. The normal is what tilts the
		// push, so the wall of a rut shoves the tyre back toward the middle.
		// Within the contact tolerance the tyre is allowed to settle under
		// gravity rather than being snapped, so a fresh rut is not a jolt.
		if (Bottom < GroundM)
		{
			PosM.Z = GroundM + RadiusM;
		}
		const float Vn = FVector::DotProduct(VelocityMps, Normal);
		if (Vn < 0.0f && Bottom <= GroundM)
		{
			VelocityMps -= Vn * Normal;
		}

		// Directions in the ground plane.
		const FVector FwdGround = (Forward - FVector::DotProduct(Forward, Normal) * Normal).GetSafeNormal();
		const FVector Lateral = FVector::CrossProduct(Normal, FwdGround).GetSafeNormal();

		const float Load = MassKg * Gravity * FMath::Max(Normal.Z, 0.3f);
		const float VForward = FVector::DotProduct(VelocityMps, FwdGround);
		const float VLateral = FVector::DotProduct(VelocityMps, Lateral);

		// Longitudinal: traction from slip. Positive slip = wheelspin.
		SlipMps = WheelOmega * RadiusM - VForward;
		const float TractionN = Friction * Load * FMath::Tanh(SlipMps / FMath::Max(SlipScaleMps, 0.05f));
		const float RollingN = -FMath::Sign(VForward) * RollingResistance * Load * FMath::Min(1.0f, FMath::Abs(VForward) / 0.2f);

		VelocityMps += FwdGround * ((TractionN + RollingN) / MassKg) * Dt;
		WheelOmega += ((AxleTorque - TractionN * RadiusM) / Inertia) * Dt;

		// Lateral: the tyre resists sliding sideways up to its grip; past that it
		// slithers, which is what happens in mud.
		const float LateralN = -FMath::Clamp(VLateral / 0.3f, -1.0f, 1.0f) * Friction * Load;
		const float LateralImpulse = FMath::Clamp(LateralN * Dt, -FMath::Abs(VLateral) * MassKg, FMath::Abs(VLateral) * MassKg);
		VelocityMps += Lateral * (LateralImpulse / MassKg);

		// The ground under the tyre's width tilts it toward its lower edge. This
		// is the rut holding the wheel, and the berm wall pushing it back inside.
		VelocityMps += Lateral * (-LateralSlope * Gravity * Dt);

		// --- the wheel's mark on the dirt --------------------------------------------
		const float Moved = FMath::Abs(VForward) * Dt;
		OdometerM += Moved;
		StrokeDistanceM += Moved;
		StrokeSlipM += FMath::Min(FMath::Abs(SlipMps), RoostSlipCapMps) * Dt;
		StrokeSlipSign = SlipMps;
		StrokeTimeS += Dt;

		// One batch of strokes every sixth of a radius travelled, or every 1/20 s
		// when spinning on the spot.
		if (bDeformsDirt && (StrokeDistanceM >= RadiusM / 6.0f || (StrokeSlipM > 0.0f && StrokeTimeS >= 0.05f)))
		{
			const FVector2D ContactCm(PosM.X * CmPerM, PosM.Y * CmPerM);
			const float HalfWidthCm = WidthM * 0.5f * CmPerM;
			const float LoadFactor = Load / (MassKg * Gravity);

			// Rolling presses a rut and packs the line. Strokes are spaced along the
			// ground, so a cell sees about (stroke spacing / cell size) of a pass per
			// stroke; scaling by that makes "per pass" mean per pass whatever the
			// grid resolution. Loose dirt takes the rut, packed dirt takes little more.
			if (StrokeDistanceM > 0.0f)
			{
				const float PassFraction = StrokeDistanceM * CmPerM / B->GetTexelSizeCm();
				const float RutCm = RutCmPerPass * PassFraction * LoadFactor * (1.0f - 0.85f * Compaction);
				if (RutCm > 0.005f)
				{
					// No disturb: a tyre pressing a rut is packing, not breaking up.
					B->ApplyBrush(ContactCm, HalfWidthCm * 1.2f, RutCm, EDirtBrushMode::Dig, 0.0f);
				}
				B->ApplyBrush(ContactCm, HalfWidthCm * 1.3f, PackPerPass * PassFraction * LoadFactor, EDirtBrushMode::Pack);
			}

			// Slip scoops dirt out from under the tyre and throws it. Wheelspin
			// throws it backwards (roost); a locked brake shoves it forwards.
			if (StrokeSlipM > 0.001f)
			{
				// Never scoop more than the layer under the tyre can give: past
				// bedrock there is nothing to throw, and the shader's clamp would
				// otherwise turn the paired Dump into dirt from nowhere.
				const float ScoopRadiusCm = HalfWidthCm * 1.1f;
				const float AvailableCm3 = FMath::Max(State.R, 0.0f) * PI * ScoopRadiusCm * ScoopRadiusCm * 0.4f;
				const float WantedCm3 = RoostLitresPerSlipMetre * StrokeSlipM * LoadFactor * (1.0f - 0.6f * Compaction) * 1000.0f;
				const float VolumeCm3 = FMath::Min(WantedCm3, AvailableCm3);
				const float Litres = VolumeCm3 / 1000.0f;
				const float ThrowDir = (StrokeSlipSign >= 0.0f) ? -1.0f : 1.0f;
				const float ThrowM = RadiusM * RoostDistanceRadii + FMath::Min(FMath::Abs(StrokeSlipSign), 15.0f) * 0.12f;
				const FVector2D LandCm = ContactCm + FVector2D(FwdGround.X, FwdGround.Y) * ThrowDir * ThrowM * CmPerM;

				if (VolumeCm3 > 1.0f)
				{
					B->TransferDirt(ContactCm, ScoopRadiusCm, LandCm, HalfWidthCm * 2.2f, VolumeCm3);
					RoostLitresTotal += Litres;
				}
			}

			StrokeDistanceM = 0.0f;
			StrokeSlipM = 0.0f;
			StrokeTimeS = 0.0f;
		}
	}
	else
	{
		// In the air the wheel just spins up or down.
		WheelOmega += (AxleTorque / Inertia) * Dt;
	}

	// Engine braking / bearing drag so a free wheel eventually stops.
	WheelOmega *= FMath::Max(0.0f, 1.0f - 0.15f * Dt);

	LastSlipMps = SlipMps;
	SpinAngle += WheelOmega * Dt;

	SetActorLocation(PosM * CmPerM);
}

void ADirtWheel::UpdateVisuals(float)
{
	// Root carries the heading; the tyre is a cylinder laid on its side (axle
	// along local Y) and spun about that axle.
	Root->SetWorldRotation(FRotator(0.0f, HeadingDeg, 0.0f));

	const FQuat Align(FVector::ForwardVector, FMath::DegreesToRadians(90.0f));
	const FQuat Spin(FVector::RightVector, -SpinAngle);
	Tyre->SetRelativeRotation(Spin * Align);
}

void ADirtWheel::UpdateFollowCamera()
{
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!Pawn)
	{
		return;
	}

	const float HeadingRad = FMath::DegreesToRadians(HeadingDeg);
	const FVector Back(-FMath::Cos(HeadingRad), -FMath::Sin(HeadingRad), 0.0f);
	const FVector Target = GetActorLocation() + Back * 450.0f + FVector(0, 0, 220.0f);

	// Ease toward the target so the camera does not jitter with the substeps.
	const FVector NewLoc = FMath::Lerp(Pawn->GetActorLocation(), Target, 0.15);
	const FRotator LookAt = (GetActorLocation() + FVector(0, 0, 20.0f) - NewLoc).Rotation();
	Pawn->SetActorLocation(NewLoc);
	Pawn->SetActorRotation(LookAt);
	PC->SetControlRotation(LookAt);
}
