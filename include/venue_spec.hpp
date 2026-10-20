#pragma once

#include <string>

namespace mokuren::venue {

// bybit exposes three independent worlds and an api key is valid in exactly
// one of them: mainnet (real money), testnet (own key store, thin synthetic
// book) and demo (production market data, simulated fills). the environment is
// therefore part of the venue identity, never a cosmetic flag.
enum class Env { Mainnet, Testnet, Demo };

struct Spec {
  Env env = Env::Testnet;
  std::string category = "linear"; // linear | spot | inverse
  std::string symbol = "BTCUSDT";
  std::string rest_base;
  std::string public_ws;
  std::string private_ws;
};

// throws std::invalid_argument on an unknown environment or category, or an
// empty symbol. this is the ONLY place a venue hostname may appear.
Spec Resolve(const std::string& env, const std::string& category,
             const std::string& symbol);
Spec Resolve(Env env, const std::string& category, const std::string& symbol);

// string <-> enum. a typo must fail, never fall back to mainnet.
Env ParseEnv(const std::string& name);
const char* EnvName(Env env);

bool IsKnownCategory(const std::string& category);

// one canonical line naming every host the spec resolves to, printed by both
// the doctor and the engine startup log so the two can be diffed.
std::string Manifest(const Spec& spec);

} // namespace mokuren::venue
