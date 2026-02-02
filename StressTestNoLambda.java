import java.util.*;

/**
 * Stress test WITHOUT lambdas - to test if ET3 tracks all threads.
 *
 * The issue: ET3 skips methods starting with "lambda$", so allocations
 * inside lambda bodies are not tracked.
 *
 * This test uses explicit Runnable classes instead of lambdas.
 */
public class StressTestNoLambda {

    static volatile int counter = 0;

    public static void main(String[] args) throws Exception {
        System.out.println("=== StressTestNoLambda Starting ===");

        // Part 1: Main thread allocations
        System.out.println("[Phase 1] Main thread allocations...");
        List<Object> mainObjects = new ArrayList<>();
        for (int i = 0; i < 50; i++) {
            Object o = new Object();
            use(o);
            mainObjects.add(o);
        }

        // Part 2: Worker threads using explicit Runnable class (NOT lambda)
        System.out.println("[Phase 2] Starting worker threads (no lambdas)...");

        Thread[] workers = new Thread[4];
        for (int t = 0; t < 4; t++) {
            workers[t] = new Thread(new WorkerRunnable(t), "Worker-" + t);
            workers[t].start();
        }

        // Wait for workers
        for (Thread w : workers) {
            w.join();
        }

        System.out.println("[Phase 3] Final check...");
        int sum = 0;
        for (Object o : mainObjects) {
            sum += o.hashCode() & 0xFF;
        }

        System.out.println("\n=== StressTestNoLambda Complete ===");
        System.out.println("Main thread objects: " + mainObjects.size());
        System.out.println("Total worker allocations: " + counter);
        System.out.println("Sum: " + sum);
    }

    static void use(Object o) {
        int hash = o.hashCode();
        if (hash == Integer.MIN_VALUE) {
            System.out.println("Edge case");
        }
    }

    // Explicit Runnable class - should be instrumented by ET3
    static class WorkerRunnable implements Runnable {
        private final int id;

        WorkerRunnable(int id) {
            this.id = id;
        }

        @Override
        public void run() {
            List<Object> localObjects = new ArrayList<>();

            for (int i = 0; i < 100; i++) {
                Object obj;
                switch (i % 3) {
                    case 0:
                        obj = new Object();
                        break;
                    case 1:
                        obj = new Node(i);
                        break;
                    default:
                        obj = new int[5];
                        break;
                }

                use(obj);

                // Keep some alive
                if (i % 10 == 0) {
                    localObjects.add(obj);
                }

                synchronized (StressTestNoLambda.class) {
                    counter++;
                }
            }

            // Use local objects
            for (Object o : localObjects) {
                use(o);
            }
        }
    }

    static class Node {
        int value;
        Node next;

        Node(int v) {
            this.value = v;
        }
    }
}
