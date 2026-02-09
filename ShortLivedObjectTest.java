/**
 * ShortLivedObjectTest -- validates liveness oracle produces early frees.
 *
 * ~35 application allocations, single-threaded.
 *
 * Group A: 10 ephemeral Objects (die 1 alloc later)
 * Group B: 5 medium-lived Objects (die ~15 allocs later)
 * Group C: 3 long-lived Objects (die at end)
 *
 * Uses only Object and Object[] to avoid class-loading issues.
 *
 * Expected: Liveness oracle frees Group A objects much earlier than
 * reachability oracle. If both oracles produce identical CSV, liveness
 * tracking is broken.
 */
public class ShortLivedObjectTest {

    public static void main(String[] args) {
        System.err.println("ShortLivedObjectTest: starting");

        // Group C: 3 long-lived objects (survive entire test)
        Object longA = new Object();
        Object longB = new Object();
        Object longC = new Object();

        // Touch them early
        touch(longA);
        touch(longB);
        touch(longC);

        // Group B: 5 medium-lived objects stored in array
        // These stay alive across Group A allocations
        Object[] medium = new Object[5];
        for (int i = 0; i < 5; i++) {
            medium[i] = new Object();
            touch(medium[i]);
        }

        // Group A: 10 ephemeral objects (each dies before next is allocated)
        // Liveness oracle should free these almost immediately
        for (int i = 0; i < 10; i++) {
            Object ephemeral = new Object();
            touch(ephemeral);
            // ephemeral goes out of scope here -- last access was touch()
        }

        // Touch medium objects again (extends liveness past Group A)
        for (int i = 0; i < 5; i++) {
            touch(medium[i]);
        }

        // Kill medium objects
        for (int i = 0; i < 5; i++) {
            medium[i] = null;
        }

        // Allocate filler after medium dies (gives reachability oracle
        // a different free point than liveness)
        Object[] filler = new Object[5];
        for (int i = 0; i < 5; i++) {
            filler[i] = new Object();
            touch(filler[i]);
        }

        // Final touch of long-lived objects
        touch(longA);
        touch(longB);
        touch(longC);

        // Touch filler
        for (int i = 0; i < 5; i++) {
            touch(filler[i]);
        }

        System.err.println("ShortLivedObjectTest: done");
    }

    static void touch(Object o) {
        // hashCode() registers as a read access in Elephant Tracks
        o.hashCode();
    }
}
