.PHONY: build build-upstream version test bench clean

build:
	bash ./scripts/build.sh

build-upstream:
	GVISOR_REF=master bash ./scripts/build.sh

version: build
	./bin/tcp-shift --version

test: build
	go test -modfile=go.local.mod ./...

bench: build
	sudo -E bash ./scripts/bench-long-fat.sh

clean:
	rm -rf bin .deps .bench go.local.mod go.local.sum
