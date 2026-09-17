find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${EXTRACTOR}" "${SOURCE}" "${OUTPUT}"
    COMMAND_ERROR_IS_FATAL ANY)
