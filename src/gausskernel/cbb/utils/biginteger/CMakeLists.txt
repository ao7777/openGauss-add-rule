#This is the main CMAKE for build bin.
AUX_SOURCE_DIRECTORY(${CMAKE_CURRENT_SOURCE_DIR} TGT_biginteger_SRC)

set(TGT_biginteger_INC 
    ${PROJECT_SRC_DIR}/include/libcomm
    ${PROJECT_SRC_DIR}/include
    ${PROJECT_SRC_DIR}/lib/gstrace
    ${LIBCGROUP_INCLUDE_PATH}
    ${ZLIB_INCLUDE_PATH}
)

set(biginteger_DEF_OPTIONS ${MACRO_OPTIONS})
set(biginteger_COMPILE_OPTIONS ${OPTIMIZE_OPTIONS} ${OS_OPTIONS} ${PROTECT_OPTIONS} ${WARNING_OPTIONS} ${BIN_SECURE_OPTIONS} ${CHECK_OPTIONS})
set(biginteger_LINK_OPTIONS ${BIN_LINK_OPTIONS})
add_static_objtarget(gausskernel_cbb_utils_biginteger TGT_biginteger_SRC TGT_biginteger_INC "${biginteger_DEF_OPTIONS}" "${biginteger_COMPILE_OPTIONS}" "${biginteger_LINK_OPTIONS}")
