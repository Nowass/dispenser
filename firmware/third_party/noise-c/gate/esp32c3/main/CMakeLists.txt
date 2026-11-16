idf_component_register(SRCS "gate_main.c" "../../nprobe.c" INCLUDE_DIRS "."
                       REQUIRES esp_hw_support esp_system freertos heap)
target_compile_definitions(${COMPONENT_LIB} PRIVATE NPROBE_NO_MAIN=1)
