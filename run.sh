#!/usr/bin/env bash
# ============================================================
#  run.sh — HFT Engine launcher with kernel/CPU tuning
# ============================================================
set -euo pipefail

# ─── Colours ──────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'

info()  { echo -e "${CYAN}[INFO]${NC}  $*"; }
ok()    { echo -e "${GREEN}[OK]${NC}    $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
err()   { echo -e "${RED}[ERR]${NC}   $*"; }
step()  { echo -e "\n${BOLD}──── $* ────${NC}"; }

# ─── Defaults ─────────────────────────────────────────────────
BINARY="./hft_engine"
SIGNAL_CORE=3
FEED_CORE=2
NO_ROOT=0
DRY_RUN=0
NUMA_NODE=0

# ─── Parse args ───────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case $1 in
        --no-root)  NO_ROOT=1 ;;
        --dry-run)  DRY_RUN=1 ;;
        --core)     SIGNAL_CORE="$2"; FEED_CORE=$(( SIGNAL_CORE - 1 )); shift ;;
        --binary)   BINARY="$2"; shift ;;
        --numa)     NUMA_NODE="$2"; shift ;;
        -h|--help)
            echo "Usage: $0 [--no-root] [--dry-run] [--core N] [--binary PATH] [--numa N]"
            exit 0 ;;
        *) err "Unknown arg: $1"; exit 1 ;;
    esac
    shift
done

RUN() {
    if [[ $DRY_RUN -eq 1 ]]; then echo -e "  ${YELLOW}[DRY]${NC} $*"; else eval "$@"; fi
}

NEED_ROOT() {
    if [[ $EUID -ne 0 && $NO_ROOT -eq 0 ]]; then
        err "This step requires root. Re-run with sudo or pass --no-root to skip tuning."
        exit 1
    fi
    if [[ $EUID -ne 0 ]]; then warn "Skipping (not root): $*"; return 1; fi
    return 0
}

echo -e "\n${BOLD}  HFT Signal Engine — launcher${NC}"
echo -e "  signal core=${SIGNAL_CORE}  feed core=${FEED_CORE}  numa=${NUMA_NODE}\n"

step "1/7 binary check"
if [[ ! -f "$BINARY" ]]; then
    warn "Binary not found — building now..."
    RUN make release
fi
ok "Binary: $BINARY"

step "2/7 system check"
TOTAL_CORES=$(sysctl -n hw.logicalcpu 2>/dev/null || nproc)
info "CPU cores available: $TOTAL_CORES"

step "3/7 CPU governor"
if NEED_ROOT "set CPU governor"; then
    GOVERNOR_PATH="/sys/devices/system/cpu/cpu${SIGNAL_CORE}/cpufreq/scaling_governor"
    if [[ -f "$GOVERNOR_PATH" ]]; then
        RUN "echo performance > $GOVERNOR_PATH"
        ok "Governor set to performance"
    else
        warn "cpufreq not available (MacOS/VM — skipping)"
    fi
fi

step "4/7 C-state latency"
if NEED_ROOT "disable C-states"; then
    LATENCY_FILE="/dev/cpu_dma_latency"
    if [[ -c "$LATENCY_FILE" ]]; then
        info "Locking CPU to C0 state"
        RUN "exec 9>$LATENCY_FILE && printf '\\x00\\x00\\x00\\x00' >&9"
        ok "C-states disabled"
    else
        warn "/dev/cpu_dma_latency not available — skipping"
    fi
fi

step "5/7 huge pages"
if NEED_ROOT "allocate huge pages"; then
    if [[ -f /proc/sys/vm/nr_hugepages ]]; then
        RUN "echo 32 > /proc/sys/vm/nr_hugepages"
        ok "Huge pages allocated"
    else
        warn "Huge pages unavailable on this OS"
    fi
fi

step "6/7 IRQ affinity"
if NEED_ROOT "move IRQs off hot cores"; then
    for irq_dir in /proc/irq/*/smp_affinity_list; do
        [[ -f "$irq_dir" ]] && RUN "echo 0 > $irq_dir" 2>/dev/null || true
    done
    ok "IRQs migrated"
fi

step "7/7 launch"
LAUNCH_CMD=""
if command -v numactl &>/dev/null && [[ $EUID -eq 0 ]]; then
    LAUNCH_CMD="numactl --cpunodebind=$NUMA_NODE --membind=$NUMA_NODE "
fi
if [[ $EUID -eq 0 ]] && command -v chrt &>/dev/null; then
    LAUNCH_CMD="$LAUNCH_CMD chrt -f 99 "
fi
if command -v taskset &>/dev/null; then
    LAUNCH_CMD="$LAUNCH_CMD taskset -c ${FEED_CORE},${SIGNAL_CORE} "
fi
LAUNCH_CMD="$LAUNCH_CMD $BINARY"

info "Command: $LAUNCH_CMD"

if [[ $DRY_RUN -eq 1 ]]; then
    ok "[DRY RUN] Would execute: $LAUNCH_CMD"
    exit 0
fi

eval $LAUNCH_CMD