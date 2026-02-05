import java.util.ArrayList;
import java.util.List;

/**
 * Minimal test program that matches the oracle trace exactly.
 * No string operations or other allocations that could interfere.
 */
public class SimpleTestProgram {

    public static void main(String[] args) {
        System.err.println("SimpleTestProgram: Starting...");

        // Allocation 1: ArrayList
        System.err.println("SimpleTestProgram: About to create ArrayList");
        List<Object> longLived = new ArrayList<>();
        System.err.println("SimpleTestProgram: ArrayList created");

        // Allocations 2-51: 50 Objects
        System.err.println("SimpleTestProgram: About to create 50 Objects");
        for (int i = 0; i < 50; i++) {
            longLived.add(new Object());
        }
        System.err.println("SimpleTestProgram: 50 Objects created");

        // Allocations 52-71: 20 Nodes (linked list)
        Node head = createLinkedList(20);

        // Allocation 72: int array
        int[] intArray = new int[100];

        // Allocation 73: String array
        String[] stringArray = new String[50];

        // Use the objects to prevent optimization
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

        // Clear to make collectible
        longLived.clear();
        head = null;

        // Print OracleSignal debug counts
        try {
            Class<?> osClass = Class.forName("OracleSignal");
            java.lang.reflect.Method m = osClass.getMethod("printDebugCounts");
            m.invoke(null);
        } catch (Exception e) {
            // Ignore if OracleSignal not available
        }
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
}

class Node {
    int value;
    Node next;

    Node(int value) {
        this.value = value;
    }
}
