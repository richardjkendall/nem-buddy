# Wraps the host-tested core/ library as an ESP-IDF component.
# Sources stay the single source of truth under core/src.
set(CORE "${CMAKE_CURRENT_LIST_DIR}/../../../core")
idf_component_register(
    SRCS
        "${CORE}/src/regions.c"
        "${CORE}/src/config.c"
        "${CORE}/src/timeutil.c"
        "${CORE}/src/aemo_client.c"
        "${CORE}/src/history.c"
        "${CORE}/src/fuel.c"
        "${CORE}/src/oe_client.c"
        "${CORE}/src/alerts.c"
    INCLUDE_DIRS "${CORE}/include"
    REQUIRES json)
