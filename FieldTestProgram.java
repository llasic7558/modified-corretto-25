import java.util.ArrayList;
import java.util.List;

/**
 * Test program using FIELDS instead of local variables.
 * This allows Elephant Tracks to properly track object accesses.
 */
public class FieldTestProgram {
    // Store references in fields so ET can track accesses
    static List<Object> longLived;
    static Node head;
    static int[] intArray;
    static String[] stringArray;

    public static void main(String[] args) {
        System.err.println("FieldTestProgram: Starting...");

        // Allocation 1: ArrayList (stored in field)
        longLived = new ArrayList<>();
        System.err.println("FieldTestProgram: ArrayList created");

        // Allocations 2-51: 50 Objects
        for (int i = 0; i < 50; i++) {
            longLived.add(new Object());
        }
        System.err.println("FieldTestProgram: 50 Objects created");

        // Allocations 52-71: 20 Nodes (linked list)
        head = createLinkedList(20);
        System.err.println("FieldTestProgram: Linked list created");

        // Allocation 72: int array
        intArray = new int[100];

        // Allocation 73: String array
        stringArray = new String[50];
        System.err.println("FieldTestProgram: Arrays created");

        // Use the objects
        int sum = 0;
        for (Object o : longLived) {
            sum += o.hashCode();
        }
        Node n = head;
        while (n != null) {
            sum += n.value;
            n = n.next;
        }
        sum += intArray.length + stringArray.length;
        System.err.println("FieldTestProgram: Sum = " + sum);

        // Clear references to make objects collectible
        longLived.clear();
        longLived = null;
        head = null;
        intArray = null;
        stringArray = null;

        // Force GC to generate death events
        System.gc();
        try { Thread.sleep(100); } catch (Exception e) {}

        System.err.println("FieldTestProgram: Complete");
    }

    static Node createLinkedList(int size) {
        Node h = new Node(0);
        Node current = h;
        for (int i = 1; i < size; i++) {
            current.next = new Node(i);
            current = current.next;
        }
        return h;
    }
}

class Node {
    int value;
    Node next;
    Node(int value) { this.value = value; }
}
