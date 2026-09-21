#include "DirtWheel.h"
#include "Algo/Sort.h"

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
			TEXT("grip %.2f, traction %.0f N, sink %.1f cm, resist %.0f N, heap %.1f cm carried %.2f plough %.0f N, ")
			TEXT("pond %.1f cm drag %.0f N lift %.0f N, ")
			TEXT("compaction %.2f, moisture %.2f, %s (air %.2f s, max %.1f cm), odometer %.1f m, roost %.2f L, ploughed %.2f L, %d parcels live"),
			P.X / CmPerM, P.Y / CmPerM, HeadingDeg, VelocityMps.Size(), VelocityMps.Size() * 3.6f,
			WheelOmega * RadiusM, LastSlipMps, LastSlipRatio, LastFriction, LastTractionN, LastSinkageCm, LastResistanceN,
			LastHeapCm, LastCarried, LastPloughN,
			LastPondCm, LastWaterDragN, LastHydroLiftN,
			LastCompaction, LastMoisture, bOnGround ? TEXT("on ground") : TEXT("airborne"), AirTimeS, MaxAirCm, OdometerM, RoostLitresTotal,
			PloughLitresTotal, B ? B->GetLiveParcels() : 0);
	}
}

float ADirtWheel::ContactPatchLength(float Compaction, float Moisture, float LoadN) const
{
	float SinkageM, ContactLengthM, ResistanceN;
	SoilResponse(Compaction, Moisture, LoadN, SinkageM, ContactLengthM, ResistanceN);
	return ContactLengthM;
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
	// The patch is the longer of what the ground gives (sinkage) and what the
	// tyre gives (its own deflection).
	OutContactLengthM = FMath::Max(2.0f * FMath::Sqrt(2.0f * RadiusM * FMath::Max(OutSinkageM, TyreDeflectionM)), 0.03f);

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
	float PondCm = 0.0f;
	float LateralSlope = 0.0f;
	bool bHaveGround = B && B->SampleHeightWindow(WindowId, FVector2D(PosM) * CmPerM, GroundCm, Normal, &State, &PondCm);
	LastPondCm = PondCm;
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

	// --- a heap is not a half-space ------------------------------------------------
	// How far the ground under the tyre stands above the ground around it is
	// dirt with nothing confining it: a spoil pile, a shoulder, the mound at the
	// end of a rut. It carries only its own bearing capacity; the rest of the
	// wheel sinks through it and shoves it ahead. Everything below that is the
	// confined soil Bekker describes, and the rut is pressed into that.
	float HeapM = 0.0f;
	float Carried = 1.0f;
	float WedgeAheadM = 0.0f;
	float BladeM = 0.0f;
	float TanPhiHeap = 0.6f, CohesionHeapKPa = 0.0f;
	if (bHaveGround)
	{
		// A heap stands above everything around it, and it stops growing once you
		// step back: a spoil pile is as tall seen from one tyre radius as from
		// two. The reference is the HIGHEST point of a ring at each distance. A
		// hillside fails at once (the uphill side is higher than the tyre); a hill
		// crest clears the near ring but stands far taller over the far one, so it
		// is not a heap either and is ridden as ground. A rut shoulder clears
		// both by the same amount (the pad is the highest thing round it).
		// Judged against one ring's mean, the dome at (22, 30) read as a 26 cm
		// heap all the way up and the wheel tried to bulldoze the hill.
		// Each ring is read twice: everything, and "aside" (90 degrees and more
		// off the heading). The aside references follow the contour, so a
		// hillside reads as level ground; the forward samples of the near ring
		// may sit on a pile just ahead, so they never serve as a reference; the
		// forward sample of the far ring tells a pile (beyond it: back at the
		// base) from a slope (beyond it: higher still).
		float RingMax1 = -1e9f, RingMax2 = -1e9f;
		float RingMax1Aside = -1e9f, RingMax2Aside = -1e9f;
		int32 RingN = 0;
		const float RingHeadingRad = FMath::Atan2(Forward.Y, Forward.X);
		for (int32 Ring = 1; Ring <= 2; ++Ring)
		{
			const float RingCm = RadiusM * HeapRingRadius * Ring * CmPerM;
			for (int32 K = 0; K < 8; ++K)
			{
				const float A = RingHeadingRad + K * PI / 4.0f;
				float H = 0.0f;
				FVector Unused;
				if (B->SampleHeightWindow(WindowId, FVector2D(PosM) * CmPerM + FVector2D(FMath::Cos(A), FMath::Sin(A)) * RingCm, H, Unused))
				{
					float& All = (Ring == 1) ? RingMax1 : RingMax2;
					float& Aside = (Ring == 1) ? RingMax1Aside : RingMax2Aside;
					All = FMath::Max(All, H);
					if (K >= 2 && K <= 6)          // 90 degrees and more off the heading
					{
						Aside = FMath::Max(Aside, H);
					}
					++RingN;
				}
			}
		}
		if (RingN >= 12)
		{
			const float P1 = (GroundCm - RingMax1Aside) / CmPerM;
			const float P2 = (GroundCm - RingMax2Aside) / CmPerM;
			if (P1 > 0.0f && P2 <= P1 * 1.3f + 0.02f)
			{
				HeapM = FMath::Min(P1, RadiusM * 0.9f);
			}
		}

		// The obstacle is the whole thing in front of the tyre, not just the part
		// under it now: the toe of a packed lip is held by the lip behind it.
		float AheadMaxCm = GroundCm;
		float BladeCm = GroundCm;
		{
			const FVector2D Fwd2(Forward.X, Forward.Y);
			for (float Reach : { PloughReach * 0.7f, PloughReach, 1.0f })
			{
				float H = 0.0f;
				FVector Unused;
				if (B->SampleHeightWindow(WindowId, FVector2D(PosM) * CmPerM + Fwd2 * (RadiusM * Reach * CmPerM), H, Unused))
				{
					AheadMaxCm = FMath::Max(AheadMaxCm, H);
					if (Reach < PloughReach)
					{
						BladeCm = H;
					}
				}
			}
		}
		// A pile just ahead of the tyre, not yet under it, is a heap by the same
		// rule: it must stand above the tyre's ground AND above both rings. Rising
		// ground on a slope fails that (the rings uphill are higher still).
		// (The near ring's forward samples are left out of the reference: on a
		// pile the size of the tyre they sit on the pile itself.)
		const float AheadHeapM = (RingN >= 12)
			? FMath::Clamp((AheadMaxCm - FMath::Max3(GroundCm, RingMax2, RingMax1Aside)) / CmPerM, 0.0f, RadiusM * 0.9f)
			: 0.0f;
		const float ObstacleM = FMath::Max(HeapM, AheadHeapM);

		// Anything lower than the tyre's own sinkage is inside its contact patch:
		// it is pressed as part of the rut, not shoved. Only what stands above
		// that is an obstacle.
		float StaticSinkM = 0.0f, UnusedL = 0.0f, UnusedR = 0.0f;
		SoilResponse(FMath::Clamp(State.G, 0.0f, 1.0f), FMath::Clamp(State.B, 0.0f, 1.0f), MassKg * Gravity, StaticSinkM, UnusedL, UnusedR);
		if (bPartPlough && ObstacleM > StaticSinkM + 0.005f)
		{
			// Climb it or shove it, whichever is cheaper.
			DirtSoilStrength(FMath::Clamp(State.G, 0.0f, 1.0f), FMath::Clamp(State.B, 0.0f, 1.0f), B->Settings, TanPhiHeap, CohesionHeapKPa);
			const float Phi = FMath::Atan(FMath::Max(TanPhiHeap, 0.05f));
			const float Kp = FMath::Square(FMath::Tan(PI / 4.0f + Phi / 2.0f));
			const float ShoveN = WidthM * (0.5f * B->Settings.UnitWeightKNm3 * 1000.0f * ObstacleM * ObstacleM * Kp
										   + 2.0f * CohesionHeapKPa * 1000.0f * ObstacleM * FMath::Sqrt(Kp));
			const float Theta = FMath::Acos(FMath::Clamp(1.0f - ObstacleM / RadiusM, 0.0f, 1.0f));
			const float ClimbN = MassKg * Gravity * FMath::Tan(FMath::Min(Theta, 1.4f));
			Carried = FMath::Clamp(ShoveN / FMath::Max(ClimbN, 1.0f), 0.0f, 1.0f);
		}

		// The wedge in front of the tyre, above where the tyre will sit: the
		// tallest thing ahead decides the push back, what stands at the blade
		// decides how much dirt moves.
		const float FloorCm = GroundCm - HeapM * (1.0f - Carried) * CmPerM;
		WedgeAheadM = ObstacleM;
		BladeM = (RingN >= 12) ? FMath::Max(BladeCm - FMath::Max3(FloorCm, RingMax2, RingMax1Aside), 0.0f) / CmPerM : 0.0f;
	}
	const float HeapSinkM = HeapM * (1.0f - Carried);
	GroundCm -= HeapSinkM * CmPerM;
	// A heap that yields has no face to launch off: the tyre feels the ground
	// around it, not the pile it is going through.
	if (HeapM > 0.002f)
	{
		Normal = FMath::Lerp(FVector::UpVector, Normal, Carried).GetSafeNormal();
	}
	LastHeapCm = HeapM * CmPerM;
	LastCarried = Carried;

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
	if (!bOnGround)
	{
		FallSpeedMps = FMath::Max(-VelocityMps.Z, 0.0f);
		AirTimeS += Dt;
		MaxAirCm = FMath::Max(MaxAirCm, (Bottom - GroundM) * CmPerM);
	}
	const bool bTouchdown = bOnGround && !bWasOnGround;
	bWasOnGround = bOnGround;

	// --- what the dirt under the tyre is like --------------------------------------
	// Under standing water the surface is saturated whatever the moisture
	// channel has had time to say: the knobs are in soup.
	const float Compaction = FMath::Clamp(State.G, 0.0f, 1.0f);
	const float Moisture = FMath::Max(FMath::Clamp(State.B, 0.0f, 1.0f), FMath::Clamp(PondCm / 1.0f, 0.0f, 1.0f));
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

		const float VForward = FVector::DotProduct(VelocityMps, FwdGround);
		const float VLateral = FVector::DotProduct(VelocityMps, Lateral);
		const float GroundSpeed = FMath::Sqrt(VForward * VForward + VLateral * VLateral);

		// --- standing water --------------------------------------------------------------
		// Deeper than the knobs, the water under the patch carries part of the
		// load and the knobs float off the soil; and the submerged front of the
		// tyre has to push the water aside.
		const float PondM = PondCm / CmPerM;
		const float RhoWater = 1000.0f;
		float HydroLiftN = 0.0f;
		float WaterDragN = 0.0f;
		if (PondM > 0.0f)
		{
			const float Flooded = FMath::Clamp((PondCm - KnobHeightCm) / FMath::Max(KnobHeightCm, 0.1f), 0.0f, 1.0f);
			const float PatchM2 = WidthM * FMath::Max(2.0f * FMath::Sqrt(2.0f * RadiusM * TyreDeflectionM), 0.03f);
			HydroLiftN = 0.5f * RhoWater * GroundSpeed * GroundSpeed * PatchM2 * HydroLiftCoeff * Flooded;
			const float FrontalM2 = WidthM * FMath::Min(PondM, 2.0f * RadiusM);
			WaterDragN = 0.5f * RhoWater * WaterDragCoeff * FrontalM2 * GroundSpeed * GroundSpeed;
		}
		const float FullLoad = MassKg * Gravity * FMath::Max(Normal.Z, 0.3f);
		HydroLiftN = FMath::Min(HydroLiftN, FullLoad);
		const float Load = FullLoad - HydroLiftN;
		const float Floating = HydroLiftN / FullLoad;      // share of the tyre that is on water, not soil
		LastHydroLiftN = HydroLiftN;
		LastWaterDragN = WaterDragN;
		if (WaterDragN > 0.0f && GroundSpeed > 1e-3f)
		{
			// Opposes the motion through the water; can stop the tyre, never reverse it.
			const FVector DirGround = (FwdGround * VForward + Lateral * VLateral) / GroundSpeed;
			const float DragImpulse = FMath::Min(WaterDragN * Dt, GroundSpeed * MassKg);
			VelocityMps -= DirGround * (DragImpulse / MassKg);
		}

		// --- a landing: the tyre punches into the dirt and splashes it out sideways ---
		if (bTouchdown && bDeformsDirt && bPartSplash && FallSpeedMps > SplashImpactMps)
		{
			const FVector2D ContactCm(PosM.X * CmPerM, PosM.Y * CmPerM);
			const float HalfWidthCm = WidthM * 0.5f * CmPerM;
			const float ScoopRadiusCm = HalfWidthCm * 1.6f;
			const float AvailableCm3 = B->MaxScoopCm3(State.R, ScoopRadiusCm);
			const float WantedCm3 = SplashLitresPerMps * (FallSpeedMps - SplashImpactMps) * (1.0f - 0.7f * Compaction) * 1000.0f;
			const float VolumeCm3 = FMath::Min(WantedCm3, AvailableCm3);
			if (VolumeCm3 > 0.5f)
			{
				// Two fans, one off each side of the tyre, low and wide.
				const float SplashSpeed = FMath::Min(FallSpeedMps * 0.8f, 8.0f) * CmPerM;
				const FVector Launch(ContactCm.X, ContactCm.Y, GroundCm + 3.0f);
				// One scoop, two throws: the shortfall is shared by linking both
				// to the same stroke and halving the request each side.
				const int32 Link = B->ScoopDirt(ContactCm, ScoopRadiusCm, VolumeCm3, 0.15f);
				for (int32 Side = -1; Side <= 1; Side += 2)
				{
					const FVector Dir = (Lateral * static_cast<float>(Side) * 0.85f + Normal * 0.5f).GetSafeNormal();
					B->SpawnParcels(Launch, Dir * SplashSpeed + VelocityMps * CmPerM * 0.5f, 30.0f, VolumeCm3 * 0.5f, Moisture, Compaction, Link);
					B->SpawnDust(Launch, Dir * SplashSpeed * 0.5f, 45.0f,
								 FMath::RoundToInt(B->Settings.DustPerLitre * VolumeCm3 * 0.5f / 1000.0f * (1.0f - Moisture)), Moisture);
				}
				RoostLitresTotal += VolumeCm3 / 1000.0f;
			}
		}

		// --- the soil's answer to this load ----------------------------------------------
		float SinkageM, ContactLengthM, ResistanceN;
		SoilResponse(Compaction, Moisture, Load, SinkageM, ContactLengthM, ResistanceN);

		// Mohr-Coulomb ceiling on traction: cohesion over the whole contact patch
		// plus friction under the load. Cohesion gives grip even under a light
		// wheel; saturation takes both away.
		float TanPhi = 0.6f, CohesionKPa = 0.0f;
		DirtSoilStrength(Compaction, Moisture, B->Settings, TanPhi, CohesionKPa);
		const float PatchAreaM2 = WidthM * ContactLengthM;
		// Cohesion needs the knobs in the soil; the floating share of the patch has none.
		const float MaxTractionN = CohesionKPa * 1000.0f * PatchAreaM2 * (1.0f - Floating) + Load * TanPhi;
		LastFriction = MaxTractionN / FMath::Max(Load, 1.0f);

		// Janosi-Hanamoto: shear stress builds along the patch with the shear
		// displacement j = i x, so the mean over the patch of (1 - e^(-j/K)) is
		// the fraction of the ceiling the tyre actually gets at this slip.
		SlipMps = WheelOmega * RadiusM - VForward;
		SlipRatio = SlipMps / FMath::Max3(FMath::Abs(WheelOmega * RadiusM), FMath::Abs(VForward), 0.3f);
		const float Kj = FMath::Lerp(ShearModulusLooseM, ShearModulusDenseM, Compaction);
		const float A = FMath::Abs(SlipRatio) * ContactLengthM / Kj;
		const float Build = (A > 1e-4f) ? 1.0f - (1.0f - FMath::Exp(-A)) / A : 0.0f;
		// Shear can only ever bring the tyre surface and the ground to the same
		// speed; a substep must not carry it past that. The impulse that stops
		// the slip within the step (wheel spin and vehicle mass together) caps
		// the force. Without this cap the wheel speed chattered about zero at
		// low throttle and the tyre never got going.
		const float EffMass = 1.0f / (1.0f / MassKg + RadiusM * RadiusM / Inertia);
		const float StopN = EffMass * FMath::Abs(SlipMps) / Dt;
		const float TractionN = FMath::Sign(SlipRatio) * FMath::Min(MaxTractionN * Build, StopN);

		// Motion resistance: the rut being pressed, plus a floor for hardpack.
		const float RollingN = -FMath::Sign(VForward) * (ResistanceN + BaseRollingResistance * Load)
			* FMath::Min(1.0f, FMath::Abs(VForward) / 0.2f);

		// Bulldozing: the wedge of yielding dirt ahead of the tyre resists with
		// its passive earth pressure, R_b = b (1/2 gamma z^2 K_pg + c z K_pc),
		// Rankine's K_p = tan^2(45 + phi/2). Twice the height, four times the push.
		float PloughN = 0.0f;
		if (bPartPlough && WedgeAheadM > 0.0f && Carried < 1.0f)
		{
			const float Phi = FMath::Atan(FMath::Max(TanPhiHeap, 0.05f));
			const float Kp = FMath::Square(FMath::Tan(PI / 4.0f + Phi / 2.0f));
			const float Z = FMath::Min(WedgeAheadM, RadiusM);
			PloughN = WidthM * (0.5f * B->Settings.UnitWeightKNm3 * 1000.0f * Z * Z * Kp
								+ CohesionHeapKPa * 1000.0f * Z * 2.0f * FMath::Sqrt(Kp)) * (1.0f - Carried);
		}
		LastPloughN = PloughN;
		// It can slow the tyre to a stop against the pile, never push it back.
		const float PloughImpulse = FMath::Min(PloughN * Dt, FMath::Abs(VForward) * MassKg);

		VelocityMps += FwdGround * ((TractionN + RollingN) / MassKg) * Dt - FwdGround * FMath::Sign(VForward) * (PloughImpulse / MassKg);
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
		// What the tyre removes from its own path: the wedge ahead, above the
		// floor it rides on, for the share that will not hold.
		StrokePloughM2 += BladeM * (1.0f - Carried) * Moved;
		StrokeSinkageM += SinkageM * (1.0f + SlipSinkage * FMath::Abs(SlipRatio)) * Moved;
		const float ShearSpeed = FMath::Max(FMath::Abs(SlipMps) - RoostSlipThresholdMps, 0.0f);
		StrokeSlipM += FMath::Min(ShearSpeed, RoostSlipCapMps) * Dt;
		StrokeSlipSign = SlipMps;
		StrokeSlipRatio = SlipRatio;
		// Sideways slither: the flank of the tyre ploughing dirt outward in a corner.
		const float SideShear = FMath::Max(FMath::Abs(VLateral) - SpraySlipThresholdMps, 0.0f);
		StrokeSideSlipM += FMath::Min(SideShear, RoostSlipCapMps) * Dt;
		StrokeSideSign = VLateral;
		StrokeTimeS += Dt;

		// One batch of strokes every sixth of a radius travelled, or every 1/20 s
		// when spinning on the spot.
		if (bDeformsDirt && (StrokeDistanceM >= RadiusM / 6.0f || ((StrokeSlipM > 0.0f || StrokeSideSlipM > 0.0f) && StrokeTimeS >= 0.05f)))
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
				// The ground is pressed where the tyre first meets it, at the front
				// of the contact patch, so the axle always rides on floor it has
				// already made. Pressed under the axle instead, the unpressed ground
				// ahead was a step the tyre had to climb every stroke, on top of
				// the Bekker resistance that already charges for pressing it: a
				// wheel at quarter throttle dug itself in and never got going.
				const FVector2D PressCm = ContactCm + FVector2D(FwdGround.X, FwdGround.Y) * FMath::Sign(VForward) * (0.5f * ContactLengthM * CmPerM);
				if (RutCm > 0.005f && bPartRut)
				{
					// No disturb: a tyre pressing a rut is packing, not breaking up.
					B->ApplyBrush(PressCm, HalfWidthCm * 1.2f, RutCm, EDirtBrushMode::Dig, 0.0f);
				}
				if (bPartPack)
				{
					B->ApplyBrush(PressCm, HalfWidthCm * 1.3f, PackPerPass * PassFraction * LoadFactor,
								  EDirtBrushMode::Pack, -1.0f, /*bProctor*/ true);
				}

				// The heap the tyre sank through is shoved ahead of it: the swept
				// volume (width x thickness sunk through x distance) moves from under
				// the contact to just in front, where it piles up and, next stroke,
				// stands higher, carries more, and pushes back harder.
				if (bPartPlough && StrokePloughM2 > 0.0f)
				{
					// Taken from the wedge just ahead of the contact, put down a
					// tyre radius further on: the blade of a bulldozer.
					const float ScoopRadiusCm = HalfWidthCm * 1.2f;
					const float SolidFraction = DirtSolidFraction(Compaction, B->Settings);
					const float WantedCm3 = WidthM * CmPerM * StrokePloughM2 * CmPerM * CmPerM * SolidFraction;
					const FVector2D Fwd2 = FVector2D(FwdGround.X, FwdGround.Y) * FMath::Sign(VForward);
					const FVector2D WedgeCm = ContactCm + Fwd2 * (RadiusM * PloughReach * 0.7f * CmPerM);
					const float VolumeCm3 = FMath::Min(WantedCm3, B->MaxScoopCm3(State.R, ScoopRadiusCm));
					if (VolumeCm3 > 0.5f)
					{
						// Put down over the footprint a heap of that size would spread
						// to at its angle of repose, not as a spike: a narrow dump stood
						// as a 25 cm spire in front of the tyre and stalled it against
						// its own spoil before the slump could knock it down.
						// A tyre is a narrow blade with no wings: what it pushes spills
						// round both sides as much as it piles up ahead. Ahead alone, the
						// spoil built a bow wave the tyre shoved along the whole rut.
						const float TanPhiSpoil = FMath::Max(TanPhiHeap, 0.4f);
						const float SpreadCm = FMath::Clamp(FMath::Sqrt(VolumeCm3 * 3.0f / (PI * TanPhiSpoil)) * 0.5f, HalfWidthCm * 1.6f, RadiusM * CmPerM);
						const FVector2D Side2(-Fwd2.Y, Fwd2.X);
						const FVector2D AheadCm = ContactCm + Fwd2 * (RadiusM * 0.8f * CmPerM + SpreadCm * 0.5f);
						const float Churn = 0.5f * (1.0f - Carried);
						B->TransferDirt(WedgeCm, ScoopRadiusCm, AheadCm, SpreadCm, VolumeCm3 * 0.4f, Churn);
						for (float Sign : { -1.0f, 1.0f })
						{
							const FVector2D SideCm = ContactCm + Fwd2 * (RadiusM * 0.4f * CmPerM) + Side2 * Sign * (HalfWidthCm * 1.5f + SpreadCm * 0.5f);
							B->TransferDirt(WedgeCm, ScoopRadiusCm, SideCm, SpreadCm, VolumeCm3 * 0.3f, Churn);
						}
						PloughLitresTotal += VolumeCm3 / 1000.0f;
					}
				}
			}

			// Past the traction limit the lugs shear the soil off and fling it at
			// about the slip speed: roost. Volume rate = width x failure depth x
			// slip speed. It leaves the ground as parcels and comes back down as
			// parcels; nothing is dumped by fiat.
			if (StrokeSlipM > 0.001f && bPartRoost)
			{
				// Never scoop more than the layer under the tyre can give: past
				// bedrock there is nothing to throw, and parcels spawned for dirt
				// the shader could not remove would be dirt from nowhere.
				const float ScoopRadiusCm = HalfWidthCm * 1.1f;
				const float AvailableCm3 = B->MaxScoopCm3(State.R, ScoopRadiusCm);
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
					const int32 Link = B->ScoopDirt(ContactCm, ScoopRadiusCm, VolumeCm3, 0.08f);
					B->SpawnParcels(LaunchCm, Dir * EjectMps * CmPerM + VelocityMps * CmPerM, RoostSpreadDeg, VolumeCm3, Moisture, Compaction, Link);
					// Dry dirt roosts with a plume of dust; wet dirt does not.
					B->SpawnDust(LaunchCm, Dir * EjectMps * CmPerM * 0.5f + VelocityMps * CmPerM, RoostSpreadDeg + 20.0f,
								 FMath::RoundToInt(B->Settings.DustPerLitre * VolumeCm3 / 1000.0f * (1.0f - Moisture)), Moisture);
					RoostLitresTotal += VolumeCm3 / 1000.0f;
				}
			}

			// Spray off a berm: sliding sideways, the flank shears dirt off and
			// throws it outward, low and fast. Same excavation rule as roost.
			if (StrokeSideSlipM > 0.001f && bPartSpray)
			{
				const float ScoopRadiusCm = HalfWidthCm * 1.3f;
				const float AvailableCm3 = B->MaxScoopCm3(State.R, ScoopRadiusCm);
				const float FailureDepthCm = SprayFailureDepthCm * (1.0f - 0.6f * Compaction);
				const float SolidFraction = DirtSolidFraction(Compaction, B->Settings);
				const float WantedCm3 = ContactPatchLength(Compaction, Moisture, Load) * CmPerM * FailureDepthCm * SolidFraction * StrokeSideSlipM * CmPerM * LoadFactor;
				const float VolumeCm3 = FMath::Min(WantedCm3, AvailableCm3);
				if (VolumeCm3 > 0.5f)
				{
					const float OutSign = (StrokeSideSign >= 0.0f) ? 1.0f : -1.0f;     // thrown the way the tyre is sliding
					const float EjectMps = FMath::Min(FMath::Abs(StrokeSideSign), 12.0f) * 0.9f;
					const FVector Dir = (Lateral * OutSign * 0.9f + Normal * 0.35f).GetSafeNormal();
					const FVector LaunchCm = FVector(ContactCm.X, ContactCm.Y, GroundCm) + Lateral * OutSign * HalfWidthCm + FVector(0, 0, 3.0f);
					const int32 Link = B->ScoopDirt(ContactCm, ScoopRadiusCm, VolumeCm3, 0.1f);
					B->SpawnParcels(LaunchCm, Dir * EjectMps * CmPerM + VelocityMps * CmPerM * 0.7f, 18.0f, VolumeCm3, Moisture, Compaction, Link);
					B->SpawnDust(LaunchCm, Dir * EjectMps * CmPerM * 0.5f + VelocityMps * CmPerM * 0.7f, 35.0f,
								 FMath::RoundToInt(B->Settings.DustPerLitre * VolumeCm3 / 1000.0f * (1.0f - Moisture)), Moisture);
					RoostLitresTotal += VolumeCm3 / 1000.0f;
				}
			}

			StrokeDistanceM = 0.0f;
			StrokeSinkageM = 0.0f;
			StrokeSlipM = 0.0f;
			StrokeSideSlipM = 0.0f;
			StrokePloughM2 = 0.0f;
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
