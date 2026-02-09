import java.util.ArrayList;

/**
 * CollectionGrowthTest -- validates library-internal allocation tracking.
 *
 * ~40 application allocations, single-threaded.
 *
 * ArrayList with 20 items forces internal Object[] resize (default cap 10 -> 15 -> 22).
 * Old backing arrays become garbage when ArrayList grows.
 * Clear collection mid-test, allocate more after.
 *
 * Validates:
 * - Collection resizing tracked correctly
 * - Old backing arrays freed by oracle
 * - Items freed when collection cleared
 */
public class CollectionGrowthTest {

    public static void main(String[] args) {
        System.err.println("CollectionGrowthTest: starting");

        // Phase 1: Create ArrayList, add 20 items to trigger resize
        ArrayList<Object> list = new ArrayList<>();

        for (int i = 0; i < 20; i++) {
            Object item = new Object();
            touch(item);
            list.add(item);
        }

        // Touch all items via iteration
        for (int i = 0; i < list.size(); i++) {
            touch(list.get(i));
        }

        System.err.println("CollectionGrowthTest: phase 1 done");

        // Phase 2: Clear the collection (all 20 items become garbage)
        list.clear();

        // Phase 3: Add 10 more items (reuse cleared list)
        for (int i = 0; i < 10; i++) {
            Object item = new Object();
            touch(item);
            list.add(item);
        }

        // Touch phase 3 items
        for (int i = 0; i < list.size(); i++) {
            touch(list.get(i));
        }

        System.err.println("CollectionGrowthTest: done");
    }

    static void touch(Object o) {
        o.hashCode();
    }
}
