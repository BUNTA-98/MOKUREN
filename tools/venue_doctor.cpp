// Read-only connectivity report for a Bybit-style venue.
//
// It answers exactly one question -- "are these credentials, this host and
// this instrument usable right now?" -- without placing, amending or
// cancelling anything. That is a prop-account requirement, not politeness:
// a place/cancel canary is an unauthorised order and is the pattern venue
// surveillance flags.
//
// The restriction is structural: HttpGet() refuses any path outside
// kAllowedPaths and the transport type has no method that could send a write.
#include "venue_spec.hpp"

#include <curl/curl.h>
#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace {

using json = nlohmann::json;

int64_t NowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string Hex(const unsigned char* data, size_t len) {
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out.push_back(kHex[(data[i] >> 4) & 0x0F]);
    out.push_back(kHex[data[i] & 0x0F]);
  }
  return out;
}

std::string HmacHex(const std::string& secret, const std::string& message) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_len = 0;
  HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
       reinterpret_cast<const unsigned char*>(message.data()), message.size(),
       digest, &digest_len);
  return Hex(digest, digest_len);
}

double JsonNum(const json& j, const char* key, double fallback = 0.0) {
  if (!j.contains(key)) return fallback;
  const auto& v = j[key];
  if (v.is_number()) return v.get<double>();
  if (v.is_string()) {
    try {
      return std::stod(v.get<std::string>());
    } catch (...) {
      return fallback;
    }
  }
  return fallback;
}

size_t WriteCb(void* contents, size_t size, size_t nmemb, void* userp) {
  const size_t total = size * nmemb;
  static_cast<std::string*>(userp)->append(static_cast<char*>(contents), total);
  return total;
}

// every path this tool may request. anything else is refused before a socket
// is opened, so a later edit cannot quietly turn the doctor into an order
// placer.
const std::set<std::string> kAllowedPaths = {
    "/v5/market/time",
    "/v5/user/query-api",
    "/v5/account/info",
    "/v5/account/wallet-balance",
    "/v5/market/instruments-info",
    "/v5/market/risk-limit",
    "/v5/position/list",
    "/v5/order/realtime",
    "/v5/account/fee-rate",
};

struct Keys {
  std::string api_key;
  std::string api_secret;
  bool present() const { return !api_key.empty() && !api_secret.empty(); }
};

struct Response {
  long http = 0;
  std::string body;
};

// GET only, on purpose. there is no Post/Put/Delete member to call.
bool HttpGet(const std::string& base, const std::string& path,
             const std::string& query, const Keys* keys, int64_t timestamp,
             const std::string& recv_window, Response& out, std::string& err) {
  if (kAllowedPaths.find(path) == kAllowedPaths.end()) {
    err = "path not on the read-only allowlist: " + path;
    return false;
  }

  CURL* curl = curl_easy_init();
  if (!curl) {
    err = "curl_easy_init failed";
    return false;
  }

  const std::string url = base + path + (query.empty() ? "" : "?" + query);

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  if (keys) {
    // GET signs timestamp + api_key + recv_window + query string.
    const std::string ts = std::to_string(timestamp);
    const std::string sig =
        HmacHex(keys->api_secret, ts + keys->api_key + recv_window + query);
    headers = curl_slist_append(headers,
                                ("X-BAPI-API-KEY: " + keys->api_key).c_str());
    headers = curl_slist_append(headers, ("X-BAPI-SIGN: " + sig).c_str());
    headers = curl_slist_append(headers, ("X-BAPI-TIMESTAMP: " + ts).c_str());
    headers = curl_slist_append(headers,
                                ("X-BAPI-RECV-WINDOW: " + recv_window).c_str());
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out.body);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

  const CURLcode res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
    err = curl_easy_strerror(res);
  } else {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out.http);
  }

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return res == CURLE_OK;
}

bool ParseOk(const Response& r, json& out, std::string& why) {
  if (r.http < 200 || r.http >= 300) {
    why = "HTTP " + std::to_string(r.http);
    return false;
  }
  out = json::parse(r.body, nullptr, false);
  if (out.is_discarded() || !out.is_object()) {
    why = "unparsable body";
    return false;
  }
  const int rc = out.value("retCode", -1);
  if (rc != 0) {
    why = "retCode " + std::to_string(rc) + " " + out.value("retMsg", "");
    return false;
  }
  return true;
}

struct Reporter {
  int failures = 0;

  void Pass(const std::string& label, const std::string& detail = "") {
    std::cout << "[PASS] " << label;
    if (!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
  }
  void Fail(const std::string& label, const std::string& detail) {
    std::cout << "[FAIL] " << label << ": " << detail << "\n";
    ++failures;
  }
  void Skip(const std::string& label, const std::string& why) {
    std::cout << "[SKIP] " << label << ": " << why << "\n";
  }
};

// prevents two processes on the same credentials from burning the shared
// per-uid rate limit at the same time.
struct LockFile {
  std::string path;
  bool held = false;

  explicit LockFile(const std::string& p) : path(p) {
    if (path.empty()) return;
    if (std::filesystem::exists(path)) return;
    std::ofstream(path) << NowMillis();
    held = true;
  }
  ~LockFile() {
    if (!held) return;
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
};

bool CheckPrivateWs(const mokuren::venue::Spec& spec, const Keys& keys,
                    int64_t drift_ms, std::string& detail) {
  ix::WebSocket ws;
  ws.setUrl(spec.private_ws);
  ws.setPingInterval(20);

  std::mutex m;
  std::string state = "connecting";
  std::string reason = "timed out before auth completed";

  ws.setOnMessageCallback([&](const ix::WebSocketMessagePtr& msg) {
    std::lock_guard<std::mutex> lock(m);
    if (msg->type == ix::WebSocketMessageType::Open) {
      const int64_t expires = NowMillis() + drift_ms + 10000;
      const std::string sig =
          HmacHex(keys.api_secret, "GET/realtime" + std::to_string(expires));
      ws.send("{\"op\":\"auth\",\"args\":[\"" + keys.api_key + "\"," +
              std::to_string(expires) + ",\"" + sig + "\"]}");
    } else if (msg->type == ix::WebSocketMessageType::Message) {
      const json j = json::parse(msg->str, nullptr, false);
      if (j.is_discarded() || !j.is_object()) return;
      const std::string op = j.value("op", "");
      if (op == "auth") {
        if (j.value("success", false)) {
          ws.send(R"({"op":"subscribe","args":["position","execution","wallet"]})");
          state = "ok";
          reason = "authenticated and subscribed";
        } else {
          state = "error";
          reason = "auth rejected: " + j.value("retMsg", "");
        }
      } else if (op == "subscribe" && !j.value("success", true)) {
        state = "error";
        reason = "subscribe rejected: " + j.value("retMsg", "");
      }
    } else if (msg->type == ix::WebSocketMessageType::Error) {
      state = "error";
      reason = msg->errorInfo.reason;
    } else if (msg->type == ix::WebSocketMessageType::Close) {
      if (state != "ok") {
        state = "error";
        reason = "closed before auth completed";
      }
    }
  });

  ws.start();
  for (int i = 0; i < 100; ++i) {
    {
      std::lock_guard<std::mutex> lock(m);
      if (state != "connecting") break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  ws.stop();

  std::lock_guard<std::mutex> lock(m);
  detail = reason;
  return state == "ok";
}

void Usage() {
  std::cout
      << "usage: venue_doctor --env=<mainnet|testnet|demo> [options]\n"
      << "  --category=<linear|spot|inverse>   default: linear\n"
      << "  --symbol=<SYMBOL>                  default: BTCUSDT\n"
      << "  --keys-file=<path>                 json {\"api_key\":..,\"api_secret\":..}\n"
      << "                                     (path, not flags, so keys stay out of\n"
      << "                                      shell history)\n"
      << "  --expect-env=<env>                 abort if the resolved env differs\n"
      << "  --no-ws                            skip the private websocket check\n"
      << "\nreads only: never places, amends or cancels an order.\n";
}

} // namespace

int main(int argc, char** argv) {
  std::string env_name = "testnet";
  std::string category = "linear";
  std::string symbol = "BTCUSDT";
  std::string expect_env;
  std::string keys_file;
  bool do_ws = true;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a.rfind("--env=", 0) == 0) env_name = a.substr(6);
    else if (a.rfind("--category=", 0) == 0) category = a.substr(11);
    else if (a.rfind("--symbol=", 0) == 0) symbol = a.substr(9);
    else if (a.rfind("--expect-env=", 0) == 0) expect_env = a.substr(13);
    else if (a.rfind("--keys-file=", 0) == 0) keys_file = a.substr(12);
    else if (a == "--no-ws") do_ws = false;
    else if (a == "--help" || a == "-h") { Usage(); return 0; }
    else { std::cerr << "unknown argument: " << a << "\n"; return 2; }
  }

  mokuren::venue::Spec spec;
  try {
    spec = mokuren::venue::Resolve(env_name, category, symbol);
  } catch (const std::exception& e) {
    std::cerr << e.what() << "\n";
    return 2;
  }

  std::cout << "mokuren venue doctor\n  " << mokuren::venue::Manifest(spec)
            << "\n";

  if (!expect_env.empty()) {
    mokuren::venue::Env expected;
    try {
      expected = mokuren::venue::ParseEnv(expect_env);
    } catch (const std::exception& e) {
      std::cerr << e.what() << "\n";
      return 2;
    }
    if (expected != spec.env) {
      std::cerr << "\n*** ENVIRONMENT MISMATCH ***\n  expected: "
                << mokuren::venue::EnvName(expected)
                << "\n  resolved: " << mokuren::venue::EnvName(spec.env)
                << "\n  refusing to report on the wrong venue.\n";
      return 1;
    }
  }
  std::cout << "\n";

  Keys keys;
  if (!keys_file.empty()) {
    std::ifstream in(keys_file);
    if (!in.is_open()) {
      std::cerr << "cannot open keys file: " << keys_file << "\n";
      return 2;
    }
    const json j = json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
      std::cerr << "keys file is not a json object\n";
      return 2;
    }
    keys.api_key = j.value("api_key", "");
    keys.api_secret = j.value("api_secret", "");
  }

  LockFile lock(
      keys.present()
          ? (std::filesystem::temp_directory_path() /
             ("mokuren_doctor_" +
              std::to_string(std::hash<std::string>{}(keys.api_key + env_name)) +
              ".lock"))
                .string()
          : std::string());
  if (keys.present() && !lock.held) {
    std::cerr << "another doctor run holds " << lock.path
              << " (same credentials); refusing to double the rate limit\n";
    return 2;
  }

  Reporter rep;
  std::string err;

  // 1. server time. every signed call below is stamped against this, so it
  // must run first: a few seconds of drift makes valid auth look invalid.
  int64_t drift_ms = 0;
  {
    Response r;
    if (!HttpGet(spec.rest_base, "/v5/market/time", "", nullptr, 0, "", r, err)) {
      rep.Fail("market/time", err);
    } else {
      json j;
      std::string why;
      if (!ParseOk(r, j, why)) {
        rep.Fail("market/time", why);
      } else {
        int64_t server_ms = 0;
        if (j.contains("result") && j["result"].is_object()) {
          const auto& res = j["result"];
          if (res.contains("timeNano")) {
            server_ms = static_cast<int64_t>(JsonNum(res, "timeNano") / 1e6);
          } else if (res.contains("timeSecond")) {
            server_ms = static_cast<int64_t>(JsonNum(res, "timeSecond") * 1000.0);
          }
        }
        drift_ms = server_ms - NowMillis();
        std::cout << "       clock drift: " << drift_ms << " ms\n";
        if (drift_ms > 5000 || drift_ms < -5000) {
          rep.Fail("market/time", "clock drift outside recv_window (+/-5000 ms)");
        } else {
          rep.Pass("market/time", "reachable");
        }
      }
    }
  }
  if (rep.failures > 0) {
    std::cout << "\naborting: clock is not usable for signed requests\n";
    return 1;
  }

  const std::string recv_window = "5000";
  const std::string cat_q =
      "category=" + spec.category + "&symbol=" + spec.symbol;

  auto signed_get = [&](const std::string& path, const std::string& query,
                        Response& r) {
    return HttpGet(spec.rest_base, path, query, &keys,
                   NowMillis() + drift_ms, recv_window, r, err);
  };

  // 2. permissions. the single most valuable check: it answers "is api trading
  // allowed at all" and "is ip whitelisting active" without guessing.
  bool trading_permitted = false;
  if (!keys.present()) {
    rep.Skip("user/query-api", "no --keys-file given");
  } else {
    Response r;
    json j;
    std::string why;
    if (!signed_get("/v5/user/query-api", "", r)) {
      rep.Fail("user/query-api", err);
    } else if (!ParseOk(r, j, why)) {
      rep.Fail("user/query-api", why);
    } else {
      const auto& res = j["result"];
      const bool read_only = res.value("readOnly", 0) != 0;
      const bool ip_locked =
          res.contains("ips") && res["ips"].is_array() && !res["ips"].empty();

      std::string perms;
      bool can_order = false;
      if (res.contains("permissions") && res["permissions"].is_object()) {
        for (auto it = res["permissions"].begin();
             it != res["permissions"].end(); ++it) {
          perms += it.key() + "=" + it.value().dump() + " ";
          if (!it.value().is_array()) continue;
          for (const auto& p : it.value()) {
            if (p.is_string() && p.get<std::string>() == "Order") can_order = true;
          }
        }
      }
      std::cout << "       readOnly=" << (read_only ? "true" : "false")
                << " ipWhitelist=" << (ip_locked ? "active" : "none") << "\n";
      std::cout << "       permissions: " << perms << "\n";

      trading_permitted = can_order && !read_only;
      if (!trading_permitted) {
        rep.Fail("user/query-api",
                 "key cannot place orders (readOnly or no 'Order' permission)");
      } else {
        rep.Pass("user/query-api", "orders permitted");
      }
    }
  }

  // public instrument facts, useful even when the key is restricted.
  {
    Response r;
    json j;
    std::string why;
    if (!HttpGet(spec.rest_base, "/v5/market/instruments-info", cat_q, nullptr,
                 0, "", r, err)) {
      rep.Fail("instruments-info", err);
    } else if (!ParseOk(r, j, why)) {
      rep.Fail("instruments-info", why);
    } else if (!j["result"].contains("list") || j["result"]["list"].empty()) {
      rep.Fail("instruments-info", "'" + spec.symbol + "' not listed");
    } else {
      const auto& item = j["result"]["list"][0];
      std::cout << "       tickSize="
                << item["priceFilter"].value("tickSize", "?")
                << " qtyStep=" << item["lotSizeFilter"].value("qtyStep", "?")
                << " minOrderQty="
                << item["lotSizeFilter"].value("minOrderQty", "?")
                << " minNotional="
                << item["lotSizeFilter"].value("minNotionalValue", "?") << "\n";
      if (item.contains("leverageFilter")) {
        std::cout << "       maxLeverage="
                  << item["leverageFilter"].value("maxLeverage", "?") << "\n";
      }
      rep.Pass("instruments-info", "instrument rules readable");
    }
  }

  if (keys.present() && trading_permitted) {
    {
      Response r;
      json j;
      std::string why;
      if (!signed_get("/v5/account/info", "", r)) {
        rep.Fail("account/info", err);
      } else if (!ParseOk(r, j, why)) {
        rep.Fail("account/info", why);
      } else {
        rep.Pass("account/info", j["result"].value("marginMode", "?"));
      }
    }
    {
      Response r;
      json j;
      std::string why;
      if (!signed_get("/v5/account/wallet-balance", "accountType=UNIFIED", r)) {
        rep.Fail("wallet-balance", err);
      } else if (!ParseOk(r, j, why)) {
        rep.Fail("wallet-balance", why);
      } else if (!j["result"].contains("list") || j["result"]["list"].empty()) {
        rep.Fail("wallet-balance", "empty account list");
      } else {
        const auto& acc = j["result"]["list"][0];
        std::cout << "       totalEquity=" << acc.value("totalEquity", "?")
                  << " imRate=" << acc.value("accountIMRate", "?")
                  << " mmRate=" << acc.value("accountMMRate", "?") << "\n";
        rep.Pass("wallet-balance", "equity feed readable");
      }
    }
    {
      Response r;
      json j;
      std::string why;
      if (!signed_get("/v5/market/risk-limit", cat_q, r)) {
        rep.Fail("risk-limit", err);
      } else if (!ParseOk(r, j, why)) {
        rep.Fail("risk-limit", why);
      } else {
        rep.Pass("risk-limit", "leverage tiers readable");
      }
    }
    {
      Response r;
      json j;
      std::string why;
      if (!signed_get("/v5/position/list", cat_q, r)) {
        rep.Fail("position/list", err);
      } else if (!ParseOk(r, j, why)) {
        rep.Fail("position/list", why);
      } else {
        int open = 0;
        if (j["result"].contains("list")) {
          for (const auto& p : j["result"]["list"]) {
            const double size = JsonNum(p, "size");
            if (size <= 0.0) continue;
            ++open;
            std::cout << "       open: " << p.value("symbol", "?") << " size="
                      << size << " side=" << p.value("side", "?") << "\n";
          }
        }
        if (open > 0) {
          rep.Fail("position/list",
                   "account is NOT flat; reconcile before arming the engine");
        } else {
          rep.Pass("position/list", "flat");
        }
      }
    }
    {
      Response r;
      json j;
      std::string why;
      if (!signed_get("/v5/order/realtime", cat_q, r)) {
        rep.Fail("order/realtime", err);
      } else if (!ParseOk(r, j, why)) {
        rep.Fail("order/realtime", why);
      } else {
        const size_t n = j["result"].contains("list")
                             ? j["result"]["list"].size()
                             : 0;
        if (n > 0) {
          rep.Fail("order/realtime",
                   std::to_string(n) + " resting order(s) already present");
        } else {
          rep.Pass("order/realtime", "no resting orders");
        }
      }
    }
    {
      Response r;
      json j;
      std::string why;
      if (!signed_get("/v5/account/fee-rate", cat_q, r)) {
        rep.Fail("account/fee-rate", err);
      } else if (!ParseOk(r, j, why)) {
        rep.Fail("account/fee-rate", why);
      } else if (j["result"].contains("list") && !j["result"]["list"].empty()) {
        const auto& f = j["result"]["list"][0];
        std::cout << "       maker=" << f.value("makerFeeRate", "?")
                  << " taker=" << f.value("takerFeeRate", "?") << "\n";
        rep.Pass("account/fee-rate", "fee schedule readable");
      } else {
        rep.Fail("account/fee-rate", "empty fee list");
      }
    }
    if (do_ws) {
      std::string detail;
      if (CheckPrivateWs(spec, keys, drift_ms, detail)) {
        rep.Pass("private-ws", detail);
      } else {
        rep.Fail("private-ws", detail);
      }
    } else {
      rep.Skip("private-ws", "--no-ws");
    }
  } else if (keys.present()) {
    rep.Skip("account/*", "orders are not permitted on this key");
  }

  std::cout << "\nengine_manifest " << mokuren::venue::Manifest(spec)
            << " clock_drift_ms=" << drift_ms << "\n";
  std::cout << (rep.failures == 0 ? "RESULT: OK\n" : "RESULT: FAILURES\n");
  return rep.failures == 0 ? 0 : 1;
}
