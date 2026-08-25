# Chay moi lan build — ghi gio compile that vao build_stamp.c
if(NOT STAMP_C)
    message(FATAL_ERROR "STAMP_C not set")
endif()
string(TIMESTAMP TS "%b %d %Y %H:%M:%S")
file(WRITE "${STAMP_C}" "/* generated — do not edit */\nconst char g_app_build_stamp[] = \"${TS}\";\n")
