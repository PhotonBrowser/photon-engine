/*
 * Copyright (c) 2026, PhotonBrowser contributors
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <LibPhotonEmbedder/PhotonEmbedder.h>

namespace WebView {
class PhotonHeadlessWebView;
}

namespace Photon {

struct View::Impl {
    ~Impl();

    bool has_completed_document_load() const;
    uint64_t document_generation() const;
    void run_javascript(String);

    OwnPtr<WebView::PhotonHeadlessWebView> view;
    int last_viewport_width { 0 };
    int last_viewport_height { 0 };
    double last_device_pixel_ratio { 0 };
};

}
