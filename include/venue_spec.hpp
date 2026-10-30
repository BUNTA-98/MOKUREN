#pragma once

#include <string>

namespace mokuren::venue {

// bybit exposes three independent worlds and an api key is valid in exactly
// one of them: mainnet (real money), testnet (own key store, thin synthetic
// book) and demo (production market data, simulated fills). the environment is
// therefore part of the venue identity, never a cosmetic flag.
enum class Env { Mainnet, Testnet, Demo };

struct Spec {
  // the venue this spec was resolved for ("bybit", "blofin"). carried on the
  // spec so a consumer can dispatch on it without re-reading the config, and
  // so Manifest() names the venue it describes.
  std::string venue;
  Env env = Env::Testnet;
  std::string category = "linear"; // linear | spot | inverse
  std::string symbol = "BTCUSDT";
  std::string rest_base;
  std::string public_ws;
  std::string private_ws;
};

// throws std::invalid_argument on an unknown venue, environment or category,
// or an empty symbol. this is the ONLY place a venue hostname may appear.
//
// the venue name selects the host table: bybit has three worlds, blofin has
// two (mainnet and demo) and no testnet. an unknown venue is a hard error, not
// a fallback to bybit -- a typo must never route orders to a venue the config
// did not name.
Spec Resolve(const std::string& venue, const std::string& env,
             const std::string& category, const std::string& symbol);
Spec Resolve(const std::string& venue, Env env, const std::string& category,
             const std::string& symbol);

// string <-> enum. a typo must fail, never fall back to mainnet.
Env ParseEnv(const std::string& name);
const char* EnvName(Env env);

bool IsKnownCategory(const std::string& category);

// one canonical line naming every host the spec resolves to, printed by both
// the doctor and the engine startup log so the two can be diffed.
std::string Manifest(const Spec& spec);

} // namespace mokuren::venue
