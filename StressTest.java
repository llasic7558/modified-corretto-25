import java.util.ArrayList;

/**
 * StressTest -- high-volume allocation test for Oracle GC.
 *
 * Rewritten to avoid crash patterns:
 * - NO java.util.concurrent (ConcurrentLinkedQueue, AtomicInteger, CountDownLatch)
 * - NO lambdas
 * - NO string concatenation (new String("str" + i))
 * - Named static inner classes for threads
 * - All output via System.err with constant strings
 * - Uses only Object, Object[], int[], and ArrayList
 *
 * Phases:
 * 1. Long-lived objects (survive entire test)
 * 2. Short-lived objects (die immediately)
 * 3. Linked structure via Object[] nodes
 * 4. Multi-threaded allocation (4 workers, named Thread subclass)
 * 5. Final use of long-lived objects
 */
public class StressTest {

    static final int OBJECTS_PER_THREAD = 100;
    static final int NUM_THREADS = 4;
    static final int LONG_LIVED_COUNT = 50;

    public static void main(String[] args) throws Exception {
        System.err.println("StressTest: starting");

        // Phase 1: Long-lived objects (survive entire test)
        System.err.println("StressTest: phase 1 - long-lived objects");
        Object[] longLived = new Object[LONG_LIVED_COUNT];
        for (int i = 0; i < LONG_LIVED_COUNT; i++) {
            longLived[i] = new Object();
            touch(longLived[i]);
        }

        // Phase 2: Short-lived objects (die quickly)
        System.err.println("StressTest: phase 2 - short-lived objects");
        for (int i = 0; i < 200; i++) {
            Object o = new Object();
            touch(o);
        }

        // Phase 3: Linked structure using Object[2] nodes: [data, next]
        System.err.println("StressTest: phase 3 - linked structure");
        Object[] head = null;
        for (int i = 0; i < 100; i++) {
            Object[] node = new Object[2];
            node[0] = new Object(); // data
            touch(node[0]);
            node[1] = head;         // link to previous head
            head = node;
        }

        // Traverse linked structure
        Object[] current = head;
        while (current != null) {
            touch(current[0]);
            touch(current);
            current = (Object[]) current[1];
        }

        // Phase 4: Multi-threaded allocation
        System.err.println("StressTest: phase 4 - multi-threaded");
        Thread[] workers = new Thread[NUM_THREADS];
        for (int t = 0; t < NUM_THREADS; t++) {
            workers[t] = new AllocWorker("Worker-" + t, OBJECTS_PER_THREAD);
            workers[t].start();
        }
        for (int t = 0; t < NUM_THREADS; t++) {
            workers[t].join();
        }

        // Phase 5: Final use of long-lived objects
        System.err.println("StressTest: phase 5 - final touch");
        for (int i = 0; i < LONG_LIVED_COUNT; i++) {
            touch(longLived[i]);
        }

        // Kill linked structure
        head = null;

        System.err.println("StressTest: done");
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
            ArrayList<Object> kept = new ArrayList<>();
            for (int i = 0; i < count; i++) {
                Object obj;
                if (i % 3 == 0) {
                    obj = new Object();
                } else if (i % 3 == 1) {
                    obj = new int[10];
                } else {
                    obj = new Object[5];
                }
                touch(obj);

                // Keep every 10th object alive longer
                if (i % 10 == 0) {
                    kept.add(obj);
                }
            }
            // Touch kept objects before thread exits
            for (int i = 0; i < kept.size(); i++) {
                touch(kept.get(i));
            }
        }
    }
}
