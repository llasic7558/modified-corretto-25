import java.util.ArrayList;

/**
 * DaCapoReadinessTest -- integration test combining all DaCapo-like patterns.
 *
 * ~170 application allocations, multi-threaded.
 *
 * Combines:
 * - Collection growth (ArrayList with resize)
 * - Short-lived objects (ephemeral allocations)
 * - Object graphs (binary trees via Object[3])
 * - Multiple worker threads (named Thread subclass)
 * - For-each iteration (iterator allocations)
 * - Mixed lifetimes (short, medium, long)
 *
 * Validates all patterns working together before attempting DaCapo benchmarks.
 *
 * Avoids: lambdas, string concat, java.util.concurrent, System.out with vars
 */
public class DaCapoReadinessTest {

    public static void main(String[] args) throws Exception {
        System.err.println("DaCapoReadinessTest: starting");

        // ── Phase 1: Collection growth ────────────────────────────────

        System.err.println("DaCapoReadinessTest: phase 1 - collection growth");

        ArrayList<Object> collection = new ArrayList<>();
        // Add 30 items (triggers multiple resizes: 10 -> 15 -> 22 -> 33)
        for (int i = 0; i < 30; i++) {
            Object item = new Object();
            touch(item);
            collection.add(item);
        }

        // Iterate with indexed for (no iterator allocation)
        for (int i = 0; i < collection.size(); i++) {
            touch(collection.get(i));
        }

        // Clear and repopulate (old items + old backing arrays die)
        collection.clear();

        for (int i = 0; i < 10; i++) {
            Object item = new Object();
            touch(item);
            collection.add(item);
        }

        // ── Phase 2: Short-lived objects ──────────────────────────────

        System.err.println("DaCapoReadinessTest: phase 2 - short-lived objects");

        for (int i = 0; i < 20; i++) {
            Object ephemeral = new Object();
            touch(ephemeral);
            // dies immediately
        }

        // ── Phase 3: Object graph (binary tree) ──────────────────────

        System.err.println("DaCapoReadinessTest: phase 3 - object graph");

        // Tree 1: depth 4 (15 nodes + 15 data = 30 allocs)
        Object[] tree1 = buildTree(4);
        touchTree(tree1);

        // Tree 2: depth 3 (7 nodes + 7 data = 14 allocs)
        Object[] tree2 = buildTree(3);
        touchTree(tree2);

        // Kill tree 1
        tree1 = null;

        // ── Phase 4: Multi-threaded allocation ────────────────────────

        System.err.println("DaCapoReadinessTest: phase 4 - multi-threaded");

        Thread w1 = new AllocWorker("DaCapoWorker-1", 15);
        Thread w2 = new AllocWorker("DaCapoWorker-2", 15);

        w1.start();
        w2.start();
        w1.join();
        w2.join();

        // ── Phase 5: Mixed lifetime objects ───────────────────────────

        System.err.println("DaCapoReadinessTest: phase 5 - mixed lifetimes");

        // Long-lived array
        Object[] longLived = new Object[10];
        for (int i = 0; i < 10; i++) {
            longLived[i] = new Object();
            touch(longLived[i]);
        }

        // Medium-lived: allocate, then kill after more allocations
        Object[] mediumLived = new Object[5];
        for (int i = 0; i < 5; i++) {
            mediumLived[i] = new Object();
            touch(mediumLived[i]);
        }

        // More short-lived between medium creation and death
        for (int i = 0; i < 10; i++) {
            Object ephemeral = new Object();
            touch(ephemeral);
        }

        // Kill medium
        for (int i = 0; i < 5; i++) {
            mediumLived[i] = null;
        }

        // ── Phase 6: Final touches ───────────────────────────────────

        System.err.println("DaCapoReadinessTest: phase 6 - final touches");

        // Touch tree 2 (still alive)
        touchTree(tree2);

        // Touch collection items
        for (int i = 0; i < collection.size(); i++) {
            touch(collection.get(i));
        }

        // Touch long-lived
        for (int i = 0; i < 10; i++) {
            touch(longLived[i]);
        }

        System.err.println("DaCapoReadinessTest: done");
    }

    static Object[] buildTree(int depth) {
        if (depth <= 0) {
            return null;
        }
        Object[] node = new Object[3]; // [left, right, data]
        node[2] = new Object();
        touch(node[2]);

        if (depth > 1) {
            node[0] = buildTree(depth - 1);
            node[1] = buildTree(depth - 1);
        }
        return node;
    }

    static void touchTree(Object[] node) {
        if (node == null) {
            return;
        }
        touchTree((Object[]) node[0]);
        touch(node[2]);
        touch(node);
        touchTree((Object[]) node[1]);
    }

    static void touch(Object o) {
        o.hashCode();
    }

    static class AllocWorker extends Thread {
        private final int count;

        AllocWorker(String name, int count) {
            super(name);
            this.count = count;
        }

        public void run() {
            Object[] kept = new Object[count];
            for (int i = 0; i < count; i++) {
                kept[i] = new Object();
                touch(kept[i]);
            }
            // Touch all at end
            for (int i = 0; i < count; i++) {
                touch(kept[i]);
            }
        }
    }
}
