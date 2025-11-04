#!/bin/bash
#
# Lock CPU to performance mode for consistent benchmarking
# This sets all cores to maximum frequency
#
# Usage: sudo ./lock_cpu_performance.sh [on|off|status]
#

set -e

ACTION="${1:-status}"

show_status() {
    echo "╔══════════════════════════════════════════════════════════════════╗"
    echo "║              CPU Performance Mode Status                         ║"
    echo "╚══════════════════════════════════════════════════════════════════╝"
    echo ""

    echo "📊 Current Governor:"
    cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
    echo ""

    echo "⚡ Current Frequencies (first 8 cores):"
    grep "cpu MHz" /proc/cpuinfo | head -8
    echo ""

    echo "🎯 Turbo Boost Status:"
    if [ -f /sys/devices/system/cpu/intel_pstate/no_turbo ]; then
        NO_TURBO=$(cat /sys/devices/system/cpu/intel_pstate/no_turbo)
        if [ "$NO_TURBO" = "1" ]; then
            echo "   Disabled (locked to base frequency)"
        else
            echo "   Enabled (can boost to max frequency)"
        fi
    else
        echo "   N/A (intel_pstate not available)"
    fi
}

set_performance() {
    echo "🚀 Setting CPU to performance mode..."
    echo ""

    # Set governor to performance for all CPUs
    for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        if [ -f "$cpu" ]; then
            echo "performance" > "$cpu"
        fi
    done

    # Optional: disable turbo boost for even more consistency
    # Uncomment if you want locked frequency (not recommended for HFT)
    # if [ -f /sys/devices/system/cpu/intel_pstate/no_turbo ]; then
    #     echo "1" > /sys/devices/system/cpu/intel_pstate/no_turbo
    # fi

    echo "✅ Performance mode enabled"
    echo ""
    show_status
}

set_powersave() {
    echo "💡 Setting CPU to powersave mode..."
    echo ""

    # Set governor to powersave for all CPUs
    for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        if [ -f "$cpu" ]; then
            echo "powersave" > "$cpu"
        fi
    done

    # Re-enable turbo boost
    if [ -f /sys/devices/system/cpu/intel_pstate/no_turbo ]; then
        echo "0" > /sys/devices/system/cpu/intel_pstate/no_turbo
    fi

    echo "✅ Powersave mode enabled"
    echo ""
    show_status
}

case "$ACTION" in
    on|performance)
        if [ "$EUID" -ne 0 ]; then
            echo "❌ Error: Must run as root"
            echo "Usage: sudo $0 on"
            exit 1
        fi
        set_performance
        ;;
    off|powersave)
        if [ "$EUID" -ne 0 ]; then
            echo "❌ Error: Must run as root"
            echo "Usage: sudo $0 off"
            exit 1
        fi
        set_powersave
        ;;
    status)
        show_status
        ;;
    *)
        echo "Usage: $0 [on|off|status]"
        echo ""
        echo "Commands:"
        echo "  on|performance  - Lock CPU to maximum performance"
        echo "  off|powersave   - Return CPU to power-saving mode"
        echo "  status          - Show current CPU frequency status"
        echo ""
        echo "Note: 'on' and 'off' require sudo"
        exit 1
        ;;
esac
