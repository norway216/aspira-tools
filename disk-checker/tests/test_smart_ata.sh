#!/bin/bash
# Test: ATA SMART data reading
# Validates: temperature, score range, state labels

source "$(dirname "$0")/helper.sh"

echo "=== test_smart_ata: SMART Data Validation ==="

JSON=$($DISK_HEALTH --json 2>/dev/null)

# 1. score is in [0, 100]
SCORE=$(echo "$JSON" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['devices'][0]['score'])" 2>/dev/null)
assert "Score is a number >= 0" python3 -c "exit(0 if int('${SCORE:-0}') >= 0 else 1)"
assert "Score is a number <= 100" python3 -c "exit(0 if int('${SCORE:-0}') <= 100 else 1)"
echo "  INFO: Score = $SCORE"

# 2. state is one of the valid labels
STATE=$(echo "$JSON" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['devices'][0]['state'])" 2>/dev/null)
assert "State is valid label" python3 -c "
state='${STATE}'
assert state in ('HEALTHY', 'WARNING', 'DEGRADED', 'CRITICAL'), f'bad state: {state}'
"
echo "  INFO: State = $STATE"

# 3. If temperature is present, it should be in a reasonable range
TEMP=$(echo "$JSON" | python3 -c "
import sys,json
d=json.load(sys.stdin)
dev=d['devices'][0]
t=dev.get('temperature')
print(t if t is not None else 'N/A')
" 2>/dev/null)

if [ "$TEMP" != "N/A" ] && [ "$TEMP" != "0" ]; then
    assert "Temperature in [0, 100] C" python3 -c "
t=int('${TEMP}')
assert 0 <= t <= 100, f'temp {t} out of range'
"
    echo "  INFO: Temperature = ${TEMP}°C"
else
    echo "  INFO: Temperature not available (permissions?)"
fi

# 4. reallocated_sectors is non-negative if present
REALLOC=$(echo "$JSON" | python3 -c "
import sys,json
d=json.load(sys.stdin)
dev=d['devices'][0]
r=dev.get('reallocated_sectors')
print(r if r is not None else 'N/A')
" 2>/dev/null)

if [ "$REALLOC" != "N/A" ]; then
    assert "Reallocated sectors >= 0" python3 -c "exit(0 if int('${REALLOC}') >= 0 else 1)"
    echo "  INFO: Reallocated = $REALLOC"
fi

# 5. Verify rotational field is present
ROT=$(echo "$JSON" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['devices'][0].get('rotational', 'N/A'))" 2>/dev/null)
assert "Rotational field is present" python3 -c "
r='${ROT}'
assert r in ('True', 'False'), f'bad rotational: {r}'
"

print_results
