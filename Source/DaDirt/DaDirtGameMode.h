#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "DaDirtGameMode.generated.h"

class ADirtBox;

UCLASS()
class DADIRT_API ADaDirtGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ADaDirtGameMode();

	virtual void BeginPlay() override;

	/**
	 * Spawn a Dirtbox at the world origin if the level does not already contain
	 * one. Means any empty level plays as the sandbox with nothing to place.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "DaDirt")
	bool bAutoSpawnDirtbox = true;

	UPROPERTY(EditDefaultsOnly, Category = "DaDirt")
	TSubclassOf<ADirtBox> DirtboxClass;

	/** Fallback camera placement if no Dirtbox is present to ask. */
	UPROPERTY(EditDefaultsOnly, Category = "DaDirt")
	FVector StartViewLocation = FVector(-10000.0f, 0.0f, 4500.0f);

	UPROPERTY(EditDefaultsOnly, Category = "DaDirt")
	FRotator StartViewRotation = FRotator(-22.0f, 0.0f, 0.0f);
};
