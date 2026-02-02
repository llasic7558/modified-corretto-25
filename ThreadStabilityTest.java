import java.util.ArrayList;
import java.util.List;

/**
 * Test program to analyze trace stability across runs.
 *
 * Part 1: Single-threaded allocations (should be deterministic)
 * Part 2: Multi-threaded allocations (may diverge in order)
 *
 * Run multiple times and compare traces to see if allocation order is stable.
 */
public class ThreadStabilityTest {
    static volatile int counter = 0;

    public static void main(String[] args) throws Exception {
        System.out.println("=== Single-threaded allocations ===");

        // Part 1: Single-threaded (should be deterministic)
        List<Object> singleThreadObjects = new ArrayList<>();
        for (int i = 0; i < 20; i++) {
            Object o = new Object();
            use(o);
            singleThreadObjects.add(o);
        }
        System.out.println("Single-threaded: " + singleThreadObjects.size() + " objects");

        // Part 2: Multi-threaded (may diverge)
        System.out.println("=== Multi-threaded allocations ===");

        Thread t1 = new Thread(() -> {
            for (int i = 0; i < 10; i++) {
                Object o = new Object();
                use(o);
                synchronized (ThreadStabilityTest.class) {
                    counter++;
                }
            }
        }, "Worker-1");

        Thread t2 = new Thread(() -> {
            for (int i = 0; i < 10; i++) {
                Object o = new Object();
                use(o);
                synchronized (ThreadStabilityTest.class) {
                    counter++;
                }
            }
        }, "Worker-2");

        t1.start();
        t2.start();
        t1.join();
        t2.join();

        System.out.println("Multi-threaded counter: " + counter);

        // Part 3: Cross-thread object sharing
        System.out.println("=== Cross-thread object sharing ===");
        final Object[] shared = new Object[1];

        Thread producer = new Thread(() -> {
            shared[0] = new Object();
            use(shared[0]);
        }, "Producer");

        Thread consumer = new Thread(() -> {
            // Wait for producer
            while (shared[0] == null) {
                Thread.yield();
            }
            use(shared[0]);  // Use object created by another thread
        }, "Consumer");

        producer.start();
        consumer.start();
        producer.join();
        consumer.join();

        System.out.println("Shared object test complete");
        System.out.println("=== Test Complete ===");
    }

    static void use(Object o) {
        // Actually use the object to register access in trace
        int hash = o.hashCode();
        if (hash == 0) {
            System.out.println("Zero hash"); // Unlikely, prevents optimization
        }
    }
}
