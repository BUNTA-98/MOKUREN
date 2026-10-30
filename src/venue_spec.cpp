#include "venue_spec.hpp"

#include <stdexcept>

namespace mokuren::venue {
namespace {

// bybit: three worlds, each with its own rest host and ws root. the public
// path is /v5/public/<category>; the private path is /v5/private.
struct BybitHosts {
  const char* rest;
  const char* ws_root;
};

BybitHosts BybitHostsFor(Env env) {
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

// blofin: two worlds. mainnet is openapi.blofin.com; demo is the
// demo-trading-openapi host. there is no testnet -- a key is either live or
// demo, and asking for testnet is a config error, not a fallback.
//
// ws_root is a SEPARATE field, not derived from rest. the previous version
// assumed /ws/private lived on the same host as REST and translated the
// scheme; that assumption is what a 403 on the demo private socket looks
// like when it is wrong. keeping it explicit means a doc lookup that says
// "demo ws is on a different host" is a one-line table edit, not a rewrite
// of the derivation. when the two hosts really are the same, the field
// simply repeats the rest host -- the cost of the field is one string.
struct BloFinHosts {
  const char* rest;
  const char* ws_root;
};

BloFinHosts BloFinHostsFor(Env env) {
  switch (env) {
    case Env::Mainnet:
      return {"https://openapi.blofin.com", "wss://openapi.blofin.com"};
    case Env::Demo:
      return {"https://demo-trading-openapi.blofin.com",
              "wss://demo-trading-openapi.blofin.com"};
    case Env::Testnet:
      throw std::invalid_argument(
          "venue: blofin has no testnet (expected mainnet or demo)");
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

Spec Resolve(const std::string& venue, Env env, const std::string& category,
             const std::string& symbol) {
  if (!IsKnownCategory(category)) {
    throw std::invalid_argument("venue: unknown category '" + category +
                                "' (expected linear, spot or inverse)");
  }
  if (symbol.empty()) {
    throw std::invalid_argument("venue: symbol must not be empty");
  }

  Spec spec;
  spec.venue = venue;
  spec.env = env;
  spec.category = category;
  spec.symbol = symbol;

  if (venue == "bybit") {
    const BybitHosts hosts = BybitHostsFor(env);
    spec.rest_base = hosts.rest;
    spec.public_ws = std::string(hosts.ws_root) + "/v5/public/" + category;
    spec.private_ws = std::string(hosts.ws_root) + "/v5/private";
    return spec;
  }

  if (venue == "blofin") {
    const BloFinHosts hosts = BloFinHostsFor(env);
    spec.rest_base = hosts.rest;
    spec.public_ws = std::string(hosts.ws_root) + "/ws/public";
    spec.private_ws = std::string(hosts.ws_root) + "/ws/private";
    return spec;
  }

  // an unknown venue is a hard error, not a fallback to bybit: a typo must
  // never route orders to a venue the config did not name.
  throw std::invalid_argument("venue: unknown venue '" + venue +
                              "' (expected bybit or blofin)");
}

Spec Resolve(const std::string& venue, const std::string& env,
             const std::string& category, const std::string& symbol) {
  return Resolve(venue, ParseEnv(env), category, symbol);
}

std::string Manifest(const Spec& spec) {
  return "venue=" + spec.venue +
         " env=" + std::string(EnvName(spec.env)) +
         " category=" + spec.category + " symbol=" + spec.symbol +
         " rest=" + spec.rest_base + " ws_public=" + spec.public_ws +
         " ws_private=" + spec.private_ws;
}

} // namespace mokuren::venue
