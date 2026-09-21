#include "DirtBall.h"

#include "DirtBox.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogDirtBall, Log, All);

ADirtBall::ADirtBall()
{
	PrimaryActorTick.bCanEverTick = true;
	// After the Dirtbox, so the window it reads was updated this frame.
	PrimaryActorTick.TickGroup = TG_PostPhysics;

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	SetRootComponent(Mesh);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCastShadow(true);

	// The engine's basic sphere is 100 cm across.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (Sphere.Succeeded())
	{
		Mesh->SetStaticMesh(Sphere.Object);
	}
	SetRadius(RadiusCm);
}

void ADirtBall::SetRadius(float NewRadiusCm)
{
	RadiusCm = FMath::Max(NewRadiusCm, 1.0f);
	if (Mesh)
	{
		Mesh->SetRelativeScale3D(FVector(RadiusCm / 50.0f));
	}
}

void ADirtBall::BeginPlay()
{
	Super::BeginPlay();

	Box = ADirtBox::GetActive();
	if (ADirtBox* B = Box.Get())
	{
		// A window a few diameters wide: enough for the normal, and for the ball
		// to move a fair way between readback frames without falling off the edge.
		const int32 Texels = FMath::Clamp(FMath::CeilToInt(RadiusCm * 6.0f / B->GetTexelSizeCm()), 16, 128);
		WindowId = B->CreateHeightWindow(Texels);
		B->SetHeightWindowCentre(WindowId, FVector2D(GetActorLocation()));
	}
	else
	{
		UE_LOG(LogDirtBall, Warning, TEXT("No Dirtbox: the ball will fall forever."));
	}
}

void ADirtBall::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (ADirtBox* B = Box.Get())
	{
		B->ReleaseHeightWindow(WindowId);
	}
	Super::EndPlay(EndPlayReason);
}

void ADirtBall::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	Age += DeltaSeconds;
	if (bAsleep)
	{
		return;
	}

	// Fixed substeps, same reason as the dirt itself: repeatable results.
	Accumulator += FMath::Min(DeltaSeconds, 0.1f);
	while (Accumulator >= SubstepSeconds && !bAsleep)
	{
		Accumulator -= SubstepSeconds;
		Step(SubstepSeconds);
	}

	if (ADirtBox* B = Box.Get())
	{
		B->SetHeightWindowCentre(WindowId, FVector2D(GetActorLocation()));
	}
}

void ADirtBall::Step(float Dt)
{
	ADirtBox* B = Box.Get();
	FVector Pos = GetActorLocation();

	Velocity.Z -= GravityCmS2 * Dt;
	Pos += Velocity * Dt;

	float Ground = 0.0f;
	FVector Normal = FVector::UpVector;
	FLinearColor State(0, 0, 0, 0);
	const bool bHaveGround = B && B->SampleHeightWindow(WindowId, FVector2D(Pos), Ground, Normal, &State);

	if (!bHaveGround)
	{
		// No data yet (first frames) or outside the box: fall, but never below the
		// box origin, so a ball dropped before the first readback lands on
		// something rather than through it.
		if (B && Pos.Z - RadiusCm < B->GetActorLocation().Z)
		{
			Pos.Z = B->GetActorLocation().Z + RadiusCm;
			Velocity.Z = 0.0f;
		}
		SetActorLocation(Pos);
		return;
	}
	bHadGroundData = true;

	const float Bottom = Pos.Z - RadiusCm;
	if (Bottom < Ground)
	{
		// Out of the ground, straight up. Good enough for a ball; the normal
		// decides how it bounces and which way it rolls.
		Pos.Z += Ground - Bottom;

		const float Vn = FVector::DotProduct(Velocity, Normal);
		if (Vn < 0.0f)
		{
			const float ImpactSpeed = -Vn;
			Velocity -= (1.0f + Restitution) * Vn * Normal;

			// A real landing, not a rolling-contact tick: dent the dirt. Depth
			// scales with impact speed; the spoil heaps up around the rim exactly
			// as the dig brush always does, and slumping then relaxes the crater.
			if (ImpactSpeed > 80.0f && bDeformsDirt)
			{
				const float Depth = FMath::Clamp(ImpactSpeed / 100.0f * DentCmPerMps, 0.5f, MaxDentCm);
				B->ApplyBrush(FVector2D(Pos), RadiusCm * 0.9f, Depth, EDirtBrushMode::Dig);
				B->ApplyBrush(FVector2D(Pos), RadiusCm * 1.5f, 0.3f, EDirtBrushMode::Loosen);
				++Impacts;
				UE_LOG(LogDirtBall, Log, TEXT("Ball %s hit at (%.1f, %.1f) m, %.1f m/s -> %.1f cm dent (impact %d)."),
					*GetName(), Pos.X * 0.01, Pos.Y * 0.01, ImpactSpeed * 0.01f, Depth, Impacts);
			}
		}

		// Rolling resistance: loose dirt drags harder than hardpack.
		const float Drag = RollingFriction * FMath::Lerp(1.5f, 0.5f, FMath::Clamp(State.G, 0.0f, 1.0f));
		FVector Tangent = Velocity - FVector::DotProduct(Velocity, Normal) * Normal;
		const float Speed = static_cast<float>(Tangent.Size());
		Tangent *= FMath::Max(0.0f, 1.0f - Drag * Dt);
		Velocity = Tangent + FVector::DotProduct(Velocity, Normal) * Normal;

		// Rolling contact leaves a shallow groove, one stroke every half radius.
		GrooveDistanceCm += Speed * Dt;
		if (bDeformsDirt && GrooveDepthCm > 0.0f && Speed > 40.0f && GrooveDistanceCm > RadiusCm * 0.5f)
		{
			GrooveDistanceCm = 0.0f;
			B->ApplyBrush(FVector2D(Pos), RadiusCm * 0.5f, GrooveDepthCm, EDirtBrushMode::Dig);
		}

		// Visual roll.
		if (Speed > 1.0f)
		{
			const FVector Axis = FVector::CrossProduct(Normal, Tangent).GetSafeNormal();
			const float Angle = Speed * Dt / RadiusCm;
			Mesh->AddWorldRotation(FQuat(Axis, Angle));
		}

		bOnGround = true;

		if (Velocity.Size() < 8.0f)
		{
			RestTime += Dt;
			if (RestTime > 0.5f)
			{
				bAsleep = true;
				Velocity = FVector::ZeroVector;
				UE_LOG(LogDirtBall, Log,
					TEXT("Ball %s at rest after %.1f s at (%.2f, %.2f) m: bottom Z %.1f cm on ground Z %.1f cm, ")
					TEXT("%d impact(s), compaction %.2f, moisture %.2f under it."),
					*GetName(), Age, Pos.X * 0.01, Pos.Y * 0.01, Pos.Z - RadiusCm, Ground, Impacts, State.G, State.B);
			}
		}
		else
		{
			RestTime = 0.0f;
		}
	}
	else
	{
		bOnGround = false;
		RestTime = 0.0f;
	}

	SetActorLocation(Pos);
}
