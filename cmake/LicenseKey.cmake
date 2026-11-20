# Keep Android and any other native entry points on the same license contract.
function(hsvj_configure_license_key target)
    if(NOT DEFINED HVIDEO_LICENSE_HMAC_KEY OR "${HVIDEO_LICENSE_HMAC_KEY}" STREQUAL "")
        message(FATAL_ERROR "LICENSE_HMAC_KEY is required. Restore the existing issuer key; do not generate a replacement key.")
    endif()
    string(REPLACE "\\" "\\\\" _license_key "${HVIDEO_LICENSE_HMAC_KEY}")
    string(REPLACE "\"" "\\\"" _license_key "${_license_key}")
    target_compile_definitions(${target} PRIVATE HVIDEO_LICENSE_HMAC_KEY="${_license_key}")
endfunction()
