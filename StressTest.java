import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

/**
 * Stress test for trace stability analysis.
 *
 * This test is designed to:
 * 1. Allocate MANY objects (thousands) to trigger GC
 * 2. Have objects with varying lifetimes (short, medium, long)
 * 3. Use multiple threads that compete for allocations
 * 4. Share some objects across threads
 * 5. Create complex object graphs (references between objects)
 *
 * Run multiple times and compare traces to check stability.
 */
public class StressTest {

    // Shared state for cross-thread communication
    static final ConcurrentLinkedQueue<Object> sharedQueue = new ConcurrentLinkedQueue<>();
    static final AtomicInteger allocCounter = new AtomicInteger(0);
    static final AtomicInteger gcCount = new AtomicInteger(0);

    // Configuration
    static final int OBJECTS_PER_THREAD = 500;
    static final int NUM_THREADS = 4;
    static final int LONG_LIVED_COUNT = 50;

    public static void main(String[] args) throws Exception {
        System.out.println("=== StressTest Starting ===");
        System.out.println("Objects per thread: " + OBJECTS_PER_THREAD);
        System.out.println("Number of threads: " + NUM_THREADS);

        // Part 1: Long-lived objects (survive entire test)
        System.out.println("\n[Phase 1] Creating long-lived objects...");
        List<Node> longLived = new ArrayList<>();
        for (int i = 0; i < LONG_LIVED_COUNT; i++) {
            Node n = new Node(i);
            longLived.add(n);
            use(n);
        }

        // Part 2: Short-lived objects (die quickly)
        System.out.println("[Phase 2] Creating short-lived objects...");
        for (int i = 0; i < 200; i++) {
            Object o = new Object();
            use(o);
            // Object becomes garbage immediately
        }

        // Part 3: Medium-lived objects with references
        System.out.println("[Phase 3] Creating linked list...");
        Node head = new Node(0);
        Node current = head;
        for (int i = 1; i < 100; i++) {
            Node next = new Node(i);
            current.next = next;
            current = next;
            use(next);
        }

        // Part 4: Trigger explicit GC
        System.out.println("[Phase 4] Triggering GC...");
        System.gc();
        Thread.sleep(100);

        // Part 5: Multi-threaded allocation storm
        System.out.println("[Phase 5] Starting " + NUM_THREADS + " worker threads...");

        CountDownLatch startLatch = new CountDownLatch(1);
        CountDownLatch doneLatch = new CountDownLatch(NUM_THREADS);

        Thread[] workers = new Thread[NUM_THREADS];
        for (int t = 0; t < NUM_THREADS; t++) {
            final int threadId = t;
            workers[t] = new Thread(() -> {
                try {
                    startLatch.await(); // Wait for all threads to be ready

                    List<Object> localObjects = new ArrayList<>();

                    for (int i = 0; i < OBJECTS_PER_THREAD; i++) {
                        // Allocate different types
                        Object obj;
                        switch (i % 5) {
                            case 0:
                                obj = new Object();
                                break;
                            case 1:
                                obj = new Node(i);
                                break;
                            case 2:
                                obj = new int[10];
                                break;
                            case 3:
                                obj = new String("str" + i);
                                break;
                            default:
                                obj = new ArrayList<>(5);
                                break;
                        }

                        use(obj);
                        allocCounter.incrementAndGet();

                        // Keep some objects alive
                        if (i % 10 == 0) {
                            localObjects.add(obj);
                        }

                        // Share some objects via queue
                        if (i % 20 == 0) {
                            sharedQueue.offer(obj);
                        }

                        // Consume shared objects
                        if (i % 25 == 0) {
                            Object shared = sharedQueue.poll();
                            if (shared != null) {
                                use(shared);
                            }
                        }
                    }

                    // Use local objects before thread exits
                    for (Object o : localObjects) {
                        use(o);
                    }

                } catch (Exception e) {
                    e.printStackTrace();
                } finally {
                    doneLatch.countDown();
                }
            }, "Worker-" + threadId);
            workers[t].start();
        }

        // Start all threads simultaneously
        startLatch.countDown();

        // Wait for completion
        doneLatch.await();

        System.out.println("[Phase 5] Workers completed. Total allocations: " + allocCounter.get());

        // Part 6: More GC to collect thread-local garbage
        System.out.println("[Phase 6] Final GC...");
        System.gc();
        Thread.sleep(100);

        // Part 7: Use long-lived objects one more time
        System.out.println("[Phase 7] Final use of long-lived objects...");
        int sum = 0;
        for (Node n : longLived) {
            sum += n.value;
            use(n);
        }

        // Traverse linked list
        current = head;
        while (current != null) {
            use(current);
            current = current.next;
        }

        System.out.println("\n=== StressTest Complete ===");
        System.out.println("Long-lived sum: " + sum);
        System.out.println("Total allocations tracked: " + allocCounter.get());
        System.out.println("Shared queue final size: " + sharedQueue.size());
    }

    static void use(Object o) {
        // Actually use the object to register access in trace
        int hash = o.hashCode();
        if (hash == Integer.MIN_VALUE) {
            System.out.println("Edge case hash"); // Unlikely, prevents optimization
        }
    }

    // Simple node class for linked structures
    static class Node {
        int value;
        Node next;

        Node(int v) {
            this.value = v;
        }
    }
}
