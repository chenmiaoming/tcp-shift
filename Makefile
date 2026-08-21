.PHONY: build test bench clean

build:
	./scripts/build.sh

test: build
	go test -modfile=go.local.mod ./...

bench: build
	sudo -E ./scripts/bench-long-fat.sh

clean:
	rm -rf bin .deps .bench go.local.mod go.local.sum
