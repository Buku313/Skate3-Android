#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace skate3 {

struct LocalProfile {
  std::string id;
  std::string gamertag;
  uint64_t xuid = 0;
  bool signed_in = true;
  bool live_signed_in = false;
};

struct LocalProfileStore {
  std::string selected_profile;
  std::vector<LocalProfile> profiles;
};

std::filesystem::path ProfilesFilePath(const std::filesystem::path& user_data_root);
LocalProfileStore LoadProfiles(const std::filesystem::path& profiles_path);
bool SaveProfiles(const std::filesystem::path& profiles_path, const LocalProfileStore& store);
LocalProfile MakeDefaultProfile(std::string gamertag);
LocalProfile* FindSelectedProfile(LocalProfileStore& store);
const LocalProfile* FindSelectedProfile(const LocalProfileStore& store);
void EnsureUsableProfileStore(LocalProfileStore& store, std::string default_gamertag);
void ApplyProfileCvars(const LocalProfile& profile);
void ApplyVideoCvars(int resolution_scale, double refresh_rate);
std::string FormatXuid(uint64_t xuid);

}  // namespace skate3
