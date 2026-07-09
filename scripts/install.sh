#!/usr/bin/env bash
# =============================================================================
# AID2.0 production installer — interactive sudo wizard.
#
# WHAT: installs the AID daemon on a Debian-ish box so the git clone becomes
#       DISPOSABLE. Unlike scripts/deploy.sh (which runs the daemon in place out
#       of build/ and only ships the plugins to a dev root), this copies the
#       daemon + aid-admin + both plugins + the built UI OUT to system locations
#       and runs them under systemd:
#         /usr/local/bin/aid, /usr/local/bin/aid-admin
#         /var/lib/aid-daemon/{plugins,ui,auth.db,inbox.log,webhook.log}
#         /var/log/aid-daemon/{backend,frontend}.log
#         /etc/aid-daemon/config.json
#         /etc/systemd/system/aid-daemon.service
#
# WHY build-on-target: the daemon and BOTH plugins must be ONE matched build or
#       the ABI layout-tag / contract-tag guards refuse to load. Compiling them
#       together here guarantees that. The clone is KEPT after install (delete it
#       later to reclaim ~1.6 GB of build scratch — it does not affect the
#       running service).
#
# Modes (auto-detected): FRESH install (runs the wizard) vs UPGRADE (rebuild +
#       atomic swap of binaries/plugins/UI + restart; touches NOTHING in
#       /var/lib/aid-daemon or /etc/aid-daemon — data is preserved).
#
# Usage: sudo ./scripts/install.sh [--run-as <existing-user>]
#
# This script MIRRORS scripts/deploy.sh's guarantees (contract-tag gate,
# write-temp-then-rename atomic .so swap, post-launch liveness) but does not
# modify it. It is packaging/ops only — no C++/source changes.
# =============================================================================
set -euo pipefail

# --- constants (prod layout) -------------------------------------------------
DEPLOY_ROOT="/var/lib/aid-daemon"          # prod state root. walPath is emitted
                                           # into config as $DR/inbox.log and the
                                           # systemd unit exports AID_DEPLOY_ROOT,
                                           # so it resolves here under this root.
CONFIG_DIR="/etc/aid-daemon"
CONFIG="$CONFIG_DIR/config.json"
LOG_DIR="/var/log/aid-daemon"              # FHS logs (separate from state)
BIN_DIR="/usr/local/bin"
PLUGIN_DIR="$DEPLOY_ROOT/plugins"
UI_DIR="$DEPLOY_ROOT/ui"
AUTH_DB="$DEPLOY_ROOT/auth.db"
UNIT="/etc/systemd/system/aid-daemon.service"
DEFAULT_SERVICE_USER="aiddaemon"

# Literal ${AID_DEPLOY_ROOT} token for config paths (the daemon expands it at
# runtime; single-quoted here so bash does NOT expand it — it must land in
# config.json verbatim).
DR='${AID_DEPLOY_ROOT}'

# Build tree (separate from the dev build/ so Debug stays intact); Release.
BUILD_DIR="build-release"
DAEMON_SRC="$BUILD_DIR/src/aid"
ADMIN_SRC="$BUILD_DIR/src/aid-admin"
OP_PLUGIN_SRC="$BUILD_DIR/lib/adapters/openproject_plugin/aid_openproject_plugin.so"
DC_PLUGIN_SRC="$BUILD_DIR/lib/adapters/davical_plugin/aid_davical_plugin.so"

# Canonical OpenProject names the operator must create (checklist == matcher).
# The 7 custom-field slot keys and 5 status keys are HARDCODED in the plugin;
# only which OP entity each maps to is configurable. We resolve them by NAME.
CF_TEXT=(callId callerNumber calledNumber callStart callEnd)   # text fields
CF_LONG=(callLength callHandler)                               # long-text fields
CF_ALL=(callId callerNumber calledNumber callStart callEnd callLength callHandler)
# Only the 3 statuses the routing/close flow actually uses. (Rejected/Tested were
# removed from the enum + config schema entirely — plugin contract 4.)
declare -A STATUS_NAMES=(
    [statusNew]="New" [statusInProgress]="In progress" [statusClosed]="Closed"
)
TYPE_NAME="Call"
PROJECT_IDENTIFIER="aid-inbox"

# --- pretty output -----------------------------------------------------------
c_red=$'\033[31m'; c_grn=$'\033[32m'; c_ylw=$'\033[33m'; c_cyn=$'\033[36m'; c_bld=$'\033[1m'; c_rst=$'\033[0m'
info() { printf '%s==>%s %s\n' "$c_cyn" "$c_rst" "$*"; }
ok()   { printf '%s  ok%s %s\n' "$c_grn" "$c_rst" "$*"; }
warn() { printf '%s WARN%s %s\n' "$c_ylw" "$c_rst" "$*" >&2; }
die()  { printf '%sFATAL%s %s\n' "$c_red" "$c_rst" "$*" >&2; exit 1; }
hr()   { printf '%s\n' "------------------------------------------------------------------------"; }

# --- deploy.sh's contract-tag helper (copied, not sourced) -------------------
# Trailing `|| true` is load-bearing under set -e (grep exit 1 on no-match).
extract_contract_tag() {
    strings -a "$1" 2>/dev/null | grep -oE 'AID_PLUGIN_CONTRACT=[0-9]+' | sort -u | head -n1 || true
}

# --- drop privileges to run a step as another user (root vs sudo, both work) --
# Root installers may be invoked as bare `root` (no SUDO_USER) on a minimal box
# that lacks `sudo` entirely, so we never hard-depend on sudo:
#   * target user == root -> run the command DIRECTLY (no sudo/su needed).
#   * otherwise           -> prefer `runuser` (util-linux, always present);
#                            fall back to `sudo -u` only if runuser is absent.
run_as() { # run_as <user> <cmd...>
    local u="$1"; shift
    if [[ "$u" == "root" ]]; then
        "$@"
    elif command -v runuser >/dev/null; then
        runuser -u "$u" -- "$@"
    else
        sudo -u "$u" -- "$@"
    fi
}
# The compile must not run as root when a real invoking user exists (avoids
# root-owned cmake/pnpm artifacts in the clone). BUILD_USER is that user (or root).
as_build_user() { run_as "$BUILD_USER" "$@"; }

# --- visible liveness for long, SILENT phases --------------------------------
# apt and the cmake CONFIGURE step (Drogon/libxml2 FetchContent git clones) emit
# nothing for minutes — this animates a spinner + elapsed seconds so the operator
# can see it is alive. Output is tee'd to a temp log; on failure the tail is shown
# before we die. Non-TTY stdout (piped/CI/headless-upgrade) → plain begin/end
# lines, no animation, no control chars in the log.
run_spinner() { # run_spinner <label> <cmd...>
    local label="$1"; shift
    local log; log="$(mktemp "${TMPDIR:-/tmp}/aid-install.XXXXXX.log")"
    if [[ ! -t 1 ]]; then
        printf '%s==>%s %s ... ' "$c_cyn" "$c_rst" "$label"
        if "$@" >"$log" 2>&1; then printf 'done\n'; rm -f "$log"; return 0; fi
        printf 'FAILED\n'; warn "$label failed — last 40 lines:"; tail -n 40 "$log" >&2; rm -f "$log"
        die "$label failed"
    fi
    "$@" >"$log" 2>&1 &
    local pid=$! frames='|/-\' i=0 secs=0
    # `kill -0` probes liveness without signalling; loop until the child exits.
    while kill -0 "$pid" 2>/dev/null; do
        printf '\r%s[%s]%s %s  %ds ' "$c_cyn" "${frames:i++%4:1}" "$c_rst" "$label" "$secs"
        sleep 1; secs=$((secs+1))
    done
    if wait "$pid"; then
        printf '\r%s  ok%s %s  (%ds)          \n' "$c_grn" "$c_rst" "$label" "$secs"; rm -f "$log"; return 0
    fi
    printf '\r%sFAILED%s %s  (%ds)          \n' "$c_red" "$c_rst" "$label" "$secs"
    warn "$label failed — last 40 lines:"; tail -n 40 "$log" >&2; rm -f "$log"
    die "$label failed"
}

# =============================================================================
# Phase 0 — preflight
# =============================================================================
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

RUN_AS=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --run-as) RUN_AS="${2:-}"; [[ -n "$RUN_AS" ]] || die "--run-as needs a username"; shift 2 ;;
        --run-as=*) RUN_AS="${1#*=}"; shift ;;
        -h|--help) grep -E '^# ' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown arg '$1' (usage: 
sudo ./scripts/install.sh [--run-as <user>])" ;;
    esac
done

[[ "$(id -u)" -eq 0 ]] || die "must run as root: sudo ./scripts/install.sh"
command -v apt-get >/dev/null || die "this installer targets Debian/Ubuntu (apt-get not found)"
[[ -f CMakeLists.txt && -d scripts ]] || die "run from a checked-out AID2.0 repo (CMakeLists.txt not found)"

# The unprivileged user who runs the compile (avoids root-owned cmake/pnpm
# artifacts in the clone). Falls back to root with a warning if invoked as a
# bare root shell rather than via sudo.
BUILD_USER="${SUDO_USER:-root}"
if [[ "$BUILD_USER" == "root" ]]; then
    warn "no SUDO_USER — building as root; cmake/pnpm artifacts in the clone will be root-owned."
fi

# =============================================================================
# Phase 1 — mode detect
# =============================================================================
if [[ -f "$CONFIG" ]]; then
    MODE="upgrade"
    # Preserve the run-as user chosen at first install.
    SERVICE_USER="$(sed -nE 's/^User=(.+)$/\1/p' "$UNIT" 2>/dev/null | head -n1)"
    SERVICE_USER="${SERVICE_USER:-$DEFAULT_SERVICE_USER}"
    info "existing install detected ($CONFIG) → ${c_bld}UPGRADE${c_rst} (config, auth.db, WAL preserved)"
else
    MODE="fresh"
    SERVICE_USER="${RUN_AS:-$DEFAULT_SERVICE_USER}"
    info "no existing install → ${c_bld}FRESH install${c_rst}"
fi
[[ -n "$RUN_AS" && "$MODE" == "upgrade" && "$RUN_AS" != "$SERVICE_USER" ]] && \
    warn "ignoring --run-as $RUN_AS on upgrade; keeping existing service user '$SERVICE_USER'"

# A FRESH install runs the interactive wizard (OpenProject token, passwords,
# DaviCal creds — all require a human). With no TTY on stdin, every `read`
# returns EOF instantly and the wizard's while-loops would spin at 100% CPU
# forever. Fail fast HERE, before the ~10-minute build, so headless/piped/CI
# runs abort in seconds with a clear message. UPGRADE has no wizard → allowed
# headless (a scripted upgrade is legitimate).
if [[ "$MODE" == "fresh" && ! -t 0 ]]; then
    die "fresh install needs an interactive terminal (the setup wizard reads secrets from stdin).
       Run it directly in a real terminal — not piped, redirected from /dev/null, headless, or under CI.
       (An UPGRADE over an existing install runs non-interactively; this guard only blocks fresh installs.)"
fi

# =============================================================================
# Phase 2 — dependencies
# =============================================================================
install_deps() {
    info "installing build + runtime dependencies (apt)"
    export DEBIAN_FRONTEND=noninteractive
    run_spinner "apt-get update" apt-get update -qq
    # Build toolchain (clang-format/clang-tidy are dev-only → omitted).
    # Corrected -dev set vs the original notes: +libjsoncpp-dev (Drogon needs
    # Jsoncpp), uuid-dev (Drogon needs libuuid headers); -libbrotli-dev
    # (BUILD_BROTLI=OFF), -libcurl4-openssl-dev (curl not used).
    #
    # NOT here: the C++ compiler. g++-12 does not exist on trixie/13 (default is
    # g++-14) and is too old to be the default on bullseye — detect_and_pin_compiler
    # (called from build_all) resolves the right g++ ≥ 11 per release and pins it.
    # binutils → `strings` (contract-tag gate); iproute2 → `ss`/`ip` (health check
    # + LAN-IP autodetect) — both missing on minimal/container Debian.
    run_spinner "apt-get install (toolchain + -dev libs)" \
        apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git pkg-config ca-certificates \
        binutils iproute2 \
        libssl-dev zlib1g-dev libc-ares-dev libjsoncpp-dev uuid-dev \
        libsqlite3-dev libsodium-dev libphonenumber-dev \
        curl jq
    ok "C++ toolchain + runtime -dev libs + curl/jq installed"

    # Node (build-time only, for the UI). Debian's nodejs is too old for
    # Vite 8 / Svelte 5, so use NodeSource 22.x if we don't already have >=20.
    local need_node=1
    if command -v node >/dev/null; then
        local major; major="$(node -p 'process.versions.node.split(".")[0]' 2>/dev/null || echo 0)"
        [[ "$major" -ge 20 ]] && need_node=0
    fi
    if [[ "$need_node" -eq 1 ]]; then
        info "installing Node 22 via NodeSource (build-time only)"
        curl -fsSL https://deb.nodesource.com/setup_22.x | bash -
        apt-get install -y nodejs
    fi
    corepack enable >/dev/null 2>&1 || warn "corepack enable failed; ensure 'pnpm' is available"
    ok "Node $(node --version 2>/dev/null || echo '?') + pnpm $(as_build_user pnpm --version 2>/dev/null || echo '?')"
}

# Resolve a C++20-capable compiler (g++ ≥ 11 for coroutines) and echo its
# binary name. We PIN it via -DCMAKE_CXX_COMPILER so the build never silently
# falls back to a too-old default g++ (bullseye ships g++-10). Strategy:
#   1. default `g++` if its major ≥ 11 (true on bookworm=12, trixie=14).
#   2. else the newest g++-N already on PATH (14→11).
#   3. else apt-install the newest g++-N actually available in THIS release
#      (never blind `install g++-12` — absent on trixie).
# Prints the chosen compiler on stdout; all diagnostics go to stderr so the
# command substitution capturing it stays clean.
detect_and_pin_compiler() {
    local c major
    if command -v g++ >/dev/null; then
        major="$(g++ -dumpversion 2>/dev/null | cut -d. -f1)"
        if [[ "${major:-0}" -ge 11 ]]; then echo "g++"; return 0; fi
    fi
    for c in g++-14 g++-13 g++-12 g++-11; do
        command -v "$c" >/dev/null && { echo "$c"; return 0; }
    done
    warn "no g++ ≥ 11 on PATH — trying to install one from this release's archive" >&2
    for c in g++-14 g++-13 g++-12 g++-11; do
        if apt-cache policy "$c" 2>/dev/null | grep -q 'Candidate: [^(]'; then
            run_spinner "apt-get install $c" apt-get install -y --no-install-recommends "$c" >&2
            command -v "$c" >/dev/null && { echo "$c"; return 0; }
        fi
    done
    die "no C++20 compiler (g++ ≥ 11) is available or installable on this system.
       AID needs g++ ≥ 11 for coroutines. On Debian bullseye enable bullseye-backports
       (which offers g++-12), or upgrade the distribution."
}

# =============================================================================
# Phase 3 — build (as BUILD_USER), mirror deploy.sh's contract gate
# =============================================================================
build_all() {
    # On a FRESH install, start from a clean tree: an interrupted earlier run
    # (e.g. one killed after a headless spin) can leave a stale ninja graph, which
    # newer cmake+ninja hit as a dyndep `RefreshDyndepDependents` assertion abort.
    # UPGRADE keeps the tree for a fast incremental rebuild.
    if [[ "$MODE" == "fresh" ]]; then rm -rf "$BUILD_DIR"; fi

    local cxx; cxx="$(detect_and_pin_compiler)"
    info "using C++ compiler: $cxx ($("$cxx" -dumpversion 2>/dev/null || echo '?'))"

    info "configuring Release build in $BUILD_DIR/ (dev build/ untouched)"
    # AID_WERROR=OFF for the prod build: -Werror is a DEV guardrail. At -O3 (Release)
    # GCC 12 emits spurious -Wnull-dereference false positives from deeply-inlined
    # std::optional/std::string/std::shared_ptr/Error copies that never fire at -O0
    # (the dev Debug build). Those are not real bugs; a prod build from known-good
    # source must not abort on them. Warnings still print, just non-fatally.
    #
    # CMAKE_CXX_SCAN_FOR_MODULES=OFF: we use no C++20 modules. cmake ≥ 3.28 would
    # otherwise auto-enable module scanning → emit ninja dyndep, and older ninja
    # aborts on it (RefreshDyndepDependents assertion). OFF is ignored by pre-3.28
    # cmake, so it is safe on every release.
    run_spinner "cmake configure (fetches + builds Drogon — this is the long silent step)" \
        as_build_user cmake -S . -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DAID_BUILD_TESTS=OFF -DAID_WERROR=OFF \
        -DCMAKE_CXX_COMPILER="$cxx" -DCMAKE_CXX_SCAN_FOR_MODULES=OFF
    info "compiling daemon + aid-admin + both plugins (one matched build)"
    # Left UNwrapped: ninja prints live [N/total] progress — the best liveness signal.
    as_build_user cmake --build "$BUILD_DIR" -j \
        --target aid aid-admin aid_openproject_plugin aid_davical_plugin

    for f in "$DAEMON_SRC" "$ADMIN_SRC" "$OP_PLUGIN_SRC" "$DC_PLUGIN_SRC"; do
        [[ -f "$f" ]] || die "build produced no $f"
    done

    # Contract-tag gate (deploy.sh logic): daemon is the source of truth.
    local expected got
    expected="$(extract_contract_tag "$DAEMON_SRC")"
    [[ -n "$expected" ]] || die "daemon $DAEMON_SRC embeds no AID_PLUGIN_CONTRACT token (stale build?)"
    info "contract gate: daemon expects $expected"
    for so in "$OP_PLUGIN_SRC" "$DC_PLUGIN_SRC"; do
        got="$(extract_contract_tag "$so")"
        [[ -n "$got" ]]          || die "plugin $so exports no AID_PLUGIN_CONTRACT token"
        [[ "$got" == "$expected" ]] || die "plugin $so contract $got != daemon $expected (mismatched build)"
        ok "contract $got  $so"
    done

    # Strip for production (daemon 51 MB → ~5 MB; also faster). build-release is
    # throwaway, so strip in place.
    strip --strip-all "$DAEMON_SRC" "$ADMIN_SRC" "$OP_PLUGIN_SRC" "$DC_PLUGIN_SRC"
    ok "stripped binaries: daemon $(du -h "$DAEMON_SRC" | cut -f1)"

    # UI (adapter-static SPA). ALWAYS built fresh from this clone so the
    # frontend matches the backend (a stale ui/build silently kills live
    # dashboard deltas). Rebuild loses no data — the UI holds no state.
    info "building the dashboard UI (pnpm)"
    run_spinner "pnpm install (UI deps)" \
        as_build_user bash -c 'cd ui && pnpm install --frozen-lockfile'
    # pnpm build (Vite) prints its own progress → leave visible.
    as_build_user bash -c 'cd ui && pnpm build'
    [[ -f ui/build/index.html ]] || die "UI build produced no ui/build/index.html"
    ok "UI built → ui/build ($(du -sh ui/build | cut -f1))"
}

# =============================================================================
# wizard helpers
# =============================================================================
# Every `read` treats EOF (non-zero return) as fatal, not as an empty answer —
# so no wizard loop can busy-spin if stdin is ever non-interactive despite the
# preflight TTY guard.
_eof() { die "unexpected EOF on stdin — the wizard needs an interactive terminal (non-interactive/piped?)"; }
ask() { # ask <prompt> <default> -> echoes answer
    local prompt="$1" def="${2:-}" ans=""
    if [[ -n "$def" ]]; then read -r -p "  $prompt [$def]: " ans || _eof; echo "${ans:-$def}"
    else read -r -p "  $prompt: " ans || _eof; echo "$ans"; fi
}
ask_required() { local v; while :; do v="$(ask "$1" "${2:-}")"; [[ -n "$v" ]] && { echo "$v"; return; }; warn "required."; done; }
ask_secret() { # ask_secret <prompt> [minlen] -> echoes value
    # Input is intentionally VISIBLE (no `read -s`): the operator asked to see
    # what they type so a mistyped token/password is caught on the spot. Still
    # enforces the minimum length.
    local prompt="$1" min="${2:-1}" a=""
    while :; do
        read -r -p "  $prompt: " a || _eof
        [[ "${#a}" -ge "$min" ]] && { echo "$a"; return; }
        warn "must be at least $min characters."
    done
}
confirm_yes() { # confirm_yes <prompt> -> returns 0 on y/yes
    local ans; read -r -p "  $1 [y/N]: " ans || _eof; [[ "$ans" == y || "$ans" == yes || "$ans" == Y || "$ans" == YES ]]; }

# curl against the OpenProject API with apikey Basic auth (matches the plugin:
# Authorization: Basic base64("apikey:<token>")). Echoes body, returns curl rc.
op_api() { curl -fsS -u "apikey:$OP_API_TOKEN" -H 'Accept: application/json' "$OP_BASE_URL$1"; }

# The exact Payload URL the operator pastes into the ticket system's OUTGOING
# webhook. The daemon accepts the secret via ?secret=<v> OR the
# X-AID-Webhook-Secret header; we standardise on the query form so the whole
# thing is one copy-pasteable URL. Depends on LAN_IF, LISTEN_PORT, WEBHOOK_SECRET.
webhook_url() { printf 'http://%s:%s/hook/ticket?secret=%s' "$LAN_IF" "$LISTEN_PORT" "$WEBHOOK_SECRET"; }

# Print the "paste this into OpenProject" block (used on first entry and whenever
# the LAN IP / port / secret changes and the URL therefore changes).
show_webhook_url() {
    printf '  %sPaste this EXACT Payload URL into your ticket system'\''s outgoing webhook:%s\n' "$c_bld" "$c_rst"
    printf '      %s%s%s\n' "$c_grn" "$(webhook_url)" "$c_rst"
    printf '  (the secret is already in the URL as ?secret=… — no separate secret field needed;\n'
    printf '   enable the webhook for work packages: created + updated.)\n'
}

# Prompt for the webhook secret. Accepts either a bare secret or a pasted full
# payload URL (keeps only the value after secret=). Visible input by design.
prompt_webhook_secret() {
    WEBHOOK_SECRET="$(ask_required "Choose a webhook secret (any word — e.g. a name)")"
    if [[ "$WEBHOOK_SECRET" == *secret=* ]]; then
        WEBHOOK_SECRET="${WEBHOOK_SECRET##*secret=}"; WEBHOOK_SECRET="${WEBHOOK_SECRET%%&*}"
        info "detected a full payload URL — keeping only the value after 'secret='"
    fi
    [[ -n "$WEBHOOK_SECRET" ]] || die "webhook secret resolved to empty"
}

# =============================================================================
# Phase 4 — wizard (FRESH only)
# =============================================================================
run_wizard() {
    hr; printf '%sAID2.0 setup wizard%s\n' "$c_bld" "$c_rst"; hr

    # --- networking first (needed to print the webhook URL) ------------------
    info "network"
    local detected; detected="$(ip -4 route get 1.1.1.1 2>/dev/null | sed -nE 's/.* src ([0-9.]+).*/\1/p' | head -n1 || true)"
    LAN_IF="$(ask_required "This machine's STATIC LAN IP" "$detected")"
    warn "the LAN IP MUST be static — a DHCP address that changes on reboot breaks the daemon's"
    warn "bind (fatal exit) AND breaks calls.py's target. Reserve/static-assign it."
    LISTEN_PORT="$(ask "Port for AID" "8088")"
    POLL_SEC="$(ask "Membership poll interval seconds (0 disables)" "30")"

    # --- webhook secret first, so the checklist can show the EXACT URL to paste
    hr; printf '%sWebhook%s\n' "$c_bld" "$c_rst"
    prompt_webhook_secret

    # --- prepare-OpenProject checklist ---------------------------------------
    hr; printf '%sPrepare OpenProject%s — create the following, then confirm:\n' "$c_bld" "$c_rst"
    cat <<EOF
  1. A work-package TYPE named exactly:  ${c_bld}Call${c_rst}
  2. A PROJECT with identifier exactly:  ${c_bld}aid-inbox${c_rst}
  3. Three STATUSES exist named exactly:  New, In progress, Closed  (all OP defaults)
  4. Seven CUSTOM FIELDS (work-package), named exactly:
       TEXT      : callId, callerNumber, calledNumber, callStart, callEnd
       LONG TEXT : callLength, callHandler
  5. An OUTGOING WEBHOOK — see the exact URL below.
EOF
    show_webhook_url
    hr
    while ! confirm_yes "Have you created ALL of the above in OpenProject?"; do
        warn "create them first — the installer resolves their IDs by these exact names."
    done

    OP_BASE_URL="$(ask_required "OpenProject baseUrl (http OK, no trailing slash)")"
    OP_BASE_URL="${OP_BASE_URL%/}"
    OP_API_TOKEN="$(ask_secret "OpenProject API token")"

    # --- API discovery -------------------------------------------------------
    DISCOVERY_OK=0
    discover_ids || true    # sets DISCOVERY_OK, ID_* , PROJECT_NAMES_JSON, CUSTOM_FIELDS_JSON
    if [[ "$DISCOVERY_OK" -ne 1 ]]; then
        # skip path: blank IDs, service will be parked until filled.
        ID_TYPE_CALL=""
        ID_STATUS_NEW="" ID_STATUS_INPROGRESS="" ID_STATUS_CLOSED=""
        OP_FALLBACK_ID="" ; PROJECT_NAMES_JSON="{}"
        CUSTOM_FIELDS_JSON="$(printf '%s\n' "${CF_ALL[@]}" | jq -R . | jq -s 'map({(.): ""}) | add')"
    fi
    OP_PROJECT_WEB_BASE="$OP_BASE_URL/projects"

    # --- DaviCal -------------------------------------------------------------
    hr; printf '%sDaviCal (CardDAV contacts)%s\n' "$c_bld" "$c_rst"
    info "create a DaviCal user 'aid' holding the addresses + companies address books."
    while :; do
        DC_ADDR="$(ask_required "bookAddresses URL (full CardDAV collection URL)")"
        [[ "$DC_ADDR" =~ ^https?:// ]] || { warn "must start with http:// or https://"; continue; }
        [[ "$DC_ADDR" == */aid/addresses/ ]] || { warn "must end with /aid/addresses/"; continue; }
        break
    done
    # Derive companies from addresses (same host+path, last segment swapped) —
    # only the bookAddresses host is ever connected to, so they MUST share it.
    DC_COMP="${DC_ADDR%addresses/}companies/"
    info "derived bookCompanies: $DC_COMP"
    if ! confirm_yes "Use that companies URL?"; then
        while :; do
            DC_COMP="$(ask_required "bookCompanies URL")"
            [[ "$DC_COMP" =~ ^https?:// && "$DC_COMP" == */aid/companies/ ]] || { warn "must be http(s)://…/aid/companies/"; continue; }
            [[ "$(sed -E 's#(https?://[^/]+).*#\1#' <<<"$DC_COMP")" == "$(sed -E 's#(https?://[^/]+).*#\1#' <<<"$DC_ADDR")" ]] \
                || { warn "must share scheme+host+port with bookAddresses (only that host is connected to)."; continue; }
            break
        done
    fi
    DC_USER="$(ask "DaviCal user" "aid")"
    DC_PASS="$(ask_secret "DaviCal password")"
    # defaultRegion from locale (territory after '_' in LANG), else prompt.
    local loc_region=""; [[ "${LANG:-}" =~ _([A-Za-z]{2}) ]] && loc_region="$(tr '[:lower:]' '[:upper:]' <<<"${BASH_REMATCH[1]}")"
    DC_REGION="$(ask_required "Default phone region (ISO-3166 alpha-2, e.g. DE)" "$loc_region")"
    DC_REGION="$(tr '[:lower:]' '[:upper:]' <<<"$DC_REGION")"

    # --- auth / routing ------------------------------------------------------
    hr; printf '%sAuth & routing%s\n' "$c_bld" "$c_rst"
    INCOGNITO="$(ask "Incognito caller subject" "Incognito Caller")"
    SESSION_LIFETIME="$(ask "Session lifetime seconds" "2592000")"
    COOKIE_NAME="$(ask "Session cookie name" "aid_session")"
    LOG_LEVEL="$(ask "Log level (TRACE/DEBUG/INFO/WARN/ERROR)" "INFO")"
    info "recovery key — keep the plaintext safe; it resets the master/admin access."
    RECOVERY_KEY="$(ask_secret "Recovery key")"
    info "first dashboard user — the username MUST equal an OpenProject login."
    DASH_USER="$(ask_required "Dashboard username (== OpenProject login)")"
    DASH_PASS="$(ask_secret "Dashboard password (min 8)" 8)"
}

# Resolve OP entity IDs by name. Sets DISCOVERY_OK=1 on full success.
discover_ids() {
    while :; do
        info "validating API token (GET /api/v3/users/me)"
        if ! op_api "/api/v3/users/me" >/dev/null 2>&1; then
            warn "API not reachable / token rejected — check the URL, token, and any firewall."
        else
            local statuses types projects schema
            if statuses="$(op_api '/api/v3/statuses?pageSize=200')" \
               && types="$(op_api '/api/v3/types?pageSize=200')" \
               && projects="$(op_api '/api/v3/projects?pageSize=200')"; then

                # statuses: match each canonical name (case-insensitive) → id
                local k name id vn miss=0
                for k in statusNew statusInProgress statusClosed; do
                    name="${STATUS_NAMES[$k]}"
                    id="$(jq -r --arg n "$name" '._embedded.elements[] | select((.name|ascii_downcase)==($n|ascii_downcase)) | .id | tostring' <<<"$statuses" | head -n1 || true)"
                    [[ -n "$id" && "$id" != null ]] || { warn "status '$name' not found in OpenProject"; miss=1; }
                    case "$k" in
                        statusNew)        vn=ID_STATUS_NEW ;;
                        statusInProgress) vn=ID_STATUS_INPROGRESS ;;
                        statusClosed)     vn=ID_STATUS_CLOSED ;;
                    esac
                    declare -g "$vn=$id"
                done
                # type "Call"
                ID_TYPE_CALL="$(jq -r --arg n "$TYPE_NAME" '._embedded.elements[] | select((.name|ascii_downcase)==($n|ascii_downcase)) | .id | tostring' <<<"$types" | head -n1 || true)"
                [[ -n "$ID_TYPE_CALL" && "$ID_TYPE_CALL" != null ]] || { warn "type '$TYPE_NAME' not found"; miss=1; }
                # project aid-inbox → fallback id. projectNames maps ONLY aid-inbox
                # (id -> identifier): we deliberately do NOT enumerate every project,
                # so the config doesn't go stale as new OP projects are created.
                OP_FALLBACK_ID="$(jq -r --arg i "$PROJECT_IDENTIFIER" '._embedded.elements[] | select(.identifier==$i) | .id | tostring' <<<"$projects" | head -n1 || true)"
                [[ -n "$OP_FALLBACK_ID" && "$OP_FALLBACK_ID" != null ]] || { warn "project '$PROJECT_IDENTIFIER' not found"; miss=1; }
                PROJECT_NAMES_JSON="$(jq -c --arg i "$PROJECT_IDENTIFIER" '[._embedded.elements[] | select(.identifier==$i) | {(.id|tostring): .identifier}] | add // {}' <<<"$projects")"

                # custom fields: the per-project-type schema exposes customFieldN
                # objects each with a .name. The GLOBAL /work_packages/schema does
                # NOT exist (404) — it is per (project-type): /schemas/{proj}-{type}.
                schema="{}"
                if [[ -n "$OP_FALLBACK_ID" && "$OP_FALLBACK_ID" != null && -n "$ID_TYPE_CALL" && "$ID_TYPE_CALL" != null ]]; then
                    schema="$(op_api "/api/v3/work_packages/schemas/${OP_FALLBACK_ID}-${ID_TYPE_CALL}" 2>/dev/null || echo '{}')"
                else
                    warn "cannot fetch custom-field schema until project + type resolve"; miss=1
                fi
                local cf_json="{}" key
                for name in "${CF_ALL[@]}"; do
                    key="$(jq -r --arg n "$name" 'to_entries[] | select(.key|startswith("customField")) | select(((.value.name? // "")|ascii_downcase)==($n|ascii_downcase)) | .key' <<<"$schema" | grep -E '^customField[0-9]+$' | head -n1 || true)"
                    if [[ -n "$key" ]]; then
                        cf_json="$(jq -c --arg k "$name" --arg v "${key#customField}" '. + {($k): $v}' <<<"$cf_json")"
                    else
                        warn "custom field '$name' not found in work-package schema"; miss=1
                        cf_json="$(jq -c --arg k "$name" '. + {($k): ""}' <<<"$cf_json")"
                    fi
                done
                CUSTOM_FIELDS_JSON="$cf_json"

                if [[ "$miss" -eq 0 ]]; then
                    ok "resolved: type=$ID_TYPE_CALL statuses(N=$ID_STATUS_NEW,IP=$ID_STATUS_INPROGRESS,C=$ID_STATUS_CLOSED) fallback=$OP_FALLBACK_ID"
                    DISCOVERY_OK=1; return 0
                fi
                warn "some names above did not match — fix them in OpenProject and retry."
            else
                warn "one or more discovery calls failed."
            fi
        fi
        # A failed API call is usually a mistyped token or base URL. Let the
        # operator fix any single input in place and retry, instead of aborting
        # the whole wizard. LAN IP / port / webhook do not affect discovery, but
        # they are offered here too so an earlier typo can be corrected before the
        # config is written (and the webhook URL is reprinted when it changes).
        local choice
        printf '  %sFix an input, then retry:%s\n' "$c_bld" "$c_rst"
        read -r -p "  [r] retry  [a] OpenProject URL+token  [i] LAN IP  [p] port  [w] webhook  [s] skip: " choice || _eof
        case "$choice" in
            a|A)
                OP_BASE_URL="$(ask_required "OpenProject baseUrl (http OK, no trailing slash)")"
                OP_BASE_URL="${OP_BASE_URL%/}"
                OP_API_TOKEN="$(ask_secret "OpenProject API token")"
                ;;
            i|I)
                LAN_IF="$(ask_required "This machine's STATIC LAN IP")"
                info "webhook URL changed:"; show_webhook_url
                ;;
            p|P)
                LISTEN_PORT="$(ask "Port for AID" "$LISTEN_PORT")"
                info "webhook URL changed:"; show_webhook_url
                ;;
            w|W)
                prompt_webhook_secret; show_webhook_url
                ;;
            s|S)
                warn "SKIPPING discovery. The config will be written with BLANK OpenProject IDs."
                warn "The daemon will NOT function until you fill them into $CONFIG and restart the service."
                if confirm_yes "Are you sure you want to skip?"; then DISCOVERY_OK=0; return 1; fi
                ;;
            *) : ;;   # anything else (incl. 'r') => loop and retry
        esac
    done
}

# =============================================================================
# Phase 5 — place & configure (root)
# =============================================================================
ensure_user_and_dirs() {
    if [[ -n "$RUN_AS" ]]; then
        id "$SERVICE_USER" >/dev/null 2>&1 || die "--run-as user '$SERVICE_USER' does not exist"
        warn "running the service as existing user '$SERVICE_USER' — it has that user's privileges."
    else
        getent group "$SERVICE_USER" >/dev/null || groupadd --system "$SERVICE_USER"
        id "$SERVICE_USER" >/dev/null 2>&1 || \
            useradd --system --gid "$SERVICE_USER" --no-create-home --shell /usr/sbin/nologin "$SERVICE_USER"
        ok "system user '$SERVICE_USER' ready"
    fi
    install -d -o "$SERVICE_USER" -g "$SERVICE_USER" -m 0750 "$DEPLOY_ROOT" "$PLUGIN_DIR" "$LOG_DIR"
    install -d -o root -g root -m 0755 "$CONFIG_DIR"
    ok "state dirs: $DEPLOY_ROOT (0750), $LOG_DIR (0750), $CONFIG_DIR"
}

# Atomic copy: write-temp-then-rename (mirror deploy.sh step 3) so a load never
# sees a half-written file; re-verify plugin contract on the landed .so.
atomic_install() { # atomic_install <src> <dst> <mode> <owner>
    local src="$1" dst="$2" mode="$3" owner="$4" tmp="$2.tmp.$$"
    install -o "$owner" -g "$owner" -m "$mode" "$src" "$tmp"
    mv -f "$tmp" "$dst"
}

place_artifacts() {
    info "placing binaries, plugins, and UI (atomic)"
    local expected; expected="$(extract_contract_tag "$DAEMON_SRC")"
    atomic_install "$DAEMON_SRC" "$BIN_DIR/aid"       0755 root
    atomic_install "$ADMIN_SRC"  "$BIN_DIR/aid-admin" 0755 root
    atomic_install "$OP_PLUGIN_SRC" "$PLUGIN_DIR/openproject_plugin.so" 0640 "$SERVICE_USER"
    atomic_install "$DC_PLUGIN_SRC" "$PLUGIN_DIR/davical_plugin.so"     0640 "$SERVICE_USER"
    for so in "$PLUGIN_DIR/openproject_plugin.so" "$PLUGIN_DIR/davical_plugin.so"; do
        [[ "$(extract_contract_tag "$so")" == "$expected" ]] || die "landed $so contract != daemon (corrupt copy)"
    done
    # UI: replace atomically via a staging dir + rename.
    rm -rf "$UI_DIR.new"
    cp -a ui/build "$UI_DIR.new"
    chown -R "$SERVICE_USER:$SERVICE_USER" "$UI_DIR.new"
    find "$UI_DIR.new" -type d -exec chmod 0755 {} +; find "$UI_DIR.new" -type f -exec chmod 0644 {} +
    rm -rf "$UI_DIR"; mv "$UI_DIR.new" "$UI_DIR"
    ok "installed → $BIN_DIR/aid, $PLUGIN_DIR/*.so (contract $expected), $UI_DIR"
}

write_config() {
    # Emits only statusNew/InProgress/Closed — the full set the daemon now knows.
    # The Rejected/Tested statuses were removed from the TicketStatus enum and the
    # config schema (plugin contract 4), so the OpenProject plugin no longer
    # requires them and loads cleanly with this config.
    info "generating $CONFIG (via jq — secrets safely escaped)"
    local recovery_hash
    recovery_hash="$(printf '%s\n%s\n' "$RECOVERY_KEY" "$RECOVERY_KEY" \
        | "$BIN_DIR/aid-admin" hash-recovery-key 2>/dev/null \
        | grep -oE '\$argon2id\$[^[:space:]]+' | head -n1 || true)"
    [[ -n "$recovery_hash" ]] || die "failed to hash the recovery key"

    local tmp="$CONFIG.tmp.$$"
    jq -n \
        --argjson listenPort "$LISTEN_PORT" \
        --arg lanInterface "$LAN_IF" \
        --argjson pollSec "$POLL_SEC" \
        --arg walPath "$DR/inbox.log" \
        --arg logLevel "$LOG_LEVEL" \
        --arg backendLog "$LOG_DIR/backend.log" \
        --arg frontendLog "$LOG_DIR/frontend.log" \
        --arg dbPath "$DR/auth.db" \
        --argjson sessLife "$SESSION_LIFETIME" \
        --arg cookieName "$COOKIE_NAME" \
        --arg recoveryHash "$recovery_hash" \
        --arg baseUrl "$OP_BASE_URL" \
        --arg apiToken "$OP_API_TOKEN" \
        --arg typeCall "$ID_TYPE_CALL" \
        --arg statusNew "$ID_STATUS_NEW" \
        --arg statusInProgress "$ID_STATUS_INPROGRESS" \
        --arg statusClosed "$ID_STATUS_CLOSED" \
        --arg projWebBase "$OP_PROJECT_WEB_BASE" \
        --argjson projectNames "$PROJECT_NAMES_JSON" \
        --argjson customFieldIds "$CUSTOM_FIELDS_JSON" \
        --arg bookAddresses "$DC_ADDR" \
        --arg bookCompanies "$DC_COMP" \
        --arg dcUser "$DC_USER" \
        --arg dcPass "$DC_PASS" \
        --arg region "$DC_REGION" \
        --arg docRoot "$DR/ui" \
        --arg unknownFallback "$OP_FALLBACK_ID" \
        --arg incognito "$INCOGNITO" \
        --arg tsLib "$DR/plugins/openproject_plugin.so" \
        --arg abLib "$DR/plugins/davical_plugin.so" \
        --arg webhookSecret "$WEBHOOK_SECRET" \
        '{
            listenPort: $listenPort,
            lanInterface: $lanInterface,
            membershipPollIntervalSec: $pollSec,
            walPath: $walPath,
            Logger: { level: $logLevel, backendLogPath: $backendLog, frontendLogPath: $frontendLog },
            Auth: {
                dbPath: $dbPath, sessionLifetimeSeconds: $sessLife,
                cookieName: $cookieName, cookieSecure: false, recoveryKeyHash: $recoveryHash
            },
            TicketSystem: {
                baseUrl: $baseUrl, apiToken: $apiToken, typeCall: $typeCall,
                statusNew: $statusNew, statusInProgress: $statusInProgress, statusClosed: $statusClosed,
                projectWebBaseUrl: $projWebBase, projectNames: $projectNames, customFieldIds: $customFieldIds
            },
            AddressSystem: {
                bookAddresses: $bookAddresses, bookCompanies: $bookCompanies,
                user: $dcUser, password: $dcPass, defaultRegion: $region
            },
            Ui: { documentRoot: $docRoot },
            TicketRouting: { unknownFallback: $unknownFallback, incognitoSubject: $incognito },
            Plugins: { ticketStore: { libPath: $tsLib }, addressBook: { libPath: $abLib } },
            Webhook: { secret: $webhookSecret }
        }' > "$tmp"
    # Config::load requires: regular file, owner == daemon-or-root, mode <= 0640.
    chown "$SERVICE_USER:$SERVICE_USER" "$tmp"; chmod 0640 "$tmp"
    mv -f "$tmp" "$CONFIG"
    ok "wrote $CONFIG (owner $SERVICE_USER, mode 0640, cookieSecure=false)"
}

create_first_user() {
    info "creating first dashboard user '$DASH_USER'"
    if printf '%s\n%s\n' "$DASH_PASS" "$DASH_PASS" \
        | run_as "$SERVICE_USER" "$BIN_DIR/aid-admin" --db "$AUTH_DB" add-user --username "$DASH_USER" >/dev/null 2>&1; then
        chown "$SERVICE_USER:$SERVICE_USER" "$AUTH_DB" 2>/dev/null || true
        ok "user '$DASH_USER' created (auth.db owned by $SERVICE_USER)"
    else
        warn "aid-admin add-user failed (user may already exist). Add later with:"
        warn "  sudo -u $SERVICE_USER aid-admin --db $AUTH_DB add-user --username <name>"
    fi
}

# =============================================================================
# Phase 6 — systemd unit + start
# =============================================================================
install_service() {
    info "installing systemd unit $UNIT"
    cat > "$UNIT" <<EOF
[Unit]
Description=AID daemon
Wants=network-online.target
After=network-online.target

[Service]
User=$SERVICE_USER
Group=$SERVICE_USER
# CWD must be a service-user-writable dir: Drogon creates its upload cache at the
# RELATIVE path ./uploads/tmp/00..FF on startup. Without this, CWD defaults to /
# and the unprivileged service user floods backend.log with 256 EACCES errors.
WorkingDirectory=$DEPLOY_ROOT
Environment=AID_DEPLOY_ROOT=$DEPLOY_ROOT
ExecStart=$BIN_DIR/aid $CONFIG
KillMode=mixed
Restart=on-failure

[Install]
WantedBy=multi-user.target
EOF
    systemctl daemon-reload
    systemctl enable --now aid-daemon
    ok "service enabled (autostart on boot) and started"
}

restart_service() { info "restarting service"; systemctl restart aid-daemon; }

health_check() {
    local port="$1" url="http://127.0.0.1:$1/health" i body
    info "verifying aid-daemon is active + GET /health on 127.0.0.1:$port"
    # CRITICAL: confirm OUR systemd unit is active BEFORE trusting /health. A
    # stray process already bound to the port (e.g. a dev daemon run from the
    # build tree) would answer /health and mask a bind failure of our service —
    # so the port responding is NOT proof our daemon started. is-active==active
    # means trantor bound the listener and the process stayed up.
    for i in $(seq 1 10); do
        systemctl is-active --quiet aid-daemon && break
        sleep 1
    done
    if ! systemctl is-active --quiet aid-daemon; then
        warn "aid-daemon is NOT active — likely a bind failure (another process on :$port?)."
        warn "  port holder: $(ss -ltnp 2>/dev/null | grep ":$port" | head -n1 || echo '?')"
        systemctl --no-pager --lines=15 status aid-daemon || true
        warn "logs: journalctl -u aid-daemon -n 40"
        return 1
    fi
    for i in $(seq 1 15); do
        if body="$(curl -fsS --max-time 2 "$url" 2>/dev/null)"; then
            ok "service active; health: $body"; return 0
        fi
        sleep 1
    done
    warn "service is active but no /health after 15s. Check: journalctl -u aid-daemon -n 40"
    return 1
}

# =============================================================================
# Phase 7 — print manual operator steps (we never touch firewalls / OP config)
# =============================================================================
print_manual_steps() {
    local port; port="$(jq -r '.listenPort' "$CONFIG")"
    local lanif; lanif="$(jq -r '.lanInterface' "$CONFIG")"
    local baseurl; baseurl="$(jq -r '.TicketSystem.baseUrl' "$CONFIG")"
    local bookaddr; bookaddr="$(jq -r '.AddressSystem.bookAddresses' "$CONFIG")"
    local secret; secret="$(jq -r '.Webhook.secret' "$CONFIG")"
    hr; printf '%sMANUAL STEPS (the installer does NOT change firewalls or OpenProject)%s\n' "$c_bld" "$c_rst"
    cat <<EOF
  Firewall — allow INBOUND to $lanif:$port from:
      * your phone-system host       (POST /call)
      * your OpenProject host        (POST /hook/ticket)
  Firewall — allow OUTBOUND from this host to:
      * OpenProject  ($baseurl)
      * DaviCal      ($(sed -E 's#(https?://[^/]+).*#\1#' <<<"$bookaddr"))
  OpenProject — confirm the outgoing webhook's Payload URL is EXACTLY:
      ${c_grn}http://$lanif:$port/hook/ticket?secret=$secret${c_rst}
  Service — manage with:
      systemctl {status|restart|stop} aid-daemon   |   journalctl -u aid-daemon -f
EOF
    hr
}

# =============================================================================
# main
# =============================================================================
install_deps
build_all

if [[ "$MODE" == "fresh" ]]; then
    run_wizard
    ensure_user_and_dirs
    place_artifacts
    write_config
    create_first_user
    install_service
    if [[ "$DISCOVERY_OK" -eq 1 ]]; then
        health_check "$LISTEN_PORT" || true
    else
        warn "OpenProject IDs are BLANK (discovery skipped) — the service is PARKED."
        warn "Fill the empty ids in $CONFIG, then: systemctl restart aid-daemon"
    fi
    print_manual_steps
    ok "FRESH install complete."
else
    # upgrade: rebuild + atomic swap + restart; config/auth.db/WAL untouched.
    place_artifacts
    restart_service
    local_port="$(jq -r '.listenPort' "$CONFIG")"
    health_check "$local_port" || true
    ok "UPGRADE complete (config, auth.db, WAL preserved)."
fi
