#include "DirtTrack.h"

namespace
{
	/** 36 control points, metres about the site centre. Solved offline so the lap
	 *  comes out at 1,520 m (FIM 047.3.1 wants 1.5-1.75 km), the tightest corner is
	 *  17 m radius, and no two passes of the track come within 36 m of each other
	 *  (FIM 047.3.8 asks for ~10 m between contiguous tracks; 8 m of track width
	 *  plus that is 18 m, so this has double the margin). */
	const FVector2f GTrackControlPoints[] =
	{
		FVector2f(-124.83f, -110.81f), FVector2f( -73.63f, -112.94f), FVector2f( -20.30f, -109.74f),
		FVector2f(  33.04f, -106.54f), FVector2f(  86.37f, -103.34f), FVector2f( 131.17f,  -95.88f),
		FVector2f( 158.90f,  -78.81f), FVector2f( 165.30f,  -55.34f), FVector2f( 156.77f,  -38.28f),
		FVector2f( 126.90f,  -29.74f), FVector2f(  75.70f,  -36.14f), FVector2f(  18.10f,  -27.61f),
		FVector2f( -39.50f,  -35.08f), FVector2f( -97.10f,  -27.61f), FVector2f(-122.70f,  -12.68f),
		FVector2f(-126.96f,    8.66f), FVector2f(-114.16f,   25.72f), FVector2f( -75.76f,   29.99f),
		FVector2f( -18.16f,   24.66f), FVector2f(  39.44f,   33.19f), FVector2f(  94.90f,   26.79f),
		FVector2f( 137.57f,   34.26f), FVector2f( 161.04f,   55.59f), FVector2f( 163.17f,   81.19f),
		FVector2f( 146.10f,   98.26f), FVector2f( 105.57f,  105.72f), FVector2f(  50.10f,  113.19f),
		FVector2f(  -7.50f,  106.79f), FVector2f( -62.96f,  112.12f), FVector2f(-112.03f,  104.66f),
		FVector2f(-146.16f,   87.59f), FVector2f(-161.10f,   53.46f), FVector2f(-165.36f,   15.06f),
		FVector2f(-161.10f,  -25.48f), FVector2f(-156.83f,  -63.88f), FVector2f(-144.03f,  -93.74f),
	};

	// --- track cross-section, all metres -----------------------------------
	constexpr float TrackHalfWidthM   = 4.0f;    // FIM 047.3.2: 8 m recommended riding width
	constexpr float NeutralZoneM      = 1.0f;    // FIM 047.3.5: >= 1 m each side
	constexpr float NeutralBankM      = 0.5f;    // FIM 047.4: earth banking approx 50 cm
	constexpr float NeutralBankWidthM = 2.0f;    // how wide that bank is before it falls away
	constexpr float MaxBermHeightM    = 1.1f;    // real berms run 0.3-1.2 m
	constexpr float BermRunM          = 1.7f;    // height ends up ~1/3 of berm width,
												 // the proportion builders aim for
	constexpr float BermBackM         = 2.5f;    // outer back-slope, where the pile ends
	constexpr float CorridorHalfM     = 5.5f;    // graded width: 8 m of track plus shoulders
	constexpr float GradeRadiusM      = 14.0f;   // grading blends back into natural ground by here
	constexpr float FineRadiusM       = 26.0f;   // how far out we track (s,t) exactly
	constexpr float SampleStepM       = 0.35f;   // centre-line sampling
	constexpr float StampStepM        = 2.0f;    // segment spacing for the distance field

	float TanDeg(float Deg)
	{
		return FMath::Tan(FMath::DegreesToRadians(FMath::Clamp(Deg, 1.0f, 89.0f)));
	}

	/** Shortest distance from P to segment AB, and where along it the foot lands. */
	float DistToSegment(const FVector2f& P, const FVector2f& A, const FVector2f& B, float& OutAlpha)
	{
		const FVector2f AB = B - A;
		const float LenSq = AB.SizeSquared();
		OutAlpha = (LenSq > SMALL_NUMBER) ? FMath::Clamp(FVector2f::DotProduct(P - A, AB) / LenSq, 0.0f, 1.0f) : 0.0f;
		return (P - (A + AB * OutAlpha)).Size();
	}
}

FDirtTrack::FDirtTrack(const FDirtSimSettings& InSettings)
	: Settings(InSettings)
	, Resolution(FMath::Max(InSettings.SimResolution, 1))
	, RegionCentreM(static_cast<float>(InSettings.SimRegionCentreCm.X) * 0.01f,
					static_cast<float>(InSettings.SimRegionCentreCm.Y) * 0.01f)
	, HalfRegionM(InSettings.RegionSizeCm() * 0.005f)
	, MetresPerCell(InSettings.TexelSizeCm() * 0.01f)
{
}

FVector2f FDirtTrack::CellToMetres(int32 X, int32 Y) const
{
	return RegionCentreM + FVector2f(
		-HalfRegionM + (static_cast<float>(X) + 0.5f) * MetresPerCell,
		-HalfRegionM + (static_cast<float>(Y) + 0.5f) * MetresPerCell);
}

FIntPoint FDirtTrack::MetresToCell(const FVector2f& M) const
{
	return FIntPoint(
		FMath::FloorToInt((M.X - RegionCentreM.X + HalfRegionM) / MetresPerCell),
		FMath::FloorToInt((M.Y - RegionCentreM.Y + HalfRegionM) / MetresPerCell));
}

// ---------------------------------------------------------------------------
// Centre line
// ---------------------------------------------------------------------------

void FDirtTrack::BuildCentreline()
{
	const int32 NumCtrl = UE_ARRAY_COUNT(GTrackControlPoints);
	constexpr int32 PerSegment = 128;

	TArray<FVector2f> Pts;
	Pts.Reserve(NumCtrl * PerSegment);

	for (int32 i = 0; i < NumCtrl; ++i)
	{
		const FVector2f& P0 = GTrackControlPoints[(i - 1 + NumCtrl) % NumCtrl];
		const FVector2f& P1 = GTrackControlPoints[i];
		const FVector2f& P2 = GTrackControlPoints[(i + 1) % NumCtrl];
		const FVector2f& P3 = GTrackControlPoints[(i + 2) % NumCtrl];

		for (int32 k = 0; k < PerSegment; ++k)
		{
			const float T = static_cast<float>(k) / PerSegment;
			const float T2 = T * T;
			const float T3 = T2 * T;

			// Uniform Catmull-Rom.
			const FVector2f V = (P1 * 2.0f)
							  + (P2 - P0) * T
							  + (P0 * 2.0f - P1 * 5.0f + P2 * 4.0f - P3) * T2
							  + (P3 - P0 + P1 * 3.0f - P2 * 3.0f) * T3;
			Pts.Add(V * 0.5f);
		}
	}

	// Arc length, tangents and normals.
	const int32 N = Pts.Num();
	Centreline.SetNum(N);

	float S = 0.0f;
	for (int32 i = 0; i < N; ++i)
	{
		Centreline[i].Pos = Pts[i];
		Centreline[i].S = S;
		S += (Pts[(i + 1) % N] - Pts[i]).Size();
	}
	LapLengthM = S;

	// Curvature over a ~4 m stencil, so it reads the corner rather than the noise.
	const int32 Stride = FMath::Max(1, FMath::RoundToInt(4.0f / FMath::Max(LapLengthM / N, KINDA_SMALL_NUMBER)));
	MinCornerRadiusM = MAX_flt;

	for (int32 i = 0; i < N; ++i)
	{
		const FVector2f& A = Pts[(i - Stride + N) % N];
		const FVector2f& B = Pts[i];
		const FVector2f& C = Pts[(i + Stride) % N];

		const FVector2f D1 = (C - A) * 0.5f;              // first derivative
		const FVector2f D2 = C - (B * 2.0f) + A;           // second derivative

		const float Speed = D1.Size();
		Centreline[i].Normal = (Speed > SMALL_NUMBER)
			? FVector2f(-D1.Y / Speed, D1.X / Speed)      // left of travel
			: FVector2f(0.0f, 1.0f);

		const float Cross = D1.X * D2.Y - D1.Y * D2.X;
		Centreline[i].Curvature = (Speed > SMALL_NUMBER) ? Cross / (Speed * Speed * Speed) : 0.0f;

		const float AbsK = FMath::Abs(Centreline[i].Curvature);
		if (AbsK > KINDA_SMALL_NUMBER)
		{
			MinCornerRadiusM = FMath::Min(MinCornerRadiusM, 1.0f / AbsK);
		}
	}

	StartPositionM = Centreline[0].Pos;
	const FVector2f Dir = (Centreline[8].Pos - Centreline[0].Pos).GetSafeNormal();
	StartHeadingRad = FMath::Atan2(Dir.Y, Dir.X);

	Log(FString::Printf(TEXT("Centre line: %.0f m lap (FIM 1500-1750), tightest corner %.0f m radius"),
		LapLengthM, MinCornerRadiusM));
}

float FDirtTrack::NaturalGroundM(const FVector2f& P)
{
	// The site as it was before anyone brought a dozer. Purely a function of where
	// you are standing — it has no idea a track is coming, which is the point.
	return 9.0f * FMath::Sin(P.X / 95.0f + 0.4f) * FMath::Cos(P.Y / 110.0f - 0.9f)
		 + 5.5f * FMath::Sin(P.X / 47.0f - 1.3f) * FMath::Cos(P.Y /  38.0f + 2.1f)
		 + 2.5f * FMath::Sin(P.X / 23.0f + 2.7f) * FMath::Cos(P.Y /  27.0f - 0.5f)
		 + 1.2f * FMath::Sin(P.X / 11.0f - 0.2f) * FMath::Cos(P.Y /  13.0f + 1.7f);
}

void FDirtTrack::BuildDesignProfile()
{
	const int32 N = Centreline.Num();
	const float StepM = LapLengthM / N;

	// Start from the ground that is actually there.
	TArray<float> Raw;
	Raw.SetNumUninitialized(N);
	for (int32 i = 0; i < N; ++i)
	{
		Raw[i] = NaturalGroundM(Centreline[i].Pos);
	}

	float RawMin = MAX_flt, RawMax = -MAX_flt;
	for (float V : Raw) { RawMin = FMath::Min(RawMin, V); RawMax = FMath::Max(RawMax, V); }

	// Smooth it until it is rideable. A centred moving average keeps the mean, so
	// the cut and fill this implies will balance.
	const auto Smooth = [N, StepM](TArray<float>& Profile, float WindowM, int32 Passes)
	{
		const int32 W = FMath::Max(1, FMath::FloorToInt(WindowM / FMath::Max(StepM, KINDA_SMALL_NUMBER)));
		TArray<float> Next;
		Next.SetNumUninitialized(N);
		for (int32 Pass = 0; Pass < Passes; ++Pass)
		{
			for (int32 i = 0; i < N; ++i)
			{
				float Acc = 0.0f;
				for (int32 k = -W; k <= W; ++k)
				{
					Acc += Profile[(i + k + N * 2) % N];
				}
				Next[i] = Acc / (2 * W + 1);
			}
			Profile = Next;
		}
	};

	DesignM = Raw;
	Smooth(DesignM, 32.0f, 2);

	// Clamp to a gradient a bike can climb. Pushing the excess equally onto both
	// samples keeps the total unchanged, so the cut/fill balance survives this too.
	constexpr float MaxGradient = 0.22f;
	const float Limit = MaxGradient * StepM;
	for (int32 Iter = 0; Iter < 600; ++Iter)
	{
		bool bChanged = false;
		for (int32 i = 0; i < N; ++i)
		{
			const int32 j = (i + 1) % N;
			const float D = DesignM[j] - DesignM[i];
			if (FMath::Abs(D) > Limit)
			{
				const float Fix = (FMath::Abs(D) - Limit) * 0.5f * FMath::Sign(D);
				DesignM[j] -= Fix;
				DesignM[i] += Fix;
				bChanged = true;
			}
		}
		if (!bChanged) { break; }
	}
	Smooth(DesignM, 14.0f, 1);

	float DMin = MAX_flt, DMax = -MAX_flt, MaxGrad = 0.0f;
	for (int32 i = 0; i < N; ++i)
	{
		DMin = FMath::Min(DMin, DesignM[i]);
		DMax = FMath::Max(DMax, DesignM[i]);
		MaxGrad = FMath::Max(MaxGrad, FMath::Abs(DesignM[(i + 1) % N] - DesignM[i]) / StepM);
	}

	MinElevationM = DMin;
	MaxElevationM = DMax;

	Log(FString::Printf(TEXT("Site: natural ground %+.1f to %+.1f m along the centre line"), RawMin, RawMax));
	Log(FString::Printf(TEXT("Grade: %+.1f to %+.1f m (%.1f m of change), steepest %.0f%% (MX climbs 20-30%%)"),
		DMin, DMax, DMax - DMin, MaxGrad * 100.0f));
}

float FDirtTrack::DesignAtS(float S) const
{
	const int32 N = DesignM.Num();
	if (N == 0)
	{
		return 0.0f;
	}

	const float F = FMath::Fmod(FMath::Fmod(S, LapLengthM) + LapLengthM, LapLengthM) / LapLengthM * N;
	const int32 I0 = FMath::Clamp(FMath::FloorToInt(F), 0, N - 1);
	const int32 I1 = (I0 + 1) % N;
	return FMath::Lerp(DesignM[I0], DesignM[I1], F - I0);
}

// ---------------------------------------------------------------------------
// Features
// ---------------------------------------------------------------------------

void FDirtTrack::BuildFeatureList()
{
	Features.Reset();

	// Placed offline against the centre line's curvature, not by eye. Every jump
	// sits where the track is straight enough for it: tabletops, doubles, the
	// triple and the step-up/down all need at least an 80 m radius under them,
	// because a take-off built inside a hairpin is nonsense. Rolling waves tolerate
	// a 45 m radius, and sand goes anywhere.
	//
	// FIM 047.5.2: the start straight carries no jumps — the first obstacle is at
	// s=190 m, well past the 125 m the rule cares about.
	const auto Add = [this](EDirtTrackFeature Type, float StartS, float LengthM, float HeightM,
							float FaceDeg, float LipRoundM, float GapM, const TCHAR* Name)
	{
		FDirtTrackFeature F;
		F.Type = Type; F.StartS = StartS; F.LengthM = LengthM; F.HeightM = HeightM;
		F.FaceAngleDeg = FaceDeg; F.LipRoundM = LipRoundM; F.GapM = GapM; F.Name = Name;
		Features.Add(F);
	};

	Add(EDirtTrackFeature::Tabletop,      190.0f, 34.0f, 2.20f, 24.0f, 1.6f,  0.0f, TEXT("tabletop after turn 1"));
	Add(EDirtTrackFeature::Double,        374.0f, 30.0f, 2.00f, 34.0f, 0.5f,  9.0f, TEXT("first double"));
	Add(EDirtTrackFeature::RollingWaves,  422.0f, 42.0f, 0.80f,  0.0f, 0.0f, 10.0f, TEXT("rolling waves (FIM 047.3.7)"));
	Add(EDirtTrackFeature::StepUp,        482.0f, 30.0f, 2.60f, 45.0f, 0.0f,  0.0f, TEXT("step-up"));
	Add(EDirtTrackFeature::Triple,        534.0f, 48.0f, 2.60f, 38.0f, 0.3f, 10.5f, TEXT("triple, 21 m of gap"));
	Add(EDirtTrackFeature::Tabletop,      680.0f, 30.0f, 1.80f, 28.0f, 0.8f,  0.0f, TEXT("second tabletop"));
	Add(EDirtTrackFeature::Double,        728.0f, 28.0f, 1.90f, 40.0f, 0.2f,  8.5f, TEXT("steep-faced double"));
	Add(EDirtTrackFeature::RollingWaves,  774.0f, 52.0f, 0.75f,  0.0f, 0.0f, 10.0f, TEXT("long rolling waves"));
	Add(EDirtTrackFeature::StepDown,      844.0f, 26.0f, 2.20f, 30.0f, 0.6f,  0.0f, TEXT("step-down"));
	Add(EDirtTrackFeature::Double,       1016.0f, 32.0f, 2.40f, 32.0f, 0.5f, 10.0f, TEXT("uphill double"));
	Add(EDirtTrackFeature::SandSection,  1066.0f, 60.0f, 0.00f,  0.0f, 0.0f,  0.0f, TEXT("deep sand"));
	Add(EDirtTrackFeature::Tabletop,     1144.0f, 32.0f, 2.00f, 26.0f, 1.2f,  0.0f, TEXT("third tabletop"));
	Add(EDirtTrackFeature::Double,       1194.0f, 30.0f, 2.10f, 36.0f, 0.4f,  9.5f, TEXT("rhythm double"));
	Add(EDirtTrackFeature::RollingWaves, 1242.0f, 32.0f, 0.70f,  0.0f, 0.0f, 10.0f, TEXT("braking waves"));
	Add(EDirtTrackFeature::Tabletop,     1324.0f, 34.0f, 2.30f, 25.0f, 1.4f,  0.0f, TEXT("finish-line tabletop"));

	for (const FDirtTrackFeature& F : Features)
	{
		Log(FString::Printf(TEXT("  s=%.0f m  %s"), F.StartS, F.Name));
	}
}

float FDirtTrack::FeatureProfile(const FDirtTrackFeature& F, float IntoS, float LateralM,
								 FDirtMaterial& OutMat, bool& bOutOverridesMaterial) const
{
	bOutOverridesMaterial = false;

	if (IntoS < 0.0f || IntoS > F.LengthM)
	{
		return 0.0f;
	}

	// Sand changes the soil, not the shape.
	if (F.Type == EDirtTrackFeature::SandSection)
	{
		if (FMath::Abs(LateralM) <= TrackHalfWidthM + 1.0f)
		{
			bOutOverridesMaterial = true;
			OutMat.Compaction = 0.05f;      // sand never packs
			OutMat.Moisture = 0.10f;
			OutMat.LayerCm = 90.0f;         // deep enough to swallow a wheel
		}
		return 0.0f;
	}

	// Everything else is built dirt: packed, and it fades out at the track edges
	// so an obstacle never leaves a vertical wall beside the racing line.
	// FIM 048.2.3 wants the landing wider than the take-off, so jumps get an extra
	// metre of width on their landing half.
	const float EdgeM = TrackHalfWidthM + ((IntoS > F.LengthM * 0.5f) ? 1.0f : 0.0f);
	const float Fade = 1.0f - FMath::SmoothStep(EdgeM - 0.5f, EdgeM + 2.0f, FMath::Abs(LateralM));
	if (Fade <= 0.0f)
	{
		return 0.0f;
	}

	const float H = F.HeightM;
	float Z = 0.0f;

	switch (F.Type)
	{
	case EDirtTrackFeature::RollingWaves:
	case EDirtTrackFeature::Whoops:
	{
		// A cosine washboard. FIM 047.3.7 caps outdoor waves at ~80 cm with about
		// 10 m between peaks; Supercross whoops are ~90 cm at ~4.3 m.
		const float Spacing = FMath::Max(F.GapM, 1.0f);
		const int32 Waves = FMath::Max(1, FMath::FloorToInt(F.LengthM / Spacing));
		if (IntoS > Waves * Spacing)
		{
			return 0.0f;
		}
		Z = H * 0.5f * (1.0f - FMath::Cos(2.0f * PI * IntoS / Spacing));
		break;
	}

	case EDirtTrackFeature::Tabletop:
	{
		const float UpRun = H / TanDeg(F.FaceAngleDeg);
		const float DownRun = H / TanDeg(20.0f);
		const float Table = FMath::Max(F.LengthM - UpRun - DownRun, 2.0f);

		if (IntoS <= UpRun)
		{
			Z = IntoS * TanDeg(F.FaceAngleDeg);
			if (F.LipRoundM > 0.0f && IntoS > UpRun - F.LipRoundM)
			{
				const float T = (IntoS - (UpRun - F.LipRoundM)) / F.LipRoundM;
				Z -= 0.5f * F.LipRoundM * TanDeg(F.FaceAngleDeg) * T * T;
			}
		}
		else if (IntoS <= UpRun + Table)
		{
			Z = H;
		}
		else
		{
			Z = FMath::Max(0.0f, H - (IntoS - UpRun - Table) * TanDeg(20.0f));
		}
		break;
	}

	case EDirtTrackFeature::Double:
	case EDirtTrackFeature::Triple:
	{
		// Take-off, gap, (mound, gap for a triple), landing. The take-off has a
		// steep 45 deg back; the landing has a steep face toward the gap and a long
		// shallow runout, which is what riders actually come down on.
		const float UpRun = H / TanDeg(F.FaceAngleDeg);
		const float BackRun = H / TanDeg(45.0f);
		const float LandFace = H / TanDeg(38.0f);
		const float LandOut = H / TanDeg(14.0f);

		const auto Wedge = [](float D, float FrontRun, float BackRunIn, float Peak, float Lip) -> float
		{
			if (D < 0.0f || D > FrontRun + BackRunIn) return 0.0f;
			if (D <= FrontRun)
			{
				float V = Peak * (D / FMath::Max(FrontRun, KINDA_SMALL_NUMBER));
				if (Lip > 0.0f && D > FrontRun - Lip)
				{
					const float T = (D - (FrontRun - Lip)) / Lip;
					V -= 0.5f * Lip * (Peak / FMath::Max(FrontRun, KINDA_SMALL_NUMBER)) * T * T;
				}
				return V;
			}
			return Peak * (1.0f - (D - FrontRun) / FMath::Max(BackRunIn, KINDA_SMALL_NUMBER));
		};

		Z = Wedge(IntoS, UpRun, BackRun, H, F.LipRoundM);

		float Cursor = UpRun + BackRun + F.GapM;
		if (F.Type == EDirtTrackFeature::Triple)
		{
			// The middle mound is what makes it a triple rather than a long double.
			const float MidH = H * 0.45f;
			Z = FMath::Max(Z, Wedge(IntoS - Cursor, MidH / TanDeg(35.0f), MidH / TanDeg(35.0f), MidH, 0.0f));
			Cursor += 2.0f * MidH / TanDeg(35.0f) + F.GapM;
		}
		Z = FMath::Max(Z, Wedge(IntoS - Cursor, LandFace, LandOut, H, 0.0f));
		break;
	}

	case EDirtTrackFeature::StepUp:
	{
		// Steep face onto a plateau, then back down over the rest of the feature.
		const float UpRun = H / TanDeg(F.FaceAngleDeg);
		const float Plateau = FMath::Max(F.LengthM * 0.35f, 4.0f);
		if (IntoS <= UpRun)            Z = IntoS * TanDeg(F.FaceAngleDeg);
		else if (IntoS <= UpRun + Plateau) Z = H;
		else                           Z = FMath::Max(0.0f, H * (1.0f - (IntoS - UpRun - Plateau)
										   / FMath::Max(F.LengthM - UpRun - Plateau, 1.0f)));
		break;
	}

	case EDirtTrackFeature::StepDown:
	{
		// The mirror image: climb gently, then drop off an edge.
		const float DropAt = F.LengthM * 0.55f;
		if (IntoS <= DropAt)
		{
			Z = H * FMath::SmoothStep(0.0f, DropAt, IntoS);
		}
		else
		{
			Z = FMath::Max(0.0f, H - (IntoS - DropAt) * TanDeg(F.FaceAngleDeg));
		}
		break;
	}

	default:
		break;
	}

	if (Z > 0.01f)
	{
		bOutOverridesMaterial = true;
		OutMat.Compaction = 0.85f;      // built and packed, so the shape holds
		OutMat.Moisture = 0.40f;
		OutMat.LayerCm = 25.0f;
	}

	return Z * Fade;
}

float FDirtTrack::CorridorProfile(float LateralM, float CurvatureAtS, FDirtMaterial& OutMat) const
{
	// Returns only the dirt PLACED on top of the graded corridor: the berm and the
	// neutral-zone banking. The ground falling away beyond that is the natural site,
	// which is already there and costs nobody anything to build.
	const float AbsT = FMath::Abs(LateralM);
	float Placed = 0.0f;

	// --- berm, generated from the corner it belongs to ----------------------
	// Berms form on the OUTSIDE of a corner, and the tighter the corner the bigger
	// they get. Curvature 0.0125 is an 80 m sweeper (no berm); 0.05 is a 20 m
	// hairpin (full berm). Real berms run 0.3-1.2 m tall.
	const float AbsK = FMath::Abs(CurvatureAtS);
	const float BermH = MaxBermHeightM * FMath::SmoothStep(0.0125f, 0.05f, AbsK);

	if (BermH > 0.01f)
	{
		const float OutwardSign = (CurvatureAtS > 0.0f) ? -1.0f : 1.0f;
		const float Outward = LateralM * OutwardSign;
		const float BermStart = TrackHalfWidthM - BermRunM;

		if (Outward > BermStart)
		{
			const float X = FMath::Min((Outward - BermStart) / BermRunM, 1.0f);

			// x^2.5 means the face steepens as it rises — near vertical at the top,
			// which is how a berm you can actually rail is shaped.
			float Shape = FMath::Pow(X, 2.5f);

			// And then it has to come back down. A berm is a ridge of piled dirt,
			// not a terrace: without this the crest height would carry on outward
			// to the fine radius and bury the whole verge on every corner.
			const float Crest = BermStart + BermRunM;
			if (Outward > Crest)
			{
				Shape *= 1.0f - FMath::SmoothStep(Crest, Crest + BermBackM, Outward);
			}

			Placed += BermH * Shape;
		}
	}

	if (AbsT <= TrackHalfWidthM)
	{
		// The racing line: ripped and watered, ready to be ridden.
		OutMat.Compaction = 0.45f;
		OutMat.Moisture = 0.35f;
		OutMat.LayerCm = Settings.RestLayerCm;
		return Placed;
	}

	OutMat.Compaction = 0.7f;
	OutMat.Moisture = 0.3f;
	OutMat.LayerCm = 20.0f;

	// FIM 047.4: earth banking about 50 cm high marks the neutral zone. It is a
	// BANK - a low ridge a couple of metres wide - not a plateau. Ramping up with
	// a smoothstep and leaving it there would lay half a metre of dirt across
	// everything out to the fine radius, which at 1.5 km of lap is tens of
	// thousands of cubic metres of earth that nobody ever moved.
	const float Over = AbsT - TrackHalfWidthM;
	if (Over > NeutralZoneM && Over < NeutralZoneM + NeutralBankWidthM)
	{
		const float X = (Over - NeutralZoneM) / NeutralBankWidthM;
		const float Bump = FMath::Sin(PI * X);
		Placed += NeutralBankM * Bump * Bump;      // rises and falls back to ground
	}

	return Placed;
}

// ---------------------------------------------------------------------------
// Stamping
// ---------------------------------------------------------------------------

void FDirtTrack::StampTrack()
{
	const int32 Count = Resolution * Resolution;
	CellS.Init(0.0f, Count);
	CellT.Init(0.0f, Count);
	CellCurvature.Init(0.0f, Count);
	CellDist.Init(MAX_flt, Count);

	const int32 N = Centreline.Num();
	const float Spacing = LapLengthM / N;
	const int32 Step = FMath::Max(1, FMath::RoundToInt(StampStepM / FMath::Max(Spacing, KINDA_SMALL_NUMBER)));

	// Exact (s, t) within FineRadiusM of the centre line. That covers the track,
	// its berms, the neutral zone and the first few metres of ground beyond.
	for (int32 i = 0; i < N; i += Step)
	{
		const FSample& A = Centreline[i];
		const FSample& B = Centreline[(i + Step) % N];

		const float MinX = FMath::Min(A.Pos.X, B.Pos.X) - FineRadiusM;
		const float MaxX = FMath::Max(A.Pos.X, B.Pos.X) + FineRadiusM;
		const float MinY = FMath::Min(A.Pos.Y, B.Pos.Y) - FineRadiusM;
		const float MaxY = FMath::Max(A.Pos.Y, B.Pos.Y) + FineRadiusM;

		const FIntPoint Lo = MetresToCell(FVector2f(MinX, MinY));
		const FIntPoint Hi = MetresToCell(FVector2f(MaxX, MaxY));

		// A segment far outside the region contributes nothing — skip it rather
		// than clamping it onto the region's edge, which would smear the whole
		// track onto one row of cells whenever the sim is focused somewhere else.
		if (Hi.X < 0 || Hi.Y < 0 || Lo.X >= Resolution || Lo.Y >= Resolution)
		{
			continue;
		}

		const int32 X0 = FMath::Clamp(Lo.X, 0, Resolution - 1);
		const int32 X1 = FMath::Clamp(Hi.X + 1, 0, Resolution - 1);
		const int32 Y0 = FMath::Clamp(Lo.Y, 0, Resolution - 1);
		const int32 Y1 = FMath::Clamp(Hi.Y + 1, 0, Resolution - 1);

		// Arc length of B, unwrapped so the seam at s=0 does not interpolate backwards.
		float SB = B.S;
		if (SB < A.S)
		{
			SB += LapLengthM;
		}

		for (int32 CY = Y0; CY <= Y1; ++CY)
		{
			for (int32 CX = X0; CX <= X1; ++CX)
			{
				const FVector2f P = CellToMetres(CX, CY);
				float Alpha;
				const float D = DistToSegment(P, A.Pos, B.Pos, Alpha);

				const int32 I = Index(CX, CY);
				if (D >= CellDist[I])
				{
					continue;
				}

				CellDist[I] = D;
				CellS[I] = FMath::Fmod(FMath::Lerp(A.S, SB, Alpha), LapLengthM);
				CellCurvature[I] = FMath::Lerp(A.Curvature, B.Curvature, Alpha);

				// Signed lateral offset: positive is left of the direction of travel.
				const FVector2f Foot = FMath::Lerp(A.Pos, B.Pos, Alpha);
				const FVector2f Nrm = FMath::Lerp(A.Normal, B.Normal, Alpha).GetSafeNormal();
				CellT[I] = FVector2f::DotProduct(P - Foot, Nrm);
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------

void FDirtTrack::GradeCorridor()
{
	const double CellAreaM2 = Settings.TexelAreaCm2() / 10000.0;

	for (int32 I = 0; I < SurfaceM.Num(); ++I)
	{
		if (CellDist[I] >= GradeRadiusM)
		{
			continue;
		}

		// Full grade on the racing line, blending back into untouched ground by
		// GradeRadiusM so there is never a step at the edge of the works.
		const float W = 1.0f - FMath::SmoothStep(CorridorHalfM, GradeRadiusM, CellDist[I]);
		const float Target = DesignAtS(CellS[I]);
		const float NewZ = FMath::Lerp(SurfaceM[I], Target, W);
		const float Delta = NewZ - SurfaceM[I];

		if (Delta < 0.0f) { CutM3 += -Delta * CellAreaM2; }
		else              { FillM3 +=  Delta * CellAreaM2; }

		SurfaceM[I] = NewZ;

		CellMaterial[I].Compaction = 0.6f;    // graded and rolled, not yet ripped
		CellMaterial[I].Moisture = 0.3f;
		CellMaterial[I].LayerCm = 25.0f;
	}

	Log(FString::Printf(TEXT("Graded %.0f m corridor: cut %.0f m3, fill %.0f m3, spoil %+.0f m3"),
		CorridorHalfM * 2.0f, CutM3, FillM3, CutM3 - FillM3));
}

void FDirtTrack::BuildFeaturesAndBerms()
{
	const double CellAreaM2 = Settings.TexelAreaCm2() / 10000.0;

	for (int32 I = 0; I < SurfaceM.Num(); ++I)
	{
		if (CellDist[I] >= FineRadiusM)
		{
			continue;
		}

		FDirtMaterial Mat = CellMaterial[I];
		float Placed = CorridorProfile(CellT[I], CellCurvature[I], Mat);

		for (const FDirtTrackFeature& F : Features)
		{
			// Features can straddle the s=0 seam, so test both windings.
			float Into = CellS[I] - F.StartS;
			if (Into < 0.0f) { Into += LapLengthM; }
			if (Into > F.LengthM) { continue; }

			FDirtMaterial FeatMat = Mat;
			bool bOverride = false;
			Placed += FeatureProfile(F, Into, CellT[I], FeatMat, bOverride);
			if (bOverride) { Mat = FeatMat; }
		}

		if (Placed > 0.0f)
		{
			SurfaceM[I] += Placed;
			BuiltM3 += Placed * CellAreaM2;
		}

		CellMaterial[I] = Mat;
	}

	Log(FString::Printf(TEXT("Built jumps and berms: %.0f m3 placed"), BuiltM3));
}

void FDirtTrack::DigBorrowPits()
{
	const double CellAreaM2 = Settings.TexelAreaCm2() / 10000.0;
	const double Spoil = CutM3 - FillM3;
	const double Deficit = BuiltM3 - Spoil;

	if (Deficit <= 0.0)
	{
		Log(FString::Printf(TEXT("Site balanced on its own: %.0f m3 of spoil left over"), -Deficit));
		return;
	}

	// The dirt in those jumps came out of the ground somewhere. These are the holes.
	// Well clear of the racing line and inside the site, in the outfield corners.
	const FVector2f PitCentres[] =
	{
		FVector2f(-160.0f,  160.0f),
		FVector2f( 160.0f,  160.0f),
		FVector2f( 160.0f, -160.0f),
	};
	const int32 NumPits = UE_ARRAY_COUNT(PitCentres);

	constexpr float PitDepthM = 2.4f;
	const double PerPit = Deficit / NumPits;

	// A cosine bowl of radius R and depth D — profile D/2 * (1 + cos(pi*d/R)) — holds
	//   V = integral 0..R of D/2 (1 + cos(pi d/R)) 2 pi d dd = pi R^2 D (1/2 - 2/pi^2)
	// which is about 0.297 * pi R^2 D, not the pi R^2 D / 2 of a straight-sided
	// bowl. Sizing with the wrong constant left the pits 40% short and the ledger
	// reporting imported dirt on a site that could have balanced.
	// Clamped so a pit can never run off the site or into the track; whatever the
	// pits cannot supply is reported honestly as imported rather than conjured.
	constexpr float MaxPitRadiusM = 30.0f;
	constexpr double CosineBowlFactor = 0.5 - 2.0 / (PI * PI);   // ~0.2974
	const float R = FMath::Min(
		FMath::Sqrt(static_cast<float>(PerPit / (CosineBowlFactor * PI * PitDepthM))), MaxPitRadiusM);

	for (const FVector2f& Centre : PitCentres)
	{
		const FIntPoint Lo = MetresToCell(Centre - FVector2f(R, R));
		const FIntPoint Hi = MetresToCell(Centre + FVector2f(R, R));
		if (Hi.X < 0 || Hi.Y < 0 || Lo.X >= Resolution || Lo.Y >= Resolution)
		{
			continue;   // this pit is not inside the region being simulated
		}

		FIntPoint MinC, MaxC;
		MinC.X = FMath::Clamp(Lo.X - 1, 0, Resolution - 1);
		MinC.Y = FMath::Clamp(Lo.Y - 1, 0, Resolution - 1);
		MaxC.X = FMath::Clamp(Hi.X + 1, 0, Resolution - 1);
		MaxC.Y = FMath::Clamp(Hi.Y + 1, 0, Resolution - 1);

		for (int32 CY = MinC.Y; CY <= MaxC.Y; ++CY)
		{
			for (int32 CX = MinC.X; CX <= MaxC.X; ++CX)
			{
				const float D = (CellToMetres(CX, CY) - Centre).Size();
				if (D >= R)
				{
					continue;
				}

				const int32 I = Index(CX, CY);
				const float Dig = PitDepthM * 0.5f * (1.0f + FMath::Cos(PI * D / R));

				SurfaceM[I] -= Dig;
				BorrowedM3 += Dig * CellAreaM2;

				CellMaterial[I].Compaction = 0.25f;   // freshly dug, loose
				CellMaterial[I].Moisture = 0.45f;     // pits hold water
				CellMaterial[I].LayerCm = 70.0f;
			}
		}
	}

	ImportedM3 = FMath::Max(0.0, Deficit - BorrowedM3);

	Log(FString::Printf(TEXT("Dug %d borrow pits (r=%.0f m, %.1f m deep): %.0f m3 recovered"),
		NumPits, R, PitDepthM, BorrowedM3));
}

void FDirtTrack::Finalise()
{
	const int32 Count = Resolution * Resolution;

	MinElevationM = MAX_flt;
	MaxElevationM = -MAX_flt;

	for (int32 I = 0; I < Count; ++I)
	{
		const FDirtMaterial& Mat = CellMaterial[I];

		// The simulation's movable layer sits on top of everything the machines
		// left behind, so bedrock is simply the finished surface less that layer.
		Bedrock[I] = SurfaceM[I] * 100.0f - Mat.LayerCm;
		Layer[I] = Mat.LayerCm;
		Compaction[I] = Mat.Compaction;
		Moisture[I] = Mat.Moisture;

		MinElevationM = FMath::Min(MinElevationM, SurfaceM[I]);
		MaxElevationM = FMath::Max(MaxElevationM, SurfaceM[I]);
	}

	double SumCm = 0.0;
	for (const float L : Layer)
	{
		SumCm += L;
	}
	BaselineVolumeM3 = SumCm * Settings.TexelAreaCm2() / 1000000.0;

	if (Settings.IsFocused())
	{
		// Cut, fill and build were only counted over the region, and the borrow
		// pits are probably outside it, so the numbers would not balance and would
		// mean nothing. Say so rather than printing a ledger that looks broken.
		Log(FString::Printf(TEXT("Earthmoving ledger not meaningful while focused on a %.0f m region"),
			Settings.RegionSizeCm() * 0.01f));
	}
	else
	{
		Log(FString::Printf(TEXT("Earthmoving ledger: cut %.0f, fill %.0f, built %.0f, borrowed %.0f, imported %.0f m3"),
			CutM3, FillM3, BuiltM3, BorrowedM3, ImportedM3));
	}
	Log(FString::Printf(TEXT("Finished ground %+.1f to %+.1f m (%.1f m of relief across the site)"),
		MinElevationM, MaxElevationM, MaxElevationM - MinElevationM));
	Log(FString::Printf(TEXT("Track %.0f m wide, %d obstacles, %.0f m3 in the worked layer"),
		TrackHalfWidthM * 2.0f, Features.Num(), BaselineVolumeM3));
}

void FDirtTrack::Build()
{
	const double StartTime = FPlatformTime::Seconds();
	const int32 Count = Resolution * Resolution;

	Bedrock.Init(0.0f, Count);
	Layer.Init(Settings.RestLayerCm, Count);
	Compaction.Init(0.5f, Count);
	Moisture.Init(0.25f, Count);
	FeatureLog.Reset();
	CutM3 = FillM3 = BuiltM3 = BorrowedM3 = ImportedM3 = 0.0;

	Log(FString::Printf(TEXT("Track: %.0f m site, %d x %d cells (%.1f cm per cell)"),
		Settings.WorldSizeCm * 0.01f, Resolution, Resolution, Settings.TexelSizeCm()));

	BuildCentreline();
	BuildDesignProfile();
	BuildFeatureList();
	StampTrack();

	// Lay the untouched site down first. Everything after this moves dirt.
	SurfaceM.SetNumUninitialized(Count);
	CellMaterial.SetNum(Count);

	for (int32 CY = 0; CY < Resolution; ++CY)
	{
		for (int32 CX = 0; CX < Resolution; ++CX)
		{
			const int32 I = Index(CX, CY);
			SurfaceM[I] = NaturalGroundM(CellToMetres(CX, CY));

			// Undisturbed ground: settled, thin topsoil.
			CellMaterial[I].Compaction = 0.8f;
			CellMaterial[I].Moisture = 0.25f;
			CellMaterial[I].LayerCm = 12.0f;
		}
	}

	GradeCorridor();
	BuildFeaturesAndBerms();
	DigBorrowPits();
	Finalise();

	Log(FString::Printf(TEXT("Built in %.2f s"), FPlatformTime::Seconds() - StartTime));
}
