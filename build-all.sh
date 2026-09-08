#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

MAIN_DIR="${ROOT_DIR}/dvplogger"
EXT_DIR="${ROOT_DIR}/dvplogger_ext"
OUTPUT_DIR="${ROOT_DIR}/binaries"

die()
{
    echo "ERROR: $*" >&2
    exit 1
}

copy_file()
{
    local src="$1"
    local dst="$2"
    local tmp="${dst}.tmp"

    [[ -f "${src}" ]] || die "File not found: ${src}"

    mkdir -p "$(dirname "${dst}")"
    install -m 0644 "${src}" "${tmp}"
    mv -f "${tmp}" "${dst}"

    echo "  ${src}"
    echo "    -> ${dst}"
}

generate_crc32_manifest()
{
    local dest_dir="$1"
    local manifest="${dest_dir}/CRC32SUMS.txt"

    echo
    echo "--- Generating CRC32 manifest ---"

    python3         "${ROOT_DIR}/make_crc32_manifest.py"         "${dest_dir}"         "${manifest}"
}

copy_ext_binaries()
{
    local build_dir="$1"
    local dest_dir="$2"

    copy_file \
        "${build_dir}/jk1dvplog_ext.bin" \
        "${dest_dir}/app0.bin"

    copy_file \
        "${build_dir}/bootloader/bootloader.bin" \
        "${dest_dir}/bootload.bin"

    copy_file \
        "${build_dir}/partition_table/partition-table.bin" \
        "${dest_dir}/partitio.bin"

    copy_file \
        "${build_dir}/spiffs.bin" \
        "${dest_dir}/spiffs.bin"
}

build_set()
{
    local hwver="$1"
    local model_dir="$2"

    local ext_build="${ROOT_DIR}/build-ext-hw${hwver}"
    local main_build="${ROOT_DIR}/build-main-hw${hwver}"

    local dest_dir="${OUTPUT_DIR}/${model_dir}"
    local subcpu_dir="${dest_dir}/subcpu"

    echo
    echo "============================================================"
    echo " Building HW${hwver} / ${model_dir}"
    echo "============================================================"

    #
    # 1. サブCPUを先にビルド
    #
    echo
    echo "--- Building dvplogger_ext HW${hwver} ---"

    idf.py \
        -C "${EXT_DIR}" \
        -B "${ext_build}" \
        -DJK1DVPLOG_HWVER="${hwver}" \
        build

    echo
    echo "--- Generating binaries.c for HW${hwver} ---"

    rm -f "${ext_build}/binaries.c"

    cmake \
	-DINPUT_DIR="${ext_build}" \
	-DOUTPUT_FILE="${ext_build}/binaries.c" \
	-P "${EXT_DIR}/main/bin2array_runner.cmake"

    [[ -f "${ext_build}/binaries.c" ]] ||
	die "binaries.c was not generated: ${ext_build}/binaries.c"

    grep -q "jk1dvplog_ext_bin\\[\\]" "${ext_build}/binaries.c" ||
	die "jk1dvplog_ext.bin was not included in binaries.c"    
    #
    # 2. サブCPUの配布用バイナリをコピー
    #
    echo
    echo "--- Copying sub-CPU binaries ---"

    copy_ext_binaries \
        "${ext_build}" \
        "${subcpu_dir}"

    #
    # 3. extビルドで生成されたbinaries.cをメイン側へコピー
    #
    echo
    echo "--- Installing binaries.c into main firmware ---"

    copy_file \
        "${ext_build}/binaries.c" \
        "${MAIN_DIR}/main/binaries.c"

    #
    # 4. メインCPUをビルド
    #
    echo
    echo "--- Building dvplogger HW${hwver} ---"

    idf.py \
        -C "${MAIN_DIR}" \
        -B "${main_build}" \
        -DJK1DVPLOG_HWVER="${hwver}" \
        build

    #
    # 5. メインCPUの.binをコピー
    #
    echo
    echo "--- Copying main firmware ---"

    copy_file \
        "${main_build}/dvplogger-hw${hwver}.bin" \
        "${dest_dir}/dvplogger.bin"

    copy_file \
        "${main_build}/bootloader/bootloader.bin" \
        "${dest_dir}/bootloader.bin"

    copy_file \
        "${main_build}/partition_table/partition-table.bin" \
        "${dest_dir}/partition-table.bin"
    
    echo
    echo "HW${hwver} / ${model_dir} completed."
}

[[ -d "${MAIN_DIR}" ]] ||
    die "Directory not found: ${MAIN_DIR}"

[[ -d "${EXT_DIR}" ]] ||
    die "Directory not found: ${EXT_DIR}"

build_set 1 mini
build_set 3 Wide

echo
echo "============================================================"
echo " All builds completed successfully"
echo "============================================================"
echo
echo "HW1 / mini:"
echo "  binaries/mini/dvplogger.bin"
echo "  binaries/mini/bootloader.bin"
echo "  binaries/mini/partition-table.bin"
echo "  binaries/mini/subcpu/app0.bin"
echo "  binaries/mini/subcpu/bootload.bin"
echo "  binaries/mini/subcpu/partitio.bin"
echo "  binaries/mini/subcpu/spiffs.bin"
echo "  binaries/mini/CRC32SUMS.txt"
echo
echo "HW3 / Wide:"
echo "  binaries/Wide/dvplogger.bin"
echo "  binaries/Wide/bootloader.bin"
echo "  binaries/Wide/partition-table.bin"
echo "  binaries/Wide/subcpu/app0.bin"
echo "  binaries/Wide/subcpu/bootload.bin"
echo "  binaries/Wide/subcpu/partitio.bin"
echo "  binaries/Wide/subcpu/spiffs.bin"
echo "  binaries/Wide/CRC32SUMS.txt"
