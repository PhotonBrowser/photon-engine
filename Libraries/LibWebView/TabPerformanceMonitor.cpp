/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/HashTable.h>
#include <LibRequests/RequestControlClient.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWebView/ProcessManager.h>
#include <LibWebView/RequestServerManager.h>
#include <LibWebView/TabPerformanceMonitor.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>
#include <LibWebView/WebWorkerClient.h>
#include <LibWebView/WorkerProcessManager.h>

namespace WebView {

static TabPerformanceMonitor* s_monitor;
static constexpr int residency_sampling_interval_ms = 30'000;
static constexpr int pressure_sampling_interval_ms = 2'000;

TabPerformanceMonitor& TabPerformanceMonitor::the()
{
    if (!s_monitor)
        s_monitor = new TabPerformanceMonitor;
    return *s_monitor;
}

void TabPerformanceMonitor::configure(Requests::RequestControlClient& requests)
{
    if (!s_monitor)
        return;
    auto diagnostics_enabled = s_monitor->diagnostics_enabled();
    requests.async_set_performance_monitor_enabled(diagnostics_enabled);
    if (!diagnostics_enabled) {
        requests.on_network_usage = nullptr;
        return;
    }
    requests.on_network_usage = [](Vector<Requests::NetworkUsage> usage, u64 interval_microseconds) {
        if (!s_monitor || !s_monitor->diagnostics_enabled())
            return;
        auto now = MonotonicTime::now();
        for (auto const& entry : usage) {
            Optional<u64> owner;
            WebContentClient::for_each_client([&](WebContentClient& client) {
                if (client.pid() != entry.process_id)
                    return IterationDecision::Continue;
                if (auto* page = client.page(entry.page_id)) {
                    auto const& view = page->view();
                    owner = view.owner_view_id().value_or(view.view_id());
                }
                return IterationDecision::Break;
            });
            if (!owner.has_value())
                owner = WorkerProcessManager::the().exclusive_performance_owner(entry.process_id);
            s_monitor->m_process_networks.ensure(entry.process_id).add_network_bytes(now, interval_microseconds, entry.download_bytes, entry.upload_bytes);
            if (owner.has_value())
                s_monitor->m_tabs.ensure(*owner).add_network_bytes(now, interval_microseconds, entry.download_bytes, entry.upload_bytes);
        }
    };
}

void TabPerformanceMonitor::forget_view(u64 view_id)
{
    if (s_monitor) {
        WebContentClient::set_task_queue_diagnostics_enabled(view_id, false);
        s_monitor->m_explicitly_enabled_views.remove(view_id);
        s_monitor->m_residency_enabled_views.remove(view_id);
        s_monitor->m_task_queue_diagnostics_views.remove(view_id);
        s_monitor->m_tabs.remove(view_id);
        s_monitor->update_monitoring();
    }
}

void TabPerformanceMonitor::set_view_enabled(u64 view_id, bool enabled)
{
    auto& monitor = the();
    if (enabled)
        monitor.m_explicitly_enabled_views.set(view_id);
    else
        monitor.m_explicitly_enabled_views.remove(view_id);
    monitor.update_monitoring();
}

void TabPerformanceMonitor::set_view_residency_enabled(u64 view_id, bool enabled)
{
    auto& monitor = the();
    if (enabled)
        monitor.m_residency_enabled_views.set(view_id);
    else
        monitor.m_residency_enabled_views.remove(view_id);
    monitor.update_monitoring();
}

void TabPerformanceMonitor::set_memory_pressure(bool pressured)
{
    auto& monitor = the();
    if (monitor.m_memory_pressure == pressured)
        return;
    monitor.m_memory_pressure = pressured;
    monitor.update_monitoring();
}

bool TabPerformanceMonitor::set_view_task_queue_diagnostics_enabled(u64 view_id, bool enabled)
{
    auto& monitor = the();
    if (enabled) {
        if (!WebContentClient::set_task_queue_diagnostics_enabled(view_id, true))
            return false;
        monitor.m_task_queue_diagnostics_views.set(view_id);
    } else {
        monitor.m_task_queue_diagnostics_views.remove(view_id);
        WebContentClient::set_task_queue_diagnostics_enabled(view_id, false);
    }
    return true;
}

String TabPerformanceMonitor::task_queue_diagnostics_json(u64 view_id)
{
    if (!s_monitor || !s_monitor->m_task_queue_diagnostics_views.contains(view_id))
        return "{\"schema_version\":1,\"scope\":\"webcontent_processes\",\"processes\":[]}"_string;
    return WebContentClient::task_queue_diagnostics_json(view_id);
}

TabPerformanceMonitor::TabPerformanceMonitor()
{
    config_variable_changed(ConfigVariableID::ShowTabPerformanceMonitor);
}

void TabPerformanceMonitor::config_variable_changed(ConfigVariableID id)
{
    if (id != ConfigVariableID::ShowTabPerformanceMonitor)
        return;
    m_setting_enabled = Application::settings().config_variable_as_bool(id);
    update_monitoring();
}

void TabPerformanceMonitor::update_monitoring()
{
    auto diagnostics = diagnostics_enabled();
    auto should_enable = diagnostics || !m_residency_enabled_views.is_empty();
    if (diagnostics != m_configured_diagnostics_enabled) {
        m_configured_diagnostics_enabled = diagnostics;
        Application::request_server_manager().for_each_instance([](RequestServerInstance& instance) {
            configure(instance.control_client());
            return IterationDecision::Continue;
        });
    }
    if (!should_enable) {
        if (m_timer)
            m_timer = nullptr;
        m_enabled = false;
        m_sampling_interval_ms = 0;
        m_tabs.clear();
        m_process_networks.clear();
        m_process_network_snapshots.clear();
        return;
    }
    auto interval_ms = diagnostics ? 500 : m_memory_pressure ? pressure_sampling_interval_ms
                                                             : residency_sampling_interval_ms;
    if (m_enabled && m_sampling_interval_ms == interval_ms)
        return;
    auto was_enabled = m_enabled;
    if (m_timer)
        m_timer = nullptr;
    m_enabled = true;
    m_sampling_interval_ms = interval_ms;
    m_timer = Core::Timer::create_repeating(AK::Duration::from_milliseconds(interval_ms), [this] { sample(); });
    m_timer->start();
    if (!was_enabled || diagnostics || m_memory_pressure)
        sample();
}

bool TabPerformanceMonitor::diagnostics_enabled() const
{
    return m_setting_enabled || !m_explicitly_enabled_views.is_empty();
}

bool TabPerformanceMonitor::process_network_statistics_enabled()
{
    return s_monitor && s_monitor->diagnostics_enabled();
}

Optional<ProcessNetworkUsage> TabPerformanceMonitor::process_network_usage(pid_t pid)
{
    if (!process_network_statistics_enabled())
        return {};
    if (auto usage = s_monitor->m_process_network_snapshots.get(pid); usage.has_value())
        return *usage;
    return {};
}

void TabPerformanceMonitor::did_present(u64 view_id)
{
    if (s_monitor && s_monitor->diagnostics_enabled())
        s_monitor->m_tabs.ensure(view_id).did_present(MonotonicTime::now());
}

void TabPerformanceMonitor::sample()
{
    HashMap<u64, HashMap<pid_t, Core::Platform::ProcessResourceUsage>> processes;
    HashMap<u64, HashMap<pid_t, u64>> process_memory;
    HashMap<u64, HashMap<pid_t, u64>> managed_heaps;
    // Process-level memory is split across live tabs so the per-tab totals do not duplicate it.
    auto attribute_memory = [](HashMap<u64, HashMap<pid_t, u64>>& memory_by_view, Vector<u64> const& owners, pid_t pid, u64 bytes) {
        if (owners.is_empty())
            return;
        auto owner_count = static_cast<u64>(owners.size());
        auto share = bytes / owner_count;
        auto remainder = bytes % owner_count;
        for (size_t i = 0; i < owners.size(); ++i)
            memory_by_view.ensure(owners[i]).set(pid, share + static_cast<u64>(i < remainder));
    };

    WebContentClient::for_each_client([&](WebContentClient& client) {
        auto owners = client.performance_owners();
        auto bytes = client.managed_heap_bytes();
        if (bytes.has_value())
            attribute_memory(managed_heaps, owners, client.pid(), *bytes);
        return IterationDecision::Continue;
    });
    WorkerProcessManager::the().for_each_client([&](WebWorkerClient& client) {
        auto owners = WorkerProcessManager::the().performance_owners(client.pid());
        auto bytes = client.managed_heap_bytes();
        if (bytes.has_value())
            attribute_memory(managed_heaps, owners, client.pid(), *bytes);
        return IterationDecision::Continue;
    });
    Application::process_manager().for_each_process([&](Process& process) {
        Vector<u64> owners;
        if (process.type() == ProcessType::WebContent) {
            if (auto client = process.client<WebContentClient>())
                owners = client->performance_owners();
        } else if (process.type() == ProcessType::WebWorker) {
            owners = WorkerProcessManager::the().performance_owners(process.pid());
        }
        if (owners.is_empty())
            return;
        auto usage = Application::process_manager().resource_usage(process.pid());
        if (!usage.has_value())
            return;
        attribute_memory(process_memory, owners, process.pid(), usage->attributed_memory_bytes);
        if (owners.size() == 1)
            processes.ensure(owners.first()).set(process.pid(), *usage);
    });
    auto now = MonotonicTime::now();
    HashTable<pid_t> live_processes;
    Application::process_manager().for_each_process([&](Process& process) {
        live_processes.set(process.pid());
    });
    m_process_networks.remove_all_matching([&](pid_t pid, auto const&) { return !live_processes.contains(pid); });
    m_process_network_snapshots.clear();
    for (auto& entry : m_process_networks) {
        auto stats = entry.value.sample(now, {});
        m_process_network_snapshots.set(entry.key, {
                                                       .download_bytes_per_second = stats.download_bytes_per_second,
                                                       .upload_bytes_per_second = stats.upload_bytes_per_second,
                                                   });
    }
    ViewImplementation::for_each_view([&](ViewImplementation& view) {
        if (view.owner_view_id().has_value())
            return IterationDecision::Continue;
        Optional<pid_t> top_level_process;
        if (auto page = view.traversable().display_page())
            top_level_process = page->client().pid();
        auto stats = m_tabs.ensure(view.view_id()).sample(now, processes.ensure(view.view_id()), top_level_process);
        Checked<u64> process_memory_bytes = 0;
        bool has_process_memory = false;
        if (auto memory = process_memory.get(view.view_id()); memory.has_value()) {
            for (auto const& process : *memory) {
                process_memory_bytes += process.value;
                has_process_memory = true;
            }
        }
        if (has_process_memory && !process_memory_bytes.has_overflow())
            stats.memory_bytes = process_memory_bytes.value();
        else
            stats.memory_bytes = {};

        Checked<u64> managed_heap_bytes = 0;
        if (auto heaps = managed_heaps.get(view.view_id()); heaps.has_value()) {
            for (auto const& heap : *heaps)
                managed_heap_bytes += heap.value;
            if (!managed_heap_bytes.has_overflow())
                stats.managed_heap_bytes = managed_heap_bytes.value();
        }
        if ((diagnostics_enabled() || m_residency_enabled_views.contains(view.view_id())) && view.on_performance_stats)
            view.on_performance_stats(stats);
        return IterationDecision::Continue;
    });
}

}
