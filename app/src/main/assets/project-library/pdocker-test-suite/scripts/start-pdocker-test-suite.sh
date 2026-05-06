#!/bin/sh
set -eu

mkdir -p /workspace /reports /documents /shared
cat > /reports/ready <<'EOF'
pdocker test suite container is ready.

Run the suite through docker exec:

  docker exec pdocker-test-suite run-pdocker-test-suite

Reports are written to /reports and mirrored to
/documents/pdocker-exports/pdocker-test-suite when the Documents mount is
writable.
EOF

printf 'pdocker test suite ready; run with docker exec pdocker-test-suite run-pdocker-test-suite\n'

while :; do
  sleep 3600
done
