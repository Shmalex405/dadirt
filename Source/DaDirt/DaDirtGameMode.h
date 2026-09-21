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
	virtual void Tick(float DeltaSeconds) override;

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

private:
	// --- scripted console driver ------------------------------------------
	//
	// Launch with -DirtScript=<file> and every line of the file is run as a console
	// command, in order, with a few extras so a whole test session can run without
	// anyone at the keyboard:
	//
	//   wait <seconds> [label]   pause, then log average frame / game / render / GPU ms
	//   screenshot <name>        save a screenshot to Saved/Screenshots
	//   # anything               comment
	//
	// Everything else goes straight to the console, so "DaDirt.Test conserve" and
	// "quit" both work. This is what makes the dirt tests repeatable on a machine
	// nobody is sitting at.

	void LoadScriptFromCommandLine();
	void TickScript(float DeltaSeconds);
	void RunScriptLine(const FString& Line);
	void ReportWaitStats();

	TArray<FString> ScriptLines;
	int32 ScriptNext = 0;
	bool bScriptActive = false;

	/** Screenshot in flight: -1 idle, 0..2 = which of the three requests is next. */
	int32 ShotStage = -1;
	FString ShotName;

	float WaitRemaining = 0.0f;
	FString WaitLabel;
	int32 WaitFrames = 0;
	double WaitFrameMs = 0.0;
	double WaitGameMs = 0.0;
	double WaitRenderMs = 0.0;
	double WaitGPUMs = 0.0;
	double WaitWorstFrameMs = 0.0;
};
