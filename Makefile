.PHONY: fetch configure build validate clean distclean

fetch:
	sh ./scripts/fetch-lwip.sh

configure: fetch
	cmake -S . -B .build -DCMAKE_BUILD_TYPE=Release

build: configure
	cmake --build .build --parallel

validate: build
	sh ./scripts/validate-p0.sh

clean:
	rm -rf .build

distclean: clean
	rm -rf .deps
