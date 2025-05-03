FILE(GLOB_RECURSE app_sources *.*)

# Explicitly adding mbedtls for GCMEncryption, not sure why its needed yet, as GCMEncryption is already adding this.
idf_component_register(
    SRCS ${app_sources}
    REQUIRES mbedtls
)