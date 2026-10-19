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
    static void set_view_residency_enabled(u64 view_id, bool enabled);
    static void set_memory_pressure(bool pressured);
    static bool set_view_task_queue_diagnostics_enabled(u64 view_id, bool enabled);
    static String task_queue_diagnostics_json(u64 view_id);
    // Every RequestServer reports its clients' network use to the monitor.
    static void configure(Requests::RequestControlClient&);

private:
    TabPerformanceMonitor();
    virtual void config_variable_changed(ConfigVariableID) override;
    void update_monitoring();
    void sample();
    bool diagnostics_enabled() const;

    bool m_enabled { false };
    bool m_setting_enabled { false };
    bool m_configured_diagnostics_enabled { false };
    bool m_memory_pressure { false };
    int m_sampling_interval_ms { 0 };
    HashTable<u64> m_explicitly_enabled_views;
    HashTable<u64> m_residency_enabled_views;
    HashTable<u64> m_task_queue_diagnostics_views;
    RefPtr<Core::Timer> m_timer;
    HashMap<u64, TabPerformanceAccumulator> m_tabs;
};

}
