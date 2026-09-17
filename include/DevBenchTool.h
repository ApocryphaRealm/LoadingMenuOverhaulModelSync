#pragma once

namespace DevBenchTool
{
	// Registers "lmo.modelsync" with DevBench when it is there. Called with false at kPostLoad and
	// true at kDataLoaded - DevBench may not have loaded yet at the first attempt.
	void Init(bool a_lastAttempt);
}
