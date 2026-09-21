// DaDirt — the Dirtbox.
//
// One actor that owns the whole dirt simulation: the GPU textures, the display
// mesh, the fixed-step sim tick, the deformation brushes, the water, the parcels
// (dirt in the air), the debug views and the volume-conservation audit.
//
// Drop one into a level and it builds its own testbed terrain. Do not rotate or
// scale it: the display material offsets vertices straight up in world space by
// the simulated height, which assumes an unrotated, unscaled actor.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DirtSimTypes.h"
#include "DirtBox.generated.h"

class UMaterialInstanceDynamic;
class UMaterialInterface;
class UProceduralMeshComponent;
class UTexture2D;
class UTextureRenderTarget2D;

struct FDirtWindowSlot;
struct FDirtParcelResources;
struct FDirtTileReadback;

/**
 * One tile of the persistent world at the current cell size. The simulated
 * window is 4 x 4 of these and slides over the world by whole tiles; a tile
 * that leaves the window is read back here, a tile that enters comes back
 * from here or is generated fresh. This is what makes a rut you cut in one
 * corner still be there when you come round again.
 */
struct FDirtTile
{
	TArray<FLinearColor> State;      // R solid cm, G compaction, B moisture, A surface
	TArray<float> Pond;
	/** Solid m^3 the builder gave this tile: its share of the world baseline. */
	double PristineM3 = 0.0;
	/** Solid m^3 it held when it last left the window; counted while it is out. */
	double StoredM3 = 0.0;
	bool bHasState = false;
};

/**
 * A small patch of the dirt state that is kept current from the GPU without ever
 * stalling: a sub-rectangle readback is in flight every frame and lands one or
 * two frames later. Physics objects (balls, wheels) each own one, centred on
 * themselves, and sample height and normal from it every substep.
 */
struct FDirtHeightWindow
{
	/** Texel origin of Data within the sim grid. */
	FIntPoint Origin = FIntPoint::ZeroValue;
	int32 Size = 0;
	/** Size x Size texels: R solid layer cm, G compaction, B moisture, A surface height cm. */
	TArray<FLinearColor> Data;
	/** Size x Size texels: ponded water on the surface, cm. */
	TArray<float> Pond;
	bool bValid = false;
};

/** Result of a volume-conservation audit. Volumes are SOLID cubic metres unless named otherwise. */
struct FDirtAudit
{
	/** Solid dirt in the ground. */
	double GroundM3 = 0.0;
	/** Solid dirt in the air, carried by live parcels. */
	double AirborneM3 = 0.0;
	/** Solid dirt in tiles that are out of the simulated window. */
	double StoredM3 = 0.0;
	/** Ground + airborne: what must match the baseline. */
	double VolumeM3 = 0.0;
	double BaselineM3 = 0.0;
	double DriftM3 = 0.0;
	double DriftPercent = 0.0;
	/** The same ground dirt measured with a ruler: bulk, voids included. */
	double BulkM3 = 0.0;
	/** Water in the pores and ponded on the surface. */
	double PoreWaterM3 = 0.0;
	double PondM3 = 0.0;
	int32 LiveParcels = 0;
	float MinLayerCm = 0.0f;
	float MaxLayerCm = 0.0f;
	int32 BedrockExposedCells = 0;
	/** Cells holding less than no dirt: a bug wherever they come from. */
	int32 NegativeCells = 0;
	double NegativeCm3 = 0.0;
	float MinSolidCm = 0.0f;
	float MaxLooseSlopeDeg = 0.0f;
	float MaxAnySlopeDeg = 0.0f;
};

UCLASS()
class DADIRT_API ADirtBox : public AActor
{
	GENERATED_BODY()

public:
	ADirtBox();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;

	/**
	 * Which terrain to build. Changing this at runtime resizes the box and
	 * rebuilds both the terrain and the display mesh.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DaDirt")
	EDirtTerrainMode TerrainMode = EDirtTerrainMode::Track;

	/** Tunables. Everything about how the dirt behaves lives here. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DaDirt")
	FDirtSimSettings Settings;

	/**
	 * Material that displaces the ground mesh from the simulated height. Must
	 * expose texture parameters DirtDisplay, DirtNormal and DirtDebug.
	 * See docs/DirtboxSetup.md for how to build it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DaDirt")
	TObjectPtr<UMaterialInterface> GroundMaterial;

	/**
	 * Where to look for the ground material if GroundMaterial is not set. Build it
	 * at exactly this path and the Dirtbox picks it up with nothing to wire up.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DaDirt",
			  meta = (AllowedClasses = "/Script/Engine.MaterialInterface"))
	FSoftObjectPath GroundMaterialPath = FSoftObjectPath(TEXT("/Game/Dirt/M_DirtGround.M_DirtGround"));

	/**
	 * Material for the parcels (dirt in the air): one tiny mesh per parcel, moved
	 * in the vertex shader from the ParcelPos texture. Tools/BuildDirtAssets.py
	 * builds it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DaDirt")
	TObjectPtr<UMaterialInterface> ParcelMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DaDirt",
			  meta = (AllowedClasses = "/Script/Engine.MaterialInterface"))
	FSoftObjectPath ParcelMaterialPath = FSoftObjectPath(TEXT("/Game/Dirt/M_DirtParcel.M_DirtParcel"));

	/** Material for dust: one camera-facing quad per mote, soft and translucent, fading with age. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DaDirt")
	TObjectPtr<UMaterialInterface> DustMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DaDirt",
			  meta = (AllowedClasses = "/Script/Engine.MaterialInterface"))
	FSoftObjectPath DustMaterialPath = FSoftObjectPath(TEXT("/Game/Dirt/M_DirtDust.M_DirtDust"));

	/** Material for the far ground (the world outside the simulated window): vertex colour and vertex normals. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DaDirt")
	TObjectPtr<UMaterialInterface> FarMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DaDirt",
			  meta = (AllowedClasses = "/Script/Engine.MaterialInterface"))
	FSoftObjectPath FarMaterialPath = FSoftObjectPath(TEXT("/Game/Dirt/M_DirtFar.M_DirtFar"));

	/** Which channel the ground is coloured by. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DaDirt|Debug")
	EDirtDebugView DebugView = EDirtDebugView::Dirt;

	/** Range the LayerDepth heatmap spans, in cm. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DaDirt|Debug")
	float DebugLayerRangeCm = 120.0f;

	/** Stop stepping the sim. Strokes still queue up and apply when unpaused. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DaDirt|Debug")
	bool bPaused = false;

	// --- deformation -------------------------------------------------------

	/**
	 * Queue a deformation stroke. World XY in centimetres, radius in cm.
	 * For Dig and Raise, Amount is the peak depth/height in cm of loose ground
	 * and the stroke is zero-sum: the core loses exactly what the rim gains. For
	 * the other modes Amount is a 0-1 strength. bProctor makes a Pack stroke
	 * obey the moisture curve (a tyre); tools pack regardless.
	 */
	UFUNCTION(BlueprintCallable, Category = "DaDirt")
	void ApplyBrush(FVector2D WorldXYCm, float RadiusCm, float Amount, EDirtBrushMode Mode,
					float DisturbOverride = -1.0f, bool bProctor = false);

	/**
	 * Move a volume of SOLID dirt (cm^3) from one spot to another with no rim on
	 * either end: a Scoop at From and a Dump at To of exactly the same volume.
	 * Zero-sum except where the scoop hits bedrock, which the audit reports.
	 */
	UFUNCTION(BlueprintCallable, Category = "DaDirt")
	void TransferDirt(FVector2D FromWorldXYCm, float FromRadiusCm, FVector2D ToWorldXYCm, float ToRadiusCm, float VolumeCm3, float DisturbOverride = -1.0f);

	/**
	 * Take a volume of solid dirt (cm^3) out of the ground with no rim. The
	 * caller owes it back: pair with SpawnParcels so it lands again somewhere.
	 */
	UFUNCTION(BlueprintCallable, Category = "DaDirt")
	int32 ScoopDirt(FVector2D FromWorldXYCm, float RadiusCm, float VolumeCm3, float DisturbOverride = -1.0f);

	/**
	 * Throw a volume of solid dirt (cm^3) into the air from a world position with
	 * a velocity (cm/s), spread over a cone. It becomes parcels sized by the
	 * settings, flies, lands, and is deposited back into the ground. With parcels
	 * disabled it is dumped straight back where it started.
	 */
	UFUNCTION(BlueprintCallable, Category = "DaDirt")
	void SpawnParcels(FVector WorldPosCm, FVector VelocityCmS, float SpreadDeg, float VolumeCm3,
					  float Moisture, float Compaction, int32 ScoopStrokeIndex = -1);

	/**
	 * Puff dust: Count motes from a world position with a velocity over a cone.
	 * Dust is the one effect here; it carries no audited volume, hangs in the
	 * air under heavy drag and fades out. Roost and throws call this alongside
	 * SpawnParcels, scaled by DustPerLitre.
	 */
	UFUNCTION(BlueprintCallable, Category = "DaDirt")
	void SpawnDust(FVector WorldPosCm, FVector VelocityCmS, float SpreadDeg, int32 Count, float Moisture);

	/**
	 * Pour water on a spot, like a water truck. It lands as surface water and
	 * soaks in at the soil's own rate: gone in seconds on loose dirt, a puddle on
	 * hardpack. World XY in cm, radius in cm, volume in litres.
	 */
	UFUNCTION(BlueprintCallable, Category = "DaDirt")
	void PourWater(FVector2D WorldXYCm, float RadiusCm, float Litres);

	/** Rebuild the terrain and throw away everything that has been dug. */
	UFUNCTION(BlueprintCallable, Category = "DaDirt")
	void ResetToTestbed();

	/** Switch between the testbed and the real track, rebuilding everything. */
	UFUNCTION(BlueprintCallable, Category = "DaDirt")
	void SetTerrainMode(EDirtTerrainMode NewMode);

	/**
	 * Point the simulation at part of the box instead of all of it. The grid stays
	 * at SimResolution, so a smaller region means finer cells — this is what makes
	 * rut-scale detail possible on a track-scale world. SizeCm of 0 goes back to
	 * covering the whole box. Centre is in cm relative to the Dirtbox origin.
	 */
	UFUNCTION(BlueprintCallable, Category = "DaDirt")
	void SetSimRegion(FVector2D CentreCm, float SizeCm);

	/** A camera position and rotation that frames whatever is currently built. */
	void GetSuggestedViewpoint(FVector& OutLocation, FRotator& OutRotation) const;

	/** The soil of a cell of the window, or at a box-relative position. */
	const FDirtSoil& SoilAtTexel(FIntPoint Texel) const;
	int32 SoilIdAtWorld(FVector2D WorldXYCm) const;
	const FDirtSoil& SoilAtWorld(FVector2D WorldXYCm) const { return Settings.Soil(SoilIdAtWorld(WorldXYCm)); }
	/** Paint a soil over the window (RadiusCm <= 0) or a disc of it, and re-upload. Tests and DaDirt.Soil. */
	void PaintSoil(int32 SoilId, FVector2D CentreCm, float RadiusCm);
	/** A soil painted over everything by DaDirt.Soil <name>: survives Reset and slides, until DaDirt.Soil built. */
	int32 SoilOverride = -1;

	/** Keep the simulated window centred on the test wheel, sliding by tiles as it drives. */
	void SetFollowWheel(bool bFollow);
	bool IsFollowingWheel() const { return bFollowWheel; }

	/** Tile coordinates of the window's first tile, and cells per tile. */
	FIntPoint GetWindowTile() const { return WindowTile; }
	int32 GetTileCells() const { return TileCells; }
	int32 GetCachedTileCount() const { return TileCache.Num(); }
	int32 GetShiftCount() const { return ShiftsThisSession; }

	// --- queries -----------------------------------------------------------

	/** Refresh the CPU copy of the dirt state. Blocking — debug use only. */
	void RefreshReadback();

	/** Run a volume-conservation audit against the CPU copy. Refreshes it first. */
	FDirtAudit RunAudit();

	/** Total surface height in cm at a world XY, from the last readback. */
	float GetSurfaceHeightAtWorld(FVector2D WorldXYCm) const;

	/** Full dirt state at a world XY from the last readback: R solid cm, G compaction, B moisture, A surface. */
	FLinearColor GetStateAtWorld(FVector2D WorldXYCm) const;

	/** Ponded water depth in cm at a world XY, from the last readback. */
	float GetPondAtWorld(FVector2D WorldXYCm) const;

	/** True if the world XY lies inside the box. */
	bool IsInsideBox(FVector2D WorldXYCm) const;

	/**
	 * The most solid volume (cm^3) a Scoop of this radius may take from a spot
	 * whose layer holds SolidCm, without its peak cell running out. The core
	 * kernel puts 1/CoreNorm of the volume into the centre cell, so at fine
	 * cells a small scoop is nearly all one cell: guard on that cell, not on the
	 * whole patch. What a scoop cannot take, the shader clamps, and the parcels
	 * spawned for it would land dirt that never left: the rut-resolution leak.
	 */
	float MaxScoopCm3(float SolidCm, float RadiusCm) const;

	/** Parcel counters from the GPU, a frame or two old. Indices are DirtSim::ParcelCounter*. */
	void GetParcelCounters(uint32 OutCounters[8]) const;

	/** Parcels alive right now (pool minus free), a frame or two old. */
	int32 GetLiveParcels() const;

	/** Dust motes alive right now, a frame or two old. */
	int32 GetLiveDust() const;
	void GetDustCounters(uint32 OutCounters[8]) const;

	/** Show or hide the parcel mesh (it is also hidden when parcels are switched off). */
	void SetParcelsVisible(bool bVisible);

	/**
	 * The parcel size the soil says a throw breaks into: clods held together by
	 * cohesion in damp loam, grains in dry sand. Honours ParcelDiameterCm when
	 * that is set, and the min/max either way.
	 */
	float ChooseParcelDiameterCm(float Moisture, float Compaction) const;

	// --- height windows (non-stalling ground queries for physics objects) ------

	/** Create a window of SizeTexels x SizeTexels. Returns its id. */
	int32 CreateHeightWindow(int32 SizeTexels = 32);
	void ReleaseHeightWindow(int32 Id);

	/** Ask for the window to follow this world position from the next frame on. */
	void SetHeightWindowCentre(int32 Id, FVector2D WorldXYCm);

	/**
	 * Surface height (world Z, cm) and world-space normal at a point, from the
	 * window's latest data. False if the window has no data yet or the point lies
	 * outside it. OutState, if given, receives the bilinear dirt state there.
	 */
	bool SampleHeightWindow(int32 Id, FVector2D WorldXYCm, float& OutHeightCm, FVector& OutNormal,
							FLinearColor* OutState = nullptr, float* OutPondCm = nullptr) const;

	float GetTexelSizeCm() const { return Settings.TexelSizeCm(); }

	/** The live Dirtbox, for console commands. */
	static ADirtBox* GetActive() { return ActiveBox.Get(); }

	/** Description of every testbed feature, for the console. */
	const TArray<FString>& GetFeatureLog() const { return FeatureLog; }

	/** Run one of the named scripted tests. Returns false if the name is unknown. */
	bool RunTest(const FString& TestName, FString& OutMessage);

private:
	void ApplyModeDefaults();
	void CreateResources();

	/** Build one tile of the world with the current builder, converted to solids. Records its pristine volume. */
	void BuildTile(FIntPoint Tile, TArray<float>& OutBedrock, TArray<FLinearColor>& OutState, TArray<uint8>& OutSoil);

	void UploadBytes(UTexture2D* Texture, const TArray<uint8>& Data);

	/** Run the whole-site builder once for its feature log and site-wide constants. */
	void BuildWholeSiteLog();

	/** Slide the window by whole tiles; the next sim step performs it on the GPU. */
	void ShiftWindow(FIntPoint DeltaTiles);

	/** Pull finished tile readbacks into the cache. Blocking waits for every one in flight. */
	void HarvestTileReadbacks(bool bBlock);

	/** Slide the window after the wheel when following. */
	void UpdateFollow();

	/** Snap a requested region centre onto the tile grid. */
	FIntPoint TileForCentre(FVector2D CentreCm, float RegionCm) const;

	/**
	 * The far ground: the whole box outside the simulated window, one mesh
	 * section per world tile, built from the whole-site ground and updated from
	 * the cache as tiles leave the window. The window's own tiles are hidden.
	 */
	void BuildFarMesh();
	void UpdateFarTile(FIntPoint Tile);
	void UpdateFarVisibility();
	void FillFarTile(FIntPoint Tile, const FDirtTile* Cached, TArray<FVector>& Verts, TArray<FVector>& Normals, TArray<FLinearColor>& Colors) const;
	static FLinearColor FarTint(const FDirtSoil& Soil, float SolidCm, float Compaction, float Moisture);
	float TileSizeCm() const { return Settings.RegionSizeCm() / TilesPerSide; }
	void UploadFloats(UTexture2D* Texture, const TArray<float>& Data);
	void UploadColors(UTexture2D* Texture, const TArray<FLinearColor>& Data);
	void ReleaseResources();
	void BuildTerrainAndUpload();
	void RebuildTerrainAndMesh();
	void BuildDisplayMesh();
	void BuildParcelMesh();
	void BuildDustMesh();
	void UpdateMaterialParameters();
	void StepSimulation(bool bForceReinit);

	/** Harvest finished window readbacks and enqueue the next ones. Once per frame. */
	void UpdateHeightWindows();

	/** Pull the latest parcel counters over from the render thread. Once per frame. */
	void UpdateParcelCounters();

	TArray<TSharedPtr<FDirtWindowSlot, ESPMode::ThreadSafe>> Windows;

	/** Convert world XY in cm to grid coordinates. */
	FVector2f WorldToTexel(FVector2D WorldXYCm) const;

	/**
	 * Build a stroke, including the discrete kernel sums that make dig and raise
	 * conserve volume exactly on this grid.
	 */
	FDirtBrushStroke MakeStroke(FVector2D WorldXYCm, float RadiusCm, float Amount, EDirtBrushMode Mode,
								float DisturbOverride = -1.0f, bool bProctor = false) const;

	UPROPERTY(Transient)
	TObjectPtr<UProceduralMeshComponent> GroundMesh;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> GroundMID;

	/** One tiny tetrahedron per parcel slot, positioned by the material. */
	UPROPERTY(Transient)
	TObjectPtr<UProceduralMeshComponent> ParcelMesh;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> ParcelMID;

	/** The actor's root; the ground mesh sits under it and moves with the window. */
	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> SceneRoot;

	/** The world outside the window, drawn from CPU data. */
	UPROPERTY(Transient)
	TObjectPtr<UProceduralMeshComponent> FarMesh;

	/** Static bedrock, R32F. */
	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> BaseHeightTex;

	/** The soil id of every cell of the window, R8_UINT. Static like bedrock; painted by DaDirt.Soil. */
	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> SoilTex;

	/** CPU-built starting state, RGBA32F. Also the patch of entering cells on a slide. */
	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> InitialStateTex;

	/** Entering pond on a slide, R32F. */
	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> InitialPondTex;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> StateA;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> StateB;

	/** Ponded surface water, cm. R32F, ping-ponged by the water pass. */
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> PondA;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> PondB;

	/** What the material samples for displacement. */
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> DisplayRT;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> NormalRT;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> DebugRT;

	/** Parcel state, ParcelPoolSide^2 texels each. See DirtParcels.usf. */
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> ParcelPosRT;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> ParcelVelRT;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> ParcelPropRT;

	/** Free list, counters and deposit accumulators. Render thread owns the contents. */
	TSharedPtr<FDirtParcelResources, ESPMode::ThreadSafe> ParcelGPU;

	/** Dust: camera-facing quads, one per mote. */
	UPROPERTY(Transient)
	TObjectPtr<UProceduralMeshComponent> DustMesh;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> DustMID;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> DustPosRT;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> DustVelRT;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> DustPropRT;

	TSharedPtr<FDirtParcelResources, ESPMode::ThreadSafe> DustGPU;

	/** Dust puffs waiting for the next sim step. */
	TArray<FDirtParcelSpawn> PendingDust;
	uint32 DustCounters[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	int32 DustPoolCount = 0;
	int32 DustSectionsVisible = 0;
	int32 DustSlotsRequested = 0;
	float DustSectionHoldSeconds = 0.0f;
	uint32 FrameCounter = 0;

	/** Strokes waiting for the next sim step: the taking halves. */
	TArray<FDirtBrushStroke> PendingStrokes;

	/** The giving halves, applied after every taking stroke has reported its shortfall. */
	TArray<FDirtBrushStroke> PendingGiving;

	/** Throws waiting for the next sim step. */
	TArray<FDirtParcelSpawn> PendingSpawns;

	/** Water pours waiting for the next sim step. */
	TArray<FDirtWaterSource> PendingWater;

	/** CPU mirror of the dirt state, from the last RefreshReadback. */
	TArray<FLinearColor> Readback;

	/** CPU mirror of the pond, from the last RefreshReadback. */
	TArray<float> PondReadback;

	/** Bedrock kept on the CPU too, so audits and height probes need no GPU round trip. */
	TArray<float> BedrockCm;
	/** Soil id per cell of the window, the CPU copy of SoilTex. */
	TArray<uint8> SoilId;

	TArray<FString> FeatureLog;

	/** Solid m^3 in the ground at build time: the world baseline, every tile ever generated. */
	double BaselineVolumeM3 = 0.0;

	// --- the persistent world ------------------------------------------------
	static constexpr int32 TilesPerSide = 4;
	int32 TileCells = 256;
	FIntPoint WindowTile = FIntPoint::ZeroValue;
	TMap<FIntPoint, TSharedPtr<FDirtTile>> TileCache;
	/** Solid m^3 held by tiles that are out of the window right now. */
	double WorldStoredM3 = 0.0;
	TArray<TSharedPtr<FDirtTileReadback, ESPMode::ThreadSafe>> PendingTileReadbacks;
	TArray<TSharedPtr<FDirtTileReadback, ESPMode::ThreadSafe>> InFlightTileReadbacks;
	FIntPoint PendingShiftTexels = FIntPoint::ZeroValue;
	/** Bumped on every slide; height-window data from an older window is discarded. */
	uint32 WindowGeneration = 0;
	bool bFollowWheel = false;
	int32 ShiftsThisSession = 0;

	/** The whole site as first built, at whole-box resolution: what the far ground shows until a tile is touched. */
	int32 WholeRes = 0;
	TArray<uint8> WholeSoil;
	TArray<float> WholeSurfaceCm;
	TArray<float> WholeSolidCm;
	TArray<float> WholeCompaction;
	TArray<float> WholeMoisture;
	/** Far mesh layout: sections are world tiles, FarVerts x FarVerts each. */
	int32 FarTilesPerSide = 0;
	int32 FarVerts = 0;
	float FarTileCm = 0.0f;

	/** Lap length in metres when a track is built, 0 for the testbed. */
	float LapLengthM = 0.0f;

	/** Latest parcel counters seen on the game thread. */
	uint32 ParcelCounters[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	int32 ParcelPoolCount = 0;
	uint32 SpawnSeed = 1;

	/**
	 * The parcel mesh is split into sections of this many slots. Live parcels
	 * always occupy the lowest slots (the free list is a stack that starts with
	 * slot 0 on top), so only the sections up to the highest live slot need
	 * drawing, and an idle pool of a quarter million costs nothing.
	 */
	static constexpr int32 ParcelSectionSlots = 8192;
	int32 ParcelSectionsVisible = 0;
	/** Slots asked for this frame, so a burst shows up before the GPU count arrives. */
	int32 ParcelSlotsRequested = 0;
	float ParcelSectionHoldSeconds = 0.0f;
	void UpdateParcelSections(float DeltaSeconds);

	float StepAccumulator = 0.0f;
	bool bResourcesReady = false;
	bool bNeedsReinit = true;

	/** Countdown for a test that measures itself a moment after it runs. */
	float PendingMeasureSeconds = -1.0f;
	FString PendingMeasureLabel;

	static TWeakObjectPtr<ADirtBox> ActiveBox;
};
