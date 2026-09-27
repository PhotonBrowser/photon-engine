set(SOURCES PhotonEmbedder.cpp)
ladybird_lib(LibPhotonEmbedder photonembedder EXPLICIT_SYMBOL_EXPORT)
add_library(Photon::Embedder ALIAS LibPhotonEmbedder)
target_include_directories(LibPhotonEmbedder PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/Libraries>
)
target_link_libraries(LibPhotonEmbedder PRIVATE LibWebView LibWeb LibIPC LibGfx LibCore LibURL LibCompositing LibMain)
