/**
 * MultiThreadAllocTest -- focused multi-thread test for Oracle GC.
 *
 * ~70 application allocations, 3 worker threads + main thread.
 *
 * Main thread: allocates setup objects
 * Each worker: allocates 20 Objects independently
 *
 * Validates:
 * - Per-thread logical ID mapping
 * - Concurrent death processing
 * - Thread creation/join doesn't corrupt oracle state
 *
 * Avoids: lambdas, string concat, java.util.concurrent
 */
public class MultiThreadAllocTest {

    public static void main(String[] args) throws Exception {
        System.err.println("MultiThreadAllocTest: starting");

        // Main thread: setup objects
        Object[] setup = new Object[5];
        for (int i = 0; i < 5; i++) {
            setup[i] = new Object();
            touch(setup[i]);
        }

        System.err.println("MultiThreadAllocTest: starting workers");

        // 3 worker threads, each allocates 20 objects
        Thread w1 = new Worker("Worker-1", 20);
        Thread w2 = new Worker("Worker-2", 20);
        Thread w3 = new Worker("Worker-3", 20);

        w1.start();
        w2.start();
        w3.start();

        w1.join();
        w2.join();
        w3.join();

        System.err.println("MultiThreadAllocTest: workers done");

        // Main thread: final allocations after workers finish
        Object[] postWorker = new Object[5];
        for (int i = 0; i < 5; i++) {
            postWorker[i] = new Object();
            touch(postWorker[i]);
        }

        // Touch setup objects again (extends liveness past worker phase)
        for (int i = 0; i < 5; i++) {
            touch(setup[i]);
        }

        // Touch post-worker objects
        for (int i = 0; i < 5; i++) {
            touch(postWorker[i]);
        }

        System.err.println("MultiThreadAllocTest: done");
    }

    static void touch(Object o) {
        o.hashCode();
    }

    static class Worker extends Thread {
        private final int count;

        Worker(String name, int count) {
            super(name);
            this.count = count;
        }

        public void run() {
            Object[] kept = new Object[count];
            for (int i = 0; i < count; i++) {
                kept[i] = new Object();
                touch(kept[i]);
            }
            // Touch all at end to extend liveness
            for (int i = 0; i < count; i++) {
                touch(kept[i]);
            }
        }
    }
}
