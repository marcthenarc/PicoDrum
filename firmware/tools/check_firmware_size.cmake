# The firmware starts at 0x10000000, the sample library at SAMPLE_LIB_FLASH_OFFSET.
# If the firmware grows into that region, flashing the library corrupts it (or the
# other way round) and the symptom on the hardware is unreadable: wrong samples,
# random crashes. Much better to stop right here.
file(SIZE "${BIN}" FW_SIZE)

if(FW_SIZE GREATER_EQUAL LIMIT)
    message(FATAL_ERROR
        "The firmware takes ${FW_SIZE} bytes and runs into the sample library "
        "region, which starts at ${LIMIT}. Move SAMPLE_LIB_FLASH_OFFSET higher "
        "(--flash-offset in tools/convert_wav.py) and reflash the library.")
endif()

math(EXPR FW_PCT "${FW_SIZE} * 100 / ${LIMIT}")
message(STATUS "Firmware ${FW_SIZE} bytes, ${FW_PCT}% of the space before the sample library")
