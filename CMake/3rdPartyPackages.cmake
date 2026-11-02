# 3rdparty package definitions. Included by CmDepFetch (and by `cmdep` in script
# mode). CmDepFetchPackage/CmDepFetchGit auto-declare, per dep: PRISM_<DEP>_* (CACHE
# overrides, -D wins) and PRISM_USE_SYSTEM_<DEP> (option; ON => consume a pre-built
# copy via find_package instead of fetching). The PRISM_ prefix is derived from the
# project() name. Build against a local checkout with -DCMDEP_SOURCE_DIR_VIO=<dir>.
# Run `cmdep lock -DPRISM_BUILD_TESTS=ON` after editing this file.

CmDepFetchGit(vio 6dde1b6
    https://github.com/jorgen/vio.git
    6dde1b6d3b0cd1092bfaccf4a201f0d723aa237a
    SHA256=0aed5201b64fa1aaf25287f105c13a4f1aac5307b12ffc4d6761d3aef8a3fcbb)

# The same pin as vio's, so the build has one structify whichever declares it first.
CmDepFetchPackage(structify b114582
    https://github.com/jorgen/structify/archive/b1145826ab4a06cbe32ae0c373a3161f74c48ac8.tar.gz
    SHA256=49f3af94194bd37aa76d99c1d3e351eb237b838ae75d21fecaf6ac4a806c7606)

if (PRISM_BUILD_TESTS)
    CmDepFetchPackage(doctest 2.4.12
        https://github.com/doctest/doctest/archive/v2.4.12.tar.gz
        SHA256=73381c7aa4dee704bd935609668cf41880ea7f19fa0504a200e13b74999c2d70)
endif ()

CmDepFetchPackage(llhttp 9.4.2
    https://github.com/nodejs/llhttp/archive/refs/tags/release/v9.4.2.tar.gz
    SHA256=ba717a2f99f340a0ee9796aaf2b1acca057e1e37682ffd2bc4def4d3b6bc4005)
