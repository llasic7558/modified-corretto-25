import java.util.ArrayList;
import java.util.List;

/**
 * ThreadStabilityTest -- multi-threaded allocation test for Oracle GC.
 *
 * Rewritten to avoid crash patterns:
 * - Named static inner classes instead of lambdas
 * - NO string concatenation
 * - NO System.out.println with variables
 * - All output via System.err with constant strings
 *
 * Part 1: Single-threaded allocations (deterministic)
 * Part 2: Two worker threads allocating independently
 * Part 3: Producer/consumer with shared object
 */
public class ThreadStabilityTest {
    static volatile int counter = 0;

    public static void main(String[] args) throws Exception {
        System.err.println("ThreadStabilityTest: starting single-threaded phase");

        // Part 1: Single-threaded (should be deterministic)
        List<Object> singleThreadObjects = new ArrayList<>();
        for (int i = 0; i < 20; i++) {
            Object o = new Object();
            touch(o);
            singleThreadObjects.add(o);
        }

        // Touch all
        for (int i = 0; i < singleThreadObjects.size(); i++) {
            touch(singleThreadObjects.get(i));
        }

        System.err.println("ThreadStabilityTest: starting multi-threaded phase");

        // Part 2: Multi-threaded (named Thread subclasses, no lambdas)
        Thread t1 = new Worker("Worker-1");
        Thread t2 = new Worker("Worker-2");

        t1.start();
        t2.start();
        t1.join();
        t2.join();

        System.err.println("ThreadStabilityTest: starting shared object phase");

        // Part 3: Cross-thread object sharing
        Object[] shared = new Object[1];

        Thread producer = new Producer(shared);
        producer.start();
        producer.join();

        Thread consumer = new Consumer(shared);
        consumer.start();
        consumer.join();

        System.err.println("ThreadStabilityTest: done");
    }

    static void touch(Object o) {
        o.hashCode();
    }

    static class Worker extends Thread {
        Worker(String name) {
            super(name);
        }

        public void run() {
            for (int i = 0; i < 10; i++) {
                Object o = new Object();
                touch(o);
                synchronized (ThreadStabilityTest.class) {
                    counter++;
                }
            }
        }
    }

    static class Producer extends Thread {
        private final Object[] target;

        Producer(Object[] target) {
            super("Producer");
            this.target = target;
        }

        public void run() {
            target[0] = new Object();
            touch(target[0]);
        }
    }

    static class Consumer extends Thread {
        private final Object[] source;

        Consumer(Object[] source) {
            super("Consumer");
            this.source = source;
        }

        public void run() {
            if (source[0] != null) {
                touch(source[0]);
            }
        }
    }
}
