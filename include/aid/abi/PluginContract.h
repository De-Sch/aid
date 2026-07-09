#pragma once

// Behaviour-contract tag for the plugin `.so` <-> daemon pair (BF3 guard).
//
// The PluginAbiTag fingerprint (PluginAbiTag.h) versions the *layout* of the
// value-types that cross the boundary, and aid_plugin_api_version() versions
// the extern "C" factory *contract*. Neither catches a same-layout, same-API
// plugin that is simply STALE — built from an older source tree and therefore
// missing newer behaviour the daemon now relies on. That exact trap cost real
// debug time: a pre-callHandler-drop ticket-system `.so` loaded cleanly (matching
// layout + API) yet silently emitted no admin handler-drop deltas, because the
// deployed `.so` lagged the daemon binary.
//
// kPluginContractTag closes that gap. It is a monotonic behaviour-contract
// marker, bumped by hand whenever a plugin gains behaviour the daemon depends
// on. It is embedded verbatim — as a greppable token in `.rodata` — in BOTH the
// daemon binary (main.cpp logs it) and every plugin `.so` (each exports
// aid_plugin_contract_tag()). Two independent guards consume it:
//
//   * scripts/deploy.sh extracts the tag from the freshly-built daemon and from
//     each just-copied `.so` with `strings`, and aborts the deploy loudly if a
//     plugin's tag is missing or differs — a stale `.so` never reaches the
//     running system.
//   * main.cpp logs the daemon tag + each loaded plugin's tag at startup and
//     REFUSES to start on absence/mismatch (checkPluginContractTag), so a
//     hand-restarted daemon pointed at a stale `.so` fails fast instead of
//     running degraded.
//
// Bump protocol: when a plugin's behaviour contract changes, increment the
// integer here AND rebuild + redeploy BOTH plugins together (scripts/deploy.sh
// does this atomically). The trailing integer is the single source of truth;
// the string form exists only so `strings` can find it.
//
// Contract levels:
//   1 — legacy / pre-callHandler-drop plugins (these predate this tag and
//       therefore export NO aid_plugin_contract_tag symbol at all).
//   2 — admin callHandler-drop -> live ticket_remove deltas (S7/S8) and later.
//   3 — ports gained the cancelPendingRequests() shutdown hook: adds a vtable
//       slot the daemon CALLS during graceful drain, so a
//       contract-2 `.so` must be rejected (its vtable lacks the slot — calling
//       it would be UB) AND it would not cancel in-flight upstream requests.
//   4 — the Rejected/Tested ticket statuses were removed. The config schema no
//       longer carries statusRejected/statusTested, so a contract-3 `.so` (which
//       still requireString()s them) would refuse the new config at load time;
//       bumping lets deploy.sh/main() reject the stale `.so` up front. Layout is
//       unchanged (enum-member removal does not alter any struct sizeof), so the
//       PluginAbiTag deliberately does NOT move.
//
// Header has no dependencies beyond <cstring>'s declarations indirectly; it is
// includable by a plugin `.so` (which links only aid_ports) and by the daemon.

namespace aid::abi {

// The behaviour-contract level this translation unit was compiled against,
// rendered as the greppable token both deploy.sh and main.cpp consume.
// `inline constexpr` gives it a single definition across every TU; it is
// odr-used (returned by the plugin factory symbol, logged by main) so the
// literal is guaranteed to land in the binary's `.rodata` for `strings`.
inline constexpr char kPluginContractTag[] = "AID_PLUGIN_CONTRACT=4";

} // namespace aid::abi
