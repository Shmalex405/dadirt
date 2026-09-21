// The reference soils. Every number is a measurable property with a source
// in docs/SoilPhysics.md section 8; none is a feel setting. Wet, mud and
// dust are what the moisture channel does to these, not soils of their own.

#include "DirtSimTypes.h"

void FDirtSoil::Presets(TArray<FDirtSoil>& Out)
{
	Out.Reset();

	// 0. Sand: dry beach/pit sand, rounded quartz. Wong's dry sand for the tyre.
	{
		FDirtSoil S;
		S.Name = TEXT("sand");
		S.LoosePorosity = 0.44f;  S.DensePorosity = 0.36f;
		S.LooseReposeDeg = 31.0f; S.PackedReposeDeg = 38.0f;
		S.SuctionCohesionKPa = 2.0f; S.PackedCohesionKPa = 0.5f;
		S.UnitWeightKNm3 = 16.5f; S.SaturationFrictionLoss = 0.5f;
		S.InfiltrationCmPerSec = 1.2f; S.FieldCapacity = 0.10f; S.ProctorOptimum = 0.7f; S.DryingMultiplier = 1.5f;
		S.Dustiness = 0.6f;
		S.DryColour = FLinearColor(0.60f, 0.53f, 0.38f); S.PackedColour = FLinearColor(0.46f, 0.40f, 0.28f);
		S.BekkerNLoose = 1.1f; S.BekkerNDense = 0.9f;
		S.BekkerKcLoose = 0.99f; S.BekkerKcDense = 3.0f;
		S.BekkerKphiLoose = 1528.0f; S.BekkerKphiDense = 4000.0f;
		S.ShearModulusLooseM = 0.015f; S.ShearModulusDenseM = 0.025f;
		S.SaturationStiffnessLoss = 0.5f;
		Out.Add(S);
	}
	// 1. Loam: the classic track dirt. The defaults, measured in sections 9b-9k.
	{
		FDirtSoil S;
		S.Name = TEXT("loam");
		Out.Add(S);
	}
	// 2. Pacific Northwest: dark silty loam, organic, holds water, tacky when damp, dusts little.
	{
		FDirtSoil S;
		S.Name = TEXT("pnw");
		S.LoosePorosity = 0.52f;  S.DensePorosity = 0.36f;
		S.LooseReposeDeg = 28.0f; S.PackedReposeDeg = 38.0f;
		S.SuctionCohesionKPa = 6.0f; S.PackedCohesionKPa = 12.0f;
		S.UnitWeightKNm3 = 16.0f; S.SaturationFrictionLoss = 0.65f;
		S.InfiltrationCmPerSec = 0.15f; S.FieldCapacity = 0.45f; S.ProctorOptimum = 0.6f; S.DryingMultiplier = 0.6f;
		S.Dustiness = 0.3f;
		S.DryColour = FLinearColor(0.28f, 0.20f, 0.13f); S.PackedColour = FLinearColor(0.17f, 0.12f, 0.08f);
		S.BekkerNLoose = 0.7f; S.BekkerNDense = 0.5f;
		S.BekkerKcLoose = 5.27f; S.BekkerKcDense = 13.0f;
		S.BekkerKphiLoose = 1515.0f; S.BekkerKphiDense = 5000.0f;
		S.ShearModulusLooseM = 0.03f; S.ShearModulusDenseM = 0.05f;
		S.SaturationStiffnessLoss = 0.85f;
		Out.Add(S);
	}
	// 3. American Southwest: decomposed granite, angular sand and grit, dries in minutes, a dust storm.
	{
		FDirtSoil S;
		S.Name = TEXT("granite");
		S.LoosePorosity = 0.42f;  S.DensePorosity = 0.33f;
		S.LooseReposeDeg = 36.0f; S.PackedReposeDeg = 44.0f;
		S.SuctionCohesionKPa = 1.5f; S.PackedCohesionKPa = 3.0f;
		S.UnitWeightKNm3 = 17.5f; S.SaturationFrictionLoss = 0.45f;
		S.InfiltrationCmPerSec = 1.0f; S.FieldCapacity = 0.12f; S.ProctorOptimum = 0.45f; S.DryingMultiplier = 1.6f;
		S.Dustiness = 1.6f;
		S.DryColour = FLinearColor(0.55f, 0.42f, 0.30f); S.PackedColour = FLinearColor(0.40f, 0.30f, 0.21f);
		S.BekkerNLoose = 1.0f; S.BekkerNDense = 0.6f;
		S.BekkerKcLoose = 2.0f; S.BekkerKcDense = 10.0f;
		S.BekkerKphiLoose = 1200.0f; S.BekkerKphiDense = 6000.0f;
		S.ShearModulusLooseM = 0.015f; S.ShearModulusDenseM = 0.03f;
		S.SaturationStiffnessLoss = 0.55f;
		Out.Add(S);
	}
	// 4. Hardpack clay / caliche: the Southwest's cemented base and any blue-groove line.
	{
		FDirtSoil S;
		S.Name = TEXT("clay");
		S.LoosePorosity = 0.45f;  S.DensePorosity = 0.30f;
		S.LooseReposeDeg = 24.0f; S.PackedReposeDeg = 34.0f;
		S.SuctionCohesionKPa = 8.0f; S.PackedCohesionKPa = 25.0f;
		S.UnitWeightKNm3 = 18.0f; S.SaturationFrictionLoss = 0.75f;
		S.InfiltrationCmPerSec = 0.02f; S.FieldCapacity = 0.40f; S.ProctorOptimum = 0.5f; S.DryingMultiplier = 0.8f;
		S.Dustiness = 0.8f;
		S.DryColour = FLinearColor(0.50f, 0.45f, 0.36f); S.PackedColour = FLinearColor(0.34f, 0.30f, 0.24f);
		S.BekkerNLoose = 0.5f; S.BekkerNDense = 0.4f;
		S.BekkerKcLoose = 13.2f; S.BekkerKcDense = 40.0f;
		S.BekkerKphiLoose = 692.0f; S.BekkerKphiDense = 8000.0f;
		S.ShearModulusLooseM = 0.04f; S.ShearModulusDenseM = 0.06f;
		S.SaturationStiffnessLoss = 0.9f;
		Out.Add(S);
	}
}
