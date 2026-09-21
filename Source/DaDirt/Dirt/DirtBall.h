// DaDirt — a ball that lives on the dirt.
//
// The ground mesh has no collision (it is displaced in the vertex shader), so the
// ball does not use the physics engine at all. It integrates itself against the
// simulated surface: every substep it reads height and normal from a height
// window the Dirtbox keeps current from the GPU, pushes itself out of the ground,
// bounces, rolls downhill, and dents the dirt where it lands. This is the
// smallest possible "gravity meets dirt" object and the ancestor of the wheel's
// ground contact.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DirtBall.generated.h"

class ADirtBox;
class UStaticMeshComponent;

UCLASS()
class DADIRT_API ADirtBall : public AActor
{
	GENERATED_BODY()

public:
	ADirtBall();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;

	void SetRadius(float NewRadiusCm);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DaDirt")
	float RadiusCm = 30.0f;

	/** Fraction of the impact speed kept as bounce. 0 = lands dead, 1 = superball. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DaDirt", meta = (ClampMin = "0", ClampMax = "1"))
	float Restitution = 0.25f;

	/** How fast rolling speed bleeds off on contact, per second. Loose dirt is slow going. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DaDirt", meta = (ClampMin = "0"))
	float RollingFriction = 0.8f;

	/** Impact dent depth in cm per m/s of impact speed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DaDirt", meta = (ClampMin = "0"))
	float DentCmPerMps = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DaDirt", meta = (ClampMin = "0"))
	float MaxDentCm = 15.0f;

	/** Depth of each groove stroke a rolling ball leaves. 0 disables. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DaDirt", meta = (ClampMin = "0"))
	float GrooveDepthCm = 0.3f;

	/** Let the ball move dirt at all. Off = it only reads the ground. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DaDirt")
	bool bDeformsDirt = true;

	/** Current velocity in cm/s. Set it to throw the ball. */
	UPROPERTY(BlueprintReadWrite, Category = "DaDirt")
	FVector Velocity = FVector::ZeroVector;

	bool IsResting() const { return bAsleep; }

private:
	void Step(float Dt);

	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> Mesh;

	TWeakObjectPtr<ADirtBox> Box;
	int32 WindowId = -1;

	float Accumulator = 0.0f;
	float Age = 0.0f;
	float RestTime = 0.0f;
	float GrooveDistanceCm = 0.0f;
	int32 Impacts = 0;
	bool bOnGround = false;
	bool bAsleep = false;
	bool bHadGroundData = false;

	static constexpr float SubstepSeconds = 1.0f / 120.0f;
	static constexpr float GravityCmS2 = 981.0f;
};
