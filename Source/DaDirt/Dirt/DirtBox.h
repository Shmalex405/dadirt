// DaDirt — the Dirtbox.
//
// One actor that owns the whole dirt simulation: the GPU textures, the display
// mesh, the fixed-step sim tick, the deformation brushes, the debug views and the
// volume-conservation audit.
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
	/** Size x Size texels: R layer cm, G compaction, B moisture, A surface height cm. */
	TArray<FLinearColor> Data;
	bool bValid = false;
};

/** Result of a volume-conservation audit. */
struct FDirtAudit
{
	double VolumeM3 = 0.0;
	double BaselineM3 = 0.0;
	double DriftM3 = 0.0;
	double DriftPercent = 0.0;
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
	 * For Dig and Raise, Amount is the peak depth/height in cm and the stroke is
	 * zero-sum: the core loses exactly what the rim gains. For the other modes
	 * Amount is a 0-1 strength.
	 */
	UFUNCTION(BlueprintCallable, Category = "DaDirt")
	void ApplyBrush(FVector2D WorldXYCm, float RadiusCm, float Amount, EDirtBrushMode Mode);

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

	/** True if the world XY lies inside the box. */
	bool IsInsideBox(FVector2D WorldXYCm) const;

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
	void UpdateMaterialParameters();
	void StepSimulation(bool bForceReinit);

	/** Harvest finished window readbacks and enqueue the next ones. Once per frame. */
	void UpdateHeightWindows();

	TArray<TSharedPtr<FDirtWindowSlot, ESPMode::ThreadSafe>> Windows;

	/** Convert world XY in cm to grid coordinates. */
	FVector2f WorldToTexel(FVector2D WorldXYCm) const;

	/**
	 * Build a stroke, including the discrete kernel sums that make dig and raise
	 * conserve volume exactly on this grid.
	 */
	FDirtBrushStroke MakeStroke(FVector2D WorldXYCm, float RadiusCm, float Amount, EDirtBrushMode Mode) const;

	UPROPERTY(Transient)
	TObjectPtr<UProceduralMeshComponent> GroundMesh;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> GroundMID;

	/** Static bedrock, R32F. */
	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> BaseHeightTex;

	/** CPU-built starting state, RGBA16F. Kept so Reset can reseed from it. */
	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> InitialStateTex;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> StateA;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> StateB;

	/** What the material samples for displacement. */
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> DisplayRT;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> NormalRT;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> DebugRT;

	/** Strokes waiting for the next sim step. */
	TArray<FDirtBrushStroke> PendingStrokes;

	/** CPU mirror of the dirt state, from the last RefreshReadback. */
	TArray<FLinearColor> Readback;

	/** Bedrock kept on the CPU too, so audits and height probes need no GPU round trip. */
	TArray<float> BedrockCm;

	TArray<FString> FeatureLog;

	double BaselineVolumeM3 = 0.0;

	/** Lap length in metres when a track is built, 0 for the testbed. */
	float LapLengthM = 0.0f;

	float StepAccumulator = 0.0f;
	bool bResourcesReady = false;
	bool bNeedsReinit = true;

	/** Countdown for a test that measures itself a moment after it runs. */
	float PendingMeasureSeconds = -1.0f;
	FString PendingMeasureLabel;

	static TWeakObjectPtr<ADirtBox> ActiveBox;
};
