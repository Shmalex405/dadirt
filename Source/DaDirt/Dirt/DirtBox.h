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
	bool bValid = false;
};

/** Result of a volume-conservation audit. Volumes are SOLID cubic metres unless named otherwise. */
struct FDirtAudit
{
	/** Solid dirt in the ground. */
	double GroundM3 = 0.0;
	/** Solid dirt in the air, carried by live parcels. */
	double AirborneM3 = 0.0;
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
	void TransferDirt(FVector2D FromWorldXYCm, float FromRadiusCm, FVector2D ToWorldXYCm, float ToRadiusCm, float VolumeCm3);

	/**
	 * Take a volume of solid dirt (cm^3) out of the ground with no rim. The
	 * caller owes it back: pair with SpawnParcels so it lands again somewhere.
	 */
	UFUNCTION(BlueprintCallable, Category = "DaDirt")
	void ScoopDirt(FVector2D FromWorldXYCm, float RadiusCm, float VolumeCm3, float DisturbOverride = -1.0f);

	/**
	 * Throw a volume of solid dirt (cm^3) into the air from a world position with
	 * a velocity (cm/s), spread over a cone. It becomes parcels sized by the
	 * settings, flies, lands, and is deposited back into the ground. With parcels
	 * disabled it is dumped straight back where it started.
	 */
	UFUNCTION(BlueprintCallable, Category = "DaDirt")
	void SpawnParcels(FVector WorldPosCm, FVector VelocityCmS, float SpreadDeg, float VolumeCm3,
					  float Moisture, float Compaction);

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

	/** Parcel counters from the GPU, a frame or two old. Indices are DirtSim::ParcelCounter*. */
	void GetParcelCounters(uint32 OutCounters[8]) const;

	/** Parcels alive right now (pool minus free), a frame or two old. */
	int32 GetLiveParcels() const;

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
							FLinearColor* OutState = nullptr) const;

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
	void ReleaseResources();
	void BuildTerrainAndUpload();
	void RebuildTerrainAndMesh();
	void BuildDisplayMesh();
	void BuildParcelMesh();
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

	/** Static bedrock, R32F. */
	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> BaseHeightTex;

	/** CPU-built starting state, RGBA32F. Kept so Reset can reseed from it. */
	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> InitialStateTex;

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

	/** Strokes waiting for the next sim step. */
	TArray<FDirtBrushStroke> PendingStrokes;

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

	TArray<FString> FeatureLog;

	/** Solid m^3 in the ground at build time. */
	double BaselineVolumeM3 = 0.0;

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
