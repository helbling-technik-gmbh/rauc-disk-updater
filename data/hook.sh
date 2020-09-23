#!/bin/sh
VERSION=""
INDEX=0

for i in $(seq 1 $BUNDLES); do
    eval BUNDLE_VERSION=\$BUNDLE_VERSION_${i}
    if [ "$BUNDLE_VERSION" \> "$VERSION" ]; then
	VERSION="${BUNDLE_VERSION}"
	INDEX="$i"
    fi
done
exit $INDEX
