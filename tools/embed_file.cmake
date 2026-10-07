# Convert a file into a C source exposing `web_ui_data` / `web_ui_size`.
# Usage: cmake -DINPUT=<file> -DOUTPUT=<file.c> -P embed_file.cmake
file(READ "${INPUT}" HEX_CONTENT HEX)
string(LENGTH "${HEX_CONTENT}" HEX_LEN)
math(EXPR BYTE_LEN "${HEX_LEN} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," C_ARRAY "${HEX_CONTENT}")
# Wrap lines so the generated file stays readable for compilers/diff tools.
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){24})" "\\1\n" C_ARRAY "${C_ARRAY}")
get_filename_component(OUT_DIR "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${OUT_DIR}")
file(WRITE "${OUTPUT}" "// Generated from ${INPUT}. Do not edit.\n#include <stddef.h>\n#include <stdint.h>\nconst uint8_t web_ui_data[] = {\n${C_ARRAY}\n};\nconst size_t web_ui_size = ${BYTE_LEN};\n")
