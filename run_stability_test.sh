#!/bin/bash
#
# Test if traces are stable across multiple runs
# This helps determine if we need per-thread clocks or vector clocks
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# Configuration
TRACE_DIR="$SCRIPT_DIR/stability_traces"
ET_JAR="$PROJECT_ROOT/elephant-tracks/target/elephant-tracks-3.0.0-jar-with-dependencies.jar"
NUM_RUNS=3

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "=============================================="
echo "Trace Stability Test"
echo "=============================================="
echo ""

# Check prerequisites
if [ ! -f "$ET_JAR" ]; then
    echo -e "${RED}ERROR: Elephant Tracks JAR not found at:${NC}"
    echo "  $ET_JAR"
    echo ""
    echo "Build it with:"
    echo "  cd $PROJECT_ROOT/elephant-tracks && mvn package"
    exit 1
fi

# Create output directory
mkdir -p "$TRACE_DIR"
cd "$SCRIPT_DIR"

# Compile test program
echo "Compiling ThreadStabilityTest.java..."
javac ThreadStabilityTest.java

echo ""
echo "=============================================="
echo "Running $NUM_RUNS traces (normal scheduling)"
echo "=============================================="

for i in $(seq 1 $NUM_RUNS); do
    echo "Run $i..."
    java -javaagent:"$ET_JAR"=output="$TRACE_DIR/normal_$i.etb.zst" \
        ThreadStabilityTest 2>/dev/null || true
done

echo ""
echo "=============================================="
echo "Running $NUM_RUNS traces (single processor)"
echo "=============================================="

for i in $(seq 1 $NUM_RUNS); do
    echo "Run $i..."
    java -XX:ActiveProcessorCount=1 \
        -javaagent:"$ET_JAR"=output="$TRACE_DIR/single_$i.etb.zst" \
        ThreadStabilityTest 2>/dev/null || true
done

echo ""
echo "=============================================="
echo "Generating oracles from traces"
echo "=============================================="

for mode in normal single; do
    for i in $(seq 1 $NUM_RUNS); do
        echo "Generating oracle for ${mode}_$i..."
        python3 "$PROJECT_ROOT/oracle_generator.py" \
            "$TRACE_DIR/${mode}_$i.etb.zst" \
            -o "$TRACE_DIR/${mode}_$i_oracle.csv" \
            -m liveness --stats 2>/dev/null || echo "  (failed)"
    done
done

echo ""
echo "=============================================="
echo "Comparing traces"
echo "=============================================="

compare_oracles() {
    local file1=$1
    local file2=$2

    if [ ! -f "$file1" ] || [ ! -f "$file2" ]; then
        echo "  (files missing)"
        return
    fi

    # Compare line counts
    lines1=$(wc -l < "$file1")
    lines2=$(wc -l < "$file2")

    if [ "$lines1" = "$lines2" ]; then
        # Compare content (excluding first line header)
        diff_count=$(diff <(tail -n +2 "$file1" | sort) <(tail -n +2 "$file2" | sort) | wc -l)
        if [ "$diff_count" = "0" ]; then
            echo -e "  ${GREEN}IDENTICAL${NC} (both have $lines1 entries)"
        else
            echo -e "  ${YELLOW}SAME SIZE but $diff_count lines differ${NC}"
        fi
    else
        echo -e "  ${RED}DIFFERENT: $lines1 vs $lines2 entries${NC}"
    fi
}

echo ""
echo "Normal scheduling (expect differences):"
for i in $(seq 2 $NUM_RUNS); do
    echo -n "  Run 1 vs Run $i: "
    compare_oracles "$TRACE_DIR/normal_1_oracle.csv" "$TRACE_DIR/normal_${i}_oracle.csv"
done

echo ""
echo "Single processor (expect similarity):"
for i in $(seq 2 $NUM_RUNS); do
    echo -n "  Run 1 vs Run $i: "
    compare_oracles "$TRACE_DIR/single_1_oracle.csv" "$TRACE_DIR/single_${i}_oracle.csv"
done

echo ""
echo "=============================================="
echo "Cross-thread analysis"
echo "=============================================="

if [ -f "$TRACE_DIR/normal_1.etb.zst" ]; then
    python3 "$SCRIPT_DIR/analyze_cross_thread.py" "$TRACE_DIR/normal_1.etb.zst"
fi

echo ""
echo "=============================================="
echo "Summary"
echo "=============================================="
echo ""
echo "Results saved in: $TRACE_DIR"
echo ""
echo "Next steps based on results:"
echo "  1. If single-processor traces are identical:"
echo "     -> Use -XX:ActiveProcessorCount=1 for experiments"
echo ""
echo "  2. If cross-thread sharing is <5%:"
echo "     -> Implement per-thread clocks in oracle_generator.py"
echo ""
echo "  3. If cross-thread sharing is >20%:"
echo "     -> Consider implementing vector clocks"
echo ""
