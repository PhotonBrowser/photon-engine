#!/bin/sh

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 /path/to/TestPaintingSurface" >&2
    exit 2
fi

test_binary=$1
temporary_directory=$(mktemp -d "${TMPDIR:-/tmp}/photon-ganesh-benchmark.XXXXXX")
trap 'rm -rf "$temporary_directory"' EXIT HUP INT TERM

if ! PHOTON_VERBOSE=1 "$test_binary" ganesh_persistent_cache_rect_workload >"$temporary_directory/process-a.log" 2>&1; then
    cat "$temporary_directory/process-a.log" >&2
    exit 1
fi
if ! PHOTON_VERBOSE=1 "$test_binary" ganesh_persistent_cache_rect_workload >"$temporary_directory/process-b.log" 2>&1; then
    cat "$temporary_directory/process-b.log" >&2
    exit 1
fi

extract_benchmark_line() {
    sed -n '/Photon Ganesh persistent-cache benchmark:/p' "$1" | tail -n 1
}

extract_descriptor_hashes() {
    sed -n 's/.* descriptor_hashes=\([^ ]*\) flush_and_submit=.*/\1/p' "$1"
}

extract_render_task_count() {
    sed -n 's/.*Photon Ganesh flush detail:.* tasks=\([0-9][0-9]*\).*/\1/p' "$1" | tail -n 1
}

extract_ops_task_signature() {
    sed -n 's/.* task=Ops#\([0-9][0-9]*\) target=\([^ ]*\) op_chain=\([^ ]*\).*/\1 target=\2 op_chain=\3/p' "$1" | awk '!seen[$0]++'
}

extract_ops_task_count() {
    extract_ops_task_signature "$1" | wc -l | tr -d ' '
}

extract_shader_compile_ms() {
    awk '
        /Photon Metal shader libraries:/ {
            for (i = 1; i <= NF; ++i) {
                if ($i ~ /^vertex=/) { split($i, value, "="); total += value[2] }
                if ($i ~ /^fragment=/) { split($i, value, "="); total += value[2] }
            }
        }
        END { printf "%.3f", total }
    ' "$1"
}

extract_worst_flush_ms() {
    awk '
        /Photon Ganesh flush detail:/ {
            for (i = 1; i <= NF; ++i) {
                if ($i ~ /^total=/) { split($i, value, "="); if (value[2] > maximum) maximum = value[2] }
            }
        }
        END { printf "%.3f", maximum }
    ' "$1"
}

line_a=$(extract_benchmark_line "$temporary_directory/process-a.log")
line_b=$(extract_benchmark_line "$temporary_directory/process-b.log")
hashes_a=$(extract_descriptor_hashes "$temporary_directory/process-a.log")
hashes_b=$(extract_descriptor_hashes "$temporary_directory/process-b.log")
tasks_a=$(extract_render_task_count "$temporary_directory/process-a.log")
tasks_b=$(extract_render_task_count "$temporary_directory/process-b.log")
ops_a=$(extract_ops_task_signature "$temporary_directory/process-a.log")
ops_b=$(extract_ops_task_signature "$temporary_directory/process-b.log")
ops_count_a=$(extract_ops_task_count "$temporary_directory/process-a.log")
ops_count_b=$(extract_ops_task_count "$temporary_directory/process-b.log")

if [ -z "$line_a" ] || [ -z "$line_b" ]; then
    echo "benchmark did not produce a diagnostics line in both processes" >&2
    exit 1
fi
if [ -z "$hashes_a" ] || [ -z "$hashes_b" ]; then
    echo "benchmark did not request any persistent descriptors" >&2
    exit 1
fi
if [ -z "$tasks_a" ] || [ "$tasks_a" -eq 0 ] || [ -z "$tasks_b" ] || [ "$tasks_b" -eq 0 ]; then
    echo "Ganesh did not report render tasks in both processes" >&2
    cat "$temporary_directory/process-a.log" "$temporary_directory/process-b.log" >&2
    exit 1
fi
if [ -z "$ops_a" ] || [ -z "$ops_b" ] || [ "$ops_count_a" -eq 0 ] || [ "$ops_count_b" -eq 0 ]; then
    echo "Ganesh did not report Ops tasks in both processes" >&2
    cat "$temporary_directory/process-a.log" "$temporary_directory/process-b.log" >&2
    exit 1
fi
if [ "$tasks_a" != "$tasks_b" ]; then
    echo "render-task count differs: process A=$tasks_a process B=$tasks_b" >&2
    exit 1
fi
if [ "$ops_a" != "$ops_b" ]; then
    echo "Ops-task sequence differs between processes" >&2
    diff -u "$temporary_directory/process-a.log" "$temporary_directory/process-b.log" >&2 || true
    exit 1
fi
if [ "$hashes_a" != "$hashes_b" ]; then
    echo "descriptor hash sequence differs between processes" >&2
    echo "process A: $hashes_a" >&2
    echo "process B: $hashes_b" >&2
    exit 1
fi
echo "Ganesh workload invariants passed: render_tasks=$tasks_a ops_tasks=$ops_count_a descriptor_requests=$(printf '%s' "$hashes_a" | awk -F, '{ print NF }') descriptor_hashes_match=yes"
echo "process A: $line_a shader_compile_ms=$(extract_shader_compile_ms "$temporary_directory/process-a.log") worst_flush_ms=$(extract_worst_flush_ms "$temporary_directory/process-a.log")"
echo "process B: $line_b shader_compile_ms=$(extract_shader_compile_ms "$temporary_directory/process-b.log") worst_flush_ms=$(extract_worst_flush_ms "$temporary_directory/process-b.log")"
