#include "DirtBox.h"

#include "DirtBall.h"
#include "DirtWheel.h"
#include "DirtSimulation.h"
#include "DirtTestbed.h"
#include "DirtTrack.h"

#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Math/RandomStream.h"
#include "ProceduralMeshComponent.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"
#include "TextureResource.h"

DEFINE_LOG_CATEGORY_STATIC(LogDirt, Log, All);

TWeakObjectPtr<ADirtBox> ADirtBox::ActiveBox;

/**
 * One height window's plumbing. Shared between the game thread (which moves it
 * and reads Game) and the render thread (which owns the readbacks). Two
 * readbacks alternate so one can be in flight while the other is harvested.
 */
struct FDirtWindowSlot
{
	explicit FDirtWindowSlot(int32 InSize) : Size(InSize) {}

	const int32 Size;
	bool bReleased = false;

	// Game thread only.
	FDirtHeightWindow Game;

	// Shared, under Lock.
	FCriticalSection Lock;
	FIntPoint RequestedOrigin = FIntPoint::ZeroValue;
	FIntPoint ReadyOrigin = FIntPoint::ZeroValue;
	TArray<FLinearColor> ReadyData;
	bool bReady = false;

	// Render thread only.
	TUniquePtr<FRHIGPUTextureReadback> Readback[2];
	FIntPoint PendingOrigin[2];
	bool bPending[2] = { false, false };
};

ADirtBox::ADirtBox()
{
	PrimaryActorTick.bCanEverTick = true;

	GroundMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("GroundMesh"));
	SetRootComponent(GroundMesh);

	// The mesh is displaced entirely in the vertex shader, so its collision would
	// be a flat plane and lie about where the ground is. Physics queries go
	// through GetSurfaceHeightAtWorld instead.
	GroundMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	GroundMesh->bUseAsyncCooking = false;
	GroundMesh->SetCastShadow(true);
}

void ADirtBox::BeginPlay()
{
	Super::BeginPlay();

	ActiveBox = this;

	// A Dirtbox spawned by the game mode has nothing in its details panel to set,
	// so fall back to loading the material by path.
	if (!GroundMaterial && GroundMaterialPath.IsValid())
	{
		GroundMaterial = Cast<UMaterialInterface>(GroundMaterialPath.TryLoad());
	}

	ApplyModeDefaults();
	CreateResources();
	BuildTerrainAndUpload();
	BuildDisplayMesh();
	UpdateMaterialParameters();

	bResourcesReady = true;
	bNeedsReinit = true;

	UE_LOG(LogDirt, Log, TEXT("Dirtbox ready [%s]. %d x %d cells over %.0f m (%.2f cm per cell)."),
		TerrainMode == EDirtTerrainMode::Track ? TEXT("track") : TEXT("testbed"),
		Settings.SimResolution, Settings.SimResolution,
		Settings.WorldSizeCm * 0.01f, Settings.TexelSizeCm());

	for (const FString& Line : FeatureLog)
	{
		UE_LOG(LogDirt, Log, TEXT("  %s"), *Line);
	}
}

void ADirtBox::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	bResourcesReady = false;

	if (ActiveBox.Get() == this)
	{
		ActiveBox = nullptr;
	}

	ReleaseResources();

	Super::EndPlay(EndPlayReason);
}

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------

void ADirtBox::CreateResources()
{
	const int32 Res = FMath::Max(Settings.SimResolution, 16);

	// The state targets are full 32-bit float, not 16. A half float quantises to
	// about 1 cm at 12 m of height, and the slump threshold at 32 degrees over a
	// 12.5 cm cell is only 7.8 cm — so 16-bit precision would terrace the tall
	// hills and make the angle of repose wobble by several degrees near the
	// summits. Normal and debug stay at 16f, where precision does not matter.
	//
	// Nothing here is 8-bit: typed UAV writes to 8-bit formats are optional in
	// D3D12 and quietly unsupported on some drivers.
	const auto MakeRT = [this, Res](const TCHAR* Name, ETextureRenderTargetFormat Format)
		-> UTextureRenderTarget2D*
	{
		UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(this, Name);
		RT->RenderTargetFormat = Format;
		RT->ClearColor = FLinearColor::Black;
		RT->bAutoGenerateMips = false;
		RT->bCanCreateUAV = true;               // the sim writes these from compute
		RT->AddressX = TA_Clamp;
		RT->AddressY = TA_Clamp;
		RT->Filter = TF_Bilinear;
		RT->InitAutoFormat(Res, Res);
		RT->UpdateResourceImmediate(true);
		return RT;
	};

	StateA = MakeRT(TEXT("DirtStateA"), RTF_RGBA32f);
	StateB = MakeRT(TEXT("DirtStateB"), RTF_RGBA32f);
	DisplayRT = MakeRT(TEXT("DirtDisplay"), RTF_RGBA32f);
	NormalRT = MakeRT(TEXT("DirtNormal"), RTF_RGBA16f);
	DebugRT = MakeRT(TEXT("DirtDebug"), RTF_RGBA16f);

	// Bedrock is static, so it lives in a plain texture rather than a render
	// target. R32F because 16-bit floats quantise to about 1 cm at 10 m, which
	// would put visible steps in the big hills.
	BaseHeightTex = UTexture2D::CreateTransient(Res, Res, PF_R32_FLOAT);
	BaseHeightTex->SRGB = false;
	BaseHeightTex->Filter = TF_Bilinear;
	BaseHeightTex->AddressX = TA_Clamp;
	BaseHeightTex->AddressY = TA_Clamp;
	BaseHeightTex->NeverStream = true;
	// No UpdateResource yet: BuildTestbedAndUpload fills the pixels first, so the
	// resource is never created from uninitialised memory.

	InitialStateTex = UTexture2D::CreateTransient(Res, Res, PF_A32B32G32R32F);
	InitialStateTex->SRGB = false;
	InitialStateTex->Filter = TF_Nearest;
	InitialStateTex->AddressX = TA_Clamp;
	InitialStateTex->AddressY = TA_Clamp;
	InitialStateTex->NeverStream = true;
}

void ADirtBox::ReleaseResources()
{
	Readback.Empty();
	BedrockCm.Empty();
	PendingStrokes.Empty();
}

void ADirtBox::ApplyModeDefaults()
{
	// Each mode has a scale it only makes sense at. The testbed is sized so a cell
	// is 12.5 cm (fine enough to see dirt behave); the track is sized so a real
	// 1.5 km lap fits (which costs cell size - see docs/MXTrackReference.md).
	if (TerrainMode == EDirtTerrainMode::Track)
	{
		Settings.WorldSizeCm = FDirtTrack::RecommendedWorldSizeCm();   // 384 m
		Settings.MeshVertsPerSide = 768;                               // ~50 cm vertex spacing
	}
	else
	{
		Settings.WorldSizeCm = 12800.0f;                               // 128 m
		Settings.MeshVertsPerSide = 512;                               // ~25 cm vertex spacing
	}

	// Changing what the box contains invalidates where the sim was pointed.
	Settings.SimRegionSizeCm = 0.0f;
	Settings.SimRegionCentreCm = FVector2D::ZeroVector;
}

void ADirtBox::BuildTerrainAndUpload()
{
	const int32 Res = FMath::Max(Settings.SimResolution, 16);
	const int32 Count = Res * Res;

	TArray<float> SrcBedrock, SrcLayer, SrcCompaction, SrcMoisture;

	if (TerrainMode == EDirtTerrainMode::Track)
	{
		FDirtTrack Track(Settings);
		Track.Build();
		FeatureLog = Track.FeatureLog;
		BaselineVolumeM3 = Track.BaselineVolumeM3;
		LapLengthM = Track.LapLengthM;
		SrcBedrock = MoveTemp(Track.Bedrock);   SrcLayer = MoveTemp(Track.Layer);
		SrcCompaction = MoveTemp(Track.Compaction); SrcMoisture = MoveTemp(Track.Moisture);
	}
	else
	{
		FDirtTestbed Testbed(Settings);
		Testbed.Build();
		FeatureLog = Testbed.FeatureLog;
		BaselineVolumeM3 = Testbed.BaselineVolumeM3;
		LapLengthM = 0.0f;
		SrcBedrock = MoveTemp(Testbed.Bedrock); SrcLayer = MoveTemp(Testbed.Layer);
		SrcCompaction = MoveTemp(Testbed.Compaction); SrcMoisture = MoveTemp(Testbed.Moisture);
	}

	BedrockCm = SrcBedrock;

	// --- bedrock -> R32F ---------------------------------------------------
	{
		FTexture2DMipMap& Mip = BaseHeightTex->GetPlatformData()->Mips[0];
		void* Dest = Mip.BulkData.Lock(LOCK_READ_WRITE);
		FMemory::Memcpy(Dest, SrcBedrock.GetData(), Count * sizeof(float));
		Mip.BulkData.Unlock();
		BaseHeightTex->UpdateResource();
	}

	// --- initial dirt state -> RGBA32F ------------------------------------
	{
		TArray<FLinearColor> Initial;
		Initial.SetNumUninitialized(Count);

		for (int32 i = 0; i < Count; ++i)
		{
			FLinearColor& C = Initial[i];
			C.R = SrcLayer[i];                                       // dirt thickness, cm
			C.G = SrcCompaction[i];
			C.B = SrcMoisture[i];
			C.A = SrcBedrock[i] + SrcLayer[i];                       // total surface, cm
		}

		FTexture2DMipMap& Mip = InitialStateTex->GetPlatformData()->Mips[0];
		void* Dest = Mip.BulkData.Lock(LOCK_READ_WRITE);
		FMemory::Memcpy(Dest, Initial.GetData(), Count * sizeof(FLinearColor));
		Mip.BulkData.Unlock();
		InitialStateTex->UpdateResource();
	}

	// Make sure both uploads have actually landed before the first sim step
	// reads them. This happens once, at startup, so the stall does not matter.
	FlushRenderingCommands();
}

void ADirtBox::BuildDisplayMesh()
{
	const int32 N = FMath::Clamp(Settings.MeshVertsPerSide, 16, 2048);
	const float Size = Settings.RegionSizeCm();
	const float Half = Size * 0.5f;
	const float Step = Size / static_cast<float>(N - 1);

	// The mesh covers exactly what is simulated. When the sim is focused on part
	// of the box, the mesh follows it there.
	const float CentreX = static_cast<float>(Settings.SimRegionCentreCm.X);
	const float CentreY = static_cast<float>(Settings.SimRegionCentreCm.Y);

	TArray<FVector> Vertices;
	TArray<FVector2D> UVs;
	TArray<FVector> Normals;
	TArray<int32> Triangles;
	TArray<FProcMeshTangent> Tangents;
	TArray<FLinearColor> Colors;

	Vertices.Reserve(N * N);
	UVs.Reserve(N * N);
	Normals.Reserve(N * N);
	Tangents.Reserve(N * N);

	for (int32 Y = 0; Y < N; ++Y)
	{
		for (int32 X = 0; X < N; ++X)
		{
			Vertices.Add(FVector(CentreX - Half + X * Step, CentreY - Half + Y * Step, 0.0f));

			// UVs land on cell centres so the mesh samples the sim grid without
			// a half-cell shift.
			UVs.Add(FVector2D(static_cast<double>(X) / (N - 1), static_cast<double>(Y) / (N - 1)));

			// Placeholder. The material overrides this from the normal texture,
			// which is the only thing that knows the real surface shape.
			Normals.Add(FVector::UpVector);
			Tangents.Add(FProcMeshTangent(1.0f, 0.0f, 0.0f));
		}
	}

	Triangles.Reserve((N - 1) * (N - 1) * 6);
	for (int32 Y = 0; Y < N - 1; ++Y)
	{
		for (int32 X = 0; X < N - 1; ++X)
		{
			const int32 I0 = Y * N + X;
			const int32 I1 = I0 + 1;
			const int32 I2 = I0 + N;
			const int32 I3 = I2 + 1;

			Triangles.Add(I0); Triangles.Add(I2); Triangles.Add(I1);
			Triangles.Add(I1); Triangles.Add(I2); Triangles.Add(I3);
		}
	}

	GroundMesh->ClearAllMeshSections();
	GroundMesh->CreateMeshSection_LinearColor(0, Vertices, Triangles, Normals, UVs,
											  Colors, Tangents, /*bCreateCollision*/ false);

	// The grid is dead flat in local space, so its bounds have zero height — which
	// means the renderer would cull it as soon as the displaced ground filled the
	// screen from below or above. Section 1 is a single zero-area triangle spanning
	// the full height range the sim can produce. It draws nothing and exists only
	// to give the component honest bounds.
	{
		// Must cover the full height range the terrain can reach, or the renderer
		// culls the ground the moment it fills the screen. Track mode's natural site
		// runs to about -14 m in the low corners and +15 m on the high ground, and
		// borrow pits go 2.4 m below that again — so this is generous on purpose.
		const float MaxUpCm = 3000.0f;
		const float MaxDownCm = -2500.0f;

		TArray<FVector> BoundsVerts;
		BoundsVerts.Add(FVector(CentreX - Half, CentreY - Half, MaxDownCm));
		BoundsVerts.Add(FVector(CentreX + Half, CentreY + Half, MaxUpCm));
		BoundsVerts.Add(FVector(CentreX - Half, CentreY - Half, MaxDownCm));

		TArray<int32> BoundsTris = { 0, 1, 2 };
		TArray<FVector> BoundsNormals = { FVector::UpVector, FVector::UpVector, FVector::UpVector };
		TArray<FVector2D> BoundsUVs = { FVector2D::ZeroVector, FVector2D::ZeroVector, FVector2D::ZeroVector };
		TArray<FProcMeshTangent> BoundsTangents;
		TArray<FLinearColor> BoundsColors;

		GroundMesh->CreateMeshSection_LinearColor(1, BoundsVerts, BoundsTris, BoundsNormals,
												  BoundsUVs, BoundsColors, BoundsTangents, false);
	}

	UE_LOG(LogDirt, Log, TEXT("Display mesh: %d x %d verts (%.1f cm spacing), %d triangles, over %.1f m."),
		N, N, Step, (N - 1) * (N - 1) * 2, Size * 0.01f);
}

void ADirtBox::UpdateMaterialParameters()
{
	if (!GroundMaterial)
	{
		UE_LOG(LogDirt, Warning,
			TEXT("Dirtbox has no GroundMaterial assigned — the ground will render as ")
			TEXT("a flat grey plane. See docs/DirtboxSetup.md."));
		return;
	}

	if (!GroundMID)
	{
		GroundMID = UMaterialInstanceDynamic::Create(GroundMaterial, this);
		GroundMesh->SetMaterial(0, GroundMID);
		GroundMesh->SetMaterial(1, GroundMID);
	}

	GroundMID->SetTextureParameterValue(TEXT("DirtDisplay"), DisplayRT);
	GroundMID->SetTextureParameterValue(TEXT("DirtNormal"), NormalRT);
	GroundMID->SetTextureParameterValue(TEXT("DirtDebug"), DebugRT);
}

// ---------------------------------------------------------------------------
// Tick and stepping
// ---------------------------------------------------------------------------

void ADirtBox::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!bResourcesReady)
	{
		return;
	}

	// A test that asked to measure itself once the dirt has had time to settle.
	if (PendingMeasureSeconds > 0.0f)
	{
		PendingMeasureSeconds -= DeltaSeconds;
		if (PendingMeasureSeconds <= 0.0f)
		{
			PendingMeasureSeconds = -1.0f;

			const FDirtAudit A = RunAudit();
			UE_LOG(LogDirt, Log, TEXT("[%s] volume %.3f m3 (baseline %.3f, drift %+.4f m3 = %+.3f%%)"),
				*PendingMeasureLabel, A.VolumeM3, A.BaselineM3, A.DriftM3, A.DriftPercent);
			UE_LOG(LogDirt, Log, TEXT("[%s] max slope on loose dirt %.1f deg (repose setting %.1f), ")
				TEXT("max slope anywhere %.1f deg, cells scraped to bedrock %d"),
				*PendingMeasureLabel, A.MaxLooseSlopeDeg, Settings.LooseReposeDeg,
				A.MaxAnySlopeDeg, A.BedrockExposedCells);
		}
	}

	if (bNeedsReinit)
	{
		StepSimulation(true);
		bNeedsReinit = false;
		StepAccumulator = 0.0f;
		return;
	}

	if (bPaused)
	{
		// Paused stops the dirt settling, but sculpting still works — otherwise
		// you cannot set up a starting state to watch.
		if (PendingStrokes.Num() > 0)
		{
			StepSimulation(false);
		}
		return;
	}

	// Fixed timestep. This is what makes the test suite give the same answer at
	// 30 fps and at 120 fps.
	const float StepTime = 1.0f / FMath::Max(Settings.SimHz, 1.0f);
	StepAccumulator += DeltaSeconds;

	int32 Steps = 0;
	while (StepAccumulator >= StepTime && Steps < Settings.MaxStepsPerFrame)
	{
		StepAccumulator -= StepTime;
		StepSimulation(false);
		++Steps;
	}

	// Drop any backlog rather than letting one hitch snowball into a slideshow.
	if (StepAccumulator > StepTime * Settings.MaxStepsPerFrame)
	{
		StepAccumulator = 0.0f;
	}

	// After the step is enqueued, so a window reads this frame's surface.
	UpdateHeightWindows();
}

// ---------------------------------------------------------------------------
// Height windows
// ---------------------------------------------------------------------------

int32 ADirtBox::CreateHeightWindow(int32 SizeTexels)
{
	const int32 Size = FMath::Clamp(SizeTexels, 4, 256);
	Windows.Add(MakeShared<FDirtWindowSlot, ESPMode::ThreadSafe>(Size));
	return Windows.Num() - 1;
}

void ADirtBox::ReleaseHeightWindow(int32 Id)
{
	if (Windows.IsValidIndex(Id) && Windows[Id])
	{
		// Keep the slot so ids stay stable; the render side stops touching it.
		Windows[Id]->bReleased = true;
		Windows[Id]->Game.bValid = false;
	}
}

void ADirtBox::SetHeightWindowCentre(int32 Id, FVector2D WorldXYCm)
{
	if (!Windows.IsValidIndex(Id) || !Windows[Id] || Windows[Id]->bReleased)
	{
		return;
	}

	FDirtWindowSlot& Slot = *Windows[Id];
	const FVector2f Centre = WorldToTexel(WorldXYCm);
	const int32 Res = Settings.SimResolution;
	const int32 Max = FMath::Max(Res - Slot.Size, 0);

	FIntPoint Origin;
	Origin.X = FMath::Clamp(FMath::FloorToInt(Centre.X) - Slot.Size / 2, 0, Max);
	Origin.Y = FMath::Clamp(FMath::FloorToInt(Centre.Y) - Slot.Size / 2, 0, Max);

	FScopeLock L(&Slot.Lock);
	Slot.RequestedOrigin = Origin;
}

bool ADirtBox::SampleHeightWindow(int32 Id, FVector2D WorldXYCm, float& OutHeightCm, FVector& OutNormal,
								  FLinearColor* OutState) const
{
	if (!Windows.IsValidIndex(Id) || !Windows[Id] || Windows[Id]->bReleased)
	{
		return false;
	}

	const FDirtHeightWindow& W = Windows[Id]->Game;
	const int32 N = W.Size;
	if (!W.bValid || W.Data.Num() < N * N)
	{
		return false;
	}

	// Texel-centre space, relative to the window. Need one texel of margin for
	// the normal's central differences.
	const FVector2f T = WorldToTexel(WorldXYCm) - FVector2f(0.5f, 0.5f) - FVector2f(W.Origin.X, W.Origin.Y);
	if (T.X < 1.0f || T.Y < 1.0f || T.X > static_cast<float>(N - 3) || T.Y > static_cast<float>(N - 3))
	{
		return false;
	}

	const int32 X0 = FMath::FloorToInt(T.X);
	const int32 Y0 = FMath::FloorToInt(T.Y);
	const float FX = T.X - X0;
	const float FY = T.Y - Y0;

	const auto At = [&W, N](int32 X, int32 Y) -> const FLinearColor& { return W.Data[Y * N + X]; };

	const FLinearColor Bottom = FMath::Lerp(At(X0, Y0), At(X0 + 1, Y0), FX);
	const FLinearColor Top = FMath::Lerp(At(X0, Y0 + 1), At(X0 + 1, Y0 + 1), FX);
	const FLinearColor S = FMath::Lerp(Bottom, Top, FY);

	OutHeightCm = static_cast<float>(GetActorLocation().Z) + S.A;
	if (OutState)
	{
		*OutState = S;
	}

	// Normal from central differences around the nearest texel.
	const int32 XN = FMath::RoundToInt(T.X);
	const int32 YN = FMath::RoundToInt(T.Y);
	const float Texel = Settings.TexelSizeCm();
	const float DHDX = (At(XN + 1, YN).A - At(XN - 1, YN).A) / (2.0f * Texel);
	const float DHDY = (At(XN, YN + 1).A - At(XN, YN - 1).A) / (2.0f * Texel);
	OutNormal = FVector(-DHDX, -DHDY, 1.0).GetSafeNormal();

	return true;
}

void ADirtBox::UpdateHeightWindows()
{
	if (Windows.Num() == 0 || !DisplayRT)
	{
		return;
	}

	// 1. Pull anything the render thread has finished over to the game side.
	for (const TSharedPtr<FDirtWindowSlot, ESPMode::ThreadSafe>& Slot : Windows)
	{
		if (!Slot || Slot->bReleased)
		{
			continue;
		}
		FScopeLock L(&Slot->Lock);
		if (Slot->bReady)
		{
			Slot->Game.Origin = Slot->ReadyOrigin;
			Slot->Game.Size = Slot->Size;
			Swap(Slot->Game.Data, Slot->ReadyData);
			Slot->Game.bValid = true;
			Slot->bReady = false;
		}
	}

	// 2. Harvest and re-arm on the render thread.
	FTextureRenderTargetResource* Resource = DisplayRT->GameThread_GetRenderTargetResource();
	if (!Resource)
	{
		return;
	}

	TArray<TSharedPtr<FDirtWindowSlot, ESPMode::ThreadSafe>> Slots = Windows;

	ENQUEUE_RENDER_COMMAND(DirtHeightWindows)(
		[Slots, Resource](FRHICommandListImmediate& RHICmdList)
		{
			FRHITexture* Texture = Resource->GetRenderTargetTexture();
			if (!Texture)
			{
				return;
			}

			for (const TSharedPtr<FDirtWindowSlot, ESPMode::ThreadSafe>& Slot : Slots)
			{
				if (!Slot || Slot->bReleased)
				{
					continue;
				}
				const int32 N = Slot->Size;

				// Finished copies become ReadyData.
				for (int32 i = 0; i < 2; ++i)
				{
					if (!Slot->bPending[i] || !Slot->Readback[i]->IsReady())
					{
						continue;
					}

					int32 RowPitchPixels = 0;
					if (const void* Src = Slot->Readback[i]->Lock(RowPitchPixels))
					{
						TArray<FLinearColor> Copy;
						Copy.SetNumUninitialized(N * N);
						const FLinearColor* Rows = static_cast<const FLinearColor*>(Src);
						for (int32 Y = 0; Y < N; ++Y)
						{
							FMemory::Memcpy(Copy.GetData() + Y * N, Rows + Y * RowPitchPixels, N * sizeof(FLinearColor));
						}
						Slot->Readback[i]->Unlock();

						FScopeLock L(&Slot->Lock);
						Slot->ReadyData = MoveTemp(Copy);
						Slot->ReadyOrigin = Slot->PendingOrigin[i];
						Slot->bReady = true;
					}
					Slot->bPending[i] = false;
				}

				// Start the next copy in a free readback.
				for (int32 i = 0; i < 2; ++i)
				{
					if (Slot->bPending[i])
					{
						continue;
					}
					if (!Slot->Readback[i])
					{
						Slot->Readback[i] = MakeUnique<FRHIGPUTextureReadback>(TEXT("DirtHeightWindow"));
					}

					FIntPoint Origin;
					{
						FScopeLock L(&Slot->Lock);
						Origin = Slot->RequestedOrigin;
					}

					Slot->Readback[i]->EnqueueCopy(RHICmdList, Texture,
						FResolveRect(Origin.X, Origin.Y, Origin.X + N, Origin.Y + N));
					Slot->PendingOrigin[i] = Origin;
					Slot->bPending[i] = true;
					break;
				}
			}
		});
}

void ADirtBox::StepSimulation(bool bForceReinit)
{
	if (!StateA || !StateB || !DisplayRT || !NormalRT || !DebugRT || !BaseHeightTex || !InitialStateTex)
	{
		return;
	}

	// Resource pointers are safe to hand across from the game thread because the
	// Dirtbox creates them once and never resizes them. The actual RHI textures
	// are pulled out on the render thread, where they belong.
	FTextureResource* BaseHeightRes = BaseHeightTex->GetResource();
	FTextureResource* InitialStateRes = InitialStateTex->GetResource();
	FTextureRenderTargetResource* StateARes = StateA->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* StateBRes = StateB->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* DisplayRes = DisplayRT->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* NormalRes = NormalRT->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* DebugRes = DebugRT->GameThread_GetRenderTargetResource();

	if (!BaseHeightRes || !InitialStateRes || !StateARes || !StateBRes || !DisplayRes || !NormalRes || !DebugRes)
	{
		return;
	}

	FDirtSimFrame Frame;
	Frame.Resolution = FIntPoint(Settings.SimResolution, Settings.SimResolution);
	Frame.TexelSizeCm = Settings.TexelSizeCm();
	Frame.LooseReposeDeg = Settings.LooseReposeDeg;
	Frame.PackedReposeDeg = Settings.PackedReposeDeg;
	Frame.SuctionCohesionKPa = Settings.SuctionCohesionKPa;
	Frame.PackedCohesionKPa = Settings.PackedCohesionKPa;
	Frame.UnitWeightKNm3 = Settings.UnitWeightKNm3;
	Frame.SaturationFrictionLoss = Settings.SaturationFrictionLoss;
	Frame.MaxCohesiveHeightCm = Settings.MaxCohesiveHeightCm;
	Frame.SlumpRate = Settings.SlumpRate;
	Frame.LooseningRate = Settings.LooseningRate;
	Frame.LooseningScaleCm = Settings.LooseningScaleCm;
	Frame.SlumpIterations = bPaused ? 0 : Settings.SlumpIterations;
	Frame.DebugMode = static_cast<int32>(DebugView);
	Frame.DebugLayerRangeCm = DebugLayerRangeCm;
	Frame.bReinitialise = bForceReinit;
	Frame.Strokes = MoveTemp(PendingStrokes);
	PendingStrokes.Reset();

	ENQUEUE_RENDER_COMMAND(DirtSimStep)(
		[Frame, BaseHeightRes, InitialStateRes, StateARes, StateBRes, DisplayRes, NormalRes, DebugRes]
		(FRHICommandListImmediate& RHICmdList) mutable
		{
			Frame.BaseHeight = BaseHeightRes->TextureRHI;
			Frame.InitialState = InitialStateRes->TextureRHI;
			Frame.StateA = StateARes->GetRenderTargetTexture();
			Frame.StateB = StateBRes->GetRenderTargetTexture();
			Frame.Display = DisplayRes->GetRenderTargetTexture();
			Frame.NormalOut = NormalRes->GetRenderTargetTexture();
			Frame.DebugOut = DebugRes->GetRenderTargetTexture();

			if (Frame.IsValid())
			{
				DirtSim::Execute_RenderThread(RHICmdList, Frame);
			}
		});
}

// ---------------------------------------------------------------------------
// Deformation
// ---------------------------------------------------------------------------

FVector2f ADirtBox::WorldToTexel(FVector2D WorldXYCm) const
{
	const FVector Origin = GetActorLocation();
	const float Region = Settings.RegionSizeCm();
	const float Half = Region * 0.5f;
	const float CellsPerCm = static_cast<float>(Settings.SimResolution) / Region;

	// Relative to the region's centre, not the box's — when the sim is focused
	// somewhere, brush coordinates have to land in the same place.
	const float LocalX = static_cast<float>(WorldXYCm.X - Origin.X - Settings.SimRegionCentreCm.X);
	const float LocalY = static_cast<float>(WorldXYCm.Y - Origin.Y - Settings.SimRegionCentreCm.Y);

	return FVector2f((LocalX + Half) * CellsPerCm, (LocalY + Half) * CellsPerCm);
}

FDirtBrushStroke ADirtBox::MakeStroke(FVector2D WorldXYCm, float RadiusCm, float Amount, EDirtBrushMode Mode,
									  float DisturbOverride) const
{
	const float TexelSize = Settings.TexelSizeCm();

	FDirtBrushStroke S;
	S.Mode = static_cast<int32>(Mode);
	S.CenterTexel = WorldToTexel(WorldXYCm);
	S.CoreRadiusTexels = FMath::Max(RadiusCm / TexelSize, 1.0f);
	S.RimRadiusTexels = S.CoreRadiusTexels * FMath::Max(Settings.BrushRimScale, 1.05f);
	// A hand tool breaks the dirt up; a rolling tyre pressing a rut does not.
	S.Disturb = (DisturbOverride >= 0.0f) ? DisturbOverride : Settings.BrushDisturb;

	// Sum both kernels over exactly the cells the shader will touch, at exactly
	// the sub-cell offsets the shader will use, skipping the cells the shader
	// skips at the box walls. Matching it this precisely is what makes dig and
	// raise zero-sum to the cell rather than merely approximately zero-sum.
	const int32 CentreX = FMath::FloorToInt(S.CenterTexel.X);
	const int32 CentreY = FMath::FloorToInt(S.CenterTexel.Y);
	const int32 Reach = FMath::CeilToInt(S.RimRadiusTexels) + 1;
	const int32 Res = Settings.SimResolution;

	double CoreSum = 0.0;
	double RimSum = 0.0;

	for (int32 DY = -Reach; DY <= Reach; ++DY)
	{
		for (int32 DX = -Reach; DX <= Reach; ++DX)
		{
			const int32 CX = CentreX + DX;
			const int32 CY = CentreY + DY;
			if (CX < 0 || CY < 0 || CX >= Res || CY >= Res)
			{
				continue;
			}

			const float PX = static_cast<float>(CX) + 0.5f - S.CenterTexel.X;
			const float PY = static_cast<float>(CY) + 0.5f - S.CenterTexel.Y;
			const float R = FMath::Sqrt(PX * PX + PY * PY);

			CoreSum += DirtBrushCoreWeight(R, S.CoreRadiusTexels);
			RimSum += DirtBrushRimWeight(R, S.CoreRadiusTexels, S.RimRadiusTexels);
		}
	}

	S.CoreNorm = FMath::Max(static_cast<float>(CoreSum), KINDA_SMALL_NUMBER);
	S.RimNorm = FMath::Max(static_cast<float>(RimSum), KINDA_SMALL_NUMBER);

	if (Mode == EDirtBrushMode::Dig || Mode == EDirtBrushMode::Raise)
	{
		// The core kernel is 1.0 at its centre, so scaling by CoreNorm makes
		// Amount read as "peak depth in cm" at the middle of the stroke.
		S.Amount = Amount * S.CoreNorm;
	}
	else
	{
		S.Amount = Amount;
	}

	return S;
}

void ADirtBox::ApplyBrush(FVector2D WorldXYCm, float RadiusCm, float Amount, EDirtBrushMode Mode, float DisturbOverride)
{
	if (!bResourcesReady || RadiusCm <= 0.0f)
	{
		return;
	}

	PendingStrokes.Add(MakeStroke(WorldXYCm, RadiusCm, Amount, Mode, DisturbOverride));
}

void ADirtBox::TransferDirt(FVector2D FromWorldXYCm, float FromRadiusCm, FVector2D ToWorldXYCm, float ToRadiusCm, float VolumeCm3)
{
	if (!bResourcesReady || VolumeCm3 <= 0.0f || FromRadiusCm <= 0.0f || ToRadiusCm <= 0.0f)
	{
		return;
	}

	// Scoop/Dump amounts are layer-height sums in cm x texel^2: the shader adds
	// Amount * CoreW per texel and the core weights sum to exactly 1.
	const float Amount = VolumeCm3 / Settings.TexelAreaCm2();
	PendingStrokes.Add(MakeStroke(FromWorldXYCm, FromRadiusCm, Amount, EDirtBrushMode::Scoop));
	PendingStrokes.Add(MakeStroke(ToWorldXYCm, ToRadiusCm, Amount, EDirtBrushMode::Dump));
}

void ADirtBox::ResetToTestbed()
{
	if (!bResourcesReady)
	{
		return;
	}

	PendingStrokes.Reset();
	BuildTerrainAndUpload();
	bNeedsReinit = true;

	UE_LOG(LogDirt, Log, TEXT("Dirtbox reset. Baseline volume %.3f m3."), BaselineVolumeM3);
}

void ADirtBox::RebuildTerrainAndMesh()
{
	PendingStrokes.Reset();
	Readback.Empty();

	// The simulation textures keep their resolution; only the ground they cover
	// and the mesh that displays it change.
	BuildTerrainAndUpload();
	BuildDisplayMesh();
	UpdateMaterialParameters();
	bNeedsReinit = true;
}

void ADirtBox::SetTerrainMode(EDirtTerrainMode NewMode)
{
	if (!bResourcesReady)
	{
		TerrainMode = NewMode;
		return;
	}

	TerrainMode = NewMode;
	ApplyModeDefaults();
	RebuildTerrainAndMesh();

	for (const FString& Line : FeatureLog)
	{
		UE_LOG(LogDirt, Log, TEXT("  %s"), *Line);
	}
}

void ADirtBox::SetSimRegion(FVector2D CentreCm, float SizeCm)
{
	const float Half = Settings.WorldSizeCm * 0.5f;
	const float Clamped = (SizeCm > 0.0f) ? FMath::Clamp(SizeCm, 200.0f, Settings.WorldSizeCm) : 0.0f;

	// Keep the region inside the box, or the generators would be asked to build
	// ground that does not exist.
	const float Margin = (Clamped > 0.0f) ? Half - Clamped * 0.5f : 0.0f;
	Settings.SimRegionCentreCm.X = FMath::Clamp(CentreCm.X, -Margin, Margin);
	Settings.SimRegionCentreCm.Y = FMath::Clamp(CentreCm.Y, -Margin, Margin);
	Settings.SimRegionSizeCm = Clamped;

	if (!bResourcesReady)
	{
		return;
	}

	RebuildTerrainAndMesh();

	UE_LOG(LogDirt, Log, TEXT("Sim region: %.1f m square centred on (%.0f, %.0f) m -> %.2f cm per cell."),
		Settings.RegionSizeCm() * 0.01f,
		Settings.SimRegionCentreCm.X * 0.01, Settings.SimRegionCentreCm.Y * 0.01,
		Settings.TexelSizeCm());
}

void ADirtBox::GetSuggestedViewpoint(FVector& OutLocation, FRotator& OutRotation) const
{
	const FVector Origin = GetActorLocation();
	const float HalfCm = Settings.RegionSizeCm() * 0.5f;
	const FVector Centre = Origin + FVector(Settings.SimRegionCentreCm.X, Settings.SimRegionCentreCm.Y, 0.0);

	// Stand back, high enough to take in whatever is currently simulated.
	OutLocation = Centre + FVector(-HalfCm * 1.35f, 0.0f, HalfCm * 0.75f);
	OutRotation = FRotator(-26.0f, 0.0f, 0.0f);
}

// ---------------------------------------------------------------------------
// Queries and audit
// ---------------------------------------------------------------------------

void ADirtBox::RefreshReadback()
{
	if (!DisplayRT)
	{
		return;
	}

	FTextureRenderTargetResource* Resource = DisplayRT->GameThread_GetRenderTargetResource();
	if (!Resource)
	{
		return;
	}

	// Blocking GPU-to-CPU read of the whole grid. At 1024 that is 16 MB and a
	// visible hitch, which is why nothing calls this per frame — only audits,
	// height probes and tests.
	Readback.Reset();
	Resource->ReadLinearColorPixels(Readback);
}

FDirtAudit ADirtBox::RunAudit()
{
	RefreshReadback();

	FDirtAudit Audit;
	Audit.BaselineM3 = BaselineVolumeM3;

	const int32 Res = Settings.SimResolution;
	if (Readback.Num() < Res * Res)
	{
		UE_LOG(LogDirt, Warning, TEXT("Audit could not read the dirt state back from the GPU."));
		return Audit;
	}

	double SumCm = 0.0;
	float MinLayer = MAX_flt;
	float MaxLayer = -MAX_flt;
	int32 Exposed = 0;

	for (int32 i = 0; i < Res * Res; ++i)
	{
		const float Layer = Readback[i].R;
		SumCm += Layer;
		MinLayer = FMath::Min(MinLayer, Layer);
		MaxLayer = FMath::Max(MaxLayer, Layer);
		if (Layer < 1.0f)
		{
			++Exposed;
		}
	}

	Audit.VolumeM3 = SumCm * Settings.TexelAreaCm2() / 1000000.0;
	Audit.DriftM3 = Audit.VolumeM3 - Audit.BaselineM3;
	Audit.DriftPercent = (Audit.BaselineM3 > 0.0) ? 100.0 * Audit.DriftM3 / Audit.BaselineM3 : 0.0;
	Audit.MinLayerCm = MinLayer;
	Audit.MaxLayerCm = MaxLayer;
	Audit.BedrockExposedCells = Exposed;

	// Steepest cell-to-cell slope, measured the same way the slump pass measures
	// it — over all eight neighbours, with diagonals sqrt(2) cells away — so this
	// checks the invariant the sim actually enforces rather than a different one.
	const float TexelSize = Settings.TexelSizeCm();
	const int32 Offsets[8] = { 1, -1, Res, -Res, Res + 1, -Res - 1, Res - 1, -Res + 1 };
	const float Dists[8] = { 1.0f, 1.0f, 1.0f, 1.0f, UE_SQRT_2, UE_SQRT_2, UE_SQRT_2, UE_SQRT_2 };

	for (int32 Y = 1; Y < Res - 1; ++Y)
	{
		for (int32 X = 1; X < Res - 1; ++X)
		{
			const int32 I = Y * Res + X;
			const float Surface = Readback[I].A;

			float MaxTan = 0.0f;
			for (int32 N = 0; N < 8; ++N)
			{
				MaxTan = FMath::Max(MaxTan, (Surface - Readback[I + Offsets[N]].A) / (TexelSize * Dists[N]));
			}

			const float SlopeDeg = FMath::RadiansToDegrees(FMath::Atan(MaxTan));
			Audit.MaxAnySlopeDeg = FMath::Max(Audit.MaxAnySlopeDeg, SlopeDeg);

			// Only dirt that is both genuinely loose and genuinely present tells
			// us anything about the loose angle of repose.
			const float Compaction = Readback[I].G;
			const float Layer = Readback[I].R;
			if (Compaction < 0.15f && Layer > 2.0f)
			{
				Audit.MaxLooseSlopeDeg = FMath::Max(Audit.MaxLooseSlopeDeg, SlopeDeg);
			}
		}
	}

	return Audit;
}

bool ADirtBox::IsInsideBox(FVector2D WorldXYCm) const
{
	const FVector2f T = WorldToTexel(WorldXYCm);
	return T.X >= 0.0f && T.Y >= 0.0f
		&& T.X <= static_cast<float>(Settings.SimResolution)
		&& T.Y <= static_cast<float>(Settings.SimResolution);
}

float ADirtBox::GetSurfaceHeightAtWorld(FVector2D WorldXYCm) const
{
	const int32 Res = Settings.SimResolution;
	if (Readback.Num() < Res * Res)
	{
		return GetActorLocation().Z;
	}

	const FVector2f T = WorldToTexel(WorldXYCm) - FVector2f(0.5f, 0.5f);

	const int32 X0 = FMath::Clamp(FMath::FloorToInt(T.X), 0, Res - 1);
	const int32 Y0 = FMath::Clamp(FMath::FloorToInt(T.Y), 0, Res - 1);
	const int32 X1 = FMath::Min(X0 + 1, Res - 1);
	const int32 Y1 = FMath::Min(Y0 + 1, Res - 1);

	const float FX = FMath::Clamp(T.X - static_cast<float>(X0), 0.0f, 1.0f);
	const float FY = FMath::Clamp(T.Y - static_cast<float>(Y0), 0.0f, 1.0f);

	const float H00 = Readback[Y0 * Res + X0].A;
	const float H10 = Readback[Y0 * Res + X1].A;
	const float H01 = Readback[Y1 * Res + X0].A;
	const float H11 = Readback[Y1 * Res + X1].A;

	const float Bottom = FMath::Lerp(H00, H10, FX);
	const float Top = FMath::Lerp(H01, H11, FX);

	return GetActorLocation().Z + FMath::Lerp(Bottom, Top, FY);
}

// ---------------------------------------------------------------------------
// Scripted tests
// ---------------------------------------------------------------------------
// Every test resets to the testbed first, so results are repeatable. Combined
// with the fixed simulation timestep, running the same test twice gives the same
// numbers — which is the only way tuning dirt can be anything but guesswork.

bool ADirtBox::RunTest(const FString& TestName, FString& OutMessage)
{
	const FVector Origin = GetActorLocation();

	// Tests are written in metres relative to the box centre, matching the
	// testbed layout. This converts to the world centimetres the brushes want.
	const auto At = [Origin](float Xm, float Ym)
	{
		return FVector2D(Origin.X + Xm * 100.0, Origin.Y + Ym * 100.0);
	};

	const FString Name = TestName.ToLower();

	if (Name == TEXT("list"))
	{
		OutMessage = TEXT("repose, anglefan, conserve, trench");
		return true;
	}

	if (Name == TEXT("repose"))
	{
		// The testbed already contains a 3 m cone of bone-dry loose dirt at a far
		// steeper angle than dry dirt can stand. Reset, wait for it to collapse,
		// then measure what angle it actually settled at.
		ResetToTestbed();
		PendingMeasureSeconds = 3.0f;
		PendingMeasureLabel = TEXT("repose");

		OutMessage = FString::Printf(
			TEXT("Watch the cone at (3, -45) collapse. Measuring in 3 s; expect the ")
			TEXT("max loose slope to land near %.0f deg."), Settings.LooseReposeDeg);
		return true;
	}

	if (Name == TEXT("anglefan"))
	{
		ResetToTestbed();

		TArray<FDirtAngleWedge> Wedges;
		FDirtTestbed::GetAngleSpectrum(Settings, Wedges);

		// Pile loose dirt halfway up each face. Faces shallower than the loose
		// repose angle keep their pile; steeper ones shed it to the bottom.
		for (const FDirtAngleWedge& W : Wedges)
		{
			const FVector2D P = At(W.FaceMidX, W.CentreY);
			ApplyBrush(P, 250.0f, 1.0f, EDirtBrushMode::Loosen);
			ApplyBrush(P, 150.0f, 50.0f, EDirtBrushMode::Raise);
			ApplyBrush(P, 250.0f, 1.0f, EDirtBrushMode::Loosen);
		}

		PendingMeasureSeconds = 4.0f;
		PendingMeasureLabel = TEXT("anglefan");

		OutMessage = FString::Printf(
			TEXT("Dropped loose piles on %d faces (%.0f to %.0f deg). Switch to ")
			TEXT("DaDirt.DebugView 4 to see stability. Faces under %.0f deg should hold."),
			Wedges.Num(), Wedges[0].AngleDeg, Wedges.Last().AngleDeg, Settings.LooseReposeDeg);
		return true;
	}

	if (Name == TEXT("conserve"))
	{
		ResetToTestbed();

		// Forty holes scattered over the flat calibration pad, all shallow enough
		// not to reach bedrock — so nothing can legitimately vanish and the audit
		// drift should come out at essentially zero.
		FRandomStream Rand(1337);
		for (int32 i = 0; i < 40; ++i)
		{
			const float Xm = Rand.FRandRange(-4.0f, 10.0f);
			const float Ym = Rand.FRandRange(-28.0f, 28.0f);
			const float RadiusCm = Rand.FRandRange(100.0f, 300.0f);
			const float DepthCm = Rand.FRandRange(5.0f, 25.0f);
			ApplyBrush(At(Xm, Ym), RadiusCm, DepthCm, EDirtBrushMode::Dig);
		}

		PendingMeasureSeconds = 2.5f;
		PendingMeasureLabel = TEXT("conserve");

		OutMessage = TEXT("Dug 40 shallow holes on the calibration pad. Auditing in 2.5 s; ")
					 TEXT("drift should be near 0.000 m3.");
		return true;
	}

	if (Name == TEXT("trench"))
	{
		ResetToTestbed();

		// A straight line of overlapping digs down the pad — the hand-tool stand-in
		// for what a spinning tire will carve in Phase 1e.
		for (int32 i = 0; i < 30; ++i)
		{
			const float Ym = FMath::Lerp(-20.0f, 20.0f, static_cast<float>(i) / 29.0f);
			ApplyBrush(At(3.0f, Ym), 60.0f, 12.0f, EDirtBrushMode::Dig);
		}

		PendingMeasureSeconds = 2.5f;
		PendingMeasureLabel = TEXT("trench");

		OutMessage = TEXT("Cut a 40 m trench down the pad. The spoil should sit in ridges ")
					 TEXT("along both sides and slump back toward repose.");
		return true;
	}

	return false;
}

// ---------------------------------------------------------------------------
// Console commands
// ---------------------------------------------------------------------------
// Driving the sandbox from the console rather than the mouse is deliberate for
// now: it costs no input assets, and it makes every interaction a repeatable
// scripted step. Mouse tools arrive with Phase 1d.
//
// All positions are in METRES relative to the centre of the box, so (0,0) is the
// middle and the corners are (-64,-64) to (64,64) in a 128 m box.

namespace
{
	ADirtBox* GetDirtBoxOrWarn()
	{
		ADirtBox* Box = ADirtBox::GetActive();
		if (!Box)
		{
			UE_LOG(LogDirt, Warning,
				TEXT("No active Dirtbox. Is a DirtBox actor in the level and is the game running?"));
		}
		return Box;
	}

	float ArgFloat(const TArray<FString>& Args, int32 Index, float Default)
	{
		return Args.IsValidIndex(Index) ? FCString::Atof(*Args[Index]) : Default;
	}

	int32 ArgInt(const TArray<FString>& Args, int32 Index, int32 Default)
	{
		return Args.IsValidIndex(Index) ? FCString::Atoi(*Args[Index]) : Default;
	}

	/** Shared body for every brush command. */
	void RunBrushCommand(const TArray<FString>& Args, EDirtBrushMode Mode,
						 float DefaultRadiusCm, float DefaultAmount, const TCHAR* AmountUnits)
	{
		ADirtBox* Box = GetDirtBoxOrWarn();
		if (!Box)
		{
			return;
		}

		if (Args.Num() < 2)
		{
			UE_LOG(LogDirt, Warning, TEXT("Need X and Y in metres. Radius defaults to %.0f cm, ")
				TEXT("amount to %.2f %s."), DefaultRadiusCm, DefaultAmount, AmountUnits);
			return;
		}

		const float Xm = ArgFloat(Args, 0, 0.0f);
		const float Ym = ArgFloat(Args, 1, 0.0f);
		const float RadiusCm = ArgFloat(Args, 2, DefaultRadiusCm);
		const float Amount = ArgFloat(Args, 3, DefaultAmount);

		const FVector Origin = Box->GetActorLocation();
		const FVector2D World(Origin.X + Xm * 100.0, Origin.Y + Ym * 100.0);

		if (!Box->IsInsideBox(World))
		{
			UE_LOG(LogDirt, Warning, TEXT("(%.1f, %.1f) m is outside the box."), Xm, Ym);
			return;
		}

		Box->ApplyBrush(World, RadiusCm, Amount, Mode);
		UE_LOG(LogDirt, Log, TEXT("Brush at (%.1f, %.1f) m, radius %.0f cm, amount %.2f %s."),
			Xm, Ym, RadiusCm, Amount, AmountUnits);
	}
}

static FAutoConsoleCommandWithWorldAndArgs GDirtDigCmd(
	TEXT("DaDirt.Dig"),
	TEXT("DaDirt.Dig <Xm> <Ym> [RadiusCm=150] [DepthCm=15] - scoop dirt out; the spoil heaps on the rim."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld*)
		{
			RunBrushCommand(Args, EDirtBrushMode::Dig, 150.0f, 15.0f, TEXT("cm deep"));
		}));

static FAutoConsoleCommandWithWorldAndArgs GDirtRaiseCmd(
	TEXT("DaDirt.Raise"),
	TEXT("DaDirt.Raise <Xm> <Ym> [RadiusCm=150] [HeightCm=15] - pile dirt up, borrowed from the rim."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld*)
		{
			RunBrushCommand(Args, EDirtBrushMode::Raise, 150.0f, 15.0f, TEXT("cm high"));
		}));

static FAutoConsoleCommandWithWorldAndArgs GDirtSmoothCmd(
	TEXT("DaDirt.Smooth"),
	TEXT("DaDirt.Smooth <Xm> <Ym> [RadiusCm=200] [Strength=0.5] - flatten the surface."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld*)
		{
			RunBrushCommand(Args, EDirtBrushMode::Smooth, 200.0f, 0.5f, TEXT("strength"));
		}));

static FAutoConsoleCommandWithWorldAndArgs GDirtWetCmd(
	TEXT("DaDirt.Wet"),
	TEXT("DaDirt.Wet <Xm> <Ym> [RadiusCm=300] [Amount=0.5] - add moisture; damp dirt holds steeper."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld*)
		{
			RunBrushCommand(Args, EDirtBrushMode::Wet, 300.0f, 0.5f, TEXT("moisture"));
		}));

static FAutoConsoleCommandWithWorldAndArgs GDirtPackCmd(
	TEXT("DaDirt.Pack"),
	TEXT("DaDirt.Pack <Xm> <Ym> [RadiusCm=300] [Amount=0.5] - compact the dirt so it holds steeper."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld*)
		{
			RunBrushCommand(Args, EDirtBrushMode::Pack, 300.0f, 0.5f, TEXT("compaction"));
		}));

static FAutoConsoleCommandWithWorldAndArgs GDirtLoosenCmd(
	TEXT("DaDirt.Loosen"),
	TEXT("DaDirt.Loosen <Xm> <Ym> [RadiusCm=300] [Amount=0.5] - break up hardpack."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld*)
		{
			RunBrushCommand(Args, EDirtBrushMode::Loosen, 300.0f, 0.5f, TEXT("looseness"));
		}));

static FAutoConsoleCommandWithWorldAndArgs GDirtDebugViewCmd(
	TEXT("DaDirt.DebugView"),
	TEXT("DaDirt.DebugView <0-6> - 0 dirt, 1 layer depth, 2 compaction, 3 moisture, ")
	TEXT("4 stability vs repose, 5 bedrock exposure, 6 slope angle."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld*)
		{
			ADirtBox* Box = GetDirtBoxOrWarn();
			if (!Box)
			{
				return;
			}

			static const TCHAR* Names[] = { TEXT("dirt"), TEXT("layer depth"), TEXT("compaction"),
											TEXT("moisture"), TEXT("stability"), TEXT("bedrock"),
											TEXT("slope angle") };

			const int32 Mode = FMath::Clamp(ArgInt(Args, 0, 0), 0, 6);
			Box->DebugView = static_cast<EDirtDebugView>(Mode);
			UE_LOG(LogDirt, Log, TEXT("Debug view %d (%s)."), Mode, Names[Mode]);
		}));

static FAutoConsoleCommandWithWorldAndArgs GDirtAuditCmd(
	TEXT("DaDirt.Audit"),
	TEXT("DaDirt.Audit - volume conservation check. Reads the grid back from the GPU, so it hitches."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>&, UWorld*)
		{
			ADirtBox* Box = GetDirtBoxOrWarn();
			if (!Box)
			{
				return;
			}

			const FDirtAudit A = Box->RunAudit();
			UE_LOG(LogDirt, Log, TEXT("--- volume audit ---"));
			UE_LOG(LogDirt, Log, TEXT("  dirt in the box   %.4f m3"), A.VolumeM3);
			UE_LOG(LogDirt, Log, TEXT("  baseline          %.4f m3"), A.BaselineM3);
			UE_LOG(LogDirt, Log, TEXT("  drift             %+.5f m3  (%+.4f %%)"), A.DriftM3, A.DriftPercent);
			UE_LOG(LogDirt, Log, TEXT("  layer min / max   %.1f / %.1f cm"), A.MinLayerCm, A.MaxLayerCm);
			UE_LOG(LogDirt, Log, TEXT("  scraped to rock   %d cells"), A.BedrockExposedCells);
			UE_LOG(LogDirt, Log, TEXT("  steepest loose    %.1f deg  (repose setting %.1f)"),
				A.MaxLooseSlopeDeg, Box->Settings.LooseReposeDeg);
			UE_LOG(LogDirt, Log, TEXT("  steepest anywhere %.1f deg"), A.MaxAnySlopeDeg);
		}));

static FAutoConsoleCommandWithWorldAndArgs GDirtProbeCmd(
	TEXT("DaDirt.Probe"),
	TEXT("DaDirt.Probe <Xm> <Ym> - surface height at a point, in cm."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld*)
		{
			ADirtBox* Box = GetDirtBoxOrWarn();
			if (!Box)
			{
				return;
			}

			const float Xm = ArgFloat(Args, 0, 0.0f);
			const float Ym = ArgFloat(Args, 1, 0.0f);
			const FVector Origin = Box->GetActorLocation();
			const FVector2D World(Origin.X + Xm * 100.0, Origin.Y + Ym * 100.0);

			Box->RefreshReadback();
			UE_LOG(LogDirt, Log, TEXT("Surface at (%.1f, %.1f) m is Z = %.1f cm."),
				Xm, Ym, Box->GetSurfaceHeightAtWorld(World));
		}));

static FAutoConsoleCommandWithWorldAndArgs GDirtResetCmd(
	TEXT("DaDirt.Reset"),
	TEXT("DaDirt.Reset - rebuild the testbed and discard everything dug."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>&, UWorld*)
		{
			if (ADirtBox* Box = GetDirtBoxOrWarn())
			{
				Box->ResetToTestbed();
			}
		}));

static FAutoConsoleCommandWithWorldAndArgs GDirtPauseCmd(
	TEXT("DaDirt.Pause"),
	TEXT("DaDirt.Pause [0|1] - freeze slumping. Brushes still work while paused."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld*)
		{
			ADirtBox* Box = GetDirtBoxOrWarn();
			if (!Box)
			{
				return;
			}

			Box->bPaused = Args.IsValidIndex(0) ? (ArgInt(Args, 0, 1) != 0) : !Box->bPaused;
			UE_LOG(LogDirt, Log, TEXT("Dirt sim %s."), Box->bPaused ? TEXT("paused") : TEXT("running"));
		}));

static FAutoConsoleCommandWithWorldAndArgs GDirtSlumpCmd(
	TEXT("DaDirt.Slump"),
	TEXT("DaDirt.Slump <iterations> - slump passes per sim step. 0 freezes settling, 3 is the default."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld*)
		{
			ADirtBox* Box = GetDirtBoxOrWarn();
			if (!Box)
			{
				return;
			}

			Box->Settings.SlumpIterations = FMath::Clamp(ArgInt(Args, 0, 3), 0, 16);
			UE_LOG(LogDirt, Log, TEXT("Slump iterations %d."), Box->Settings.SlumpIterations);
		}));

static FAutoConsoleCommandWithWorldAndArgs GDirtReposeCmd(
	TEXT("DaDirt.Repose"),
	TEXT("DaDirt.Repose <looseDeg> [denseDeg] [suctionKPa] [packedKPa] - soil strength: friction angles loose/dense, ")
	TEXT("cohesion from moisture and from packing. See docs/SoilPhysics.md."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld*)
		{
			ADirtBox* Box = GetDirtBoxOrWarn();
			if (!Box)
			{
				return;
			}

			FDirtSimSettings& S = Box->Settings;
			S.LooseReposeDeg = FMath::Clamp(ArgFloat(Args, 0, S.LooseReposeDeg), 1.0f, 89.0f);
			S.PackedReposeDeg = FMath::Clamp(ArgFloat(Args, 1, S.PackedReposeDeg), 1.0f, 60.0f);
			S.SuctionCohesionKPa = FMath::Clamp(ArgFloat(Args, 2, S.SuctionCohesionKPa), 0.0f, 50.0f);
			S.PackedCohesionKPa = FMath::Clamp(ArgFloat(Args, 3, S.PackedCohesionKPa), 0.0f, 100.0f);

			const float MudDeg = FMath::RadiansToDegrees(FMath::Atan(
				FMath::Tan(FMath::DegreesToRadians(S.LooseReposeDeg)) * (1.0f - S.SaturationFrictionLoss)));
			const float DampWallCm = 400.0f * S.SuctionCohesionKPa / S.UnitWeightKNm3
				* FMath::Tan(FMath::DegreesToRadians(45.0f + 0.5f * S.LooseReposeDeg));
			UE_LOG(LogDirt, Log, TEXT("Soil: friction %.0f deg loose / %.0f dense, mud %.0f deg; cohesion %.1f kPa damp, ")
				TEXT("%.1f kPa packed; a damp vertical wall stands to %.0f cm."),
				S.LooseReposeDeg, S.PackedReposeDeg, MudDeg, S.SuctionCohesionKPa, S.PackedCohesionKPa, DampWallCm);
		}));

static FAutoConsoleCommandWithWorldAndArgs GDirtTestCmd(
	TEXT("DaDirt.Test"),
	TEXT("DaDirt.Test <repose|anglefan|conserve|trench|list> - run a scripted, repeatable test."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld*)
		{
			ADirtBox* Box = GetDirtBoxOrWarn();
			if (!Box)
			{
				return;
			}

			const FString Name = Args.IsValidIndex(0) ? Args[0] : TEXT("list");

			FString Message;
			if (Box->RunTest(Name, Message))
			{
				UE_LOG(LogDirt, Log, TEXT("[%s] %s"), *Name, *Message);
			}
			else
			{
				UE_LOG(LogDirt, Warning, TEXT("Unknown test '%s'. Try: repose, anglefan, conserve, trench."), *Name);
			}
		}));

static FAutoConsoleCommandWithWorldAndArgs GDirtModeCmd(
	TEXT("DaDirt.Mode"),
	TEXT("DaDirt.Mode <testbed|track> - testbed is the 128 m measuring rig, track is a real 1.5 km circuit."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld*)
		{
			ADirtBox* Box = GetDirtBoxOrWarn();
			if (!Box)
			{
				return;
			}

			const FString Want = Args.IsValidIndex(0) ? Args[0].ToLower() : FString();
			if (Want == TEXT("track"))
			{
				Box->SetTerrainMode(EDirtTerrainMode::Track);
			}
			else if (Want == TEXT("testbed"))
			{
				Box->SetTerrainMode(EDirtTerrainMode::Testbed);
			}
			else
			{
				UE_LOG(LogDirt, Warning, TEXT("Usage: DaDirt.Mode testbed | track"));
				return;
			}

			UE_LOG(LogDirt, Log, TEXT("Rebuilt as %s. Fly to a viewpoint with DaDirt.View."), *Want);
		}));

static FAutoConsoleCommandWithWorldAndArgs GDirtFocusCmd(
	TEXT("DaDirt.Focus"),
	TEXT("DaDirt.Focus <xM> <yM> [sizeM=51.2] | off - point the simulation at part of ")
	TEXT("the box. Same 1024 cells over less ground means finer cells: 51 m gives 5 cm ")
	TEXT("cells, which is where ruts start to look like ruts."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld*)
		{
			ADirtBox* Box = GetDirtBoxOrWarn();
			if (!Box)
			{
				return;
			}

			if (Args.IsValidIndex(0) && Args[0].ToLower() == TEXT("off"))
			{
				Box->SetSimRegion(FVector2D::ZeroVector, 0.0f);
				UE_LOG(LogDirt, Log, TEXT("Simulating the whole box again."));
				return;
			}

			if (Args.Num() < 2)
			{
				UE_LOG(LogDirt, Warning, TEXT("Usage: DaDirt.Focus <xM> <yM> [sizeM] | off"));
				return;
			}

			const float Xm = ArgFloat(Args, 0, 0.0f);
			const float Ym = ArgFloat(Args, 1, 0.0f);
			const float SizeM = ArgFloat(Args, 2, 51.2f);

			Box->SetSimRegion(FVector2D(Xm * 100.0f, Ym * 100.0f), SizeM * 100.0f);
			UE_LOG(LogDirt, Log, TEXT("Run DaDirt.View to look at it."));
		}));

static FAutoConsoleCommandWithWorldAndArgs GDirtViewCmd(
	TEXT("DaDirt.View"),
	TEXT("DaDirt.View - jump the camera to a viewpoint that frames the whole box."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>&, UWorld* World)
		{
			ADirtBox* Box = GetDirtBoxOrWarn();
			if (!Box || !World)
			{
				return;
			}

			FVector Loc; FRotator Rot;
			Box->GetSuggestedViewpoint(Loc, Rot);

			if (APlayerController* PC = World->GetFirstPlayerController())
			{
				if (APawn* Pawn = PC->GetPawn())
				{
					Pawn->SetActorLocation(Loc);
					Pawn->SetActorRotation(Rot);
					PC->SetControlRotation(Rot);
				}
			}
		}));

static FAutoConsoleCommandWithWorldAndArgs GDirtBallCmd(
	TEXT("DaDirt.Ball"),
	TEXT("DaDirt.Ball <Xm> <Ym> [dropM=5] [radiusCm=30] [vxMps=0] [vyMps=0] - drop a ball onto the dirt. ")
	TEXT("It dents where it lands, rolls downhill and comes to rest on the deformed ground."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld* World)
		{
			ADirtBox* Box = GetDirtBoxOrWarn();
			if (!Box || !World)
			{
				return;
			}
			if (Args.Num() < 2)
			{
				UE_LOG(LogDirt, Warning, TEXT("Usage: DaDirt.Ball <Xm> <Ym> [dropM] [radiusCm] [vxMps] [vyMps]"));
				return;
			}

			const float Xm = ArgFloat(Args, 0, 0.0f);
			const float Ym = ArgFloat(Args, 1, 0.0f);
			const float DropM = ArgFloat(Args, 2, 5.0f);
			const float RadiusCm = FMath::Clamp(ArgFloat(Args, 3, 30.0f), 5.0f, 300.0f);
			const FVector Origin = Box->GetActorLocation();
			const FVector2D World2D(Origin.X + Xm * 100.0, Origin.Y + Ym * 100.0);

			if (!Box->IsInsideBox(World2D))
			{
				UE_LOG(LogDirt, Warning, TEXT("(%.1f, %.1f) m is outside the box."), Xm, Ym);
				return;
			}

			// One blocking readback to find the ground under the drop point. The
			// ball itself never blocks: it reads a height window.
			Box->RefreshReadback();
			const float Ground = Box->GetSurfaceHeightAtWorld(World2D);

			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			ADirtBall* Ball = World->SpawnActor<ADirtBall>(ADirtBall::StaticClass(),
				FVector(World2D.X, World2D.Y, Ground + DropM * 100.0f + RadiusCm), FRotator::ZeroRotator, Params);
			if (Ball)
			{
				Ball->SetRadius(RadiusCm);
				Ball->Velocity = FVector(ArgFloat(Args, 4, 0.0f) * 100.0f, ArgFloat(Args, 5, 0.0f) * 100.0f, 0.0f);
				UE_LOG(LogDirt, Log, TEXT("Dropped a %.0f cm ball at (%.1f, %.1f) m from %.1f m above the ground (Z %.1f cm)."),
					RadiusCm, Xm, Ym, DropM, Ground);
			}
		}));

static FAutoConsoleCommandWithWorldAndArgs GDirtWheelCmd(
	TEXT("DaDirt.Wheel"),
	TEXT("DaDirt.Wheel <Xm> <Ym> [headingDeg=0] - put the powered test wheel on the dirt (replaces any existing one). ")
	TEXT("Then DaDirt.Drive <throttle> <steer> [brake]."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld* World)
		{
			ADirtBox* Box = GetDirtBoxOrWarn();
			if (!Box || !World)
			{
				return;
			}
			if (Args.Num() < 2)
			{
				UE_LOG(LogDirt, Warning, TEXT("Usage: DaDirt.Wheel <Xm> <Ym> [headingDeg]"));
				return;
			}

			for (TActorIterator<ADirtWheel> It(World); It; ++It)
			{
				It->Destroy();
			}

			const float Xm = ArgFloat(Args, 0, 0.0f);
			const float Ym = ArgFloat(Args, 1, 0.0f);
			const float HeadingDeg = ArgFloat(Args, 2, 0.0f);
			const FVector Origin = Box->GetActorLocation();
			const FVector2D World2D(Origin.X + Xm * 100.0, Origin.Y + Ym * 100.0);

			if (!Box->IsInsideBox(World2D))
			{
				UE_LOG(LogDirt, Warning, TEXT("(%.1f, %.1f) m is outside the box."), Xm, Ym);
				return;
			}

			Box->RefreshReadback();
			const float Ground = Box->GetSurfaceHeightAtWorld(World2D);

			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			ADirtWheel* Wheel = World->SpawnActor<ADirtWheel>(ADirtWheel::StaticClass(),
				FVector(World2D.X, World2D.Y, Ground + 60.0f), FRotator(0.0f, HeadingDeg, 0.0f), Params);
			if (Wheel)
			{
				UE_LOG(LogDirt, Log, TEXT("Test wheel at (%.1f, %.1f) m heading %.0f deg. DaDirt.Drive <throttle> <steer> to go."),
					Xm, Ym, HeadingDeg);
			}
		}));

static FAutoConsoleCommandWithWorldAndArgs GDirtDriveCmd(
	TEXT("DaDirt.Drive"),
	TEXT("DaDirt.Drive <throttle -1..1> [steer -1..1] [brake 0..1] - inputs for the test wheel, held until changed."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld* World)
		{
			for (TActorIterator<ADirtWheel> It(World); It; ++It)
			{
				It->SetInputs(FMath::Clamp(ArgFloat(Args, 0, 0.0f), -1.0f, 1.0f),
							  FMath::Clamp(ArgFloat(Args, 1, 0.0f), -1.0f, 1.0f),
							  FMath::Clamp(ArgFloat(Args, 2, 0.0f), 0.0f, 1.0f));
				return;
			}
			UE_LOG(LogDirt, Warning, TEXT("No test wheel. DaDirt.Wheel <x> <y> first."));
		}));

static FAutoConsoleCommandWithWorldAndArgs GDirtAnchorCmd(
	TEXT("DaDirt.Anchor"),
	TEXT("DaDirt.Anchor [0|1] - hold the test wheel in place so it can spin against the dirt (a burnout on a stand)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld* World)
		{
			for (TActorIterator<ADirtWheel> It(World); It; ++It)
			{
				It->bAnchored = Args.IsValidIndex(0) ? (ArgInt(Args, 0, 1) != 0) : !It->bAnchored;
				UE_LOG(LogDirt, Log, TEXT("Wheel %s."), It->bAnchored ? TEXT("anchored") : TEXT("free"));
				return;
			}
		}));

static FAutoConsoleCommandWithWorldAndArgs GDirtFollowCmd(
	TEXT("DaDirt.Follow"),
	TEXT("DaDirt.Follow [0|1] - chase camera behind the test wheel."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld* World)
		{
			for (TActorIterator<ADirtWheel> It(World); It; ++It)
			{
				It->bFollowCamera = Args.IsValidIndex(0) ? (ArgInt(Args, 0, 1) != 0) : !It->bFollowCamera;
				UE_LOG(LogDirt, Log, TEXT("Chase camera %s."), It->bFollowCamera ? TEXT("on") : TEXT("off"));
				return;
			}
		}));

static FAutoConsoleCommandWithWorldAndArgs GDirtClearBallsCmd(
	TEXT("DaDirt.ClearBalls"),
	TEXT("DaDirt.ClearBalls - remove every ball."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>&, UWorld* World)
		{
			int32 Count = 0;
			for (TActorIterator<ADirtBall> It(World); It; ++It)
			{
				It->Destroy();
				++Count;
			}
			UE_LOG(LogDirt, Log, TEXT("Removed %d balls."), Count);
		}));

static FAutoConsoleCommandWithWorldAndArgs GDirtInfoCmd(
	TEXT("DaDirt.Info"),
	TEXT("DaDirt.Info - grid settings and a map of what is in the testbed."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>&, UWorld*)
		{
			ADirtBox* Box = GetDirtBoxOrWarn();
			if (!Box)
			{
				return;
			}

			const FDirtSimSettings& S = Box->Settings;
			UE_LOG(LogDirt, Log, TEXT("--- Dirtbox ---"));
			UE_LOG(LogDirt, Log, TEXT("  mode           %s"),
				Box->TerrainMode == EDirtTerrainMode::Track ? TEXT("track") : TEXT("testbed"));
			UE_LOG(LogDirt, Log, TEXT("  box            %.0f m square"), S.WorldSizeCm * 0.01f);
			if (S.IsFocused())
			{
				UE_LOG(LogDirt, Log, TEXT("  sim region     %.1f m square at (%.0f, %.0f) m  [DaDirt.Focus off to widen]"),
					S.RegionSizeCm() * 0.01f,
					S.SimRegionCentreCm.X * 0.01, S.SimRegionCentreCm.Y * 0.01);
			}
			else
			{
				UE_LOG(LogDirt, Log, TEXT("  sim region     the whole box"));
			}
			UE_LOG(LogDirt, Log, TEXT("  sim grid       %d x %d (%.2f cm per cell, a 12 cm rut is %.1f cells)"),
				S.SimResolution, S.SimResolution, S.TexelSizeCm(), 12.0f / FMath::Max(S.TexelSizeCm(), 0.01f));
			UE_LOG(LogDirt, Log, TEXT("  display mesh   %d x %d verts"), S.MeshVertsPerSide, S.MeshVertsPerSide);
			UE_LOG(LogDirt, Log, TEXT("  soil           friction %.0f deg loose / %.0f dense (x%.1f when saturated), ")
				TEXT("cohesion %.1f kPa suction peak + %.1f kPa packed, %.0f kN/m3"),
				S.LooseReposeDeg, S.PackedReposeDeg, 1.0f - S.SaturationFrictionLoss,
				S.SuctionCohesionKPa, S.PackedCohesionKPa, S.UnitWeightKNm3);
			UE_LOG(LogDirt, Log, TEXT("  slump          %d iterations at rate %.3f, %.0f Hz fixed step"),
				S.SlumpIterations, S.SlumpRate, S.SimHz);

			UE_LOG(LogDirt, Log, TEXT("--- testbed ---"));
			for (const FString& Line : Box->GetFeatureLog())
			{
				UE_LOG(LogDirt, Log, TEXT("  %s"), *Line);
			}
		}));
