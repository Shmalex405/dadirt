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
	ApplyTyre();

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

void ADirtWheel::SetOrbit(FVector2D CentreM, float InRadiusM)
{
	bOrbit = true;
	OrbitBias = 0.0f;
	OrbitCentreM = CentreM;
	OrbitRadiusM = FMath::Max(InRadiusM, 1.0f);
	// Go round the way the wheel is already pointing.
	const FVector2D Rel = FVector2D(GetActorLocation()) / CmPerM - CentreM;
	const float HeadingRad = FMath::DegreesToRadians(HeadingDeg);
	const FVector2D Fwd(FMath::Cos(HeadingRad), FMath::Sin(HeadingRad));
	OrbitSign = (Rel.X * Fwd.Y - Rel.Y * Fwd.X >= 0.0f) ? 1.0f : -1.0f;
	UE_LOG(LogDirtWheel, Log, TEXT("Wheel orbit: centre (%.1f, %.1f) m, radius %.1f m, %s."),
		CentreM.X, CentreM.Y, OrbitRadiusM, OrbitSign > 0.0f ? TEXT("anticlockwise") : TEXT("clockwise"));
}

void ADirtWheel::ApplyTyre()
{
	RadiusM = FMath::Max(OuterDiameterM * 0.5f, 0.1f);
	// A circular arc through the crown and both tread edges: chord half-width w,
	// sagitta d, radius (w^2 + d^2) / 2d.
	const float W = FMath::Max(TreadHalfWidthM, 0.01f);
	const float D = FMath::Clamp(CrownDropM, 0.002f, W);
	CrownRadiusM = (W * W + D * D) / (2.0f * D);
	LastContactWidthM = ContactWidthM(0.0f, TyreDeflectionM(MassKg * Gravity));
	if (Tyre)
	{
		Tyre->SetRelativeScale3D(FVector(RadiusM * 2.0f, RadiusM * 2.0f, SectionWidthM));
	}
	const float Defl = TyreDeflectionM(MassKg * Gravity);
	UE_LOG(LogDirtWheel, Log, TEXT("Tyre: %.0f mm outside diameter, section %.0f mm, tread %.0f mm across on a %.0f mm crown radius, knobs %.0f mm, land ratio %.2f, %.0f kPa. ")
		TEXT("Under %.0f N it deflects %.1f cm (%.0f kN/m), a hard-ground patch %.0f x %.0f mm; wheel %.1f kg."),
		OuterDiameterM * 1000.0f, SectionWidthM * 1000.0f, TreadHalfWidthM * 2000.0f, CrownRadiusM * 1000.0f, KnobHeightCm * 10.0f, LandRatio, PressureKPa,
		MassKg * Gravity, Defl * CmPerM, TyreStiffnessNPerM() * 0.001f,
		2000.0f * FMath::Sqrt(2.0f * RadiusM * Defl), 1000.0f * ContactWidthM(0.0f, Defl), WheelMassKg);
}

bool ADirtWheel::SetTyrePreset(const FString& Name)
{
	const FString N = Name.ToLower();
	if (N == TEXT("rear"))            // 110/90-19 soft-intermediate rear, as above
	{
		OuterDiameterM = 0.680f; SectionWidthM = 0.110f; TreadHalfWidthM = 0.0525f; CrownDropM = 0.035f;
		KnobHeightCm = 1.9f; LandRatio = 0.20f; PressureKPa = 83.0f; WheelMassKg = 11.7f; MassKg = 100.0f;
	}
	else if (N == TEXT("front"))      // 80/100-21: 533.4 + 2 x 80 = 693 mm, narrow and more pointed, 12-13 mm blocks, 3.8 kg tyre
	{
		OuterDiameterM = 0.693f; SectionWidthM = 0.080f; TreadHalfWidthM = 0.038f; CrownDropM = 0.030f;
		KnobHeightCm = 1.3f; LandRatio = 0.22f; PressureKPa = 90.0f; WheelMassKg = 8.0f; MassKg = 91.0f;
	}
	else if (N == TEXT("sand"))       // a sand/mud rear (MX12 class): the tallest, sparsest blocks, run soft
	{
		OuterDiameterM = 0.680f; SectionWidthM = 0.110f; TreadHalfWidthM = 0.0525f; CrownDropM = 0.035f;
		KnobHeightCm = 2.0f; LandRatio = 0.12f; PressureKPa = 79.0f; WheelMassKg = 11.7f; MassKg = 100.0f;
	}
	else if (N == TEXT("hard"))       // a hard-terrain rear (MX53 class): short close blocks, run harder
	{
		OuterDiameterM = 0.680f; SectionWidthM = 0.110f; TreadHalfWidthM = 0.0525f; CrownDropM = 0.035f;
		KnobHeightCm = 1.3f; LandRatio = 0.28f; PressureKPa = 93.0f; WheelMassKg = 11.7f; MassKg = 100.0f;
	}
	else
	{
		return false;
	}
	ApplyTyre();
	return true;
}

float ADirtWheel::TyreStiffnessNPerM() const
{
	return 2.0f * PI * PressureKPa * 1000.0f * FMath::Sqrt(FMath::Max(RadiusM * CrownRadiusM, 1e-4f));
}

float ADirtWheel::ContactWidthM(float SinkM, float DeflectionM) const
{
	const float Depth = FMath::Max(SinkM + DeflectionM, 0.0f);
	return FMath::Clamp(2.0f * FMath::Sqrt(2.0f * CrownRadiusM * Depth), 0.01f, 2.0f * TreadHalfWidthM);
}

void ADirtWheel::SetInputs(float InThrottle, float InSteer, float InBrake)
{
	Throttle = InThrottle;
	Steer = InSteer;
	Brake = InBrake;
	bOrbit = false;
	bLap = false;
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
			TEXT("pond %.1f cm drag %.0f N lift %.0f N, lateral %.0f N at %.1f deg, dyn %+.1f g, impact %.1f g %.1f cm, patch %.0f mm wide at %+.0f cm, ")
			TEXT("compaction %.2f, moisture %.2f, %s (air %.2f s, max %.1f cm), odometer %.1f m, roost %.2f L, ploughed %.2f L, shoved %.2f L, %d parcels live"),
			P.X / CmPerM, P.Y / CmPerM, HeadingDeg, VelocityMps.Size(), VelocityMps.Size() * 3.6f,
			WheelOmega * RadiusM, LastSlipMps, LastSlipRatio, LastFriction, LastTractionN, LastSinkageCm, LastResistanceN,
			LastHeapCm, LastCarried, LastPloughN,
			LastPondCm, LastWaterDragN, LastHydroLiftN, LastLateralN, LastSlipAngleDeg, LastDynamicG, LastImpactG, LastImpactCm, LastContactWidthM * 1000.0f, LastContactOffsetM * CmPerM,
			LastCompaction, LastMoisture, bOnGround ? TEXT("on ground") : TEXT("airborne"), AirTimeS, MaxAirCm, OdometerM, RoostLitresTotal,
			PloughLitresTotal, ShovedLitresTotal, B ? B->GetLiveParcels() : 0);
	}
}

float ADirtWheel::ContactPatchLength(const FDirtSoil& Soil, float Compaction, float Moisture, float LoadN) const
{
	float SinkageM, ContactLengthM, ContactWidth, ResistanceN;
	SoilResponse(Soil, Compaction, Moisture, LoadN, SinkageM, ContactLengthM, ContactWidth, ResistanceN);
	return ContactLengthM;
}

void ADirtWheel::BekkerConstants(const FDirtSoil& Soil, float Compaction, float Moisture, float& OutN, float& OutKcKN, float& OutKphiKN) const
{
	const float C = FMath::Clamp(Compaction, 0.0f, 1.0f);
	const float Sat = FMath::SmoothStep(0.55f, 1.0f, FMath::Clamp(Moisture, 0.0f, 1.0f));
	const float Weak = 1.0f - Soil.SaturationStiffnessLoss * Sat;

	OutN = FMath::Lerp(Soil.BekkerNLoose, Soil.BekkerNDense, C);
	const float Kc = FMath::Exp(FMath::Lerp(FMath::Loge(FMath::Max(Soil.BekkerKcLoose, 0.01f)), FMath::Loge(FMath::Max(Soil.BekkerKcDense, 0.01f)), C)) * Weak;
	const float Kphi = FMath::Exp(FMath::Lerp(FMath::Loge(FMath::Max(Soil.BekkerKphiLoose, 1.0f)), FMath::Loge(FMath::Max(Soil.BekkerKphiDense, 1.0f)), C)) * Weak;
	OutKcKN = Kc;
	OutKphiKN = Kphi;
}

void ADirtWheel::ImpactResponse(const FDirtSoil& Soil, float Compaction, float Moisture, float InFallSpeedMps,
								float& OutPeakN, float& OutPunchM, float& OutTyreDeflectionM) const
{
	// The landing's energy, 1/2 m v^2, is taken by two springs in series at one
	// force F: the tyre (linear, k_t = its load over its deflection: a 12 psi MX
	// tyre is about 45 kN/m) stores F^2 / 2 k_t, and the soil, whose force at a
	// punch depth z is Bekker's pressure over the patch it has made by then,
	//   F(z) = K b 2 sqrt(2 r) z^(n + 1/2),   W(z) = K b 2 sqrt(2 r) z^(n + 3/2) / (n + 3/2).
	// The sum is monotonic in F, so bisect for the F that spends the energy.
	// There is no suspension yet: the whole mass lands on the tyre, which is
	// why these hits are hard. The bike build puts a spring and a damper
	// between the wheel and the rest (docs/BikeEngineering.md).
	float N = 1.0f, KcKN = 1.0f, KphiKN = 1000.0f;
	BekkerConstants(Soil, Compaction, Moisture, N, KcKN, KphiKN);
	const float Kt = TyreStiffnessNPerM();
	const float Energy = 0.5f * MassKg * InFallSpeedMps * InFallSpeedMps;

	// The round crown's contact widens as the tyre punches in, so the soil's
	// stiffness K b depends on the depth: two fixed-point passes settle it.
	// The tyre spring runs out at the rim: past MaxTyreDeflectionM the carcass
	// is bottomed and stores nothing more, and the soil must take the rest.
	const float DeflMax = MaxTyreDeflectionM();
	const auto TyreDefl = [&](float F) { return FMath::Min(F / Kt, DeflMax); };
	const auto KnFor = [&](float Z, float F)
	{
		const float B = ContactWidthM(Z, TyreDefl(F));
		return (KcKN / B + KphiKN) * 1000.0f * B * 2.0f * FMath::Sqrt(2.0f * RadiusM);   // N / m^(n+1/2)
	};
	const auto PunchFor = [&](float F)
	{
		float Z = 0.02f;
		for (int32 It = 0; It < 3; ++It)
		{
			Z = FMath::Pow(F / KnFor(Z, F), 1.0f / (N + 0.5f));
		}
		return Z;
	};
	const auto Spent = [&](float F)
	{
		const float Z = PunchFor(F);
		const float D = TyreDefl(F);
		return 0.5f * Kt * D * D + KnFor(Z, F) * FMath::Pow(Z, N + 1.5f) / (N + 1.5f);
	};

	float Lo = 0.0f, Hi = MassKg * Gravity * 80.0f;
	for (int32 It = 0; It < 40; ++It)
	{
		const float Mid = 0.5f * (Lo + Hi);
		if (Spent(Mid) < Energy) Lo = Mid; else Hi = Mid;
	}
	OutPeakN = 0.5f * (Lo + Hi);
	OutPunchM = FMath::Clamp(PunchFor(OutPeakN), 0.0f, RadiusM * 0.5f);
	OutTyreDeflectionM = TyreDefl(OutPeakN);
}

void ADirtWheel::SoilResponse(const FDirtSoil& Soil, float Compaction, float Moisture, float LoadN,
							  float& OutSinkageM, float& OutContactLengthM, float& OutContactWidthM, float& OutResistanceN) const
{
	// Bekker: p = (k_c / b + k_phi) z^n, with the rigid wheel's contact length
	// l = 2 sqrt(2 r z) and p = N / (b l). Both sides are power laws in z, so
	// the sinkage has a closed form:  z = (N / (K b 2 sqrt(2 r)))^(1 / (n + 1/2)).
	// The patch width b is the round crown's chord at this depth (sinkage plus
	// the tyre's own flattening), so b and z settle together in a few passes: a
	// narrow strip on hardpack, the whole tread in loam.
	float N = 1.0f, KcKN = 1.0f, KphiKN = 1000.0f;
	BekkerConstants(Soil, Compaction, Moisture, N, KcKN, KphiKN);
	const float LoadKN = FMath::Max(LoadN, 1.0f) * 0.001f;
	const float Defl = TyreDeflectionM(FMath::Max(LoadN, 1.0f));

	float B = ContactWidthM(0.0f, Defl);
	float Z = 0.0f;
	float K = KcKN / B + KphiKN;
	for (int32 It = 0; It < 3; ++It)
	{
		K = KcKN / B + KphiKN;
		Z = FMath::Clamp(FMath::Pow(LoadKN / (K * B * 2.0f * FMath::Sqrt(2.0f * RadiusM)), 1.0f / (N + 0.5f)), 0.0f, RadiusM * 0.5f);
		B = ContactWidthM(Z, Defl);
	}
	OutSinkageM = Z;
	OutContactWidthM = B;
	// The patch is what the ground gives (sinkage) plus what the tyre gives (its
	// flattening), one arc cut at both.
	OutContactLengthM = FMath::Max(2.0f * FMath::Sqrt(2.0f * RadiusM * (OutSinkageM + Defl)), 0.03f);

	// Motion resistance is the work of pressing the rut: R = b K z^(n+1) / (n+1).
	OutResistanceN = 1000.0f * B * K * FMath::Pow(OutSinkageM, N + 1.0f) / (N + 1.0f);
}

void ADirtWheel::Step(float Dt)
{
	ADirtBox* B = Box.Get();
	FVector PosM = GetActorLocation() / CmPerM;

	// --- steer: heading turns with speed, like a bike, not on the spot -------------
	const float Speed = static_cast<float>(FVector2D(VelocityMps).Size());
	const float SpeedFactor = FMath::Clamp(Speed / 3.0f, 0.0f, 1.0f);
	if (bOrbit)
	{
		// Pure pursuit: aim at the point on the circle a little way ahead. The
		// rider who takes the same line every lap.
		const FVector2D Rel = FVector2D(PosM) - OrbitCentreM;
		const float Dist = static_cast<float>(Rel.Size());
		const float Theta = FMath::Atan2(Rel.Y, Rel.X);
		const float LookaheadM = FMath::Max(1.5f, 0.4f * Speed);
		const float Target = Theta + OrbitSign * LookaheadM / OrbitRadiusM;
		const FVector2D TargetPos = OrbitCentreM + FVector2D(FMath::Cos(Target), FMath::Sin(Target)) * OrbitRadiusM;
		const FVector2D ToTarget = TargetPos - FVector2D(PosM);
		const float WantDeg = FMath::RadiansToDegrees(FMath::Atan2(ToTarget.Y, ToTarget.X));
		// A tyre at a slip angle travels outside where it points, so pursuit alone
		// runs a metre wide: a radial term, and a slow integral of it, hold the line.
		const float Radial = (Dist - OrbitRadiusM) * OrbitSign;
		OrbitBias = FMath::Clamp(OrbitBias + Radial * 0.3f * Dt, -0.6f, 0.6f);
		Steer = FMath::Clamp(FMath::FindDeltaAngleDegrees(HeadingDeg, WantDeg) / 20.0f + Radial * 0.6f + OrbitBias, -1.0f, 1.0f);
	}
	if (bLap && B && B->TrackLineM.Num() > 2)
	{
		// The same pursuit along the track's centre line: nearest point (searched
		// near last substep's), a target a little way further round the lap, and
		// a cross-track term with a slow integral so the slip angle does not
		// carry the line wide through every corner.
		const TArray<FVector2D>& L = B->TrackLineM;
		const int32 N = L.Num();
		const FVector2D P2(PosM);
		// Nearest point that runs the way the wheel is pointing: the track folds
		// back on itself thirty metres over, and a wheel thrown off a jump that
		// locked onto the neighbouring pass drove round in circles for ever.
		const FVector2D Fwd2(FMath::Cos(FMath::DegreesToRadians(HeadingDeg)), FMath::Sin(FMath::DegreesToRadians(HeadingDeg)));
		const auto Nearest = [&](int32 From, int32 To, bool bSameWay) -> int32
		{
			int32 Found = -1;
			double FoundD = 1e18;
			for (int32 K = From; K <= To; ++K)
			{
				const int32 I = ((K % N) + N) % N;
				if (bSameWay && FVector2D::DotProduct((L[(I + 3) % N] - L[I]).GetSafeNormal(), Fwd2) < 0.0f)
				{
					continue;
				}
				const double D = FVector2D::DistSquared(L[I], P2);
				if (D < FoundD) { FoundD = D; Found = I; }
			}
			return (Found >= 0 && FoundD < 15.0 * 15.0) ? Found : -1;
		};
		int32 Best = (LapIndex >= 0) ? Nearest(LapIndex - 60, LapIndex + 60, true) : -1;
		if (Best < 0) Best = Nearest(0, N - 1, true);
		if (Best < 0) Best = Nearest(0, N - 1, false);
		if (Best < 0) Best = 0;
		LapIndex = Best;
		const float LookaheadM = FMath::Max(2.0f, 0.5f * Speed);
		float Along = 0.0f;
		int32 J = Best;
		for (int32 Guard = 0; Guard < N && Along < LookaheadM; ++Guard)
		{
			Along += static_cast<float>(FVector2D::Distance(L[J], L[(J + 1) % N]));
			J = (J + 1) % N;
		}
		const FVector2D ToTarget = L[J] - P2;
		const float WantDeg = FMath::RadiansToDegrees(FMath::Atan2(ToTarget.Y, ToTarget.X));
		const FVector2D Tangent = (L[(Best + 1) % N] - L[Best]).GetSafeNormal();
		const FVector2D Off = P2 - L[Best];
		const float Cross = static_cast<float>(Tangent.X * Off.Y - Tangent.Y * Off.X);   // + when left of the line
		OrbitBias = FMath::Clamp(OrbitBias - Cross * 0.3f * Dt, -0.6f, 0.6f);
		Steer = FMath::Clamp(FMath::FindDeltaAngleDegrees(HeadingDeg, WantDeg) / 20.0f - Cross * 0.6f + OrbitBias, -1.0f, 1.0f);
	}
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
	static const FDirtSoil NoSoil;
	const FDirtSoil& Soil = B ? B->SoilAtWorld(FVector2D(PosM) * CmPerM) : NoSoil;
	float ContactOffsetM = 0.0f;
	if (bHaveGround)
	{
		const FVector Right(-Forward.Y, Forward.X, 0.0f);
		const FVector2D Fwd2(Forward.X, Forward.Y);
		// The two edges of the ground contact, as wide as the crown touched last
		// substep: on hardpack a narrow strip, in loam most of the tread.
		const FVector2D EdgeOffsetCm = FVector2D(Right.X, Right.Y) * (LastContactWidthM * 0.5f * CmPerM);

		// A wheel is round: it rests wherever its circumference first meets the
		// ground, not on the ground under its axle. Along the rolling direction
		// the tyre sits (R - sqrt(R^2 - x^2)) higher at x from the contact, so
		// the ground that holds it is the highest of (height - that sag) along
		// the profile. A hole narrower than the tyre is bridged; a bump ahead
		// meets the tyre's front and pushes it up and back along the radius
		// there, which is a kicker. Read only under the axle, the wheel fell
		// into every crater it had punched and punched it deeper.
		float BestCm = -1e9f;
		int32 BestK = 0;
		float BestLeftCm = 0.0f, BestRightCm = 0.0f;
		for (int32 K = -3; K <= 3; ++K)
		{
			const float X = K * RadiusM * 0.2f;
			const float SagCm = (RadiusM - FMath::Sqrt(FMath::Max(RadiusM * RadiusM - X * X, 0.0f))) * CmPerM;
			const FVector2D At = FVector2D(PosM) * CmPerM + Fwd2 * (X * CmPerM);
			float MidCm = 0.0f, LeftCm = 0.0f, RightCm = 0.0f;
			FVector Unused;
			if (!B->SampleHeightWindow(WindowId, At, MidCm, Unused))
			{
				continue;
			}
			if (!B->SampleHeightWindow(WindowId, At - EdgeOffsetCm, LeftCm, Unused)) LeftCm = MidCm;
			if (!B->SampleHeightWindow(WindowId, At + EdgeOffsetCm, RightCm, Unused)) RightCm = MidCm;
			// The front of the patch presses the ground as it arrives, so the
			// profile ahead is read as it will be once pressed: a fresh loose
			// ridge a couple of centimetres tall is squashed, not ridden over.
			const float PressedCm = (K > 0) ? PlasticSinkage * LastSinkageCm : 0.0f;
			const float Held = FMath::Max3(MidCm, LeftCm, RightCm) - SagCm - PressedCm;
			if (Held > BestCm)
			{
				BestCm = Held;
				BestK = K;
				BestLeftCm = LeftCm;
				BestRightCm = RightCm;
			}
		}
		if (BestCm > -1e8f)
		{
			GroundCm = BestCm;
			ContactOffsetM = BestK * RadiusM * 0.2f;
			LateralSlope = (BestRightCm - BestLeftCm) / (LastContactWidthM * CmPerM);
			if (BestK != 0)
			{
				// The dirt the tyre is actually on, and the direction it pushes
				// back: the radius from the contact to the axle.
				const FVector2D At = FVector2D(PosM) * CmPerM + Fwd2 * (ContactOffsetM * CmPerM);
				float UnusedH = 0.0f;
				FVector UnusedN;
				B->SampleHeightWindow(WindowId, At, UnusedH, UnusedN, &State, &PondCm);
				const float Sx = ContactOffsetM / RadiusM;
				Normal = (Forward * (-Sx) + FVector::UpVector * FMath::Sqrt(FMath::Max(1.0f - Sx * Sx, 0.0f))).GetSafeNormal();
			}
		}
	}
	LastContactOffsetM = ContactOffsetM;
	const FVector2D ContactXYCm = FVector2D(PosM) * CmPerM + FVector2D(Forward.X, Forward.Y) * (ContactOffsetM * CmPerM);

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
		float StaticSinkM = 0.0f, UnusedL = 0.0f, UnusedB = 0.0f, UnusedR = 0.0f;
		SoilResponse(Soil, FMath::Clamp(State.G, 0.0f, 1.0f), FMath::Clamp(State.B, 0.0f, 1.0f), MassKg * Gravity, StaticSinkM, UnusedL, UnusedB, UnusedR);
		if (bPartPlough && ObstacleM > StaticSinkM + 0.005f)
		{
			// Climb it or shove it, whichever is cheaper.
			DirtSoilStrength(FMath::Clamp(State.G, 0.0f, 1.0f), FMath::Clamp(State.B, 0.0f, 1.0f), Soil, TanPhiHeap, CohesionHeapKPa);
			const float Phi = FMath::Atan(FMath::Max(TanPhiHeap, 0.05f));
			const float Kp = FMath::Square(FMath::Tan(PI / 4.0f + Phi / 2.0f));
			const float ShoveN = LastContactWidthM * (0.5f * Soil.UnitWeightKNm3 * 1000.0f * ObstacleM * ObstacleM * Kp
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
	SinceImpactS += Dt;
	StrokeLoadTimeS += Dt;

	// --- what the dirt under the tyre is like --------------------------------------
	// Under standing water the surface is saturated whatever the moisture
	// channel has had time to say: the knobs are in soup.
	const float Compaction = FMath::Clamp(State.G, 0.0f, 1.0f);
	const float Moisture = FMath::Max(FMath::Clamp(State.B, 0.0f, 1.0f), FMath::Clamp(PondCm / 1.0f, 0.0f, 1.0f));
	LastCompaction = Compaction;
	LastMoisture = Moisture;

	// --- wheel spin from the engine and brake --------------------------------------
	// Most of a wheel's mass is tyre, tube and rim, out at the radius: a ring
	// more than a disc.
	const float Inertia = 0.8f * WheelMassKg * RadiusM * RadiusM;

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
		// The velocity the ground takes away this substep IS its normal force,
		// m (-Vn) / dt: the weight on level ground, more in a transition or the
		// bottom of a bowl, where the tyre is being bent round a curve (m v^2 /
		// rho). What it gives beyond the static load is the dynamic load: it
		// presses the rut deeper, packs harder and grips more, capped so a
		// landing's first substep does not count twice (the impact model below
		// owns that).
		float DynamicN = 0.0f;
		if (Vn < 0.0f && Bottom <= GroundM)
		{
			VelocityMps -= Vn * Normal;
			const float StaticN = MassKg * Gravity * FMath::Max(Normal.Z, 0.3f);
			DynamicN = FMath::Clamp(MassKg * (-Vn) / Dt - StaticN, 0.0f, MaxDynamicLoadG * MassKg * Gravity);
		}
		LastDynamicG = DynamicN / (MassKg * Gravity);

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
			const float StaticDefl = TyreDeflectionM(MassKg * Gravity);
			const float PatchM2 = ContactWidthM(0.0f, StaticDefl) * FMath::Max(2.0f * FMath::Sqrt(2.0f * RadiusM * StaticDefl), 0.03f);
			HydroLiftN = 0.5f * RhoWater * GroundSpeed * GroundSpeed * PatchM2 * HydroLiftCoeff * Flooded;
			const float FrontalM2 = SectionWidthM * FMath::Min(PondM, 2.0f * RadiusM);
			WaterDragN = 0.5f * RhoWater * WaterDragCoeff * FrontalM2 * GroundSpeed * GroundSpeed;
		}
		const float FullLoad = MassKg * Gravity * FMath::Max(Normal.Z, 0.3f) + DynamicN;
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

		// --- a landing: Proctor's hammer ---------------------------------------------------
		// The tyre spring and the soil share the landing's energy at one peak
		// force; the punch it makes packs the ground at that load, its plastic
		// share stays as a crater whose rim is the heaved dirt, and past the
		// splash speed part of the punched dirt squirts out from under the tyre
		// instead of heaving. Loose dirt splashes half of it; packed dirt none.
		// A hop off a rut shoulder (under a metre a second, a 5 cm fall) is left
		// to the rolling dynamic load, which averages it; this is for landings.
		if (bTouchdown && FallSpeedMps > 1.0f && (SinceImpactS > 0.3f || FallSpeedMps > 2.0f))
		{
			SinceImpactS = 0.0f;
			float PeakN = 0.0f, PunchM = 0.0f, TyreM = 0.0f;
			ImpactResponse(Soil, Compaction, Moisture, FallSpeedMps, PeakN, PunchM, TyreM);
			const float ImpactG = PeakN / (MassKg * Gravity);
			LastImpactG = ImpactG;
			LastImpactCm = PunchM * CmPerM;

			if (bDeformsDirt)
			{
				const FVector2D ContactCm = ContactXYCm;
				// The patch at the bottom of the punch, as a disc of the same area:
				// the crown's chord at that depth by the arc's length.
				const float PunchWidthM = ContactWidthM(PunchM, TyreM);
				const float HalfWidthCm = PunchWidthM * 0.5f * CmPerM;
				const float PatchLengthM = 2.0f * FMath::Sqrt(2.0f * RadiusM * (PunchM + TyreM));
				const float PatchRadiusCm = FMath::Max(HalfWidthCm * 1.3f, FMath::Sqrt(PunchWidthM * PatchLengthM / PI) * CmPerM);

				if (bPartPack)
				{
					B->ApplyBrush(ContactCm, PatchRadiusCm, PackPerPass * ImpactG, EDirtBrushMode::Pack, -1.0f, /*bProctor*/ true);
				}

				const float PlasticCm = PlasticSinkage * PunchM * CmPerM;
				const float SplashShare = (bPartSplash && FallSpeedMps > SplashImpactMps) ? 0.5f * (1.0f - Compaction) : 0.0f;
				const float PunchBulkCm3 = PI * PatchRadiusCm * PatchRadiusCm * PlasticCm;
				if (PlasticCm * (1.0f - SplashShare) > 0.02f && bPartRut)
				{
					// No disturb: the ground under a landing is being packed, not broken up.
					B->ApplyBrush(ContactCm, PatchRadiusCm, PlasticCm * (1.0f - SplashShare), EDirtBrushMode::Dig, 0.0f);
				}
				const float ScoopRadiusCm = PatchRadiusCm * 1.1f;
				const float VolumeCm3 = FMath::Min(PunchBulkCm3 * SplashShare * DirtSolidFraction(Compaction, Soil), B->MaxScoopCm3(State.R, ScoopRadiusCm));
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
						// Carried along with the tyre's ground speed only: its fall speed
						// would drive them straight into the ground they came from.
						B->SpawnParcels(Launch, Dir * SplashSpeed + FVector(VelocityMps.X, VelocityMps.Y, 0.0f) * CmPerM * 0.5f, 30.0f, VolumeCm3 * 0.5f, Moisture, Compaction, Link);
						B->SpawnDust(Launch, Dir * SplashSpeed * 0.5f, 45.0f,
									 FMath::RoundToInt(B->Settings.DustPerLitre * Soil.Dustiness * VolumeCm3 * 0.5f / 1000.0f * (1.0f - Moisture)), Moisture);
					}
					RoostLitresTotal += VolumeCm3 / 1000.0f;
				}
			}
		}

		// --- the soil's answer to this load ----------------------------------------------
		float SinkageM, ContactLengthM, ContactWidth, ResistanceN;
		SoilResponse(Soil, Compaction, Moisture, Load, SinkageM, ContactLengthM, ContactWidth, ResistanceN);
		LastContactWidthM = ContactWidth;

		// Mohr-Coulomb ceiling on traction: cohesion over the whole contact patch
		// plus friction under the load. Cohesion gives grip even under a light
		// wheel; saturation takes both away.
		float TanPhi = 0.6f, CohesionKPa = 0.0f;
		DirtSoilStrength(Compaction, Moisture, Soil, TanPhi, CohesionKPa);
		const float PatchAreaM2 = ContactWidth * ContactLengthM;
		// Cohesion needs the knobs in the soil; the floating share of the patch has none.
		const float MaxTractionN = CohesionKPa * 1000.0f * PatchAreaM2 * (1.0f - Floating) + Load * TanPhi;
		LastFriction = MaxTractionN / FMath::Max(Load, 1.0f);

		// Janosi-Hanamoto: shear stress builds along the patch with the shear
		// displacement j = i x, so the mean over the patch of (1 - e^(-j/K)) is
		// the fraction of the ceiling the tyre actually gets at this slip.
		SlipMps = WheelOmega * RadiusM - VForward;
		SlipRatio = SlipMps / FMath::Max3(FMath::Abs(WheelOmega * RadiusM), FMath::Abs(VForward), 0.3f);
		const float Kj = FMath::Lerp(Soil.ShearModulusLooseM, Soil.ShearModulusDenseM, Compaction);
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
			PloughN = ContactWidth * (0.5f * Soil.UnitWeightKNm3 * 1000.0f * Z * Z * Kp
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

		// Lateral: the same Janosi build on the sideways shear displacement,
		// j = x tan(alpha) along the patch, up to what the friction circle has
		// left after the drive traction has taken its share. Half the grip
		// needs a slip angle of 5-10 degrees, as it does on a real tyre; a
		// spinning rear has almost nothing left to hold the side and steps out.
		const float TanAlpha = FMath::Abs(VLateral) / FMath::Max(FMath::Abs(VForward), 0.5f);
		const float Ay = TanAlpha * ContactLengthM / Kj;
		const float BuildY = (Ay > 1e-4f) ? 1.0f - (1.0f - FMath::Exp(-Ay)) / Ay : 0.0f;
		const float LateralMaxN = FMath::Sqrt(FMath::Max(MaxTractionN * MaxTractionN - TractionN * TractionN, 0.0f));
		const float LateralN = -FMath::Sign(VLateral) * LateralMaxN * BuildY;
		const float LateralImpulse = FMath::Clamp(LateralN * Dt, -FMath::Abs(VLateral) * MassKg, FMath::Abs(VLateral) * MassKg);
		VelocityMps += Lateral * (LateralImpulse / MassKg);
		LastLateralN = LateralImpulse / Dt;
		LastSlipAngleDeg = FMath::RadiansToDegrees(FMath::Atan(TanAlpha));

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
		StrokeLoadNs += Load * Dt;
		const float ShearSpeed = FMath::Max(FMath::Abs(SlipMps) - RoostSlipThresholdMps, 0.0f);
		StrokeSlipM += FMath::Min(ShearSpeed, RoostSlipCapMps) * Dt;
		StrokeSlipSign = SlipMps;
		StrokeSlipRatio = SlipRatio;
		// Sideways: the sliding share of the patch drags the layer the knobs are
		// in (knob height, or the sinkage where the knobs do not reach) sideways
		// at the slide speed. Volume rate = depth x patch length x slide speed,
		// the roost rule turned through ninety degrees. It leaves the line for
		// the outer shoulder at the stroke.
		// Signed by the slide direction: a wheel jiggling in a hole slides both
		// ways and shears nothing net; the integral of |v| said it dug a pit.
		const float SlideM = FMath::Clamp(VLateral, -RoostSlipCapMps, RoostSlipCapMps) * Dt;
		if (SlideM != 0.0f && LateralMaxN > 1.0f)
		{
			const float SlidingShare = FMath::Clamp(FMath::Abs(LateralImpulse / Dt) / LateralMaxN, 0.0f, 1.0f);
			const float ShearDepthCm = FMath::Min(KnobHeightCm, SinkageM * CmPerM * (1.0f + SlipSinkage * FMath::Abs(SlipRatio)));
			StrokeShoveCm3 += ShearDepthCm * ContactLengthM * CmPerM * SlideM * CmPerM * SlidingShare * (1.0f - Floating);
		}
		StrokeSideSlipM += SlideM;
		StrokeTimeS += Dt;

		// One batch of strokes every sixth of a radius travelled, or every 1/20 s
		// when spinning on the spot.
		if (bDeformsDirt && (StrokeDistanceM >= RadiusM / 6.0f || ((StrokeSlipM > 0.0f || FMath::Abs(StrokeShoveCm3) > 0.5f) && StrokeTimeS >= 0.05f)))
		{
			const FVector2D ContactCm = ContactXYCm;
			const float HalfWidthCm = ContactWidth * 0.5f * CmPerM;
			// The load the ground felt over the stroke, time-averaged with the
			// airborne substeps in: a wheel hopping in and out of its contact
			// tolerance lands at several g and flies at none, and it is the mean
			// that presses and packs. Read at the instant, a hop's landing pressed
			// the rut many times too deep and the tyre stalled in its own trench.
			const float MeanLoad = (StrokeLoadTimeS > 0.0f) ? StrokeLoadNs / StrokeLoadTimeS : Load;
			const float LoadFactor = MeanLoad / (MassKg * Gravity);

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
				// Bekker at the mean load already says how much deeper a heavier
				// wheel sinks; the load factor is not applied to the rut again.
				float MeanSinkM = 0.0f, UnusedLen = 0.0f, UnusedWid = 0.0f, UnusedRes = 0.0f;
				SoilResponse(Soil, Compaction, Moisture, MeanLoad, MeanSinkM, UnusedLen, UnusedWid, UnusedRes);
				const float MeanSinkageCm = MeanSinkM * (1.0f + SlipSinkage * FMath::Abs(StrokeSlipRatio)) * CmPerM;
				const float RutCm = PlasticSinkage * MeanSinkageCm * PassFraction;
				// The ground is pressed where the tyre first meets it, at the front
				// of the contact patch, so the axle always rides on floor it has
				// already made. Pressed under the axle instead, the unpressed ground
				// ahead was a step the tyre had to climb every stroke, on top of
				// the Bekker resistance that already charges for pressing it: a
				// wheel at quarter throttle dug itself in and never got going.
				const FVector2D PressCm = ContactCm + FVector2D(FwdGround.X, FwdGround.Y) * FMath::Sign(VForward) * (0.5f * ContactLengthM * CmPerM);
				// Laid as a strip from where the last stroke ended to here, one dab
				// per texel of travel with one texel's worth of pass in each, so the
				// dabs sum to a level trough. One dab per stroke, every sixth of a
				// radius, made a rut of overlapping dimples and a wheel that hopped
				// on them.
				const float TexelCm = B->GetTexelSizeCm();
				const FVector2D StripFrom = (bHaveLastPress && FVector2D::Distance(LastPressCm, PressCm) < RadiusM * CmPerM) ? LastPressCm : PressCm;
				const float StripCm = static_cast<float>(FVector2D::Distance(StripFrom, PressCm));
				const int32 Dabs = FMath::Max(1, FMath::CeilToInt(StripCm / TexelCm));
				const float DabPass = PassFraction / Dabs;
				for (int32 D = 0; D < Dabs; ++D)
				{
					const FVector2D At = FMath::Lerp(StripFrom, PressCm, (Dabs == 1) ? 1.0 : (static_cast<double>(D + 1) / Dabs));
					if (RutCm > 0.005f && bPartRut)
					{
						// No disturb: a tyre pressing a rut is packing, not breaking up.
						B->ApplyBrush(At, HalfWidthCm * 1.2f, RutCm / Dabs, EDirtBrushMode::Dig, 0.0f);
					}
					if (bPartPack)
					{
						B->ApplyBrush(At, HalfWidthCm * 1.3f, PackPerPass * DabPass * LoadFactor,
									  EDirtBrushMode::Pack, -1.0f, /*bProctor*/ true);
					}
				}
				LastPressCm = PressCm;
				bHaveLastPress = true;

				// The heap the tyre sank through is shoved ahead of it: the swept
				// volume (width x thickness sunk through x distance) moves from under
				// the contact to just in front, where it piles up and, next stroke,
				// stands higher, carries more, and pushes back harder.
				if (bPartPlough && StrokePloughM2 > 0.0f)
				{
					// Taken from the wedge just ahead of the contact, put down a
					// tyre radius further on: the blade of a bulldozer.
					const float ScoopRadiusCm = HalfWidthCm * 1.2f;
					const float SolidFraction = DirtSolidFraction(Compaction, Soil);
					const float WantedCm3 = ContactWidth * CmPerM * StrokePloughM2 * CmPerM * CmPerM * SolidFraction;
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
				const float SolidFraction = DirtSolidFraction(Compaction, Soil);
				const float WantedCm3 = ContactWidth * CmPerM * FailureDepthCm * SolidFraction * StrokeSlipM * CmPerM * LoadFactor;
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
								 FMath::RoundToInt(B->Settings.DustPerLitre * Soil.Dustiness * VolumeCm3 / 1000.0f * (1.0f - Moisture)), Moisture);
					RoostLitresTotal += VolumeCm3 / 1000.0f;
				}
			}

			// The layer the sliding patch sheared sideways leaves the line. At a
			// walking slide it is put down just outside the tyre's outer edge, the
			// shoulder that becomes the berm; sliding fast, the flank flings a
			// growing share of it outward, low and fast, as spray.
			if (FMath::Abs(StrokeShoveCm3) > 0.5f && (bPartSpray || bPartRut))
			{
				const float ScoopRadiusCm = HalfWidthCm * 1.1f;
				const float SolidFraction = DirtSolidFraction(Compaction, Soil);
				const float TotalCm3 = FMath::Min(FMath::Abs(StrokeShoveCm3) * SolidFraction, B->MaxScoopCm3(State.R, ScoopRadiusCm));
				const float MeanSlideMps = (StrokeTimeS > 0.0f) ? FMath::Abs(StrokeSideSlipM) / StrokeTimeS : 0.0f;
				const float FlungShare = 0.8f * FMath::Clamp((MeanSlideMps - SpraySlipThresholdMps) / FMath::Max(SprayFullSlideMps - SpraySlipThresholdMps, 0.1f), 0.0f, 1.0f);
				const float OutSign = (StrokeShoveCm3 >= 0.0f) ? 1.0f : -1.0f;     // the way the tyre slid
				const FVector2D Out2(Lateral.X * OutSign, Lateral.Y * OutSign);

				const float ShoulderCm3 = TotalCm3 * (1.0f - FlungShare);
				if (ShoulderCm3 > 0.5f && bPartRut)
				{
					// Dragged out from under the patch and left against the tyre's
					// outer flank. What a knob drags arrives loose (a dump always
					// does); what stays under the tyre is the pressed line, not
					// broken up: the knobs took its top, the load packed the rest.
					// As a strip along the stroke's travel, like the rut: a row of
					// heaps was a row of bumps for the next lap.
					const float DumpRadiusCm = HalfWidthCm * 0.9f;
					const float TexelCm = B->GetTexelSizeCm();
					const FVector2D Back2 = FVector2D(FwdGround.X, FwdGround.Y) * (-FMath::Sign(VForward));
					const float StripCm = FMath::Min(StrokeDistanceM * CmPerM, RadiusM * CmPerM);
					const int32 Dabs = FMath::Max(1, FMath::CeilToInt(StripCm / TexelCm));
					for (int32 D = 0; D < Dabs; ++D)
					{
						const FVector2D Along = Back2 * (StripCm * static_cast<float>(D) / Dabs);
						const FVector2D ShoulderCm = ContactCm + Along + Out2 * (HalfWidthCm * 1.5f + DumpRadiusCm * 0.6f);
						B->TransferDirt(ContactCm + Along, ScoopRadiusCm, ShoulderCm, DumpRadiusCm, ShoulderCm3 / Dabs, 0.0f);
					}
					ShovedLitresTotal += ShoulderCm3 / 1000.0f;
				}

				const float FlungCm3 = TotalCm3 * FlungShare;
				if (FlungCm3 > 0.5f && bPartSpray)
				{
					const float EjectMps = FMath::Min(MeanSlideMps, 12.0f) * 0.9f;
					const FVector Dir = (Lateral * OutSign * 0.9f + Normal * 0.35f).GetSafeNormal();
					const FVector LaunchCm = FVector(ContactCm.X, ContactCm.Y, GroundCm) + Lateral * OutSign * HalfWidthCm + FVector(0, 0, 3.0f);
					const int32 Link = B->ScoopDirt(ContactCm, ScoopRadiusCm, FlungCm3, 0.1f);
					B->SpawnParcels(LaunchCm, Dir * EjectMps * CmPerM + VelocityMps * CmPerM * 0.7f, 18.0f, FlungCm3, Moisture, Compaction, Link);
					B->SpawnDust(LaunchCm, Dir * EjectMps * CmPerM * 0.5f + VelocityMps * CmPerM * 0.7f, 35.0f,
								 FMath::RoundToInt(B->Settings.DustPerLitre * Soil.Dustiness * FlungCm3 / 1000.0f * (1.0f - Moisture)), Moisture);
					RoostLitresTotal += FlungCm3 / 1000.0f;
				}
			}

			StrokeDistanceM = 0.0f;
			StrokeLoadNs = 0.0f;
			StrokeLoadTimeS = 0.0f;
			StrokeSlipM = 0.0f;
			StrokeSideSlipM = 0.0f;
			StrokeShoveCm3 = 0.0f;
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
