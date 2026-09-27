# Photon-owned public boundary. Runtime and view operations are introduced as
# they become usable by the Qt Quick presentation integration.
add_library(LibPhotonEmbedder INTERFACE)
add_library(Photon::Embedder ALIAS LibPhotonEmbedder)
target_include_directories(LibPhotonEmbedder INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(LibPhotonEmbedder INTERFACE cxx_std_20)
