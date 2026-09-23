#include "DirtBox.h"

#include "DirtBall.h"
#include "DirtWheel.h"
#include "Async/Async.h"
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
	uint32 RequestedGeneration = 0;
	FIntPoint ReadyOrigin = FIntPoint::ZeroValue;
	uint32 ReadyGeneration = 0;
	TArray<FLinearColor> ReadyData;
	TArray<float> ReadyPond;
	TArray<FVector2f> ReadySkin;
	bool bReady = false;

	// Render thread only.
	TUniquePtr<FRHIGPUTextureReadback> Readback[2];
	TUniquePtr<FRHIGPUTextureReadback> PondReadback[2];
	FIntPoint PendingOrigin[2];
	uint32 PendingGeneration[2] = { 0, 0 };
	bool bPending[2] = { false, false };
};

ADirtBox::ADirtBox()
{
	PrimaryActorTick.bCanEverTick = true;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(SceneRoot);

	// The ground mesh is a child so it can move with the simulated window;
	// parcels and dust are box-relative and stay under the root.
	GroundMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("GroundMesh"));
	GroundMesh->SetupAttachment(SceneRoot);

	// The mesh is displaced entirely in the vertex shader, so its collision would
	// be a flat plane and lie about where the ground is. Physics queries go
	// through GetSurfaceHeightAtWorld instead.
	GroundMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	GroundMesh->bUseAsyncCooking = false;
	GroundMesh->SetCastShadow(true);

	// Dirt in the air: one tiny mesh per parcel slot, moved entirely in the
	// vertex shader from the parcel state textures. No shadows: a quarter of a
	// million shadow casters is not a cost the Arc can carry, and dust does not
	// need them.
	ParcelMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("ParcelMesh"));
	ParcelMesh->SetupAttachment(SceneRoot);
	ParcelMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ParcelMesh->bUseAsyncCooking = false;
	ParcelMesh->SetCastShadow(false);

	DustMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("DustMesh"));
	DustMesh->SetupAttachment(SceneRoot);
	DustMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	DustMesh->bUseAsyncCooking = false;
	DustMesh->SetCastShadow(false);

	FarMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("FarMesh"));
	FarMesh->SetupAttachment(SceneRoot);
	FarMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	FarMesh->bUseAsyncCooking = false;
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
	if (!ParcelMaterial && ParcelMaterialPath.IsValid())
	{
		ParcelMaterial = Cast<UMaterialInterface>(ParcelMaterialPath.TryLoad());
	}
	if (!FarMaterial && FarMaterialPath.IsValid())
	{
		FarMaterial = Cast<UMaterialInterface>(FarMaterialPath.TryLoad());
	}
	if (!DustMaterial && DustMaterialPath.IsValid())
	{
		DustMaterial = Cast<UMaterialInterface>(DustMaterialPath.TryLoad());
	}

	ApplyModeDefaults();
	CreateResources();
	BuildWholeSiteLog();
	WindowTile = TileForCentre(Settings.SimRegionCentreCm, Settings.RegionSizeCm());
	Settings.SimRegionCentreCm = FVector2D(-Settings.WorldSizeCm * 0.5 + (WindowTile.X + TilesPerSide * 0.5) * TileSizeCm(),
										   -Settings.WorldSizeCm * 0.5 + (WindowTile.Y + TilesPerSide * 0.5) * TileSizeCm());
	BuildTerrainAndUpload();
	BuildDisplayMesh();
	BuildFarMesh();
	BuildParcelMesh();
	BuildDustMesh();
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

	// The parcel bookkeeping holds pooled GPU buffers and a readback that must be
	// destroyed on the render thread, after every step that used them.
	if (ParcelGPU || DustGPU)
	{
		TSharedPtr<FDirtParcelResources, ESPMode::ThreadSafe> DyingParcels = MoveTemp(ParcelGPU);
		TSharedPtr<FDirtParcelResources, ESPMode::ThreadSafe> DyingDust = MoveTemp(DustGPU);
		ParcelGPU.Reset();
		DustGPU.Reset();
		ENQUEUE_RENDER_COMMAND(DirtParcelRelease)(
			[DyingParcels, DyingDust](FRHICommandListImmediate&) mutable
			{
				DyingParcels.Reset();
				DyingDust.Reset();
			});
	}

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
	PondA = MakeRT(TEXT("DirtPondA"), RTF_RGBA32f);
	PondB = MakeRT(TEXT("DirtPondB"), RTF_RGBA32f);
	DisplayRT = MakeRT(TEXT("DirtDisplay"), RTF_RGBA32f);
	NormalRT = MakeRT(TEXT("DirtNormal"), RTF_RGBA16f);
	DebugRT = MakeRT(TEXT("DirtDebug"), RTF_RGBA16f);

	// Parcel state. Same 32-bit floats: a parcel position quantised to 16 bits
	// would jitter by centimetres a hundred metres from the origin.
	const int32 ParcelSide = FMath::Clamp(Settings.ParcelPoolSide, 64, 1024);
	ParcelPoolCount = ParcelSide * ParcelSide;
	const auto MakeParcelRT = [this, ParcelSide](const TCHAR* Name) -> UTextureRenderTarget2D*
	{
		UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(this, Name);
		RT->RenderTargetFormat = RTF_RGBA32f;
		RT->ClearColor = FLinearColor::Transparent;
		RT->bAutoGenerateMips = false;
		RT->bCanCreateUAV = true;
		RT->AddressX = TA_Clamp;
		RT->AddressY = TA_Clamp;
		RT->Filter = TF_Nearest;             // the material reads one texel per parcel
		RT->InitAutoFormat(ParcelSide, ParcelSide);
		RT->UpdateResourceImmediate(true);
		return RT;
	};
	ParcelPosRT = MakeParcelRT(TEXT("DirtParcelPos"));
	ParcelVelRT = MakeParcelRT(TEXT("DirtParcelVel"));
	ParcelPropRT = MakeParcelRT(TEXT("DirtParcelProp"));
	ParcelGPU = MakeShared<FDirtParcelResources, ESPMode::ThreadSafe>();

	const int32 DustSide = FMath::Clamp(Settings.DustPoolSide, 32, 1024);
	DustPoolCount = DustSide * DustSide;
	const auto MakeDustRT = [this, DustSide](const TCHAR* Name) -> UTextureRenderTarget2D*
	{
		UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(this, Name);
		RT->RenderTargetFormat = RTF_RGBA32f;
		RT->ClearColor = FLinearColor::Transparent;
		RT->bAutoGenerateMips = false;
		RT->bCanCreateUAV = true;
		RT->AddressX = TA_Clamp;
		RT->AddressY = TA_Clamp;
		RT->Filter = TF_Nearest;
		RT->InitAutoFormat(DustSide, DustSide);
		RT->UpdateResourceImmediate(true);
		return RT;
	};
	DustPosRT = MakeDustRT(TEXT("DirtDustPos"));
	DustVelRT = MakeDustRT(TEXT("DirtDustVel"));
	DustPropRT = MakeDustRT(TEXT("DirtDustProp"));
	DustGPU = MakeShared<FDirtParcelResources, ESPMode::ThreadSafe>();

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

	SoilTex = UTexture2D::CreateTransient(Res, Res, PF_R8_UINT);
	SoilTex->SRGB = false;
	SoilTex->Filter = TF_Nearest;
	SoilTex->AddressX = TA_Clamp;
	SoilTex->AddressY = TA_Clamp;
	SoilTex->NeverStream = true;

	InitialPondTex = UTexture2D::CreateTransient(Res, Res, PF_A32B32G32R32F);
	InitialPondTex->SRGB = false;
	InitialPondTex->Filter = TF_Nearest;
	InitialPondTex->AddressX = TA_Clamp;
	InitialPondTex->AddressY = TA_Clamp;
	InitialPondTex->NeverStream = true;

	TileCells = Res / TilesPerSide;
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
	TileCells = Res / TilesPerSide;

	// The window is assembled tile by tile: a tile the cache remembers comes
	// back as it was left, anything else is built fresh and its pristine volume
	// joins the world baseline.
	TArray<float> Bedrock;
	TArray<FLinearColor> Initial;
	TArray<FLinearColor> InitialPond;
	TArray<uint8> Soils;
	Bedrock.SetNumZeroed(Count);
	Initial.SetNumZeroed(Count);
	InitialPond.SetNumZeroed(Count);
	Soils.SetNumZeroed(Count);

	TArray<float> TileBedrock;
	TArray<FLinearColor> TileState;
	TArray<uint8> TileSoil;
	for (int32 TY = 0; TY < TilesPerSide; ++TY)
	{
		for (int32 TX = 0; TX < TilesPerSide; ++TX)
		{
			const FIntPoint Tile = WindowTile + FIntPoint(TX, TY);
			BuildTile(Tile, TileBedrock, TileState, TileSoil);

			TSharedPtr<FDirtTile>* Cached = TileCache.Find(Tile);
			const bool bFromCache = Cached && (*Cached)->bHasState;
			for (int32 Y = 0; Y < TileCells; ++Y)
			{
				const int32 Row = (TY * TileCells + Y) * Res + TX * TileCells;
				FMemory::Memcpy(Bedrock.GetData() + Row, TileBedrock.GetData() + Y * TileCells, TileCells * sizeof(float));
				FMemory::Memcpy(Soils.GetData() + Row, TileSoil.GetData() + Y * TileCells, TileCells * sizeof(uint8));
				if (bFromCache)
				{
					FMemory::Memcpy(Initial.GetData() + Row, (*Cached)->State.GetData() + Y * TileCells, TileCells * sizeof(FLinearColor));
					FMemory::Memcpy(InitialPond.GetData() + Row, (*Cached)->Pond.GetData() + Y * TileCells, TileCells * sizeof(FLinearColor));
				}
				else
				{
					FMemory::Memcpy(Initial.GetData() + Row, TileState.GetData() + Y * TileCells, TileCells * sizeof(FLinearColor));
					// Fresh ground has no skin: the base is the surface.
					for (int32 X = 0; X < TileCells; ++X)
					{
						const FLinearColor& St = TileState[Y * TileCells + X];
						InitialPond[Row + X] = FLinearColor(0.0f, 0.0f, 0.0f, DirtPackBase(St.G, St.B));
					}
				}
			}
			if (bFromCache)
			{
				// Its dirt is in the window now, not in storage.
				WorldStoredM3 -= (*Cached)->StoredM3;
				(*Cached)->StoredM3 = 0.0;
				(*Cached)->bHasState = false;
			}
		}
	}

	BedrockCm = Bedrock;
	SoilId = Soils;
	UploadFloats(BaseHeightTex, Bedrock);
	UploadBytes(SoilTex, SoilId);
	UploadColors(InitialStateTex, Initial);
	UploadColors(InitialPondTex, InitialPond);

	// Make sure the uploads have actually landed before the first sim step
	// reads them. This happens on a rebuild, so the stall does not matter.
	FlushRenderingCommands();
}

void ADirtBox::UploadFloats(UTexture2D* Texture, const TArray<float>& Data)
{
	FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
	void* Dest = Mip.BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(Dest, Data.GetData(), Data.Num() * sizeof(float));
	Mip.BulkData.Unlock();
	Texture->UpdateResource();
}

void ADirtBox::UploadFloat2s(UTexture2D* Texture, const TArray<FVector2f>& Data)
{
	FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
	void* Dest = Mip.BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(Dest, Data.GetData(), Data.Num() * sizeof(FVector2f));
	Mip.BulkData.Unlock();
	Texture->UpdateResource();
}

void ADirtBox::UploadBytes(UTexture2D* Texture, const TArray<uint8>& Data)
{
	FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
	void* Dest = Mip.BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(Dest, Data.GetData(), Data.Num() * sizeof(uint8));
	Mip.BulkData.Unlock();
	Texture->UpdateResource();
}

const FDirtSoil& ADirtBox::SoilAtTexel(FIntPoint Texel) const
{
	const int32 Res = Settings.SimResolution;
	const int32 X = FMath::Clamp(Texel.X, 0, Res - 1);
	const int32 Y = FMath::Clamp(Texel.Y, 0, Res - 1);
	return Settings.Soil(SoilId.IsValidIndex(Y * Res + X) ? SoilId[Y * Res + X] : Settings.DefaultSoil);
}

int32 ADirtBox::SoilIdAtWorld(FVector2D WorldXYCm) const
{
	const int32 Res = Settings.SimResolution;
	const FVector2f T = WorldToTexel(WorldXYCm);
	const int32 X = FMath::Clamp(FMath::FloorToInt(T.X), 0, Res - 1);
	const int32 Y = FMath::Clamp(FMath::FloorToInt(T.Y), 0, Res - 1);
	return SoilId.IsValidIndex(Y * Res + X) ? SoilId[Y * Res + X] : Settings.DefaultSoil;
}

void ADirtBox::PaintSoil(int32 Id, FVector2D CentreCm, float RadiusCm)
{
	if (!bResourcesReady || !Settings.Soils.IsValidIndex(Id))
	{
		return;
	}
	const int32 Res = Settings.SimResolution;
	if (SoilId.Num() != Res * Res)
	{
		return;
	}
	if (RadiusCm <= 0.0f)
	{
		SoilOverride = Id;
		for (uint8& V : SoilId) { V = static_cast<uint8>(Id); }
	}
	else
	{
		const FVector2f C = WorldToTexel(CentreCm);
		const float R = RadiusCm / Settings.TexelSizeCm();
		for (int32 Y = 0; Y < Res; ++Y)
		{
			for (int32 X = 0; X < Res; ++X)
			{
				const float DX = X + 0.5f - C.X, DY = Y + 0.5f - C.Y;
				if (DX * DX + DY * DY <= R * R)
				{
					SoilId[Y * Res + X] = static_cast<uint8>(Id);
				}
			}
		}
	}
	UploadBytes(SoilTex, SoilId);
	// The far ground and the whole-site map keep the built soil; painting is a
	// test-time act on the window.
}

void ADirtBox::UploadColors(UTexture2D* Texture, const TArray<FLinearColor>& Data)
{
	FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
	void* Dest = Mip.BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(Dest, Data.GetData(), Data.Num() * sizeof(FLinearColor));
	Mip.BulkData.Unlock();
	Texture->UpdateResource();
}

void ADirtBox::BuildWholeSiteLog()
{
	// The builders describe the whole site (and the track settles its borrow
	// pits) only when asked for all of it. Tiles are built afterwards from the
	// same rules, so this is the one place the feature list comes from.
	FDirtSimSettings Whole = Settings;
	Whole.SimRegionSizeCm = 0.0f;
	Whole.SimRegionCentreCm = FVector2D::ZeroVector;
	TArray<float> Bedrock, Layer;
	TrackLineM.Reset();
	if (TerrainMode == EDirtTerrainMode::Track)
	{
		FDirtTrack Track(Whole);
		Track.Build();
		FeatureLog = Track.FeatureLog;
		LapLengthM = Track.LapLengthM;
		TrackLineM.Reserve(Track.Centreline.Num());
		for (const FDirtTrack::FSample& Sample : Track.Centreline)
		{
			TrackLineM.Add(FVector2D(Sample.Pos.X, Sample.Pos.Y));
		}
		Bedrock = MoveTemp(Track.Bedrock); Layer = MoveTemp(Track.Layer);
		WholeCompaction = MoveTemp(Track.Compaction); WholeMoisture = MoveTemp(Track.Moisture);
		WholeSoil = MoveTemp(Track.Soil);
	}
	else
	{
		FDirtTestbed Testbed(Whole);
		Testbed.Build();
		FeatureLog = Testbed.FeatureLog;
		LapLengthM = 0.0f;
		Bedrock = MoveTemp(Testbed.Bedrock); Layer = MoveTemp(Testbed.Layer);
		WholeCompaction = MoveTemp(Testbed.Compaction); WholeMoisture = MoveTemp(Testbed.Moisture);
		WholeSoil = MoveTemp(Testbed.Soil);
	}

	// Kept for the far ground: the surface a ruler would measure, and the solids.
	WholeRes = FMath::Max(Whole.SimResolution, 16);
	const int32 Count = WholeRes * WholeRes;
	WholeSurfaceCm.SetNumUninitialized(Count);
	WholeSolidCm.SetNumUninitialized(Count);
	for (int32 i = 0; i < Count; ++i)
	{
		const float Bulk = FMath::Max(Layer[i], 0.0f);
		WholeSurfaceCm[i] = Bedrock[i] + Bulk;
		WholeSolidCm[i] = DirtSolidCm(Bulk, WholeCompaction[i], Settings.Soil(WholeSoil.IsValidIndex(i) ? WholeSoil[i] : 1), Settings);
	}
}

FLinearColor ADirtBox::FarTint(const FDirtSoil& Soil, float SolidCm, float Compaction, float Moisture)
{
	// The resolve pass's plain dirt, mirrored (DirtSim.usf, MainResolveCS).
	FLinearColor C = FMath::Lerp(Soil.DryColour, Soil.PackedColour, FMath::Clamp(Compaction, 0.0f, 1.0f));
	C = FMath::Lerp(C, C * 0.40f, FMath::Clamp(Moisture, 0.0f, 1.0f));
	C = FMath::Lerp(FLinearColor(0.35f, 0.33f, 0.31f), C, FMath::Clamp(SolidCm / 3.0f, 0.0f, 1.0f));
	C.A = 1.0f;
	return C;
}

void ADirtBox::FillFarTile(FIntPoint Tile, const FDirtTile* Cached, TArray<FVector>& Verts, TArray<FVector>& Normals, TArray<FLinearColor>& Colors) const
{
	const int32 V = FarVerts;
	const float Step = FarTileCm / (V - 1);
	const float Half = Settings.WorldSizeCm * 0.5f;
	const FVector2D Origin(-Half + Tile.X * FarTileCm, -Half + Tile.Y * FarTileCm);

	// Height, solids, packing and moisture at a box-relative point, bilinear.
	const auto Sample = [&](float Xcm, float Ycm, float& OutSurface, float& OutSolid, float& OutComp, float& OutMoist)
	{
		if (Cached && Cached->bHasState)
		{
			const float Texel = Settings.TexelSizeCm();
			const int32 N = TileCells;
			const float FX = FMath::Clamp((Xcm - Origin.X) / Texel - 0.5f, 0.0f, N - 1.0f);
			const float FY = FMath::Clamp((Ycm - Origin.Y) / Texel - 0.5f, 0.0f, N - 1.0f);
			const int32 X0 = FMath::FloorToInt(FX), Y0 = FMath::FloorToInt(FY);
			const int32 X1 = FMath::Min(X0 + 1, N - 1), Y1 = FMath::Min(Y0 + 1, N - 1);
			const float TX = FX - X0, TY = FY - Y0;
			const FLinearColor S = FMath::Lerp(FMath::Lerp(Cached->State[Y0 * N + X0], Cached->State[Y0 * N + X1], TX),
											   FMath::Lerp(Cached->State[Y1 * N + X0], Cached->State[Y1 * N + X1], TX), TY);
			OutSurface = S.A; OutSolid = S.R; OutComp = S.G; OutMoist = S.B;
		}
		else
		{
			const float Texel = Settings.WorldSizeCm / WholeRes;
			const int32 N = WholeRes;
			const float FX = FMath::Clamp((Xcm + Half) / Texel - 0.5f, 0.0f, N - 1.0f);
			const float FY = FMath::Clamp((Ycm + Half) / Texel - 0.5f, 0.0f, N - 1.0f);
			const int32 X0 = FMath::FloorToInt(FX), Y0 = FMath::FloorToInt(FY);
			const int32 X1 = FMath::Min(X0 + 1, N - 1), Y1 = FMath::Min(Y0 + 1, N - 1);
			const float TX = FX - X0, TY = FY - Y0;
			const auto Bi = [&](const TArray<float>& A)
			{
				return FMath::Lerp(FMath::Lerp(A[Y0 * N + X0], A[Y0 * N + X1], TX), FMath::Lerp(A[Y1 * N + X0], A[Y1 * N + X1], TX), TY);
			};
			OutSurface = Bi(WholeSurfaceCm); OutSolid = Bi(WholeSolidCm); OutComp = Bi(WholeCompaction); OutMoist = Bi(WholeMoisture);
		}
	};

	// One ring beyond the tile so the normals are centred differences everywhere.
	const int32 W = V + 2;
	TArray<float> Hs;
	Hs.SetNumUninitialized(W * W);
	Verts.SetNumUninitialized(V * V);
	Normals.SetNumUninitialized(V * V);
	Colors.SetNumUninitialized(V * V);
	for (int32 J = 0; J < W; ++J)
	{
		for (int32 I = 0; I < W; ++I)
		{
			const float X = Origin.X + (I - 1) * Step;
			const float Y = Origin.Y + (J - 1) * Step;
			float Surface, Solid, Comp, Moist;
			Sample(X, Y, Surface, Solid, Comp, Moist);
			Hs[J * W + I] = Surface;
			if (I >= 1 && I <= V && J >= 1 && J <= V)
			{
				const int32 K = (J - 1) * V + (I - 1);
				Verts[K] = FVector(X, Y, Surface);
				const int32 WX = FMath::Clamp(FMath::FloorToInt((X + Half) / (Settings.WorldSizeCm / WholeRes)), 0, WholeRes - 1);
				const int32 WY = FMath::Clamp(FMath::FloorToInt((Y + Half) / (Settings.WorldSizeCm / WholeRes)), 0, WholeRes - 1);
				const int32 WSoil = WholeSoil.IsValidIndex(WY * WholeRes + WX) ? WholeSoil[WY * WholeRes + WX] : 1;
				Colors[K] = FarTint(Settings.Soil(WSoil), Solid, Comp, Moist);
			}
		}
	}
	for (int32 J = 1; J <= V; ++J)
	{
		for (int32 I = 1; I <= V; ++I)
		{
			const float DX = (Hs[J * W + I + 1] - Hs[J * W + I - 1]) / (2.0f * Step);
			const float DY = (Hs[(J + 1) * W + I] - Hs[(J - 1) * W + I]) / (2.0f * Step);
			Normals[(J - 1) * V + (I - 1)] = FVector(-DX, -DY, 1.0f).GetSafeNormal();
		}
	}
}

void ADirtBox::BuildFarMesh()
{
	FarMesh->ClearAllMeshSections();
	FarTilesPerSide = 0;
	if (!Settings.IsFocused() || WholeRes == 0)
	{
		return;                              // the window is the whole box: nothing is outside it
	}

	FarTileCm = TileSizeCm();
	FarTilesPerSide = FMath::CeilToInt(Settings.WorldSizeCm / FarTileCm);
	FarVerts = FMath::Clamp(FMath::RoundToInt(FarTileCm / 25.0f), 2, 256) + 1;
	if (FarMaterial)
	{
		FarMesh->SetMaterial(0, FarMaterial);
	}

	const int32 V = FarVerts;
	TArray<int32> Triangles;
	Triangles.Reserve((V - 1) * (V - 1) * 6);
	for (int32 Y = 0; Y < V - 1; ++Y)
	{
		for (int32 X = 0; X < V - 1; ++X)
		{
			const int32 I0 = Y * V + X, I1 = I0 + 1, I2 = I0 + V, I3 = I2 + 1;
			Triangles.Add(I0); Triangles.Add(I2); Triangles.Add(I1);
			Triangles.Add(I1); Triangles.Add(I2); Triangles.Add(I3);
		}
	}

	TArray<FVector> Verts, Normals;
	TArray<FLinearColor> Colors;
	TArray<FVector2D> UVs;
	TArray<FProcMeshTangent> Tangents;
	for (int32 TY = 0; TY < FarTilesPerSide; ++TY)
	{
		for (int32 TX = 0; TX < FarTilesPerSide; ++TX)
		{
			const FIntPoint Tile(TX, TY);
			const TSharedPtr<FDirtTile>* Cached = TileCache.Find(Tile);
			FillFarTile(Tile, Cached ? Cached->Get() : nullptr, Verts, Normals, Colors);
			const int32 Section = TY * FarTilesPerSide + TX;
			FarMesh->CreateMeshSection_LinearColor(Section, Verts, Triangles, Normals, UVs, Colors, Tangents, false);
			if (FarMaterial)
			{
				FarMesh->SetMaterial(Section, FarMaterial);
			}
		}
	}
	UpdateFarVisibility();
	UE_LOG(LogDirt, Log, TEXT("Far ground: %d x %d tiles of %.1f m, %d x %d verts each (%.0f cm spacing)."),
		FarTilesPerSide, FarTilesPerSide, FarTileCm * 0.01f, V, V, FarTileCm / (V - 1));
}

void ADirtBox::UpdateFarTile(FIntPoint Tile)
{
	if (FarTilesPerSide == 0 || Tile.X < 0 || Tile.Y < 0 || Tile.X >= FarTilesPerSide || Tile.Y >= FarTilesPerSide)
	{
		return;
	}
	const TSharedPtr<FDirtTile>* Cached = TileCache.Find(Tile);
	TArray<FVector> Verts, Normals;
	TArray<FLinearColor> Colors;
	TArray<FVector2D> UVs;
	TArray<FProcMeshTangent> Tangents;
	FillFarTile(Tile, Cached ? Cached->Get() : nullptr, Verts, Normals, Colors);
	FarMesh->UpdateMeshSection_LinearColor(Tile.Y * FarTilesPerSide + Tile.X, Verts, Normals, UVs, Colors, Tangents);
}

void ADirtBox::UpdateFarVisibility()
{
	for (int32 TY = 0; TY < FarTilesPerSide; ++TY)
	{
		for (int32 TX = 0; TX < FarTilesPerSide; ++TX)
		{
			const FIntPoint Rel = FIntPoint(TX, TY) - WindowTile;
			const bool bInWindow = Rel.X >= 0 && Rel.X < TilesPerSide && Rel.Y >= 0 && Rel.Y < TilesPerSide;
			FarMesh->SetMeshSectionVisible(TY * FarTilesPerSide + TX, !bInWindow);
		}
	}
}

FIntPoint ADirtBox::TileForCentre(FVector2D CentreCm, float RegionCm) const
{
	const float TileCm = RegionCm / TilesPerSide;
	const float Half = Settings.WorldSizeCm * 0.5f;
	const int32 MaxTile = FMath::Max(FMath::CeilToInt(Settings.WorldSizeCm / TileCm - 0.01f) - TilesPerSide, 0);
	const FVector2D Origin = CentreCm - FVector2D(RegionCm * 0.5);
	return FIntPoint(
		FMath::Clamp(FMath::RoundToInt((Origin.X + Half) / TileCm), 0, MaxTile),
		FMath::Clamp(FMath::RoundToInt((Origin.Y + Half) / TileCm), 0, MaxTile));
}

void ADirtBox::BuildTile(FIntPoint Tile, TArray<float>& OutBedrock, TArray<FLinearColor>& OutState, TArray<uint8>& OutSoil)
{
	// The builders take a region and a resolution; a tile is just a small one.
	const float TileCm = TileSizeCm();
	FDirtSimSettings TileSettings = Settings;
	TileSettings.SimResolution = TileCells;
	TileSettings.SimRegionSizeCm = TileCm;
	TileSettings.SimRegionCentreCm = FVector2D(-Settings.WorldSizeCm * 0.5 + (Tile.X + 0.5) * TileCm,
											   -Settings.WorldSizeCm * 0.5 + (Tile.Y + 0.5) * TileCm);

	TArray<float> Layer, Compaction, Moisture;
	if (TerrainMode == EDirtTerrainMode::Track)
	{
		FDirtTrack Track(TileSettings);
		Track.Build();
		OutBedrock = MoveTemp(Track.Bedrock); Layer = MoveTemp(Track.Layer);
		Compaction = MoveTemp(Track.Compaction); Moisture = MoveTemp(Track.Moisture);
		OutSoil = MoveTemp(Track.Soil);
	}
	else
	{
		FDirtTestbed Testbed(TileSettings);
		Testbed.Build();
		OutBedrock = MoveTemp(Testbed.Bedrock); Layer = MoveTemp(Testbed.Layer);
		Compaction = MoveTemp(Testbed.Compaction); Moisture = MoveTemp(Testbed.Moisture);
		OutSoil = MoveTemp(Testbed.Soil);
	}
	if (Settings.Soils.IsValidIndex(SoilOverride))
	{
		for (uint8& V : OutSoil) { V = static_cast<uint8>(SoilOverride); }
	}

	// The builders think in bulk centimetres; the simulation stores solids.
	const int32 Count = TileCells * TileCells;
	OutState.SetNumUninitialized(Count);
	double SumSolidCm = 0.0;
	for (int32 i = 0; i < Count; ++i)
	{
		const FDirtSoil& CellSoil = Settings.Soil(OutSoil.IsValidIndex(i) ? OutSoil[i] : 1);
		const float Solid = DirtSolidCm(FMath::Max(Layer[i], 0.0f), Compaction[i], CellSoil, Settings);
		SumSolidCm += Solid;
		FLinearColor& S = OutState[i];
		S.R = Solid;
		S.G = Compaction[i];
		S.B = Moisture[i];
		S.A = OutBedrock[i] + DirtBulkCm(Solid, Compaction[i], CellSoil, Settings);
	}

	TSharedPtr<FDirtTile>& Entry = TileCache.FindOrAdd(Tile);
	if (!Entry.IsValid())
	{
		Entry = MakeShared<FDirtTile>();
		Entry->PristineM3 = SumSolidCm * Settings.TexelAreaCm2() / 1000000.0;
		BaselineVolumeM3 += Entry->PristineM3;
	}
}

void ADirtBox::SetFollowWheel(bool bFollow)
{
	bFollowWheel = bFollow;
}

void ADirtBox::UpdateFollow()
{
	if (!bFollowWheel || !bResourcesReady || PendingShiftTexels != FIntPoint::ZeroValue || bNeedsReinit)
	{
		return;
	}
	ADirtWheel* Wheel = nullptr;
	for (TActorIterator<ADirtWheel> It(GetWorld()); It; ++It)
	{
		Wheel = *It;
		break;
	}
	if (!Wheel)
	{
		return;
	}

	// Keep the wheel inside the middle two by two tiles. One tile per frame.
	const FVector2D Local = FVector2D(Wheel->GetActorLocation() - GetActorLocation());
	const float TileCm = TileSizeCm();
	const FVector2D Origin = Settings.SimRegionCentreCm - FVector2D(Settings.RegionSizeCm() * 0.5);
	const int32 TX = FMath::FloorToInt((Local.X - Origin.X) / TileCm);
	const int32 TY = FMath::FloorToInt((Local.Y - Origin.Y) / TileCm);
	FIntPoint Delta(0, 0);
	if (TX < 1) Delta.X = -1; else if (TX > TilesPerSide - 2) Delta.X = 1;
	if (TY < 1) Delta.Y = -1; else if (TY > TilesPerSide - 2) Delta.Y = 1;
	if (Delta != FIntPoint::ZeroValue)
	{
		ShiftWindow(Delta);
	}
}

void ADirtBox::ShiftWindow(FIntPoint DeltaTiles)
{
	if (!bResourcesReady)
	{
		return;
	}
	const float TileCm = TileSizeCm();
	const int32 MaxTile = FMath::Max(FMath::CeilToInt(Settings.WorldSizeCm / TileCm - 0.01f) - TilesPerSide, 0);
	const FIntPoint NewTile(FMath::Clamp(WindowTile.X + DeltaTiles.X, 0, MaxTile),
							FMath::Clamp(WindowTile.Y + DeltaTiles.Y, 0, MaxTile));
	DeltaTiles = NewTile - WindowTile;
	if (DeltaTiles == FIntPoint::ZeroValue)
	{
		return;
	}

	// A tile still on its way to the cache must land before it can come back.
	HarvestTileReadbacks(true);

	const int32 Res = Settings.SimResolution;
	const int32 Count = Res * Res;
	const FIntPoint Shift = DeltaTiles * TileCells;

	// 1. The leaving tiles are read back by the next step, before the slide.
	for (int32 TY = 0; TY < TilesPerSide; ++TY)
	{
		for (int32 TX = 0; TX < TilesPerSide; ++TX)
		{
			const FIntPoint Tile = WindowTile + FIntPoint(TX, TY);
			const FIntPoint Rel = Tile - NewTile;
			const bool bStays = Rel.X >= 0 && Rel.X < TilesPerSide && Rel.Y >= 0 && Rel.Y < TilesPerSide;
			if (bStays)
			{
				continue;
			}
			TSharedPtr<FDirtTileReadback, ESPMode::ThreadSafe> RB = MakeShared<FDirtTileReadback, ESPMode::ThreadSafe>();
			RB->Tile = Tile;
			RB->OriginTexel = FIntPoint(TX * TileCells, TY * TileCells);
			RB->SizeTexels = TileCells;
			PendingTileReadbacks.Add(RB);
			InFlightTileReadbacks.Add(RB);
		}
	}

	// 2. Bedrock slides on the CPU (it is static and deterministic), the
	//    entering cells are filled, and the patch of entering state is uploaded.
	TArray<float> NewBedrock;
	TArray<uint8> NewSoil;
	TArray<FLinearColor> Patch;
	TArray<FLinearColor> PatchPond;
	NewBedrock.SetNumZeroed(Count);
	NewSoil.SetNumZeroed(Count);
	Patch.SetNumZeroed(Count);
	PatchPond.SetNumZeroed(Count);
	for (int32 Y = 0; Y < Res; ++Y)
	{
		const int32 OY = Y + Shift.Y;
		if (OY < 0 || OY >= Res)
		{
			continue;
		}
		for (int32 X = 0; X < Res; ++X)
		{
			const int32 OX = X + Shift.X;
			if (OX >= 0 && OX < Res)
			{
				NewBedrock[Y * Res + X] = BedrockCm[OY * Res + OX];
				NewSoil[Y * Res + X] = SoilId.IsValidIndex(OY * Res + OX) ? SoilId[OY * Res + OX] : 1;
			}
		}
	}

	TArray<float> TileBedrock;
	TArray<FLinearColor> TileState;
	TArray<uint8> TileSoil;
	int32 Entered = 0, FromCache = 0;
	for (int32 TY = 0; TY < TilesPerSide; ++TY)
	{
		for (int32 TX = 0; TX < TilesPerSide; ++TX)
		{
			const FIntPoint Tile = NewTile + FIntPoint(TX, TY);
			const FIntPoint Rel = Tile - WindowTile;
			const bool bWasIn = Rel.X >= 0 && Rel.X < TilesPerSide && Rel.Y >= 0 && Rel.Y < TilesPerSide;
			if (bWasIn)
			{
				continue;
			}
			BuildTile(Tile, TileBedrock, TileState, TileSoil);
			TSharedPtr<FDirtTile>* Cached = TileCache.Find(Tile);
			const bool bFromCache = Cached && (*Cached)->bHasState;
			for (int32 Y = 0; Y < TileCells; ++Y)
			{
				const int32 Row = (TY * TileCells + Y) * Res + TX * TileCells;
				FMemory::Memcpy(NewBedrock.GetData() + Row, TileBedrock.GetData() + Y * TileCells, TileCells * sizeof(float));
				FMemory::Memcpy(NewSoil.GetData() + Row, TileSoil.GetData() + Y * TileCells, TileCells * sizeof(uint8));
				if (bFromCache)
				{
					FMemory::Memcpy(Patch.GetData() + Row, (*Cached)->State.GetData() + Y * TileCells, TileCells * sizeof(FLinearColor));
					FMemory::Memcpy(PatchPond.GetData() + Row, (*Cached)->Pond.GetData() + Y * TileCells, TileCells * sizeof(FLinearColor));
				}
				else
				{
					FMemory::Memcpy(Patch.GetData() + Row, TileState.GetData() + Y * TileCells, TileCells * sizeof(FLinearColor));
				}
			}
			if (bFromCache)
			{
				WorldStoredM3 -= (*Cached)->StoredM3;
				(*Cached)->StoredM3 = 0.0;
				(*Cached)->bHasState = false;
				++FromCache;
			}
			++Entered;
		}
	}

	BedrockCm = MoveTemp(NewBedrock);
	SoilId = MoveTemp(NewSoil);
	UploadFloats(BaseHeightTex, BedrockCm);
	UploadBytes(SoilTex, SoilId);
	UploadColors(InitialStateTex, Patch);
	UploadColors(InitialPondTex, PatchPond);

	// 3. Everything that addresses the window by texel moves with it.
	WindowTile = NewTile;
	Settings.SimRegionCentreCm += FVector2D(DeltaTiles.X * TileCm, DeltaTiles.Y * TileCm);
	PendingShiftTexels = Shift;
	for (FDirtBrushStroke& Stroke : PendingStrokes) { Stroke.CenterTexel -= FVector2f(Shift.X, Shift.Y); }
	for (FDirtBrushStroke& Stroke : PendingGiving) { Stroke.CenterTexel -= FVector2f(Shift.X, Shift.Y); }
	for (FDirtWaterSource& W : PendingWater) { W.CenterTexel -= FVector2f(Shift.X, Shift.Y); }
	++WindowGeneration;
	Readback.Empty();
	PondReadback.Empty();
	if (GroundMesh)
	{
		GroundMesh->SetRelativeLocation(FVector(Settings.SimRegionCentreCm.X, Settings.SimRegionCentreCm.Y, 0.0));
	}
	UpdateFarVisibility();
	++ShiftsThisSession;

	UE_LOG(LogDirt, Log, TEXT("Window slid by (%d, %d) tiles to (%d, %d): centre (%.1f, %.1f) m, %d tiles entered (%d from the cache), %d stored."),
		DeltaTiles.X, DeltaTiles.Y, WindowTile.X, WindowTile.Y,
		Settings.SimRegionCentreCm.X * 0.01, Settings.SimRegionCentreCm.Y * 0.01, Entered, FromCache, TileCache.Num());
}

void ADirtBox::HarvestTileReadbacks(bool bBlock)
{
	if (InFlightTileReadbacks.Num() == 0)
	{
		return;
	}

	const int32 TileCellsLocal = TileCells;
	const auto Harvest = [this, TileCellsLocal](bool bWait)
	{
		TArray<TSharedPtr<FDirtTileReadback, ESPMode::ThreadSafe>> InFlight = InFlightTileReadbacks;
		ENQUEUE_RENDER_COMMAND(DirtTileHarvest)(
			[InFlight, TileCellsLocal, bWait](FRHICommandListImmediate& RHICmdList)
			{
				for (const TSharedPtr<FDirtTileReadback, ESPMode::ThreadSafe>& RB : InFlight)
				{
					if (!RB->bEnqueued || RB->bDone || !RB->State || !RB->Pond)
					{
						continue;
					}
					if (!RB->State->IsReady() || !RB->Pond->IsReady())
					{
						if (!bWait)
						{
							continue;
						}
						RHICmdList.SubmitAndBlockUntilGPUIdle();
					}
					const int32 N = RB->SizeTexels;
					int32 Pitch = 0;
					if (const FLinearColor* Src = static_cast<const FLinearColor*>(RB->State->Lock(Pitch)))
					{
						RB->ResultState.SetNumUninitialized(N * N);
						for (int32 Y = 0; Y < N; ++Y)
						{
							FMemory::Memcpy(RB->ResultState.GetData() + Y * N, Src + Y * Pitch, N * sizeof(FLinearColor));
						}
						RB->State->Unlock();
					}
					if (const FLinearColor* Src = static_cast<const FLinearColor*>(RB->Pond->Lock(Pitch)))
					{
						RB->ResultPond.SetNumUninitialized(N * N);
						for (int32 Y = 0; Y < N; ++Y)
						{
							FMemory::Memcpy(RB->ResultPond.GetData() + Y * N, Src + Y * Pitch, N * sizeof(FLinearColor));
						}
						RB->Pond->Unlock();
					}
					RB->bDone = true;
				}
			});
	};

	Harvest(bBlock);
	if (bBlock)
	{
		FlushRenderingCommands();
	}

	for (int32 i = InFlightTileReadbacks.Num() - 1; i >= 0; --i)
	{
		// By value: the array slot is removed below and a reference into it
		// would dangle (it did, and asserted once in a hundred slides).
		const TSharedPtr<FDirtTileReadback, ESPMode::ThreadSafe> RB = InFlightTileReadbacks[i];
		if (!RB->bDone)
		{
			continue;
		}
		TSharedPtr<FDirtTile>& Entry = TileCache.FindOrAdd(RB->Tile);
		if (!Entry.IsValid())
		{
			Entry = MakeShared<FDirtTile>();
		}
		Entry->State = MoveTemp(RB->ResultState);
		Entry->Pond = MoveTemp(RB->ResultPond);
		double SumSolidCm = 0.0;
		for (const FLinearColor& S : Entry->State)
		{
			SumSolidCm += S.R;
		}
		Entry->StoredM3 = SumSolidCm * Settings.TexelAreaCm2() / 1000000.0;
		Entry->bHasState = Entry->State.Num() == TileCells * TileCells;
		WorldStoredM3 += Entry->StoredM3;
		InFlightTileReadbacks.RemoveAt(i);
		UpdateFarTile(RB->Tile);
	}
}

void ADirtBox::BuildDisplayMesh()
{
	const int32 N = FMath::Clamp(Settings.MeshVertsPerSide, 16, 2048);
	const float Size = Settings.RegionSizeCm();
	const float Half = Size * 0.5f;
	const float Step = Size / static_cast<float>(N - 1);

	// The mesh covers exactly what is simulated. It is built around its own
	// origin and the component is moved to the window's centre, so a sliding
	// window costs a transform, not a rebuild.
	const float CentreX = 0.0f;
	const float CentreY = 0.0f;
	GroundMesh->SetRelativeLocation(FVector(Settings.SimRegionCentreCm.X, Settings.SimRegionCentreCm.Y, 0.0));

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

		const float BoxHalf = Settings.WorldSizeCm;      // generous: the window can be anywhere in the box
		TArray<FVector> BoundsVerts;
		BoundsVerts.Add(FVector(-BoxHalf, -BoxHalf, MaxDownCm));
		BoundsVerts.Add(FVector(BoxHalf, BoxHalf, MaxUpCm));
		BoundsVerts.Add(FVector(-BoxHalf, -BoxHalf, MaxDownCm));

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

	if (ParcelMaterial && ParcelMesh)
	{
		if (!ParcelMID)
		{
			ParcelMID = UMaterialInstanceDynamic::Create(ParcelMaterial, this);
		}
		ParcelMID->SetTextureParameterValue(TEXT("ParcelPos"), ParcelPosRT);
		ParcelMID->SetTextureParameterValue(TEXT("ParcelProp"), ParcelPropRT);
		ParcelMID->SetScalarParameterValue(TEXT("MinScreenSize"), Settings.ParcelMinScreenSize);
		for (int32 S = 0; S < FMath::DivideAndRoundUp(ParcelPoolCount, ParcelSectionSlots); ++S)
		{
			ParcelMesh->SetMaterial(S, ParcelMID);
		}
	}
	else if (Settings.bParcels)
	{
		UE_LOG(LogDirt, Warning, TEXT("Dirtbox has no ParcelMaterial — parcels will be simulated but invisible. ")
			TEXT("Run Tools/BuildDirtAssets.py to build /Game/Dirt/M_DirtParcel."));
	}

	if (DustMaterial && DustMesh)
	{
		if (!DustMID)
		{
			DustMID = UMaterialInstanceDynamic::Create(DustMaterial, this);
		}
		DustMID->SetTextureParameterValue(TEXT("ParcelPos"), DustPosRT);
		DustMID->SetTextureParameterValue(TEXT("ParcelProp"), DustPropRT);
		DustMID->SetScalarParameterValue(TEXT("DustLifetime"), Settings.DustLifetime);
		for (int32 S = 0; S < FMath::DivideAndRoundUp(DustPoolCount, ParcelSectionSlots); ++S)
		{
			DustMesh->SetMaterial(S, DustMID);
		}
	}
}

void ADirtBox::BuildDustMesh()
{
	if (!DustMesh)
	{
		return;
	}
	DustMesh->ClearAllMeshSections();
	if (DustPoolCount <= 0)
	{
		return;
	}

	// One quad per mote, all four vertices AT the origin: the material pushes
	// each corner out along the camera's right and up by the corner code in
	// UV1, so the quad always faces the camera. UV0 names the slot, as for parcels.
	const int32 Side = FMath::Clamp(Settings.DustPoolSide, 32, 1024);
	const int32 N = Side * Side;
	const int32 SectionCount = FMath::DivideAndRoundUp(N, ParcelSectionSlots);
	const float Half = Settings.WorldSizeCm * 0.5f;
	static const FVector2D Corners[4] = { FVector2D(-1, -1), FVector2D(1, -1), FVector2D(1, 1), FVector2D(-1, 1) };

	for (int32 Section = 0; Section < SectionCount; ++Section)
	{
		const int32 First = Section * ParcelSectionSlots;
		const int32 Last = FMath::Min(First + ParcelSectionSlots, N);

		TArray<FVector> Vertices;
		TArray<FVector> Normals;
		TArray<FVector2D> UV0;
		TArray<FVector2D> UV1;
		TArray<int32> Triangles;
		TArray<FProcMeshTangent> Tangents;
		TArray<FLinearColor> Colors;
		Vertices.Reserve((Last - First) * 4 + 3);

		for (int32 I = First; I < Last; ++I)
		{
			const FVector2D Slot((static_cast<double>(I % Side) + 0.5) / Side, (static_cast<double>(I / Side) + 0.5) / Side);
			const int32 Base = Vertices.Num();
			for (int32 V = 0; V < 4; ++V)
			{
				Vertices.Add(FVector::ZeroVector);
				Normals.Add(FVector::UpVector);
				UV0.Add(Slot);
				UV1.Add(Corners[V]);
			}
			Triangles.Add(Base); Triangles.Add(Base + 2); Triangles.Add(Base + 1);
			Triangles.Add(Base); Triangles.Add(Base + 3); Triangles.Add(Base + 2);
		}

		const int32 BoundsBase = Vertices.Num();
		Vertices.Add(FVector(-Half, -Half, -2500.0f)); Vertices.Add(FVector(Half, Half, 4000.0f)); Vertices.Add(FVector(-Half, -Half, -2500.0f));
		for (int32 V = 0; V < 3; ++V)
		{
			Normals.Add(FVector::UpVector);
			UV0.Add(FVector2D(1.0 - 0.5 / Side, 1.0 - 0.5 / Side));
			UV1.Add(FVector2D::ZeroVector);
		}
		Triangles.Add(BoundsBase); Triangles.Add(BoundsBase + 1); Triangles.Add(BoundsBase + 2);

		TArray<FVector2D> UV2, UV3;
		DustMesh->CreateMeshSection_LinearColor(Section, Vertices, Triangles, Normals, UV0, UV1, UV2, UV3, Colors, Tangents, false);
		DustMesh->SetMeshSectionVisible(Section, false);
	}
	DustSectionsVisible = 0;

	UE_LOG(LogDirt, Log, TEXT("Dust mesh: %d motes in %d sections."), N, SectionCount);
}

void ADirtBox::BuildParcelMesh()
{
	if (!ParcelMesh)
	{
		return;
	}
	ParcelMesh->ClearAllMeshSections();
	if (!Settings.bParcels || ParcelPoolCount <= 0)
	{
		return;
	}

	// One tetrahedron per parcel slot, one centimetre across, sitting at the
	// actor origin. Its UV names the slot: the material reads that texel of the
	// parcel state, moves the four vertices to the parcel and scales them to its
	// diameter. Dead parcels have zero volume and collapse to a point, which
	// rasterises nothing — but their vertices still cost the vertex shader, so
	// the slots are split into sections and only the sections up to the highest
	// live slot are drawn (UpdateParcelSections). Each tetrahedron is given its
	// own random orientation here, so a stream of them reads as tumbled clods,
	// not a row of identical crystals.
	const int32 Side = FMath::Clamp(Settings.ParcelPoolSide, 64, 1024);
	const int32 N = Side * Side;
	const int32 SectionCount = FMath::DivideAndRoundUp(N, ParcelSectionSlots);

	static const FVector Tetra[4] =
	{
		FVector( 0.4714f,  0.0000f, -0.3333f),
		FVector(-0.2357f,  0.4082f, -0.3333f),
		FVector(-0.2357f, -0.4082f, -0.3333f),
		FVector( 0.0000f,  0.0000f,  1.0000f)
	};
	static const int32 Faces[4][3] = { { 0, 2, 1 }, { 0, 1, 3 }, { 1, 2, 3 }, { 2, 0, 3 } };

	// Honest bounds for every section: a parcel can be anywhere in the box, and
	// a section's real vertices all sit at the origin until the material moves
	// them. A zero-area triangle spanning the box gives the renderer the truth.
	const float Half = Settings.WorldSizeCm * 0.5f;
	const FVector BoundsLo(-Half, -Half, -2500.0f);
	const FVector BoundsHi(Half, Half, 4000.0f);

	FRandomStream Rand(7);
	int32 TotalVerts = 0;
	for (int32 Section = 0; Section < SectionCount; ++Section)
	{
		const int32 First = Section * ParcelSectionSlots;
		const int32 Last = FMath::Min(First + ParcelSectionSlots, N);

		TArray<FVector> Vertices;
		TArray<FVector> Normals;
		TArray<FVector2D> UVs;
		TArray<int32> Triangles;
		TArray<FProcMeshTangent> Tangents;
		TArray<FLinearColor> Colors;
		Vertices.Reserve((Last - First) * 4 + 3);
		Normals.Reserve((Last - First) * 4 + 3);
		UVs.Reserve((Last - First) * 4 + 3);
		Triangles.Reserve((Last - First) * 12 + 3);

		for (int32 I = First; I < Last; ++I)
		{
			const FQuat Rot = FQuat::MakeFromEuler(FVector(Rand.FRandRange(0, 360), Rand.FRandRange(0, 360), Rand.FRandRange(0, 360)));
			const FVector2D UV((static_cast<double>(I % Side) + 0.5) / Side, (static_cast<double>(I / Side) + 0.5) / Side);
			const int32 Base = Vertices.Num();
			for (int32 V = 0; V < 4; ++V)
			{
				const FVector P = Rot.RotateVector(Tetra[V]);
				Vertices.Add(P);
				Normals.Add(P.GetSafeNormal());
				UVs.Add(UV);
			}
			for (int32 F = 0; F < 4; ++F)
			{
				Triangles.Add(Base + Faces[F][0]);
				Triangles.Add(Base + Faces[F][1]);
				Triangles.Add(Base + Faces[F][2]);
			}
		}

		// The bounds triangle carries the UV of a slot that is never live: its
		// own vertices then collapse to the origin at zero size, drawing nothing.
		const int32 BoundsBase = Vertices.Num();
		Vertices.Add(BoundsLo); Vertices.Add(BoundsHi); Vertices.Add(BoundsLo);
		for (int32 V = 0; V < 3; ++V)
		{
			Normals.Add(FVector::UpVector);
			UVs.Add(FVector2D(1.0 - 0.5 / Side, 1.0 - 0.5 / Side));
		}
		Triangles.Add(BoundsBase); Triangles.Add(BoundsBase + 1); Triangles.Add(BoundsBase + 2);

		ParcelMesh->CreateMeshSection_LinearColor(Section, Vertices, Triangles, Normals, UVs, Colors, Tangents, false);
		ParcelMesh->SetMeshSectionVisible(Section, false);
		TotalVerts += Vertices.Num();
	}
	ParcelSectionsVisible = 0;

	UE_LOG(LogDirt, Log, TEXT("Parcel mesh: %d slots in %d sections of %d, %d vertices; only sections with live parcels are drawn."),
		N, SectionCount, ParcelSectionSlots, TotalVerts);
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
			UE_LOG(LogDirt, Log, TEXT("[%s] solid volume %.3f m3 = ground %.3f + airborne %.4f (baseline %.3f, drift %+.4f m3 = %+.3f%%)"),
				*PendingMeasureLabel, A.VolumeM3, A.GroundM3, A.AirborneM3, A.BaselineM3, A.DriftM3, A.DriftPercent);
			UE_LOG(LogDirt, Log, TEXT("[%s] max slope on loose dirt %.1f deg (repose setting %.1f), ")
				TEXT("max slope anywhere %.1f deg, cells scraped to bedrock %d"),
				*PendingMeasureLabel, A.MaxLooseSlopeDeg, Settings.Soil(Settings.DefaultSoil).LooseReposeDeg,
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

	HarvestTileReadbacks(false);
	UpdateFollow();

	if (bPaused)
	{
		// Paused stops the dirt settling, but sculpting still works — otherwise
		// you cannot set up a starting state to watch.
		if (PendingStrokes.Num() > 0 || PendingGiving.Num() > 0)
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

	// Rain that was asked for by the second stops on its own.
	if (Settings.RainSecondsLeft > 0.0f && Settings.RainCmPerSec > 0.0f)
	{
		Settings.RainSecondsLeft -= DeltaSeconds;
		if (Settings.RainSecondsLeft <= 0.0f)
		{
			Settings.RainSecondsLeft = 0.0f;
			Settings.RainCmPerSec = 0.0f;
			UE_LOG(LogDirt, Log, TEXT("Rain stopped."));
		}
	}

	// After the step is enqueued, so a window reads this frame's surface.
	UpdateHeightWindows();
	UpdateParcelCounters();
	UpdateParcelSections(DeltaSeconds);
	++FrameCounter;
}

namespace
{
	// Show only the mesh sections up to the highest live slot. Whatever the GPU
	// last reported, plus whatever was asked for since: the readback runs a frame
	// or two behind, and a burst must not flicker in late. Grow at once, shrink
	// only after a short hold, so a stream that pulses does not make sections blink.
	void UpdateSections(UProceduralMeshComponent* Mesh, int32 PoolCount, int32 SectionSlots, bool bEnabled,
						uint32 HighestLive, int32& SlotsRequested, int32& SectionsVisible, float& HoldSeconds, float DeltaSeconds)
	{
		if (!Mesh || PoolCount <= 0)
		{
			return;
		}
		const int32 Needed = FMath::Min(static_cast<int32>(HighestLive) + SlotsRequested, PoolCount);
		const int32 SectionsNeeded = bEnabled ? FMath::DivideAndRoundUp(Needed, SectionSlots) : 0;
		SlotsRequested = 0;

		if (SectionsNeeded >= SectionsVisible)
		{
			HoldSeconds = 0.5f;
		}
		else
		{
			HoldSeconds -= DeltaSeconds;
			if (HoldSeconds > 0.0f)
			{
				return;
			}
		}

		if (SectionsNeeded != SectionsVisible)
		{
			const int32 SectionCount = FMath::DivideAndRoundUp(PoolCount, SectionSlots);
			for (int32 S = 0; S < SectionCount; ++S)
			{
				Mesh->SetMeshSectionVisible(S, S < SectionsNeeded);
			}
			SectionsVisible = SectionsNeeded;
		}
	}
}

void ADirtBox::UpdateParcelSections(float DeltaSeconds)
{
	UpdateSections(ParcelMesh, ParcelPoolCount, ParcelSectionSlots, Settings.bParcels,
				   ParcelCounters[DirtSim::ParcelCounterMaxLive], ParcelSlotsRequested, ParcelSectionsVisible,
				   ParcelSectionHoldSeconds, DeltaSeconds);
	UpdateSections(DustMesh, DustPoolCount, ParcelSectionSlots, Settings.bParcels && Settings.bDust,
				   DustCounters[DirtSim::ParcelCounterMaxLive], DustSlotsRequested, DustSectionsVisible,
				   DustSectionHoldSeconds, DeltaSeconds);
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
	Slot.RequestedGeneration = WindowGeneration;
}

bool ADirtBox::SampleHeightWindow(int32 Id, FVector2D WorldXYCm, float& OutHeightCm, FVector& OutNormal,
								  FLinearColor* OutState, float* OutPondCm,
								  float* OutSkinCm, float* OutBaseCompaction, float* OutBaseMoisture) const
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

	// The display texture carries the water top; the dirt is that minus the pond.
	float PondHere = 0.0f;
	if (W.Pond.Num() >= N * N)
	{
		const auto PondAt = [&W, N](int32 X, int32 Y) { return W.Pond[Y * N + X]; };
		PondHere = FMath::Lerp(FMath::Lerp(PondAt(X0, Y0), PondAt(X0 + 1, Y0), FX),
							   FMath::Lerp(PondAt(X0, Y0 + 1), PondAt(X0 + 1, Y0 + 1), FX), FY);
	}
	OutHeightCm = static_cast<float>(GetActorLocation().Z) + S.A - PondHere;
	if (OutState)
	{
		*OutState = S;
		OutState->A = S.A - PondHere;
	}
	if (OutPondCm)
	{
		*OutPondCm = PondHere;
	}
	if (OutSkinCm || OutBaseCompaction || OutBaseMoisture)
	{
		// Nearest texel, not blended: the packed base does not interpolate.
		float SkinCm = 0.0f, BaseC = S.G, BaseM = S.B;
		if (W.Skin.Num() >= N * N)
		{
			const FVector2f K = W.Skin[FMath::RoundToInt(T.Y) * N + FMath::RoundToInt(T.X)];
			SkinCm = K.X;
			if (SkinCm > 1e-4f)
			{
				DirtUnpackBase(K.Y, BaseC, BaseM);
			}
			else
			{
				SkinCm = 0.0f;
			}
		}
		if (OutSkinCm) *OutSkinCm = SkinCm;
		if (OutBaseCompaction) *OutBaseCompaction = BaseC;
		if (OutBaseMoisture) *OutBaseMoisture = BaseM;
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
			// Data read from a window that has since slid is addressed wrongly; drop it.
			if (Slot->ReadyGeneration == WindowGeneration)
			{
				Slot->Game.Origin = Slot->ReadyOrigin;
				Slot->Game.Size = Slot->Size;
				Swap(Slot->Game.Data, Slot->ReadyData);
				Swap(Slot->Game.Pond, Slot->ReadyPond);
				Swap(Slot->Game.Skin, Slot->ReadySkin);
				Slot->Game.bValid = true;
			}
			else
			{
				Slot->Game.bValid = false;
			}
			Slot->bReady = false;
		}
	}

	// 2. Harvest and re-arm on the render thread.
	FTextureRenderTargetResource* Resource = DisplayRT->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* PondResource = PondA ? PondA->GameThread_GetRenderTargetResource() : nullptr;
	if (!Resource || !PondResource)
	{
		return;
	}

	TArray<TSharedPtr<FDirtWindowSlot, ESPMode::ThreadSafe>> Slots = Windows;

	ENQUEUE_RENDER_COMMAND(DirtHeightWindows)(
		[Slots, Resource, PondResource](FRHICommandListImmediate& RHICmdList)
		{
			FRHITexture* Texture = Resource->GetRenderTargetTexture();
			FRHITexture* PondTexture = PondResource->GetRenderTargetTexture();
			if (!Texture || !PondTexture)
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
					if (!Slot->bPending[i] || !Slot->Readback[i]->IsReady() || !Slot->PondReadback[i]->IsReady())
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

						// The pond rides along: the wheel needs to know it is in a puddle,
						// and the skin and the base under it, to know what it sinks into.
						TArray<float> PondCopy;
						TArray<FVector2f> SkinCopy;
						PondCopy.SetNumZeroed(N * N);
						SkinCopy.SetNumZeroed(N * N);
						int32 PondPitch = 0;
						if (const void* PondSrc = Slot->PondReadback[i]->Lock(PondPitch))
						{
							const FLinearColor* PondRows = static_cast<const FLinearColor*>(PondSrc);
							for (int32 Y = 0; Y < N; ++Y)
							{
								for (int32 X = 0; X < N; ++X)
								{
									const FLinearColor& PW = PondRows[Y * PondPitch + X];
									PondCopy[Y * N + X] = PW.R;
									SkinCopy[Y * N + X] = FVector2f(PW.B, PW.A);
								}
							}
							Slot->PondReadback[i]->Unlock();
						}

						FScopeLock L(&Slot->Lock);
						Slot->ReadyData = MoveTemp(Copy);
						Slot->ReadyPond = MoveTemp(PondCopy);
						Slot->ReadySkin = MoveTemp(SkinCopy);
						Slot->ReadyOrigin = Slot->PendingOrigin[i];
						Slot->ReadyGeneration = Slot->PendingGeneration[i];
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
						Slot->PondReadback[i] = MakeUnique<FRHIGPUTextureReadback>(TEXT("DirtHeightWindowPond"));
					}

					FIntPoint Origin;
					uint32 Generation;
					{
						FScopeLock L(&Slot->Lock);
						Origin = Slot->RequestedOrigin;
						Generation = Slot->RequestedGeneration;
					}

					Slot->Readback[i]->EnqueueCopy(RHICmdList, Texture,
						FResolveRect(Origin.X, Origin.Y, Origin.X + N, Origin.Y + N));
					Slot->PondReadback[i]->EnqueueCopy(RHICmdList, PondTexture,
						FResolveRect(Origin.X, Origin.Y, Origin.X + N, Origin.Y + N));
					Slot->PendingOrigin[i] = Origin;
					Slot->PendingGeneration[i] = Generation;
					Slot->bPending[i] = true;
					break;
				}
			}
		});
}

void ADirtBox::StepSimulation(bool bForceReinit)
{
	if (!StateA || !StateB || !PondA || !PondB || !DisplayRT || !NormalRT || !DebugRT || !BaseHeightTex || !InitialStateTex)
	{
		return;
	}

	// Resource pointers are safe to hand across from the game thread because the
	// Dirtbox creates them once and never resizes them. The actual RHI textures
	// are pulled out on the render thread, where they belong.
	FTextureResource* BaseHeightRes = BaseHeightTex->GetResource();
	FTextureResource* SoilRes = SoilTex ? SoilTex->GetResource() : nullptr;
	FTextureResource* InitialStateRes = InitialStateTex->GetResource();
	FTextureResource* InitialPondRes = InitialPondTex ? InitialPondTex->GetResource() : nullptr;
	FTextureRenderTargetResource* StateARes = StateA->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* StateBRes = StateB->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* PondARes = PondA->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* PondBRes = PondB->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* DisplayRes = DisplayRT->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* NormalRes = NormalRT->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* DebugRes = DebugRT->GameThread_GetRenderTargetResource();

	if (!BaseHeightRes || !SoilRes || !InitialStateRes || !InitialPondRes || !StateARes || !StateBRes || !PondARes || !PondBRes
		|| !DisplayRes || !NormalRes || !DebugRes)
	{
		return;
	}

	// The pool's resources always go across (the slump pass binds them); the
	// settings decide what runs.
	const bool bPoolReady = ParcelPosRT && ParcelVelRT && ParcelPropRT && ParcelGPU.IsValid();
	const bool bParcelsReady = Settings.bParcels && bPoolReady;
	FTextureRenderTargetResource* ParcelPosRes = bPoolReady ? ParcelPosRT->GameThread_GetRenderTargetResource() : nullptr;
	FTextureRenderTargetResource* ParcelVelRes = bPoolReady ? ParcelVelRT->GameThread_GetRenderTargetResource() : nullptr;
	FTextureRenderTargetResource* ParcelPropRes = bPoolReady ? ParcelPropRT->GameThread_GetRenderTargetResource() : nullptr;
	FDirtParcelResources* ParcelRes = bPoolReady ? ParcelGPU.Get() : nullptr;

	const bool bDustReady = bParcelsReady && Settings.bDust && DustPosRT && DustVelRT && DustPropRT && DustGPU.IsValid();
	FTextureRenderTargetResource* DustPosRes = bDustReady ? DustPosRT->GameThread_GetRenderTargetResource() : nullptr;
	FTextureRenderTargetResource* DustVelRes = bDustReady ? DustVelRT->GameThread_GetRenderTargetResource() : nullptr;
	FTextureRenderTargetResource* DustPropRes = bDustReady ? DustPropRT->GameThread_GetRenderTargetResource() : nullptr;
	FDirtParcelResources* DustRes = bDustReady ? DustGPU.Get() : nullptr;

	FDirtSimFrame Frame;
	Frame.Resolution = FIntPoint(Settings.SimResolution, Settings.SimResolution);
	Frame.TexelSizeCm = Settings.TexelSizeCm();
	Frame.RegionOriginCm = FVector2f(
		static_cast<float>(Settings.SimRegionCentreCm.X) - Settings.RegionSizeCm() * 0.5f,
		static_cast<float>(Settings.SimRegionCentreCm.Y) - Settings.RegionSizeCm() * 0.5f);
	Frame.Dt = 1.0f / FMath::Max(Settings.SimHz, 1.0f);
	Frame.CompactionDepthCm = Settings.CompactionDepthCm;
	Frame.DeepCompaction = Settings.DeepCompaction;
	Frame.DepositCompaction = Settings.DepositCompaction;
	Frame.bWater = Settings.bWater;
	Frame.RunoffRate = Settings.RunoffRate;
	Frame.bErosion = Settings.bErosion;
	Frame.ErosionPace = Settings.ErosionPace;
	Frame.RainCmPerSec = Settings.RainCmPerSec;
	Frame.DrainPerSec = Settings.DrainPerSec;
	Frame.EvapPerSec = Settings.EvapPerSec;
	Frame.WetDepthCm = Settings.WetDepthCm;
	Frame.AmbientMoisture = Settings.AmbientMoisture;
	Frame.EvapPerSec = Settings.EvapPerSec * Settings.EvapMultiplier;
	Frame.CrustSeedCm = Settings.CrustSeedCm;
	Frame.CrustMaxCm = Settings.CrustMaxCm;
	Frame.CrustGrowCmPerSec = Settings.CrustGrowCmPerSec;
	Frame.SkinEvapChokeCm = Settings.SkinEvapChokeCm;
	Frame.bParcels = bParcelsReady;
	Frame.ParcelRes = FMath::Clamp(Settings.ParcelPoolSide, 64, 1024);
	Frame.FrameSeed = FrameCounter;
	Frame.bShed = Settings.bShed;
	Frame.ShedMinOutCm = Settings.ShedMinOutCm;
	Frame.ShedChance = Settings.ShedChance;
	Frame.ShedFraction = Settings.ShedFraction;
	Frame.ShedDiameterCm = Settings.ShedDiameterCm;
	Frame.ShedSpeedCmS = Settings.ShedSpeedCmS;
	Frame.bDust = bDustReady;
	Frame.DustRes = FMath::Clamp(Settings.DustPoolSide, 32, 1024);
	Frame.DustLifetime = Settings.DustLifetime;
	Frame.DustDragK = Settings.DustDragK;
	Frame.DustBuoyancy = Settings.DustBuoyancy;
	Frame.ParcelDragK = Settings.ParcelDragK;
	Frame.ParcelRestitutionDry = Settings.ParcelRestitutionDry;
	Frame.ParcelRestitutionWet = Settings.ParcelRestitutionWet;
	Frame.ParcelRestSpeedCmS = Settings.ParcelRestSpeedCmS;
	Frame.ParcelRestSeconds = Settings.ParcelRestSeconds;
	Frame.MaxCohesiveHeightCm = Settings.MaxCohesiveHeightCm;
	Frame.SoilTable.SetNumZeroed(DirtSim::MaxSoils * DirtSim::SoilRows);
	for (int32 i = 0; i < FMath::Min(Settings.Soils.Num(), DirtSim::MaxSoils); ++i)
	{
		Settings.Soils[i].ToRows(Frame.SoilTable.GetData() + i * DirtSim::SoilRows);
	}
	Frame.SlumpRate = Settings.SlumpRate;
	Frame.LooseningRate = Settings.LooseningRate;
	Frame.LooseningScaleCm = Settings.LooseningScaleCm;
	Frame.SlumpIterations = bPaused ? 0 : Settings.SlumpIterations;
	Frame.DebugMode = static_cast<int32>(DebugView);
	Frame.DebugLayerRangeCm = DebugLayerRangeCm;
	Frame.bReinitialise = bForceReinit;
	if (!bForceReinit)
	{
		Frame.ShiftTexels = PendingShiftTexels;
		Frame.TileReadbacks = PendingTileReadbacks;
		PendingTileReadbacks.Reset();
	}
	PendingShiftTexels = FIntPoint::ZeroValue;
	Frame.Strokes = MoveTemp(PendingStrokes);
	Frame.GivingStrokes = MoveTemp(PendingGiving);
	PendingStrokes.Reset();
	PendingGiving.Reset();
	if (bPoolReady)
	{
		Frame.Spawns = MoveTemp(PendingSpawns);
	}
	PendingSpawns.Reset();
	if (bDustReady)
	{
		Frame.DustSpawns = MoveTemp(PendingDust);
	}
	PendingDust.Reset();
	if (Settings.bWater && !bPaused)
	{
		Frame.WaterSources = MoveTemp(PendingWater);
		PendingWater.Reset();
	}

	ENQUEUE_RENDER_COMMAND(DirtSimStep)(
		[Frame, BaseHeightRes, SoilRes, InitialStateRes, InitialPondRes, StateARes, StateBRes, PondARes, PondBRes, DisplayRes, NormalRes, DebugRes,
		 ParcelPosRes, ParcelVelRes, ParcelPropRes, ParcelRes, DustPosRes, DustVelRes, DustPropRes, DustRes]
		(FRHICommandListImmediate& RHICmdList) mutable
		{
			Frame.BaseHeight = BaseHeightRes->TextureRHI;
			Frame.SoilIn = SoilRes->TextureRHI;
			Frame.InitialState = InitialStateRes->TextureRHI;
			Frame.InitialPond = InitialPondRes->TextureRHI;
			Frame.StateA = StateARes->GetRenderTargetTexture();
			Frame.StateB = StateBRes->GetRenderTargetTexture();
			Frame.PondA = PondARes->GetRenderTargetTexture();
			Frame.PondB = PondBRes->GetRenderTargetTexture();
			Frame.Display = DisplayRes->GetRenderTargetTexture();
			Frame.NormalOut = NormalRes->GetRenderTargetTexture();
			Frame.DebugOut = DebugRes->GetRenderTargetTexture();
			if (ParcelRes && ParcelPosRes && ParcelVelRes && ParcelPropRes)
			{
				Frame.ParcelPos = ParcelPosRes->GetRenderTargetTexture();
				Frame.ParcelVel = ParcelVelRes->GetRenderTargetTexture();
				Frame.ParcelProp = ParcelPropRes->GetRenderTargetTexture();
				Frame.Parcels = ParcelRes;
			}
			if (DustRes && DustPosRes && DustVelRes && DustPropRes)
			{
				Frame.DustPos = DustPosRes->GetRenderTargetTexture();
				Frame.DustVel = DustVelRes->GetRenderTargetTexture();
				Frame.DustProp = DustPropRes->GetRenderTargetTexture();
				Frame.Dust = DustRes;
			}

			if (Frame.IsValid())
			{
				DirtSim::Execute_RenderThread(RHICmdList, Frame);
			}
		});
}

void ADirtBox::UpdateParcelCounters()
{
	if (!ParcelGPU.IsValid())
	{
		return;
	}

	{
		FScopeLock L(&ParcelGPU->CounterLock);
		for (int32 i = 0; i < DirtSim::ParcelCounterCount; ++i)
		{
			ParcelCounters[i] = ParcelGPU->Counters_Shared[i];
		}
	}

	if (DustGPU.IsValid())
	{
		FScopeLock L(&DustGPU->CounterLock);
		for (int32 i = 0; i < DirtSim::ParcelCounterCount; ++i)
		{
			DustCounters[i] = DustGPU->Counters_Shared[i];
		}
	}

	TSharedPtr<FDirtParcelResources, ESPMode::ThreadSafe> Keep = ParcelGPU;
	TSharedPtr<FDirtParcelResources, ESPMode::ThreadSafe> KeepDust = DustGPU;
	ENQUEUE_RENDER_COMMAND(DirtParcelCounters)(
		[Keep, KeepDust](FRHICommandListImmediate& RHICmdList)
		{
			DirtSim::UpdateParcelCounters_RenderThread(RHICmdList, *Keep);
			if (KeepDust.IsValid())
			{
				DirtSim::UpdateParcelCounters_RenderThread(RHICmdList, *KeepDust);
			}
		});
}

void ADirtBox::GetDustCounters(uint32 OutCounters[8]) const
{
	for (int32 i = 0; i < DirtSim::ParcelCounterCount; ++i)
	{
		OutCounters[i] = DustCounters[i];
	}
}

int32 ADirtBox::GetLiveDust() const
{
	if (!DustGPU.IsValid() || !DustGPU->bInitialised)
	{
		return 0;
	}
	return FMath::Clamp(DustPoolCount - 1 - static_cast<int32>(DustCounters[DirtSim::ParcelCounterFree]), 0, DustPoolCount);
}

void ADirtBox::GetParcelCounters(uint32 OutCounters[8]) const
{
	for (int32 i = 0; i < DirtSim::ParcelCounterCount; ++i)
	{
		OutCounters[i] = ParcelCounters[i];
	}
}

void ADirtBox::SetParcelsVisible(bool bVisible)
{
	if (ParcelMesh)
	{
		ParcelMesh->SetVisibility(bVisible);
	}
}

int32 ADirtBox::GetLiveParcels() const
{
	if (!ParcelGPU.IsValid() || !ParcelGPU->bInitialised)
	{
		return 0;
	}
	return FMath::Clamp(ParcelPoolCount - 1 - static_cast<int32>(ParcelCounters[DirtSim::ParcelCounterFree]), 0, ParcelPoolCount);
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
									  float DisturbOverride, bool bProctor) const
{
	const float TexelSize = Settings.TexelSizeCm();

	FDirtBrushStroke S;
	S.Mode = static_cast<int32>(Mode);
	S.bProctor = bProctor;
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
		// Amount read as "peak depth in cm" at the middle of the stroke. The
		// layer stores solids, so a depth in bulk centimetres of natural ground
		// becomes that many solid centimetres at the ground's solid fraction: a
		// 15 cm hole in loose dirt is 15 cm deep, and the same stroke in hardpack
		// comes out shallower, which is what a shovel finds too.
		S.Amount = Amount * S.CoreNorm * DirtSolidFraction(Settings.DeepCompaction,
														  SoilAtTexel(FIntPoint(FMath::FloorToInt(S.CenterTexel.X), FMath::FloorToInt(S.CenterTexel.Y))));
	}
	else
	{
		S.Amount = Amount;
	}

	return S;
}

void ADirtBox::ApplyBrush(FVector2D WorldXYCm, float RadiusCm, float Amount, EDirtBrushMode Mode,
						  float DisturbOverride, bool bProctor)
{
	if (!bResourcesReady || RadiusCm <= 0.0f)
	{
		return;
	}

	FDirtBrushStroke Stroke = MakeStroke(WorldXYCm, RadiusCm, Amount, Mode, DisturbOverride, bProctor);
	if (Mode == EDirtBrushMode::Dig || Mode == EDirtBrushMode::Raise)
	{
		// Two halves: take first (and report any shortfall), give afterwards,
		// scaled by that shortfall. Zero-sum even over bedrock.
		const int32 TakeIndex = PendingStrokes.Num();
		PendingStrokes.Add(Stroke);
		FDirtBrushStroke Give = Stroke;
		Give.Mode = (Mode == EDirtBrushMode::Dig) ? 8 : 10;
		Give.Disturb = 0.0f;
		Give.Link = (TakeIndex < DirtSim::MaxStrokesPerStep) ? TakeIndex : -1;
		PendingGiving.Add(Give);
		return;
	}
	PendingStrokes.Add(Stroke);
}

void ADirtBox::PourWater(FVector2D WorldXYCm, float RadiusCm, float Litres)
{
	if (!bResourcesReady || Litres <= 0.0f || RadiusCm <= 0.0f)
	{
		return;
	}

	// Same core kernel and the same exact discrete normalisation as a Scoop, so
	// the pond receives exactly the litres asked for.
	const FDirtBrushStroke Kernel = MakeStroke(WorldXYCm, RadiusCm, 0.0f, EDirtBrushMode::Scoop);
	FDirtWaterSource W;
	W.CenterTexel = Kernel.CenterTexel;
	W.RadiusTexels = Kernel.CoreRadiusTexels;
	W.AmountCm = Litres * 1000.0f / Settings.TexelAreaCm2() / FMath::Max(Kernel.CoreNorm, KINDA_SMALL_NUMBER);
	PendingWater.Add(W);
}

int32 ADirtBox::ScoopDirt(FVector2D FromWorldXYCm, float RadiusCm, float VolumeCm3, float DisturbOverride)
{
	if (!bResourcesReady || VolumeCm3 <= 0.0f || RadiusCm <= 0.0f)
	{
		return -1;
	}

	const float Amount = VolumeCm3 / Settings.TexelAreaCm2();
	PendingStrokes.Add(MakeStroke(FromWorldXYCm, RadiusCm, Amount, EDirtBrushMode::Scoop, DisturbOverride));
	// The stroke's index within the step, so a throw can be shrunk by whatever
	// this scoop fails to find. Beyond the reporting range it is simply unlinked.
	const int32 Index = PendingStrokes.Num() - 1;
	return (Index < DirtSim::MaxStrokesPerStep) ? Index : -1;
}

float ADirtBox::ChooseParcelDiameterCm(float Moisture, float Compaction) const
{
	float D = Settings.ParcelDiameterCm;
	if (D <= 0.0f)
	{
		// A clod holds together while its cohesion beats its own weight stress,
		// c > gamma d, so d ~ c / gamma: with 3 kPa and 17 kN/m3 that is 18 cm —
		// far too big for anything a tyre flings, which is sheared into pieces
		// well below that. Scale it down and let the min/max bound it: dry sand
		// (no cohesion) goes to the minimum, damp loam to a few centimetres.
		float TanPhi = 0.0f, CohesionKPa = 0.0f;
		const FDirtSoil& ClodSoil = Settings.Soil(Settings.DefaultSoil);
		DirtSoilStrength(Compaction, Moisture, ClodSoil, TanPhi, CohesionKPa);
		D = 100.0f * CohesionKPa / FMath::Max(ClodSoil.UnitWeightKNm3, 1.0f) * 0.15f;
	}
	return FMath::Clamp(D, Settings.ParcelMinDiameterCm, Settings.ParcelMaxDiameterCm);
}

void ADirtBox::SpawnParcels(FVector WorldPosCm, FVector VelocityCmS, float SpreadDeg, float VolumeCm3,
							float Moisture, float Compaction, int32 ScoopStrokeIndex)
{
	if (!bResourcesReady || VolumeCm3 <= 0.0f)
	{
		return;
	}

	if (!ParcelGPU.IsValid())
	{
		return;
	}
	// With parcels switched off the request still goes to the GPU, which lands
	// it where it started (deposit-only), corrected for what the scoop found.

	FDirtParcelSpawn S;
	const FVector Origin = GetActorLocation();
	S.PositionCm = FVector3f(WorldPosCm - Origin);
	S.VelocityCmS = FVector3f(VelocityCmS);
	S.VolumeCm3 = VolumeCm3;
	S.SpreadDeg = SpreadDeg;
	S.Moisture = Moisture;
	S.Compaction = Compaction;
	S.DiameterCm = ChooseParcelDiameterCm(Moisture, Compaction);
	S.DiameterJitter = Settings.ParcelDiameterJitter;
	S.SpeedJitter = 0.25f;
	S.Seed = SpawnSeed++;
	S.ScoopStrokeIndex = ScoopStrokeIndex;

	// Split the volume into parcels of the chosen size. A throw bigger than the
	// per-throw cap gets bigger parcels rather than dropping any dirt.
	const float ParcelVolume = PI / 6.0f * S.DiameterCm * S.DiameterCm * S.DiameterCm;
	int32 Count = FMath::Max(1, FMath::RoundToInt(VolumeCm3 / FMath::Max(ParcelVolume, 1e-4f)));
	if (Count > Settings.ParcelMaxPerThrow)
	{
		Count = Settings.ParcelMaxPerThrow;
		S.DiameterCm = FMath::Pow(6.0f * VolumeCm3 / (PI * Count), 1.0f / 3.0f);
	}
	S.Count = Count;
	ParcelSlotsRequested += Count;

	PendingSpawns.Add(S);
}

void ADirtBox::SpawnDust(FVector WorldPosCm, FVector VelocityCmS, float SpreadDeg, int32 Count, float Moisture)
{
	if (!bResourcesReady || !Settings.bParcels || !Settings.bDust || Count <= 0 || !DustGPU.IsValid())
	{
		return;
	}

	FDirtParcelSpawn S;
	S.PositionCm = FVector3f(WorldPosCm - GetActorLocation());
	S.VelocityCmS = FVector3f(VelocityCmS);
	S.VolumeCm3 = 1e-3f * Count;              // a placeholder so the slot reads as live; never audited
	S.Count = FMath::Min(Count, 4096);
	S.SpreadDeg = SpreadDeg;
	S.SpeedJitter = 0.5f;
	S.Moisture = Moisture;
	S.Compaction = 0.0f;
	S.DiameterCm = Settings.DustDiameterCm;
	S.DiameterJitter = 0.5f;
	S.Seed = SpawnSeed++;
	DustSlotsRequested += S.Count;
	PendingDust.Add(S);
}

void ADirtBox::TransferDirt(FVector2D FromWorldXYCm, float FromRadiusCm, FVector2D ToWorldXYCm, float ToRadiusCm, float VolumeCm3, float DisturbOverride)
{
	if (!bResourcesReady || VolumeCm3 <= 0.0f || FromRadiusCm <= 0.0f || ToRadiusCm <= 0.0f)
	{
		return;
	}

	// Scoop/Dump amounts are layer-height sums in cm x texel^2: the shader adds
	// Amount * CoreW per texel and the core weights sum to exactly 1. The dump
	// is a giving half linked to the scoop, so it gives only what was found.
	const float Amount = VolumeCm3 / Settings.TexelAreaCm2();
	const int32 TakeIndex = PendingStrokes.Num();
	PendingStrokes.Add(MakeStroke(FromWorldXYCm, FromRadiusCm, Amount, EDirtBrushMode::Scoop, DisturbOverride));
	FDirtBrushStroke Give = MakeStroke(ToWorldXYCm, ToRadiusCm, Amount, EDirtBrushMode::Dump);
	Give.Link = (TakeIndex < DirtSim::MaxStrokesPerStep) ? TakeIndex : -1;
	PendingGiving.Add(Give);
}

void ADirtBox::ResetToTestbed()
{
	if (!bResourcesReady)
	{
		return;
	}

	// Everything queued was scooped from the ground that is about to be thrown
	// away. Letting it land on the new ground would be dirt from nowhere.
	PendingStrokes.Reset();
	PendingGiving.Reset();
	PendingSpawns.Reset();
	PendingDust.Reset();
	PendingWater.Reset();
	// The world starts over: every tile pristine, nothing stored.
	HarvestTileReadbacks(true);
	InFlightTileReadbacks.Reset();
	PendingTileReadbacks.Reset();
	PendingShiftTexels = FIntPoint::ZeroValue;
	TileCache.Empty();
	BaselineVolumeM3 = 0.0;
	WorldStoredM3 = 0.0;
	BuildTerrainAndUpload();
	BuildFarMesh();
	bNeedsReinit = true;

	UE_LOG(LogDirt, Log, TEXT("Dirtbox reset. Baseline volume %.3f m3."), BaselineVolumeM3);
}

void ADirtBox::RebuildTerrainAndMesh()
{
	PendingStrokes.Reset();
	PendingGiving.Reset();
	PendingSpawns.Reset();        // scooped from ground that no longer exists
	PendingDust.Reset();
	PendingWater.Reset();
	Readback.Empty();
	HarvestTileReadbacks(true);
	InFlightTileReadbacks.Reset();
	PendingTileReadbacks.Reset();
	PendingShiftTexels = FIntPoint::ZeroValue;
	TileCache.Empty();
	BaselineVolumeM3 = 0.0;
	WorldStoredM3 = 0.0;
	++WindowGeneration;

	// The simulation textures keep their resolution; only the ground they cover
	// and the mesh that displays it change.
	BuildTerrainAndUpload();
	BuildDisplayMesh();
	BuildFarMesh();
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
	bFollowWheel = false;
	BuildWholeSiteLog();
	WindowTile = TileForCentre(Settings.SimRegionCentreCm, Settings.RegionSizeCm());
	RebuildTerrainAndMesh();

	for (const FString& Line : FeatureLog)
	{
		UE_LOG(LogDirt, Log, TEXT("  %s"), *Line);
	}
}

void ADirtBox::SetSimRegion(FVector2D CentreCm, float SizeCm)
{
	const float Clamped = (SizeCm > 0.0f) ? FMath::Clamp(SizeCm, 200.0f, Settings.WorldSizeCm) : 0.0f;
	const float OldCell = Settings.TexelSizeCm();

	Settings.SimRegionSizeCm = Clamped;
	const float RegionCm = Settings.RegionSizeCm();
	const FIntPoint NewTile = TileForCentre(CentreCm, RegionCm);
	const float TileCm = RegionCm / TilesPerSide;
	const FVector2D SnappedCentre(-Settings.WorldSizeCm * 0.5 + (NewTile.X + TilesPerSide * 0.5) * TileCm,
								  -Settings.WorldSizeCm * 0.5 + (NewTile.Y + TilesPerSide * 0.5) * TileCm);

	if (!bResourcesReady)
	{
		WindowTile = NewTile;
		Settings.SimRegionCentreCm = SnappedCentre;
		return;
	}

	const bool bSameCells = FMath::IsNearlyEqual(Settings.TexelSizeCm(), OldCell, 1e-3f);
	if (bSameCells && NewTile != WindowTile)
	{
		// Same world, different place: slide there and keep every rut.
		ShiftWindow(NewTile - WindowTile);
	}
	else if (!bSameCells)
	{
		// New cell size: a new world. The cache is at the old size and is thrown away.
		WindowTile = NewTile;
		Settings.SimRegionCentreCm = SnappedCentre;
		RebuildTerrainAndMesh();
	}

	UE_LOG(LogDirt, Log, TEXT("Sim region: %.1f m square centred on (%.1f, %.1f) m -> %.2f cm per cell, tiles of %.1f m, window tile (%d, %d)."),
		Settings.RegionSizeCm() * 0.01f,
		Settings.SimRegionCentreCm.X * 0.01, Settings.SimRegionCentreCm.Y * 0.01,
		Settings.TexelSizeCm(), TileCm * 0.01f, WindowTile.X, WindowTile.Y);
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

	PondReadback.Reset();
	if (PondA)
	{
		if (FTextureRenderTargetResource* PondRes = PondA->GameThread_GetRenderTargetResource())
		{
			TArray<FLinearColor> Pixels;
			PondRes->ReadLinearColorPixels(Pixels);
			PondReadback.SetNumUninitialized(Pixels.Num());
			SedimentReadback.SetNumUninitialized(Pixels.Num());
			SkinReadback.SetNumUninitialized(Pixels.Num());
			BaseReadback.SetNumUninitialized(Pixels.Num());
			for (int32 i = 0; i < Pixels.Num(); ++i)
			{
				PondReadback[i] = Pixels[i].R;
				SedimentReadback[i] = Pixels[i].G;
				SkinReadback[i] = Pixels[i].B;
				BaseReadback[i] = Pixels[i].A;
			}
		}
	}
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

	double SumSolidCm = 0.0;
	double SumBulkCm = 0.0;
	double SumWaterCm = 0.0;
	double SumPondCm = 0.0;
	double SumSuspendedCm = 0.0;
	float MinLayer = MAX_flt;
	float MaxLayer = -MAX_flt;
	int32 Exposed = 0;

	for (int32 i = 0; i < Res * Res; ++i)
	{
		const FLinearColor& S = Readback[i];
		const FDirtSoil& CellSoil = Settings.Soil(SoilId.IsValidIndex(i) ? SoilId[i] : Settings.DefaultSoil);
		// The column is a skin over a base: bulk and pore water count both.
		float SkinCm = SkinReadback.IsValidIndex(i) ? SkinReadback[i] : 0.0f;
		float BaseC = S.G, BaseM = S.B;
		if (SkinCm > 1e-4f && BaseReadback.IsValidIndex(i))
		{
			DirtUnpackBase(BaseReadback[i], BaseC, BaseM);
		}
		else
		{
			SkinCm = 0.0f;
		}
		const float Bulk = DirtBulkCmSkin(S.R, SkinCm, S.G, BaseC, CellSoil, Settings);
		SumSolidCm += S.R;
		SumBulkCm += Bulk;
		const float SkinPart = FMath::Min(SkinCm, Bulk);
		SumWaterCm += FMath::Clamp(S.B, 0.0f, 1.0f) * (1.0f - DirtSolidFraction(S.G, CellSoil)) * SkinPart
					+ FMath::Clamp(BaseM, 0.0f, 1.0f) * (1.0f - DirtSolidFraction(BaseC, CellSoil)) * FMath::Min(Bulk - SkinPart, Settings.WetDepthCm);
		if (PondReadback.IsValidIndex(i))
		{
			SumPondCm += PondReadback[i];
		}
		if (SedimentReadback.IsValidIndex(i))
		{
			SumSuspendedCm += SedimentReadback[i];
		}
		MinLayer = FMath::Min(MinLayer, Bulk);
		MaxLayer = FMath::Max(MaxLayer, Bulk);
		if (S.R < 0.5f)
		{
			++Exposed;
		}
		if (S.R < 0.0f)
		{
			++Audit.NegativeCells;
			Audit.NegativeCm3 += S.R;
		}
		Audit.MinSolidCm = FMath::Min(Audit.MinSolidCm, S.R);
	}
	Audit.NegativeCm3 *= Settings.TexelAreaCm2();

	const double AreaM3PerCm = Settings.TexelAreaCm2() / 1000000.0;
	Audit.GroundM3 = SumSolidCm * AreaM3PerCm;
	Audit.BulkM3 = SumBulkCm * AreaM3PerCm;
	Audit.PoreWaterM3 = SumWaterCm * AreaM3PerCm;
	Audit.PondM3 = SumPondCm * AreaM3PerCm;
	Audit.SuspendedM3 = SumSuspendedCm * AreaM3PerCm;

	// Dirt in the air, parcel by parcel. Blocking, like the rest of the audit.
	if (Settings.bParcels && ParcelPosRT && ParcelGPU.IsValid() && ParcelGPU->bInitialised)
	{
		if (FTextureRenderTargetResource* PosRes = ParcelPosRT->GameThread_GetRenderTargetResource())
		{
			TArray<FLinearColor> Pos;
			PosRes->ReadLinearColorPixels(Pos);
			double AirCm3 = 0.0;
			for (const FLinearColor& P : Pos)
			{
				if (P.A > 0.0f)
				{
					AirCm3 += P.A;
					++Audit.LiveParcels;
				}
			}
			Audit.AirborneM3 = AirCm3 / 1000000.0;
		}
	}

	// Tiles out of the window hold dirt too. Anything still on its way to the
	// cache is waited for, so the books are complete.
	HarvestTileReadbacks(true);
	Audit.StoredM3 = WorldStoredM3;
	Audit.VolumeM3 = Audit.GroundM3 + Audit.AirborneM3 + Audit.StoredM3 + Audit.SuspendedM3;
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

FLinearColor ADirtBox::GetStateAtWorld(FVector2D WorldXYCm) const
{
	const int32 Res = Settings.SimResolution;
	if (Readback.Num() < Res * Res)
	{
		return FLinearColor(0, 0, 0, 0);
	}
	const FVector2f T = WorldToTexel(WorldXYCm);
	const int32 X = FMath::Clamp(FMath::FloorToInt(T.X), 0, Res - 1);
	const int32 Y = FMath::Clamp(FMath::FloorToInt(T.Y), 0, Res - 1);
	return Readback[Y * Res + X];
}

float ADirtBox::GetPondAtWorld(FVector2D WorldXYCm) const
{
	const int32 Res = Settings.SimResolution;
	if (PondReadback.Num() < Res * Res)
	{
		return 0.0f;
	}
	const FVector2f T = WorldToTexel(WorldXYCm);
	const int32 X = FMath::Clamp(FMath::FloorToInt(T.X), 0, Res - 1);
	const int32 Y = FMath::Clamp(FMath::FloorToInt(T.Y), 0, Res - 1);
	return PondReadback[Y * Res + X];
}

float ADirtBox::GetSkinAtWorld(FVector2D WorldXYCm, float* OutBaseCompaction, float* OutBaseMoisture) const
{
	const int32 Res = Settings.SimResolution;
	if (SkinReadback.Num() < Res * Res || BaseReadback.Num() < Res * Res)
	{
		return 0.0f;
	}
	const FVector2f T = WorldToTexel(WorldXYCm);
	const int32 X = FMath::Clamp(FMath::FloorToInt(T.X), 0, Res - 1);
	const int32 Y = FMath::Clamp(FMath::FloorToInt(T.Y), 0, Res - 1);
	const int32 I = Y * Res + X;
	float C = 0.0f, M = 0.0f;
	DirtUnpackBase(BaseReadback[I], C, M);
	if (OutBaseCompaction) *OutBaseCompaction = C;
	if (OutBaseMoisture) *OutBaseMoisture = M;
	return SkinReadback[I];
}

float ADirtBox::GetSedimentAtWorld(FVector2D WorldXYCm) const
{
	const int32 Res = Settings.SimResolution;
	if (SedimentReadback.Num() < Res * Res)
	{
		return 0.0f;
	}
	const FVector2f T = WorldToTexel(WorldXYCm);
	const int32 X = FMath::Clamp(FMath::FloorToInt(T.X), 0, Res - 1);
	const int32 Y = FMath::Clamp(FMath::FloorToInt(T.Y), 0, Res - 1);
	return SedimentReadback[Y * Res + X];
}

float ADirtBox::MaxScoopCm3(float SolidCm, float RadiusCm) const
{
	const float RadiusTexels = FMath::Max(RadiusCm / Settings.TexelSizeCm(), 1.0f);
	// Integral of (1 - t^2)^2 over the disc, in cells; never below the one cell a tiny kernel lands on.
	const float CoreNorm = FMath::Max(1.0f, PI * RadiusTexels * RadiusTexels / 3.0f);
	// A third: the layer under the centre is a bilinear sample a frame or two old.
	return 0.35f * FMath::Max(SolidCm, 0.0f) * Settings.TexelAreaCm2() * CoreNorm;
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

	// The display texture carries the water top; the dirt is that minus the pond.
	float PondHere = 0.0f;
	if (PondReadback.Num() >= Res * Res)
	{
		PondHere = FMath::Lerp(FMath::Lerp(PondReadback[Y0 * Res + X0], PondReadback[Y0 * Res + X1], FX),
							   FMath::Lerp(PondReadback[Y1 * Res + X0], PondReadback[Y1 * Res + X1], FX), FY);
	}
	return (GetActorLocation().Z + FMath::Lerp(Bottom, Top, FY)) - PondHere;
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
			TEXT("max loose slope to land near %.0f deg."), Settings.Soil(Settings.DefaultSoil).LooseReposeDeg);
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
			Wedges.Num(), Wedges[0].AngleDeg, Wedges.Last().AngleDeg, Settings.Soil(Settings.DefaultSoil).LooseReposeDeg);
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
	TEXT("DaDirt.DebugView <0-8> - 0 dirt, 1 layer depth, 2 compaction, 3 moisture, 7 skin thickness, 8 base moisture, ")
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
			UE_LOG(LogDirt, Log, TEXT("--- volume audit (solid m3: grains only, voids squeezed out) ---"));
			UE_LOG(LogDirt, Log, TEXT("  in the ground     %.4f m3   (%.4f m3 bulk, the way a ruler sees it)"), A.GroundM3, A.BulkM3);
			UE_LOG(LogDirt, Log, TEXT("  in the air        %.4f m3   (%d parcels)"), A.AirborneM3, A.LiveParcels);
			UE_LOG(LogDirt, Log, TEXT("  out of the window %.4f m3   (%d tiles cached, %d slides)"), A.StoredM3, Box->GetCachedTileCount(), Box->GetShiftCount());
			UE_LOG(LogDirt, Log, TEXT("  in the run-off    %.4f m3   (suspended in water)"), A.SuspendedM3);
			UE_LOG(LogDirt, Log, TEXT("  total             %.4f m3"), A.VolumeM3);
			UE_LOG(LogDirt, Log, TEXT("  baseline          %.4f m3"), A.BaselineM3);
			UE_LOG(LogDirt, Log, TEXT("  drift             %+.5f m3  (%+.4f %%)  = %+.2f cm3"), A.DriftM3, A.DriftPercent, A.DriftM3 * 1000000.0);
			UE_LOG(LogDirt, Log, TEXT("  water             %.3f m3 in the pores, %.4f m3 ponded"), A.PoreWaterM3, A.PondM3);
			UE_LOG(LogDirt, Log, TEXT("  layer min / max   %.1f / %.1f cm (bulk)"), A.MinLayerCm, A.MaxLayerCm);
			UE_LOG(LogDirt, Log, TEXT("  scraped to rock   %d cells"), A.BedrockExposedCells);
			if (A.NegativeCells > 0)
			{
				UE_LOG(LogDirt, Warning, TEXT("  NEGATIVE dirt     %d cells, %.3f cm3 in total, worst %.5f cm"), A.NegativeCells, A.NegativeCm3, A.MinSolidCm);
			}
			UE_LOG(LogDirt, Log, TEXT("  steepest loose    %.1f deg  (repose setting %.1f)"),
				A.MaxLooseSlopeDeg, Box->Settings.Soil(Box->Settings.DefaultSoil).LooseReposeDeg);
			UE_LOG(LogDirt, Log, TEXT("  steepest anywhere %.1f deg"), A.MaxAnySlopeDeg);
		}));

static FAutoConsoleCommandWithWorldAndArgs GDirtEvapCmd(
	TEXT("DaDirt.Evap"),
	TEXT("DaDirt.Evap <multiplier> - scale the drying rate (1 = the game pace). A test that wants a crust in a minute asks for 20."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld*)
		{
			if (ADirtBox* Box = GetDirtBoxOrWarn())
			{
				Box->Settings.EvapMultiplier = FMath::Max(ArgFloat(Args, 0, 1.0f), 0.0f);
				UE_LOG(LogDirt, Log, TEXT("Drying at %.1f x the game pace."), Box->Settings.EvapMultiplier);
			}
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

			// The readback is the simulated window; a point outside it would read
			// the nearest edge cell and look like an answer.
			const float HalfRegion = Box->Settings.RegionSizeCm() * 0.5f;
			if (FMath::Abs(Xm * 100.0 - Box->Settings.SimRegionCentreCm.X) > HalfRegion
				|| FMath::Abs(Ym * 100.0 - Box->Settings.SimRegionCentreCm.Y) > HalfRegion)
			{
				UE_LOG(LogDirt, Warning, TEXT("(%.1f, %.1f) m is outside the simulated window (%.0f m square at (%.0f, %.0f) m). DaDirt.Focus there first."),
					Xm, Ym, Box->Settings.RegionSizeCm() * 0.01f, Box->Settings.SimRegionCentreCm.X * 0.01, Box->Settings.SimRegionCentreCm.Y * 0.01);
				return;
			}

			Box->RefreshReadback();
			const FLinearColor S = Box->GetStateAtWorld(World);
			const float PondHere = Box->GetPondAtWorld(World);
			const float SurfaceZ = Box->GetSurfaceHeightAtWorld(World);
			const float SedHere = Box->GetSedimentAtWorld(World);
			const FDirtSoil& ProbeSoil = Box->SoilAtWorld(World);
			float BaseC = S.G, BaseM = S.B;
			const float SkinCm = Box->GetSkinAtWorld(World, &BaseC, &BaseM);
			const bool bSkin = SkinCm > 1e-4f;
			UE_LOG(LogDirt, Log, TEXT("Surface at (%.1f, %.1f) m is Z = %.1f cm  [%s, layer %.1f cm bulk / %.1f solid, compaction %.2f, moisture %.2f, pond %.2f cm%s%s]"),
				Xm, Ym, SurfaceZ, *ProbeSoil.Name, DirtBulkCmSkin(S.R, bSkin ? SkinCm : 0.0f, S.G, bSkin ? BaseC : S.G, ProbeSoil, Box->Settings), S.R, S.G, S.B, PondHere,
				PondHere > 0.05f ? *FString::Printf(TEXT(", water top at Z = %.1f cm, %.3f cm of dirt in it"), SurfaceZ + PondHere, SedHere) : TEXT(""),
				bSkin ? *FString::Printf(TEXT(", skin %.1f cm over base compaction %.2f moisture %.2f"), SkinCm, BaseC, BaseM) : TEXT(""));
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

			FDirtSimSettings& Settings = Box->Settings;
			// Edits the default soil, or the one named as a fifth argument.
			int32 SoilIndex = Settings.DefaultSoil;
			if (Args.IsValidIndex(4))
			{
				const int32 Named = Settings.FindSoil(Args[4]);
				if (Named >= 0) { SoilIndex = Named; }
			}
			if (!Settings.Soils.IsValidIndex(SoilIndex)) { return; }
			FDirtSoil& S = Settings.Soils[SoilIndex];
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

static FAutoConsoleCommandWithWorldAndArgs GDirtErosionCmd(
	TEXT("DaDirt.Erosion"),
	TEXT("DaDirt.Erosion [0|1] - run-off carrying dirt (detachment, transport, settling). On by default; off for attribution."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld*)
		{
			if (ADirtBox* Box = GetDirtBoxOrWarn())
			{
				Box->Settings.bErosion = Args.IsValidIndex(0) ? (ArgInt(Args, 0, 1) != 0) : !Box->Settings.bErosion;
				UE_LOG(LogDirt, Log, TEXT("Erosion %s."), Box->Settings.bErosion ? TEXT("on") : TEXT("off"));
			}
		}));

static FAutoConsoleCommandWithWorldAndArgs GDirtSoilCmd(
	TEXT("DaDirt.Soil"),
	TEXT("DaDirt.Soil - list the soils. DaDirt.Soil <name|id> - paint the whole window with it. ")
	TEXT("DaDirt.Soil <name|id> <xM> <yM> <radiusM> - paint a disc. DaDirt.Soil default <name> - the soil tools and tests use. ")
	TEXT("A whole-window paint sticks through Reset and slides until DaDirt.Soil built."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld*)
		{
			ADirtBox* Box = GetDirtBoxOrWarn();
			if (!Box)
			{
				return;
			}
			FDirtSimSettings& Settings = Box->Settings;
			if (Args.Num() == 0)
			{
				for (int32 i = 0; i < Settings.Soils.Num(); ++i)
				{
					const FDirtSoil& So = Settings.Soils[i];
					UE_LOG(LogDirt, Log, TEXT("  %d %-8s%s friction %.0f/%.0f deg, cohesion %.1f+%.1f kPa, K %.2f cm/s, field capacity %.2f, dust x%.1f"),
						i, *So.Name, i == Settings.DefaultSoil ? TEXT(" (default)") : TEXT(""),
						So.LooseReposeDeg, So.PackedReposeDeg, So.SuctionCohesionKPa, So.PackedCohesionKPa,
						So.InfiltrationCmPerSec, So.FieldCapacity, So.Dustiness);
				}
				return;
			}
			const auto Resolve = [&Settings](const FString& Arg) -> int32
			{
				const int32 Named = Settings.FindSoil(Arg);
				if (Named >= 0) { return Named; }
				const int32 Id = FCString::Atoi(*Arg);
				return (Arg.IsNumeric() && Settings.Soils.IsValidIndex(Id)) ? Id : -1;
			};
			if (Args[0].Equals(TEXT("built"), ESearchCase::IgnoreCase))
			{
				// Back to the soil map the builders made. Takes effect on the next Reset.
				Box->SoilOverride = -1;
				UE_LOG(LogDirt, Log, TEXT("The built soil map is back after the next DaDirt.Reset."));
				return;
			}
			if (Args[0].Equals(TEXT("default"), ESearchCase::IgnoreCase))
			{
				const int32 Id = Args.IsValidIndex(1) ? Resolve(Args[1]) : -1;
				if (Id < 0) { UE_LOG(LogDirt, Warning, TEXT("No such soil.")); return; }
				Settings.DefaultSoil = Id;
				UE_LOG(LogDirt, Log, TEXT("Default soil: %s."), *Settings.Soils[Id].Name);
				return;
			}
			const int32 Id = Resolve(Args[0]);
			if (Id < 0)
			{
				UE_LOG(LogDirt, Warning, TEXT("No such soil: %s. DaDirt.Soil lists them."), *Args[0]);
				return;
			}
			if (Args.Num() >= 4)
			{
				Box->PaintSoil(Id, FVector2D(ArgFloat(Args, 1, 0.0f) * 100.0f, ArgFloat(Args, 2, 0.0f) * 100.0f), ArgFloat(Args, 3, 5.0f) * 100.0f);
				UE_LOG(LogDirt, Log, TEXT("Painted %s in a %.1f m disc at (%.1f, %.1f) m."), *Settings.Soils[Id].Name, ArgFloat(Args, 3, 5.0f), ArgFloat(Args, 1, 0.0f), ArgFloat(Args, 2, 0.0f));
			}
			else
			{
				Box->PaintSoil(Id, FVector2D::ZeroVector, 0.0f);
				Settings.DefaultSoil = Id;
				UE_LOG(LogDirt, Log, TEXT("The whole window is %s now (and it is the default)."), *Settings.Soils[Id].Name);
			}
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
	TEXT("DaDirt.Focus <xM> <yM> [sizeM=51.2] | follow [sizeM] | off - point the simulation at part of ")
	TEXT("the box. Same 1024 cells over less ground means finer cells: 51 m gives 5 cm ")
	TEXT("cells. The window slides over the world in tiles and keeps every rut it leaves behind; ")
	TEXT("'follow' keeps it centred on the wheel."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld* World)
		{
			ADirtBox* Box = GetDirtBoxOrWarn();
			if (!Box)
			{
				return;
			}

			if (Args.IsValidIndex(0) && Args[0].ToLower() == TEXT("off"))
			{
				Box->SetFollowWheel(false);
				Box->SetSimRegion(FVector2D::ZeroVector, 0.0f);
				UE_LOG(LogDirt, Log, TEXT("Simulating the whole box again."));
				return;
			}

			if (Args.IsValidIndex(0) && Args[0].ToLower() == TEXT("follow"))
			{
				// Centre on the wheel and keep it there, sliding by tiles as it drives.
				FVector2D Centre = Box->Settings.SimRegionCentreCm;
				for (TActorIterator<ADirtWheel> It(World); It; ++It)
				{
					Centre = FVector2D(It->GetActorLocation() - Box->GetActorLocation());
					break;
				}
				const float SizeM = ArgFloat(Args, 1, Box->Settings.IsFocused() ? Box->Settings.RegionSizeCm() * 0.01f : 40.0f);
				Box->SetSimRegion(Centre, SizeM * 100.0f);
				Box->SetFollowWheel(true);
				UE_LOG(LogDirt, Log, TEXT("Following the wheel: the window slides by %.1f m tiles and every rut is kept."),
					Box->Settings.RegionSizeCm() * 0.01f / 4.0f);
				return;
			}

			if (Args.Num() < 2)
			{
				UE_LOG(LogDirt, Warning, TEXT("Usage: DaDirt.Focus <xM> <yM> [sizeM] | follow [sizeM] | off"));
				return;
			}

			const float Xm = ArgFloat(Args, 0, 0.0f);
			const float Ym = ArgFloat(Args, 1, 0.0f);
			const float SizeM = ArgFloat(Args, 2, 51.2f);

			Box->SetFollowWheel(false);
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
	TEXT("DaDirt.Wheel <Xm> <Ym> [headingDeg=0] [dropCm=3] - put the powered test wheel on the dirt (replaces any existing one), dropped from dropCm above it. ")
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
			const float DropCm = FMath::Clamp(ArgFloat(Args, 3, 3.0f), 0.0f, 2000.0f);
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
			// Set down just clear of the ground unless a drop is asked for: dropped
			// from 25 cm, as it used to be, every placement was a landing that
			// packed and cratered its spot. (The axle sits a tyre radius, 35 cm, up.)
			ADirtWheel* Wheel = World->SpawnActor<ADirtWheel>(ADirtWheel::StaticClass(),
				FVector(World2D.X, World2D.Y, Ground + 35.0f + DropCm), FRotator(0.0f, HeadingDeg, 0.0f), Params);
			if (Wheel)
			{
				UE_LOG(LogDirt, Log, TEXT("Test wheel at (%.1f, %.1f) m heading %.0f deg, %.0f cm up. DaDirt.Drive <throttle> <steer> to go."),
					Xm, Ym, HeadingDeg, DropCm);
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

static FAutoConsoleCommandWithWorldAndArgs GDirtLapCmd(
	TEXT("DaDirt.Lap"),
	TEXT("DaDirt.Lap [throttle=0.5] [xM yM] - in track mode, put the test wheel on the centre line (at the start, or the point nearest x y) ")
	TEXT("and have it follow the line lap after lap. DaDirt.Drive takes the steering back."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld* World)
		{
			ADirtBox* Box = GetDirtBoxOrWarn();
			if (!Box || !World)
			{
				return;
			}
			const TArray<FVector2D>& Line = Box->TrackLineM;
			if (Line.Num() < 3)
			{
				UE_LOG(LogDirt, Warning, TEXT("DaDirt.Lap needs the track: DaDirt.Mode track first."));
				return;
			}
			int32 Start = 0;
			if (Args.Num() >= 3)
			{
				const FVector2D Want(ArgFloat(Args, 1, 0.0f), ArgFloat(Args, 2, 0.0f));
				double Best = 1e18;
				for (int32 i = 0; i < Line.Num(); ++i)
				{
					const double D = FVector2D::DistSquared(Line[i], Want);
					if (D < Best) { Best = D; Start = i; }
				}
			}
			const FVector2D P = Line[Start];
			const FVector2D Dir = (Line[(Start + 4) % Line.Num()] - P).GetSafeNormal();
			const float HeadingDeg = FMath::RadiansToDegrees(FMath::Atan2(Dir.Y, Dir.X));

			for (TActorIterator<ADirtWheel> It(World); It; ++It)
			{
				It->Destroy();
			}
			const FVector Origin = Box->GetActorLocation();
			const FVector2D World2D(Origin.X + P.X * 100.0, Origin.Y + P.Y * 100.0);
			Box->RefreshReadback();
			const float Ground = Box->GetSurfaceHeightAtWorld(World2D);
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			ADirtWheel* Wheel = World->SpawnActor<ADirtWheel>(ADirtWheel::StaticClass(),
				FVector(World2D.X, World2D.Y, Ground + 38.0f), FRotator(0.0f, HeadingDeg, 0.0f), Params);
			if (Wheel)
			{
				Wheel->SetInputs(FMath::Clamp(ArgFloat(Args, 0, 0.5f), -1.0f, 1.0f), 0.0f, 0.0f);
				Wheel->SetLap();
				UE_LOG(LogDirt, Log, TEXT("Test wheel on the centre line at (%.1f, %.1f) m heading %.0f deg, following the lap (%d points)."),
					P.X, P.Y, HeadingDeg, Line.Num());
			}
		}));

static FAutoConsoleCommandWithWorldAndArgs GDirtTyreCmd(
	TEXT("DaDirt.Tyre"),
	TEXT("DaDirt.Tyre <rear|front|sand|hard> - fit the test wheel with a real tyre: the 110/90-19 soft-intermediate rear (default), ")
	TEXT("the 80/100-21 front, a sand rear or a hard-terrain rear. Prints its sizes, deflection and hard-ground patch."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld* World)
		{
			for (TActorIterator<ADirtWheel> It(World); It; ++It)
			{
				if (!Args.IsValidIndex(0) || !It->SetTyrePreset(Args[0]))
				{
					UE_LOG(LogDirt, Warning, TEXT("Usage: DaDirt.Tyre rear | front | sand | hard"));
				}
				return;
			}
			UE_LOG(LogDirt, Warning, TEXT("No test wheel. DaDirt.Wheel <x> <y> first."));
		}));

static FAutoConsoleCommandWithWorldAndArgs GDirtOrbitCmd(
	TEXT("DaDirt.Orbit"),
	TEXT("DaDirt.Orbit <cxM> <cyM> <radiusM> [throttle=0.5] - the test wheel steers itself round that circle, lap after lap, ")
	TEXT("the way it is already pointing. DaDirt.Drive takes the steering back."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld* World)
		{
			if (Args.Num() < 3)
			{
				UE_LOG(LogDirt, Warning, TEXT("Usage: DaDirt.Orbit <cxM> <cyM> <radiusM> [throttle]"));
				return;
			}
			ADirtBox* Box = GetDirtBoxOrWarn();
			if (!Box)
			{
				return;
			}
			for (TActorIterator<ADirtWheel> It(World); It; ++It)
			{
				const FVector Origin = Box->GetActorLocation();
				It->SetInputs(FMath::Clamp(ArgFloat(Args, 3, 0.5f), -1.0f, 1.0f), 0.0f, 0.0f);
				It->SetOrbit(FVector2D(Origin.X / 100.0 + ArgFloat(Args, 0, 0.0f), Origin.Y / 100.0 + ArgFloat(Args, 1, 0.0f)),
							 ArgFloat(Args, 2, 8.0f));
				return;
			}
			UE_LOG(LogDirt, Warning, TEXT("No test wheel. DaDirt.Wheel <x> <y> first."));
		}));

static FAutoConsoleCommandWithWorldAndArgs GDirtWheelPartsCmd(
	TEXT("DaDirt.WheelParts"),
	TEXT("DaDirt.WheelParts <rut> <pack> <roost> <spray> <splash> [plough=1] - 0/1 each; switch the wheel's marks on the dirt off one at a time."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld* World)
		{
			for (TActorIterator<ADirtWheel> It(World); It; ++It)
			{
				It->bPartRut = ArgInt(Args, 0, 1) != 0;
				It->bPartPack = ArgInt(Args, 1, 1) != 0;
				It->bPartRoost = ArgInt(Args, 2, 1) != 0;
				It->bPartSpray = ArgInt(Args, 3, 1) != 0;
				It->bPartSplash = ArgInt(Args, 4, 1) != 0;
				It->bPartPlough = ArgInt(Args, 5, 1) != 0;
				UE_LOG(LogDirt, Log, TEXT("Wheel parts: rut %d pack %d roost %d spray %d splash %d plough %d."),
					It->bPartRut, It->bPartPack, It->bPartRoost, It->bPartSpray, It->bPartSplash, It->bPartPlough);
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
				UE_LOG(LogDirt, Log, TEXT("  sim region     %.1f m square at (%.1f, %.1f) m, window tile (%d, %d), %d tiles cached%s  [DaDirt.Focus off to widen]"),
					S.RegionSizeCm() * 0.01f,
					S.SimRegionCentreCm.X * 0.01, S.SimRegionCentreCm.Y * 0.01,
					Box->GetWindowTile().X, Box->GetWindowTile().Y, Box->GetCachedTileCount(),
					Box->IsFollowingWheel() ? TEXT(", following the wheel") : TEXT(""));
			}
			else
			{
				UE_LOG(LogDirt, Log, TEXT("  sim region     the whole box"));
			}
			UE_LOG(LogDirt, Log, TEXT("  sim grid       %d x %d (%.2f cm per cell, a 12 cm rut is %.1f cells)"),
				S.SimResolution, S.SimResolution, S.TexelSizeCm(), 12.0f / FMath::Max(S.TexelSizeCm(), 0.01f));
			UE_LOG(LogDirt, Log, TEXT("  display mesh   %d x %d verts"), S.MeshVertsPerSide, S.MeshVertsPerSide);
			for (int32 i = 0; i < S.Soils.Num(); ++i)
			{
				const FDirtSoil& So = S.Soils[i];
				UE_LOG(LogDirt, Log, TEXT("  soil %d %-8s %s friction %.0f/%.0f deg (x%.2f wet), cohesion %.1f+%.1f kPa, %.1f kN/m3, porosity %.2f/%.2f, K %.2f cm/s, field cap %.2f, Proctor %.2f, dries x%.1f, dust x%.1f"),
					i, *So.Name, i == S.DefaultSoil ? TEXT("*") : TEXT(" "),
					So.LooseReposeDeg, So.PackedReposeDeg, 1.0f - So.SaturationFrictionLoss,
					So.SuctionCohesionKPa, So.PackedCohesionKPa, So.UnitWeightKNm3, So.LoosePorosity, So.DensePorosity,
					So.InfiltrationCmPerSec, So.FieldCapacity, So.ProctorOptimum, So.DryingMultiplier, So.Dustiness);
			}
			UE_LOG(LogDirt, Log, TEXT("  slump          %d iterations at rate %.3f, %.0f Hz fixed step"),
				S.SlumpIterations, S.SlumpRate, S.SimHz);
			UE_LOG(LogDirt, Log, TEXT("  solids         packing reaches %.0f cm, natural ground at %.2f"),
				S.CompactionDepthCm, S.DeepCompaction);
			UE_LOG(LogDirt, Log, TEXT("  water          %s, soaks in at %.2f cm/s loose, rain %.1f mm/min"),
				S.bWater ? TEXT("on") : TEXT("off"), S.Soil(S.DefaultSoil).InfiltrationCmPerSec, S.RainCmPerSec * 600.0f);
			UE_LOG(LogDirt, Log, TEXT("  parcels        %s, pool %d, %s, %d live; shedding %s; dust %s, pool %d, %d live"),
				S.bParcels ? TEXT("on") : TEXT("off"), S.ParcelPoolSide * S.ParcelPoolSide,
				S.ParcelDiameterCm > 0.0f ? *FString::Printf(TEXT("%.2f cm"), S.ParcelDiameterCm) : TEXT("sized by the soil"),
				Box->GetLiveParcels(), S.bShed ? TEXT("on") : TEXT("off"), S.bDust ? TEXT("on") : TEXT("off"),
				S.DustPoolSide * S.DustPoolSide, Box->GetLiveDust());

			UE_LOG(LogDirt, Log, TEXT("--- testbed ---"));
			for (const FString& Line : Box->GetFeatureLog())
			{
				UE_LOG(LogDirt, Log, TEXT("  %s"), *Line);
			}
		}));

static FAutoConsoleCommandWithWorldAndArgs GDirtRainCmd(
	TEXT("DaDirt.Rain"),
	TEXT("DaDirt.Rain <mm per minute> [seconds=-1 forever] - rain on the whole box. 0 stops it. ")
	TEXT("A real downpour is 20-50 mm/h; game-paced water, so 60 mm/min soaks the pad in a few seconds."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld*)
		{
			ADirtBox* Box = GetDirtBoxOrWarn();
			if (!Box)
			{
				return;
			}
			const float MmPerMin = FMath::Max(ArgFloat(Args, 0, 0.0f), 0.0f);
			Box->Settings.RainCmPerSec = MmPerMin * 0.1f / 60.0f;
			Box->Settings.RainSecondsLeft = ArgFloat(Args, 1, -1.0f);
			if (MmPerMin <= 0.0f)
			{
				UE_LOG(LogDirt, Log, TEXT("Rain off."));
			}
			else
			{
				UE_LOG(LogDirt, Log, TEXT("Raining %.0f mm/min%s."), MmPerMin,
					Box->Settings.RainSecondsLeft > 0.0f ? *FString::Printf(TEXT(" for %.0f s"), Box->Settings.RainSecondsLeft) : TEXT(""));
			}
		}));

static FAutoConsoleCommandWithWorldAndArgs GDirtWaterCmd(
	TEXT("DaDirt.Water"),
	TEXT("DaDirt.Water <Xm> <Ym> <litres> [RadiusCm=200] - pour water on a spot, like a water truck. ")
	TEXT("It soaks in as far as the dirt can drink and the rest ponds and runs downhill."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld*)
		{
			ADirtBox* Box = GetDirtBoxOrWarn();
			if (!Box)
			{
				return;
			}
			if (Args.Num() < 3)
			{
				UE_LOG(LogDirt, Warning, TEXT("Usage: DaDirt.Water <Xm> <Ym> <litres> [RadiusCm]"));
				return;
			}
			const float Xm = ArgFloat(Args, 0, 0.0f);
			const float Ym = ArgFloat(Args, 1, 0.0f);
			const float Litres = FMath::Max(ArgFloat(Args, 2, 0.0f), 0.0f);
			const float RadiusCm = FMath::Max(ArgFloat(Args, 3, 200.0f), Box->Settings.TexelSizeCm());
			const FVector Origin = Box->GetActorLocation();
			const FVector2D World(Origin.X + Xm * 100.0, Origin.Y + Ym * 100.0);
			if (!Box->IsInsideBox(World))
			{
				UE_LOG(LogDirt, Warning, TEXT("(%.1f, %.1f) m is outside the box."), Xm, Ym);
				return;
			}

			Box->PourWater(World, RadiusCm, Litres);
			const float PeakCm = Litres * 1000.0f / (PI * RadiusCm * RadiusCm / 3.0f);
			UE_LOG(LogDirt, Log, TEXT("Poured %.0f L at (%.1f, %.1f) m over a %.0f cm radius: about %.1f cm of water at the centre, ")
				TEXT("soaking in at %.2f cm/s on loose dirt and %.4f cm/s on hardpack."),
				Litres, Xm, Ym, RadiusCm, PeakCm, Box->Settings.Soil(Box->Settings.DefaultSoil).InfiltrationCmPerSec, Box->Settings.Soil(Box->Settings.DefaultSoil).InfiltrationCmPerSec * FMath::Exp(-4.6f));
		}));

static FAutoConsoleCommandWithWorldAndArgs GDirtParcelCmd(
	TEXT("DaDirt.Parcel"),
	TEXT("DaDirt.Parcel <diameterCm | soil> [minCm] [maxCm] [maxPerThrow] - how fine the airborne dirt is. ")
	TEXT("'soil' lets cohesion pick the clod size. Halving the diameter means 8x the parcels for the same roost."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld*)
		{
			ADirtBox* Box = GetDirtBoxOrWarn();
			if (!Box)
			{
				return;
			}
			FDirtSimSettings& S = Box->Settings;
			if (Args.IsValidIndex(0))
			{
				S.ParcelDiameterCm = (Args[0].ToLower() == TEXT("soil")) ? 0.0f : FMath::Clamp(FCString::Atof(*Args[0]), 0.0f, 20.0f);
			}
			S.ParcelMinDiameterCm = FMath::Clamp(ArgFloat(Args, 1, S.ParcelMinDiameterCm), 0.05f, 20.0f);
			S.ParcelMaxDiameterCm = FMath::Clamp(ArgFloat(Args, 2, S.ParcelMaxDiameterCm), S.ParcelMinDiameterCm, 50.0f);
			S.ParcelMaxPerThrow = FMath::Clamp(ArgInt(Args, 3, S.ParcelMaxPerThrow), 1, 65536);

			const float D = Box->ChooseParcelDiameterCm(0.2f, 0.25f);
			const float PerLitre = 1000.0f / (PI / 6.0f * D * D * D);
			UE_LOG(LogDirt, Log, TEXT("Parcels: %s (%.2f cm on the pad, %.0f parcels per litre), bounds %.2f-%.2f cm, at most %d per throw. Pool %d."),
				S.ParcelDiameterCm > 0.0f ? *FString::Printf(TEXT("%.2f cm"), S.ParcelDiameterCm) : TEXT("sized by the soil"),
				D, PerLitre, S.ParcelMinDiameterCm, S.ParcelMaxDiameterCm, S.ParcelMaxPerThrow, S.ParcelPoolSide * S.ParcelPoolSide);
		}));

static FAutoConsoleCommandWithWorldAndArgs GDirtParcelsCmd(
	TEXT("DaDirt.Parcels"),
	TEXT("DaDirt.Parcels [0|1] - airborne dirt on or off (off: thrown dirt lands where it was scooped). No argument: report the counters."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld*)
		{
			ADirtBox* Box = GetDirtBoxOrWarn();
			if (!Box)
			{
				return;
			}
			if (Args.IsValidIndex(0))
			{
				Box->Settings.bParcels = ArgInt(Args, 0, 1) != 0;
				Box->SetParcelsVisible(Box->Settings.bParcels);
			}
			uint32 C[8];
			Box->GetParcelCounters(C);
			uint32 D[8];
			Box->GetDustCounters(D);
			UE_LOG(LogDirt, Log, TEXT("Parcels %s: %d live (highest slot %u), %u free, %u spawned, %u landed, %u pool-full fallbacks; ")
				TEXT("dust %s: %d live (highest slot %u), %u free, %u spawned, %u died, %u fallbacks."),
				Box->Settings.bParcels ? TEXT("on") : TEXT("off"), Box->GetLiveParcels(), C[DirtSim::ParcelCounterMaxLive],
				C[DirtSim::ParcelCounterFree], C[DirtSim::ParcelCounterSpawned], C[DirtSim::ParcelCounterLanded], C[DirtSim::ParcelCounterFallback],
				Box->Settings.bDust ? TEXT("on") : TEXT("off"), Box->GetLiveDust(), D[DirtSim::ParcelCounterMaxLive],
				D[DirtSim::ParcelCounterFree], D[DirtSim::ParcelCounterSpawned], D[DirtSim::ParcelCounterLanded], D[DirtSim::ParcelCounterFallback]);
		}));

static FAutoConsoleCommandWithWorldAndArgs GDirtDustCmd(
	TEXT("DaDirt.Dust"),
	TEXT("DaDirt.Dust [0|1] [motesPerLitre] [lifetimeS] - the dust puffed with roost and throws. An effect: no audited volume."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld*)
		{
			ADirtBox* Box = GetDirtBoxOrWarn();
			if (!Box)
			{
				return;
			}
			FDirtSimSettings& S = Box->Settings;
			if (Args.IsValidIndex(0))
			{
				S.bDust = ArgInt(Args, 0, 1) != 0;
			}
			S.DustPerLitre = FMath::Max(ArgFloat(Args, 1, S.DustPerLitre), 0.0f);
			S.DustLifetime = FMath::Max(ArgFloat(Args, 2, S.DustLifetime), 0.1f);
			UE_LOG(LogDirt, Log, TEXT("Dust %s: %.0f motes per litre, %.1f s lifetime, %d live."),
				S.bDust ? TEXT("on") : TEXT("off"), S.DustPerLitre, S.DustLifetime, Box->GetLiveDust());
		}));

static FAutoConsoleCommandWithWorldAndArgs GDirtShedCmd(
	TEXT("DaDirt.Shed"),
	TEXT("DaDirt.Shed [0|1] [chance] [minOutCm] [diameterCm] - grains shedding down over-steep faces as parcels."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld*)
		{
			ADirtBox* Box = GetDirtBoxOrWarn();
			if (!Box)
			{
				return;
			}
			FDirtSimSettings& S = Box->Settings;
			if (Args.IsValidIndex(0))
			{
				S.bShed = ArgInt(Args, 0, 1) != 0;
			}
			S.ShedChance = FMath::Clamp(ArgFloat(Args, 1, S.ShedChance), 0.0f, 1.0f);
			S.ShedMinOutCm = FMath::Max(ArgFloat(Args, 2, S.ShedMinOutCm), 0.0f);
			S.ShedDiameterCm = FMath::Clamp(ArgFloat(Args, 3, S.ShedDiameterCm), 0.05f, 10.0f);
			UE_LOG(LogDirt, Log, TEXT("Shedding %s: chance %.2f per cell per step above %.2f cm of outflow, %.2f cm grains."),
				S.bShed ? TEXT("on") : TEXT("off"), S.ShedChance, S.ShedMinOutCm, S.ShedDiameterCm);
		}));

static FAutoConsoleCommandWithWorldAndArgs GDirtWaterSimCmd(
	TEXT("DaDirt.WaterSim"),
	TEXT("DaDirt.WaterSim [0|1] - the water pass (run-off, soaking, draining, drying) on or off."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld*)
		{
			ADirtBox* Box = GetDirtBoxOrWarn();
			if (!Box)
			{
				return;
			}
			Box->Settings.bWater = Args.IsValidIndex(0) ? (ArgInt(Args, 0, 1) != 0) : !Box->Settings.bWater;
			UE_LOG(LogDirt, Log, TEXT("Water pass %s."), Box->Settings.bWater ? TEXT("on") : TEXT("off"));
		}));

static FAutoConsoleCommandWithWorldAndArgs GDirtThrowCmd(
	TEXT("DaDirt.Throw"),
	TEXT("DaDirt.Throw <Xm> <Ym> [litres=2] [speedMps=8] [headingDeg=0] [elevationDeg=45] [spreadDeg=12] - scoop dirt ")
	TEXT("out of the ground and throw it. The hand-tool version of roost: watch it fly, land, roll and pile."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld*)
		{
			ADirtBox* Box = GetDirtBoxOrWarn();
			if (!Box)
			{
				return;
			}
			if (Args.Num() < 2)
			{
				UE_LOG(LogDirt, Warning, TEXT("Usage: DaDirt.Throw <Xm> <Ym> [litres] [speedMps] [headingDeg] [elevationDeg] [spreadDeg]"));
				return;
			}
			const float Xm = ArgFloat(Args, 0, 0.0f);
			const float Ym = ArgFloat(Args, 1, 0.0f);
			const float Litres = FMath::Clamp(ArgFloat(Args, 2, 2.0f), 0.01f, 500.0f);
			const float Speed = ArgFloat(Args, 3, 8.0f) * 100.0f;
			const float Heading = FMath::DegreesToRadians(ArgFloat(Args, 4, 0.0f));
			const float Elev = FMath::DegreesToRadians(ArgFloat(Args, 5, 45.0f));
			const float Spread = ArgFloat(Args, 6, 12.0f);
			const FVector Origin = Box->GetActorLocation();
			const FVector2D World(Origin.X + Xm * 100.0, Origin.Y + Ym * 100.0);
			if (!Box->IsInsideBox(World))
			{
				UE_LOG(LogDirt, Warning, TEXT("(%.1f, %.1f) m is outside the box."), Xm, Ym);
				return;
			}

			Box->RefreshReadback();
			const FLinearColor S = Box->GetStateAtWorld(World);
			const float Ground = Box->GetSurfaceHeightAtWorld(World);
			const float RadiusCm = FMath::Max(40.0f, Box->Settings.TexelSizeCm() * 3.0f);
			// Never scoop more than the ground under the shovel has.
			const float VolumeCm3 = FMath::Min(Litres * 1000.0f, Box->MaxScoopCm3(S.R, RadiusCm));

			const FVector Vel(FMath::Cos(Heading) * FMath::Cos(Elev) * Speed, FMath::Sin(Heading) * FMath::Cos(Elev) * Speed, FMath::Sin(Elev) * Speed);
			const int32 Link = Box->ScoopDirt(World, RadiusCm, VolumeCm3);
			Box->SpawnParcels(FVector(World.X, World.Y, Ground + 10.0f), Vel, Spread, VolumeCm3, S.B, S.G, Link);
			Box->SpawnDust(FVector(World.X, World.Y, Ground + 10.0f), Vel * 0.6f, Spread + 15.0f,
						   FMath::RoundToInt(Box->Settings.DustPerLitre * VolumeCm3 / 1000.0f * (1.0f - FMath::Clamp(S.B, 0.0f, 1.0f))), S.B);
			UE_LOG(LogDirt, Log, TEXT("Threw %.2f L of solid dirt from (%.1f, %.1f) m at %.1f m/s, parcels of %.2f cm."),
				VolumeCm3 / 1000.0f, Xm, Ym, Speed / 100.0f, Box->ChooseParcelDiameterCm(S.B, S.G));
		}));
