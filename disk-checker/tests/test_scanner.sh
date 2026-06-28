#!/bin/bash
# Test: Device scanner functionality
# Validates: device detection, classification, identity

source "$(dirname "$0")/helper.sh"

echo "=== test_scanner: Device Detection ==="

# 1. JSON output is valid
JSON=$($DISK_HEALTH --json 2>/dev/null)
assert "JSON output is valid" python3 -c "import sys,json; json.loads(sys.argv[1])" "$JSON"

# 2. Contains devices array
assert "JSON contains 'devices' key" python3 -c "
import sys,json
d=json.loads(sys.argv[1])
assert 'devices' in d, 'no devices key'
assert len(d['devices']) > 0, 'empty devices'
" "$JSON"

# 3. At least one SATA device on this system (should have /dev/sda)
DEV_COUNT=$(echo "$JSON" | python3 -c "import sys,json; print(len(json.load(sys.stdin)['devices']))" 2>/dev/null)
echo "  INFO: Found $DEV_COUNT device(s)"

# 4. Check device fields exist
assert "Device has 'type' field" python3 -c "
import sys,json
d=json.loads(sys.argv[1])
dev=d['devices'][0]
assert 'type' in dev
assert dev['type'] in ('SATA', 'NVMe', 'UNKNOWN')
" "$JSON"

# 5. Check device name field
assert "Device has 'name' field" python3 -c "
import sys,json
d=json.loads(sys.argv[1])
dev=d['devices'][0]
assert 'name' in dev
assert len(dev['name']) > 0
" "$JSON"

# 6. --device flag filters correctly
JSON2=$($DISK_HEALTH --device /dev/sda --json 2>/dev/null)
assert "--device /dev/sda filters to sda" python3 -c "
import sys,json
d=json.loads(sys.argv[1])
for dev in d['devices']:
    assert '/dev/sda' in dev['device'], f'unexpected device {dev[\"device\"]}'
" "$JSON2"

# 7. --help prints usage
HELP=$($DISK_HEALTH --help 2>/dev/null)
USAGE_COUNT=$(echo "$HELP" | grep -c 'Usage:' || echo 0)
assert "--help shows usage line" python3 -c "exit(0 if ${USAGE_COUNT} > 0 else 1)"

print_results
