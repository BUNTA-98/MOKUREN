#include "venue_spec.hpp"

#include <stdexcept>

namespace mokuren::venue {
namespace {

struct Hosts {
  const char* rest;
  const char* ws_root;
};

Hosts HostsFor(Env env) {
  switch (env) {
    case Env::Mainnet:
      return {"https://api.bybit.com", "wss://stream.bybit.com"};
    case Env::Testnet:
      return {"https://api-testnet.bybit.com", "wss://stream-testnet.bybit.com"};
    case Env::Demo:
      return {"https://api-demo.bybit.com", "wss://stream-demo.bybit.com"};
  }
  throw std::invalid_argument("venue: unknown environment");
}

} // namespace

bool IsKnownCategory(const std::string& category) {
  return category == "linear" || category == "spot" || category == "inverse";
}

Env ParseEnv(const std::string& name) {
  if (name == "mainnet" || name == "live" || name == "prod") return Env::Mainnet;
  if (name == "testnet" || name == "test") return Env::Testnet;
  if (name == "demo") return Env::Demo;
  throw std::invalid_argument("venue: unknown environment '" + name +
                              "' (expected mainnet, testnet or demo)");
}

const char* EnvName(Env env) {
  switch (env) {
    case Env::Mainnet: return "mainnet";
    case Env::Testnet: return "testnet";
    case Env::Demo:    return "demo";
  }
  return "unknown";
}

Spec Resolve(Env env, const std::string& category, const std::string& symbol) {
  if (!IsKnownCategory(category)) {
    throw std::invalid_argument("venue: unknown category '" + category +
                                "' (expected linear, spot or inverse)");
  }
  if (symbol.empty()) {
    throw std::invalid_argument("venue: symbol must not be empty");
  }

  const Hosts hosts = HostsFor(env);

  Spec spec;
  spec.env = env;
  spec.category = category;
  spec.symbol = symbol;
  spec.rest_base = hosts.rest;
  spec.public_ws = std::string(hosts.ws_root) + "/v5/public/" + category;
  spec.private_ws = std::string(hosts.ws_root) + "/v5/private";
  return spec;
}

Spec Resolve(const std::string& env, const std::string& category,
             const std::string& symbol) {
  return Resolve(ParseEnv(env), category, symbol);
}

std::string Manifest(const Spec& spec) {
  return "env=" + std::string(EnvName(spec.env)) +
         " category=" + spec.category + " symbol=" + spec.symbol +
         " rest=" + spec.rest_base + " ws_public=" + spec.public_ws +
         " ws_private=" + spec.private_ws;
}

} // namespace mokuren::venue
