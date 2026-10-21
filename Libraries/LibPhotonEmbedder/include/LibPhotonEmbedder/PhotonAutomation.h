/*
 * Copyright (c) 2026, PhotonBrowser contributors
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibPhotonEmbedder/PhotonEmbedder.h>

#include <cstdint>

namespace Photon {

namespace Automation {

// Read-only benchmark observation and controlled test actions for a page.
// The adapter does not own the page; its View must outlive it.
class PHOTONEMBEDDER_API View {
public:
    explicit View(Photon::View&);

    // A provisional navigation may not have a committed document yet.
    bool has_completed_document_load() const;
    // Identifies the current top-level document lifecycle.
    uint64_t document_generation() const;
    // Observes the official Speedometer 3.1 result object.
    bool install_speedometer_result_observer(uint64_t expected_document_generation);
    // Observes the official JetStream 3.0 result UI.
    bool install_jetstream_result_observer(uint64_t expected_document_generation);
    // Activates only JetStream's built-in Start Test control.
    bool request_jetstream_start(uint64_t expected_document_generation);

private:
    Photon::View& m_view;
};

}
}
