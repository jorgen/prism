# 3rdparty package definitions.
# Included by CmDepFetch and CmDepFetchDependencies.
# Caller must define CmDepFetchPackage(name version url url_hash) before including this file.

if(NOT PRISM_USE_SYSTEM_VIO)
    CmDepFetchPackage(vio dc66d06 https://github.com/jorgen/vio/archive/dc66d06ff4619a585d10c47d1f315e8976bd824a.tar.gz SHA256=db957dae80aa24a7b81121842c971e7dd169f02ebc927be8ee99c3c0a2f7cdf2)
endif()
if(NOT PRISM_USE_SYSTEM_STRUCTIFY)
    CmDepFetchPackage(structify b8fec28d24 https://github.com/jorgen/structify/archive/b8fec28d2449640e4c5668a59c736555e50aee81.tar.gz SHA256=9aa952d2f93e2762ea4e1537eb5f409a77c933fa1a79cc8d276ec113b800bde8)
endif()
if(NOT PRISM_USE_SYSTEM_DOCTEST)
    CmDepFetchPackage(doctest 2.4.12 https://github.com/doctest/doctest/archive/v2.4.12.tar.gz SHA256=73381c7aa4dee704bd935609668cf41880ea7f19fa0504a200e13b74999c2d70)
endif()
if(NOT PRISM_USE_SYSTEM_LLHTTP)
    CmDepFetchPackage(llhttp 9.4.2 https://github.com/nodejs/llhttp/archive/refs/tags/release/v9.4.2.tar.gz SHA256=ba717a2f99f340a0ee9796aaf2b1acca057e1e37682ffd2bc4def4d3b6bc4005)
endif()
