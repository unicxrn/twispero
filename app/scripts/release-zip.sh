#!/usr/bin/env bash
# =============================================================================
# scripts/release-zip.sh - build the zip a release ships, in the layout
# homebrew.page and ProsperoStore expect: the app folder <TITLE_ID>/ at the
# top of the archive, with eboot.bin, sce_sys/ (param.json, icon0.png, the
# pictures), sce_module/libc.prx and the licence texts in assets/, plus
# LICENSE, NOTICE and a short README.txt in the same folder.
#
#   ./scripts/release-zip.sh                    release build, then the zip
#   ./scripts/release-zip.sh --no-build         zip output/app/<TITLE_ID>/ as it is
#   ./scripts/release-zip.sh --source-url URL   the source line in README.txt
#                                               (or set RELEASE_SOURCE_URL)
#
# Output: output/release/<TITLE_ID>.zip (PPSA99474.zip), named after the
# title id as homebrew.page's artifact docs recommend. The version is
# projects/evoplayer/VERSION. The script prints the contents, the size and the
# sha256 the catalog entry needs.
#
# The build is package-app.sh with no dev flags: no --usb-remote, no
# --breadcrumbs. --no-build refuses a folder built with --usb-remote.
# Never deploys anything.
# =============================================================================
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

BUILD=1
SOURCE_URL="${RELEASE_SOURCE_URL:-}"
while (( $# )); do
    case "$1" in
        --no-build)   BUILD=0 ;;
        --source-url) shift; SOURCE_URL="${1:?--source-url needs a URL}" ;;
        -h|--help)    sed -n '3,20p' "$0"; exit 0 ;;
        *) die "unknown option: $1 (try --help)" ;;
    esac
    shift
done

need_cmd python3 sha256sum

EVO="${REPO_ROOT}/projects/evoplayer"
PARAM="${EVO}/sce_sys/param.json"
need_file "${PARAM}" "projects/evoplayer/sce_sys/param.json is missing"

# Name, title id and versions, all from the files that own them.
read -r TITLE_ID CONTENT_VER < <(python3 - "${PARAM}" <<'PY'
import json, sys
p = json.load(open(sys.argv[1], encoding="utf-8"))
print(p["titleId"], p["contentVersion"])
PY
)
APP_NAME="$(python3 -c 'import json,sys; p=json.load(open(sys.argv[1],encoding="utf-8")); l=p["localizedParameters"]; print(l[l["defaultLanguage"]]["titleName"])' "${PARAM}")"
VERSION="$(tr -d '[:space:]' < "${EVO}/VERSION")"
[[ -n "${VERSION}" ]] || die "projects/evoplayer/VERSION is empty"
[[ "${CONTENT_VER}" != "01.000.000" ]] || warn "contentVersion is still 01.000.000; raise it by one for every release, or consoles won't see the update"
log "${APP_NAME} ${VERSION} (${TITLE_ID}, contentVersion ${CONTENT_VER})"

APPDIR="${OUTPUT_DIR}/app/${TITLE_ID}"
REL="${OUTPUT_DIR}/release"
ZIP="${REL}/${TITLE_ID}.zip"
STAGE="${REL}/.stage"

# 1. The release build.
if (( BUILD )); then
    begin "release build (package-app.sh, no dev flags)"
    "${SCRIPTS_DIR}/package-app.sh"
fi

# 2. Check the folder is a complete release build of this version.
begin "checking ${APPDIR#"${REPO_ROOT}/"}"
for f in eboot.bin sce_sys/param.json sce_sys/icon0.png sce_module/libc.prx assets/LICENSE.txt; do
    need_file "${APPDIR}/${f}" "${f} is missing from the app folder. Build it first (drop --no-build)."
done
cmp -s "${PARAM}" "${APPDIR}/sce_sys/param.json" \
    || die "the app folder's param.json differs from sce_sys/param.json. Rebuild (drop --no-build)."
BUILD_ID="$(python3 - "${APPDIR}/eboot.bin" <<'PY'
import re, sys
data = open(sys.argv[1], "rb").read()
if b"/mnt/usb0/evo_cmd" in data:
    sys.exit("usb-remote")
m = re.search(rb"(?:[0-9a-f]{8}|unknown)(?:-dirty)?_\d{4}-\d{4}", data)
print(m.group(0).decode() if m else "none")
PY
)" || die "eboot.bin is a --usb-remote dev build. Rebuild without dev flags (drop --no-build)."
ok "build ${BUILD_ID}"
[[ "${BUILD_ID}" != *-dirty* ]] || warn "built from uncommitted changes (build id ${BUILD_ID}); commit first for a real release"
[[ -n "${SOURCE_URL}" ]] || warn "no --source-url: README.txt says the source isn't published yet. Set it for a public release."

# 3. Stage the folder (hard links, so the 37 MB eboot isn't copied) and add
#    LICENSE, NOTICE and README.txt.
begin "staging ${TITLE_ID}/"
rm -rf -- "${STAGE}"
mkdir -p "${STAGE}"
cp -al "${APPDIR}" "${STAGE}/${TITLE_ID}" 2>/dev/null || cp -a "${APPDIR}" "${STAGE}/${TITLE_ID}"
cp "${REPO_ROOT}/LICENSE" "${STAGE}/${TITLE_ID}/LICENSE"
cp "${EVO}/NOTICE" "${STAGE}/${TITLE_ID}/NOTICE"
python3 - "${SCRIPTS_DIR}/release-readme.txt" "${STAGE}/${TITLE_ID}/README.txt" \
    "${APP_NAME}" "${VERSION}" "${TITLE_ID}" "${SOURCE_URL:-not published yet}" <<'PY'
import sys
src, dst, name, version, title_id, source = sys.argv[1:7]
text = open(src, encoding="utf-8").read()
for key, value in (("@NAME@", name), ("@VERSION@", version),
                   ("@TITLE_ID@", title_id), ("@SOURCE@", source)):
    text = text.replace(key, value)
# Windows' Notepad and most PC tools read CRLF best; the console never reads it.
open(dst, "w", encoding="utf-8", newline="\r\n").write(text)
PY

# 4. Zip it, then drop the staging copy.
begin "zipping $(basename "${ZIP}")"
rm -f -- "${ZIP}"
python3 "${REPO_ROOT}/tools/package_zip.py" "${STAGE}/${TITLE_ID}" "${ZIP}" "${TITLE_ID}" >/dev/null
rm -rf -- "${STAGE}"

python3 - "${ZIP}" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
    for i in z.infolist():
        if not i.is_dir():
            print(f"     {i.file_size:>10}  {i.filename}")
PY
ok "${ZIP#"${REPO_ROOT}/"}  ($(stat -c %s "${ZIP}") bytes)"
echo "     sha256 $(sha256sum "${ZIP}" | cut -d' ' -f1)"
