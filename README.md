# Photon Engine

Photon Engine is the web engine used by the Photon browser. It is derived from [Ladybird](https://github.com/LadybirdBrowser/ladybird) and is actively maintained as a downstream repository. Photon-specific changes include engine integration, embedding APIs, rendering and web-platform work, performance fixes, services and tests.

## Relationship to Ladybird

The Git remote named `upstream` should point to `https://github.com/LadybirdBrowser/ladybird.git`. Photon Engine retains Ladybird's BSD-2-Clause license, copyright notices and attribution. This repository does not claim authorship of the inherited engine.

Maintainers regularly merge Ladybird's `master` branch into the current Photon Engine branch:

```sh
git fetch upstream
git merge upstream/master
```

Resolve conflicts in Photon Engine, validate the engine changes, then update the exact `Engine/` submodule commit in `PhotonBrowser/photon`. Do not merge Ladybird directly into the browser repository. Photon Engine is a normal maintained Git repository; it does not use a patch series or source-copy workflow.

## Architecture

The engine repository contains the Ladybird-derived libraries (`LibWeb`, `LibJS`, `LibGfx`, `LibWebView` and supporting libraries), out-of-process services such as `WebContent`, `RequestServer`, `ImageDecoder` and `Compositor`, engine tests, Web Platform Tests and development utilities. Photon browser state and its Qt Quick/QML UI belong in `PhotonBrowser/photon`.

The CMake option `ENABLE_PHOTON_EMBEDDER=ON` builds engine libraries and services while `ENABLE_LADYBIRD_UI=OFF` omits Ladybird's browser application and UI tests. The named `LibPhotonEmbedder` target is currently a boundary scaffold; the stable runtime/view/presentation API remains to be implemented before Photon can navigate through the engine.

## Build

For the integrated developer workflow, clone `PhotonBrowser/photon` with submodules and run `./photon setup`, `./photon build` and `./photon run`. The CLI keeps CMake, vcpkg and application build output under the browser repository's ignored `build/` directory.

Photon Engine keeps upstream build documentation in `Documentation/BuildInstructionsLadybird.md`. For contribution and licensing details, see [`CONTRIBUTING.md`](CONTRIBUTING.md) and [`LICENSE`](LICENSE).
