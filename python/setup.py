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

project_name = "libosdp"
project_version = "3.2.0"
current_dir = os.path.dirname(os.path.realpath(__file__))
repo_root = os.path.realpath(os.path.join(current_dir, ".."))

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
    "src/osdp_pd.c",
    "src/osdp_cp.c",
    "src/osdp_metrics.c",
    "src/osdp_trs.c",
    "src/crypto/tinyaes_src.c",
    "src/crypto/tinyaes.c",
]

lib_includes = [
    "include/osdp.h",
    "include/osdp_export.h",
    "src/osdp_common.h",
    "src/osdp_file.h",
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
]

# LICENSE lives at the repo root; vendor a copy so wheel/sdist builds
# (which run from python/) can ship it as the PEP 639 License-File.
license_file = "LICENSE"

definitions = [
    "OPT_OSDP_PACKET_TRACE",
    "OPT_BUILD_OSDP_TRS",
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

# PyPI Windows wheels are built with MSVC via cibuildwheel. Its legacy
# preprocessor breaks the IS_ENABLED() macro in utils/include/utils/utils.h;
# /Zc:preprocessor switches to the C99/C11-conformant preprocessor.
if sys.platform == "win32":
    compile_args.append("/Zc:preprocessor")

if os.path.exists("README.md"):
    with open("README.md", "r") as f:
        long_description = f.read()
else:
    long_description = ""

setup(
    name         = project_name,
    version      = project_version,
    author       = "Siddharth Chandrasekaran",
    author_email = "sidcha.dev@gmail.com",
    description  = "Library implementation of IEC 60839-11-5 OSDP (Open Supervised Device Protocol)",
    url          = "https://github.com/osdp-dev/libosdp",
    project_urls = {
        "Documentation": "https://doc.osdp.dev/",
        "Python Docs":   "https://doc.osdp.dev/python/getting-started",
        "Source":        "https://github.com/osdp-dev/libosdp",
        "Changelog":     "https://doc.osdp.dev/changelog",
    },
    ext_modules  = [
        Extension(
            name               = "osdp_sys",
            sources            = source_files,
            extra_compile_args = compile_args,
            extra_link_args    = [],
            define_macros      = [],
            language           = "C",
        )
    ],
    packages     = [ "osdp" ],
    license_expression = "Apache-2.0",
    classifiers  = [
        "Programming Language :: Python :: 3",
        "Operating System :: OS Independent",
    ],
    long_description              = long_description,
    long_description_content_type = "text/markdown",
    python_requires               = ">=3.9",
    license_files                 = [ os.path.join("vendor", license_file) ],
)
