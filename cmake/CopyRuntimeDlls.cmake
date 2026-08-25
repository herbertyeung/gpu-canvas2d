if(NOT DEFINED SOURCE_DIR OR NOT IS_DIRECTORY "${SOURCE_DIR}")
  message(FATAL_ERROR "SOURCE_DIR must identify a runtime DLL directory")
endif()

if(NOT DEFINED DESTINATION_DIR)
  message(FATAL_ERROR "DESTINATION_DIR is required")
endif()

file(MAKE_DIRECTORY "${DESTINATION_DIR}")
file(GLOB RUNTIME_DLLS "${SOURCE_DIR}/*.dll")
foreach(RUNTIME_DLL IN LISTS RUNTIME_DLLS)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${RUNTIME_DLL}" "${DESTINATION_DIR}"
    COMMAND_ERROR_IS_FATAL ANY
  )
endforeach()