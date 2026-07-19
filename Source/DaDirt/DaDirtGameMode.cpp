#include "DaDirtGameMode.h"
#include "GameFramework/SpectatorPawn.h"

ADaDirtGameMode::ADaDirtGameMode()
{
	// Free-flying camera pawn until the sandbox tools pawn exists.
	DefaultPawnClass = ASpectatorPawn::StaticClass();
}
