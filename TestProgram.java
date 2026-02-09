import java.util.ArrayList;
import java.util.List;

/**
 * TestProgram -- basic Oracle GC test with mixed allocation patterns.
 *
 * Rewritten to avoid crash patterns:
 * - NO string concatenation during allocation phases
 * - NO System.out.println with variables
 * - NO System.gc() (not available with Epsilon)
 * - Uses Object/Object[] and Node only
 * - All output via System.err with constant strings
 */
public class TestProgram {

    public static void main(String[] args) {
        System.err.println("TestProgram: starting");

        // Short-lived objects (each dies immediately)
        for (int i = 0; i < 100; i++) {
            Object o = new Object();
            touch(o);
        }

        System.err.println("TestProgram: short-lived done");

        // Longer-lived objects in a list
        List<Object> longLived = new ArrayList<>();
        for (int i = 0; i < 50; i++) {
            longLived.add(new Object());
        }

        // Touch all items
        for (int i = 0; i < longLived.size(); i++) {
            touch(longLived.get(i));
        }

        // Create linked list (Node is a top-level class)
        Node head = createLinkedList(20);
        traverseList(head);

        // Create arrays
        int[] intArray = new int[100];
        Object[] objArray = new Object[50];
        for (int i = 0; i < 50; i++) {
            objArray[i] = new Object();
            touch(objArray[i]);
        }

        // Clear long-lived objects to make them collectible
        longLived.clear();
        head = null;

        // Use arrays to prevent optimization
        int sum = intArray.length + objArray.length;
        for (int i = 0; i < objArray.length; i++) {
            if (objArray[i] != null) {
                touch(objArray[i]);
            }
        }

        System.err.println("TestProgram: done");
    }

    static Node createLinkedList(int size) {
        Node head = new Node(0);
        Node current = head;
        for (int i = 1; i < size; i++) {
            current.next = new Node(i);
            current = current.next;
        }
        return head;
    }

    static void traverseList(Node head) {
        Node current = head;
        int sum = 0;
        while (current != null) {
            sum += current.value;
            touch(current);
            current = current.next;
        }
    }

    static void touch(Object o) {
        o.hashCode();
    }
}

class Node {
    int value;
    Node next;

    Node(int value) {
        this.value = value;
    }
}
