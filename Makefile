#   NZB/Usenet downloader payload for PS5.
#
#   Build:
#     export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
#     make ps5_nzb_downloader.elf # the real app -- NNTP download + yEnc decode +
#                                 # PAR2 verify + extraction, queue + web UI;
#                                 # see download.c's top comment
#     make appcheck               # syntax-check every app module, no link/output
#
#   Third-party ports needed in the SDK sysroot (each built once with the
#   pattern in expat.sh / websrv/libmicrohttpd.sh -- configure --host=x86_64
#   against the prospero toolchain, install into $PS5_PAYLOAD_SDK/target):
#     - openssl   (NNTPS)
#     - expat     (NZB XML parsing)
#     - libmicrohttpd (web UI/API)
#
#   yEnc decode (src/yenc, wrapping the vendored rapidyenc library), PAR2
#   verify+repair (src/par2, pure C, no third-party PAR2 library -- see
#   par2.h; the Reed-Solomon math itself lives in src/par2/rs.c, ported
#   from par2cmdline's reference algorithm), and archive extraction
#   (src/extract, wrapping the vendored libarchive) have all been
#   reimplemented since the strip-down mentioned in download.c's top
#   comment.

PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

CFLAGS := -Wall -O2 -g

SSL_FLAGS := $(shell $(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config libssl libcrypto --cflags --libs)

all: ps5_nzb_downloader.elf

# --- modular app: src/log, src/config, src/nntp, src/nzb, ...
# No runnable target yet -- these build into the eboot.elf once src/main.c
# exists (Phase 5). Individual modules are compile-checked via `make appcheck`.
APP_SRCS := src/log/log.c src/config/config.c
APP_SRCS += src/nntp/nntp_conn.c src/nntp/nntp_pool.c
APP_SRCS += src/nzb/nzb_parse.c
APP_SRCS += src/queue/job.c src/queue/queue.c
APP_SRCS += src/vendor/cjson/cJSON.c
APP_SRCS += src/storage/paths.c src/storage/shadowmount.c
APP_SRCS += src/util/crc32.c src/util/notify.c
APP_SRCS += src/yenc/yenc.c
APP_SRCS += src/par2/par2.c src/par2/rs.c
APP_SRCS += src/extract/extract.c src/extract/rar5_crypt.c
APP_SRCS += src/download/download.c
APP_SRCS += src/system/pkg_install.c
APP_SRCS += src/web/app_state.c src/web/asset.c src/web/json_util.c src/web/job_json.c
APP_SRCS += src/web/httpd.c
APP_SRCS += src/web/api_status.c src/web/api_jobs.c src/web/api_config.c src/web/api_logs.c
APP_SRCS += src/web/api_system.c

EXPAT_FLAGS := $(shell $(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config expat --cflags --libs)
MHD_FLAGS   := $(shell $(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config libmicrohttpd --cflags --libs)
# rapidyenc (yEnc decode) has no .pc file in the SDK sysroot, unlike the
# other ports here -- flags spelled out by hand instead of via
# prospero-pkg-config. Confirmed (via `nm -u` on librapidyenc.a) to link
# fine with plain $(CC) -- its only external undefined symbols are
# memset/posix_memalign, both ordinary libc, no C++ runtime needed despite
# being implemented in C++ internally. That matters here specifically
# because an *earlier* C++ PAR2 library attempt in this project needed the
# $(CXX)/`-x c` dance (see BUILD_TOOLCHAIN.md §4b) and was backed out --
# rapidyenc does not have that problem.
RAPIDYENC_FLAGS := -I$(PS5_PAYLOAD_SDK)/target/user/homebrew/include -L$(PS5_PAYLOAD_SDK)/target/user/homebrew/lib -lrapidyenc
# libarchive does have a .pc file (unlike rapidyenc above), pulls in its
# own -lcrypto/-lz/-llzma/-pthread. It's pure C (unlike rapidyenc), so no
# C++ runtime question here at all -- still smoke-linked a trivial
# program against it once to be sure, same as rapidyenc. PAR2 (src/par2)
# is pure C with no third-party lib at all.
ARCHIVE_FLAGS := $(shell $(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config libarchive --cflags --libs)
# No .pc file (same as rapidyenc above) -- these are plain sce_stubs, not a
# pacbrew port. -lSceIpmi is needed alongside -lSceAppInstUtil because
# AppInstUtil's real implementation lives inside libSceIpmi.so on the
# console (see src/system/pkg_install.c and its matching pragma comment(lib)
# -- confirmed against ps5-payload-dev/websrv's PKGInstall homebrew, which
# links the same pair for the same sceAppInstUtilInstallByPackage() call).
APPINSTUTIL_FLAGS := -lSceIpmi -lSceAppInstUtil
APP_FLAGS   := $(SSL_FLAGS) $(EXPAT_FLAGS) $(MHD_FLAGS) $(RAPIDYENC_FLAGS) $(ARCHIVE_FLAGS) $(APPINSTUTIL_FLAGS)

appcheck:
	$(CC) $(CFLAGS) -Isrc $(APP_FLAGS) -fsyntax-only $(APP_SRCS) src/main.c

# --- web UI assets, compiled in as byte arrays (see src/web/asset.c) ---
PYTHON ?= python3
ASSETS   := $(wildcard src/web/assets/*)
GEN_SRCS := $(patsubst src/web/assets/%,gen/%, $(ASSETS:=.c))

gen:
	mkdir -p gen

gen/%.c: src/web/assets/% gen
	$(PYTHON) gen-asset-module.py --path $* $< > $@

# --- the real app ---
# Plain $(CC), not $(CXX): rapidyenc (see RAPIDYENC_FLAGS above) links
# fine without the C++ runtime despite being C++ internally, so none of
# the $(CXX)/`-x c` dance an actual C++ static lib would need applies here.
ps5_nzb_downloader.elf: $(APP_SRCS) src/main.c $(GEN_SRCS)
	$(CC) $(CFLAGS) -Isrc -o $@ $^ $(APP_FLAGS)

clean:
	rm -rf ps5_nzb_downloader.elf gen

# Streams the ELF over the socket. Reads /data/nzb-downloader/nzb.conf (or
# argv[1] -- but the streamed-ELF deploy path gives no argv), serves the
# web UI on port 4202 (see main.c's HTTP_PORT).
test-nzb: ps5_nzb_downloader.elf
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^

.PHONY: all clean test-nzb appcheck
