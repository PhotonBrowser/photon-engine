/*
 * Copyright (c) 2026, PhotonBrowser contributors
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "PhotonEmbedderInternal.h"
#include <AK/StringBuilder.h>
#include <LibPhotonEmbedder/PhotonAutomation.h>

namespace Photon {

using namespace AK;

Automation::View::View(Photon::View& view)
    : m_view(view)
{
}

bool Automation::View::has_completed_document_load() const
{
    return m_view.m_impl && m_view.m_impl->has_completed_document_load();
}
uint64_t Automation::View::document_generation() const
{
    return m_view.m_impl ? m_view.m_impl->document_generation() : 0;
}
bool Automation::View::install_speedometer_result_observer(uint64_t expected_document_generation)
{
    if (!m_view.m_impl
        || !m_view.m_impl->has_completed_document_load()
        || m_view.m_impl->document_generation() != expected_document_generation)
        return false;
    StringBuilder script_builder;
    script_builder.append(R"JS((() => {
        const prefix = "VelocitySpeedometer3.1:";
        const documentGeneration = )JS"sv);
    auto generation = String::number(expected_document_generation);
    script_builder.append(generation.bytes_as_string_view());
    script_builder.append(R"JS(;
        const root = document.documentElement;
        if (!root || globalThis.__velocitySpeedometerObserverGeneration === documentGeneration)
            return;
        globalThis.__velocitySpeedometerObserverGeneration = documentGeneration;
        const errors = [];
        let lastProgressKey = null;
        let lastProgressAt = performance.now();
        const boundedText = value => String(value || "").slice(0, 192);
        const runnerState = marks => ({
            runner_is_running: typeof globalThis.benchmarkClient?._isRunning === "boolean" ? globalThis.benchmarkClient._isRunning : null,
            runner_finished_test_count: Number.isSafeInteger(globalThis.benchmarkClient?._finishedTestCount) ? globalThis.benchmarkClient._finishedTestCount : null,
            runner_marks: marks,
            document_visibility_state: document.visibilityState || null,
        });
        const testFrameState = () => {
            const frame = document.querySelector("iframe.test-runner");
            if (!frame)
                return { test_frame_src: null, test_frame_location: null, test_frame_ready_state: null, test_frame_visibility_state: null, test_frame_has_todo_app: null, test_frame_has_new_todo: null, test_frame_boot_diagnostics: null, test_frame_onload_handler: null };
            try {
                const frameDocument = frame.contentDocument;
                const frameWindow = frame.contentWindow;
                const scripts = Array.from(frameDocument?.querySelectorAll("script[src]") || []).slice(0, 4).map(script => {
                    const entries = frameWindow?.performance?.getEntriesByName(script.src) || [];
                    const resource = entries[entries.length - 1];
                    const path = new URL(script.src, frameDocument.location.href).pathname.split("/").slice(-2).join("/");
                    return `${path}:defer=${script.defer}:async=${script.async}:loaded=${!!resource && resource.responseEnd > 0}:bytes=${resource?.encodedBodySize || 0}`;
                }).join(",");
                const bootDiagnostics = frameDocument ? boundedText(
                    `body=${frameDocument.body?.children.length ?? 0};inputs=${frameDocument.getElementsByTagName("input").length};class=${frameDocument.getElementsByClassName("new-todo").length};selector=${!!frameDocument.querySelector(".new-todo")};scripts=${scripts}`
                ) : null;
                return {
                    test_frame_src: boundedText(frame.getAttribute("src")),
                    test_frame_location: frameDocument ? boundedText(frameDocument.location.href) : null,
                    test_frame_ready_state: frameDocument?.readyState || null,
                    test_frame_visibility_state: frameDocument?.visibilityState || null,
                    test_frame_has_todo_app: !!frameDocument?.querySelector("todo-app"),
                    test_frame_has_new_todo: !!frameDocument?.querySelector(".new-todo"),
                    test_frame_boot_diagnostics: bootDiagnostics,
                    test_frame_onload_handler: typeof frame.onload === "function",
                };
            } catch (error) {
                return {
                    test_frame_src: boundedText(frame.getAttribute("src")),
                    test_frame_location: null,
                    test_frame_ready_state: null,
                    test_frame_visibility_state: null,
                    test_frame_has_todo_app: null,
                    test_frame_has_new_todo: null,
                    test_frame_boot_diagnostics: null,
                    test_frame_onload_handler: typeof frame.onload === "function",
                    test_frame_error: boundedText(error?.message || error),
                };
            }
        };
        const recordError = (name, message) => {
            errors.push({ name: boundedText(name), message: boundedText(message) });
            if (errors.length > 4)
                errors.shift();
            publish();
        };
        const publish = () => {
            const section = root.getAttribute("data-visible-section");
            const marks = performance.getEntriesByType("mark").slice(-8).map(mark => boundedText(mark.name));
            const latestSuiteMark = [...marks].reverse().find(mark => mark.startsWith("suite-"));
            const markedSuite = latestSuiteMark?.replace(/^suite-/, "").replace(/-(?:prepare-start|prepare-end|start|end)$/, "") || null;
            const latestOperationMark = [...marks].reverse().find(mark => mark.startsWith("suite-") || mark.includes("."));
            const testMatch = latestOperationMark?.match(/\.([^.]+)-(?:start|sync-end|async-start|async-end)$/);
            const suite = markedSuite || document.querySelector("#info-label")?.textContent?.trim() || null;
            const progressNode = document.querySelector("#progress-completed");
            const completed = progressNode && Number.isFinite(progressNode.value) ? progressNode.value : null;
            const total = progressNode && Number.isFinite(progressNode.max) ? progressNode.max : null;
            const requestedIterations = Number.parseInt(new URLSearchParams(location.search).get("iterationCount") || "10", 10);
            const iterationCount = Number.isSafeInteger(requestedIterations) && requestedIterations > 0
                ? requestedIterations
                : null;
            const testsPerIteration = iterationCount && total !== null && total % iterationCount === 0
                ? total / iterationCount
                : null;
            const currentIteration = completed !== null && testsPerIteration
                ? Math.min(Math.floor(completed / testsPerIteration) + 1, iterationCount)
                : null;
            const currentTest = testMatch?.[1] || null;
            const progressKey = `${section}|${suite}|${currentTest}|${completed}|${total}`;
            if (progressKey !== lastProgressKey) {
                lastProgressKey = progressKey;
                lastProgressAt = performance.now();
            }
            const warning = document.querySelector("#screen-size-warning");
            const viewportWarning = !!warning && getComputedStyle(warning).display !== "none";
            if (section === "running") {
                const title = prefix + JSON.stringify({
                    state: "running",
                    version: "3.1",
                    document_generation: documentGeneration,
                    document_url: location.origin + location.pathname,
                    document_ready_state: document.readyState,
                    benchmark_running: true,
                    viewport_warning: viewportWarning,
                    current_suite: suite,
                    current_test: currentTest,
                    current_iteration: currentIteration,
                    completed_tests: completed,
                    total_tests: total,
                    ...runnerState(marks),
                    ...testFrameState(),
                    last_progress_at_ms: lastProgressAt,
                    observer_errors: errors,
                });
                if (document.title !== title)
                    document.title = title;
                return;
            }
            if (section !== "summary") {
                const title = prefix + JSON.stringify({
                    state: "waiting",
                    version: "3.1",
                    document_generation: documentGeneration,
                    document_url: location.origin + location.pathname,
                    document_ready_state: document.readyState,
                    benchmark_running: false,
                    viewport_warning: viewportWarning,
                    current_suite: suite,
                    current_test: null,
                    current_iteration: null,
                    completed_tests: completed,
                    total_tests: total,
                    ...runnerState(marks),
                    ...testFrameState(),
                    last_progress_at_ms: lastProgressAt,
                    observer_errors: errors,
                });
                if (document.title !== title)
                    document.title = title;
                return;
            }
            const summary = document.querySelector("#summary");
            const invalid = summary && summary.classList.contains("invalid");
            const scoreMetric = globalThis.benchmarkClient?.metrics?.Score;
            const score = scoreMetric && Number.isFinite(scoreMetric.mean) ? scoreMetric.mean : null;
            const internalScores = scoreMetric && Array.isArray(scoreMetric.values) ? scoreMetric.values : [];
            const payload = invalid
                ? { state: "failed", version: "3.1", document_generation: documentGeneration, document_url: location.origin + location.pathname, document_ready_state: document.readyState, benchmark_running: false, error: boundedText(document.querySelector("#invalid-score-text")?.innerText?.trim() || "Speedometer reported an invalid score"), viewport_warning: viewportWarning, observer_errors: errors }
                : { state: "complete", version: "3.1", document_generation: documentGeneration, document_url: location.origin + location.pathname, document_ready_state: document.readyState, benchmark_running: false, score, internal_scores: internalScores, viewport_warning: viewportWarning, observer_errors: errors };
            const title = prefix + JSON.stringify(payload);
            if (document.title !== title)
                document.title = title;
        };
        addEventListener("error", event => recordError(event.error?.name || "Error", event.message));
        addEventListener("unhandledrejection", event => recordError(event.reason?.name || "UnhandledRejection", event.reason?.message || event.reason));
        document.addEventListener("visibilitychange", publish);
        const observedTestFrames = new WeakSet();
        const observedFrameDocuments = new WeakSet();
        const observeTestFrameDocument = frame => {
            try {
                const frameDocument = frame.contentDocument;
                const frameWindow = frame.contentWindow;
                if (!frameDocument || !frameWindow || observedFrameDocuments.has(frameDocument))
                    return;
                observedFrameDocuments.add(frameDocument);
                frameWindow.addEventListener("error", event => recordError(`frame:${event.error?.name || "Error"}`, event.message), true);
                frameWindow.addEventListener("unhandledrejection", event => recordError(`frame:${event.reason?.name || "UnhandledRejection"}`, event.reason?.message || event.reason));
                frameDocument.addEventListener("visibilitychange", publish);
            } catch (error) {
                recordError("frame_observer", error?.message || error);
            }
        };
        const observeTestFrame = frame => {
            if (frame instanceof HTMLIFrameElement && frame.classList.contains("test-runner") && !observedTestFrames.has(frame)) {
                observedTestFrames.add(frame);
                observer.observe(frame, { attributes: true, attributeFilter: ["src"] });
                frame.addEventListener("load", () => {
                    observeTestFrameDocument(frame);
                    publish();
                });
                observeTestFrameDocument(frame);
            }
        };
        const observer = new MutationObserver(records => {
            for (const record of records) {
                for (const node of record.addedNodes || []) {
                    observeTestFrame(node);
                }
            }
            publish();
        });
        observer.observe(root, { attributes: true, attributeFilter: ["data-visible-section"] });
        if (document.body)
            observer.observe(document.body, { childList: true });
        for (const frame of document.querySelectorAll("iframe.test-runner"))
            observeTestFrame(frame);
        const summary = document.querySelector("#summary");
        if (summary)
            observer.observe(summary, { attributes: true, attributeFilter: ["class"] });
        const progressNode = document.querySelector("#progress-completed");
        if (progressNode)
            observer.observe(progressNode, { attributes: true, attributeFilter: ["max", "value"] });
        for (const selector of ["#info-label", "#info-progress"])
            if (document.querySelector(selector))
                observer.observe(document.querySelector(selector), { childList: true, characterData: true, subtree: true });
        publish();
    })())JS"sv);
    auto script = MUST(script_builder.to_string());
    m_view.m_impl->run_javascript(move(script));
    return true;
}
bool Automation::View::install_jetstream_result_observer(uint64_t expected_document_generation)
{
    if (!m_view.m_impl
        || !m_view.m_impl->has_completed_document_load()
        || m_view.m_impl->document_generation() != expected_document_generation)
        return false;
    StringBuilder script_builder;
    script_builder.append(R"JS((() => {
        const prefix = "VelocityJetStream3.0:";
        const documentGeneration = )JS"sv);
    auto generation = String::number(expected_document_generation);
    script_builder.append(generation.bytes_as_string_view());
    script_builder.append(R"JS(;
        const root = document.documentElement;
        if (!root || globalThis.__velocityJetStreamObserverGeneration === documentGeneration)
            return;
        globalThis.__velocityJetStreamObserverGeneration = documentGeneration;
        const boundedText = value => String(value || "").slice(0, 192);
        const observerErrors = [];
        const recordError = error => {
            observerErrors.push(boundedText(error?.message || error));
            if (observerErrors.length > 4)
                observerErrors.shift();
            publish();
        };
        const publish = () => {
            const driver = globalThis.JetStream;
            const status = document.querySelector("#status");
            const summary = document.querySelector("#result-summary");
            const scoreText = summary?.querySelector(".score")?.textContent?.trim() || "";
            const score = Number(scoreText);
            const workloads = Array.isArray(driver?.benchmarks) ? driver.benchmarks : [];
            const completed = workloads.filter(benchmark =>
                document.getElementById(`benchmark-${benchmark.name}`)?.classList.contains("benchmark-done")
            ).length;
            const current = workloads.find(benchmark =>
                document.getElementById(`benchmark-${benchmark.name}`)?.classList.contains("benchmark-running")
            );
            const driverErrors = Array.isArray(driver?.errors)
                ? driver.errors.slice(0, 4).map(error => boundedText(error?.error || error))
                : [];
            const errors = [...driverErrors, ...observerErrors].slice(0, 8);
            const statusText = boundedText(status?.innerText || status?.textContent || "");
            const failed = globalThis.allIsGood === false
                || status?.classList.contains("error") === true
                || document.querySelector(".benchmark-error") !== null
                || errors.length > 0;
            const finished = driver?.isDone === true
                && summary?.classList.contains("done") === true
                && workloads.length > 0
                && completed === workloads.length
                && Number.isFinite(score)
                && score > 0
                && !failed;
            const subtestScores = {};
            for (const benchmark of workloads) {
                const value = benchmark?.score;
                if (typeof benchmark?.name === "string" && Number.isFinite(value) && value > 0)
                    subtestScores[benchmark.name] = value;
            }
            const initialized = driver?.isReady === true && globalThis.allIsGood !== false;
            const started = statusText.includes("Running") || !!current || finished;
            const state = finished ? "complete" : failed ? "failed" : started ? "running" : initialized ? "ready" : "loading";
            const title = prefix + JSON.stringify({
                state,
                initialized,
                started,
                completed: finished,
                failed,
                official_score: Number.isFinite(score) && score > 0 ? score : null,
                completed_workloads: completed,
                total_workloads: workloads.length,
                current_workload: current?.name || null,
                status_text: statusText,
                errors,
                subtest_scores: subtestScores,
                document_generation: documentGeneration,
                document_url: location.origin + location.pathname,
            });
            if (document.title !== title)
                document.title = title;
        };
        addEventListener("JetStreamReady", publish);
        addEventListener("JetStreamDone", publish);
        addEventListener("error", event => recordError(event.error || event.message));
        addEventListener("unhandledrejection", event => recordError(event.reason));
        const observer = new MutationObserver(publish);
        if (document.body)
            observer.observe(document.body, { attributes: true, childList: true, characterData: true, subtree: true });
        publish();
    })())JS"sv);
    auto script = MUST(script_builder.to_string());
    m_view.m_impl->run_javascript(move(script));
    return true;
}
bool Automation::View::request_jetstream_start(uint64_t expected_document_generation)
{
    if (!m_view.m_impl
        || !m_view.m_impl->has_completed_document_load()
        || m_view.m_impl->document_generation() != expected_document_generation)
        return false;
    StringBuilder script_builder;
    script_builder.append(R"JS((() => {
        const expectedGeneration = )JS"sv);
    auto generation = String::number(expected_document_generation);
    script_builder.append(generation.bytes_as_string_view());
    script_builder.append(R"JS(;
        if (globalThis.__velocityJetStreamObserverGeneration !== expectedGeneration)
            return;
        const officialHost = location.protocol === "https:" && location.hostname === "browserbench.org";
        const loopbackHost = location.protocol === "http:" && location.hostname === "127.0.0.1";
        const validPath = location.pathname === "/JetStream3.0"
            || location.pathname === "/JetStream3.0/"
            || location.pathname === "/JetStream3.0/index.html";
        if (!(officialHost || loopbackHost) || !validPath || location.search.length)
            return;
        const driver = globalThis.JetStream;
        const status = document.querySelector("#status");
        if (driver?.isReady !== true || driver.isDone || !status
            || typeof status.onclick !== "function"
            || !status.querySelector("a.button"))
            return;
        status.click();
    })())JS"sv);
    auto script = MUST(script_builder.to_string());
    m_view.m_impl->run_javascript(move(script));
    return true;
}

}
