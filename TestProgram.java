import java.util.ArrayList;
import java.util.List;

/**
 * Simple test program for Elephant Tracks 3.
 * Creates objects with various lifetimes to verify trace collection.
 */
public class TestProgram {

    public static void main(String[] args) {
        System.out.println("TestProgram: Starting...");

        // Create some short-lived objects
        for (int i = 0; i < 100; i++) {
            String s = "Short-lived string " + i;
            s.length(); // Use the object
        }

        // Create some longer-lived objects
        List<Object> longLived = new ArrayList<>();
        for (int i = 0; i < 50; i++) {
            longLived.add(new Object());
        }

        // Create some objects with references between them
        Node head = createLinkedList(20);
        traverseList(head);

        // Create arrays
        int[] intArray = new int[100];
        String[] stringArray = new String[50];
        for (int i = 0; i < 50; i++) {
            stringArray[i] = "Array element " + i;
        }

        // Clear long-lived objects to make them collectible
        longLived.clear();
        head = null;

        // Force GC to generate death events
        System.gc();

        try {
            Thread.sleep(100); // Give GC time to run
        } catch (InterruptedException e) {
            // Ignore
        }

        System.out.println("TestProgram: Complete.");
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
            current = current.next;
        }
        System.out.println("List sum: " + sum);
    }
}

class Node {
    int value;
    Node next;

    Node(int value) {
        this.value = value;
    }
}
