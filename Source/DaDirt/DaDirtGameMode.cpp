#include "DaDirtGameMode.h"

#include "Dirt/DirtBox.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpectatorPawn.h"

DEFINE_LOG_CATEGORY_STATIC(LogDaDirtGameMode, Log, All);

ADaDirtGameMode::ADaDirtGameMode()
{
	// Free-flying camera pawn until the sandbox tools pawn exists.
	DefaultPawnClass = ASpectatorPawn::StaticClass();
	DirtboxClass = ADirtBox::StaticClass();
}

void ADaDirtGameMode::BeginPlay()
{
	Super::BeginPlay();

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	if (bAutoSpawnDirtbox)
	{
		bool bAlreadyPresent = false;
		for (TActorIterator<ADirtBox> It(World); It; ++It)
		{
			bAlreadyPresent = true;
			break;
		}

		if (!bAlreadyPresent && DirtboxClass)
		{
			FActorSpawnParameters SpawnParams;
			SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

			ADirtBox* Box = World->SpawnActor<ADirtBox>(DirtboxClass, FVector::ZeroVector,
													   FRotator::ZeroRotator, SpawnParams);
			UE_LOG(LogDaDirtGameMode, Log, TEXT("%s"),
				Box ? TEXT("Spawned a Dirtbox at the origin.") : TEXT("Failed to spawn a Dirtbox."));
		}
	}

	// Put the camera where the terrain is actually visible, rather than wherever
	// the level's PlayerStart happens to sit (often buried in the dirt). The box
	// knows how big it ended up, so ask it rather than guessing.
	if (APlayerController* PC = World->GetFirstPlayerController())
	{
		if (APawn* Pawn = PC->GetPawn())
		{
			FVector Loc = StartViewLocation;
			FRotator Rot = StartViewRotation;

			if (ADirtBox* Box = ADirtBox::GetActive())
			{
				Box->GetSuggestedViewpoint(Loc, Rot);
			}

			Pawn->SetActorLocation(Loc);
			Pawn->SetActorRotation(Rot);
			PC->SetControlRotation(Rot);
		}
	}
}
