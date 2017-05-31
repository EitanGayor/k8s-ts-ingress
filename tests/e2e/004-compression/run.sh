#! /bin/sh
# vim:set sw=8 ts=8 noet:

# Test compression.

expect_output() {
	if echo "$output" | grep -q "$@"; then
		return 0
	else
		echo "Failed: output did not include test string: [$*]"
		echo "Test output: [$output]"
		exit 1
	fi
}


set -e

# Uncached compression.

output=$(curl -isS --resolve echoheaders.test:58080:127.0.0.1 \
		http://echoheaders.test:58080/this-is-a-test)
expect_output "Vary: Accept-Encoding"
expect_output -v "Content-Encoding"

output=$(curl -isS --compressed --resolve echoheaders.test:58080:127.0.0.1 \
		http://echoheaders.test:58080/this-is-a-test)
expect_output "Vary: Accept-Encoding"
expect_output "Content-Encoding: gzip"
expect_output "Request path: /this-is-a-test"

# Cached compression.
output=$(curl -isS --resolve echoheaders.test:58080:127.0.0.1 \
		http://echoheaders.test:58080/cached/test)
output=$(curl -isS --resolve echoheaders.test:58080:127.0.0.1 \
		http://echoheaders.test:58080/cached/test)
expect_output "X-Cache-Status: hit-fresh"
expect_output "Vary: Accept-Encoding"
expect_output -v "Content-Encoding"

output=$(curl -isS --compressed --resolve echoheaders.test:58080:127.0.0.1 \
		http://echoheaders.test:58080/cached/test)
expect_output "X-Cache-Status: hit-fresh"
expect_output "Vary: Accept-Encoding"
expect_output "Content-Encoding"
expect_output "Request path: /cached/test"
