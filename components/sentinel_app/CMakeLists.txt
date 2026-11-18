# Sentinel app information (include/sentinel_app.h). Header only; the linker flag keeps the
# structure that SENTINEL_APP_INFO() defines, which nothing in the firmware refers to.
idf_component_register(INCLUDE_DIRS include)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u sentinel_app_info")
