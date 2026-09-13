.PHONY: fetch configure build clean distclean

fetch:
	sh ./scripts/fetch-lwip.sh

configure: fetch
	cmake -S . -B .build -DCMAKE_BUILD_TYPE=Release

build: configure
	cmake --build .build --parallel

clean:
	rm -rf .build

distclean: clean
	rm -rf .deps
