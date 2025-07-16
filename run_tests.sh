#!/usr/bin/env bash
set -euo pipefail

# directory dove risiede questo script (e quindi i tuoi binari)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

SERVER="$SCRIPT_DIR/server"
CLIENT="$SCRIPT_DIR/collector_client"

echo "Usando server  : $SERVER"
echo "Usando client  : $CLIENT"

# --- STEP 0: pulizia eventuali FIFO residue ---
echo "🔄 Pulizia FIFO esistenti…"
rm -f /tmp/fifo_in /tmp/fifo_client_*

# --- STEP 1: creazione file di prova ---
echo "🗂️  Creazione file di test…"
WORKDIR=$(mktemp -d)
pushd "$WORKDIR" >/dev/null

# piccolo file
echo -n "hello" > f_small
# 1 KB
dd if=/dev/zero of=f_1k bs=1 count=1024 &>/dev/null
# 2 KB
dd if=/dev/zero of=f_2k bs=1 count=2048 &>/dev/null

# --- STEP 2: avvio server in background ---
echo "🚀 Avvio server…"
"$SERVER" &
SERVER_PID=$!
# attendi che mkfifo sia eseguito
sleep 1

# funzione d’aiuto per hash atteso
expected_hash() {
  sha256sum "$1" | awk '{print $1}'
}

# --- TEST 1: hash singolo file ---
echo -n "Test 1 – hash singolo… "
OUT=$("$CLIENT" f_small)
GOT=$(echo "$OUT" | grep "SHA-256" | awk '{print $2}')
EXP=$(expected_hash f_small)
if [[ "$GOT" == "$EXP" ]]; then
  echo "✔"
else
  echo "✗ atteso $EXP, ottenuto $GOT"
  kill $SERVER_PID; exit 1
fi

# --- TEST 2: ordinamento per dimensione ---
echo -n "Test 2 – ordinamento… "
OUT=$("$CLIENT" f_2k f_small f_1k)
# prendi solo le linee con il nome file
ORDER=($(echo "$OUT" | grep '(' | awk '{print $1}'))
if [[ "${ORDER[0]}" == "f_small" && "${ORDER[1]}" == "f_1k" && "${ORDER[2]}" == "f_2k" ]]; then
  echo "✔"
else
  echo "✗ trovato ordine: ${ORDER[*]}"
  kill $SERVER_PID; exit 1
fi

# --- TEST 3: file inesistente ---
echo -n "Test 3 – file inesistente… "
if "$CLIENT" nofile &>/dev/null; then
  echo "✗ non ha segnalato errore"
  kill $SERVER_PID; exit 1
else
  echo "✔"
fi

# --- TEST 4: cache – misuro tempi ---
echo -n "Test 4 – cache… "
START1=$(date +%s%3N)
"$CLIENT" f_1k &>/dev/null
END1=$(date +%s%3N)
START2=$(date +%s%3N)
"$CLIENT" f_1k &>/dev/null
END2=$(date +%s%3N)
DT1=$((END1-START1))
DT2=$((END2-START2))
if (( DT2 < DT1 )); then
  echo "✔ ($DT1 ms vs $DT2 ms)"
else
  echo "✗ ($DT1 ms vs $DT2 ms)"
  kill $SERVER_PID; exit 1
fi

# --- TEST 5: concorrenti / stress test ---
echo -n "Test 5 – stress concorrenti… "
for i in {1..8}; do
  "$CLIENT" f_small f_1k f_2k & 
done
wait
echo "✔"

# --- CLEANUP ---
kill $SERVER_PID
popd >/dev/null
rm -rf "$WORKDIR"

echo "🎉 Tutti i test sono passati!"
