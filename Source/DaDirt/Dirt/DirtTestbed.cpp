#include "DirtTestbed.h"

namespace
{
	/** tan() of an angle in degrees, clamped away from vertical. */
	float TanDeg(float Deg)
	{
		return FMath::Tan(FMath::DegreesToRadians(FMath::Clamp(Deg, 0.1f, 89.0f)));
	}

	/** Smooth 0-1 ramp used to taper feature edges instead of leaving cliffs. */
	float SmoothTaper(float Distance, float TaperWidth)
	{
		if (TaperWidth <= 0.0f)
		{
			return 1.0f;
		}
		return FMath::SmoothStep(0.0f, TaperWidth, Distance);
	}
}

FDirtTestbed::FDirtTestbed(const FDirtSimSettings& InSettings)
	: Settings(InSettings)
	, Resolution(FMath::Max(InSettings.SimResolution, 1))
	, RegionCentreM(static_cast<float>(InSettings.SimRegionCentreCm.X) * 0.01f,
					static_cast<float>(InSettings.SimRegionCentreCm.Y) * 0.01f)
	, HalfRegionM(InSettings.RegionSizeCm() * 0.005f)              // cm -> m, halved
	, MetresPerCell(InSettings.TexelSizeCm() * 0.01f)
{
}

// ---------------------------------------------------------------------------
// Grid helpers
// ---------------------------------------------------------------------------

FVector2f FDirtTestbed::CellToMetres(int32 X, int32 Y) const
{
	return RegionCentreM + FVector2f(
		-HalfRegionM + (static_cast<float>(X) + 0.5f) * MetresPerCell,
		-HalfRegionM + (static_cast<float>(Y) + 0.5f) * MetresPerCell);
}

FIntPoint FDirtTestbed::MetresToCell(const FVector2f& M) const
{
	return FIntPoint(
		FMath::FloorToInt((M.X - RegionCentreM.X + HalfRegionM) / MetresPerCell),
		FMath::FloorToInt((M.Y - RegionCentreM.Y + HalfRegionM) / MetresPerCell));
}

void FDirtTestbed::MetresToCellRect(FVector2f MinM, FVector2f MaxM, FIntPoint& OutMin, FIntPoint& OutMax) const
{
	const FIntPoint Lo = MetresToCell(MinM);
	const FIntPoint Hi = MetresToCell(MaxM);

	OutMin.X = FMath::Clamp(Lo.X - 1, 0, Resolution - 1);
	OutMin.Y = FMath::Clamp(Lo.Y - 1, 0, Resolution - 1);
	OutMax.X = FMath::Clamp(Hi.X + 1, 0, Resolution - 1);
	OutMax.Y = FMath::Clamp(Hi.Y + 1, 0, Resolution - 1);
}

void FDirtTestbed::RaiseTo(int32 X, int32 Y, float NewHeightM, const FDirtMaterial& Mat)
{
	const int32 I = Index(X, Y);
	const float NewCm = NewHeightM * 100.0f;

	if (NewCm > Bedrock[I])
	{
		// Grade the feature's dirt cover into whatever was there. Where the
		// bedrock barely rises (the toe of a dome, the foot of a ramp) the layer
		// used to jump from the pad's 60 cm to the hill's 45 in one cell, leaving
		// a 15 cm circular step that rendered as a ring of teeth around every dome.
		const float T = FMath::Clamp((NewCm - Bedrock[I]) / 30.0f, 0.0f, 1.0f);
		Bedrock[I] = NewCm;
		Compaction[I] = FMath::Lerp(Compaction[I], Mat.Compaction, T);
		Moisture[I] = FMath::Lerp(Moisture[I], Mat.Moisture, T);
		if (T > 0.5f)
		{
			Soil[I] = Mat.SoilId;
		}
		Layer[I] = FMath::Lerp(Layer[I], Mat.LayerCm, T);
	}
}

void FDirtTestbed::LowerTo(int32 X, int32 Y, float NewHeightM, const FDirtMaterial& Mat)
{
	const int32 I = Index(X, Y);
	const float NewCm = NewHeightM * 100.0f;

	if (NewCm < Bedrock[I])
	{
		Bedrock[I] = NewCm;
		Compaction[I] = Mat.Compaction;
		Moisture[I] = Mat.Moisture;
		Soil[I] = Mat.SoilId;
		Layer[I] = Mat.LayerCm;
	}
}

// ---------------------------------------------------------------------------
// Feature primitives
// ---------------------------------------------------------------------------

void FDirtTestbed::AddWedge(float CentreY, float WidthM, float ToeX, float FrontAngleDeg,
							float MaxHeightM, float FrontRunLimitM, float BackAngleDeg,
							float LipRoundM, const FDirtMaterial& Mat)
{
	const float FrontTan = TanDeg(FrontAngleDeg);
	const float BackTan = TanDeg(BackAngleDeg);

	// The crest is wherever the face reaches MaxHeight, or wherever it runs out
	// of room — whichever comes first.
	const float FrontRun = FMath::Min(FrontRunLimitM, MaxHeightM / FrontTan);
	const float PeakM = FrontRun * FrontTan;
	const float BackRun = PeakM / BackTan;

	const float HalfWidth = WidthM * 0.5f;
	const float SideTaper = 1.0f;

	FIntPoint MinC, MaxC;
	MetresToCellRect(FVector2f(ToeX, CentreY - HalfWidth),
					 FVector2f(ToeX + FrontRun + BackRun, CentreY + HalfWidth),
					 MinC, MaxC);

	for (int32 CY = MinC.Y; CY <= MaxC.Y; ++CY)
	{
		for (int32 CX = MinC.X; CX <= MaxC.X; ++CX)
		{
			const FVector2f P = CellToMetres(CX, CY);

			const float DY = FMath::Abs(P.Y - CentreY);
			if (DY > HalfWidth)
			{
				continue;
			}

			const float AlongX = P.X - ToeX;
			if (AlongX < 0.0f || AlongX > FrontRun + BackRun)
			{
				continue;
			}

			float H;
			if (AlongX <= FrontRun)
			{
				H = AlongX * FrontTan;

				// Round the lip by rolling the face off as it nears the crest.
				if (LipRoundM > 0.0f && AlongX > FrontRun - LipRoundM)
				{
					const float T = (AlongX - (FrontRun - LipRoundM)) / LipRoundM;
					H -= 0.5f * LipRoundM * FrontTan * T * T;
				}
			}
			else
			{
				H = PeakM - (AlongX - FrontRun) * BackTan;
			}

			// Taper the sides so a wedge is a built ramp, not a floating slab.
			H *= SmoothTaper(HalfWidth - DY, SideTaper);

			if (H > 0.0f)
			{
				RaiseTo(CX, CY, H, Mat);
			}
		}
	}
}

void FDirtTestbed::AddDome(FVector2f CentreM, float RadiusM, float HeightM, const FDirtMaterial& Mat)
{
	FIntPoint MinC, MaxC;
	MetresToCellRect(CentreM - FVector2f(RadiusM), CentreM + FVector2f(RadiusM), MinC, MaxC);

	for (int32 CY = MinC.Y; CY <= MaxC.Y; ++CY)
	{
		for (int32 CX = MinC.X; CX <= MaxC.X; ++CX)
		{
			const float D = (CellToMetres(CX, CY) - CentreM).Size();
			if (D >= RadiusM)
			{
				continue;
			}

			// Raised cosine: flat at the summit, flat where it meets the ground.
			const float H = HeightM * 0.5f * (1.0f + FMath::Cos(PI * D / RadiusM));
			RaiseTo(CX, CY, H, Mat);
		}
	}
}

void FDirtTestbed::AddLongRidge(float CentreX, float HalfWidthM, float HeightM, float FromY, float ToY,
								const FDirtMaterial& Mat)
{
	FIntPoint MinC, MaxC;
	MetresToCellRect(FVector2f(CentreX - HalfWidthM, FromY),
					 FVector2f(CentreX + HalfWidthM, ToY), MinC, MaxC);

	for (int32 CY = MinC.Y; CY <= MaxC.Y; ++CY)
	{
		for (int32 CX = MinC.X; CX <= MaxC.X; ++CX)
		{
			const FVector2f P = CellToMetres(CX, CY);
			if (P.Y < FromY || P.Y > ToY)
			{
				continue;
			}

			const float DX = FMath::Abs(P.X - CentreX);
			if (DX >= HalfWidthM)
			{
				continue;
			}

			float H = HeightM * 0.5f * (1.0f + FMath::Cos(PI * DX / HalfWidthM));

			// Fade out at the ends of the ridge.
			H *= SmoothTaper(P.Y - FromY, 6.0f) * SmoothTaper(ToY - P.Y, 6.0f);

			RaiseTo(CX, CY, H, Mat);
		}
	}
}

void FDirtTestbed::AddPad(FVector2f CentreM, FVector2f HalfExtentM, float HeightM, float EdgeFalloffM,
						  const FDirtMaterial& Mat)
{
	FIntPoint MinC, MaxC;
	MetresToCellRect(CentreM - HalfExtentM - FVector2f(EdgeFalloffM),
					 CentreM + HalfExtentM + FVector2f(EdgeFalloffM), MinC, MaxC);

	for (int32 CY = MinC.Y; CY <= MaxC.Y; ++CY)
	{
		for (int32 CX = MinC.X; CX <= MaxC.X; ++CX)
		{
			const FVector2f P = CellToMetres(CX, CY);
			const FVector2f D = FVector2f(FMath::Abs(P.X - CentreM.X), FMath::Abs(P.Y - CentreM.Y));

			if (D.X > HalfExtentM.X + EdgeFalloffM || D.Y > HalfExtentM.Y + EdgeFalloffM)
			{
				continue;
			}

			const float Fade = SmoothTaper(HalfExtentM.X + EdgeFalloffM - D.X, EdgeFalloffM)
							 * SmoothTaper(HalfExtentM.Y + EdgeFalloffM - D.Y, EdgeFalloffM);

			// A pad at height 0 still needs to claim its material, so stamp the
			// material even when the height does not win.
			const int32 I = Index(CX, CY);
			const float HCm = HeightM * 100.0f * Fade;
			if (HCm >= Bedrock[I])
			{
				Bedrock[I] = HCm;
				Compaction[I] = Mat.Compaction;
				Moisture[I] = Mat.Moisture;
				Soil[I] = Mat.SoilId;
				Layer[I] = Mat.LayerCm;
			}
		}
	}
}

void FDirtTestbed::AddLooseCone(FVector2f CentreM, float RadiusM, float HeightM, const FDirtMaterial& Mat)
{
	FIntPoint MinC, MaxC;
	MetresToCellRect(CentreM - FVector2f(RadiusM), CentreM + FVector2f(RadiusM), MinC, MaxC);

	for (int32 CY = MinC.Y; CY <= MaxC.Y; ++CY)
	{
		for (int32 CX = MinC.X; CX <= MaxC.X; ++CX)
		{
			const float D = (CellToMetres(CX, CY) - CentreM).Size();
			if (D >= RadiusM)
			{
				continue;
			}

			// Straight-sided cone, added to the DIRT LAYER not bedrock, so the
			// slump pass is free to collapse it.
			const float H = HeightM * (1.0f - D / RadiusM);
			const int32 I = Index(CX, CY);
			Layer[I] += H * 100.0f;
			Compaction[I] = Mat.Compaction;
			Moisture[I] = Mat.Moisture;
			Soil[I] = Mat.SoilId;
		}
	}
}

void FDirtTestbed::AddBowl(FVector2f CentreM, float RadiusM, float DepthM, const FDirtMaterial& Mat)
{
	FIntPoint MinC, MaxC;
	MetresToCellRect(CentreM - FVector2f(RadiusM), CentreM + FVector2f(RadiusM), MinC, MaxC);

	for (int32 CY = MinC.Y; CY <= MaxC.Y; ++CY)
	{
		for (int32 CX = MinC.X; CX <= MaxC.X; ++CX)
		{
			const float D = (CellToMetres(CX, CY) - CentreM).Size();
			if (D >= RadiusM)
			{
				continue;
			}

			const float H = -DepthM * 0.5f * (1.0f + FMath::Cos(PI * D / RadiusM));
			LowerTo(CX, CY, H, Mat);
		}
	}
}

void FDirtTestbed::AddBermArc(FVector2f CentreM, float RadiusM, float FromAngleDeg, float ToAngleDeg,
							  float BankAngleDeg, float HeightM, const FDirtMaterial& Mat)
{
	const float BankTan = TanDeg(BankAngleDeg);
	const float OuterM = RadiusM + HeightM / BankTan + 2.5f;   // run-up plus back-slope

	FIntPoint MinC, MaxC;
	MetresToCellRect(CentreM - FVector2f(OuterM), CentreM + FVector2f(OuterM), MinC, MaxC);

	for (int32 CY = MinC.Y; CY <= MaxC.Y; ++CY)
	{
		for (int32 CX = MinC.X; CX <= MaxC.X; ++CX)
		{
			const FVector2f Rel = CellToMetres(CX, CY) - CentreM;
			const float D = Rel.Size();

			// Only the outside of the arc carries the banked wall.
			const float Outward = D - RadiusM;
			if (Outward <= 0.0f || Outward > OuterM - RadiusM)
			{
				continue;
			}

			float AngleDeg = FMath::RadiansToDegrees(FMath::Atan2(Rel.Y, Rel.X));
			if (AngleDeg < 0.0f)
			{
				AngleDeg += 360.0f;
			}
			if (AngleDeg < FromAngleDeg || AngleDeg > ToAngleDeg)
			{
				continue;
			}

			// Real berms run 0.3-1.2 m tall, with the face steepening to near
			// vertical at the top rather than climbing at one constant angle.
			// BankAngleDeg here is the AVERAGE angle, which sets how far out the
			// berm reaches; the exponent does the steepening.
			const float Run = HeightM / BankTan;
			const float X = FMath::Clamp(Outward / FMath::Max(Run, KINDA_SMALL_NUMBER), 0.0f, 1.0f);
			float H = HeightM * FMath::Pow(X, 2.5f);

			// Past the crest the pile has to come back down to the ground. Without
			// this the berm ends in a vertical wall its own height, which is not a
			// berm, and is not something dirt does.
			constexpr float BackRunM = 2.0f;
			if (Outward > Run)
			{
				H *= 1.0f - SmoothTaper(Outward - Run, BackRunM);
			}

			// Fade in and out at the ends of the arc so it blends into the flat.
			H *= SmoothTaper(AngleDeg - FromAngleDeg, 12.0f)
			   * SmoothTaper(ToAngleDeg - AngleDeg, 12.0f);

			if (H > 0.0f)
			{
				RaiseTo(CX, CY, H, Mat);
			}
		}
	}
}

// ---------------------------------------------------------------------------
// The composed testbed
// ---------------------------------------------------------------------------

void FDirtTestbed::Build()
{
	const int32 Count = Resolution * Resolution;

	Bedrock.Init(0.0f, Count);
	Layer.Init(Settings.RestLayerCm, Count);
	Compaction.Init(0.3f, Count);
	Moisture.Init(0.2f, Count);
	Soil.Init(1, Count);
	FeatureLog.Reset();

	Log(FString::Printf(TEXT("Testbed %.0f m box, %d x %d cells, %.1f cm per cell"),
		Settings.WorldSizeCm * 0.01f, Resolution, Resolution, Settings.TexelSizeCm()));

	BuildAngleSpectrum();
	BuildJumpLineup();
	BuildCalibrationPad();
	BuildHills();
	BuildBermAndBowls();
	BuildWaveSections();

	// Baseline dirt volume, for the conservation audit to measure drift against.
	double SumCm = 0.0;
	for (const float L : Layer)
	{
		SumCm += L;
	}
	BaselineVolumeM3 = SumCm * Settings.TexelAreaCm2() / 1000000.0;

	Log(FString::Printf(TEXT("Baseline dirt volume %.3f m3"), BaselineVolumeM3));
}

void FDirtTestbed::GetAngleSpectrum(const FDirtSimSettings& Settings, TArray<FDirtAngleWedge>& Out)
{
	// Twelve wedges across the full width, bracketing both the loose repose angle
	// (~32 deg) and the packed one (~70 deg), so some must hold and some must shed.
	static const float Angles[] = { 10.f, 15.f, 20.f, 25.f, 30.f, 35.f, 40.f, 45.f, 50.f, 55.f, 65.f, 80.f };
	static constexpr float MaxHeightM = 7.0f;
	static constexpr float FrontRunLimitM = 10.0f;

	const int32 NumWedges = UE_ARRAY_COUNT(Angles);
	const float HalfWorldM = Settings.WorldSizeCm * 0.005f;
	const float Pitch = (HalfWorldM * 2.0f) / NumWedges;
	const float ToeX = -HalfWorldM + 2.0f;

	Out.Reset(NumWedges);

	for (int32 i = 0; i < NumWedges; ++i)
	{
		FDirtAngleWedge W;
		W.AngleDeg = Angles[i];
		W.CentreY = -HalfWorldM + Pitch * (static_cast<float>(i) + 0.5f);
		W.WidthM = Pitch - 1.2f;
		W.ToeX = ToeX;

		const float FrontRun = FMath::Min(FrontRunLimitM, MaxHeightM / TanDeg(Angles[i]));
		W.CrestX = ToeX + FrontRun;
		W.FaceMidX = ToeX + FrontRun * 0.5f;
		W.PeakHeightM = FrontRun * TanDeg(Angles[i]);

		Out.Add(W);
	}
}

void FDirtTestbed::BuildAngleSpectrum()
{
	TArray<FDirtAngleWedge> Wedges;
	GetAngleSpectrum(Settings, Wedges);

	FDirtMaterial Mat;
	Mat.Compaction = 0.92f;      // built and packed, so the shape holds
	Mat.Moisture = 0.35f;        // tacky loam
	Mat.LayerCm = 20.0f;         // a thin skin of dirt over the built shape

	for (const FDirtAngleWedge& W : Wedges)
	{
		AddWedge(W.CentreY, W.WidthM, W.ToeX, W.AngleDeg,
				 /*MaxHeightM*/ 7.0f, /*FrontRunLimitM*/ 10.0f,
				 /*BackAngleDeg*/ 25.0f, /*LipRoundM*/ 0.0f, Mat);
	}

	Log(FString::Printf(TEXT("Angle spectrum: %d wedges, %.0f to %.0f deg, toe at X=%.0f m"),
		Wedges.Num(), Wedges[0].AngleDeg, Wedges.Last().AngleDeg, Wedges[0].ToeX));
}

void FDirtTestbed::BuildJumpLineup()
{
	FDirtMaterial Packed;
	Packed.Compaction = 0.95f;
	Packed.Moisture = 0.4f;
	Packed.LayerCm = 18.0f;

	// Four takeoffs, same height, deliberately different lip character: the lip
	// is what decides whether a jump kicks you or launches you smoothly.
	struct FJump
	{
		float CentreY;
		float FaceAngleDeg;
		float HeightM;
		float LipRoundM;
		const TCHAR* Name;
	};

	static const FJump Jumps[] =
	{
		{ -48.0f, 22.0f, 2.5f, 2.0f, TEXT("rounded tabletop") },
		{ -16.0f, 32.0f, 2.5f, 0.6f, TEXT("standard face, crisp lip") },
		{  16.0f, 45.0f, 2.5f, 0.0f, TEXT("steep kicker, sharp lip") },
		{  48.0f, 70.0f, 3.5f, 0.0f, TEXT("step-up, near vertical") },
	};

	const float TakeoffToeX = -34.0f;
	const float LandingToeX = -18.0f;

	for (const FJump& J : Jumps)
	{
		// Takeoff: the given face angle, steep 45 deg back.
		AddWedge(J.CentreY, 9.0f, TakeoffToeX, J.FaceAngleDeg, J.HeightM,
				 /*FrontRunLimitM*/ 8.0f, /*BackAngleDeg*/ 45.0f, J.LipRoundM, Packed);

		// Landing: steep face toward the gap, long gentle runout away from it.
		AddWedge(J.CentreY, 11.0f, LandingToeX, 38.0f, J.HeightM,
				 /*FrontRunLimitM*/ 7.0f, /*BackAngleDeg*/ 14.0f, 1.0f, Packed);
	}

	Log(FString::Printf(TEXT("Jump lineup: %d takeoff/landing pairs, faces 22/32/45/70 deg"),
		static_cast<int32>(UE_ARRAY_COUNT(Jumps))));
}

void FDirtTestbed::BuildCalibrationPad()
{
	// Dead flat, known area, loose-ish dirt. Every volume audit and repose
	// measurement happens here, where nothing else can contaminate the reading.
	FDirtMaterial PadMat;
	PadMat.Compaction = 0.25f;
	PadMat.Moisture = 0.15f;
	PadMat.LayerCm = Settings.RestLayerCm;

	AddPad(FVector2f(3.0f, 0.0f), FVector2f(9.0f, 30.0f), 0.0f, 3.0f, PadMat);

	// The soil quilt: the north end of the pad in five lanes across, one per
	// reference soil, so every soil test drives the same flat ground. X = 3
	// stays loam, so nothing measured on the pad's centre line changes.
	{
		FIntPoint QMin, QMax;
		MetresToCellRect(FVector2f(-6.0f, 20.0f), FVector2f(12.0f, 30.0f), QMin, QMax);
		static const uint8 Lanes[5] = { 0, 3, 1, 2, 4 };      // sand, granite, loam, pnw, clay
		for (int32 CY = QMin.Y; CY <= QMax.Y; ++CY)
		{
			for (int32 CX = QMin.X; CX <= QMax.X; ++CX)
			{
				const FVector2f P = CellToMetres(CX, CY);
				if (P.X < -6.0f || P.X > 12.0f || P.Y < 20.0f || P.Y > 30.0f)
				{
					continue;
				}
				const int32 Lane = FMath::Clamp(FMath::FloorToInt((P.X + 6.0f) / 3.6f), 0, 4);
				Soil[Index(CX, CY)] = Lanes[Lane];
			}
		}
	}

	// A cone far steeper than loose dirt can stand. It should visibly collapse
	// within the first second and settle at the loose repose angle — the fastest
	// possible check that slumping is alive and correctly tuned.
	FDirtMaterial LooseMat;
	LooseMat.Compaction = 0.0f;
	LooseMat.Moisture = 0.0f;
	AddLooseCone(FVector2f(3.0f, -45.0f), /*RadiusM*/ 2.0f, /*HeightM*/ 3.0f, LooseMat);

	// A known-volume block of loose dirt: 10 m x 10 m x 1 m = 100 m3 exactly.
	// Handy for sanity-checking the audit's arithmetic by eye.
	FIntPoint MinC, MaxC;
	MetresToCellRect(FVector2f(-2.0f, 40.0f), FVector2f(8.0f, 50.0f), MinC, MaxC);
	for (int32 CY = MinC.Y; CY <= MaxC.Y; ++CY)
	{
		for (int32 CX = MinC.X; CX <= MaxC.X; ++CX)
		{
			const FVector2f P = CellToMetres(CX, CY);
			if (P.X < -2.0f || P.X > 8.0f || P.Y < 40.0f || P.Y > 50.0f)
			{
				continue;
			}
			const int32 I = Index(CX, CY);
			Layer[I] += 100.0f;              // 1 m of extra dirt
			Compaction[I] = 0.1f;
			Moisture[I] = 0.05f;
		}
	}

	Log(TEXT("Calibration pad: flat 18 x 60 m, loose cone at (3, -45), 100 m3 block at (3, 45)"));
	Log(TEXT("Soil quilt on the pad, Y = 20..30: lanes of sand, granite, loam, pnw, clay from X = -6 to 12"));
}

void FDirtTestbed::BuildHills()
{
	// The dramatic stuff. Medium compaction so the slopes hold: a cosine dome
	// 11 m tall over an 18 m radius peaks at about 44 deg, which is well past
	// loose repose and comfortably inside what half-packed dirt can stand.
	FDirtMaterial HillMat;
	HillMat.Compaction = 0.55f;
	HillMat.Moisture = 0.35f;
	HillMat.LayerCm = 45.0f;

	AddDome(FVector2f(28.0f, -32.0f), /*RadiusM*/ 18.0f, /*HeightM*/ 11.0f, HillMat);
	AddDome(FVector2f(22.0f,  30.0f), /*RadiusM*/ 14.0f, /*HeightM*/  7.5f, HillMat);
	AddLongRidge(/*CentreX*/ 40.0f, /*HalfWidthM*/ 10.0f, /*HeightM*/ 9.0f,
				 /*FromY*/ -10.0f, /*ToY*/ 20.0f, HillMat);

	Log(TEXT("Hills: 11 m dome at (28, -32), 7.5 m dome at (22, 30), 9 m ridge at X=40"));
}

void FDirtTestbed::AddWaveStrip(float CentreY, float WidthM, float FromX, float ToX,
								float SpacingM, float HeightM, const FDirtMaterial& Mat)
{
	const float HalfWidth = WidthM * 0.5f;

	FIntPoint MinC, MaxC;
	MetresToCellRect(FVector2f(FromX, CentreY - HalfWidth),
					 FVector2f(ToX, CentreY + HalfWidth), MinC, MaxC);

	for (int32 CY = MinC.Y; CY <= MaxC.Y; ++CY)
	{
		for (int32 CX = MinC.X; CX <= MaxC.X; ++CX)
		{
			const FVector2f P = CellToMetres(CX, CY);

			const float DY = FMath::Abs(P.Y - CentreY);
			if (DY > HalfWidth || P.X < FromX || P.X > ToX)
			{
				continue;
			}

			const float Along = P.X - FromX;
			const int32 Waves = FMath::Max(1, FMath::FloorToInt((ToX - FromX) / SpacingM));
			if (Along > Waves * SpacingM)
			{
				continue;
			}

			float H = HeightM * 0.5f * (1.0f - FMath::Cos(2.0f * PI * Along / SpacingM));
			H *= SmoothTaper(HalfWidth - DY, 1.0f);

			if (H > 0.0f)
			{
				RaiseTo(CX, CY, H, Mat);
			}
		}
	}
}

void FDirtTestbed::BuildWaveSections()
{
	FDirtMaterial Mat;
	Mat.Compaction = 0.8f;
	Mat.Moisture = 0.35f;
	Mat.LayerCm = 22.0f;

	// FIM 047.3.7 rolling waves: about 10 m between peaks, capped at 80 cm. These
	// are what outdoor motocross is allowed to have.
	AddWaveStrip(/*CentreY*/ -60.0f, /*WidthM*/ 8.0f, /*FromX*/ 14.0f, /*ToX*/ 64.0f,
				 /*SpacingM*/ 10.0f, /*HeightM*/ 0.8f, Mat);

	// Supercross whoops: about 4.3 m peak to peak and 90 cm tall. Explicitly
	// forbidden outdoors ("Washboards/Whoops sections are not allowed"), here only
	// so the two can be compared side by side.
	AddWaveStrip(/*CentreY*/ 60.0f, /*WidthM*/ 8.0f, /*FromX*/ 14.0f, /*ToX*/ 64.0f,
				 /*SpacingM*/ 4.3f, /*HeightM*/ 0.9f, Mat);

	Log(TEXT("Waves: FIM rolling waves (10 m / 80 cm) at Y=-60, SX whoops (4.3 m / 90 cm) at Y=+60"));
}

void FDirtTestbed::BuildBermAndBowls()
{
	FDirtMaterial BermMat;
	BermMat.Compaction = 0.9f;
	BermMat.Moisture = 0.45f;
	BermMat.LayerCm = 25.0f;

	// A banked corner wall — the thing Phase 1e's test wheel will carve into.
	// 1.1 m tall, the top of the real 0.3-1.2 m range (the earlier 2.8 m was more
	// than twice what a berm ever reaches). 34 deg average puts the height at about
	// a third of the berm's width, which is the proportion builders aim for, and
	// the face steepens to roughly 59 deg at the top.
	AddBermArc(FVector2f(56.0f, 0.0f), /*RadiusM*/ 14.0f,
			   /*FromAngleDeg*/ 200.0f, /*ToAngleDeg*/ 340.0f,
			   /*BankAngleDeg*/ 34.0f, /*HeightM*/ 1.1f, BermMat);

	// Concave and convex of the same size, side by side. Dirt should collect in
	// the bowl and shed off the mound — the clearest read on whether slumping
	// respects curvature.
	FDirtMaterial NeutralMat;
	NeutralMat.Compaction = 0.3f;
	NeutralMat.Moisture = 0.2f;
	NeutralMat.LayerCm = Settings.RestLayerCm;

	AddBowl(FVector2f(54.0f, -42.0f), /*RadiusM*/ 10.0f, /*DepthM*/ 3.0f, NeutralMat);
	AddDome(FVector2f(54.0f,  42.0f), /*RadiusM*/ 10.0f, /*HeightM*/ 3.0f, NeutralMat);

	Log(TEXT("Berm arc at (56, 0) r=14 m bank 40 deg; bowl at (54, -42), mound at (54, 42)"));
}
