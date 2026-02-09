/**
 * ObjectGraphTest -- tests sub-graph unreachability in reachability oracle.
 *
 * ~55 application allocations, single-threaded.
 *
 * Builds binary trees using Object[3] nodes:
 *   node[0] = left child
 *   node[1] = right child
 *   node[2] = data (an Object for liveness tracking)
 *
 * Tree 1: depth 4 = 15 nodes (+ 15 data objects = 30 allocs)
 * Tree 2: depth 3 = 7 nodes (+ 7 data objects = 14 allocs)
 *
 * Kill tree 1, allocate filler, touch tree 2.
 *
 * Validates:
 * - Entire sub-graph becomes unreachable when root nulled (reachability)
 * - Data objects freed earlier in liveness (last touch before tree death)
 */
public class ObjectGraphTest {

    public static void main(String[] args) {
        System.err.println("ObjectGraphTest: starting");

        // Build tree 1 (depth 4, 15 nodes)
        Object[] tree1 = buildTree(4);
        touchTree(tree1);

        System.err.println("ObjectGraphTest: tree1 built");

        // Build tree 2 (depth 3, 7 nodes)
        Object[] tree2 = buildTree(3);
        touchTree(tree2);

        System.err.println("ObjectGraphTest: tree2 built");

        // Kill tree 1 (entire sub-graph becomes unreachable)
        tree1 = null;

        // Allocate filler objects after tree1 death
        Object[] filler = new Object[5];
        for (int i = 0; i < 5; i++) {
            filler[i] = new Object();
            touch(filler[i]);
        }

        // Touch tree 2 again (extends liveness)
        touchTree(tree2);

        // Touch filler again
        for (int i = 0; i < 5; i++) {
            touch(filler[i]);
        }

        System.err.println("ObjectGraphTest: done");
    }

    /**
     * Build a complete binary tree of given depth.
     * Each node is Object[3]: [left, right, data].
     * Returns root node.
     */
    static Object[] buildTree(int depth) {
        if (depth <= 0) {
            return null;
        }
        Object[] node = new Object[3];
        node[2] = new Object(); // data object
        touch(node[2]);

        if (depth > 1) {
            node[0] = buildTree(depth - 1); // left
            node[1] = buildTree(depth - 1); // right
        }
        return node;
    }

    /**
     * Touch all data objects in the tree (in-order traversal).
     */
    static void touchTree(Object[] node) {
        if (node == null) {
            return;
        }
        touchTree((Object[]) node[0]); // left
        touch(node[2]);                 // data
        touch(node);                    // node itself
        touchTree((Object[]) node[1]); // right
    }

    static void touch(Object o) {
        o.hashCode();
    }
}
