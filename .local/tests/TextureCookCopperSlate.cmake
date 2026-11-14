# Cooks the CopperSlate demo set for TextureCompressionTests.
file(MAKE_DIRECTORY "${OUT}")
foreach(map BaseColor Height Normal)
    execute_process(COMMAND "${COOK}" "${SRC}/CopperSlate_${map}.png"
        "${OUT}/CopperSlate_${map}.ktx2" RESULT_VARIABLE result)
    if(result)
        message(FATAL_ERROR "TextureCook failed for ${map}")
    endif()
endforeach()
execute_process(COMMAND "${COOK}" --toksvig "${SRC}/CopperSlate_Normal.png"
    "${SRC}/CopperSlate_MetallicRoughness.png"
    "${OUT}/CopperSlate_MetallicRoughness.ktx2" RESULT_VARIABLE result)
if(result)
    message(FATAL_ERROR "TextureCook failed for MetallicRoughness")
endif()
