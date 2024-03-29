.PHONY: all clean format debug release duckdb_debug duckdb_release pull update

all: release

MKFILE_PATH := $(abspath $(lastword $(MAKEFILE_LIST)))
PROJ_DIR := $(dir $(MKFILE_PATH))

ifeq (${STATIC_LIBCPP}, 1)
	STATIC_LIBCPP=-DSTATIC_LIBCPP=TRUE
endif

CMAKE_OSX_ARCHITECTURES_FLAG=
ifneq (${CMAKE_OSX_ARCHITECTURES},)
	CMAKE_OSX_ARCHITECTURES_FLAG=-DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
endif

Rust_CARGO_TARGET_FLAG=
ifneq ($(Rust_CARGO_TARGET),)
	Rust_CARGO_TARGET_FLAG=-DRust_CARGO_TARGET=${Rust_CARGO_TARGET}
endif

ifeq ($(GEN),ninja)
	GENERATOR=-G "Ninja"
	FORCE_COLOR=-DFORCE_COLORED_OUTPUT=1
endif

#### Configuration for this extension
EXTENSION_NAME=PSQL
EXTENSION_FLAGS=\
-DDUCKDB_EXTENSION_NAMES="psql" \
-DDUCKDB_EXTENSION_${EXTENSION_NAME}_PATH="$(PROJ_DIR)" \
-DDUCKDB_EXTENSION_${EXTENSION_NAME}_LOAD_TESTS=1 \
-DDUCKDB_EXTENSION_${EXTENSION_NAME}_INCLUDE_PATH="$(PROJ_DIR)src/include" \
-DDUCKDB_EXTENSION_${EXTENSION_NAME}_TEST_PATH="$(PROJ_DIR)test/sql"

BUILD_FLAGS=${STATIC_LIBCPP} $(EXTENSION_FLAGS) ${CMAKE_OSX_ARCHITECTURES_FLAG} ${Rust_CARGO_TARGET_FLAG}

ifeq (${EXTENSION_STATIC_BUILD}, 1)
	BUILD_FLAGS:=${BUILD_FLAGS} -DEXTENSION_STATIC_BUILD=1
endif

pull:
	git submodule init
	git submodule update --recursive

clean:
	rm -rf build
	rm -rf testext
	cd duckdb && make clean

#### Main build
# For regular CLI build, we link the quack extension directly into the DuckDB executable
CLIENT_FLAGS=-DDUCKDB_EXTENSION_${EXTENSION_NAME}_SHOULD_LINK=1

debug:
	mkdir -p  build/debug && \
	cmake $(GENERATOR) $(BUILD_FLAGS) $(CLIENT_FLAGS) -DCMAKE_BUILD_TYPE=Debug -S ./duckdb/ -B build/debug && \
	cmake --build build/debug --config Debug

release:
	mkdir -p build/release && \
	cmake $(GENERATOR) $(BUILD_FLAGS)  $(CLIENT_FLAGS)  -DCMAKE_BUILD_TYPE=Release -S ./duckdb/ -B build/release && \
	cmake --build build/release --config Release

# Main tests
test: test_release

test_release: release
	./build/release/test/unittest --test-dir . "[sql]"

test_debug: debug
	./build/debug/test/unittest --test-dir . "[sql]"

format:
	find src/ -iname *.hpp -o -iname *.cpp | xargs clang-format --sort-includes=0 -style=file -i
	cmake-format -i CMakeLists.txt

update:
	git submodule update --remote --merge

VCPKG_EMSDK_FLAGS=-DVCPKG_CHAINLOAD_TOOLCHAIN_FILE=$(EMSDK)/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake
WASM_COMPILE_TIME_COMMON_FLAGS=-DWASM_LOADABLE_EXTENSIONS=1 -DBUILD_EXTENSIONS_ONLY=1 -DSKIP_EXTENSIONS="parquet;json" $(VCPKG_EMSDK_FLAGS)
WASM_CXX_MVP_FLAGS=
WASM_CXX_EH_FLAGS=$(WASM_CXX_MVP_FLAGS) -fwasm-exceptions -DWEBDB_FAST_EXCEPTIONS=1
WASM_CXX_THREADS_FLAGS=$(WASM_COMPILE_TIME_EH_FLAGS) -DWITH_WASM_THREADS=1 -DWITH_WASM_SIMD=1 -DWITH_WASM_BULK_MEMORY=1
WASM_LINK_TIME_FLAGS=../../third_party/re2/libduckdb_re2.a -sSIDE_MODULE=2 -sEXPORTED_FUNCTIONS="_psql_init,_psql_version"
EXT_NAME=psql

wasm_mvp:
	mkdir -p build/wasm_mvp
	emcmake cmake $(GENERATOR) $(EXTENSION_FLAGS) $(WASM_COMPILE_TIME_COMMON_FLAGS) -Bbuild/wasm_mvp -DCMAKE_CXX_FLAGS="$(WASM_CXX_MVP_FLAGS) -DDUCKDB_CUSTOM_PLATFORM=wasm_mvp" -S duckdb
	emmake make -j8 -Cbuild/wasm_mvp
	cd build/wasm_mvp/extension/${EXT_NAME} && emcc $f -o ../../${EXT_NAME}.duckdb_extension.wasm -O3 ${EXT_NAME}.duckdb_extension $(WASM_LINK_TIME_FLAGS)

wasm_eh:
	mkdir -p build/wasm_eh
	emcmake cmake $(GENERATOR) $(EXTENSION_FLAGS) $(WASM_COMPILE_TIME_COMMON_FLAGS) -Bbuild/wasm_eh -DCMAKE_CXX_FLAGS="$(WASM_CXX_EH_FLAGS) -DDUCKDB_CUSTOM_PLATFORM=wasm_eh" -S duckdb
	emmake make -j8 -Cbuild/wasm_eh
	cd build/wasm_eh/extension/${EXT_NAME} && emcc $f -o ../../${EXT_NAME}.duckdb_extension.wasm -O3 ${EXT_NAME}.duckdb_extension $(WASM_LINK_TIME_FLAGS)

wasm_threads:
	mkdir -p ./build/wasm_threads
	emcmake cmake $(GENERATOR) $(EXTENSION_FLAGS) $(WASM_COMPILE_TIME_COMMON_FLAGS) -Bbuild/wasm_threads -DCMAKE_CXX_FLAGS="$(WASM_CXX_THREADS_FLAGS) -DDUCKDB_CUSTOM_PLATFORM=wasm_threads" -S duckdb
	emmake make -j8 -Cbuild/wasm_threads
	cd build/wasm_threads/extension/${EXT_NAME} && emcc $f -o ../../${EXT_NAME}.duckdb_extension.wasm -O3 ${EXT_NAME}.duckdb_extension $(WASM_LINK_TIME_FLAGS)
