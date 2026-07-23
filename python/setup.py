#
#  Copyright (c) 2020-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

import os
import re
import sys
from setuptools import setup, Extension
import shutil
import subprocess

current_dir = os.path.dirname(os.path.realpath(__file__))
repo_root = os.path.realpath(os.path.join(current_dir, ".."))

def get_project_version():
    # Metadata lives in pyproject.toml; osdp_config.h needs the version too, so
    # read it back rather than keep a second copy in sync. tomllib is 3.11+ and
    # the interpreter running the build may be older.
    with open(os.path.join(current_dir, "pyproject.toml")) as f:
        match = re.search(r'^version\s*=\s*"([^"]+)"', f.read(), re.MULTILINE)
    if not match:
        raise RuntimeError("Unable to read version from pyproject.toml")
    return match.group(1)

project_name = "libosdp"
project_version = get_project_version()

def add_prefix_to_path(src_list, path, check_files=True):
    paths = [ os.path.join(path, src) for src in src_list ]
    for path in paths:
        if check_files:
            if not os.path.exists(path):
                raise RuntimeError(f"Path '{path}' does not exist")
    return paths

def exec_cmd(cmd, cwd=None):
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=cwd)
    return r.returncode, r.stdout.strip()

def get_git_info():
    d = {
        "branch": "None",
        "tag": "",
        "diff": "",
        "rev": "",
        "root": repo_root,
    }

    rc, _ = exec_cmd(["git", "rev-parse", "--is-inside-work-tree"], cwd=repo_root)
    if rc != 0:
        return d

    rc, root = exec_cmd(["git", "rev-parse", "--show-toplevel"], cwd=repo_root)
    if rc == 0 and root:
        d["root"] = root

    rc, branch = exec_cmd(["git", "symbolic-ref", "--short", "-q", "HEAD"], cwd=repo_root)
    if rc == 0 and branch:
        d["branch"] = branch
    else:
        d["branch"] = "detached"

    rc, rev = exec_cmd(["git", "describe", "--tags", "--long", "--always", "--abbrev=7"], cwd=repo_root)
    if rc == 0:
        d["rev"] = rev

    rc, tag = exec_cmd(["git", "describe", "--exact-match", "--tags", "HEAD"], cwd=repo_root)
    if rc == 0 and tag:
        d["tag"] = tag

    rc, status = exec_cmd(["git", "status", "--porcelain", "--untracked-files=normal"], cwd=repo_root)
    if rc == 0 and status:
        d["diff"] = "+"
        if d["tag"]:
            d["tag"] = d["tag"] + "+"

    return d

def compose_version_str(pyproject_version, git):
    # Human-visible version, mirroring cmake/GitInfo.cmake. A released version
    # (X.Y.Z or the X.Y.Z.postN PyPI re-release form) is reported verbatim; a
    # prepared cycle (PEP 440 X.Y.Z.devN in pyproject.toml) is decorated with the
    # pre-release marker plus git position so a source build never looks released.
    match = re.fullmatch(r"(\d+\.\d+\.\d+)\.dev\d+", pyproject_version)
    if not match:
        return pyproject_version
    version = f"{match.group(1)}-dev"
    rev = git.get("rev", "")
    described = re.search(r"-(\d+)-g([0-9a-f]+)$", rev)
    if described:
        version += f".{described.group(1)}+g{described.group(2)}"
    elif rev:
        version += f"+g{rev}"
    return version

def configure_file(file, replacements):
    with open(file) as f:
        contents = f.read()
    pat = re.compile(r"@(\w+)@")
    for match in pat.findall(contents):
        if match in replacements:
            contents = contents.replace(f"@{match}@", replacements[match])
    with open(file, "w") as f:
        f.write(contents)

def try_vendor_sources(src_dir, src_files, vendor_dir):
    test_file = os.path.join(src_dir, src_files[0])
    if not os.path.exists(test_file):
        return
    print("Vendoring sources...")

    ## copy source tree into ./vendor

    shutil.rmtree(vendor_dir, ignore_errors=True)
    for file in src_files:
        src = os.path.join(src_dir, file)
        dest = os.path.join(vendor_dir, file)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        shutil.copyfile(src, dest)

    ## generate build headers into ./vendor

    git = get_git_info()
    shutil.move("vendor/src/osdp_config.h.in", "vendor/src/osdp_config.h")
    configure_file("vendor/src/osdp_config.h", {
        "PROJECT_VERSION": project_version,
        "LIBOSDP_VERSION_STR": compose_version_str(project_version, git),
        "PROJECT_NAME": project_name,
        "GIT_BRANCH": git["branch"],
        "GIT_REV": git["rev"],
        "GIT_TAG": git["tag"],
        "GIT_DIFF": git["diff"],
        "REPO_ROOT": git["root"],
    })

utils_sources = [
    "utils/src/list.c",
    "utils/src/queue.c",
    "utils/src/utils.c",
    "utils/src/logger.c",
    "utils/src/disjoint_set.c",
    "utils/src/crc16.c",
]

utils_includes = [
    "utils/include/utils/assert.h",
    "utils/include/utils/list.h",
    "utils/include/utils/queue.h",
    "utils/include/utils/slab.h",
    "utils/include/utils/utils.h",
    "utils/include/utils/logger.h",
    "utils/include/utils/disjoint_set.h",
    "utils/include/utils/crc16.h",
]

lib_sources = [
    "src/osdp_common.c",
    "src/osdp_phy.c",
    "src/osdp_sc.c",
    "src/osdp_file.c",
    "src/osdp_multipart.c",
    "src/osdp_piv.c",
    "src/osdp_bio.c",
    "src/osdp_pd.c",
    "src/osdp_cp.c",
    "src/osdp_metrics.c",
    "src/crypto/tinyaes_src.c",
    "src/crypto/tinyaes.c",
]

lib_includes = [
    "include/osdp.h",
    "include/osdp_export.h",
    "src/osdp_common.h",
    "src/osdp_file.h",
    "src/osdp_multipart.h",
    "src/osdp_piv.h",
    "src/osdp_bio.h",
    "src/osdp_metrics.h",
    "src/osdp_trs.h",
    "src/crypto/tinyaes_src.h",
]

osdp_sys_sources = [
    "python/osdp_sys/module.c",
    "python/osdp_sys/base.c",
    "python/osdp_sys/cp.c",
    "python/osdp_sys/pd.c",
    "python/osdp_sys/data.c",
    "python/osdp_sys/utils.c",
]

osdp_sys_include = [
    "python/osdp_sys/module.h",
]

other_files = [
    "src/osdp_config.h.in",

    # Optional when PACKET_TRACE is enabled
    "src/osdp_diag.c",
    "src/osdp_diag.h",
    "utils/include/utils/pcap_gen.h",
    "utils/src/pcap_gen.c",

    # Optional when TRS is enabled
    "src/osdp_trs.c",
]

# LICENSE lives at the repo root; vendor a copy so wheel/sdist builds
# (which run from python/) can ship it as the PEP 639 License-File.
license_file = "LICENSE"

definitions = [
    "OPT_OSDP_PACKET_TRACE",
    # osdp_sys exposes no TRS bindings yet; keep TRS out of the extension.
    # "OPT_BUILD_OSDP_TRS",
    # "OPT_OSDP_DATA_TRACE",
    # "OPT_OSDP_SKIP_MARK_BYTE",
]

source_files = utils_sources + lib_sources + osdp_sys_sources

try_vendor_sources(
    repo_root,
    source_files + utils_includes + lib_includes + osdp_sys_include + other_files + [ license_file ],
    "vendor"
)

if ("OPT_OSDP_PACKET_TRACE" in definitions or
    "OPT_OSDP_DATA_TRACE" in definitions):
    source_files += [
        "src/osdp_diag.c",
        "utils/src/pcap_gen.c",
    ]

if "OPT_BUILD_OSDP_TRS" in definitions:
    source_files += [
        "src/osdp_trs.c",
    ]

source_files = add_prefix_to_path(source_files, "vendor")

include_dirs = [
    "vendor/utils/include",
    "vendor/include",
    "vendor/src",
    "vendor/python/osdp_sys",
    "vendor/src/crypto"
]

compile_args = (
    [ "-I" + path for path in include_dirs ] +
    [ "-D" + define + "=1" for define in definitions ]
)
link_args = []

# The timeouts in src/osdp_config.h.in are #ifndef-guarded so that a build can
# dial them down; tests/unit-tests does the same via -DOSDP_PD_ONLINE_TOUT_MS.
# Without this the pytest suite has to wait out the 8s production timeout to
# observe a PD going offline.
for tunable in ("OSDP_PD_ONLINE_TOUT_MS", "OSDP_RESP_TOUT_MS"):
    if tunable in os.environ:
        compile_args.append(f"-D{tunable}={int(os.environ[tunable])}")

# scripts/run_coverage.sh sets this to collect gcov data. The extension links
# src/ and utils/ straight in, so one instrumented build covers the bindings
# and the library itself. Counters are bumped from the refresh thread while the
# main thread runs test code, hence -fprofile-update=atomic.
if os.environ.get("OSDP_COVERAGE") == "1":
    compile_args += [
        "--coverage", "-O0", "-g",
        "-fprofile-abs-path", "-fprofile-update=atomic",
    ]
    link_args.append("--coverage")

# PyPI Windows wheels are built with MSVC via cibuildwheel. Its legacy
# preprocessor breaks the IS_ENABLED() macro in utils/include/utils/utils.h;
# /Zc:preprocessor switches to the C99/C11-conformant preprocessor.
if sys.platform == "win32":
    compile_args.append("/Zc:preprocessor")

# Everything else (name, version, classifiers, ...) lives in pyproject.toml.
# The extension is built into the osdp package as a private submodule so that
# its type stub and the py.typed marker ship beside it.
setup(
    ext_modules  = [
        Extension(
            name               = "osdp._sys",
            sources            = source_files,
            extra_compile_args = compile_args,
            extra_link_args    = link_args,
            define_macros      = [],
            language           = "C",
        )
    ],
)
