TEMPLATE = subdirs
CONFIG += ordered

SUBDIRS = \
    test_pathutils \
    test_rangeplanner \
    test_json_roundtrip

test_pathutils.file = test_pathutils.pro
test_rangeplanner.file = test_rangeplanner.pro
test_json_roundtrip.file = test_json_roundtrip.pro

exists(test_range_validation.pro) {
    SUBDIRS += test_range_validation
    test_range_validation.file = test_range_validation.pro
}

exists(test_download_lifecycle.pro) {
    SUBDIRS += test_download_lifecycle
    test_download_lifecycle.file = test_download_lifecycle.pro
}

exists(test_bridge_auth.pro) {
    SUBDIRS += test_bridge_auth
    test_bridge_auth.file = test_bridge_auth.pro
}
