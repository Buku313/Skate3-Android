#pragma once

namespace rex::runtime {
class FunctionDispatcher;
}

namespace skate3::demo_path {

void InstallHooks(rex::runtime::FunctionDispatcher* dispatcher);
bool ShouldForceIntroMovieComplete();

}  // namespace skate3::demo_path
