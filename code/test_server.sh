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
RAMP_START_CONCURRENCY="${RAMP_START_CONCURRENCY:-50}"
RAMP_STEP="${RAMP_STEP:-100}"
RAMP_MAX_CONCURRENCY="${RAMP_MAX_CONCURRENCY:-2000}"
RPS_STOP_THRESHOLD="${RPS_STOP_THRESHOLD:-50}"
LOAD_THREADS="${LOAD_THREADS:-4}"
AB_REQUESTS_PER_CLIENT="${AB_REQUESTS_PER_CLIENT:-50}"
LOAD_CSV_PATH="${LOAD_CSV_PATH:-./load_ramp.csv}"
LOAD_WORKER_COUNTS="${LOAD_WORKER_COUNTS:-4 8 16}"
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

stop_server() {
  if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
    kill -TERM "${SERVER_PID}" 2>/dev/null || true
    wait "${SERVER_PID}" 2>/dev/null || true
  fi
  SERVER_PID=""
  sleep 0.2
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
  local header
  header="timestamp,tool,workers,concurrency,duration_sec,rps,transfer_sec_human,transfer_bytes_sec,latency_ms,failed_requests,connect_errors,read_errors,write_errors,timeout_errors,stop_reason"

  if [[ ! -f "${LOAD_CSV_PATH}" || ! -s "${LOAD_CSV_PATH}" ]]; then
    printf "%s\n" "${header}" > "${LOAD_CSV_PATH}"
    return
  fi

  local first_line
  first_line="$(awk 'NR==1 {print; exit}' "${LOAD_CSV_PATH}")"
  if [[ "${first_line}" != "${header}" ]]; then
    yellow "CSV header mismatch for ${LOAD_CSV_PATH}. Keeping existing file and appending anyway."
  fi
}

csv_append() {
  local tool="$1"
  local workers="$2"
  local c="$3"
  local duration="$4"
  local rps="$5"
  local transfer_human="$6"
  local transfer_bps="$7"
  local latency_ms="$8"
  local failed="$9"
  local connect_err="${10}"
  local read_err="${11}"
  local write_err="${12}"
  local timeout_err="${13}"
  local stop_reason="${14}"
  printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n" \
    "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
    "${tool}" "${workers}" "${c}" "${duration}" "${rps}" "${transfer_human}" "${transfer_bps}" \
    "${latency_ms}" "${failed}" "${connect_err}" "${read_err}" "${write_err}" "${timeout_err}" \
    "${stop_reason}" \
    >> "${LOAD_CSV_PATH}"
}

transfer_human_to_bps() {
  local raw="$1"
  local value unit factor
  value="${raw//[^0-9.]/}"
  unit="${raw//[0-9.]/}"
  factor=0
  case "${unit}" in
    B) factor=1 ;;
    KB) factor=1024 ;;
    MB) factor=$((1024*1024)) ;;
    GB) factor=$((1024*1024*1024)) ;;
    TB) factor=$((1024*1024*1024*1024)) ;;
    *)
      echo "NaN"
      return
      ;;
  esac
  awk "BEGIN { printf \"%.3f\", ${value} * ${factor} }"
}

run_client_ramp_wrk_for_workers() {
  local workers="$1"
  WORKERS="${workers}"
  start_server
  green "[LOAD] worker_count=${workers}, url=${BASE_URL}"

  local c="${RAMP_START_CONCURRENCY}"
  local stopped=0
  while [[ "${c}" -le "${RAMP_MAX_CONCURRENCY}" ]]; do
    local out
    out="$(wrk -t"${LOAD_THREADS}" -c"${c}" -d"${LOAD_DURATION_SEC}s" --latency "${BASE_URL}/" 2>/dev/null || true)"

    local rps
    rps="$(printf "%s\n" "${out}" | awk '/Requests\/sec:/ {print $2; exit}')"
    if [[ -z "${rps}" ]]; then
      yellow "[WARN] wrk failed to produce metrics at c=${c}"
      csv_append "wrk" "${workers}" "${c}" "${LOAD_DURATION_SEC}" "NaN" "NaN" "NaN" "NaN" "NaN" "NaN" "NaN" "NaN" "NaN" "metrics_parse_failed"
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

    local transfer_human transfer_bps
    transfer_human="$(printf "%s\n" "${out}" | awk '/Transfer\/sec:/ {print $2; exit}')"
    [[ -z "${transfer_human}" ]] && transfer_human="NaN"
    transfer_bps="$(transfer_human_to_bps "${transfer_human}")"

    local connect_err read_err write_err timeout_err
    read -r connect_err read_err write_err timeout_err < <(
      printf "%s\n" "${out}" | awk '
        /Socket errors:/ {
          c=0; r=0; w=0; t=0;
          for (i=1; i<=NF; i++) {
            if ($i=="connect") {v=$(i+1); gsub(/[^0-9]/, "", v); c=v+0}
            if ($i=="read")    {v=$(i+1); gsub(/[^0-9]/, "", v); r=v+0}
            if ($i=="write")   {v=$(i+1); gsub(/[^0-9]/, "", v); w=v+0}
            if ($i=="timeout") {v=$(i+1); gsub(/[^0-9]/, "", v); t=v+0}
          }
          print c, r, w, t;
          found=1;
          exit;
        }
        END { if (!found) print "0 0 0 0"; }'
    )

    local failed=$((connect_err + read_err + write_err + timeout_err))
    local stop_reason="continue"
    if [[ "${failed}" -gt 0 ]]; then
      stop_reason="first_socket_error"
    fi

    csv_append "wrk" "${workers}" "${c}" "${LOAD_DURATION_SEC}" "${rps}" "${transfer_human}" "${transfer_bps}" \
      "${latency_ms}" "${failed}" "${connect_err}" "${read_err}" "${write_err}" "${timeout_err}" "${stop_reason}"
    printf "w=%-3s c=%-5s rps=%-12s transfer=%-10s latency_ms=%-10s socket_errors(connect=%s read=%s write=%s timeout=%s)\n" \
      "${workers}" "${c}" "${rps}" "${transfer_human}" "${latency_ms}" "${connect_err}" "${read_err}" "${write_err}" "${timeout_err}"

    if [[ "${failed}" -gt 0 ]]; then
      yellow "Stop: first socket error at c=${c} (total=${failed})"
      stopped=1
      break
    fi
    c=$((c + RAMP_STEP))
  done

  if [[ "${stopped}" -eq 0 && "${c}" -gt "${RAMP_MAX_CONCURRENCY}" ]]; then
    yellow "Reached max concurrency (${RAMP_MAX_CONCURRENCY}) without socket errors."
  fi
  stop_server
}

run_client_ramp_ab_for_workers() {
  local workers="$1"
  WORKERS="${workers}"
  start_server
  green "[LOAD] worker_count=${workers}, url=${BASE_URL}"

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
      csv_append "ab" "${workers}" "${c}" "NaN" "NaN" "NaN" "NaN" "NaN" "NaN" "NaN" "NaN" "NaN" "NaN" "metrics_parse_failed"
      break
    fi

    local failed
    failed="$(printf "%s\n" "${out}" | awk -F': *' '/Failed requests/ {print $2}' | awk '{print $1}' | head -n1)"
    [[ -z "${failed}" ]] && failed="0"

    local latency_ms
    latency_ms="$(printf "%s\n" "${out}" | awk -F': *' '/Time per request/ && !seen {print $2; seen=1}' | awk '{print $1}' | head -n1)"
    [[ -z "${latency_ms}" ]] && latency_ms="NaN"

    local transfer_value transfer_bps transfer_human
    transfer_value="$(printf "%s\n" "${out}" | awk -F': *' '/Transfer rate/ {print $2}' | awk '{print $1}' | head -n1)"
    if [[ -n "${transfer_value}" ]]; then
      transfer_human="${transfer_value}KB"
      transfer_bps="$(awk "BEGIN { printf \"%.3f\", ${transfer_value} * 1024 }")"
    else
      transfer_human="NaN"
      transfer_bps="NaN"
    fi

    local stop_reason="continue"
    if [[ "${failed}" -gt 0 ]]; then
      stop_reason="first_failed_request"
    fi

    csv_append "ab" "${workers}" "${c}" "NaN" "${rps}" "${transfer_human}" "${transfer_bps}" "${latency_ms}" "${failed}" "NaN" "NaN" "NaN" "NaN" "${stop_reason}"
    printf "w=%-3s c=%-5s n=%-7s rps=%-12s transfer=%-10s latency_ms=%-10s failed=%s\n" \
      "${workers}" "${c}" "${n}" "${rps}" "${transfer_human}" "${latency_ms}" "${failed}"

    if [[ "${failed}" -gt 0 ]]; then
      yellow "Stop: first failed request at c=${c} (failed=${failed})"
      stopped=1
      break
    fi
    c=$((c + RAMP_STEP))
  done

  if [[ "${stopped}" -eq 0 && "${c}" -gt "${RAMP_MAX_CONCURRENCY}" ]]; then
    yellow "Reached max concurrency (${RAMP_MAX_CONCURRENCY}) without failed requests."
  fi
  stop_server
}

run_load_matrix_wrk() {
  blue "== Client ramp test (wrk) across worker counts =="
  csv_init
  for w in ${LOAD_WORKER_COUNTS}; do
    blue "-- worker_count=${w} --"
    run_client_ramp_wrk_for_workers "${w}"
  done
  green "CSV exported: ${LOAD_CSV_PATH}"
}

run_load_matrix_ab() {
  blue "== Client ramp test (ab) across worker counts =="
  csv_init
  for w in ${LOAD_WORKER_COUNTS}; do
    blue "-- worker_count=${w} --"
    run_client_ramp_ab_for_workers "${w}"
  done
  green "CSV exported: ${LOAD_CSV_PATH}"
}

main() {
  local original_workers="${WORKERS}"
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

  stop_server

  if command -v wrk >/dev/null 2>&1; then
    run_load_matrix_wrk
  elif command -v ab >/dev/null 2>&1; then
    run_load_matrix_ab
  else
    yellow "Neither wrk nor ab found; skipping load test."
    yellow "Install one of them: wrk or apache2-utils (ab)."
  fi
  WORKERS="${original_workers}"

  echo
  green "Done."
}

main "$@"
