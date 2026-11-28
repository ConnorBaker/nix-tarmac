// Shims over Nix C++ API churn so the rest of the plugin has no version
// conditionals. NIX_COMPAT_VERSION_* come from meson.
#pragma once

#include <nix/store/derivations.hh>
#include <nix/store/path.hh>
#include <nix/store/store-dir-config.hh>
#include <nix/util/source-accessor.hh>

#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#define TARMAC_NIX_AT_LEAST(major, minor)                                      \
  (NIX_COMPAT_VERSION_MAJOR > (major) ||                                       \
   (NIX_COMPAT_VERSION_MAJOR == (major) &&                                     \
    NIX_COMPAT_VERSION_MINOR >= (minor)))

// Store / SourceAccessor grew an anchor() key function and the Store API
// settled enough for TarmacStore.
#define TARMAC_HAVE_STORE TARMAC_NIX_AT_LEAST(2, 35)

#if TARMAC_NIX_AT_LEAST(2, 35)
#define TARMAC_ANCHOR_OVERRIDE                                                 \
  void anchor() override {}
#else
#define TARMAC_ANCHOR_OVERRIDE
#endif

// 2.36 split Derivation into a template over its inputs, moved the ATerm
// (de)serialiser into nix::derivation and renamed the registerDrvOutput
// hook to registerDrvOutputUnchecked.
#if TARMAC_NIX_AT_LEAST(2, 36)
#include <nix/store/derivation/aterm.hh>
#define TARMAC_REGISTER_DRV_OUTPUT registerDrvOutputUnchecked
#else
#define TARMAC_REGISTER_DRV_OUTPUT registerDrvOutput
#endif

namespace nix_compat {

#if TARMAC_HAVE_STORE
inline auto parseDrv(const nix::StoreDirConfig &store, std::string &&text,
                     std::string_view name) -> nix::Derivation {
#if TARMAC_NIX_AT_LEAST(2, 36)
  return nix::derivation::parse(store, std::move(text), name);
#else
  return nix::parseDerivation(store, std::move(text), name);
#endif
}

inline auto unparseDrv(const nix::StoreDirConfig &store,
                       const nix::Derivation &drv) -> std::string {
#if TARMAC_NIX_AT_LEAST(2, 36)
  return nix::derivation::unparse(drv, store, false);
#else
  return drv.unparse(store, false);
#endif
}

// store paths a .drv refers to (input sources and input derivations)
inline auto drvReferences(const nix::Derivation &drv) -> nix::StorePathSet {
#if TARMAC_NIX_AT_LEAST(2, 36)
  nix::StorePathSet refs;
  for (const auto &input : drv.inputs) {
    refs.insert(input.getBaseStorePath());
  }
  return refs;
#else
  auto refs = drv.inputSrcs;
  for (const auto &[inputDrv, node] : drv.inputDrvs.map) {
    refs.insert(inputDrv);
  }
  return refs;
#endif
}
#endif

// NixOS/nix 7569dc8726 named directory entries with nix::Filename, a string
// that is one file name, instead of std::string, with no version bump. The
// key type is detected; Name is a template parameter only so that the branch
// not taken is dependent and never instantiated.

// The DirEntries key for a tree entry's name. readTree refuses a name that is
// not one file name, so it is known valid: Nix's asserting constructor.
template <typename Name = nix::SourceAccessor::DirEntries::key_type>
auto dirEntryName(std::string name) -> Name {
  if constexpr (std::is_same_v<Name, std::string>) {
    return name;
  } else {
    return Name::fromStringAsserting(std::move(name));
  }
}

// The string of a directory entry's name, a nix::Filename or a std::string.
template <typename Name>
auto dirEntryString(const Name &name) -> const std::string & {
  if constexpr (std::is_same_v<Name, std::string>) {
    return name;
  } else {
    return name.str();
  }
}

} // namespace nix_compat
