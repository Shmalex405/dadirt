#include "DaDirtGameMode.h"

#include "Dirt/DirtBox.h"
#include "Engine/DirectionalLight.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpectatorPawn.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RHI.h"
#include "UnrealClient.h"

DEFINE_LOG_CATEGORY_STATIC(LogDaDirtGameMode, Log, All);

ADaDirtGameMode::ADaDirtGameMode()
{
	PrimaryActorTick.bCanEverTick = true;

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

	LoadScriptFromCommandLine();
}

void ADaDirtGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (bScriptActive)
	{
		TickScript(DeltaSeconds);
	}
}

// ---------------------------------------------------------------------------
// Scripted console driver
// ---------------------------------------------------------------------------

void ADaDirtGameMode::LoadScriptFromCommandLine()
{
	FString ScriptPath;
	if (!FParse::Value(FCommandLine::Get(), TEXT("DirtScript="), ScriptPath))
	{
		return;
	}

	ScriptPath = ScriptPath.TrimQuotes();
	if (FPaths::IsRelative(ScriptPath))
	{
		ScriptPath = FPaths::Combine(FPaths::ProjectDir(), ScriptPath);
	}

	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *ScriptPath))
	{
		UE_LOG(LogDaDirtGameMode, Error, TEXT("DirtScript: could not read '%s'."), *ScriptPath);
		return;
	}

	Text.ParseIntoArrayLines(ScriptLines, /*bCullEmpty*/ false);
	ScriptNext = 0;
	bScriptActive = ScriptLines.Num() > 0;

	UE_LOG(LogDaDirtGameMode, Log, TEXT("DirtScript: running %d lines from '%s'."), ScriptLines.Num(), *ScriptPath);
}

void ADaDirtGameMode::TickScript(float DeltaSeconds)
{
	// A screenshot in flight: one request per frame, see RunScriptLine.
	if (ShotStage >= 0)
	{
		const FString Name = (ShotStage < 2) ? ShotName : FString(TEXT("_flush"));
		const FString Path = FPaths::Combine(FPaths::ScreenShotDir(), Name + TEXT(".png"));
		FScreenshotRequest::RequestScreenshot(Path, /*bShowUI*/ false, /*bAddFilenameSuffix*/ false);

		if (++ShotStage > 2)
		{
			ShotStage = -1;
			UE_LOG(LogDaDirtGameMode, Log, TEXT("DirtScript: screenshot requested -> %s.png"), *ShotName);
		}
		return;
	}

	// While waiting, accumulate the timings that stat unit / stat gpu would show
	// on screen, so the log carries the performance picture too.
	if (WaitRemaining > 0.0f)
	{
		const double FrameMs = DeltaSeconds * 1000.0;
		WaitFrames++;
		WaitFrameMs += FrameMs;
		WaitWorstFrameMs = FMath::Max(WaitWorstFrameMs, FrameMs);
		WaitGameMs += FPlatformTime::ToMilliseconds(GGameThreadTime);
		WaitRenderMs += FPlatformTime::ToMilliseconds(GRenderThreadTime);
		WaitGPUMs += FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles());

		WaitRemaining -= DeltaSeconds;
		if (WaitRemaining > 0.0f)
		{
			return;
		}

		ReportWaitStats();
	}

	// Run lines until one asks us to wait or take a screenshot, or the script is spent.
	while (ScriptNext < ScriptLines.Num() && WaitRemaining <= 0.0f && ShotStage < 0)
	{
		RunScriptLine(ScriptLines[ScriptNext++]);
	}

	if (ScriptNext >= ScriptLines.Num() && WaitRemaining <= 0.0f && ShotStage < 0)
	{
		bScriptActive = false;
		UE_LOG(LogDaDirtGameMode, Log, TEXT("DirtScript: finished."));
	}
}

void ADaDirtGameMode::RunScriptLine(const FString& RawLine)
{
	const FString Line = RawLine.TrimStartAndEnd();
	if (Line.IsEmpty() || Line.StartsWith(TEXT("#")))
	{
		return;
	}

	UE_LOG(LogDaDirtGameMode, Log, TEXT("DirtScript> %s"), *Line);

	FString Cmd, Rest;
	if (!Line.Split(TEXT(" "), &Cmd, &Rest))
	{
		Cmd = Line;
	}
	Rest = Rest.TrimStartAndEnd();

	if (Cmd.Equals(TEXT("wait"), ESearchCase::IgnoreCase))
	{
		FString SecondsStr, Label;
		if (!Rest.Split(TEXT(" "), &SecondsStr, &Label))
		{
			SecondsStr = Rest;
		}
		WaitRemaining = FMath::Max(FCString::Atof(*SecondsStr), 0.01f);
		WaitLabel = Label.TrimStartAndEnd();
		WaitFrames = 0;
		WaitFrameMs = WaitGameMs = WaitRenderMs = WaitGPUMs = WaitWorstFrameMs = 0.0;
		return;
	}

	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PC)
	{
		UE_LOG(LogDaDirtGameMode, Warning, TEXT("DirtScript: no player controller to run '%s'."), *Line);
		return;
	}

	if (Cmd.Equals(TEXT("camera"), ESearchCase::IgnoreCase))
	{
		// camera <x> <y> <z> <pitch> <yaw>  — metres from the world origin, degrees.
		TArray<FString> A;
		Rest.ParseIntoArrayWS(A);
		if (A.Num() < 5)
		{
			UE_LOG(LogDaDirtGameMode, Warning, TEXT("DirtScript: camera needs x y z pitch yaw."));
			return;
		}
		if (APawn* Pawn = PC->GetPawn())
		{
			const FVector Loc(FCString::Atof(*A[0]) * 100.0f, FCString::Atof(*A[1]) * 100.0f, FCString::Atof(*A[2]) * 100.0f);
			const FRotator Rot(FCString::Atof(*A[3]), FCString::Atof(*A[4]), 0.0f);
			Pawn->SetActorLocation(Loc);
			Pawn->SetActorRotation(Rot);
			PC->SetControlRotation(Rot);
		}
		return;
	}

	if (Cmd.Equals(TEXT("sun"), ESearchCase::IgnoreCase))
	{
		// sun <pitch> <yaw> — aim the level's directional light. Low sun shows shape.
		TArray<FString> A;
		Rest.ParseIntoArrayWS(A);
		if (A.Num() < 2)
		{
			UE_LOG(LogDaDirtGameMode, Warning, TEXT("DirtScript: sun needs pitch yaw."));
			return;
		}
		for (TActorIterator<ADirectionalLight> It(GetWorld()); It; ++It)
		{
			It->SetActorRotation(FRotator(FCString::Atof(*A[0]), FCString::Atof(*A[1]), 0.0f));
			break;
		}
		return;
	}

	if (Cmd.Equals(TEXT("screenshot"), ESearchCase::IgnoreCase))
	{
		// Screenshots in an unattended -game window are captured one REQUEST late:
		// the file named by request N reliably holds the frame at which request N+1
		// was made (or the frame at exit). Reading the viewport ourselves from the
		// tick gives a black back buffer. So: request the name on two consecutive
		// frames, then a throwaway request to flush the second. The first write is
		// overwritten by the second, and the file ends up one frame late, which is
		// fine. TickScript drives the stages and holds the script meanwhile.
		ShotName = Rest.IsEmpty() ? TEXT("DirtShot") : Rest;
		ShotStage = 0;
		return;
	}

	PC->ConsoleCommand(Line);
}

void ADaDirtGameMode::ReportWaitStats()
{
	if (WaitFrames <= 0)
	{
		return;
	}

	const double N = static_cast<double>(WaitFrames);
	const double AvgFrame = WaitFrameMs / N;
	UE_LOG(LogDaDirtGameMode, Log,
		TEXT("DirtScript stats%s%s: %d frames, avg %.1f ms (%.0f fps), worst %.1f ms | game %.1f  render %.1f  gpu %.1f ms"),
		WaitLabel.IsEmpty() ? TEXT("") : TEXT(" "), *WaitLabel,
		WaitFrames, AvgFrame, AvgFrame > 0.0 ? 1000.0 / AvgFrame : 0.0, WaitWorstFrameMs,
		WaitGameMs / N, WaitRenderMs / N, WaitGPUMs / N);
}
