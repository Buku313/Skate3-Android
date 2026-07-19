#pragma once

#include <functional>

namespace rex::runtime {
class FunctionDispatcher;
}
namespace rex::input {
class InputSystem;
}

namespace skate3::demo_path {

void InstallHooks(rex::runtime::FunctionDispatcher* dispatcher);
// Provider for the merged UI pad state consumed by the user-facing frontend
// movie skip. Set during app setup, before the guest boots (the provider is
// read from the guest thread without further synchronization).
void SetUiInputProvider(std::function<rex::input::InputSystem*()> provider);
bool ShouldForceIntroMovieComplete();

}  // namespace skate3::demo_path
