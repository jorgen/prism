# 3rdparty package definitions. Included by CmDepFetch (and CmDepFetchSetup in
# script mode). CmDepFetchPackage auto-declares, per dep: PRISM_<DEP>_{VERSION,URL,SHA256}
# (CACHE STRING overrides, -D wins) and PRISM_USE_SYSTEM_<DEP> (option; ON => consume
# a pre-built copy via find_package instead of fetching). The PRISM_ prefix is derived
# from the project() name (PROJECT_NAME).

CmDepFetchPackage(vio 5338286
    https://github.com/jorgen/vio/archive/5338286248642532fcd62d7018e880ce58d7195f.tar.gz
    SHA256=28f6eab15218b317076d7ad593352c9829b4b73787b80e6eae108458710ade01)

CmDepFetchPackage(structify b8fec28d24
    https://github.com/jorgen/structify/archive/b8fec28d2449640e4c5668a59c736555e50aee81.tar.gz
    SHA256=9aa952d2f93e2762ea4e1537eb5f409a77c933fa1a79cc8d276ec113b800bde8)

CmDepFetchPackage(doctest 2.4.12
    https://github.com/doctest/doctest/archive/v2.4.12.tar.gz
    SHA256=73381c7aa4dee704bd935609668cf41880ea7f19fa0504a200e13b74999c2d70)

CmDepFetchPackage(llhttp 9.4.2
    https://github.com/nodejs/llhttp/archive/refs/tags/release/v9.4.2.tar.gz
    SHA256=ba717a2f99f340a0ee9796aaf2b1acca057e1e37682ffd2bc4def4d3b6bc4005)
