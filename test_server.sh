#!/usr/bin/env bash
set -euo pipefail

PORT="${PORT:-8080}"
WORKERS="${WORKERS:-4}"
DOCROOT="${DOCROOT:-./www}"
ACCESS_LOG="${ACCESS_LOG:-./access.log}"
ERROR_LOG="${ERROR_LOG:-./error.log}"
HOST="${HOST:-127.0.0.1}"
BASE_URL="http://${HOST}:${PORT}"

# Load test tuning
LOAD_DURATION_SEC="${LOAD_DURATION_SEC:-8}"
LOAD_CONCURRENCY_LIST="${LOAD_CONCURRENCY_LIST:-10 50 100 200}"
RAMP_START_CONCURRENCY="${RAMP_START_CONCURRENCY:-100}"
RAMP_STEP="${RAMP_STEP:-100}"
RAMP_MAX_CONCURRENCY="${RAMP_MAX_CONCURRENCY:-10000}"
RPS_STOP_THRESHOLD="${RPS_STOP_THRESHOLD:-50}"
LOAD_THREADS="${LOAD_THREADS:-4}"
AB_REQUESTS_PER_CLIENT="${AB_REQUESTS_PER_CLIENT:-50}"
LOAD_CSV_PATH="${LOAD_CSV_PATH:-./load_ramp.csv}"
LOCK_DIR="${LOCK_DIR:-./.test_server.lock}"

SERVER_PID=""
TMP_DIR=""

red() { printf "\033[31m%s\033[0m\n" "$*"; }
green() { printf "\033[32m%s\033[0m\n" "$*"; }
yellow() { printf "\033[33m%s\033[0m\n" "$*"; }
blue() { printf "\033[34m%s\033[0m\n" "$*"; }

cleanup() {
  if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
    kill -TERM "${SERVER_PID}" 2>/dev/null || true
    wait "${SERVER_PID}" 2>/dev/null || true
  fi
  if [[ -n "${TMP_DIR}" && -d "${TMP_DIR}" ]]; then
    rm -rf "${TMP_DIR}"
  fi
  rm -f "${DOCROOT}/huge.bin" 2>/dev/null || true
  rm -f "${DOCROOT}/near_limit.bin" 2>/dev/null || true
  rmdir "${LOCK_DIR}" 2>/dev/null || true
}
trap cleanup EXIT

require_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    red "Missing required command: $1"
    exit 1
  fi
}

acquire_lock() {
  if mkdir "${LOCK_DIR}" 2>/dev/null; then
    return 0
  fi
  red "Another test_server.sh instance is already running."
  yellow "If this is stale, remove lock dir: ${LOCK_DIR}"
  exit 1
}

http_code() {
  local url="$1"
  shift
  curl -sS -o /dev/null -w "%{http_code}" "$@" "$url"
}

assert_code() {
  local name="$1"
  local expected="$2"
  local actual="$3"
  if [[ "${actual}" == "${expected}" ]]; then
    green "[OK] ${name}: ${actual}"
  else
    red "[FAIL] ${name}: expected ${expected}, got ${actual}"
    return 1
  fi
}

start_server() {
  rm -f "${ACCESS_LOG}" "${ERROR_LOG}"
  ./server -p "${PORT}" -w "${WORKERS}" -d "${DOCROOT}" -l "${ACCESS_LOG}" -e "${ERROR_LOG}" &
  SERVER_PID=$!
  sleep 0.7
  if kill -0 "${SERVER_PID}" 2>/dev/null; then
    return 0
  fi

  red "Server failed to start on ${HOST}:${PORT}"
  if [[ -f "${ERROR_LOG}" ]]; then
    yellow "Last error log lines:"
    awk 'NR>max{print buf[NR%max]} {buf[NR%max]=$0} END{for(i=NR-max+1;i<=NR;i++) if(i>0) print buf[i%max]}' max=5 "${ERROR_LOG}" || true
  fi
  yellow "Tip: port may be busy. Try:"
  yellow "  PORT=18080 ./test_server.sh"
  exit 1
}

run_functional_tests() {
  blue "== Functional tests =="
  local ok=0
  local total=0

  total=$((total + 1))
  assert_code "GET / -> 200" "200" "$(http_code "${BASE_URL}/")" && ok=$((ok + 1))

  total=$((total + 1))
  assert_code "HEAD / -> 200" "200" "$(http_code "${BASE_URL}/" -I)" && ok=$((ok + 1))

  total=$((total + 1))
  assert_code "POST / -> 405" "405" "$(http_code "${BASE_URL}/" -X POST)" && ok=$((ok + 1))

  total=$((total + 1))
  assert_code "GET missing -> 404" "404" "$(http_code "${BASE_URL}/no_such_file.txt")" && ok=$((ok + 1))

  total=$((total + 1))
  assert_code "GET traversal -> 403" "403" "$(http_code "${BASE_URL}/../etc/passwd" --path-as-is)" && ok=$((ok + 1))

  total=$((total + 1))
  truncate -s 125829120 "${DOCROOT}/near_limit.bin"
  local near_tmp="${TMP_DIR}/near_limit_download.bin"
  local near_code
  near_code="$(curl -sS -o "${near_tmp}" -w "%{http_code}" "${BASE_URL}/near_limit.bin")"
  if [[ "${near_code}" != "200" ]]; then
    red "[FAIL] GET <=128MiB (status): expected 200, got ${near_code}"
  else
    local expected_size actual_size
    expected_size="$(wc -c < "${DOCROOT}/near_limit.bin" | tr -d ' ')"
    actual_size="$(wc -c < "${near_tmp}" | tr -d ' ')"
    if [[ "${actual_size}" == "${expected_size}" ]]; then
      green "[OK] GET <=128MiB transfer size: ${actual_size} bytes"
      ok=$((ok + 1))
    else
      red "[FAIL] GET <=128MiB transfer size: expected ${expected_size}, got ${actual_size}"
    fi
  fi
  rm -f "${DOCROOT}/near_limit.bin" "${near_tmp}"

  total=$((total + 1))
  truncate -s 135266304 "${DOCROOT}/huge.bin"
  assert_code "GET >128MiB -> 403" "403" "$(http_code "${BASE_URL}/huge.bin")" && ok=$((ok + 1))
  rm -f "${DOCROOT}/huge.bin"

  echo
  blue "Functional: ${ok}/${total} passed"
  if [[ "${ok}" -ne "${total}" ]]; then
    return 1
  fi
}

run_log_checks() {
  blue "== Log checks =="
  if [[ -f "${ACCESS_LOG}" ]]; then
    local lines
    lines=$(wc -l < "${ACCESS_LOG}" | tr -d ' ')
    if [[ "${lines}" -gt 0 ]]; then
      green "[OK] access log exists and has ${lines} line(s)"
    else
      yellow "[WARN] access log exists but empty"
    fi
  else
    red "[FAIL] access log does not exist: ${ACCESS_LOG}"
    return 1
  fi

  if [[ -f "${ERROR_LOG}" ]]; then
    local err_lines
    err_lines=$(wc -l < "${ERROR_LOG}" | tr -d ' ')
    yellow "[INFO] error log lines: ${err_lines}"
  else
    yellow "[INFO] error log not created yet (no errors)"
  fi
}

run_load_test_wrk() {
  blue "== Load test (wrk) =="
  local best_c=0
  local best_rps=0

  for c in ${LOAD_CONCURRENCY_LIST}; do
    local out
    out="$(wrk -t4 -c"${c}" -d"${LOAD_DURATION_SEC}s" "${BASE_URL}/" 2>/dev/null || true)"
    local rps
    rps="$(printf "%s\n" "${out}" | awk '/Requests\/sec:/ {print $2; exit}')"
    local latency
    latency="$(printf "%s\n" "${out}" | awk '/Latency/ {print $2 " " $3; exit}')"

    if [[ -z "${rps}" ]]; then
      yellow "[WARN] wrk failed for c=${c}"
      continue
    fi

    printf "c=%-4s rps=%-12s latency=%s\n" "${c}" "${rps}" "${latency:-n/a}"
    awk "BEGIN{exit !(${rps} > ${best_rps})}" && { best_rps="${rps}"; best_c="${c}"; }
  done

  echo
  if [[ "${best_c}" -gt 0 ]]; then
    green "Best wrk run: c=${best_c}, rps=${best_rps}"
  else
    yellow "No successful wrk runs"
  fi
}

run_load_test_ab() {
  blue "== Load test (ab) =="
  local best_c=0
  local best_rps=0
  local best_fail=999999

  for c in ${LOAD_CONCURRENCY_LIST}; do
    local n=$((c * 50))
    local out
    out="$(ab -n "${n}" -c "${c}" "${BASE_URL}/" 2>/dev/null || true)"
    local rps
    local fail
    rps="$(printf "%s\n" "${out}" | awk -F': *' '/Requests per second/ {print $2}' | awk '{print $1}' | head -n1)"
    fail="$(printf "%s\n" "${out}" | awk -F': *' '/Failed requests/ {print $2}' | awk '{print $1}' | head -n1)"

    if [[ -z "${rps}" || -z "${fail}" ]]; then
      yellow "[WARN] ab failed for c=${c}"
      continue
    fi

    printf "c=%-4s n=%-6s rps=%-12s failed=%s\n" "${c}" "${n}" "${rps}" "${fail}"

    if [[ "${fail}" -eq 0 ]]; then
      awk "BEGIN{exit !(${rps} > ${best_rps})}" && { best_rps="${rps}"; best_c="${c}"; best_fail="${fail}"; }
    fi
  done

  echo
  if [[ "${best_c}" -gt 0 ]]; then
    green "Best ab run (zero fails): c=${best_c}, rps=${best_rps}"
  else
    yellow "No zero-fail ab runs. Check output above."
  fi
}

csv_init() {
  cat > "${LOAD_CSV_PATH}" <<'EOF'
timestamp,tool,concurrency,duration_sec,rps,latency_ms,failed_requests,stop_threshold
EOF
}

csv_append() {
  local tool="$1"
  local c="$2"
  local duration="$3"
  local rps="$4"
  local latency_ms="$5"
  local failed="$6"
  printf "%s,%s,%s,%s,%s,%s,%s,%s\n" \
    "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
    "${tool}" "${c}" "${duration}" "${rps}" "${latency_ms}" "${failed}" "${RPS_STOP_THRESHOLD}" \
    >> "${LOAD_CSV_PATH}"
}

run_client_ramp_wrk() {
  blue "== Client ramp test (wrk) =="
  csv_init

  local c="${RAMP_START_CONCURRENCY}"
  local stopped=0
  while [[ "${c}" -le "${RAMP_MAX_CONCURRENCY}" ]]; do
    local out
    out="$(wrk -t"${LOAD_THREADS}" -c"${c}" -d"${LOAD_DURATION_SEC}s" --latency "${BASE_URL}/" 2>/dev/null || true)"

    local rps
    rps="$(printf "%s\n" "${out}" | awk '/Requests\/sec:/ {print $2; exit}')"
    if [[ -z "${rps}" ]]; then
      yellow "[WARN] wrk failed to produce metrics at c=${c}"
      csv_append "wrk" "${c}" "${LOAD_DURATION_SEC}" "NaN" "NaN" "NaN"
      break
    fi

    local lat_raw
    local lat_value lat_unit latency_ms
    lat_raw="$(printf "%s\n" "${out}" | awk '/Latency/ {print $2; exit}')"
    lat_value="${lat_raw//[^0-9.]/}"
    lat_unit="${lat_raw//[0-9.]/}"
    latency_ms="NaN"
    if [[ -n "${lat_value}" && -n "${lat_unit}" ]]; then
      case "${lat_unit}" in
        us) latency_ms="$(awk "BEGIN { printf \"%.3f\", ${lat_value}/1000 }")" ;;
        ms) latency_ms="$(awk "BEGIN { printf \"%.3f\", ${lat_value} }")" ;;
        s) latency_ms="$(awk "BEGIN { printf \"%.3f\", ${lat_value}*1000 }")" ;;
      esac
    fi

    local failed
    failed="$(printf "%s\n" "${out}" | awk '/Socket errors:/ {sum=0; for(i=3;i<=NF;i++){gsub(/[^0-9]/,"",$i); if($i!="") sum+=$i} print sum; exit}')"
    if [[ -z "${failed}" ]]; then
      failed="0"
    fi

    csv_append "wrk" "${c}" "${LOAD_DURATION_SEC}" "${rps}" "${latency_ms}" "${failed}"
    printf "c=%-5s rps=%-12s latency_ms=%-10s failed=%s\n" "${c}" "${rps}" "${latency_ms}" "${failed}"

    if awk "BEGIN {exit !(${rps} <= ${RPS_STOP_THRESHOLD})}"; then
      yellow "Stop: RPS ${rps} <= threshold ${RPS_STOP_THRESHOLD}"
      stopped=1
      break
    fi
    c=$((c + RAMP_STEP))
  done

  if [[ "${stopped}" -eq 0 && "${c}" -gt "${RAMP_MAX_CONCURRENCY}" ]]; then
    yellow "Reached max concurrency (${RAMP_MAX_CONCURRENCY}) before threshold."
  fi
  green "CSV exported: ${LOAD_CSV_PATH}"
}

run_client_ramp_ab() {
  blue "== Client ramp test (ab) =="
  csv_init

  local c="${RAMP_START_CONCURRENCY}"
  local stopped=0
  while [[ "${c}" -le "${RAMP_MAX_CONCURRENCY}" ]]; do
    local n=$((c * AB_REQUESTS_PER_CLIENT))
    local out
    out="$(ab -n "${n}" -c "${c}" "${BASE_URL}/" 2>/dev/null || true)"

    local rps
    rps="$(printf "%s\n" "${out}" | awk -F': *' '/Requests per second/ {print $2}' | awk '{print $1}' | head -n1)"
    if [[ -z "${rps}" ]]; then
      yellow "[WARN] ab failed to produce metrics at c=${c}"
      csv_append "ab" "${c}" "NaN" "NaN" "NaN" "NaN"
      break
    fi

    local failed
    failed="$(printf "%s\n" "${out}" | awk -F': *' '/Failed requests/ {print $2}' | awk '{print $1}' | head -n1)"
    [[ -z "${failed}" ]] && failed="0"

    local latency_ms
    latency_ms="$(printf "%s\n" "${out}" | awk -F': *' '/Time per request/ && !seen {print $2; seen=1}' | awk '{print $1}' | head -n1)"
    [[ -z "${latency_ms}" ]] && latency_ms="NaN"

    csv_append "ab" "${c}" "NaN" "${rps}" "${latency_ms}" "${failed}"
    printf "c=%-5s n=%-7s rps=%-12s latency_ms=%-10s failed=%s\n" "${c}" "${n}" "${rps}" "${latency_ms}" "${failed}"

    if awk "BEGIN {exit !(${rps} <= ${RPS_STOP_THRESHOLD})}"; then
      yellow "Stop: RPS ${rps} <= threshold ${RPS_STOP_THRESHOLD}"
      stopped=1
      break
    fi
    c=$((c + RAMP_STEP))
  done

  if [[ "${stopped}" -eq 0 && "${c}" -gt "${RAMP_MAX_CONCURRENCY}" ]]; then
    yellow "Reached max concurrency (${RAMP_MAX_CONCURRENCY}) before threshold."
  fi
  green "CSV exported: ${LOAD_CSV_PATH}"
}

main() {
  acquire_lock

  require_cmd make
  require_cmd curl
  require_cmd truncate

  TMP_DIR="$(mktemp -d)"

  blue "== Build =="
  make clean >/dev/null
  make >/dev/null
  green "[OK] build complete"
  echo

  blue "== Start server =="
  start_server
  green "[OK] server started: pid=${SERVER_PID}, url=${BASE_URL}"
  echo

  run_functional_tests
  echo
  run_log_checks
  echo

  if command -v wrk >/dev/null 2>&1; then
    run_client_ramp_wrk
  elif command -v ab >/dev/null 2>&1; then
    run_client_ramp_ab
  else
    yellow "Neither wrk nor ab found; skipping load test."
    yellow "Install one of them: wrk or apache2-utils (ab)."
  fi

  echo
  green "Done."
}

main "$@"
