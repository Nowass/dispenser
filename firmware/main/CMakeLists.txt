# Composition root. Dependencies are listed explicitly (no implicit "all components").
idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES dispenser_domain dispenser_platform esp_app_format log)
