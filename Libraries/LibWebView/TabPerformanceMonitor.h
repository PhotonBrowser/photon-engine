/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <AK/kmalloc.h>
#include <LibCore/Timer.h>
#include <LibWebView/Settings.h>
#include <LibWebView/TabPerformanceStats.h>

namespace Requests {

class RequestControlClient;

}

namespace WebView {

class WEBVIEW_API TabPerformanceMonitor final : public SettingsObserver {
public:
    AK_ALLOC_WITH_KMALLOC;

    static TabPerformanceMonitor& the();
    bool enabled() const { return m_enabled; }
    static void did_present(u64 view_id);
    static void forget_view(u64 view_id);
    static void set_view_enabled(u64 view_id, bool enabled);
    // Every RequestServer reports its clients' network use to the monitor.
    static void configure(Requests::RequestControlClient&);

private:
    TabPerformanceMonitor();
    virtual void config_variable_changed(ConfigVariableID) override;
    void update_monitoring();
    void sample();

    bool m_enabled { false };
    bool m_setting_enabled { false };
    HashTable<u64> m_explicitly_enabled_views;
    RefPtr<Core::Timer> m_timer;
    HashMap<u64, TabPerformanceAccumulator> m_tabs;
};

}
