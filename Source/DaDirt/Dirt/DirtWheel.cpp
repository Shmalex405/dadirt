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
		// Wide enough that a wheel at 12 m/s can outrun the readback's two-frame
		// lag, even through a hitch, without falling off its own window.
		const int32 Texels = FMath::Clamp(FMath::CeilToInt(RadiusM * CmPerM * 16.0f / B->GetTexelSizeCm()), 24, 160);
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
		ADirtBox* B = Box.Get();
		UE_LOG(LogDirtWheel, Log,
			TEXT("Wheel at (%.1f, %.1f) m hdg %.0f: %.1f m/s (%.0f km/h), wheel %.1f m/s, slip %+.1f m/s (i %+.2f), ")
			TEXT("grip %.2f, traction %.0f N, sink %.1f cm, resist %.0f N, compaction %.2f, moisture %.2f, %s, ")
			TEXT("odometer %.1f m, roost %.2f L solid, %d parcels live"),
			P.X / CmPerM, P.Y / CmPerM, HeadingDeg, VelocityMps.Size(), VelocityMps.Size() * 3.6f,
			WheelOmega * RadiusM, LastSlipMps, LastSlipRatio, LastFriction, LastTractionN, LastSinkageCm, LastResistanceN,
			LastCompaction, LastMoisture, bOnGround ? TEXT("on ground") : TEXT("airborne"), OdometerM, RoostLitresTotal,
			B ? B->GetLiveParcels() : 0);
	}
}

void ADirtWheel::SoilResponse(float Compaction, float Moisture, float LoadN,
							  float& OutSinkageM, float& OutContactLengthM, float& OutResistanceN) const
{
	// Bekker: p = (k_c / b + k_phi) z^n, with the rigid wheel's contact length
	// l = 2 sqrt(2 r z) and p = N / (b l). Both sides are power laws in z, so
	// the sinkage has a closed form:  z = (N / (K b 2 sqrt(2 r)))^(1 / (n + 1/2)).
	const float C = FMath::Clamp(Compaction, 0.0f, 1.0f);
	const float Sat = FMath::SmoothStep(0.55f, 1.0f, FMath::Clamp(Moisture, 0.0f, 1.0f));
	const float Weak = 1.0f - SaturationStiffnessLoss * Sat;

	const float N = FMath::Lerp(BekkerNLoose, BekkerNDense, C);
	const float Kc = FMath::Exp(FMath::Lerp(FMath::Loge(FMath::Max(BekkerKcLoose, 0.01f)), FMath::Loge(FMath::Max(BekkerKcDense, 0.01f)), C)) * Weak;
	const float Kphi = FMath::Exp(FMath::Lerp(FMath::Loge(FMath::Max(BekkerKphiLoose, 1.0f)), FMath::Loge(FMath::Max(BekkerKphiDense, 1.0f)), C)) * Weak;
	const float K = Kc / WidthM + Kphi;                                   // kN / m^(n+2)
	const float LoadKN = FMath::Max(LoadN, 1.0f) * 0.001f;

	const float Z = FMath::Pow(LoadKN / (K * WidthM * 2.0f * FMath::Sqrt(2.0f * RadiusM)), 1.0f / (N + 0.5f));
	OutSinkageM = FMath::Clamp(Z, 0.0f, RadiusM * 0.5f);
	OutContactLengthM = FMath::Max(2.0f * FMath::Sqrt(2.0f * RadiusM * OutSinkageM), 0.03f);

	// Motion resistance is the work of pressing the rut: R = b K z^(n+1) / (n+1).
	OutResistanceN = 1000.0f * WidthM * K * FMath::Pow(OutSinkageM, N + 1.0f) / (N + 1.0f);
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
		// No ground data here. Off the simulated ground (outside the box or a
		// focused region) the wheel parks rather than coasting off into nothing;
		// inside it the window is just lagging behind a fast wheel, so keep
		// rolling on for a substep and hold the current height.
		const bool bInside = B && B->IsInsideBox(FVector2D(PosM) * CmPerM);
		if (!bInside)
		{
			VelocityMps.X = VelocityMps.Y = 0.0f;
		}
		if (B && PosM.Z - RadiusM < B->GetActorLocation().Z / CmPerM)
		{
			PosM.Z = B->GetActorLocation().Z / CmPerM + RadiusM;
			VelocityMps.Z = 0.0f;
		}
		else if (bInside)
		{
			PosM.Z -= VelocityMps.Z * Dt;         // undo this substep's fall: no data, no verdict
			VelocityMps.Z = 0.0f;
		}
		SetActorLocation(PosM * CmPerM);
		bOnGround = bInside;
		return;
	}

	const float GroundM = GroundCm / CmPerM;
	const float Bottom = PosM.Z - RadiusM;
	bOnGround = Bottom <= GroundM + ContactToleranceM;

	// --- what the dirt under the tyre is like --------------------------------------
	const float Compaction = FMath::Clamp(State.G, 0.0f, 1.0f);
	const float Moisture = FMath::Clamp(State.B, 0.0f, 1.0f);
	LastCompaction = Compaction;
	LastMoisture = Moisture;

	// --- wheel spin from the engine and brake --------------------------------------
	const float Inertia = 0.5f * WheelMassKg * RadiusM * RadiusM;

	// Torque falls off toward the top of the gearing, so a free-spinning tyre
	// settles at MaxWheelSpeed instead of climbing forever.
	const float TyreSpeed = WheelOmega * RadiusM;
	const float TopEnd = (Throttle * TyreSpeed > 0.0f)
		? FMath::Clamp(1.0f - FMath::Abs(TyreSpeed) / FMath::Max(MaxWheelSpeedMps, 1.0f), 0.0f, 1.0f)
		: 1.0f;
	const float AxleTorque = Throttle * MaxDriveTorqueNm * TopEnd;
	const float BrakeTorque = Brake * MaxBrakeTorqueNm;

	// A brake is a friction clutch: it resists whatever net torque is trying to
	// turn the wheel, up to its limit, and holds the wheel dead still below it.
	// Applying it as a fixed torque let the ground's reaction spin the wheel a
	// few tenths of a metre per second each way every substep, which the roost
	// logic read as endless slip at a standstill.
	const auto ApplyBrake = [&](float NetTorque) -> float
	{
		if (BrakeTorque <= 0.0f)
		{
			return NetTorque;
		}
		const float Needed = -(WheelOmega * Inertia / Dt + NetTorque);     // torque that would stop and hold it
		if (FMath::Abs(Needed) <= BrakeTorque)
		{
			WheelOmega = 0.0f;
			return 0.0f;
		}
		return NetTorque + FMath::Sign(Needed) * BrakeTorque;
	};

	float SlipMps = 0.0f;
	float SlipRatio = 0.0f;

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

		// --- the soil's answer to this load ----------------------------------------------
		float SinkageM, ContactLengthM, ResistanceN;
		SoilResponse(Compaction, Moisture, Load, SinkageM, ContactLengthM, ResistanceN);

		// Mohr-Coulomb ceiling on traction: cohesion over the whole contact patch
		// plus friction under the load. Cohesion gives grip even under a light
		// wheel; saturation takes both away.
		float TanPhi = 0.6f, CohesionKPa = 0.0f;
		DirtSoilStrength(Compaction, Moisture, B->Settings, TanPhi, CohesionKPa);
		const float PatchAreaM2 = WidthM * ContactLengthM;
		const float MaxTractionN = CohesionKPa * 1000.0f * PatchAreaM2 + Load * TanPhi;
		LastFriction = MaxTractionN / FMath::Max(Load, 1.0f);

		// Janosi-Hanamoto: shear stress builds along the patch with the shear
		// displacement j = i x, so the mean over the patch of (1 - e^(-j/K)) is
		// the fraction of the ceiling the tyre actually gets at this slip.
		SlipMps = WheelOmega * RadiusM - VForward;
		SlipRatio = SlipMps / FMath::Max3(FMath::Abs(WheelOmega * RadiusM), FMath::Abs(VForward), 0.3f);
		const float Kj = FMath::Lerp(ShearModulusLooseM, ShearModulusDenseM, Compaction);
		const float A = FMath::Abs(SlipRatio) * ContactLengthM / Kj;
		const float Build = (A > 1e-4f) ? 1.0f - (1.0f - FMath::Exp(-A)) / A : 0.0f;
		const float TractionN = FMath::Sign(SlipRatio) * MaxTractionN * Build;

		// Motion resistance: the rut being pressed, plus a floor for hardpack.
		const float RollingN = -FMath::Sign(VForward) * (ResistanceN + BaseRollingResistance * Load)
			* FMath::Min(1.0f, FMath::Abs(VForward) / 0.2f);

		VelocityMps += FwdGround * ((TractionN + RollingN) / MassKg) * Dt;
		WheelOmega += (ApplyBrake(AxleTorque - TractionN * RadiusM) / Inertia) * Dt;

		LastTractionN = TractionN;
		LastResistanceN = ResistanceN;
		LastSinkageCm = SinkageM * CmPerM * (1.0f + SlipSinkage * FMath::Abs(SlipRatio));

		// Lateral: the tyre resists sliding sideways up to its grip; past that it
		// slithers, which is what happens in mud.
		const float LateralN = -FMath::Clamp(VLateral / 0.3f, -1.0f, 1.0f) * MaxTractionN;
		const float LateralImpulse = FMath::Clamp(LateralN * Dt, -FMath::Abs(VLateral) * MassKg, FMath::Abs(VLateral) * MassKg);
		VelocityMps += Lateral * (LateralImpulse / MassKg);

		// The ground under the tyre's width tilts it toward its lower edge. This
		// is the rut holding the wheel, and the berm wall pushing it back inside.
		VelocityMps += Lateral * (-LateralSlope * Gravity * Dt);

		// --- the wheel's mark on the dirt --------------------------------------------
		const float Moved = FMath::Abs(VForward) * Dt;
		OdometerM += Moved;
		StrokeDistanceM += Moved;
		StrokeSinkageM += SinkageM * (1.0f + SlipSinkage * FMath::Abs(SlipRatio)) * Moved;
		const float ShearSpeed = FMath::Max(FMath::Abs(SlipMps) - RoostSlipThresholdMps, 0.0f);
		StrokeSlipM += FMath::Min(ShearSpeed, RoostSlipCapMps) * Dt;
		StrokeSlipSign = SlipMps;
		StrokeSlipRatio = SlipRatio;
		StrokeTimeS += Dt;

		// One batch of strokes every sixth of a radius travelled, or every 1/20 s
		// when spinning on the spot.
		if (bDeformsDirt && (StrokeDistanceM >= RadiusM / 6.0f || (StrokeSlipM > 0.0f && StrokeTimeS >= 0.05f)))
		{
			const FVector2D ContactCm(PosM.X * CmPerM, PosM.Y * CmPerM);
			const float HalfWidthCm = WidthM * 0.5f * CmPerM;
			const float LoadFactor = Load / (MassKg * Gravity);

			// Rolling presses the Bekker sinkage as a rut and packs the line at
			// the Proctor rate. Strokes are spaced along the ground, so a cell
			// sees about (stroke spacing / cell size) of a pass per stroke;
			// scaling by that makes "per pass" mean per pass whatever the grid
			// resolution. The sinkage already knows the soil: loose sinks
			// centimetres, hardpack a millimetre, so a rut saturates as its floor
			// packs without any rule saying so.
			if (StrokeDistanceM > 0.0f)
			{
				const float PassFraction = StrokeDistanceM * CmPerM / B->GetTexelSizeCm();
				const float MeanSinkageCm = StrokeSinkageM / StrokeDistanceM * CmPerM;
				const float RutCm = PlasticSinkage * MeanSinkageCm * PassFraction * LoadFactor;
				if (RutCm > 0.005f)
				{
					// No disturb: a tyre pressing a rut is packing, not breaking up.
					B->ApplyBrush(ContactCm, HalfWidthCm * 1.2f, RutCm, EDirtBrushMode::Dig, 0.0f);
				}
				B->ApplyBrush(ContactCm, HalfWidthCm * 1.3f, PackPerPass * PassFraction * LoadFactor,
							  EDirtBrushMode::Pack, -1.0f, /*bProctor*/ true);
			}

			// Past the traction limit the lugs shear the soil off and fling it at
			// about the slip speed: roost. Volume rate = width x failure depth x
			// slip speed. It leaves the ground as parcels and comes back down as
			// parcels; nothing is dumped by fiat.
			if (StrokeSlipM > 0.001f)
			{
				// Never scoop more than the layer under the tyre can give: past
				// bedrock there is nothing to throw.
				const float ScoopRadiusCm = HalfWidthCm * 1.1f;
				const float AvailableCm3 = FMath::Max(State.R, 0.0f) * PI * ScoopRadiusCm * ScoopRadiusCm * 0.4f;
				const float FailureDepthCm = LugFailureDepthCm * (1.0f - 0.6f * Compaction);
				const float SolidFraction = DirtSolidFraction(Compaction, B->Settings);
				const float WantedCm3 = WidthM * CmPerM * FailureDepthCm * SolidFraction * StrokeSlipM * CmPerM * LoadFactor;
				const float VolumeCm3 = FMath::Min(WantedCm3, AvailableCm3);

				if (VolumeCm3 > 0.5f)
				{
					const float ThrowDir = (StrokeSlipSign >= 0.0f) ? -1.0f : 1.0f;     // wheelspin throws back, a locked brake forward
					const float EjectMps = FMath::Min(FMath::Abs(StrokeSlipSign), 25.0f) * RoostSpeedFraction;
					const float Elev = FMath::DegreesToRadians(RoostElevationDeg);
					const FVector Dir = (FwdGround * ThrowDir * FMath::Cos(Elev) + Normal * FMath::Sin(Elev)).GetSafeNormal();
					const FVector LaunchCm = FVector(ContactCm.X, ContactCm.Y, GroundCm) + FwdGround * ThrowDir * RadiusM * 0.4f * CmPerM
						+ FVector(0, 0, 4.0f);

					// A gentle disturb: the lugs shear the top off, the floor under
					// the patch is still being pressed. Full disturb wiped the line's
					// packing every stroke and the rut never firmed up.
					B->ScoopDirt(ContactCm, ScoopRadiusCm, VolumeCm3, 0.08f);
					B->SpawnParcels(LaunchCm, Dir * EjectMps * CmPerM + VelocityMps * CmPerM, RoostSpreadDeg, VolumeCm3, Moisture, Compaction);
					RoostLitresTotal += VolumeCm3 / 1000.0f;
				}
			}

			StrokeDistanceM = 0.0f;
			StrokeSinkageM = 0.0f;
			StrokeSlipM = 0.0f;
			StrokeTimeS = 0.0f;
		}
	}
	else
	{
		// In the air the wheel just spins up or down.
		WheelOmega += (ApplyBrake(AxleTorque) / Inertia) * Dt;
	}

	// Engine braking / bearing drag so a free wheel eventually stops.
	WheelOmega *= FMath::Max(0.0f, 1.0f - 0.15f * Dt);

	LastSlipMps = SlipMps;
	LastSlipRatio = SlipRatio;
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
