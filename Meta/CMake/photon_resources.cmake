# Resources required by the reusable Photon engine runtime. Keep this separate
# from UI/cmake/ResourceFiles.cmake so headless/embedder builds do not configure
# any Ladybird browser application frontend.
set(photon_engine_resources
    "Base/res/fonts/NotoEmoji.ttf|fonts"
    "Base/res/fonts/SerenitySans-Regular.ttf|fonts"
    "Base/res/themes/Default.ini|themes"
    "Base/res/themes/Dark.ini|themes"
    "Base/res/ladybird/ladybird.css|ladybird"
    "Base/res/ladybird/utils.js|ladybird"
    "Base/res/ladybird/templates/error.html|ladybird/templates"
    "WebCompat/aljazeera.com.json|ladybird/site-compatibility"
    "WebCompat/cnn.com.json|ladybird/site-compatibility"
    "WebCompat/nytimes.com.json|ladybird/site-compatibility"
)

set(photon_engine_resource_outputs)
foreach(resource IN LISTS photon_engine_resources)
    string(REPLACE "|" ";" parts "${resource}")
    list(GET parts 0 source_relative)
    list(GET parts 1 destination_relative)
    get_filename_component(resource_name "${source_relative}" NAME)
    set(source "${LADYBIRD_SOURCE_DIR}/${source_relative}")
    set(destination "${CMAKE_BINARY_DIR}/share/Lagom/${destination_relative}/${resource_name}")
    add_custom_command(
        OUTPUT "${destination}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/share/Lagom/${destination_relative}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${source}" "${destination}"
        DEPENDS "${source}"
        VERBATIM
    )
    list(APPEND photon_engine_resource_outputs "${destination}")
endforeach()

add_custom_target(PhotonEngineResources DEPENDS ${photon_engine_resource_outputs})
add_dependencies(LibPhotonEmbedder PhotonEngineResources)
